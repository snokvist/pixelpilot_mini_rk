#ifndef VIDEO_CTM_H
#define VIDEO_CTM_H

#include <glib.h>
#include <stdint.h>

#include "config.h"

#if defined(HAVE_LIBRGA) && defined(HAVE_GBM_GLES2)
#include <im2d.h>
struct VideoCtmGpuState;
#endif

typedef struct VideoCtm {
    gboolean enabled;
    double matrix[9];
    double sharpness;
    double gamma_value;
    double gamma_lift;
    double gamma_gain;
    double gamma_r_mult;
    double gamma_g_mult;
    double gamma_b_mult;
    VideoCtmBackend backend;
    gboolean hw_supported;
    gboolean hw_applied;
    int hw_fd;
    uint32_t hw_object_id;
    uint32_t hw_object_type;
    uint32_t hw_prop_id;
    uint32_t hw_blob_id;
    int render_fd;
#if defined(HAVE_LIBRGA) && defined(HAVE_GBM_GLES2)
    struct VideoCtmGpuState *gpu_state;
#endif
} VideoCtm;

void video_ctm_init(VideoCtm *ctm, const AppCfg *cfg);
void video_ctm_reset(VideoCtm *ctm);
void video_ctm_set_render_fd(VideoCtm *ctm, int drm_fd);
void video_ctm_use_drm_property(VideoCtm *ctm, int drm_fd, uint32_t object_id,
                                uint32_t object_type, uint32_t prop_id);
void video_ctm_disable_drm(VideoCtm *ctm);
void video_ctm_apply_config(VideoCtm *ctm, const VideoCtmCfg *cfg);
int video_ctm_prepare(VideoCtm *ctm, uint32_t width, uint32_t height, uint32_t hor_stride,
                      uint32_t ver_stride, uint32_t fourcc);
int video_ctm_process(VideoCtm *ctm, int src_fd, int dst_fd, uint32_t width, uint32_t height,
                      uint32_t hor_stride, uint32_t ver_stride, uint32_t fourcc);

#endif // VIDEO_CTM_H
