#include "video_ctm.h"

#include "logging.h"

#include <math.h>
#include <string.h>

#if defined(HAVE_LIBRGA)
#include <drm_fourcc.h>
#endif

static void video_ctm_set_identity(VideoCtm *ctm) {
    if (ctm == NULL) {
        return;
    }
    for (int i = 0; i < 9; ++i) {
        ctm->matrix[i] = (i % 4 == 0) ? 1.0 : 0.0;
    }
}

void video_ctm_init(VideoCtm *ctm, const AppCfg *cfg) {
    if (ctm == NULL) {
        return;
    }
    memset(ctm, 0, sizeof(*ctm));
    video_ctm_set_identity(ctm);

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
#endif
}

void video_ctm_reset(VideoCtm *ctm) {
    if (ctm == NULL) {
        return;
    }
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
#endif
}

#if defined(HAVE_LIBRGA)
static inline double clamp_unit(double v) {
    if (v < 0.0) {
        return 0.0;
    }
    if (v > 1.0) {
        return 1.0;
    }
    return v;
}

static inline guint8 clamp_byte_from_unit(double v) {
    double scaled = v * 255.0;
    if (scaled < 0.0) {
        return 0;
    }
    if (scaled > 255.0) {
        return 255;
    }
    return (guint8)lround(scaled);
}

static void apply_rgba_matrix(VideoCtm *ctm, uint32_t width, uint32_t height) {
    if (ctm == NULL || ctm->rgba_buf == NULL || ctm->rgba_stride == 0) {
        return;
    }
    guint8 *row = ctm->rgba_buf;
    uint32_t stride = ctm->rgba_stride;
    for (uint32_t y = 0; y < height; ++y) {
        guint8 *pixel = row + (size_t)y * stride * 4u;
        for (uint32_t x = 0; x < width; ++x) {
            double r = (double)pixel[0] / 255.0;
            double g = (double)pixel[1] / 255.0;
            double b = (double)pixel[2] / 255.0;

            double nr = clamp_unit(ctm->matrix[0] * r + ctm->matrix[1] * g + ctm->matrix[2] * b);
            double ng = clamp_unit(ctm->matrix[3] * r + ctm->matrix[4] * g + ctm->matrix[5] * b);
            double nb = clamp_unit(ctm->matrix[6] * r + ctm->matrix[7] * g + ctm->matrix[8] * b);

            pixel[0] = clamp_byte_from_unit(nr);
            pixel[1] = clamp_byte_from_unit(ng);
            pixel[2] = clamp_byte_from_unit(nb);

            pixel += 4;
        }
    }
}
#endif

int video_ctm_prepare(VideoCtm *ctm, uint32_t width, uint32_t height, uint32_t hor_stride,
                      uint32_t ver_stride, uint32_t fourcc) {
    if (ctm == NULL || !ctm->enabled) {
        return 0;
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

    rga_buffer_t src = wrapbuffer_fd(src_fd, (int)width, (int)height, (int)hor_stride, (int)ver_stride,
                                     RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_fd(dst_fd, (int)width, (int)height, (int)hor_stride, (int)ver_stride,
                                     RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t tmp = wrapbuffer_virtualaddr(ctm->rgba_buf, (int)width, (int)height, (int)ctm->rgba_stride,
                                              (int)ctm->rgba_ver_stride, RK_FORMAT_RGBA_8888);

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
