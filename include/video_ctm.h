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
    gboolean flip;
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

#define VIDEO_CTM_UPDATE_MATRIX (1u << 0)
#define VIDEO_CTM_UPDATE_SHARPNESS (1u << 1)
#define VIDEO_CTM_UPDATE_GAMMA (1u << 2)
#define VIDEO_CTM_UPDATE_GAMMA_LIFT (1u << 3)
#define VIDEO_CTM_UPDATE_GAMMA_GAIN (1u << 4)
#define VIDEO_CTM_UPDATE_GAMMA_R_MULT (1u << 5)
#define VIDEO_CTM_UPDATE_GAMMA_G_MULT (1u << 6)
#define VIDEO_CTM_UPDATE_GAMMA_B_MULT (1u << 7)
#define VIDEO_CTM_UPDATE_FLIP (1u << 8)

typedef struct VideoCtmUpdate {
    uint32_t fields;
    double matrix[9];
    double sharpness;
    double gamma_value;
    double gamma_lift;
    double gamma_gain;
    double gamma_r_mult;
    double gamma_g_mult;
    double gamma_b_mult;
    gboolean flip;
} VideoCtmUpdate;

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
void video_ctm_apply_update(VideoCtm *ctm, const VideoCtmUpdate *update);

#endif // VIDEO_CTM_H
