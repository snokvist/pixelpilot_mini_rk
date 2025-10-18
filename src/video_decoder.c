#include "video_decoder.h"

#include "drm_props.h"
#include "idr_requester.h"
#include "logging.h"
#include "video_stabilizer.h"
#include "video_motion_estimator.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <gst/gst.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_err.h>

#if defined(__GNUC__)
extern int mpp_frame_get_fd(MppFrame frame) __attribute__((weak));
#else
extern int mpp_frame_get_fd(MppFrame frame);
#endif

#if defined(PIXELPILOT_DISABLE_NEON)
#define PIXELPILOT_NEON_AVAILABLE 0
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(PIXELPILOT_HAS_NEON)
#define PIXELPILOT_NEON_AVAILABLE 1
#else
#define PIXELPILOT_NEON_AVAILABLE 0
#endif

#if PIXELPILOT_NEON_AVAILABLE
#include <arm_neon.h>
#endif

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DECODER_READ_BUF_SIZE (1024 * 1024)
#define DECODER_MAX_FRAMES 24

struct FrameSlot {
    int prime_fd;
    uint32_t fb_id;
    uint32_t handle;
    int processed_prime_fd;
    uint32_t processed_fb_id;
    uint32_t processed_handle;
    void *map_addr;
    size_t map_size;
};

struct VideoDecoder {
    gboolean initialized;
    gboolean running;
    gboolean frame_thread_running;
    gboolean display_thread_running;
    gboolean eos_received;
    gboolean lock_initialized;
    gboolean cond_initialized;

    int drm_fd;
    uint32_t plane_id;
    uint32_t crtc_id;
    int mode_w;
    int mode_h;
    uint32_t src_w;
    uint32_t src_h;

    uint32_t prop_fb_id;
    uint32_t prop_crtc_id;
    uint32_t prop_crtc_x;
    uint32_t prop_crtc_y;
    uint32_t prop_crtc_w;
    uint32_t prop_crtc_h;
    uint32_t prop_src_x;
    uint32_t prop_src_y;
    uint32_t prop_src_w;
    uint32_t prop_src_h;
    uint32_t prop_in_fence_fd;
    uint32_t prop_out_fence_ptr;

    MppCtx ctx;
    MppApi *mpi;
    MppBufferGroup frm_grp;
    struct FrameSlot frame_map[DECODER_MAX_FRAMES];

    guint8 *packet_buf;
    size_t packet_buf_size;
    MppPacket packet;

    GMutex lock;
    GCond cond;
    uint32_t pending_fb;
    uint64_t pending_pts;
    int pending_in_fence_fd;
    uint32_t pending_src_w;
    uint32_t pending_src_h;

    GThread *frame_thread;
    GThread *display_thread;

    IdrRequester *idr_requester;
    VideoStabilizer *stabilizer;
    StabilizerConfig stabilizer_cfg;
    gboolean stabilizer_ready;
    StabilizerParams pending_stabilizer_params;
    gboolean stabilizer_params_valid;
    int last_out_fence_fd;
    VideoMotionEstimator *estimator;
    MotionEstimatorConfig estimator_cfg;
    gboolean estimator_ready;
};

VideoDecoder *video_decoder_new(void) {
    return g_new0(VideoDecoder, 1);
}

void video_decoder_free(VideoDecoder *vd) {
    if (vd == NULL) {
        return;
    }
    video_decoder_deinit(vd);
    g_free(vd);
}

static inline guint64 get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (guint64)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
}

static MotionEstimatorConfig derive_estimator_cfg(const StabilizerConfig *cfg) {
    MotionEstimatorConfig out = {0};
    if (cfg == NULL) {
        return out;
    }
    if (cfg->estimator_enable < 0) {
        out.enable = cfg->enable ? 1 : 0;
    } else {
        out.enable = cfg->estimator_enable ? 1 : 0;
    }
    int diag = cfg->estimator_diagnostics;
    if (diag < 0) {
        diag = cfg->diagnostics;
    }
    out.diagnostics = diag ? 1 : 0;
    if (cfg->estimator_search_radius_px > 0) {
        out.search_radius_px = cfg->estimator_search_radius_px;
    } else {
        float max_tx = cfg->max_translation_px > 0.0f ? cfg->max_translation_px : 16.0f;
        out.search_radius_px = (int)ceilf(max_tx);
    }
    if (cfg->estimator_downsample_factor > 0) {
        out.downsample_factor = cfg->estimator_downsample_factor;
    } else {
        out.downsample_factor = 4;
    }
    out.max_sample_width_px = cfg->estimator_max_sample_width_px;
    out.max_sample_height_px = cfg->estimator_max_sample_height_px;
    if (isfinite(cfg->estimator_smoothing_factor) && cfg->estimator_smoothing_factor >= 0.0f) {
        out.smoothing_factor = cfg->estimator_smoothing_factor;
    } else {
        out.smoothing_factor = 0.6f;
    }
    if (out.smoothing_factor > 0.98f) {
        out.smoothing_factor = 0.98f;
    }
    return out;
}

static inline void copy_packet_data(guint8 *dst, const guint8 *src, size_t size) {
#if PIXELPILOT_NEON_AVAILABLE
    /*
     * Use NEON vector loads/stores to move packets in 64-byte bursts when
     * running on ARM targets with NEON support. For remaining tail bytes we
     * fall back to memcpy to avoid reimplementing scalar copies.
     */
    if (G_UNLIKELY(size == 0)) {
        return;
    }

    size_t offset = 0;

    while (offset + 64 <= size) {
        vst1q_u8(dst + offset, vld1q_u8(src + offset));
        vst1q_u8(dst + offset + 16, vld1q_u8(src + offset + 16));
        vst1q_u8(dst + offset + 32, vld1q_u8(src + offset + 32));
        vst1q_u8(dst + offset + 48, vld1q_u8(src + offset + 48));
        offset += 64;
    }

    while (offset + 16 <= size) {
        vst1q_u8(dst + offset, vld1q_u8(src + offset));
        offset += 16;
    }

    if (offset < size) {
        memcpy(dst + offset, src + offset, size - offset);
    }
#else
    /* Fallback to memcpy on non-NEON platforms. */
    if (size > 0) {
        memcpy(dst, src, size);
    }
#endif
}

static int wait_for_fence_fd(int fence_fd) {
    if (fence_fd < 0) {
        return 0;
    }
    struct pollfd pfd = {.fd = fence_fd, .events = POLLIN};
    while (TRUE) {
        int ret = poll(&pfd, 1, -1);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        break;
    }
    return 0;
}

static void log_decoder_neon_status_once(void) {
    static gsize once_init = 0;
    if (g_once_init_enter(&once_init)) {
#if defined(PIXELPILOT_DISABLE_NEON)
        LOGI("Video decoder: NEON packet acceleration disabled by build flag");
#elif PIXELPILOT_NEON_AVAILABLE
        LOGI("Video decoder: NEON packet acceleration enabled");
#else
        LOGI("Video decoder: NEON packet acceleration unavailable on this build");
#endif
        g_once_init_leave(&once_init, 1);
    }
}

static void reset_frame_map(VideoDecoder *vd) {
    for (int i = 0; i < DECODER_MAX_FRAMES; ++i) {
        vd->frame_map[i].prime_fd = -1;
        vd->frame_map[i].fb_id = 0;
        vd->frame_map[i].handle = 0;
        vd->frame_map[i].processed_prime_fd = -1;
        vd->frame_map[i].processed_fb_id = 0;
        vd->frame_map[i].processed_handle = 0;
        vd->frame_map[i].map_addr = NULL;
        vd->frame_map[i].map_size = 0;
    }
}

static void release_frame_group(VideoDecoder *vd) {
    if (vd->frm_grp == NULL) {
        return;
    }
    for (int i = 0; i < DECODER_MAX_FRAMES; ++i) {
        if (vd->frame_map[i].fb_id) {
            drmModeRmFB(vd->drm_fd, vd->frame_map[i].fb_id);
            vd->frame_map[i].fb_id = 0;
        }
        if (vd->frame_map[i].prime_fd >= 0) {
            close(vd->frame_map[i].prime_fd);
            vd->frame_map[i].prime_fd = -1;
        }
        if (vd->frame_map[i].map_addr) {
            munmap(vd->frame_map[i].map_addr, vd->frame_map[i].map_size);
            vd->frame_map[i].map_addr = NULL;
            vd->frame_map[i].map_size = 0;
        }
        if (vd->frame_map[i].handle) {
            struct drm_mode_destroy_dumb dmd = {.handle = vd->frame_map[i].handle};
            ioctl(vd->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmd);
            vd->frame_map[i].handle = 0;
        }
        if (vd->frame_map[i].processed_fb_id) {
            drmModeRmFB(vd->drm_fd, vd->frame_map[i].processed_fb_id);
            vd->frame_map[i].processed_fb_id = 0;
        }
        if (vd->frame_map[i].processed_prime_fd >= 0) {
            close(vd->frame_map[i].processed_prime_fd);
            vd->frame_map[i].processed_prime_fd = -1;
        }
        if (vd->frame_map[i].processed_handle) {
            struct drm_mode_destroy_dumb dmd_out = {.handle = vd->frame_map[i].processed_handle};
            ioctl(vd->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmd_out);
            vd->frame_map[i].processed_handle = 0;
        }
    }
    mpp_buffer_group_clear(vd->frm_grp);
    mpp_buffer_group_put(vd->frm_grp);
    vd->frm_grp = NULL;
    vd->src_w = 0;
    vd->src_h = 0;
    vd->stabilizer_ready = FALSE;
    vd->estimator_ready = FALSE;
    if (vd->estimator) {
        motion_estimator_reset(vd->estimator);
    }
}

static void set_control_verbose(MppApi *mpi, MppCtx ctx, MpiCmd control, RK_U32 enable) {
    RK_U32 res = mpi->control(ctx, control, &enable);
    if (res) {
        LOGW("MPP: control %d -> %u failed (%u)", control, enable, res);
    }
}

static void set_mpp_decoding_parameters(VideoDecoder *vd) {
    MppDecCfg cfg = NULL;
    if (mpp_dec_cfg_init(&cfg) != MPP_OK) {
        LOGW("MPP: mpp_dec_cfg_init failed");
        return;
    }

    if (vd->mpi->control(vd->ctx, MPP_DEC_GET_CFG, cfg) != MPP_OK) {
        LOGW("MPP: GET_CFG failed");
        mpp_dec_cfg_deinit(cfg);
        return;
    }

    if (mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1) != MPP_OK) {
        LOGW("MPP: failed to enable split_parse");
    }

    if (vd->mpi->control(vd->ctx, MPP_DEC_SET_CFG, cfg) != MPP_OK) {
        LOGW("MPP: SET_CFG failed");
    }

    mpp_dec_cfg_deinit(cfg);

    set_control_verbose(vd->mpi, vd->ctx, MPP_DEC_SET_DISABLE_ERROR, 0xffff);
    set_control_verbose(vd->mpi, vd->ctx, MPP_DEC_SET_IMMEDIATE_OUT, 0xffff);
    set_control_verbose(vd->mpi, vd->ctx, MPP_DEC_SET_ENABLE_FAST_PLAY, 0xffff);
}

static int commit_plane(VideoDecoder *vd, uint32_t fb_id, uint32_t src_w, uint32_t src_h, int in_fence_fd,
                        int *out_fence_fd) {
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        return -1;
    }

    if (out_fence_fd) {
        *out_fence_fd = -1;
    }

    if (src_w == 0) {
        src_w = vd->src_w ? vd->src_w : (uint32_t)vd->mode_w;
    } else {
        vd->src_w = src_w;
    }
    if (src_h == 0) {
        src_h = vd->src_h ? vd->src_h : (uint32_t)vd->mode_h;
    } else {
        vd->src_h = src_h;
    }

    int dst_x = 0;
    int dst_y = 0;
    int dst_w = vd->mode_w;
    int dst_h = vd->mode_h;

    if (src_w > 0 && src_h > 0 && vd->mode_w > 0 && vd->mode_h > 0) {
        double src_ar = (double)src_w / (double)src_h;
        double mode_ar = (double)vd->mode_w / (double)vd->mode_h;

        if (src_ar > 0.0 && mode_ar > 0.0) {
            if (src_ar > mode_ar) {
                dst_w = vd->mode_w;
                dst_h = (int)lround((double)vd->mode_w / src_ar);
                if (dst_h > vd->mode_h) {
                    dst_h = vd->mode_h;
                }
                dst_y = (vd->mode_h - dst_h) / 2;
            } else {
                dst_h = vd->mode_h;
                dst_w = (int)lround((double)vd->mode_h * src_ar);
                if (dst_w > vd->mode_w) {
                    dst_w = vd->mode_w;
                }
                dst_x = (vd->mode_w - dst_w) / 2;
            }
        }

        if (dst_w <= 0) {
            dst_w = 1;
        }
        if (dst_h <= 0) {
            dst_h = 1;
        }
    }

    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_fb_id, fb_id);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_crtc_id, vd->crtc_id);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_crtc_x, dst_x);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_crtc_y, dst_y);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_crtc_w, dst_w);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_crtc_h, dst_h);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_src_x, 0);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_src_y, 0);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_src_w, (uint64_t)src_w << 16);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_src_h, (uint64_t)src_h << 16);

    if (vd->prop_in_fence_fd != 0 && in_fence_fd >= 0) {
        drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_in_fence_fd, (uint64_t)in_fence_fd);
    } else if (in_fence_fd >= 0) {
        int wait_ret = wait_for_fence_fd(in_fence_fd);
        if (wait_ret != 0) {
            LOGW("Video decoder: wait on input fence failed: %s", g_strerror(-wait_ret));
        }
    }

    int local_out_fence_fd = -1;
    if (vd->prop_out_fence_ptr != 0 && out_fence_fd != NULL) {
        drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_out_fence_ptr,
                                 (uint64_t)(uintptr_t)&local_out_fence_fd);
    }

    int ret = drmModeAtomicCommit(vd->drm_fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
    if (ret != 0 && errno == EBUSY) {
        ret = drmModeAtomicCommit(vd->drm_fd, req, 0, NULL);
    }
    if (ret != 0) {
        LOGW("Atomic commit failed: %s", g_strerror(errno));
    }
    if (vd->prop_in_fence_fd != 0 && in_fence_fd >= 0) {
        close(in_fence_fd);
    } else if (vd->prop_in_fence_fd == 0 && in_fence_fd >= 0) {
        close(in_fence_fd);
    }

    drmModeAtomicFree(req);
    if (ret == 0 && vd->prop_out_fence_ptr != 0 && out_fence_fd != NULL) {
        *out_fence_fd = local_out_fence_fd;
    } else if (out_fence_fd) {
        *out_fence_fd = -1;
    }
    return ret;
}

static int setup_external_buffers(VideoDecoder *vd, MppFrame frame) {
    RK_U32 width = mpp_frame_get_width(frame);
    RK_U32 height = mpp_frame_get_height(frame);
    RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
    RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
    MppFrameFormat fmt = mpp_frame_get_fmt(frame);

    if (fmt != MPP_FMT_YUV420SP && fmt != MPP_FMT_YUV420SP_10BIT) {
        LOGE("MPP: unexpected format %d", fmt);
        return -1;
    }

    release_frame_group(vd);

    if (mpp_buffer_group_get_external(&vd->frm_grp, MPP_BUFFER_TYPE_DRM) != MPP_OK) {
        LOGE("MPP: failed to get external buffer group");
        return -1;
    }

    reset_frame_map(vd);

    gboolean need_stabilizer = FALSE;
    gboolean stabilizer_buffers_ok = TRUE;
    if (vd->stabilizer && vd->stabilizer_cfg.enable) {
        need_stabilizer = video_stabilizer_is_available(vd->stabilizer);
        if (!need_stabilizer) {
            LOGI("Video stabilizer not available; continuing without post-processing");
        }
    }

    for (int i = 0; i < DECODER_MAX_FRAMES; ++i) {
        struct drm_mode_create_dumb dmcd;
        memset(&dmcd, 0, sizeof(dmcd));
        dmcd.bpp = (fmt == MPP_FMT_YUV420SP) ? 8 : 10;
        dmcd.width = hor_stride;
        dmcd.height = ver_stride * 2;

        int ret;
        do {
            ret = ioctl(vd->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dmcd);
        } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
        if (ret != 0) {
            LOGW("DRM_IOCTL_MODE_CREATE_DUMB failed: %s", g_strerror(errno));
            continue;
        }
        vd->frame_map[i].handle = dmcd.handle;
        vd->frame_map[i].map_addr = NULL;
        vd->frame_map[i].map_size = 0;

        struct drm_mode_map_dumb dmmap;
        memset(&dmmap, 0, sizeof(dmmap));
        dmmap.handle = dmcd.handle;
        if (ioctl(vd->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &dmmap) == 0) {
            void *addr = mmap(NULL, dmcd.size, PROT_READ | PROT_WRITE, MAP_SHARED, vd->drm_fd, dmmap.offset);
            if (addr != MAP_FAILED) {
                vd->frame_map[i].map_addr = addr;
                vd->frame_map[i].map_size = dmcd.size;
            } else {
                LOGW("Video decoder: mmap failed for frame slot %d: %s", i, g_strerror(errno));
            }
        } else {
            LOGW("Video decoder: MAP_DUMB failed: %s", g_strerror(errno));
        }

        struct drm_prime_handle dph;
        memset(&dph, 0, sizeof(dph));
        dph.handle = dmcd.handle;
        dph.fd = -1;
        do {
            ret = ioctl(vd->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph);
        } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
        if (ret != 0) {
            LOGW("PRIME_HANDLE_TO_FD failed: %s", g_strerror(errno));
            continue;
        }

        MppBufferInfo info;
        memset(&info, 0, sizeof(info));
        info.type = MPP_BUFFER_TYPE_DRM;
        info.size = dmcd.size;
        info.fd = dph.fd;
        ret = mpp_buffer_commit(vd->frm_grp, &info);
        if (ret != MPP_OK) {
            LOGW("MPP: buffer_commit failed (%d)", ret);
            close(dph.fd);
            continue;
        }
        vd->frame_map[i].prime_fd = info.fd;
        if (dph.fd != info.fd) {
            close(dph.fd);
        }

        uint32_t handles[4] = {0};
        uint32_t pitches[4] = {0};
        uint32_t offsets[4] = {0};
        handles[0] = vd->frame_map[i].handle;
        handles[1] = vd->frame_map[i].handle;
        pitches[0] = dmcd.pitch;
        pitches[1] = dmcd.pitch;
        offsets[0] = 0;
        offsets[1] = dmcd.pitch * ver_stride;

        ret = drmModeAddFB2(vd->drm_fd, width, height, DRM_FORMAT_NV12, handles, pitches, offsets,
                            &vd->frame_map[i].fb_id, 0);
        if (ret != 0) {
            LOGW("drmModeAddFB2 failed: %s", g_strerror(errno));
            continue;
        }

        if (need_stabilizer) {
            struct drm_mode_create_dumb dmcd_out;
            memset(&dmcd_out, 0, sizeof(dmcd_out));
            dmcd_out.bpp = dmcd.bpp;
            dmcd_out.width = hor_stride;
            dmcd_out.height = ver_stride * 2;

            do {
                ret = ioctl(vd->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dmcd_out);
            } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
            if (ret != 0) {
                LOGW("DRM_IOCTL_MODE_CREATE_DUMB (processed) failed: %s", g_strerror(errno));
                stabilizer_buffers_ok = FALSE;
                continue;
            }
            vd->frame_map[i].processed_handle = dmcd_out.handle;

            struct drm_prime_handle dph_out;
            memset(&dph_out, 0, sizeof(dph_out));
            dph_out.handle = dmcd_out.handle;
            dph_out.fd = -1;
            do {
                ret = ioctl(vd->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph_out);
            } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
            if (ret != 0) {
                LOGW("PRIME_HANDLE_TO_FD (processed) failed: %s", g_strerror(errno));
                struct drm_mode_destroy_dumb dmd_out = {.handle = dmcd_out.handle};
                ioctl(vd->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmd_out);
                vd->frame_map[i].processed_handle = 0;
                stabilizer_buffers_ok = FALSE;
                continue;
            }
            vd->frame_map[i].processed_prime_fd = dph_out.fd;

            uint32_t proc_handles[4] = {0};
            uint32_t proc_pitches[4] = {0};
            uint32_t proc_offsets[4] = {0};
            proc_handles[0] = vd->frame_map[i].processed_handle;
            proc_handles[1] = vd->frame_map[i].processed_handle;
            proc_pitches[0] = dmcd_out.pitch;
            proc_pitches[1] = dmcd_out.pitch;
            proc_offsets[0] = 0;
            proc_offsets[1] = dmcd_out.pitch * ver_stride;

            ret = drmModeAddFB2(vd->drm_fd, width, height, DRM_FORMAT_NV12, proc_handles, proc_pitches, proc_offsets,
                                &vd->frame_map[i].processed_fb_id, 0);
            if (ret != 0) {
                LOGW("drmModeAddFB2 (processed) failed: %s", g_strerror(errno));
                close(vd->frame_map[i].processed_prime_fd);
                vd->frame_map[i].processed_prime_fd = -1;
                struct drm_mode_destroy_dumb dmd_out = {.handle = dmcd_out.handle};
                ioctl(vd->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmd_out);
                vd->frame_map[i].processed_handle = 0;
                stabilizer_buffers_ok = FALSE;
                continue;
            }
        }
    }

    vd->mpi->control(vd->ctx, MPP_DEC_SET_EXT_BUF_GROUP, vd->frm_grp);
    vd->mpi->control(vd->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);

    vd->src_w = width;
    vd->src_h = height;
    if (need_stabilizer && stabilizer_buffers_ok && vd->stabilizer) {
        if (video_stabilizer_configure(vd->stabilizer, width, height, hor_stride, ver_stride) == 0) {
            vd->stabilizer_ready = TRUE;
        } else {
            LOGW("Video stabilizer: failed to configure geometry; disabling");
            vd->stabilizer_ready = FALSE;
        }
    } else {
        vd->stabilizer_ready = FALSE;
    }
    vd->estimator_ready = FALSE;
    if (vd->estimator) {
        motion_estimator_update(vd->estimator, &vd->estimator_cfg);
        if (motion_estimator_configure(vd->estimator, width, height, hor_stride, ver_stride) == 0) {
            vd->estimator_ready = TRUE;
        } else {
            LOGW("Motion estimator: failed to configure geometry; disabling");
        }
    }

    commit_plane(vd, vd->frame_map[0].fb_id, width, height, -1, NULL);
    return 0;
}

static gpointer frame_thread_func(gpointer data) {
    VideoDecoder *vd = (VideoDecoder *)data;
    vd->frame_thread_running = TRUE;
    while (TRUE) {
        if (!vd->running) {
            break;
        }
        MppFrame frame = NULL;
        MPP_RET ret = vd->mpi->decode_get_frame(vd->ctx, &frame);
        if (ret != MPP_OK || frame == NULL) {
            g_usleep(1000);
            continue;
        }

        if (mpp_frame_get_info_change(frame)) {
            setup_external_buffers(vd, frame);
        } else {
            RK_U32 errinfo = mpp_frame_get_errinfo(frame);
            RK_U32 discard = mpp_frame_get_discard(frame);
            if (G_UNLIKELY(errinfo || discard)) {
                LOGW("MPP: dropping frame errinfo=%u discard=%u", errinfo, discard);
                if (vd->idr_requester != NULL) {
                    idr_requester_handle_warning(vd->idr_requester);
                }
                vd->eos_received = mpp_frame_get_eos(frame) ? TRUE : FALSE;
                mpp_frame_deinit(&frame);
                if (vd->eos_received) {
                    break;
                }
                continue;
            }

            MppBuffer buffer = mpp_frame_get_buffer(frame);
            if (buffer != NULL) {
                MppBufferInfo info;
                memset(&info, 0, sizeof(info));
                if (mpp_buffer_info_get(buffer, &info) == MPP_OK) {
                    for (int i = 0; i < DECODER_MAX_FRAMES; ++i) {
                        if (vd->frame_map[i].prime_fd == info.fd) {
                            uint32_t fb_to_signal = vd->frame_map[i].fb_id;
                            int in_fence_fd = -1;

                            if (vd->stabilizer && vd->stabilizer_ready && vd->frame_map[i].processed_prime_fd >= 0 &&
                                vd->frame_map[i].processed_fb_id != 0) {
                                StabilizerParams params = {0};
                                params.enable = vd->stabilizer_cfg.enable ? TRUE : FALSE;
                                params.acquire_fence_fd = -1;
                                params.has_transform = FALSE;
                                params.translate_x = 0.0f;
                                params.translate_y = 0.0f;
                                params.rotate_deg = 0.0f;

                                gboolean external_params = FALSE;
                                g_mutex_lock(&vd->lock);
                                if (vd->stabilizer_params_valid) {
                                    params = vd->pending_stabilizer_params;
                                    external_params = TRUE;
                                }
                                g_mutex_unlock(&vd->lock);

                                MotionEstimate estimate = {0};
                                gboolean estimate_valid = FALSE;
                                if (vd->estimator && vd->estimator_ready && vd->frame_map[i].map_addr != NULL) {
                                    int est_ret = motion_estimator_analyse(vd->estimator,
                                                                           (const uint8_t *)vd->frame_map[i].map_addr,
                                                                           vd->frame_map[i].map_size, &estimate);
                                    if (est_ret == 0 && estimate.valid) {
                                        estimate_valid = TRUE;
                                    }
                                }

                                if (!vd->stabilizer_cfg.enable) {
                                    params.enable = FALSE;
                                }

                                if (!external_params && estimate_valid) {
                                    params.enable = TRUE;
                                    params.has_transform = TRUE;
                                    params.translate_x = estimate.translate_x;
                                    params.translate_y = estimate.translate_y;
                                }

                                if (params.enable) {
                                    int acquire_fd = -1;
                                    if (mpp_frame_get_fd) {
                                        acquire_fd = mpp_frame_get_fd(frame);
                                    }
                                    if (acquire_fd >= 0 && acquire_fd != info.fd) {
                                        params.acquire_fence_fd = dup(acquire_fd);
                                        if (params.acquire_fence_fd < 0) {
                                            LOGW("Video stabilizer: failed to dup acquire fence: %s", g_strerror(errno));
                                        }
                                    }
                                    int release_fence_fd = -1;
                                    int process_ret = video_stabilizer_process(vd->stabilizer, info.fd,
                                                                               vd->frame_map[i].processed_prime_fd,
                                                                               &params, &release_fence_fd);
                                    if (process_ret == 0) {
                                        fb_to_signal = vd->frame_map[i].processed_fb_id;
                                        in_fence_fd = release_fence_fd;
                                    } else if (process_ret < 0) {
                                        if (release_fence_fd >= 0) {
                                            close(release_fence_fd);
                                        }
                                        LOGW("Video stabilizer: processing failed (%d); using raw frame", process_ret);
                                    }
                                }
                            }

                            g_mutex_lock(&vd->lock);
                            if (vd->pending_in_fence_fd >= 0) {
                                close(vd->pending_in_fence_fd);
                                vd->pending_in_fence_fd = -1;
                            }
                            vd->pending_fb = fb_to_signal;
                            vd->pending_pts = mpp_frame_get_pts(frame);
                            vd->pending_in_fence_fd = in_fence_fd;
                            vd->pending_src_w = mpp_frame_get_width(frame);
                            vd->pending_src_h = mpp_frame_get_height(frame);
                            g_cond_signal(&vd->cond);
                            g_mutex_unlock(&vd->lock);
                            break;
                        }
                    }
                }
            }
        }

        vd->eos_received = mpp_frame_get_eos(frame) ? TRUE : FALSE;
        mpp_frame_deinit(&frame);
        if (vd->eos_received) {
            break;
        }
    }

    g_mutex_lock(&vd->lock);
    vd->running = FALSE;
    g_cond_broadcast(&vd->cond);
    g_mutex_unlock(&vd->lock);
    vd->frame_thread_running = FALSE;
    return NULL;
}

static gpointer display_thread_func(gpointer data) {
    VideoDecoder *vd = (VideoDecoder *)data;
    vd->display_thread_running = TRUE;
    while (TRUE) {
        g_mutex_lock(&vd->lock);
        while (vd->running && vd->pending_fb == 0) {
            g_cond_wait(&vd->cond, &vd->lock);
        }
        uint32_t fb = vd->pending_fb;
        uint32_t src_w = vd->pending_src_w;
        uint32_t src_h = vd->pending_src_h;
        int in_fence_fd = vd->pending_in_fence_fd;
        vd->pending_fb = 0;
        vd->pending_src_w = 0;
        vd->pending_src_h = 0;
        vd->pending_in_fence_fd = -1;
        gboolean still_running = vd->running;
        g_mutex_unlock(&vd->lock);

        if (!still_running && fb == 0) {
            if (in_fence_fd >= 0) {
                close(in_fence_fd);
            }
            break;
        }
        if (fb != 0) {
            int out_fence_fd = -1;
            commit_plane(vd, fb, src_w, src_h, in_fence_fd, &out_fence_fd);
            if (out_fence_fd >= 0) {
                if (vd->last_out_fence_fd >= 0) {
                    close(vd->last_out_fence_fd);
                }
                vd->last_out_fence_fd = out_fence_fd;
            }
        } else if (in_fence_fd >= 0) {
            close(in_fence_fd);
        }
        if (!still_running) {
            break;
        }
    }
    vd->display_thread_running = FALSE;
    return NULL;
}

size_t video_decoder_max_packet_size(const VideoDecoder *vd) {
    if (vd == NULL || vd->packet_buf_size == 0) {
        return DECODER_READ_BUF_SIZE;
    }
    return vd->packet_buf_size;
}

int video_decoder_init(VideoDecoder *vd, const AppCfg *cfg, const ModesetResult *ms, int drm_fd) {
    if (vd == NULL || cfg == NULL || ms == NULL) {
        return -1;
    }

    log_decoder_neon_status_once();

    memset(vd, 0, sizeof(*vd));
    vd->drm_fd = -1;
    vd->plane_id = (uint32_t)cfg->plane_id;
    vd->crtc_id = ms->crtc_id;
    vd->mode_w = ms->mode_w;
    vd->mode_h = ms->mode_h;
    vd->packet_buf_size = DECODER_READ_BUF_SIZE;
    vd->packet_buf = g_malloc0(vd->packet_buf_size);
    if (vd->packet_buf == NULL) {
        LOGE("Video decoder: failed to allocate packet buffer");
        return -1;
    }

    vd->pending_in_fence_fd = -1;
    vd->last_out_fence_fd = -1;
    vd->stabilizer_cfg = cfg->stabilizer;
    vd->stabilizer_params_valid = FALSE;
    vd->stabilizer_ready = FALSE;
    vd->estimator_cfg = derive_estimator_cfg(&vd->stabilizer_cfg);
    vd->estimator_ready = FALSE;
    vd->stabilizer = video_stabilizer_new();
    if (vd->stabilizer != NULL) {
        if (video_stabilizer_init(vd->stabilizer, &vd->stabilizer_cfg) != 0) {
            LOGW("Video stabilizer: initialization failed");
            video_stabilizer_free(vd->stabilizer);
            vd->stabilizer = NULL;
        } else if (vd->stabilizer_cfg.enable && !video_stabilizer_is_available(vd->stabilizer)) {
            LOGW("Video stabilizer requested but unavailable; continuing without it");
        }
    }
    vd->estimator = motion_estimator_new();
    if (vd->estimator != NULL) {
        if (motion_estimator_init(vd->estimator, &vd->estimator_cfg) != 0) {
            LOGW("Motion estimator: initialization failed");
            motion_estimator_free(vd->estimator);
            vd->estimator = NULL;
        }
    }

    int dup_fd = fcntl(drm_fd, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0) {
        LOGE("Video decoder: failed to dup DRM fd: %s", g_strerror(errno));
        g_free(vd->packet_buf);
        vd->packet_buf = NULL;
        return -1;
    }
    vd->drm_fd = dup_fd;

    if (drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &vd->prop_fb_id) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &vd->prop_crtc_id) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", &vd->prop_crtc_x) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &vd->prop_crtc_y) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", &vd->prop_crtc_w) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", &vd->prop_crtc_h) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", &vd->prop_src_x) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", &vd->prop_src_y) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", &vd->prop_src_w) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", &vd->prop_src_h) != 0) {
        LOGE("Video decoder: failed to query plane properties");
        video_decoder_deinit(vd);
        return -1;
    }

    if (drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD", &vd->prop_in_fence_fd) != 0) {
        vd->prop_in_fence_fd = 0;
    }
    if (drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "OUT_FENCE_PTR", &vd->prop_out_fence_ptr) != 0) {
        vd->prop_out_fence_ptr = 0;
    }

    if (mpp_create(&vd->ctx, &vd->mpi) != MPP_OK) {
        LOGE("Video decoder: mpp_create failed");
        video_decoder_deinit(vd);
        return -1;
    }

    if (mpp_packet_init(&vd->packet, vd->packet_buf, vd->packet_buf_size) != MPP_OK) {
        LOGE("Video decoder: mpp_packet_init failed");
        video_decoder_deinit(vd);
        return -1;
    }

    const RK_U32 need_split = 1;
    set_control_verbose(vd->mpi, vd->ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, need_split);

    MppCodingType coding = MPP_VIDEO_CodingHEVC;
    if (mpp_init(vd->ctx, MPP_CTX_DEC, coding) != MPP_OK) {
        LOGE("Video decoder: mpp_init failed");
        video_decoder_deinit(vd);
        return -1;
    }

    set_mpp_decoding_parameters(vd);

#if defined(MPP_SET_OUTPUT_TIMEOUT)
    int64_t timeout = 5000; // allow decode_get_frame() to wake at ~5 ms intervals
    if (vd->mpi->control(vd->ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout) != MPP_OK) {
        LOGW("Video decoder: failed to set output timeout");
    }
#else
    int block = 5; // poll with a small timeout so shutdown does not block indefinitely
    if (vd->mpi->control(vd->ctx, MPP_SET_OUTPUT_BLOCK, &block) != MPP_OK) {
        LOGW("Video decoder: failed to set output block timeout");
    }
#endif

    g_mutex_init(&vd->lock);
    g_cond_init(&vd->cond);
    vd->lock_initialized = TRUE;
    vd->cond_initialized = TRUE;
    vd->pending_fb = 0;
    vd->pending_pts = 0;
    vd->frm_grp = NULL;
    reset_frame_map(vd);

    vd->initialized = TRUE;
    return 0;
}

void video_decoder_deinit(VideoDecoder *vd) {
    if (vd == NULL) {
        return;
    }

    video_decoder_stop(vd);

    if (vd->pending_in_fence_fd >= 0) {
        close(vd->pending_in_fence_fd);
        vd->pending_in_fence_fd = -1;
    }
    if (vd->last_out_fence_fd >= 0) {
        close(vd->last_out_fence_fd);
        vd->last_out_fence_fd = -1;
    }

    if (vd->stabilizer) {
        video_stabilizer_free(vd->stabilizer);
        vd->stabilizer = NULL;
        vd->stabilizer_ready = FALSE;
    }
    if (vd->estimator) {
        motion_estimator_free(vd->estimator);
        vd->estimator = NULL;
        vd->estimator_ready = FALSE;
    }

    if (vd->packet) {
        mpp_packet_deinit(&vd->packet);
        vd->packet = NULL;
    }

    if (vd->mpi && vd->ctx) {
        vd->mpi->reset(vd->ctx);
        mpp_destroy(vd->ctx);
    }
    vd->ctx = NULL;
    vd->mpi = NULL;

    release_frame_group(vd);

    if (vd->packet_buf) {
        g_free(vd->packet_buf);
        vd->packet_buf = NULL;
    }

    if (vd->drm_fd >= 0) {
        close(vd->drm_fd);
        vd->drm_fd = -1;
    }

    if (vd->lock_initialized) {
        g_mutex_clear(&vd->lock);
        vd->lock_initialized = FALSE;
    }
    if (vd->cond_initialized) {
        g_cond_clear(&vd->cond);
        vd->cond_initialized = FALSE;
    }
    vd->initialized = FALSE;
}

int video_decoder_start(VideoDecoder *vd) {
    if (vd == NULL || !vd->initialized) {
        return -1;
    }
    if (vd->running) {
        return 0;
    }

    vd->running = TRUE;
    vd->eos_received = FALSE;

    vd->frame_thread = g_thread_new("mpp-frame", frame_thread_func, vd);
    if (vd->frame_thread == NULL) {
        vd->running = FALSE;
        return -1;
    }
    vd->display_thread = g_thread_new("mpp-display", display_thread_func, vd);
    if (vd->display_thread == NULL) {
        vd->running = FALSE;
        g_thread_join(vd->frame_thread);
        vd->frame_thread = NULL;
        return -1;
    }
    return 0;
}

void video_decoder_stop(VideoDecoder *vd) {
    if (vd == NULL) {
        return;
    }

    gboolean was_running = FALSE;

    if (vd->lock_initialized) {
        g_mutex_lock(&vd->lock);
        was_running = vd->running;
        vd->running = FALSE;
        if (vd->cond_initialized) {
            g_cond_broadcast(&vd->cond);
        }
        g_mutex_unlock(&vd->lock);
    } else {
        was_running = vd->running;
        vd->running = FALSE;
        if (vd->cond_initialized) {
            g_cond_broadcast(&vd->cond);
        }
    }

    if (was_running && vd->mpi && vd->ctx) {
        video_decoder_send_eos(vd);

        MPP_RET ret = vd->mpi->reset(vd->ctx);
        if (ret != MPP_OK) {
            LOGW("Video decoder: reset during stop failed (%d)", ret);
        }
    }

    if (vd->frame_thread) {
        g_thread_join(vd->frame_thread);
        vd->frame_thread = NULL;
    }
    if (vd->display_thread) {
        g_thread_join(vd->display_thread);
        vd->display_thread = NULL;
    }
}

int video_decoder_feed(VideoDecoder *vd, const guint8 *data, size_t size) {
    if (vd == NULL || !vd->running) {
        return -1;
    }
    if (size == 0 || size > vd->packet_buf_size) {
        return -1;
    }

    copy_packet_data(vd->packet_buf, data, size);
    mpp_packet_set_length(vd->packet, 0);
    mpp_packet_set_size(vd->packet, vd->packet_buf_size);
    mpp_packet_set_data(vd->packet, vd->packet_buf);
    mpp_packet_set_pos(vd->packet, vd->packet_buf);
    mpp_packet_set_length(vd->packet, size);
    mpp_packet_set_pts(vd->packet, (RK_S64)get_time_ms());

    while (vd->running) {
        MPP_RET ret = vd->mpi->decode_put_packet(vd->ctx, vd->packet);
        if (ret == MPP_OK) {
            return 0;
        }
        g_usleep(2000);
    }
    return -1;
}

void video_decoder_send_eos(VideoDecoder *vd) {
    if (vd == NULL || vd->packet == NULL) {
        return;
    }

    mpp_packet_set_length(vd->packet, 0);
    mpp_packet_set_size(vd->packet, vd->packet_buf_size);
    mpp_packet_set_data(vd->packet, vd->packet_buf);
    mpp_packet_set_pos(vd->packet, vd->packet_buf);
    mpp_packet_set_eos(vd->packet);

    int attempts = 0;
    while (vd->mpi && vd->ctx) {
        MPP_RET ret = vd->mpi->decode_put_packet(vd->ctx, vd->packet);
        if (ret == MPP_OK) {
            break;
        }
        if (ret != MPP_ERR_BUFFER_FULL) {
            LOGW("Video decoder: decode_put_packet(EOS) failed (%d)", ret);
        }
        if (++attempts > 50) {
            LOGW("Video decoder: giving up on EOS after %d attempts", attempts);
            break;
        }
        g_usleep(2000);
    }
}

void video_decoder_set_idr_requester(VideoDecoder *vd, IdrRequester *requester) {
    if (vd == NULL) {
        return;
    }
    vd->idr_requester = requester;
}

void video_decoder_set_stabilizer_params(VideoDecoder *vd, const StabilizerParams *params) {
    if (vd == NULL) {
        return;
    }
    if (vd->lock_initialized) {
        g_mutex_lock(&vd->lock);
    }
    if (params != NULL) {
        vd->pending_stabilizer_params = *params;
        vd->stabilizer_params_valid = TRUE;
    } else {
        memset(&vd->pending_stabilizer_params, 0, sizeof(vd->pending_stabilizer_params));
        vd->stabilizer_params_valid = FALSE;
    }
    if (vd->lock_initialized) {
        g_mutex_unlock(&vd->lock);
    }
}
