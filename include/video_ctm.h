#ifndef VIDEO_CTM_H
#define VIDEO_CTM_H

#include <glib.h>
#include <stdint.h>

#include "config.h"

typedef struct VideoCtm {
    gboolean enabled;
    double matrix[9];
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
    struct {
        guint64 frame_count;
    } metrics;
} VideoCtm;

#define VIDEO_CTM_UPDATE_MATRIX (1u << 0)

typedef struct VideoCtmUpdate {
    uint32_t fields;
    double matrix[9];
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
} VideoCtmMetrics;

void video_ctm_get_metrics(const VideoCtm *ctm, VideoCtmMetrics *out_metrics);

#endif // VIDEO_CTM_H
