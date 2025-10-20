#include "stabilizer.h"

#include "video_decoder.h"

#include <math.h>
#include <string.h>

#ifndef CLAMP
#define CLAMP(val, low, high) (((val) < (low)) ? (low) : (((val) > (high)) ? (high) : (val)))
#endif

typedef struct {
    guint64 pts;
    guint64 capture_ns;
    guint32 src_w;
    guint32 src_h;
    guint32 down_w;
    guint32 down_h;
    guint8 *downsampled;
} StabilizerSample;

struct Stabilizer {
    StabilizerConfig cfg;
    gboolean configured;
    gboolean running;
    gboolean stop_requested;
    gboolean zoom_armed;

    GMutex queue_lock;
    GCond queue_cond;
    GQueue queue;

    StabilizerSample *recycle;

    GThread *worker;
    struct VideoDecoder *decoder;

    gdouble filtered_x;
    gdouble filtered_y;
    gdouble integrated_x;
    gdouble integrated_y;

    gdouble max_shift_down_x;
    gdouble max_shift_down_y;
};

static void stabilizer_sample_free(StabilizerSample *sample) {
    if (sample == NULL) {
        return;
    }
    g_free(sample->downsampled);
    g_free(sample);
}

static StabilizerSample *stabilizer_sample_new(guint32 down_w, guint32 down_h) {
    StabilizerSample *sample = g_new0(StabilizerSample, 1);
    if (sample == NULL) {
        return NULL;
    }
    gsize count = (gsize)down_w * (gsize)down_h;
    sample->downsampled = g_new(guint8, count > 0 ? count : 1);
    if (sample->downsampled == NULL) {
        g_free(sample);
        return NULL;
    }
    sample->down_w = down_w;
    sample->down_h = down_h;
    return sample;
}

static void stabilizer_reset_history(Stabilizer *st) {
    g_mutex_lock(&st->queue_lock);
    while (!g_queue_is_empty(&st->queue)) {
        StabilizerSample *sample = g_queue_pop_head(&st->queue);
        stabilizer_sample_free(sample);
    }
    st->recycle = NULL;
    st->filtered_x = 0.0;
    st->filtered_y = 0.0;
    st->integrated_x = 0.0;
    st->integrated_y = 0.0;
    g_mutex_unlock(&st->queue_lock);
}

static inline guint32 clamp_downscale_dim(guint32 dim, guint32 fallback) {
    if (dim == 0) {
        return fallback;
    }
    if (dim < 4) {
        return 4;
    }
    if (dim > 512) {
        return 512;
    }
    return dim;
}

static inline guint32 clamp_search_radius(guint32 radius) {
    if (radius == 0) {
        return 4;
    }
    if (radius > 64) {
        return 64;
    }
    return radius;
}

static inline guint32 clamp_inset_percent(guint32 inset) {
    if (inset > 45u) {
        return 45u;
    }
    return inset;
}

static inline gdouble clamp_smoothing(gdouble value) {
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 0.99) {
        return 0.99;
    }
    return value;
}

static void downsample_average(const guint8 *src, guint32 stride, guint32 width, guint32 height,
                               guint32 out_w, guint32 out_h, guint8 *dst) {
    if (src == NULL || dst == NULL || out_w == 0 || out_h == 0 || width == 0 || height == 0) {
        return;
    }

    for (guint32 oy = 0; oy < out_h; ++oy) {
        guint32 sy0 = (guint64)oy * height / out_h;
        guint32 sy1 = (guint64)(oy + 1) * height / out_h;
        if (sy1 <= sy0) {
            sy1 = sy0 + 1;
        }
        if (sy1 > height) {
            sy1 = height;
        }
        for (guint32 ox = 0; ox < out_w; ++ox) {
            guint32 sx0 = (guint64)ox * width / out_w;
            guint32 sx1 = (guint64)(ox + 1) * width / out_w;
            if (sx1 <= sx0) {
                sx1 = sx0 + 1;
            }
            if (sx1 > width) {
                sx1 = width;
            }
            guint64 sum = 0;
            guint32 count = 0;
            for (guint32 sy = sy0; sy < sy1; ++sy) {
                const guint8 *row = src + sy * stride;
                for (guint32 sx = sx0; sx < sx1; ++sx) {
                    sum += row[sx];
                    ++count;
                }
            }
            if (count == 0) {
                dst[oy * out_w + ox] = 0;
            } else {
                dst[oy * out_w + ox] = (guint8)(sum / count);
            }
        }
    }
}

static guint64 compute_sad(const guint8 *a, const guint8 *b, guint32 width, guint32 height,
                           gint dx, gint dy) {
    guint32 x_start = dx < 0 ? (guint32)(-dx) : 0u;
    guint32 y_start = dy < 0 ? (guint32)(-dy) : 0u;
    guint32 x_end = dx > 0 ? width - (guint32)dx : width;
    guint32 y_end = dy > 0 ? height - (guint32)dy : height;

    if (x_end <= x_start || y_end <= y_start) {
        return G_MAXUINT64;
    }

    guint64 sad = 0;
    for (guint32 y = y_start; y < y_end; ++y) {
        const guint8 *row_a = a + y * width + x_start;
        const guint8 *row_b = b + (y + dy) * width + (x_start + dx);
        for (guint32 x = 0; x < x_end - x_start; ++x) {
            guint8 va = row_a[x];
            guint8 vb = row_b[x];
            sad += (guint64)ABS((gint)va - (gint)vb);
        }
    }

    return sad;
}

static gboolean estimate_motion(const StabilizerSample *prev, const StabilizerSample *cur,
                                guint32 radius, gint *dx_out, gint *dy_out, gdouble *quality_out) {
    if (prev == NULL || cur == NULL || prev->down_w != cur->down_w || prev->down_h != cur->down_h) {
        return FALSE;
    }

    const guint8 *prev_buf = prev->downsampled;
    const guint8 *cur_buf = cur->downsampled;
    guint32 width = cur->down_w;
    guint32 height = cur->down_h;

    guint64 best_cost = G_MAXUINT64;
    gint best_dx = 0;
    gint best_dy = 0;
    guint64 second_cost = G_MAXUINT64;

    for (gint dy = -(gint)radius; dy <= (gint)radius; ++dy) {
        for (gint dx = -(gint)radius; dx <= (gint)radius; ++dx) {
            guint64 cost = compute_sad(prev_buf, cur_buf, width, height, dx, dy);
            if (cost < best_cost) {
                second_cost = best_cost;
                best_cost = cost;
                best_dx = dx;
                best_dy = dy;
            } else if (cost < second_cost) {
                second_cost = cost;
            }
        }
    }

    if (best_cost == G_MAXUINT64) {
        return FALSE;
    }

    if (dx_out) {
        *dx_out = best_dx;
    }
    if (dy_out) {
        *dy_out = best_dy;
    }
    if (quality_out) {
        gdouble overlap = (gdouble)(width - (guint32)ABS(best_dx)) * (gdouble)(height - (guint32)ABS(best_dy));
        if (overlap <= 0.0) {
            *quality_out = 0.0;
        } else {
            gdouble norm = overlap * 255.0;
            gdouble ratio = best_cost / (norm > 0.0 ? norm : 1.0);
            gdouble separation = (second_cost > 0 && second_cost < G_MAXUINT64) ? (second_cost - best_cost) / (gdouble)second_cost : 0.0;
            gdouble quality = (1.0 - ratio) * 0.7 + CLAMP(separation, 0.0, 1.0) * 0.3;
            *quality_out = CLAMP(quality, 0.0, 1.0);
        }
    }
    return TRUE;
}

static StabilizerSample *stabilizer_pop_locked(Stabilizer *st) {
    StabilizerSample *sample = NULL;
    while (!st->stop_requested && g_queue_is_empty(&st->queue)) {
        g_cond_wait(&st->queue_cond, &st->queue_lock);
    }
    if (!g_queue_is_empty(&st->queue)) {
        sample = g_queue_pop_head(&st->queue);
    }
    return sample;
}

static gpointer stabilizer_worker_thread(gpointer user_data) {
    Stabilizer *st = (Stabilizer *)user_data;
    StabilizerSample *previous = NULL;

    while (TRUE) {
        g_mutex_lock(&st->queue_lock);
        StabilizerSample *current = stabilizer_pop_locked(st);
        gboolean should_stop = st->stop_requested;
        g_mutex_unlock(&st->queue_lock);

        if (current == NULL) {
            if (should_stop) {
                break;
            }
            continue;
        }

        if (previous == NULL) {
            previous = current;
            continue;
        }

        gint dx = 0;
        gint dy = 0;
        gdouble quality = 0.0;
        if (!estimate_motion(previous, current, st->cfg.search_radius, &dx, &dy, &quality)) {
            stabilizer_sample_free(previous);
            previous = current;
            continue;
        }

        /* Negative shift follows the intuitive "camera moved right" -> "slide crop left". */
        st->integrated_x = CLAMP(st->integrated_x - (gdouble)dx, -st->max_shift_down_x, st->max_shift_down_x);
        st->integrated_y = CLAMP(st->integrated_y - (gdouble)dy, -st->max_shift_down_y, st->max_shift_down_y);

        st->filtered_x = st->filtered_x * st->cfg.smoothing_factor +
                         st->integrated_x * (1.0 - st->cfg.smoothing_factor);
        st->filtered_y = st->filtered_y * st->cfg.smoothing_factor +
                         st->integrated_y * (1.0 - st->cfg.smoothing_factor);

        if (st->decoder != NULL) {
            VideoDecoderZoomRequest req = {0};
            guint32 inset = st->cfg.inset_percent;
            req.scale_x_percent = 100u - inset * 2u;
            req.scale_y_percent = 100u - inset * 2u;
            gdouble center_x = 50.0 + (st->filtered_x / (gdouble)current->down_w) * 100.0;
            gdouble center_y = 50.0 + (st->filtered_y / (gdouble)current->down_h) * 100.0;
            if (req.scale_x_percent < 10u) {
                req.scale_x_percent = 10u;
            }
            if (req.scale_y_percent < 10u) {
                req.scale_y_percent = 10u;
            }
            req.center_x_percent = (guint32)CLAMP(lround(center_x), 0, 100);
            req.center_y_percent = (guint32)CLAMP(lround(center_y), 0, 100);

            if (!st->zoom_armed) {
                video_decoder_set_zoom(st->decoder, TRUE, &req);
                st->zoom_armed = TRUE;
            } else {
                video_decoder_set_zoom(st->decoder, TRUE, &req);
            }
        }

        stabilizer_sample_free(previous);
        previous = current;
    }

    if (previous != NULL) {
        stabilizer_sample_free(previous);
    }

    return NULL;
}

Stabilizer *stabilizer_new(void) {
    Stabilizer *st = g_new0(Stabilizer, 1);
    if (st == NULL) {
        return NULL;
    }
    g_mutex_init(&st->queue_lock);
    g_cond_init(&st->queue_cond);
    g_queue_init(&st->queue);
    st->cfg.enable = FALSE;
    st->cfg.queue_depth = 3;
    st->cfg.downscale_width = 96;
    st->cfg.downscale_height = 54;
    st->cfg.search_radius = 6;
    st->cfg.inset_percent = 8;
    st->cfg.smoothing_factor = 0.4;
    return st;
}

void stabilizer_free(Stabilizer *st) {
    if (st == NULL) {
        return;
    }
    stabilizer_stop(st);
    g_mutex_lock(&st->queue_lock);
    while (!g_queue_is_empty(&st->queue)) {
        StabilizerSample *sample = g_queue_pop_head(&st->queue);
        stabilizer_sample_free(sample);
    }
    if (st->recycle != NULL) {
        stabilizer_sample_free(st->recycle);
        st->recycle = NULL;
    }
    g_mutex_unlock(&st->queue_lock);
    g_mutex_clear(&st->queue_lock);
    g_cond_clear(&st->queue_cond);
    g_free(st);
}

int stabilizer_configure(Stabilizer *st, const StabilizerConfig *cfg) {
    if (st == NULL || cfg == NULL) {
        return -1;
    }

    StabilizerConfig sanitized = *cfg;
    sanitized.queue_depth = cfg->queue_depth == 0 ? 3 : cfg->queue_depth;
    sanitized.downscale_width = clamp_downscale_dim(cfg->downscale_width, 96);
    sanitized.downscale_height = clamp_downscale_dim(cfg->downscale_height, 54);
    sanitized.search_radius = clamp_search_radius(cfg->search_radius);
    sanitized.inset_percent = clamp_inset_percent(cfg->inset_percent);
    sanitized.smoothing_factor = clamp_smoothing(cfg->smoothing_factor);

    st->cfg = sanitized;
    st->configured = TRUE;
    st->max_shift_down_x = (gdouble)sanitized.downscale_width * (gdouble)sanitized.inset_percent / 100.0;
    st->max_shift_down_y = (gdouble)sanitized.downscale_height * (gdouble)sanitized.inset_percent / 100.0;
    stabilizer_reset_history(st);
    return 0;
}

static void stabilizer_drain(Stabilizer *st) {
    g_mutex_lock(&st->queue_lock);
    while (!g_queue_is_empty(&st->queue)) {
        StabilizerSample *sample = g_queue_pop_head(&st->queue);
        stabilizer_sample_free(sample);
    }
    g_mutex_unlock(&st->queue_lock);
}

int stabilizer_start(Stabilizer *st, struct VideoDecoder *decoder) {
    if (st == NULL || decoder == NULL) {
        return -1;
    }
    if (!st->configured || !st->cfg.enable) {
        return 0;
    }

    st->decoder = decoder;
    st->stop_requested = FALSE;
    st->running = TRUE;
    st->zoom_armed = FALSE;

    if (st->worker != NULL) {
        g_thread_join(st->worker);
        st->worker = NULL;
    }

    st->worker = g_thread_new("stabilizer", stabilizer_worker_thread, st);
    if (st->worker == NULL) {
        st->running = FALSE;
        return -1;
    }
    return 0;
}

void stabilizer_stop(Stabilizer *st) {
    if (st == NULL) {
        return;
    }
    if (!st->running) {
        return;
    }

    g_mutex_lock(&st->queue_lock);
    st->stop_requested = TRUE;
    g_cond_broadcast(&st->queue_cond);
    g_mutex_unlock(&st->queue_lock);

    if (st->worker != NULL) {
        g_thread_join(st->worker);
        st->worker = NULL;
    }

    if (st->decoder != NULL && st->zoom_armed) {
        video_decoder_set_zoom(st->decoder, FALSE, NULL);
        st->zoom_armed = FALSE;
    }

    st->running = FALSE;
    st->decoder = NULL;
    stabilizer_drain(st);
}

gboolean stabilizer_submit_frame(Stabilizer *st, const StabilizerFrameDescriptor *frame) {
    if (st == NULL || frame == NULL || !st->configured || !st->cfg.enable) {
        return FALSE;
    }
    if (!st->running || st->decoder == NULL) {
        return FALSE;
    }
    if (frame->y_plane == NULL || frame->stride == 0 || frame->width == 0 || frame->height == 0) {
        return FALSE;
    }

    StabilizerSample *sample = stabilizer_sample_new(st->cfg.downscale_width, st->cfg.downscale_height);
    if (sample == NULL) {
        return FALSE;
    }

    sample->pts = frame->pts;
    sample->capture_ns = frame->capture_ns;
    sample->src_w = frame->width;
    sample->src_h = frame->height;

    downsample_average(frame->y_plane, frame->stride, frame->width, frame->height,
                       sample->down_w, sample->down_h, sample->downsampled);

    g_mutex_lock(&st->queue_lock);
    if (st->cfg.queue_depth > 0) {
        while ((guint)g_queue_get_length(&st->queue) >= st->cfg.queue_depth) {
            StabilizerSample *oldest = g_queue_pop_head(&st->queue);
            stabilizer_sample_free(oldest);
        }
    }
    g_queue_push_tail(&st->queue, sample);
    g_cond_signal(&st->queue_cond);
    g_mutex_unlock(&st->queue_lock);
    return TRUE;
}
