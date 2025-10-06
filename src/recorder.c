#define _GNU_SOURCE

#include "recorder.h"

#include "logging.h"

#ifndef GST_USE_UNSTABLE_API
#define GST_USE_UNSTABLE_API
#endif

#include <string.h>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

struct Recorder {
    GstElement *pipeline;
    GstAppSrc *video_appsrc;
    GstAppSrc *audio_appsrc;
    GstElement *mux;
    GstPad *mux_video_pad;
    GstPad *mux_audio_pad;
    gchar *output_path;
    gboolean audio_requested;
    gboolean audio_enabled;
    gboolean running;
    gboolean logged_video_flow_error;
    gboolean video_caps_set;
    gboolean logged_caps_warning;
    GMutex lock;
};

static GstBusSyncReply recorder_bus_sync_handler(GstBus *bus, GstMessage *msg, gpointer user_data) {
    (void)bus;
    Recorder *rec = (Recorder *)user_data;
    if (rec == NULL || msg == NULL) {
        return GST_BUS_PASS;
    }

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        LOGE("Recorder pipeline error: %s (debug=%s)", err != NULL ? err->message : "unknown",
             dbg != NULL ? dbg : "none");
        if (err != NULL) {
            g_error_free(err);
        }
        g_free(dbg);
        break;
    }
    case GST_MESSAGE_WARNING: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_warning(msg, &err, &dbg);
        LOGW("Recorder pipeline warning: %s (debug=%s)", err != NULL ? err->message : "unknown",
             dbg != NULL ? dbg : "none");
        if (err != NULL) {
            g_error_free(err);
        }
        g_free(dbg);
        break;
    }
    default:
        break;
    }
    return GST_BUS_PASS;
}

Recorder *recorder_new(void) {
    Recorder *rec = g_new0(Recorder, 1);
    if (rec == NULL) {
        return NULL;
    }
    g_mutex_init(&rec->lock);
    rec->audio_requested = FALSE;
    rec->audio_enabled = FALSE;
    rec->running = FALSE;
    rec->logged_video_flow_error = FALSE;
    rec->video_caps_set = FALSE;
    rec->logged_caps_warning = FALSE;
    rec->output_path = NULL;
    return rec;
}

static void recorder_release_pads_locked(Recorder *rec) {
    if (rec == NULL) {
        return;
    }
    if (rec->mux != NULL) {
        if (rec->mux_video_pad != NULL) {
            gst_element_release_request_pad(rec->mux, rec->mux_video_pad);
            gst_object_unref(rec->mux_video_pad);
            rec->mux_video_pad = NULL;
        }
        if (rec->mux_audio_pad != NULL) {
            gst_element_release_request_pad(rec->mux, rec->mux_audio_pad);
            gst_object_unref(rec->mux_audio_pad);
            rec->mux_audio_pad = NULL;
        }
    }
}

void recorder_stop(Recorder *rec) {
    if (rec == NULL) {
        return;
    }

    GstElement *pipeline = NULL;
    GstAppSrc *video_appsrc = NULL;
    GstAppSrc *audio_appsrc = NULL;
    GstElement *mux = NULL;
    gchar *output_path = NULL;

    g_mutex_lock(&rec->lock);
    if (!rec->running && rec->pipeline == NULL) {
        output_path = rec->output_path;
        rec->output_path = NULL;
        g_mutex_unlock(&rec->lock);
        if (output_path != NULL) {
            g_free(output_path);
        }
        return;
    }

    pipeline = rec->pipeline;
    video_appsrc = rec->video_appsrc;
    audio_appsrc = rec->audio_appsrc;
    mux = rec->mux;
    output_path = rec->output_path;

    rec->running = FALSE;
    rec->audio_enabled = FALSE;
    rec->logged_video_flow_error = FALSE;
    rec->video_caps_set = FALSE;
    rec->logged_caps_warning = FALSE;
    rec->output_path = NULL;

    recorder_release_pads_locked(rec);

    rec->pipeline = NULL;
    rec->video_appsrc = NULL;
    rec->audio_appsrc = NULL;
    rec->mux = NULL;

    g_mutex_unlock(&rec->lock);

    if (video_appsrc != NULL) {
        gst_app_src_end_of_stream(video_appsrc);
    }
    if (audio_appsrc != NULL) {
        gst_app_src_end_of_stream(audio_appsrc);
    }

    if (pipeline != NULL) {
        gst_element_send_event(pipeline, gst_event_new_eos());
        gst_element_set_state(pipeline, GST_STATE_NULL);
    }

    if (pipeline != NULL) {
        gst_object_unref(pipeline);
    }
    (void)mux;
    if (output_path != NULL) {
        g_free(output_path);
    }
}

void recorder_free(Recorder *rec) {
    if (rec == NULL) {
        return;
    }
    recorder_stop(rec);
    g_mutex_clear(&rec->lock);
    g_free(rec);
}

gboolean recorder_is_running(Recorder *rec) {
    if (rec == NULL) {
        return FALSE;
    }
    g_mutex_lock(&rec->lock);
    gboolean running = rec->running;
    g_mutex_unlock(&rec->lock);
    return running;
}

GstAppSrc *recorder_get_audio_appsrc(Recorder *rec) {
    if (rec == NULL) {
        return NULL;
    }
    g_mutex_lock(&rec->lock);
    GstAppSrc *src = rec->audio_enabled ? rec->audio_appsrc : NULL;
    g_mutex_unlock(&rec->lock);
    return src;
}

static gchar *recorder_make_unique_path(const char *base_path) {
    if (base_path == NULL || base_path[0] == '\0') {
        return NULL;
    }

    GDateTime *now = g_date_time_new_now_local();
    if (now == NULL) {
        return g_strdup(base_path);
    }

    gchar *timestamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
    g_date_time_unref(now);
    if (timestamp == NULL) {
        return g_strdup(base_path);
    }

    const gchar *last_sep = strrchr(base_path, G_DIR_SEPARATOR);
#ifdef G_OS_WIN32
    const gchar *last_win_sep = strrchr(base_path, '\\');
    if (last_win_sep != NULL && (last_sep == NULL || last_win_sep > last_sep)) {
        last_sep = last_win_sep;
    }
#endif
    const gchar *dot = strrchr(base_path, '.');
    gboolean dot_is_ext = dot != NULL && (last_sep == NULL || dot > last_sep);

    gchar *prefix = NULL;
    const gchar *suffix = dot_is_ext ? dot : "";

    if (dot_is_ext) {
        prefix = g_strndup(base_path, (gsize)(dot - base_path));
    } else {
        prefix = g_strdup(base_path);
    }

    if (prefix == NULL) {
        g_free(timestamp);
        return g_strdup(base_path);
    }

    gchar *result = g_strdup_printf("%s-%s%s", prefix, timestamp, suffix);
    g_free(prefix);
    g_free(timestamp);
    if (result == NULL) {
        return g_strdup(base_path);
    }
    return result;
}

static gboolean recorder_set_video_caps_if_needed(Recorder *rec, const GstCaps *caps) {
    if (rec == NULL || caps == NULL) {
        return FALSE;
    }

    GstAppSrc *appsrc = NULL;
    gboolean already_set = FALSE;

    g_mutex_lock(&rec->lock);
    already_set = rec->video_caps_set;
    if (!rec->running || rec->video_appsrc == NULL) {
        g_mutex_unlock(&rec->lock);
        return FALSE;
    }
    if (!already_set) {
        appsrc = GST_APP_SRC(gst_object_ref(rec->video_appsrc));
    }
    g_mutex_unlock(&rec->lock);

    if (already_set || appsrc == NULL) {
        if (appsrc != NULL) {
            gst_object_unref(appsrc);
        }
        return already_set;
    }

    GstCaps *caps_copy = gst_caps_copy(caps);
    if (caps_copy == NULL) {
        gst_object_unref(appsrc);
        g_mutex_lock(&rec->lock);
        if (!rec->logged_caps_warning) {
            rec->logged_caps_warning = TRUE;
            g_mutex_unlock(&rec->lock);
            LOGW("Recorder: failed to copy video caps; recorded stream may be invalid");
        } else {
            g_mutex_unlock(&rec->lock);
        }
        return FALSE;
    }

    gst_app_src_set_caps(appsrc, caps_copy);
    gst_caps_unref(caps_copy);

    g_mutex_lock(&rec->lock);
    rec->video_caps_set = TRUE;
    g_mutex_unlock(&rec->lock);

    gst_object_unref(appsrc);
    return TRUE;
}

static gboolean recorder_setup_audio_branch(Recorder *rec,
                                            const AppCfg *cfg,
                                            gboolean enable_audio,
                                            GstElement *pipeline,
                                            GstElement *mux) {
    if (rec == NULL || cfg == NULL || pipeline == NULL || mux == NULL) {
        return FALSE;
    }

    if (!enable_audio) {
        return FALSE;
    }

    GstElement *audio_appsrc = gst_element_factory_make("appsrc", "rec_audio_src");
    GstElement *audio_queue = gst_element_factory_make("queue", "rec_audio_queue");
    GstElement *audio_depay = gst_element_factory_make("rtpopusdepay", "rec_audio_depay");
    GstElement *audio_parse = gst_element_factory_make("opusparse", "rec_audio_parse");

    if (audio_appsrc == NULL || audio_queue == NULL || audio_depay == NULL || audio_parse == NULL) {
        if (audio_appsrc != NULL) {
            gst_object_unref(audio_appsrc);
        }
        if (audio_queue != NULL) {
            gst_object_unref(audio_queue);
        }
        if (audio_depay != NULL) {
            gst_object_unref(audio_depay);
        }
        if (audio_parse != NULL) {
            gst_object_unref(audio_parse);
        }
        LOGW("Recorder: audio branch unavailable; recording without audio");
        return FALSE;
    }

    g_object_set(audio_appsrc,
                 "is-live",
                 TRUE,
                 "format",
                 GST_FORMAT_TIME,
                 "stream-type",
                 GST_APP_STREAM_TYPE_STREAM,
                 "do-timestamp",
                 FALSE,
                 NULL);
    gst_app_src_set_latency(GST_APP_SRC(audio_appsrc), 0, 0);
    gst_app_src_set_max_bytes(GST_APP_SRC(audio_appsrc), 512 * 1024);

    GstCaps *audio_caps = gst_caps_new_simple("application/x-rtp",
                                              "media",
                                              G_TYPE_STRING,
                                              "audio",
                                              "payload",
                                              G_TYPE_INT,
                                              cfg->aud_pt,
                                              "clock-rate",
                                              G_TYPE_INT,
                                              48000,
                                              "encoding-name",
                                              G_TYPE_STRING,
                                              "OPUS",
                                              NULL);
    if (audio_caps == NULL) {
        gst_object_unref(audio_appsrc);
        gst_object_unref(audio_queue);
        gst_object_unref(audio_depay);
        gst_object_unref(audio_parse);
        LOGW("Recorder: failed to allocate audio caps; recording without audio");
        return FALSE;
    }

    gst_app_src_set_caps(GST_APP_SRC(audio_appsrc), audio_caps);
    gst_caps_unref(audio_caps);

    g_object_set(audio_queue, "leaky", 2, NULL);

    gst_bin_add_many(GST_BIN(pipeline), audio_appsrc, audio_queue, audio_depay, audio_parse, NULL);

    if (!gst_element_link_many(audio_appsrc, audio_queue, audio_depay, audio_parse, NULL)) {
        gst_bin_remove_many(GST_BIN(pipeline), audio_appsrc, audio_queue, audio_depay, audio_parse, NULL);
        LOGW("Recorder: failed to link audio branch; recording without audio");
        return FALSE;
    }

    GstPad *audio_src_pad = gst_element_get_static_pad(audio_parse, "src");
    GstPad *audio_sink_pad = gst_element_get_request_pad(mux, "audio_%u");
    if (audio_src_pad == NULL || audio_sink_pad == NULL) {
        if (audio_src_pad != NULL) {
            gst_object_unref(audio_src_pad);
        }
        if (audio_sink_pad != NULL) {
            gst_element_release_request_pad(mux, audio_sink_pad);
            gst_object_unref(audio_sink_pad);
        }
        gst_element_unlink(audio_appsrc, audio_queue);
        gst_element_unlink(audio_queue, audio_depay);
        gst_element_unlink(audio_depay, audio_parse);
        gst_bin_remove_many(GST_BIN(pipeline), audio_appsrc, audio_queue, audio_depay, audio_parse, NULL);
        LOGW("Recorder: failed to obtain audio pads; recording without audio");
        return FALSE;
    }

    if (gst_pad_link(audio_src_pad, audio_sink_pad) != GST_PAD_LINK_OK) {
        gst_element_release_request_pad(mux, audio_sink_pad);
        gst_object_unref(audio_sink_pad);
        gst_object_unref(audio_src_pad);
        gst_element_unlink(audio_appsrc, audio_queue);
        gst_element_unlink(audio_queue, audio_depay);
        gst_element_unlink(audio_depay, audio_parse);
        gst_bin_remove_many(GST_BIN(pipeline), audio_appsrc, audio_queue, audio_depay, audio_parse, NULL);
        LOGW("Recorder: failed to connect audio branch; recording without audio");
        return FALSE;
    }

    gst_object_unref(audio_src_pad);

    g_mutex_lock(&rec->lock);
    rec->audio_appsrc = GST_APP_SRC(audio_appsrc);
    rec->mux_audio_pad = audio_sink_pad;
    rec->audio_enabled = TRUE;
    g_mutex_unlock(&rec->lock);

    return TRUE;
}

gboolean recorder_start(Recorder *rec, const AppCfg *cfg, gboolean enable_audio, const GstCaps *video_caps) {
    if (rec == NULL || cfg == NULL) {
        return FALSE;
    }

    recorder_stop(rec);

    if (cfg->record.output_path[0] == '\0') {
        LOGE("Recorder: output path not configured");
        return FALSE;
    }

    GstElement *pipeline = gst_pipeline_new("pixelpilot-recorder");
    if (pipeline == NULL) {
        LOGE("Recorder: failed to create pipeline");
        return FALSE;
    }

    GstElement *video_appsrc = gst_element_factory_make("appsrc", "rec_video_src");
    GstElement *video_queue = gst_element_factory_make("queue", "rec_video_queue");
    GstElement *video_parse = gst_element_factory_make("h265parse", "rec_video_parse");
    GstElement *mux = gst_element_factory_make("matroskamux", "rec_mux");
    GstElement *filesink = gst_element_factory_make("filesink", "rec_sink");

    if (video_appsrc == NULL || video_queue == NULL || video_parse == NULL || mux == NULL || filesink == NULL) {
        if (pipeline != NULL) {
            gst_object_unref(pipeline);
        }
        if (video_appsrc != NULL) {
            gst_object_unref(video_appsrc);
        }
        if (video_queue != NULL) {
            gst_object_unref(video_queue);
        }
        if (video_parse != NULL) {
            gst_object_unref(video_parse);
        }
        if (mux != NULL) {
            gst_object_unref(mux);
        }
        if (filesink != NULL) {
            gst_object_unref(filesink);
        }
        LOGE("Recorder: failed to construct video branch");
        return FALSE;
    }

    g_object_set(video_appsrc,
                 "is-live",
                 TRUE,
                 "format",
                 GST_FORMAT_TIME,
                 "stream-type",
                 GST_APP_STREAM_TYPE_STREAM,
                 "do-timestamp",
                 FALSE,
                 NULL);
    gst_app_src_set_latency(GST_APP_SRC(video_appsrc), 0, 0);
    gst_app_src_set_max_bytes(GST_APP_SRC(video_appsrc), 4 * 1024 * 1024);

    g_object_set(video_queue, "leaky", 2, NULL);
    g_object_set(video_parse, "config-interval", -1, NULL);
    g_object_set(mux, "writing-app", "pixelpilot-mini-rk", "streamable", TRUE, NULL);

    gchar *output_path = recorder_make_unique_path(cfg->record.output_path);
    if (output_path == NULL) {
        gst_object_unref(pipeline);
        gst_object_unref(video_appsrc);
        gst_object_unref(video_queue);
        gst_object_unref(video_parse);
        gst_object_unref(mux);
        gst_object_unref(filesink);
        LOGE("Recorder: failed to prepare output path");
        return FALSE;
    }

    g_object_set(filesink, "location", output_path, "async", FALSE, NULL);

    gst_bin_add_many(GST_BIN(pipeline), video_appsrc, video_queue, video_parse, mux, filesink, NULL);

    if (!gst_element_link_many(video_appsrc, video_queue, video_parse, NULL)) {
        g_free(output_path);
        gst_object_unref(pipeline);
        LOGE("Recorder: failed to link video branch");
        return FALSE;
    }

    if (!gst_element_link(mux, filesink)) {
        g_free(output_path);
        gst_object_unref(pipeline);
        LOGE("Recorder: failed to link mux to filesink");
        return FALSE;
    }

    GstPad *video_src_pad = gst_element_get_static_pad(video_parse, "src");
    GstPad *video_sink_pad = gst_element_get_request_pad(mux, "video_%u");
    if (video_src_pad == NULL || video_sink_pad == NULL) {
        if (video_src_pad != NULL) {
            gst_object_unref(video_src_pad);
        }
        if (video_sink_pad != NULL) {
            gst_element_release_request_pad(mux, video_sink_pad);
            gst_object_unref(video_sink_pad);
        }
        g_free(output_path);
        gst_object_unref(pipeline);
        LOGE("Recorder: failed to obtain video pads");
        return FALSE;
    }

    if (gst_pad_link(video_src_pad, video_sink_pad) != GST_PAD_LINK_OK) {
        g_free(output_path);
        gst_element_release_request_pad(mux, video_sink_pad);
        gst_object_unref(video_sink_pad);
        gst_object_unref(video_src_pad);
        gst_object_unref(pipeline);
        LOGE("Recorder: failed to connect video branch");
        return FALSE;
    }
    gst_object_unref(video_src_pad);

    gboolean audio_enabled = recorder_setup_audio_branch(rec, cfg, enable_audio && cfg->aud_pt >= 0, pipeline, mux);

    GstBus *bus = gst_element_get_bus(pipeline);
    if (bus != NULL) {
        gst_bus_set_sync_handler(bus, recorder_bus_sync_handler, rec, NULL);
        gst_object_unref(bus);
    }

    GstStateChangeReturn state_ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        g_free(output_path);
        LOGE("Recorder: failed to start pipeline");
        return FALSE;
    }

    if (state_ret == GST_STATE_CHANGE_ASYNC) {
        GstState current = GST_STATE_NULL;
        GstState pending = GST_STATE_NULL;
        state_ret = gst_element_get_state(pipeline, &current, &pending, GST_SECOND);
        if (state_ret == GST_STATE_CHANGE_FAILURE) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            g_free(output_path);
            LOGE("Recorder: pipeline did not reach playing state");
            return FALSE;
        }
    }

    g_mutex_lock(&rec->lock);
    rec->pipeline = pipeline;
    rec->video_appsrc = GST_APP_SRC(video_appsrc);
    rec->mux = mux;
    rec->mux_video_pad = video_sink_pad;
    rec->audio_requested = enable_audio ? TRUE : FALSE;
    rec->audio_enabled = audio_enabled ? TRUE : FALSE;
    rec->running = TRUE;
    rec->logged_video_flow_error = FALSE;
    rec->video_caps_set = FALSE;
    if (rec->output_path != NULL) {
        g_free(rec->output_path);
    }
    rec->output_path = output_path;
    if (!audio_enabled) {
        rec->audio_appsrc = NULL;
        if (rec->mux_audio_pad != NULL) {
            gst_element_release_request_pad(mux, rec->mux_audio_pad);
            gst_object_unref(rec->mux_audio_pad);
            rec->mux_audio_pad = NULL;
        }
    }
    g_mutex_unlock(&rec->lock);

    if (video_caps != NULL) {
        recorder_set_video_caps_if_needed(rec, video_caps);
    }

    LOGI("Recorder: writing Matroska to %s (%s)",
         output_path,
         audio_enabled ? "audio enabled" : "video only");

    return TRUE;
}

void recorder_push_video_buffer(Recorder *rec, GstBuffer *buffer, const GstCaps *caps) {
    if (rec == NULL || buffer == NULL) {
        if (buffer != NULL) {
            gst_buffer_unref(buffer);
        }
        return;
    }

    if (caps != NULL) {
        recorder_set_video_caps_if_needed(rec, caps);
    } else {
        g_mutex_lock(&rec->lock);
        gboolean warned = rec->logged_caps_warning;
        if (!warned) {
            rec->logged_caps_warning = TRUE;
            g_mutex_unlock(&rec->lock);
            LOGW("Recorder: missing video caps for buffer; stream headers may be incomplete");
        } else {
            g_mutex_unlock(&rec->lock);
        }
    }

    GstAppSrc *appsrc = NULL;
    gboolean running = FALSE;

    g_mutex_lock(&rec->lock);
    running = rec->running;
    appsrc = rec->video_appsrc;
    g_mutex_unlock(&rec->lock);

    if (!running || appsrc == NULL) {
        gst_buffer_unref(buffer);
        return;
    }

    GstFlowReturn flow = gst_app_src_push_buffer(appsrc, buffer);
    if (G_UNLIKELY(flow != GST_FLOW_OK)) {
        if (!rec->logged_video_flow_error) {
            LOGW("Recorder: video push returned %s", gst_flow_get_name(flow));
            rec->logged_video_flow_error = TRUE;
        }
    }
}
