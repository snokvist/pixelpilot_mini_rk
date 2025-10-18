#ifndef VIDEO_MOTION_ESTIMATOR_H
#define VIDEO_MOTION_ESTIMATOR_H

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int enable;
    int diagnostics;
    int search_radius_px;
    int downsample_factor;
    int max_sample_width_px;
    int max_sample_height_px;
    float smoothing_factor;
} MotionEstimatorConfig;

typedef struct {
    gboolean valid;
    float translate_x;
    float translate_y;
    float confidence;
} MotionEstimate;

typedef struct VideoMotionEstimator VideoMotionEstimator;

VideoMotionEstimator *motion_estimator_new(void);
void motion_estimator_free(VideoMotionEstimator *estimator);

int motion_estimator_init(VideoMotionEstimator *estimator, const MotionEstimatorConfig *config);
int motion_estimator_update(VideoMotionEstimator *estimator, const MotionEstimatorConfig *config);
int motion_estimator_configure(VideoMotionEstimator *estimator, uint32_t width, uint32_t height,
                               uint32_t hor_stride, uint32_t ver_stride);
int motion_estimator_reset(VideoMotionEstimator *estimator);
int motion_estimator_analyse(VideoMotionEstimator *estimator, const uint8_t *nv12_base, size_t nv12_size,
                             MotionEstimate *estimate_out);

#endif /* VIDEO_MOTION_ESTIMATOR_H */
