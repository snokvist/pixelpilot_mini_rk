#include "video_motion_estimator.h"

#include "logging.h"

#include <glib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct VideoMotionEstimator {
    MotionEstimatorConfig config;
    gboolean initialized;
    gboolean configured;
    gboolean have_previous;

    uint32_t width;
    uint32_t height;
    uint32_t hor_stride;
    uint32_t ver_stride;

    int sample_width;
    int sample_height;

    guint8 *prev_sample;
    guint8 *curr_sample;

    float filtered_x;
    float filtered_y;
    guint64 frames_total;
    guint64 frames_valid;

    gboolean diag_logged_disabled;
    gboolean diag_logged_unconfigured;
    gboolean diag_logged_no_prev;
};

static void motion_estimator_apply_defaults(MotionEstimatorConfig *cfg) {
    if (cfg == NULL) {
        return;
    }
    if (cfg->search_radius_px <= 0) {
        cfg->search_radius_px = 24;
    }
    if (cfg->downsample_factor <= 0) {
        cfg->downsample_factor = 4;
    }
    if (cfg->downsample_factor > 16) {
        cfg->downsample_factor = 16;
    }
    if (!isfinite(cfg->smoothing_factor) || cfg->smoothing_factor < 0.0f) {
        cfg->smoothing_factor = 0.5f;
    }
    if (cfg->smoothing_factor > 0.98f) {
        cfg->smoothing_factor = 0.98f;
    }
    cfg->enable = cfg->enable ? 1 : 0;
    cfg->diagnostics = cfg->diagnostics ? 1 : 0;
}

VideoMotionEstimator *motion_estimator_new(void) {
    return g_new0(VideoMotionEstimator, 1);
}

void motion_estimator_free(VideoMotionEstimator *estimator) {
    if (estimator == NULL) {
        return;
    }
    motion_estimator_reset(estimator);
    g_free(estimator);
}

int motion_estimator_init(VideoMotionEstimator *estimator, const MotionEstimatorConfig *config) {
    if (estimator == NULL) {
        return -1;
    }
    memset(estimator, 0, sizeof(*estimator));
    MotionEstimatorConfig cfg = {0};
    if (config != NULL) {
        cfg = *config;
    }
    motion_estimator_apply_defaults(&cfg);
    estimator->config = cfg;
    estimator->initialized = TRUE;
    return 0;
}

int motion_estimator_update(VideoMotionEstimator *estimator, const MotionEstimatorConfig *config) {
    if (estimator == NULL) {
        return -1;
    }
    if (!estimator->initialized) {
        return motion_estimator_init(estimator, config);
    }
    MotionEstimatorConfig cfg = estimator->config;
    if (config != NULL) {
        cfg = *config;
    }
    motion_estimator_apply_defaults(&cfg);
    estimator->config = cfg;
    estimator->diag_logged_disabled = FALSE;
    estimator->diag_logged_unconfigured = FALSE;
    estimator->diag_logged_no_prev = FALSE;
    return 0;
}

int motion_estimator_reset(VideoMotionEstimator *estimator) {
    if (estimator == NULL) {
        return -1;
    }
    if (estimator->prev_sample) {
        g_free(estimator->prev_sample);
        estimator->prev_sample = NULL;
    }
    if (estimator->curr_sample) {
        g_free(estimator->curr_sample);
        estimator->curr_sample = NULL;
    }
    estimator->configured = FALSE;
    estimator->have_previous = FALSE;
    estimator->frames_total = 0;
    estimator->frames_valid = 0;
    estimator->filtered_x = 0.0f;
    estimator->filtered_y = 0.0f;
    return 0;
}

int motion_estimator_configure(VideoMotionEstimator *estimator, uint32_t width, uint32_t height,
                               uint32_t hor_stride, uint32_t ver_stride) {
    if (estimator == NULL || !estimator->initialized) {
        return -1;
    }
    motion_estimator_reset(estimator);

    if (width == 0 || height == 0 || hor_stride == 0 || ver_stride == 0) {
        return -1;
    }
    if (hor_stride < width || ver_stride < height) {
        return -1;
    }

    estimator->width = width;
    estimator->height = height;
    estimator->hor_stride = hor_stride;
    estimator->ver_stride = ver_stride;

    int factor = estimator->config.downsample_factor;
    if (factor <= 0) {
        factor = 4;
    }
    int sample_w = (int)(width / (uint32_t)factor);
    int sample_h = (int)(height / (uint32_t)factor);
    if (sample_w < 8) {
        sample_w = width >= 8 ? 8 : (int)width;
    }
    if (sample_h < 8) {
        sample_h = height >= 8 ? 8 : (int)height;
    }
    estimator->sample_width = sample_w;
    estimator->sample_height = sample_h;

    size_t sample_count = (size_t)sample_w * (size_t)sample_h;
    estimator->prev_sample = g_malloc0(sample_count);
    estimator->curr_sample = g_malloc0(sample_count);
    if (estimator->prev_sample == NULL || estimator->curr_sample == NULL) {
        motion_estimator_reset(estimator);
        return -1;
    }

    estimator->configured = TRUE;
    return 0;
}

static void downsample_average(const uint8_t *src, uint32_t width, uint32_t height, uint32_t stride, int factor,
                               uint8_t *dst, int dst_w, int dst_h) {
    if (factor <= 1) {
        for (int y = 0; y < dst_h && (uint32_t)y < height; ++y) {
            const uint8_t *src_row = src + (size_t)y * stride;
            memcpy(dst + (size_t)y * dst_w, src_row, MIN((size_t)dst_w, (size_t)width));
        }
        return;
    }

    for (int y = 0; y < dst_h; ++y) {
        int src_y0 = y * factor;
        if ((uint32_t)src_y0 >= height) {
            break;
        }
        uint8_t *dst_row = dst + (size_t)y * dst_w;
        for (int x = 0; x < dst_w; ++x) {
            int src_x0 = x * factor;
            if ((uint32_t)src_x0 >= width) {
                break;
            }
            int max_y = MIN(height, (uint32_t)(src_y0 + factor));
            int max_x = MIN(width, (uint32_t)(src_x0 + factor));
            int count = 0;
            int sum = 0;
            for (int yy = src_y0; yy < max_y; ++yy) {
                const uint8_t *row = src + (size_t)yy * stride;
                for (int xx = src_x0; xx < max_x; ++xx) {
                    sum += row[xx];
                    ++count;
                }
            }
            if (count <= 0) {
                dst_row[x] = 0;
            } else {
                dst_row[x] = (uint8_t)(sum / count);
            }
        }
    }
}

static guint64 compute_sad(const guint8 *a, const guint8 *b, int width, int height, int stride_a, int stride_b) {
    guint64 sad = 0;
    for (int y = 0; y < height; ++y) {
        const guint8 *row_a = a + (size_t)y * stride_a;
        const guint8 *row_b = b + (size_t)y * stride_b;
        for (int x = 0; x < width; ++x) {
            sad += (guint64)ABS(row_a[x] - row_b[x]);
        }
    }
    return sad;
}

int motion_estimator_analyse(VideoMotionEstimator *estimator, const uint8_t *nv12_base, size_t nv12_size,
                             MotionEstimate *estimate_out) {
    if (estimate_out) {
        memset(estimate_out, 0, sizeof(*estimate_out));
    }
    if (estimator == NULL || !estimator->initialized) {
        return -1;
    }
    if (!estimator->config.enable) {
        if (estimator->config.diagnostics && !estimator->diag_logged_disabled) {
            LOGI("Motion estimator bypassed (disabled)");
            estimator->diag_logged_disabled = TRUE;
        }
        return 1;
    }
    if (!estimator->configured) {
        if (estimator->config.diagnostics && !estimator->diag_logged_unconfigured) {
            LOGI("Motion estimator awaiting configuration before analysing frames");
            estimator->diag_logged_unconfigured = TRUE;
        }
        return 1;
    }
    if (nv12_base == NULL || nv12_size == 0) {
        return -1;
    }

    estimator->frames_total++;

    size_t required = (size_t)estimator->hor_stride * estimator->ver_stride;
    if (nv12_size < required) {
        return -1;
    }

    uint32_t width = estimator->width;
    uint32_t height = estimator->height;
    int factor = estimator->config.downsample_factor <= 0 ? 4 : estimator->config.downsample_factor;
    if (!estimator->curr_sample || !estimator->prev_sample) {
        return -1;
    }

    downsample_average(nv12_base, width, height, estimator->hor_stride, factor, estimator->curr_sample,
                       estimator->sample_width, estimator->sample_height);

    if (!estimator->have_previous) {
        memcpy(estimator->prev_sample, estimator->curr_sample,
               (size_t)estimator->sample_width * (size_t)estimator->sample_height);
        estimator->have_previous = TRUE;
        if (estimator->config.diagnostics && !estimator->diag_logged_no_prev) {
            LOGI("Motion estimator primed with first frame");
            estimator->diag_logged_no_prev = TRUE;
        }
        return 1;
    }

    int radius_px = estimator->config.search_radius_px;
    if (radius_px <= 0) {
        radius_px = 1;
    }
    int radius = radius_px / factor;
    if (radius < 1) {
        radius = 1;
    }
    if (radius > estimator->sample_width - 1) {
        radius = estimator->sample_width - 1;
    }
    if (radius > estimator->sample_height - 1) {
        radius = estimator->sample_height - 1;
    }

    guint64 best_cost = G_MAXUINT64;
    int best_dx = 0;
    int best_dy = 0;
    int best_overlap_w = 0;
    int best_overlap_h = 0;

    for (int dy = -radius; dy <= radius; ++dy) {
        int y0 = dy >= 0 ? dy : 0;
        int y1 = dy >= 0 ? estimator->sample_height : estimator->sample_height + dy;
        if (y1 - y0 <= 4) {
            continue;
        }
        for (int dx = -radius; dx <= radius; ++dx) {
            int x0 = dx >= 0 ? dx : 0;
            int x1 = dx >= 0 ? estimator->sample_width : estimator->sample_width + dx;
            int overlap_w = x1 - x0;
            int overlap_h = y1 - y0;
            if (overlap_w <= 4 || overlap_h <= 4) {
                continue;
            }
            const guint8 *curr = estimator->curr_sample + (size_t)y0 * estimator->sample_width + x0;
            const guint8 *prev = estimator->prev_sample + (size_t)(y0 - dy) * estimator->sample_width + (x0 - dx);
            guint64 cost = compute_sad(curr, prev, overlap_w, overlap_h, estimator->sample_width, estimator->sample_width);
            if (cost < best_cost) {
                best_cost = cost;
                best_dx = dx;
                best_dy = dy;
                best_overlap_w = overlap_w;
                best_overlap_h = overlap_h;
            }
        }
    }

    memcpy(estimator->prev_sample, estimator->curr_sample,
           (size_t)estimator->sample_width * (size_t)estimator->sample_height);

    if (best_overlap_w == 0 || best_overlap_h == 0 || best_cost == G_MAXUINT64) {
        return 1;
    }

    estimator->frames_valid++;

    float raw_tx = (float)(-best_dx * factor);
    float raw_ty = (float)(-best_dy * factor);

    float alpha = estimator->config.smoothing_factor;
    if (alpha < 0.0f) {
        alpha = 0.0f;
    } else if (alpha > 0.98f) {
        alpha = 0.98f;
    }

    if (estimator->frames_valid <= 1) {
        estimator->filtered_x = raw_tx;
        estimator->filtered_y = raw_ty;
    } else {
        estimator->filtered_x = alpha * estimator->filtered_x + (1.0f - alpha) * raw_tx;
        estimator->filtered_y = alpha * estimator->filtered_y + (1.0f - alpha) * raw_ty;
    }

    if (estimate_out) {
        estimate_out->valid = TRUE;
        estimate_out->translate_x = estimator->filtered_x;
        estimate_out->translate_y = estimator->filtered_y;
        guint64 max_cost = (guint64)best_overlap_w * (guint64)best_overlap_h * 255ull;
        if (max_cost == 0) {
            estimate_out->confidence = 0.0f;
        } else {
            float confidence = 1.0f - ((float)best_cost / ((float)max_cost + 1.0f));
            if (confidence < 0.0f) {
                confidence = 0.0f;
            }
            if (confidence > 1.0f) {
                confidence = 1.0f;
            }
            estimate_out->confidence = confidence;
        }
    }

    return 0;
}
