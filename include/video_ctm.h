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
    uint32_t src_fourcc;
    uint32_t dst_fourcc;
    uint32_t dst_pitch;
#if defined(HAVE_LIBRGA) && defined(HAVE_GBM_GLES2)
    struct VideoCtmGpuState *gpu_state;
#endif
    struct {
        guint64 frame_count;
        guint64 last_gpu_issue_ns;
        guint64 last_gpu_wait_ns;
        guint64 last_gpu_total_ns;
        guint64 last_convert_ns;
        guint64 last_frame_ns;
        guint64 sum_gpu_issue_ns;
        guint64 sum_gpu_wait_ns;
        guint64 sum_gpu_total_ns;
        guint64 sum_convert_ns;
        guint64 sum_frame_ns;
        guint64 max_gpu_issue_ns;
        guint64 max_gpu_wait_ns;
        guint64 max_gpu_total_ns;
        guint64 max_convert_ns;
        guint64 max_frame_ns;
        guint64 pending_gpu_issue_ns;
        guint64 pending_gpu_wait_ns;
        guint64 pending_gpu_total_ns;
        gboolean pending_gpu_valid;
    } metrics;
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
int video_ctm_prepare(VideoCtm *ctm, uint32_t width, uint32_t height, uint32_t src_hor_stride,
                      uint32_t src_ver_stride, uint32_t src_fourcc, uint32_t dst_pitch,
                      uint32_t dst_fourcc);
int video_ctm_process(VideoCtm *ctm, int src_fd, int dst_fd, uint32_t width, uint32_t height,
                      uint32_t src_hor_stride, uint32_t src_ver_stride, uint32_t src_fourcc,
                      uint32_t dst_pitch, uint32_t dst_fourcc);
void video_ctm_apply_update(VideoCtm *ctm, const VideoCtmUpdate *update);

typedef struct VideoCtmMetrics {
    guint64 frame_count;
    double last_gpu_issue_ms;
    double last_gpu_wait_ms;
    double last_gpu_total_ms;
    double last_convert_ms;
    double last_frame_ms;
    double avg_gpu_issue_ms;
    double avg_gpu_wait_ms;
    double avg_gpu_total_ms;
    double avg_convert_ms;
    double avg_frame_ms;
    double max_gpu_issue_ms;
    double max_gpu_wait_ms;
    double max_gpu_total_ms;
    double max_convert_ms;
    double max_frame_ms;
} VideoCtmMetrics;

void video_ctm_get_metrics(const VideoCtm *ctm, VideoCtmMetrics *out_metrics);

#endif // VIDEO_CTM_H
