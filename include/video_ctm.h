#ifndef VIDEO_CTM_H
#define VIDEO_CTM_H

#include <glib.h>
#include <stdint.h>

#include "config.h"

#if defined(HAVE_LIBRGA)
#include <im2d.h>
#endif

typedef struct VideoCtm {
    gboolean enabled;
    double matrix[9];
#if defined(HAVE_LIBRGA)
    guint8 *rgba_buf;
    size_t rgba_buf_size;
    uint32_t rgba_width;
    uint32_t rgba_height;
    uint32_t rgba_stride;
    uint32_t rgba_ver_stride;
    gboolean lut_ready;
    int32_t lut[3][3][256];
#endif
} VideoCtm;

void video_ctm_init(VideoCtm *ctm, const AppCfg *cfg);
void video_ctm_reset(VideoCtm *ctm);
int video_ctm_prepare(VideoCtm *ctm, uint32_t width, uint32_t height, uint32_t hor_stride,
                      uint32_t ver_stride, uint32_t fourcc);
int video_ctm_process(VideoCtm *ctm, int src_fd, int dst_fd, uint32_t width, uint32_t height,
                      uint32_t hor_stride, uint32_t ver_stride, uint32_t fourcc);

#endif // VIDEO_CTM_H
