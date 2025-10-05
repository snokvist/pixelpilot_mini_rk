#define _GNU_SOURCE

#include "pipeline.h"
#include "logging.h"

#ifndef GST_USE_UNSTABLE_API
#define GST_USE_UNSTABLE_API
#endif

#include <errno.h>
#include <glib.h>
#include <glib-object.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/gstutils.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

#define CHECK_ELEM(elem, name)                                                                      \
    do {                                                                                            \
        if ((elem) == NULL) {                                                                       \
            LOGE("Failed to create GStreamer element '%s'", (name));                               \
            goto fail;                                                                              \
        }                                                                                           \
    } while (0)

static void ensure_gst_initialized(const AppCfg *cfg) {
    static gsize inited = 0;
    if (cfg->gst_log && getenv("GST_DEBUG") == NULL) {
        setenv("GST_DEBUG", "3", 1);
    }
    if (g_once_init_enter(&inited)) {
        gst_init(NULL, NULL);
        g_once_init_leave(&inited, 1);
    }
}

static GstCaps *build_appsrc_caps(const AppCfg *cfg, gboolean video_only) {
    if (video_only && cfg != NULL) {
        return gst_caps_new_simple("application/x-rtp", "media", G_TYPE_STRING, "video", "payload", G_TYPE_INT,
                                   cfg->vid_pt, "clock-rate", G_TYPE_INT, 90000, "encoding-name", G_TYPE_STRING,
                                   "H265", NULL);
    }

    return gst_caps_new_empty_simple("application/x-rtp");
}

static GstElement *create_udp_app_source(const AppCfg *cfg, gboolean video_only, UdpReceiver **receiver_out) {
    GstElement *appsrc_elem = gst_element_factory_make("appsrc", "udp_appsrc");
    UdpReceiver *receiver = NULL;
    GstCaps *caps = NULL;
    CHECK_ELEM(appsrc_elem, "appsrc");

    caps = build_appsrc_caps(cfg, video_only);
    if (caps == NULL) {
        LOGE("Failed to allocate RTP caps for appsrc");
        goto fail;
    }

    g_object_set(appsrc_elem, "is-live", TRUE, "format", GST_FORMAT_TIME, "stream-type",
                 GST_APP_STREAM_TYPE_STREAM, "max-bytes", (guint64)(4 * 1024 * 1024),
                 "do-timestamp", TRUE, NULL);

    GstAppSrc *appsrc = GST_APP_SRC(appsrc_elem);
    gst_app_src_set_caps(appsrc, caps);
    gst_caps_unref(caps);
    caps = NULL;
    gst_app_src_set_latency(appsrc, 0, 0);
    gst_app_src_set_max_bytes(appsrc, 4 * 1024 * 1024);

    receiver = udp_receiver_create(cfg->udp_port, cfg->vid_pt, cfg->aud_pt, appsrc);
    if (receiver == NULL) {
        LOGE("Failed to create UDP receiver");
        goto fail;
    }

    if (receiver_out != NULL) {
        *receiver_out = receiver;
    } else {
        udp_receiver_destroy(receiver);
    }
    return appsrc_elem;

fail:
    if (caps != NULL) {
        gst_caps_unref(caps);
    }
    if (receiver != NULL) {
        udp_receiver_destroy(receiver);
    }
    if (appsrc_elem != NULL) {
        gst_object_unref(appsrc_elem);
    }
    return NULL;
}

static gboolean set_enum_property_by_nick(GObject *object, const char *property, const char *nick);

static gboolean setup_udp_receiver_passthrough(PipelineState *ps, const AppCfg *cfg, int audio_disabled) {
    GstElement *pipeline = NULL;
    GstElement *appsrc = NULL;
    GstElement *depay = NULL;
    GstElement *parser = NULL;
    GstElement *capsfilter = NULL;
    GstElement *appsink = NULL;
    GstCaps *raw_caps = NULL;
    UdpReceiver *receiver = NULL;
    GstElement *audio_appsrc = NULL;
    GstElement *audio_queue_start = NULL;
    GstElement *audio_depay = NULL;
    GstElement *audio_decoder = NULL;
    GstElement *audio_convert = NULL;
    GstElement *audio_resample = NULL;
    GstElement *audio_queue_sink = NULL;
    GstElement *audio_sink = NULL;
    GstCaps *audio_caps = NULL;

    pipeline = gst_pipeline_new("pixelpilot-receiver");
    CHECK_ELEM(pipeline, "pipeline");

    appsrc = create_udp_app_source(cfg, TRUE, &receiver);
    if (appsrc == NULL) {
        goto fail;
    }

    depay = gst_element_factory_make("rtph265depay", "video_depay");
    CHECK_ELEM(depay, "rtph265depay");
    parser = gst_element_factory_make("h265parse", "video_parser");
    CHECK_ELEM(parser, "h265parse");
    appsink = gst_element_factory_make("appsink", "out_appsink");
    CHECK_ELEM(appsink, "appsink");

    g_object_set(appsink, "drop", TRUE, "max-buffers", 4, "sync", FALSE, NULL);
    gst_app_sink_set_max_buffers(GST_APP_SINK(appsink), 4);
    gst_app_sink_set_drop(GST_APP_SINK(appsink), TRUE);

    g_object_set(parser, "config-interval", -1, "disable-passthrough", TRUE, NULL);
    if (!set_enum_property_by_nick(G_OBJECT(parser), "stream-format", "byte-stream")) {
        LOGW("Failed to force h265parse stream-format=byte-stream; downstream decoder may misbehave");
    }
    if (!set_enum_property_by_nick(G_OBJECT(parser), "alignment", "au")) {
        LOGW("Failed to force h265parse alignment=au; downstream decoder may misbehave");
    }

    capsfilter = gst_element_factory_make("capsfilter", "video_capsfilter");
    CHECK_ELEM(capsfilter, "capsfilter");

    raw_caps = gst_caps_new_simple("video/x-h265", "stream-format", G_TYPE_STRING, "byte-stream",
                                   "alignment", G_TYPE_STRING, "au", NULL);
    if (raw_caps == NULL) {
        LOGE("Failed to allocate caps for video byte-stream enforcement");
        goto fail;
    }
    g_object_set(capsfilter, "caps", raw_caps, NULL);
    gst_app_sink_set_caps(GST_APP_SINK(appsink), raw_caps);
    gst_caps_unref(raw_caps);
    raw_caps = NULL;

    gst_bin_add_many(GST_BIN(pipeline), appsrc, depay, parser, capsfilter, appsink, NULL);
    if (!gst_element_link_many(appsrc, depay, parser, capsfilter, appsink, NULL)) {
        LOGE("Failed to link UDP receiver passthrough pipeline");
        goto fail;
    }

    gboolean enable_audio = (!cfg->no_audio && cfg->aud_pt >= 0 && !audio_disabled);
    if (enable_audio) {
        audio_appsrc = gst_element_factory_make("appsrc", "udp_audio_appsrc");
        audio_queue_start = gst_element_factory_make("queue", "audio_queue_start");
        audio_depay = gst_element_factory_make("rtpopusdepay", "audio_depay");
        audio_decoder = gst_element_factory_make("opusdec", "audio_decoder");
        audio_convert = gst_element_factory_make("audioconvert", "audio_convert");
        audio_resample = gst_element_factory_make("audioresample", "audio_resample");
        audio_queue_sink = gst_element_factory_make("queue", "audio_queue_sink");
        audio_sink = gst_element_factory_make("alsasink", "audio_sink");

        CHECK_ELEM(audio_appsrc, "appsrc");
        CHECK_ELEM(audio_queue_start, "queue");
        CHECK_ELEM(audio_depay, "rtpopusdepay");
        CHECK_ELEM(audio_decoder, "opusdec");
        CHECK_ELEM(audio_convert, "audioconvert");
        CHECK_ELEM(audio_resample, "audioresample");
        CHECK_ELEM(audio_queue_sink, "queue");
        CHECK_ELEM(audio_sink, "alsasink");

        g_object_set(audio_appsrc, "is-live", TRUE, "format", GST_FORMAT_TIME, "stream-type",
                     GST_APP_STREAM_TYPE_STREAM, "do-timestamp", TRUE, "max-bytes", (guint64)(1024 * 1024),
                     NULL);
        gst_app_src_set_latency(GST_APP_SRC(audio_appsrc), 0, 0);
        gst_app_src_set_max_bytes(GST_APP_SRC(audio_appsrc), 1024 * 1024);

        audio_caps = gst_caps_new_simple("application/x-rtp", "media", G_TYPE_STRING, "audio", "payload",
                                         G_TYPE_INT, cfg->aud_pt, "clock-rate", G_TYPE_INT, 48000, "encoding-name",
                                         G_TYPE_STRING, "OPUS", NULL);
        if (audio_caps == NULL) {
            LOGE("Failed to allocate RTP caps for audio appsrc");
            goto fail;
        }
        gst_app_src_set_caps(GST_APP_SRC(audio_appsrc), audio_caps);
        gst_caps_unref(audio_caps);
        audio_caps = NULL;

        g_object_set(audio_queue_start, "leaky", 2, "max-size-time", (guint64)0, "max-size-bytes", (guint64)0,
                     NULL);
        g_object_set(audio_queue_sink, "leaky", 2, NULL);
        g_object_set(audio_sink, "device", cfg->aud_dev, "sync", FALSE, "async", FALSE, NULL);

        gst_bin_add_many(GST_BIN(pipeline), audio_appsrc, audio_queue_start, audio_depay, audio_decoder,
                         audio_convert, audio_resample, audio_queue_sink, audio_sink, NULL);
        if (!gst_element_link_many(audio_appsrc, audio_queue_start, audio_depay, audio_decoder, audio_convert,
                                   audio_resample, audio_queue_sink, audio_sink, NULL)) {
            LOGE("Failed to link audio branch for UDP receiver passthrough");
            goto fail;
        }

        if (receiver != NULL) {
            udp_receiver_set_audio_appsrc(receiver, GST_APP_SRC(audio_appsrc));
        }
        ps->audio_disabled = 0;
    } else {
        if (receiver != NULL) {
            udp_receiver_set_audio_appsrc(receiver, NULL);
        }
        ps->audio_disabled = 1;
    }

    ps->pipeline = pipeline;
    ps->video_sink = appsink;
    ps->udp_receiver = receiver;
    return TRUE;

fail:
    if (raw_caps != NULL) {
        gst_caps_unref(raw_caps);
    }
    if (audio_caps != NULL) {
        gst_caps_unref(audio_caps);
    }
    if (receiver != NULL) {
        udp_receiver_destroy(receiver);
    }
    if (audio_sink != NULL && GST_OBJECT_PARENT(audio_sink) == NULL) {
        gst_object_unref(audio_sink);
    }
    if (audio_queue_sink != NULL && GST_OBJECT_PARENT(audio_queue_sink) == NULL) {
        gst_object_unref(audio_queue_sink);
    }
    if (audio_resample != NULL && GST_OBJECT_PARENT(audio_resample) == NULL) {
        gst_object_unref(audio_resample);
    }
    if (audio_convert != NULL && GST_OBJECT_PARENT(audio_convert) == NULL) {
        gst_object_unref(audio_convert);
    }
    if (audio_decoder != NULL && GST_OBJECT_PARENT(audio_decoder) == NULL) {
        gst_object_unref(audio_decoder);
    }
    if (audio_depay != NULL && GST_OBJECT_PARENT(audio_depay) == NULL) {
        gst_object_unref(audio_depay);
    }
    if (audio_queue_start != NULL && GST_OBJECT_PARENT(audio_queue_start) == NULL) {
        gst_object_unref(audio_queue_start);
    }
    if (audio_appsrc != NULL && GST_OBJECT_PARENT(audio_appsrc) == NULL) {
        gst_object_unref(audio_appsrc);
    }
    if (appsink != NULL && GST_OBJECT_PARENT(appsink) == NULL) {
        gst_object_unref(appsink);
    }
    if (capsfilter != NULL && GST_OBJECT_PARENT(capsfilter) == NULL) {
        gst_object_unref(capsfilter);
    }
    if (parser != NULL && GST_OBJECT_PARENT(parser) == NULL) {
        gst_object_unref(parser);
    }
    if (depay != NULL && GST_OBJECT_PARENT(depay) == NULL) {
        gst_object_unref(depay);
    }
    if (appsrc != NULL && GST_OBJECT_PARENT(appsrc) == NULL) {
        gst_object_unref(appsrc);
    }
    if (pipeline != NULL) {
        gst_object_unref(pipeline);
    }
    ps->pipeline = NULL;
    ps->video_sink = NULL;
    ps->udp_receiver = NULL;
    return FALSE;
}

static gboolean setup_gst_udpsrc_pipeline(PipelineState *ps, const AppCfg *cfg) {
    if (ps == NULL || cfg == NULL) {
        return FALSE;
    }

    gchar *desc = g_strdup_printf(
        "udpsrc name=udp_source port=%d caps=\"application/x-rtp, media=(string)video, encoding-name=(string)H265, clock-rate=(int)90000\" ! "
        "rtph265depay name=video_depay ! "
        "h265parse name=video_parser config-interval=-1 ! "
        "video/x-h265, stream-format=\"byte-stream\" ! "
        "appsink drop=true name=out_appsink",
        cfg->udp_port);

    if (desc == NULL) {
        LOGE("Failed to allocate udpsrc pipeline description");
        return FALSE;
    }

    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &error);
    g_free(desc);

    if (pipeline == NULL) {
        LOGE("Failed to create udpsrc pipeline: %s", error != NULL ? error->message : "unknown error");
        if (error != NULL) {
            g_error_free(error);
        }
        return FALSE;
    }
    if (error != NULL) {
        LOGW("udpsrc pipeline reported warnings: %s", error->message);
        g_error_free(error);
    }

    GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "out_appsink");
    if (appsink == NULL) {
        LOGE("Failed to find appsink in udpsrc pipeline");
        gst_object_unref(pipeline);
        return FALSE;
    }

    g_object_set(appsink, "drop", TRUE, "max-buffers", 4, "sync", FALSE, NULL);
    gst_app_sink_set_max_buffers(GST_APP_SINK(appsink), 4);
    gst_app_sink_set_drop(GST_APP_SINK(appsink), TRUE);

    GstCaps *caps = gst_caps_new_simple("video/x-h265", "stream-format", G_TYPE_STRING, "byte-stream",
                                        "alignment", G_TYPE_STRING, "au", NULL);
    if (caps != NULL) {
        gst_app_sink_set_caps(GST_APP_SINK(appsink), caps);
        gst_caps_unref(caps);
    } else {
        LOGW("Failed to allocate caps for udpsrc appsink");
    }

    GstElement *parser = gst_bin_get_by_name(GST_BIN(pipeline), "video_parser");
    if (parser != NULL) {
        if (!set_enum_property_by_nick(G_OBJECT(parser), "stream-format", "byte-stream")) {
            LOGW("Failed to force h265parse stream-format=byte-stream; downstream decoder may misbehave");
        }
        if (!set_enum_property_by_nick(G_OBJECT(parser), "alignment", "au")) {
            LOGW("Failed to force h265parse alignment=au; downstream decoder may misbehave");
        }
        gst_object_unref(parser);
    } else {
        LOGW("udpsrc pipeline missing h265parse element; byte-stream enforcement skipped");
    }

    ps->pipeline = pipeline;
    ps->video_sink = appsink;

    gst_object_unref(appsink);
    return TRUE;
}

static gchar *canonicalize_enum_token(const char *input) {
    if (input == NULL) {
        return NULL;
    }

    gsize len = strlen(input);
    gchar *canonical = g_malloc(len + 1);
    if (canonical == NULL) {
        return NULL;
    }

    gchar *dst = canonical;
    for (const gchar *src = input; *src != '\0'; ++src) {
        if (*src == '-' || *src == '_' || *src == ' ') {
            continue;
        }
        *dst++ = g_ascii_toupper(*src);
    }
    *dst = '\0';
    return canonical;
}

static gboolean enum_matches_string(const char *candidate, const char *target) {
    if (candidate == NULL || target == NULL) {
        return FALSE;
    }

    if (g_ascii_strcasecmp(candidate, target) == 0) {
        return TRUE;
    }

    g_autofree gchar *canon_candidate = canonicalize_enum_token(candidate);
    g_autofree gchar *canon_target = canonicalize_enum_token(target);
    if (canon_candidate == NULL || canon_target == NULL) {
        return FALSE;
    }

    if (g_strcmp0(canon_candidate, canon_target) == 0) {
        return TRUE;
    }

    return g_str_has_suffix(canon_candidate, canon_target);
}

static GParamSpec *find_property_with_aliases(GObjectClass *klass, const char *property) {
    if (klass == NULL || property == NULL) {
        return NULL;
    }

    GParamSpec *pspec = g_object_class_find_property(klass, property);
    if (pspec != NULL) {
        return pspec;
    }

    g_autofree gchar *alternate = g_strdup(property);
    if (alternate != NULL) {
        for (gchar *p = alternate; *p != '\0'; ++p) {
            if (*p == '-') {
                *p = '_';
            } else if (*p == '_') {
                *p = '-';
            }
        }

        pspec = g_object_class_find_property(klass, alternate);
        if (pspec != NULL) {
            return pspec;
        }
    }

    guint n_props = 0;
    GParamSpec **props = g_object_class_list_properties(klass, &n_props);
    if (props == NULL) {
        return NULL;
    }

    for (guint i = 0; i < n_props; ++i) {
        const char *name = props[i]->name;
        const char *nick = g_param_spec_get_nick(props[i]);
        if ((name != NULL && (enum_matches_string(name, property) || enum_matches_string(property, name))) ||
            (nick != NULL && (enum_matches_string(nick, property) || enum_matches_string(property, nick)))) {
            pspec = props[i];
            break;
        }
    }

    g_free(props);
    return pspec;
}

static gboolean set_enum_property_by_nick(GObject *object, const char *property, const char *nick) {
    if (object == NULL || property == NULL || nick == NULL) {
        return FALSE;
    }

    GObjectClass *klass = G_OBJECT_GET_CLASS(object);
    if (klass == NULL) {
        return FALSE;
    }

    GParamSpec *pspec = find_property_with_aliases(klass, property);
    if (pspec == NULL) {
        return FALSE;
    }

    const char *canon_property = pspec->name != NULL ? pspec->name : property;

    if (!G_IS_PARAM_SPEC_ENUM(pspec)) {
        gst_util_set_object_arg(object, canon_property, nick);
        return TRUE;
    }

    GEnumClass *enum_class = G_ENUM_CLASS(g_type_class_ref(pspec->value_type));
    if (enum_class == NULL) {
        return FALSE;
    }

    gboolean success = FALSE;
    for (gint i = 0; i < enum_class->n_values; ++i) {
        const GEnumValue *value = &enum_class->values[i];
        if ((value->value_name != NULL && enum_matches_string(value->value_name, nick)) ||
            (value->value_nick != NULL && enum_matches_string(value->value_nick, nick))) {
            g_object_set(object, canon_property, value->value, NULL);
            success = TRUE;
            break;
        }
    }

    if (!success) {
        gst_util_set_object_arg(object, canon_property, nick);
        success = TRUE;
    }

    g_type_class_unref(enum_class);
    return success;
}

static gpointer appsink_thread_func(gpointer data) {
    PipelineState *ps = (PipelineState *)data;
    GstAppSink *appsink = ps->video_sink != NULL ? GST_APP_SINK(ps->video_sink) : NULL;
    if (appsink == NULL) {
        g_mutex_lock(&ps->lock);
        ps->appsink_thread_running = FALSE;
        g_mutex_unlock(&ps->lock);
        return NULL;
    }

    size_t max_packet = video_decoder_max_packet_size(ps->decoder_initialized ? ps->decoder : NULL);
    if (max_packet == 0) {
        max_packet = 1024 * 1024;
    }

    while (TRUE) {
        g_mutex_lock(&ps->lock);
        gboolean stop_requested = ps->stop_requested;
        gboolean decoder_running = ps->decoder_running;
        g_mutex_unlock(&ps->lock);

        if (stop_requested || !decoder_running) {
            break;
        }

        GstSample *sample = gst_app_sink_try_pull_sample(appsink, 100 * GST_MSECOND);
        if (sample == NULL) {
            continue;
        }

        GstBuffer *buffer = gst_sample_get_buffer(sample);
        if (buffer != NULL) {
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                if (map.size > 0 && map.size <= max_packet) {
                    if (video_decoder_feed(ps->decoder, map.data, map.size) != 0) {
                        LOGV("Video decoder feed busy; retrying");
                    }
                } else if (map.size > max_packet) {
                    LOGW("Video sample too large (%zu bytes > %zu)", map.size, max_packet);
                }
                gst_buffer_unmap(buffer, &map);
            }
        }
        gst_sample_unref(sample);
    }

    if (ps->decoder != NULL) {
        video_decoder_send_eos(ps->decoder);
    }

    g_mutex_lock(&ps->lock);
    ps->appsink_thread_running = FALSE;
    g_mutex_unlock(&ps->lock);
    return NULL;
}

static gpointer bus_thread_func(gpointer data) {
    PipelineState *ps = (PipelineState *)data;
    cpu_set_t thread_mask;
    if (cfg_get_thread_affinity(ps->cfg, ps->bus_thread_cpu_slot, &thread_mask)) {
        int err = pthread_setaffinity_np(pthread_self(), sizeof(thread_mask), &thread_mask);
        if (err != 0) {
            LOGW("Pipeline bus thread: pthread_setaffinity_np failed: %s", g_strerror(err));
        }
    }
    GstBus *bus = gst_element_get_bus(ps->pipeline);
    if (bus == NULL) {
        LOGE("Failed to get pipeline bus");
        g_mutex_lock(&ps->lock);
        ps->encountered_error = TRUE;
        ps->bus_thread_running = FALSE;
        ps->state = PIPELINE_STOPPED;
        g_cond_signal(&ps->cond);
        g_mutex_unlock(&ps->lock);
        return NULL;
    }

    gboolean running = TRUE;
    while (running) {
        GstMessage *msg = gst_bus_timed_pop(bus, GST_MSECOND * 100);
        if (msg == NULL) {
            g_mutex_lock(&ps->lock);
            running = ps->stop_requested ? FALSE : TRUE;
            g_mutex_unlock(&ps->lock);
            continue;
        }

        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            LOGE("Pipeline error: %s (debug=%s)", err != NULL ? err->message : "unknown",
                 dbg != NULL ? dbg : "none");
            if (err != NULL) {
                g_error_free(err);
            }
            g_free(dbg);
            g_mutex_lock(&ps->lock);
            ps->encountered_error = TRUE;
            ps->stop_requested = TRUE;
            g_mutex_unlock(&ps->lock);
            running = FALSE;
            break;
        }
        case GST_MESSAGE_EOS:
            LOGI("Pipeline reached EOS");
            g_mutex_lock(&ps->lock);
            ps->stop_requested = TRUE;
            g_mutex_unlock(&ps->lock);
            running = FALSE;
            break;
        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(ps->pipeline)) {
                GstState old_state, new_state, pending;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending);
                LOGV("Pipeline state changed: %s -> %s", gst_element_state_get_name(old_state),
                     gst_element_state_get_name(new_state));
                if (new_state == GST_STATE_NULL) {
                    g_mutex_lock(&ps->lock);
                    ps->stop_requested = TRUE;
                    g_mutex_unlock(&ps->lock);
                    running = FALSE;
                }
            }
            break;
        }
        default:
            break;
        }
        gst_message_unref(msg);
    }

    if (ps->pipeline != NULL) {
        gst_element_set_state(ps->pipeline, GST_STATE_NULL);
    }
    gst_object_unref(bus);

    g_mutex_lock(&ps->lock);
    ps->bus_thread_running = FALSE;
    ps->state = PIPELINE_STOPPED;
    g_cond_signal(&ps->cond);
    g_mutex_unlock(&ps->lock);

    return NULL;
}

static void cleanup_pipeline(PipelineState *ps) {
    if (ps->appsink_thread != NULL) {
        g_thread_join(ps->appsink_thread);
        ps->appsink_thread = NULL;
    }
    ps->appsink_thread_running = FALSE;

    if (ps->decoder != NULL) {
        video_decoder_free(ps->decoder);
        ps->decoder = NULL;
    }
    ps->decoder_initialized = FALSE;
    ps->decoder_running = FALSE;

    if (ps->udp_receiver != NULL) {
        udp_receiver_destroy(ps->udp_receiver);
        ps->udp_receiver = NULL;
    }
    ps->video_sink = NULL;
    if (ps->pipeline != NULL) {
        gst_object_unref(ps->pipeline);
        ps->pipeline = NULL;
    }
    g_mutex_lock(&ps->lock);
    ps->bus_thread_running = FALSE;
    ps->encountered_error = FALSE;
    ps->stop_requested = FALSE;
    g_mutex_unlock(&ps->lock);
}

int pipeline_start(const AppCfg *cfg, const ModesetResult *ms, int drm_fd, int audio_disabled, PipelineState *ps) {
    if (ps->state != PIPELINE_STOPPED) {
        LOGW("pipeline_start: refused (state=%d)", ps->state);
        return -1;
    }

    if (ms == NULL) {
        LOGE("pipeline_start: modeset information unavailable");
        return -1;
    }

    ensure_gst_initialized(cfg);

    if (!ps->initialized) {
        g_mutex_init(&ps->lock);
        g_cond_init(&ps->cond);
        ps->initialized = TRUE;
    }

    ps->pipeline = NULL;
    ps->udp_receiver = NULL;
    ps->video_sink = NULL;
    ps->appsink_thread = NULL;
    ps->appsink_thread_running = FALSE;
    ps->decoder_initialized = FALSE;
    ps->decoder_running = FALSE;
    ps->cfg = cfg;
    ps->bus_thread_cpu_slot = 0;

    GstElement *pipeline = NULL;
    gboolean force_audio_disabled = FALSE;

    if (cfg->custom_sink == CUSTOM_SINK_UDPSRC) {
        LOGI("Custom sink mode 'udpsrc' selected; UDP receiver stats disabled");
        if (!setup_gst_udpsrc_pipeline(ps, cfg)) {
            goto fail;
        }
        pipeline = ps->pipeline;
        force_audio_disabled = TRUE;
    } else if (cfg->custom_sink == CUSTOM_SINK_RECEIVER) {
        LOGI("Custom sink mode 'receiver' selected; using direct RTP pipeline");
        if (!setup_udp_receiver_passthrough(ps, cfg, audio_disabled)) {
            goto fail;
        }
        pipeline = ps->pipeline;
        force_audio_disabled = ps->audio_disabled;
    } else {
        LOGE("Unknown custom sink mode %d", cfg->custom_sink);
        goto fail;
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOGE("Failed to set pipeline to PLAYING");
        goto fail;
    }
    if (ret == GST_STATE_CHANGE_ASYNC) {
        GstState state = GST_STATE_NULL;
        GstState pending = GST_STATE_NULL;
        ret = gst_element_get_state(pipeline, &state, &pending, GST_SECOND);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            LOGE("Pipeline state change failed during async wait");
            goto fail;
        }
    }

    if (drm_fd < 0) {
        LOGE("Invalid DRM file descriptor");
        goto fail;
    }

    if (ps->decoder == NULL) {
        ps->decoder = video_decoder_new();
        if (ps->decoder == NULL) {
            LOGE("Failed to allocate video decoder state");
            goto fail;
        }
    }

    if (video_decoder_init(ps->decoder, cfg, ms, drm_fd) != 0) {
        LOGE("Failed to initialise video decoder");
        goto fail;
    }
    ps->decoder_initialized = TRUE;

    if (video_decoder_start(ps->decoder) != 0) {
        LOGE("Failed to start video decoder threads");
        goto fail;
    }
    ps->decoder_running = TRUE;

    ps->appsink_thread_running = TRUE;
    ps->appsink_thread = g_thread_new("appsink-pump", appsink_thread_func, ps);
    if (ps->appsink_thread == NULL) {
        ps->appsink_thread_running = FALSE;
        LOGE("Failed to start appsink thread");
        goto fail;
    }

    int cpu_slot = 0;

    if (ps->udp_receiver != NULL) {
        if (udp_receiver_start(ps->udp_receiver, cfg, cpu_slot) != 0) {
            LOGE("Failed to start UDP receiver");
            goto fail;
        }
        cpu_slot++;
    }

    ps->bus_thread_running = TRUE;
    ps->stop_requested = FALSE;
    ps->encountered_error = FALSE;
    ps->audio_disabled = (audio_disabled || cfg->no_audio || cfg->aud_pt < 0 || force_audio_disabled) ? 1 : 0;
    ps->bus_thread_cpu_slot = cpu_slot;
    ps->bus_thread = g_thread_new("gst-bus", bus_thread_func, ps);
    if (ps->bus_thread == NULL) {
        LOGE("Failed to start bus thread");
        goto fail;
    }

    ps->state = PIPELINE_RUNNING;
    return 0;

fail:
    g_mutex_lock(&ps->lock);
    ps->stop_requested = TRUE;
    ps->decoder_running = FALSE;
    g_mutex_unlock(&ps->lock);
    if (ps->bus_thread != NULL) {
        g_thread_join(ps->bus_thread);
        ps->bus_thread = NULL;
    }
    if (pipeline != NULL) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
    }
    cleanup_pipeline(ps);
    ps->state = PIPELINE_STOPPED;
    return -1;
}

void pipeline_stop(PipelineState *ps, int wait_ms_total) {
    if (ps->state == PIPELINE_STOPPED) {
        return;
    }

    g_mutex_lock(&ps->lock);
    ps->state = PIPELINE_STOPPING;
    ps->stop_requested = TRUE;
    ps->decoder_running = FALSE;
    g_mutex_unlock(&ps->lock);

    if (ps->udp_receiver != NULL) {
        udp_receiver_stop(ps->udp_receiver);
    }

    if (ps->pipeline != NULL) {
        gst_element_send_event(ps->pipeline, gst_event_new_eos());
        gst_element_set_state(ps->pipeline, GST_STATE_NULL);
    }

    if (ps->bus_thread != NULL) {
        if (wait_ms_total > 0) {
            gint64 deadline = g_get_monotonic_time() + (gint64)wait_ms_total * G_TIME_SPAN_MILLISECOND;
            g_mutex_lock(&ps->lock);
            while (ps->bus_thread_running) {
                if (!g_cond_wait_until(&ps->cond, &ps->lock, deadline)) {
                    break;
                }
            }
            gboolean still_running = ps->bus_thread_running;
            g_mutex_unlock(&ps->lock);
            if (still_running) {
                LOGW("Pipeline bus thread did not exit in time; forcing join");
            }
        }
        g_thread_join(ps->bus_thread);
        ps->bus_thread = NULL;
    }

    cleanup_pipeline(ps);
    ps->state = PIPELINE_STOPPED;
}

void pipeline_poll_child(PipelineState *ps) {
    if (ps->bus_thread == NULL) {
        return;
    }

    gboolean running;
    gboolean had_error;
    g_mutex_lock(&ps->lock);
    running = ps->bus_thread_running;
    had_error = ps->encountered_error;
    g_mutex_unlock(&ps->lock);

    if (!running) {
        g_thread_join(ps->bus_thread);
        ps->bus_thread = NULL;
        cleanup_pipeline(ps);
        ps->state = PIPELINE_STOPPED;
        if (had_error) {
            LOGI("Pipeline exited due to error");
        } else {
            LOGI("Pipeline exited cleanly");
        }
    }
}

void pipeline_set_receiver_stats_enabled(PipelineState *ps, gboolean enabled) {
    if (ps == NULL) {
        return;
    }

    g_mutex_lock(&ps->lock);
    UdpReceiver *receiver = ps->udp_receiver;
    if (receiver != NULL) {
        udp_receiver_set_stats_enabled(receiver, enabled);
    }
    g_mutex_unlock(&ps->lock);
}

int pipeline_get_receiver_stats(const PipelineState *ps, UdpReceiverStats *stats) {
    if (ps == NULL || stats == NULL) {
        return -1;
    }
    if (ps->udp_receiver == NULL) {
        return -1;
    }
    udp_receiver_get_stats(ps->udp_receiver, stats);
    return 0;
}
