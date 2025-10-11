#include "video_recorder.h"

#include "logging.h"

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gst/app/gstappsink.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define MINIMP4_IMPLEMENTATION
#define MP4E_MAX_TRACKS 1
#if defined(__GNUC__) && !defined(__clang_analyzer__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "minimp4.h"
#if defined(__GNUC__) && !defined(__clang_analyzer__)
#pragma GCC diagnostic pop
#endif

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
    gchar *output_path;
    RecordMode mode;
    int sequential_mode_flag;
    int enable_fragmentation;
    struct PendingSample pending;
    gboolean awaiting_sync_warning;
    guint64 bytes_written;
    guint64 total_duration_90k;
    guint64 start_time_ns;
    GMutex stats_lock;
};

static gchar *recorder_timestamp_string(void) {
    GDateTime *now = g_date_time_new_now_local();
    if (now == NULL) {
        return g_strdup("00000000-000000");
    }
    gchar *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
    g_date_time_unref(now);
    if (stamp == NULL) {
        stamp = g_strdup("00000000-000000");
    }
    return stamp;
}

static gboolean path_looks_like_directory(const gchar *path) {
    if (path == NULL || *path == '\0') {
        return TRUE;
    }
    size_t len = strlen(path);
    if (len == 0) {
        return TRUE;
    }
    if (path[len - 1] == G_DIR_SEPARATOR) {
        return TRUE;
    }
    return g_file_test(path, G_FILE_TEST_IS_DIR);
}

static gchar *build_timestamped_output_path(const gchar *requested_path) {
    const gchar *base_path = (requested_path != NULL && requested_path[0] != '\0') ? requested_path : "/media";
    gchar *timestamp = recorder_timestamp_string();
    if (timestamp == NULL) {
        return NULL;
    }

    gchar *full_path = NULL;
    if (path_looks_like_directory(base_path)) {
        gchar *filename = g_strdup_printf("pixelpilot-%s.mp4", timestamp);
        full_path = g_build_filename(base_path, filename, NULL);
        g_free(filename);
    } else {
        gchar *dir = g_path_get_dirname(base_path);
        gchar *basename = g_path_get_basename(base_path);
        const gchar *dot = strrchr(basename, '.');
        gchar *with_timestamp = NULL;
        if (dot != NULL && dot != basename) {
            gsize name_len = (gsize)(dot - basename);
            gchar *name = g_strndup(basename, name_len);
            with_timestamp = g_strdup_printf("%s-%s%s", name, timestamp, dot);
            g_free(name);
        } else {
            with_timestamp = g_strdup_printf("%s-%s.mp4", basename, timestamp);
        }

        if (dir != NULL && dir[0] != '\0' && strcmp(dir, ".") != 0) {
            full_path = g_build_filename(dir, with_timestamp, NULL);
        } else {
            full_path = g_strdup(with_timestamp);
        }
        g_free(dir);
        g_free(basename);
        g_free(with_timestamp);
    }

    g_free(timestamp);
    return full_path;
}

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
    g_mutex_lock(&rec->stats_lock);
    rec->bytes_written += written;
    g_mutex_unlock(&rec->stats_lock);
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
    if (err == MP4E_STATUS_BAD_ARGUMENTS) {
        if (!rec->awaiting_sync_warning) {
            LOGI("record: waiting for VPS/SPS/PPS+IDR before writing MP4; dropping frame");
            rec->awaiting_sync_warning = TRUE;
        }
    } else if (err != MP4E_STATUS_OK) {
        LOGE("minimp4: failed to write access unit (err=%d)", err);
        rec->failed = TRUE;
    } else {
        rec->awaiting_sync_warning = FALSE;
        g_mutex_lock(&rec->stats_lock);
        rec->total_duration_90k += duration_90k;
        g_mutex_unlock(&rec->stats_lock);
    }

    pending_reset(&rec->pending);
}

VideoRecorder *video_recorder_new(const RecordCfg *cfg) {
    if (cfg == NULL || !cfg->enable) {
        return NULL;
    }

    gchar *output_path = build_timestamped_output_path(cfg->output_path);
    if (output_path == NULL) {
        LOGE("record: failed to prepare output path");
        return NULL;
    }

    gchar *dir = g_path_get_dirname(output_path);
    if (dir != NULL && dir[0] != '\0' && strcmp(dir, ".") != 0) {
        if (g_mkdir_with_parents(dir, 0755) != 0 && errno != EEXIST) {
            LOGE("record: failed to create directory %s: %s", dir, g_strerror(errno));
            g_free(dir);
            g_free(output_path);
            return NULL;
        }
    }
    g_free(dir);

    VideoRecorder *rec = g_new0(VideoRecorder, 1);
    if (rec == NULL) {
        g_free(output_path);
        return NULL;
    }

    g_mutex_init(&rec->stats_lock);
    rec->enabled = TRUE;
    rec->failed = FALSE;
    rec->default_duration_90k = 3000;
    rec->last_duration_90k = 0;
    rec->output_path = output_path;
    rec->bytes_written = 0;
    rec->total_duration_90k = 0;
    rec->start_time_ns = (guint64)g_get_monotonic_time() * 1000u;

    rec->mode = cfg->mode;
    rec->sequential_mode_flag = 1;
    rec->enable_fragmentation = 0;
    rec->awaiting_sync_warning = FALSE;
    switch (rec->mode) {
    case RECORD_MODE_STANDARD:
        rec->sequential_mode_flag = 0;
        rec->enable_fragmentation = 0;
        break;
    case RECORD_MODE_FRAGMENTED:
        rec->sequential_mode_flag = 1;
        rec->enable_fragmentation = 1;
        break;
    case RECORD_MODE_SEQUENTIAL:
    default:
        rec->sequential_mode_flag = 1;
        rec->enable_fragmentation = 0;
        break;
    }

    rec->fp = fopen(rec->output_path, "wb");
    if (rec->fp == NULL) {
        LOGE("record: failed to open %s: %s", rec->output_path, g_strerror(errno));
        g_free(rec->output_path);
        g_free(rec);
        return NULL;
    }

    LOGI("record: writing video to %s", rec->output_path);
    LOGI("record: MP4 mode=%s (sequential=%d, fragmented=%d)",
         cfg_record_mode_name(rec->mode), rec->sequential_mode_flag, rec->enable_fragmentation);

    rec->mux = MP4E_open(rec->sequential_mode_flag, rec->enable_fragmentation, rec, recorder_write_callback);
    if (rec->mux == NULL) {
        LOGE("minimp4: failed to allocate muxer");
        fclose(rec->fp);
        rec->fp = NULL;
        g_free(rec->output_path);
        g_mutex_clear(&rec->stats_lock);
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

    if (rec->output_path != NULL) {
        g_free(rec->output_path);
        rec->output_path = NULL;
    }
    pending_reset(&rec->pending);

    g_mutex_clear(&rec->stats_lock);
    g_free(rec);
}

void video_recorder_get_stats(const VideoRecorder *rec, VideoRecorderStats *stats) {
    if (stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    if (rec == NULL) {
        return;
    }

    g_mutex_lock((GMutex *)&rec->stats_lock);
    stats->active = rec->enabled && !rec->failed && rec->fp != NULL && rec->mux != NULL;
    stats->bytes_written = rec->bytes_written;
    stats->media_duration_ns = gst_util_uint64_scale(rec->total_duration_90k, GST_SECOND, 90000);
    if (rec->start_time_ns != 0) {
        guint64 now_ns = (guint64)g_get_monotonic_time() * 1000u;
        if (now_ns > rec->start_time_ns) {
            stats->elapsed_ns = now_ns - rec->start_time_ns;
        }
    }
    if (rec->output_path != NULL) {
        g_strlcpy(stats->output_path, rec->output_path, sizeof(stats->output_path));
    } else {
        stats->output_path[0] = '\0';
    }
    g_mutex_unlock((GMutex *)&rec->stats_lock);
}
