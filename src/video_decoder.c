#include "video_decoder.h"

#include "drm_props.h"
#include "idr_requester.h"
#include "logging.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <gst/gst.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_err.h>

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

    gboolean zoom_enabled;
    VideoDecoderZoomRequest zoom_request;
    VideoDecoderZoomRect zoom_rect;
    uint32_t last_committed_fb;

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

    GThread *frame_thread;
    GThread *display_thread;

    IdrRequester *idr_requester;
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

static gboolean sanitize_zoom_rect(uint32_t max_w, uint32_t max_h, VideoDecoderZoomRect *rect) {
    if (rect == NULL || max_w == 0 || max_h == 0) {
        return FALSE;
    }
    if (rect->w == 0 || rect->h == 0) {
        return FALSE;
    }
    if (rect->w > max_w) {
        rect->w = max_w;
    }
    if (rect->h > max_h) {
        rect->h = max_h;
    }
    if (rect->x >= max_w) {
        rect->x = max_w - rect->w;
    }
    if (rect->y >= max_h) {
        rect->y = max_h - rect->h;
    }
    if (rect->x + rect->w > max_w) {
        rect->x = max_w - rect->w;
    }
    if (rect->y + rect->h > max_h) {
        rect->y = max_h - rect->h;
    }
    return rect->w > 0 && rect->h > 0;
}

static gboolean sanitize_zoom_request(VideoDecoderZoomRequest *request) {
    if (request == NULL) {
        return FALSE;
    }
    if (request->scale_x_percent == 0 || request->scale_y_percent == 0) {
        return FALSE;
    }
    if (request->scale_x_percent > 100) {
        request->scale_x_percent = 100;
    }
    if (request->scale_y_percent > 100) {
        request->scale_y_percent = 100;
    }
    if (request->center_x_percent > 100) {
        request->center_x_percent = 100;
    }
    if (request->center_y_percent > 100) {
        request->center_y_percent = 100;
    }
    return TRUE;
}

static gboolean compute_zoom_rect_from_request(uint32_t frame_w, uint32_t frame_h,
                                               const VideoDecoderZoomRequest *request,
                                               VideoDecoderZoomRect *rect) {
    if (rect == NULL || request == NULL) {
        return FALSE;
    }
    if (frame_w == 0 || frame_h == 0) {
        return FALSE;
    }

    double scale_x = (double)request->scale_x_percent / 100.0;
    double scale_y = (double)request->scale_y_percent / 100.0;
    if (scale_x <= 0.0 || scale_y <= 0.0) {
        return FALSE;
    }

    uint32_t width = (uint32_t)ceil((double)frame_w * scale_x);
    uint32_t height = (uint32_t)ceil((double)frame_h * scale_y);
    if (width == 0) {
        width = 1;
    }
    if (height == 0) {
        height = 1;
    }
    if (width > frame_w) {
        width = frame_w;
    }
    if (height > frame_h) {
        height = frame_h;
    }

    double center_x = ((double)frame_w - 1.0) * ((double)request->center_x_percent / 100.0);
    double center_y = ((double)frame_h - 1.0) * ((double)request->center_y_percent / 100.0);

    double half_w = (double)width / 2.0;
    double half_h = (double)height / 2.0;

    double left = center_x - half_w;
    double top = center_y - half_h;

    double max_left = (double)frame_w - (double)width;
    double max_top = (double)frame_h - (double)height;

    if (left < 0.0) {
        left = 0.0;
    }
    if (top < 0.0) {
        top = 0.0;
    }
    if (left > max_left) {
        left = max_left;
    }
    if (top > max_top) {
        top = max_top;
    }

    rect->w = width;
    rect->h = height;
    rect->x = (uint32_t)llround(left);
    rect->y = (uint32_t)llround(top);

    if (rect->x + rect->w > frame_w) {
        rect->x = frame_w - rect->w;
    }
    if (rect->y + rect->h > frame_h) {
        rect->y = frame_h - rect->h;
    }

    return rect->w > 0 && rect->h > 0;
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
        if (vd->frame_map[i].handle) {
            struct drm_mode_destroy_dumb dmd = {.handle = vd->frame_map[i].handle};
            ioctl(vd->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmd);
            vd->frame_map[i].handle = 0;
        }
    }
    mpp_buffer_group_clear(vd->frm_grp);
    mpp_buffer_group_put(vd->frm_grp);
    vd->frm_grp = NULL;
    vd->src_w = 0;
    vd->src_h = 0;
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

static int commit_plane(VideoDecoder *vd, uint32_t fb_id, uint32_t src_w, uint32_t src_h) {
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        return -1;
    }

    uint32_t base_src_w = 0;
    uint32_t base_src_h = 0;
    gboolean zoom_enabled = FALSE;
    VideoDecoderZoomRect zoom_rect = {0};

    g_mutex_lock(&vd->lock);
    base_src_w = vd->src_w;
    base_src_h = vd->src_h;
    if (src_w != 0) {
        base_src_w = src_w;
    } else if (base_src_w == 0) {
        base_src_w = vd->mode_w > 0 ? (uint32_t)vd->mode_w : 1;
    }
    if (src_h != 0) {
        base_src_h = src_h;
    } else if (base_src_h == 0) {
        base_src_h = vd->mode_h > 0 ? (uint32_t)vd->mode_h : 1;
    }
    zoom_enabled = vd->zoom_enabled;
    zoom_rect = vd->zoom_rect;
    g_mutex_unlock(&vd->lock);

    if (base_src_w == 0) {
        base_src_w = 1;
    }
    if (base_src_h == 0) {
        base_src_h = 1;
    }

    gboolean use_zoom = FALSE;
    VideoDecoderZoomRect zoom_local = zoom_rect;
    if (zoom_enabled && sanitize_zoom_rect(base_src_w, base_src_h, &zoom_local)) {
        use_zoom = TRUE;
    }

    uint32_t display_src_w = use_zoom ? zoom_local.w : base_src_w;
    uint32_t display_src_h = use_zoom ? zoom_local.h : base_src_h;

    if (display_src_w == 0) {
        display_src_w = 1;
    }
    if (display_src_h == 0) {
        display_src_h = 1;
    }

    int dst_x = 0;
    int dst_y = 0;
    int dst_w = vd->mode_w;
    int dst_h = vd->mode_h;

    if (display_src_w > 0 && display_src_h > 0 && vd->mode_w > 0 && vd->mode_h > 0) {
        double src_ar = (double)display_src_w / (double)display_src_h;
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

    uint64_t src_x_q16 = use_zoom ? ((uint64_t)zoom_local.x << 16) : 0;
    uint64_t src_y_q16 = use_zoom ? ((uint64_t)zoom_local.y << 16) : 0;
    uint64_t src_w_q16 = (uint64_t)(use_zoom ? zoom_local.w : base_src_w) << 16;
    uint64_t src_h_q16 = (uint64_t)(use_zoom ? zoom_local.h : base_src_h) << 16;

    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_fb_id, fb_id);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_crtc_id, vd->crtc_id);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_crtc_x, dst_x);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_crtc_y, dst_y);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_crtc_w, dst_w);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_crtc_h, dst_h);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_src_x, src_x_q16);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_src_y, src_y_q16);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_src_w, src_w_q16);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_src_h, src_h_q16);

    int ret = drmModeAtomicCommit(vd->drm_fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
    if (ret != 0 && errno == EBUSY) {
        ret = drmModeAtomicCommit(vd->drm_fd, req, 0, NULL);
    }
    if (ret != 0) {
        LOGW("Atomic commit failed: %s", g_strerror(errno));
    } else {
        g_mutex_lock(&vd->lock);
        if (src_w != 0) {
            vd->src_w = src_w;
        } else if (vd->src_w == 0) {
            vd->src_w = base_src_w;
        }
        if (src_h != 0) {
            vd->src_h = src_h;
        } else if (vd->src_h == 0) {
            vd->src_h = base_src_h;
        }
        vd->last_committed_fb = fb_id;
        g_mutex_unlock(&vd->lock);
    }
    drmModeAtomicFree(req);
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
    }

    vd->mpi->control(vd->ctx, MPP_DEC_SET_EXT_BUF_GROUP, vd->frm_grp);
    vd->mpi->control(vd->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);

    g_mutex_lock(&vd->lock);
    vd->src_w = width;
    vd->src_h = height;
    if (vd->zoom_enabled) {
        VideoDecoderZoomRect adjusted = {0};
        if (!compute_zoom_rect_from_request(width, height, &vd->zoom_request, &adjusted) ||
            !sanitize_zoom_rect(width, height, &adjusted)) {
            LOGW("Video decoder: disabling zoom after %ux%u format change", width, height);
            vd->zoom_enabled = FALSE;
            memset(&vd->zoom_request, 0, sizeof(vd->zoom_request));
            memset(&vd->zoom_rect, 0, sizeof(vd->zoom_rect));
        } else if (adjusted.x != vd->zoom_rect.x || adjusted.y != vd->zoom_rect.y || adjusted.w != vd->zoom_rect.w ||
                   adjusted.h != vd->zoom_rect.h) {
            LOGI("Video decoder: clamped zoom to %ux%u+%u,%u for %ux%u frame", adjusted.w, adjusted.h,
                 adjusted.x, adjusted.y, width, height);
            vd->zoom_rect = adjusted;
        }
    }
    g_mutex_unlock(&vd->lock);

    commit_plane(vd, vd->frame_map[0].fb_id, width, height);
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
                            g_mutex_lock(&vd->lock);
                            vd->pending_fb = vd->frame_map[i].fb_id;
                            vd->pending_pts = mpp_frame_get_pts(frame);
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
        vd->pending_fb = 0;
        gboolean still_running = vd->running;
        g_mutex_unlock(&vd->lock);

        if (!still_running && fb == 0) {
            break;
        }
        if (fb != 0) {
            commit_plane(vd, fb, 0, 0);
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

int video_decoder_set_zoom(VideoDecoder *vd, gboolean enabled, const VideoDecoderZoomRequest *request) {
    if (vd == NULL) {
        return -1;
    }
    if (!enabled) {
        g_mutex_lock(&vd->lock);
        gboolean was_enabled = vd->zoom_enabled;
        vd->zoom_enabled = FALSE;
        memset(&vd->zoom_request, 0, sizeof(vd->zoom_request));
        memset(&vd->zoom_rect, 0, sizeof(vd->zoom_rect));
        uint32_t fb = was_enabled ? vd->last_committed_fb : 0;
        g_mutex_unlock(&vd->lock);
        if (was_enabled && fb != 0) {
            if (commit_plane(vd, fb, 0, 0) != 0) {
                LOGW("Video decoder: failed to clear zoom on framebuffer %u", fb);
            }
        }
        return 0;
    }

    if (request == NULL) {
        LOGW("Video decoder: zoom request missing parameters");
        return -1;
    }

    g_mutex_lock(&vd->lock);
    uint32_t max_w = vd->src_w ? vd->src_w : (uint32_t)vd->mode_w;
    uint32_t max_h = vd->src_h ? vd->src_h : (uint32_t)vd->mode_h;
    VideoDecoderZoomRequest sanitized_request = *request;
    gboolean changed = FALSE;

    if (!sanitize_zoom_request(&sanitized_request)) {
        g_mutex_unlock(&vd->lock);
        LOGW("Video decoder: rejected zoom percentages %u%%x%u%% @ %u%%,%u%%",
             request->scale_x_percent, request->scale_y_percent, request->center_x_percent, request->center_y_percent);
        return -1;
    }

    gboolean request_was_clamped = sanitized_request.scale_x_percent != request->scale_x_percent ||
                                   sanitized_request.scale_y_percent != request->scale_y_percent ||
                                   sanitized_request.center_x_percent != request->center_x_percent ||
                                   sanitized_request.center_y_percent != request->center_y_percent;

    if (max_w == 0 || max_h == 0) {
        gboolean queued_request_changed = !vd->zoom_enabled ||
                                          vd->zoom_request.scale_x_percent != sanitized_request.scale_x_percent ||
                                          vd->zoom_request.scale_y_percent != sanitized_request.scale_y_percent ||
                                          vd->zoom_request.center_x_percent != sanitized_request.center_x_percent ||
                                          vd->zoom_request.center_y_percent != sanitized_request.center_y_percent;
        if (queued_request_changed) {
            vd->zoom_enabled = TRUE;
            vd->zoom_request = sanitized_request;
            memset(&vd->zoom_rect, 0, sizeof(vd->zoom_rect));
        }
        g_mutex_unlock(&vd->lock);
        LOGI("Video decoder: queued zoom request (%u%%x%u%% @ %u%%,%u%%) until frame size is known",
             sanitized_request.scale_x_percent, sanitized_request.scale_y_percent,
             sanitized_request.center_x_percent, sanitized_request.center_y_percent);
        return 0;
    }

    VideoDecoderZoomRect rect = {0};
    if (!compute_zoom_rect_from_request(max_w, max_h, &sanitized_request, &rect) ||
        !sanitize_zoom_rect(max_w, max_h, &rect)) {
        g_mutex_unlock(&vd->lock);
        LOGW("Video decoder: rejected zoom request after clamping (frame %ux%u)", max_w, max_h);
        return -1;
    }

    gboolean request_changed = !vd->zoom_enabled ||
                               vd->zoom_request.scale_x_percent != sanitized_request.scale_x_percent ||
                               vd->zoom_request.scale_y_percent != sanitized_request.scale_y_percent ||
                               vd->zoom_request.center_x_percent != sanitized_request.center_x_percent ||
                               vd->zoom_request.center_y_percent != sanitized_request.center_y_percent;

    gboolean rect_changed = !vd->zoom_enabled || vd->zoom_rect.x != rect.x || vd->zoom_rect.y != rect.y ||
                            vd->zoom_rect.w != rect.w || vd->zoom_rect.h != rect.h;

    if (request_changed || rect_changed) {
        vd->zoom_enabled = TRUE;
        vd->zoom_request = sanitized_request;
        vd->zoom_rect = rect;
        changed = TRUE;
    }
    VideoDecoderZoomRequest applied_request = sanitized_request;

    uint32_t fb = changed ? vd->last_committed_fb : 0;
    g_mutex_unlock(&vd->lock);

    if (changed && fb != 0) {
        if (commit_plane(vd, fb, 0, 0) != 0) {
            LOGW("Video decoder: failed to apply zoom on framebuffer %u", fb);
        }
    }
    if (request_was_clamped) {
        LOGI("Video decoder: clamped zoom request to %u%%x%u%% @ %u%%,%u%%",
             applied_request.scale_x_percent, applied_request.scale_y_percent,
             applied_request.center_x_percent, applied_request.center_y_percent);
    }
    return 0;
}

void video_decoder_set_idr_requester(VideoDecoder *vd, IdrRequester *requester) {
    if (vd == NULL) {
        return;
    }
    vd->idr_requester = requester;
}
