#include "video_ctm.h"

#include "logging.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_mode.h>

#if defined(HAVE_LIBRGA)
#include <drm_fourcc.h>
#endif

#if defined(HAVE_LIBRGA)
/* Fixed-point lookup tables (10 fractional bits) avoid per-pixel multiplications. */
#define CTM_LUT_SHIFT 10
#define CTM_LUT_SCALE (1 << CTM_LUT_SHIFT)
#define CTM_LUT_ROUND (1 << (CTM_LUT_SHIFT - 1))
#endif

static void video_ctm_set_identity(VideoCtm *ctm) {
    if (ctm == NULL) {
        return;
    }
    for (int i = 0; i < 9; ++i) {
        ctm->matrix[i] = (i % 4 == 0) ? 1.0 : 0.0;
    }
}

static gboolean video_ctm_hw_available(const VideoCtm *ctm) {
    return (ctm != NULL && ctm->hw_supported && ctm->hw_fd >= 0 && ctm->hw_prop_id != 0 &&
            ctm->hw_object_id != 0 && ctm->hw_object_type != 0);
}

static void video_ctm_destroy_blob(VideoCtm *ctm) {
    if (ctm == NULL) {
        return;
    }
    if (ctm->hw_blob_id != 0 && ctm->hw_fd >= 0) {
        drmModeDestroyPropertyBlob(ctm->hw_fd, ctm->hw_blob_id);
        ctm->hw_blob_id = 0;
    }
}

static uint64_t video_ctm_to_s3132(double value) {
    double abs_val = fabs(value);
    const double max_val = ((double)((1ULL << 63) - 1)) / (double)(1ULL << 32);
    if (abs_val > max_val) {
        abs_val = max_val;
    }
    uint64_t magnitude = (uint64_t)llround(abs_val * (double)(1ULL << 32));
    if (magnitude > ((1ULL << 63) - 1)) {
        magnitude = (1ULL << 63) - 1;
    }
    if (value < 0.0) {
        magnitude |= (1ULL << 63);
    }
    return magnitude;
}

static gboolean video_ctm_apply_hw(VideoCtm *ctm) {
    if (!video_ctm_hw_available(ctm) || !ctm->enabled) {
        return FALSE;
    }

    struct drm_color_ctm blob;
    for (int i = 0; i < 9; ++i) {
        blob.matrix[i] = video_ctm_to_s3132(ctm->matrix[i]);
    }

    video_ctm_destroy_blob(ctm);

    if (drmModeCreatePropertyBlob(ctm->hw_fd, &blob, sizeof(blob), &ctm->hw_blob_id) != 0) {
        LOGW("Video CTM: failed to create DRM CTM blob: %s", g_strerror(errno));
        ctm->hw_blob_id = 0;
        return FALSE;
    }

    if (drmModeObjectSetProperty(ctm->hw_fd, ctm->hw_object_id, ctm->hw_object_type, ctm->hw_prop_id,
                                 ctm->hw_blob_id) != 0) {
        LOGW("Video CTM: failed to set DRM CTM property: %s", g_strerror(errno));
        drmModeDestroyPropertyBlob(ctm->hw_fd, ctm->hw_blob_id);
        ctm->hw_blob_id = 0;
        return FALSE;
    }

    ctm->hw_applied = TRUE;
    return TRUE;
}

void video_ctm_init(VideoCtm *ctm, const AppCfg *cfg) {
    if (ctm == NULL) {
        return;
    }
    memset(ctm, 0, sizeof(*ctm));
    video_ctm_set_identity(ctm);
    ctm->hw_supported = FALSE;
    ctm->hw_applied = FALSE;
    ctm->hw_fd = -1;
    ctm->hw_object_id = 0;
    ctm->hw_object_type = 0;
    ctm->hw_prop_id = 0;
    ctm->hw_blob_id = 0;

    if (cfg != NULL) {
        if (cfg->video_ctm.enable) {
            ctm->enabled = TRUE;
        }
        for (int i = 0; i < 9; ++i) {
            ctm->matrix[i] = cfg->video_ctm.matrix[i];
        }
    }
#if !defined(HAVE_LIBRGA)
    if (ctm->enabled) {
        LOGW("Video CTM requested but librga support is unavailable; disabling color transform");
        ctm->enabled = FALSE;
    }
#else
    ctm->lut_ready = FALSE;
#endif
}

void video_ctm_reset(VideoCtm *ctm) {
    if (ctm == NULL) {
        return;
    }
    video_ctm_disable_drm(ctm);
#if defined(HAVE_LIBRGA)
    if (ctm->rgba_buf) {
        g_free(ctm->rgba_buf);
        ctm->rgba_buf = NULL;
    }
    ctm->rgba_buf_size = 0;
    ctm->rgba_width = 0;
    ctm->rgba_height = 0;
    ctm->rgba_stride = 0;
    ctm->rgba_ver_stride = 0;
    ctm->lut_ready = FALSE;
#endif
}

void video_ctm_use_drm_property(VideoCtm *ctm, int drm_fd, uint32_t object_id, uint32_t object_type,
                                uint32_t prop_id) {
    if (ctm == NULL) {
        return;
    }

    video_ctm_disable_drm(ctm);

    ctm->hw_fd = drm_fd;
    ctm->hw_object_id = object_id;
    ctm->hw_object_type = object_type;
    ctm->hw_prop_id = prop_id;
    ctm->hw_supported = (drm_fd >= 0 && object_id != 0 && prop_id != 0);
}

void video_ctm_disable_drm(VideoCtm *ctm) {
    if (ctm == NULL) {
        return;
    }

    if (ctm->hw_applied && video_ctm_hw_available(ctm)) {
        if (drmModeObjectSetProperty(ctm->hw_fd, ctm->hw_object_id, ctm->hw_object_type, ctm->hw_prop_id, 0) != 0) {
            LOGW("Video CTM: failed to clear DRM CTM property: %s", g_strerror(errno));
        }
    }
    ctm->hw_applied = FALSE;
    video_ctm_destroy_blob(ctm);
}

#if defined(HAVE_LIBRGA)
static gboolean video_ctm_build_lut(VideoCtm *ctm) {
    if (ctm == NULL) {
        return FALSE;
    }

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            double coeff = ctm->matrix[row * 3 + col];
            if (!isfinite(coeff)) {
                LOGW("Video CTM: invalid coefficient (%d,%d); disabling transform", row, col);
                ctm->enabled = FALSE;
                ctm->lut_ready = FALSE;
                return FALSE;
            }
            for (int value = 0; value < 256; ++value) {
                double scaled = coeff * (double)value * (double)CTM_LUT_SCALE;
                if (scaled > (double)INT32_MAX) {
                    scaled = (double)INT32_MAX;
                } else if (scaled < (double)INT32_MIN) {
                    scaled = (double)INT32_MIN;
                }
                ctm->lut[row][col][value] = (int32_t)llround(scaled);
            }
        }
    }

    ctm->lut_ready = TRUE;
    return TRUE;
}

static inline guint8 clamp_byte_from_shifted(int64_t value) {
    int64_t shifted;
    if (value >= 0) {
        shifted = (value + CTM_LUT_ROUND) >> CTM_LUT_SHIFT;
    } else {
        shifted = (value - CTM_LUT_ROUND) >> CTM_LUT_SHIFT;
    }
    if (shifted < 0) {
        return 0;
    }
    if (shifted > 255) {
        return 255;
    }
    return (guint8)shifted;
}

static void apply_rgba_matrix(VideoCtm *ctm, uint32_t width, uint32_t height) {
    if (ctm == NULL || ctm->rgba_buf == NULL || ctm->rgba_stride == 0) {
        return;
    }
    if (!ctm->lut_ready && !video_ctm_build_lut(ctm)) {
        return;
    }

    guint8 *row = ctm->rgba_buf;
    size_t stride_bytes = (size_t)ctm->rgba_stride * 4u;
    for (uint32_t y = 0; y < height; ++y) {
        guint8 *pixel = row + (size_t)y * stride_bytes;
        for (uint32_t x = 0; x < width; ++x) {
            guint8 r = pixel[0];
            guint8 g = pixel[1];
            guint8 b = pixel[2];

            int64_t acc_r = (int64_t)ctm->lut[0][0][r] + (int64_t)ctm->lut[0][1][g] +
                            (int64_t)ctm->lut[0][2][b];
            int64_t acc_g = (int64_t)ctm->lut[1][0][r] + (int64_t)ctm->lut[1][1][g] +
                            (int64_t)ctm->lut[1][2][b];
            int64_t acc_b = (int64_t)ctm->lut[2][0][r] + (int64_t)ctm->lut[2][1][g] +
                            (int64_t)ctm->lut[2][2][b];

            pixel[0] = clamp_byte_from_shifted(acc_r);
            pixel[1] = clamp_byte_from_shifted(acc_g);
            pixel[2] = clamp_byte_from_shifted(acc_b);

            pixel += 4;
        }
    }
}
#endif

int video_ctm_prepare(VideoCtm *ctm, uint32_t width, uint32_t height, uint32_t hor_stride,
                      uint32_t ver_stride, uint32_t fourcc) {
    if (ctm == NULL || !ctm->enabled) {
        video_ctm_disable_drm(ctm);
        return 0;
    }
    if (video_ctm_hw_available(ctm)) {
        if (video_ctm_apply_hw(ctm)) {
            return 0;
        }
        LOGW("Video CTM: falling back to software path after DRM CTM failure");
        video_ctm_disable_drm(ctm);
        ctm->hw_supported = FALSE;
    }

#if !defined(HAVE_LIBRGA)
    (void)width;
    (void)height;
    (void)hor_stride;
    (void)ver_stride;
    (void)fourcc;
    return -1;
#else

    if (fourcc != DRM_FORMAT_NV12) {
        LOGW("Video CTM: unsupported DRM format 0x%08x; disabling transform", fourcc);
        ctm->enabled = FALSE;
        return -1;
    }
    (void)hor_stride;
    (void)ver_stride;

    if (width == 0 || height == 0) {
        LOGW("Video CTM: refusing to prepare zero-sized buffer (%ux%u)", width, height);
        ctm->enabled = FALSE;
        return -1;
    }

    uint32_t rgba_stride = hor_stride != 0 ? hor_stride : width;
    uint32_t rgba_ver_stride = ver_stride != 0 ? ver_stride : height;
    if (rgba_stride < width || rgba_ver_stride < height) {
        LOGW("Video CTM: invalid stride %u/%u for %ux%u frame; disabling transform", rgba_stride, rgba_ver_stride,
             width, height);
        ctm->enabled = FALSE;
        return -1;
    }

    if (!ctm->lut_ready && !video_ctm_build_lut(ctm)) {
        return -1;
    }

    size_t needed = (size_t)rgba_stride * rgba_ver_stride * 4u;
    if (ctm->rgba_buf_size < needed) {
        guint8 *new_buf = g_try_malloc0(needed);
        if (new_buf == NULL) {
            LOGE("Video CTM: failed to allocate intermediate RGBA buffer");
            ctm->enabled = FALSE;
            return -1;
        }
        if (ctm->rgba_buf) {
            g_free(ctm->rgba_buf);
        }
        ctm->rgba_buf = new_buf;
        ctm->rgba_buf_size = needed;
    }
    ctm->rgba_width = width;
    ctm->rgba_height = height;
    ctm->rgba_stride = rgba_stride;
    ctm->rgba_ver_stride = rgba_ver_stride;
    return 0;
#endif
}

int video_ctm_process(VideoCtm *ctm, int src_fd, int dst_fd, uint32_t width, uint32_t height,
                      uint32_t hor_stride, uint32_t ver_stride, uint32_t fourcc) {
    if (ctm == NULL || !ctm->enabled) {
        return -1;
    }
    if (video_ctm_hw_available(ctm)) {
        return ctm->hw_applied ? 0 : -1;
    }
#if !defined(HAVE_LIBRGA)
    (void)src_fd;
    (void)dst_fd;
    (void)width;
    (void)height;
    (void)hor_stride;
    (void)ver_stride;
    (void)fourcc;
    return -1;
#else
    if (ctm->rgba_buf == NULL || ctm->rgba_width != width || ctm->rgba_height != height) {
        if (video_ctm_prepare(ctm, width, height, hor_stride, ver_stride, fourcc) != 0) {
            return -1;
        }
    }

    if (fourcc != DRM_FORMAT_NV12) {
        return -1;
    }

    rga_buffer_t src = wrapbuffer_fd(src_fd, (int)width, (int)height, RK_FORMAT_YCbCr_420_SP,
                                     (int)hor_stride, (int)ver_stride);
    rga_buffer_t dst = wrapbuffer_fd(dst_fd, (int)width, (int)height, RK_FORMAT_YCbCr_420_SP,
                                     (int)hor_stride, (int)ver_stride);
    rga_buffer_t tmp = wrapbuffer_virtualaddr(ctm->rgba_buf, (int)width, (int)height, RK_FORMAT_RGBA_8888,
                                              (int)ctm->rgba_stride, (int)ctm->rgba_ver_stride);

    IM_STATUS ret = imcvtcolor(src, tmp, src.format, tmp.format, IM_COLOR_SPACE_DEFAULT);
    if (ret != IM_STATUS_SUCCESS) {
        LOGW("Video CTM: imcvtcolor NV12->RGBA failed: %s", imStrError(ret));
        ctm->enabled = FALSE;
        return -1;
    }

    apply_rgba_matrix(ctm, width, height);

    ret = imcvtcolor(tmp, dst, tmp.format, dst.format, IM_COLOR_SPACE_DEFAULT);
    if (ret != IM_STATUS_SUCCESS) {
        LOGW("Video CTM: imcvtcolor RGBA->NV12 failed: %s", imStrError(ret));
        ctm->enabled = FALSE;
        return -1;
    }

    return 0;
#endif
}
