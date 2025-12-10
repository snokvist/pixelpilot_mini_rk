#include "video_ctm.h"

#include "logging.h"

#include <errno.h>
#include <math.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_mode.h>
#include <drm_fourcc.h>

static void video_ctm_metrics_clear(VideoCtm *ctm) {
    if (ctm == NULL) {
        return;
    }
    memset(&ctm->metrics, 0, sizeof(ctm->metrics));
}

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
    video_ctm_metrics_clear(ctm);
    video_ctm_set_identity(ctm);
    ctm->hw_supported = FALSE;
    ctm->hw_applied = FALSE;
    ctm->hw_fd = -1;
    ctm->hw_object_id = 0;
    ctm->hw_object_type = 0;
    ctm->hw_prop_id = 0;
    ctm->hw_blob_id = 0;
    ctm->render_fd = -1;

    gboolean config_enable = FALSE;
    if (cfg != NULL) {
        config_enable = cfg->video_ctm.enable != 0;
        for (int i = 0; i < 9; ++i) {
            ctm->matrix[i] = cfg->video_ctm.matrix[i];
        }
    }
    ctm->enabled = config_enable;
}

void video_ctm_reset(VideoCtm *ctm) {
    if (ctm == NULL) {
        return;
    }
    video_ctm_disable_drm(ctm);
    video_ctm_metrics_clear(ctm);
    ctm->src_fourcc = 0;
    ctm->dst_fourcc = 0;
    ctm->dst_pitch = 0;
}

void video_ctm_set_render_fd(VideoCtm *ctm, int drm_fd) {
    if (ctm == NULL) {
        return;
    }
    ctm->render_fd = drm_fd;
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
    if (ctm->render_fd < 0) {
        ctm->render_fd = drm_fd;
    }
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

int video_ctm_prepare(VideoCtm *ctm, uint32_t width, uint32_t height, uint32_t hor_stride, uint32_t ver_stride,
                      uint32_t fourcc, uint32_t dst_pitch, uint32_t dst_fourcc) {
    (void)width;
    (void)height;
    (void)hor_stride;
    (void)ver_stride;
    (void)dst_pitch;
    (void)dst_fourcc;

    if (ctm == NULL || !ctm->enabled) {
        video_ctm_disable_drm(ctm);
        return 0;
    }

    ctm->src_fourcc = fourcc;
    ctm->dst_fourcc = DRM_FORMAT_NV12;
    ctm->dst_pitch = 0;

    if (video_ctm_hw_available(ctm)) {
        if (video_ctm_apply_hw(ctm)) {
            return 0;
        }
        LOGW("Video CTM: failed to apply DRM CTM property; disabling transform");
        video_ctm_disable_drm(ctm);
        ctm->hw_supported = FALSE;
    }

    LOGW("Video CTM: no hardware CTM support; disabling transform");
    ctm->enabled = FALSE;
    return -1;
}

int video_ctm_process(VideoCtm *ctm, int src_fd, int dst_fd, uint32_t width, uint32_t height, uint32_t hor_stride,
                      uint32_t ver_stride, uint32_t fourcc, uint32_t dst_pitch, uint32_t dst_fourcc) {
    (void)src_fd;
    (void)dst_fd;
    (void)width;
    (void)height;
    (void)hor_stride;
    (void)ver_stride;
    (void)fourcc;
    (void)dst_pitch;
    (void)dst_fourcc;

    if (ctm == NULL || !ctm->enabled) {
        return -1;
    }
    if (video_ctm_hw_available(ctm)) {
        ctm->metrics.frame_count = video_ctm_hw_available(ctm) ? ctm->metrics.frame_count + 1 : ctm->metrics.frame_count;
        return ctm->hw_applied ? 0 : -1;
    }
    return -1;
}

void video_ctm_apply_update(VideoCtm *ctm, const VideoCtmUpdate *update) {
    if (ctm == NULL || update == NULL || update->fields == 0) {
        return;
    }
    if ((update->fields & VIDEO_CTM_UPDATE_MATRIX) != 0u) {
        for (int i = 0; i < 9; ++i) {
            ctm->matrix[i] = update->matrix[i];
        }
        if (ctm->hw_applied) {
            video_ctm_destroy_blob(ctm);
            ctm->hw_applied = FALSE;
        }
    }

}

void video_ctm_get_metrics(const VideoCtm *ctm, VideoCtmMetrics *out_metrics) {
    if (out_metrics == NULL) {
        return;
    }
    memset(out_metrics, 0, sizeof(*out_metrics));
    if (ctm == NULL) {
        return;
    }
    out_metrics->frame_count = ctm->metrics.frame_count;
}
