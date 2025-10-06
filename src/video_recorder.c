#include "video_recorder.h"

#include "logging.h"

#include <errno.h>
#include <glib.h>
#include <gst/app/gstappsink.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "minimp4.h"

struct PendingSample {
    guint8 *data;
    size_t size;
    GstClockTime pts;
    GstClockTime duration;
    gboolean valid;
};

struct VideoRecorder {
    gboolean enabled;
    gboolean failed;
    FILE *fp;
    MP4E_mux_t *mux;
    mp4_h26x_writer_t writer;
    gboolean writer_initialized;
    guint width;
    guint height;
    guint64 default_duration_90k;
    guint64 last_duration_90k;
    struct PendingSample pending;
};

static int recorder_write_callback(int64_t offset, const void *buffer, size_t size, void *token) {
    VideoRecorder *rec = (VideoRecorder *)token;
    if (rec == NULL || rec->fp == NULL || buffer == NULL) {
        return -1;
    }
    if (fseeko(rec->fp, offset, SEEK_SET) != 0) {
        LOGE("minimp4: fseeko failed at offset %" G_GINT64_FORMAT ": %s", offset, g_strerror(errno));
        return -1;
    }
    size_t written = fwrite(buffer, 1, size, rec->fp);
    if (written != size) {
        LOGE("minimp4: fwrite short write (%zu/%zu): %s", written, size, g_strerror(errno));
        return -1;
    }
    return 0;
}

static guint64 gst_time_to_90k(GstClockTime value) {
    if (!GST_CLOCK_TIME_IS_VALID(value)) {
        return 0;
    }
    return gst_util_uint64_scale(value, 90000, GST_SECOND);
}

static void pending_reset(struct PendingSample *pending) {
    if (pending == NULL) {
        return;
    }
    if (pending->data != NULL) {
        g_free(pending->data);
    }
    memset(pending, 0, sizeof(*pending));
}

static gboolean ensure_writer_initialized(VideoRecorder *rec, GstSample *sample) {
    if (rec->writer_initialized || rec->mux == NULL) {
        return rec->writer_initialized;
    }

    guint width = 0;
    guint height = 0;
    guint default_fps_n = 0;
    guint default_fps_d = 1;

    if (sample != NULL) {
        GstCaps *caps = gst_sample_get_caps(sample);
        if (caps != NULL) {
            GstStructure *s = gst_caps_get_structure(caps, 0);
            if (s != NULL) {
                gint tmp = 0;
                if (gst_structure_get_int(s, "width", &tmp) && tmp > 0) {
                    width = (guint)tmp;
                }
                if (gst_structure_get_int(s, "height", &tmp) && tmp > 0) {
                    height = (guint)tmp;
                }
                gint fps_n = 0;
                gint fps_d = 1;
                if (gst_structure_get_fraction(s, "framerate", &fps_n, &fps_d) && fps_n > 0 && fps_d > 0) {
                    default_fps_n = (guint)fps_n;
                    default_fps_d = (guint)fps_d;
                }
            }
        }
    }

    if (default_fps_n == 0 || default_fps_d == 0) {
        default_fps_n = 30;
        default_fps_d = 1;
    }

    rec->default_duration_90k = (guint64)90000 * default_fps_d / default_fps_n;
    if (rec->default_duration_90k == 0) {
        rec->default_duration_90k = 3000;
    }

    if (mp4_h26x_write_init(&rec->writer, rec->mux, width, height, 1) != MP4E_STATUS_OK) {
        LOGE("minimp4: failed to initialise H.265 writer");
        rec->failed = TRUE;
        return FALSE;
    }

    rec->writer_initialized = TRUE;
    rec->width = width;
    rec->height = height;
    return TRUE;
}

static guint32 compute_duration_90k(VideoRecorder *rec, GstClockTime prev_pts, GstClockTime prev_duration, GstClockTime next_pts) {
    guint64 duration = 0;
    if (GST_CLOCK_TIME_IS_VALID(prev_duration)) {
        duration = gst_time_to_90k(prev_duration);
    } else if (GST_CLOCK_TIME_IS_VALID(prev_pts) && GST_CLOCK_TIME_IS_VALID(next_pts) && next_pts > prev_pts) {
        duration = gst_time_to_90k(next_pts - prev_pts);
    }

    if (duration == 0) {
        if (rec->last_duration_90k != 0) {
            duration = rec->last_duration_90k;
        } else if (rec->default_duration_90k != 0) {
            duration = rec->default_duration_90k;
        } else {
            duration = 3000;
        }
    }

    rec->last_duration_90k = duration;
    if (duration > G_MAXUINT32) {
        duration = G_MAXUINT32;
    }
    return (guint32)duration;
}

static void emit_pending(VideoRecorder *rec, guint32 duration_90k) {
    if (!rec->pending.valid) {
        return;
    }

    if (rec->failed || !rec->writer_initialized || rec->mux == NULL) {
        pending_reset(&rec->pending);
        return;
    }

    if (rec->pending.size == 0 || rec->pending.data == NULL) {
        pending_reset(&rec->pending);
        return;
    }

    if (rec->pending.size > (size_t)INT_MAX) {
        LOGW("minimp4: dropping oversized access unit (%zu bytes)", rec->pending.size);
        pending_reset(&rec->pending);
        return;
    }

    int err = mp4_h26x_write_nal(&rec->writer, rec->pending.data, (int)rec->pending.size, duration_90k);
    if (err != MP4E_STATUS_OK) {
        LOGE("minimp4: failed to write access unit (err=%d)", err);
        rec->failed = TRUE;
    }

    pending_reset(&rec->pending);
}

VideoRecorder *video_recorder_new(const RecordCfg *cfg) {
    if (cfg == NULL || !cfg->enable || cfg->output_path[0] == '\0') {
        return NULL;
    }

    VideoRecorder *rec = g_new0(VideoRecorder, 1);
    if (rec == NULL) {
        return NULL;
    }

    rec->enabled = TRUE;
    rec->failed = FALSE;
    rec->default_duration_90k = 3000;
    rec->last_duration_90k = 0;

    rec->fp = fopen(cfg->output_path, "wb");
    if (rec->fp == NULL) {
        LOGE("record: failed to open %s: %s", cfg->output_path, g_strerror(errno));
        g_free(rec);
        return NULL;
    }

    rec->mux = MP4E_open(1, 0, rec, recorder_write_callback);
    if (rec->mux == NULL) {
        LOGE("minimp4: failed to allocate muxer");
        fclose(rec->fp);
        rec->fp = NULL;
        g_free(rec);
        return NULL;
    }

    return rec;
}

void video_recorder_handle_sample(VideoRecorder *rec, GstSample *sample, const guint8 *data, size_t size) {
    if (rec == NULL || !rec->enabled || rec->failed || data == NULL || size == 0) {
        return;
    }

    if (!ensure_writer_initialized(rec, sample)) {
        return;
    }

    guint8 *copy = (guint8 *)g_malloc(size);
    if (copy == NULL) {
        LOGE("record: failed to allocate buffer for copy (%zu bytes)", size);
        return;
    }
    memcpy(copy, data, size);

    GstClockTime pts = GST_CLOCK_TIME_NONE;
    GstClockTime duration = GST_CLOCK_TIME_NONE;
    GstBuffer *buffer = sample != NULL ? gst_sample_get_buffer(sample) : NULL;
    if (buffer != NULL) {
        pts = GST_BUFFER_PTS(buffer);
        if (!GST_CLOCK_TIME_IS_VALID(pts)) {
            pts = GST_BUFFER_DTS(buffer);
        }
        duration = GST_BUFFER_DURATION(buffer);
    }

    if (rec->pending.valid) {
        guint32 dur90k = compute_duration_90k(rec, rec->pending.pts, rec->pending.duration, pts);
        emit_pending(rec, dur90k);
    }

    rec->pending.data = copy;
    rec->pending.size = size;
    rec->pending.pts = pts;
    rec->pending.duration = duration;
    rec->pending.valid = TRUE;
}

void video_recorder_flush(VideoRecorder *rec) {
    if (rec == NULL || !rec->enabled || rec->failed) {
        pending_reset(&rec->pending);
        return;
    }

    if (rec->pending.valid) {
        guint32 dur90k = compute_duration_90k(rec, rec->pending.pts, rec->pending.duration, GST_CLOCK_TIME_NONE);
        emit_pending(rec, dur90k);
    }

    if (rec->fp != NULL) {
        fflush(rec->fp);
    }
}

void video_recorder_free(VideoRecorder *rec) {
    if (rec == NULL) {
        return;
    }

    video_recorder_flush(rec);

    if (rec->writer_initialized) {
        mp4_h26x_write_close(&rec->writer);
    }

    if (rec->mux != NULL) {
        int err = MP4E_close(rec->mux);
        if (err != MP4E_STATUS_OK) {
            LOGE("minimp4: MP4E_close failed (err=%d)", err);
        }
        rec->mux = NULL;
    }

    if (rec->fp != NULL) {
        fclose(rec->fp);
        rec->fp = NULL;
    }

    pending_reset(&rec->pending);

    g_free(rec);
}
