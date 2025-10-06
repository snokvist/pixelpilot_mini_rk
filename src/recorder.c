#include "recorder.h"

#include "logging.h"

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gst/gst.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"

#define RECORDER_AUDIO_MAGIC GINT32_TO_BE(0x5050434d) /* 'PPCM' */
#define RECORDER_AUDIO_VERSION 1

typedef struct {
    guint32 magic;
    guint8 version;
    guint8 channels;
    guint8 bytes_per_sample;
    guint8 reserved;
    guint32 sample_rate_le;
} RecorderAudioDsi;

struct Recorder {
    gboolean enabled;
    FILE *fp;
    char *output_path;
    MP4E_mux_t *mux;
    mp4_h26x_writer_t video_writer;
    gboolean video_ready;
    guint video_default_duration_90k;
    gboolean audio_enabled;
    guint audio_rate;
    guint audio_channels;
    guint audio_bytes_per_sample;
    int audio_track_id;
    GMutex lock;
};

static int recorder_write_cb(int64_t offset, const void *buffer, size_t size, void *token) {
    Recorder *rec = (Recorder *)token;
    if (rec == NULL || rec->fp == NULL) {
        return MP4E_STATUS_FILE_WRITE_ERROR;
    }
    if (fseeko(rec->fp, offset, SEEK_SET) != 0) {
        return MP4E_STATUS_FILE_WRITE_ERROR;
    }
    size_t written = fwrite(buffer, 1, size, rec->fp);
    if (written != size) {
        return MP4E_STATUS_FILE_WRITE_ERROR;
    }
    return MP4E_STATUS_OK;
}

static char *recorder_make_path(const RecordCfg *cfg) {
    if (cfg == NULL || !cfg->enable || cfg->directory[0] == '\0') {
        return NULL;
    }

    if (g_mkdir_with_parents(cfg->directory, 0755) != 0) {
        LOGE("Recorder: failed to create directory '%s': %s", cfg->directory, g_strerror(errno));
        return NULL;
    }

    GDateTime *now = g_date_time_new_now_local();
    if (now == NULL) {
        LOGE("Recorder: failed to query current time");
        return NULL;
    }

    gchar *timestamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
    g_date_time_unref(now);
    if (timestamp == NULL) {
        LOGE("Recorder: failed to format timestamp");
        return NULL;
    }

    gchar *filename = g_strdup_printf("pixelpilot-%s.mp4", timestamp);
    g_free(timestamp);
    if (filename == NULL) {
        LOGE("Recorder: failed to build output filename");
        return NULL;
    }

    gchar *path = g_build_filename(cfg->directory, filename, NULL);
    g_free(filename);
    return path;
}

static gboolean recorder_add_video_track(Recorder *rec, MP4E_mux_t *mux, int width, int height) {
    if (rec == NULL || mux == NULL) {
        return FALSE;
    }

    MP4E_track_t tr = {0};
    tr.object_type_indication = MP4_OBJECT_TYPE_HEVC;
    tr.track_media_kind = e_video;
    tr.time_scale = 90000;
    tr.default_duration = 0;
    tr.u.v.width = width > 0 ? width : 1920;
    tr.u.v.height = height > 0 ? height : 1080;

    int track_id = MP4E_add_track(mux, &tr);
    if (track_id < 0) {
        LOGE("Recorder: failed to add video track (%d)", track_id);
        return FALSE;
    }

    if (mp4_h26x_write_init(&rec->video_writer, mux, tr.u.v.width, tr.u.v.height, 1) != MP4E_STATUS_OK) {
        LOGE("Recorder: failed to initialise H.265 writer");
        return FALSE;
    }

    rec->video_ready = TRUE;
    rec->video_default_duration_90k = 3003; // ~30 FPS fallback
    return TRUE;
}

static gboolean recorder_add_audio_track(Recorder *rec, MP4E_mux_t *mux, guint rate, guint channels, guint bytes_per_sample) {
    if (rec == NULL || mux == NULL || !rec->audio_enabled) {
        return FALSE;
    }

    MP4E_track_t tr = {0};
    tr.object_type_indication = MP4_OBJECT_TYPE_USER_PRIVATE;
    tr.track_media_kind = e_private;
    tr.time_scale = rate > 0 ? rate : 48000;
    tr.default_duration = 0;
    tr.u.a.channelcount = channels;

    int track_id = MP4E_add_track(mux, &tr);
    if (track_id < 0) {
        LOGE("Recorder: failed to add audio track (%d)", track_id);
        return FALSE;
    }

    RecorderAudioDsi dsi = {0};
    dsi.magic = RECORDER_AUDIO_MAGIC;
    dsi.version = RECORDER_AUDIO_VERSION;
    dsi.channels = (guint8)channels;
    dsi.bytes_per_sample = (guint8)bytes_per_sample;
    dsi.sample_rate_le = GUINT32_TO_LE(rate);

    if (MP4E_set_dsi(mux, track_id, &dsi, sizeof(dsi)) != MP4E_STATUS_OK) {
        LOGE("Recorder: failed to set audio track metadata");
        return FALSE;
    }

    rec->audio_track_id = track_id;
    rec->audio_rate = rate;
    rec->audio_channels = channels;
    rec->audio_bytes_per_sample = bytes_per_sample;
    return TRUE;
}

Recorder *recorder_open(const RecordCfg *cfg,
                        int video_width,
                        int video_height,
                        gboolean audio_enabled,
                        guint audio_rate,
                        guint audio_channels,
                        guint audio_bytes_per_sample) {
    if (cfg == NULL || !cfg->enable || cfg->directory[0] == '\0') {
        return NULL;
    }

    Recorder *rec = g_new0(Recorder, 1);
    if (rec == NULL) {
        LOGE("Recorder: allocation failed");
        return NULL;
    }

    g_mutex_init(&rec->lock);
    rec->enabled = TRUE;
    rec->audio_enabled = audio_enabled ? TRUE : FALSE;

    rec->output_path = recorder_make_path(cfg);
    if (rec->output_path == NULL) {
        recorder_free(rec);
        return NULL;
    }

    rec->fp = fopen(rec->output_path, "wb+");
    if (rec->fp == NULL) {
        LOGE("Recorder: failed to open %s: %s", rec->output_path, g_strerror(errno));
        recorder_free(rec);
        return NULL;
    }

    int sequential_mode = 0;
    int fragmented_mode = 0;
    switch (cfg->mux_mode) {
    case RECORD_MUX_SEQUENTIAL:
        sequential_mode = 1;
        break;
    case RECORD_MUX_FRAGMENTED:
        sequential_mode = 1;
        fragmented_mode = 1;
        break;
    case RECORD_MUX_STANDARD:
    default:
        sequential_mode = 0;
        fragmented_mode = 0;
        break;
    }

    rec->mux = MP4E_open(sequential_mode, fragmented_mode, rec, recorder_write_cb);
    if (rec->mux == NULL) {
        LOGE("Recorder: failed to initialise MP4 muxer");
        recorder_free(rec);
        return NULL;
    }

    if (!recorder_add_video_track(rec, rec->mux, video_width, video_height)) {
        recorder_free(rec);
        return NULL;
    }

    if (rec->audio_enabled) {
        if (!recorder_add_audio_track(rec, rec->mux, audio_rate, audio_channels, audio_bytes_per_sample)) {
            LOGW("Recorder: disabling audio track due to initialisation failure");
            rec->audio_enabled = FALSE;
        }
    }

    LOGI("Recorder: writing MP4 to %s (%s mux)", rec->output_path,
         cfg_record_mux_mode_name(cfg->mux_mode));
    return rec;
}

void recorder_close(Recorder *rec) {
    if (rec == NULL) {
        return;
    }

    g_mutex_lock(&rec->lock);
    if (rec->mux != NULL) {
        MP4E_close(rec->mux);
        rec->mux = NULL;
    }
    if (rec->video_ready) {
        mp4_h26x_write_close(&rec->video_writer);
        rec->video_ready = FALSE;
    }
    g_mutex_unlock(&rec->lock);

    if (rec->fp != NULL) {
        fflush(rec->fp);
        fclose(rec->fp);
        rec->fp = NULL;
    }
}

void recorder_free(Recorder *rec) {
    if (rec == NULL) {
        return;
    }
    recorder_close(rec);
    if (rec->output_path != NULL) {
        g_free(rec->output_path);
        rec->output_path = NULL;
    }
    g_mutex_clear(&rec->lock);
    g_free(rec);
}

static guint32 recorder_video_duration(const Recorder *rec, GstClockTime duration_ns) {
    if (duration_ns == GST_CLOCK_TIME_NONE || duration_ns == 0) {
        return rec->video_default_duration_90k;
    }
    guint64 duration = duration_ns * 90000ull;
    duration += GST_SECOND / 2;
    duration /= GST_SECOND;
    if (duration == 0) {
        duration = rec->video_default_duration_90k;
    }
    if (duration > G_MAXUINT32) {
        duration = G_MAXUINT32;
    }
    return (guint32)duration;
}

void recorder_push_video(Recorder *rec, const guint8 *data, gsize size, GstClockTime duration_ns) {
    if (rec == NULL || !rec->enabled || rec->mux == NULL || !rec->video_ready || data == NULL || size == 0) {
        return;
    }

    guint32 duration = recorder_video_duration(rec, duration_ns);

    g_mutex_lock(&rec->lock);
    int err = mp4_h26x_write_nal(&rec->video_writer, data, (int)size, (int)duration);
    g_mutex_unlock(&rec->lock);

    if (G_UNLIKELY(err != MP4E_STATUS_OK)) {
        LOGW("Recorder: failed to write video sample (%d)");
    }
}

void recorder_push_audio(Recorder *rec, const guint8 *data, gsize size, guint samples) {
    if (rec == NULL || !rec->enabled || rec->mux == NULL || !rec->audio_enabled || data == NULL || size == 0) {
        return;
    }

    guint channels = rec->audio_channels > 0 ? rec->audio_channels : 2;
    guint bytes_per_sample = rec->audio_bytes_per_sample > 0 ? rec->audio_bytes_per_sample : 2;
    if (samples == 0 && channels > 0 && bytes_per_sample > 0) {
        samples = (guint)(size / (channels * bytes_per_sample));
    }
    if (samples == 0) {
        return;
    }

    g_mutex_lock(&rec->lock);
    int err = MP4E_put_sample(rec->mux, rec->audio_track_id, data, (int)size, (int)samples, MP4E_SAMPLE_DEFAULT);
    g_mutex_unlock(&rec->lock);

    if (G_UNLIKELY(err != MP4E_STATUS_OK)) {
        LOGW("Recorder: failed to write audio sample (%d)");
    }
}

const char *recorder_output_path(const Recorder *rec) {
    return rec != NULL ? rec->output_path : NULL;
}
