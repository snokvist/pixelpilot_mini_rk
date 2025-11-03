#ifndef VIDEO_CTM_H
#define VIDEO_CTM_H

#include <glib.h>
#include <stdint.h>

#include "config.h"

#if defined(HAVE_LIBRGA)
#include <im2d.h>
#endif

struct VideoCtmGpuState;

typedef struct VideoCtm {
    gboolean enabled;
    double matrix[9];
    VideoCtmBackend backend;
    gboolean hw_supported;
    gboolean hw_applied;
    int hw_fd;
    uint32_t hw_object_id;
    uint32_t hw_object_type;
    uint32_t hw_prop_id;
    uint32_t hw_blob_id;
    int render_fd;
#if defined(HAVE_LIBRGA)
    gboolean gpu_active;
    gboolean gpu_forced_off;
    struct VideoCtmGpuState *gpu_state;
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
void video_ctm_set_render_fd(VideoCtm *ctm, int drm_fd);
void video_ctm_use_drm_property(VideoCtm *ctm, int drm_fd, uint32_t object_id,
                                uint32_t object_type, uint32_t prop_id);
void video_ctm_disable_drm(VideoCtm *ctm);
int video_ctm_prepare(VideoCtm *ctm, uint32_t width, uint32_t height, uint32_t hor_stride,
                      uint32_t ver_stride, uint32_t fourcc);
int video_ctm_process(VideoCtm *ctm, int src_fd, int dst_fd, uint32_t width, uint32_t height,
                      uint32_t hor_stride, uint32_t ver_stride, uint32_t fourcc);

#endif // VIDEO_CTM_H
