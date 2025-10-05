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

static GstCaps *build_appsrc_caps(void) {
    return gst_caps_new_empty_simple("application/x-rtp");
}

static GstElement *create_udp_udpsrc(const AppCfg *cfg) {
    GstElement *udpsrc = gst_element_factory_make("udpsrc", "udp_udpsrc");
    GstCaps *caps = NULL;
    CHECK_ELEM(udpsrc, "udpsrc");

    caps = build_appsrc_caps();
    if (caps == NULL) {
        LOGE("Failed to allocate RTP caps for udpsrc");
        goto fail;
    }

    g_object_set(udpsrc, "port", cfg->udp_port, "caps", caps, "buffer-size", 262144, NULL);
    gst_caps_unref(caps);
    caps = NULL;

    return udpsrc;

fail:
    if (caps != NULL) {
        gst_caps_unref(caps);
    }
    if (udpsrc != NULL) {
        gst_object_unref(udpsrc);
    }
    return NULL;
}

static GstElement *create_udp_app_source(const AppCfg *cfg, UdpReceiver **receiver_out) {
    GstElement *appsrc_elem = gst_element_factory_make("appsrc", "udp_appsrc");
    UdpReceiver *receiver = NULL;
    GstCaps *caps = NULL;
    CHECK_ELEM(appsrc_elem, "appsrc");

    caps = build_appsrc_caps();
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

    receiver = udp_receiver_create(cfg->udp_port, cfg->udp_fallback_port, cfg->vid_pt, cfg->aud_pt, appsrc);
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

static GstElement *create_udp_source(const AppCfg *cfg, UdpReceiver **receiver_out) {
    if (cfg->use_gst_udpsrc) {
        if (receiver_out != NULL) {
            *receiver_out = NULL;
        }
        return create_udp_udpsrc(cfg);
    }
    return create_udp_app_source(cfg, receiver_out);
}

static GstCaps *make_rtp_caps(const char *media, int payload_type, int clock_rate, const char *encoding_name) {
    return gst_caps_new_simple("application/x-rtp", "media", G_TYPE_STRING, media, "payload", G_TYPE_INT,
                               payload_type, "clock-rate", G_TYPE_INT, clock_rate, "encoding-name", G_TYPE_STRING,
                               encoding_name, NULL);
}

static GstCaps *caps_for_payload(const PipelineState *ps, gint payload_type) {
    if (ps == NULL || ps->cfg == NULL) {
        return NULL;
    }

    const AppCfg *cfg = ps->cfg;
    if (payload_type == cfg->vid_pt) {
        return make_rtp_caps("video", cfg->vid_pt, 90000, "H265");
    }

    if (payload_type == cfg->aud_pt && cfg->aud_pt >= 0) {
        return make_rtp_caps("audio", cfg->aud_pt, 48000, "OPUS");
    }

    return NULL;
}

static GstCaps *rtpbin_request_pt_map_cb(GstElement *element, guint session, guint payload_type, gpointer user_data) {
    (void)element;
    (void)session;
    PipelineState *ps = (PipelineState *)user_data;
    GstCaps *caps = caps_for_payload(ps, (gint)payload_type);
    if (caps == NULL) {
        LOGW("No caps mapping available for payload type %u", payload_type);
    }
    return caps;
}

static gboolean link_pad_to_queue(GstPad *src_pad, GstElement *queue, GstPad **stored_pad, const char *name) {
    if (queue == NULL || src_pad == NULL) {
        return FALSE;
    }

    if (stored_pad != NULL && *stored_pad != NULL) {
        return TRUE;
    }

    GstPad *sink_pad = gst_element_get_static_pad(queue, "sink");
    if (sink_pad == NULL) {
        LOGE("Failed to get sink pad for %s branch", name);
        return FALSE;
    }

    GstPadLinkReturn link_ret = gst_pad_link(src_pad, sink_pad);
    gst_object_unref(sink_pad);
    if (link_ret != GST_PAD_LINK_OK) {
        LOGE("Failed to link rtpbin pad to %s branch (ret=%d)", name, link_ret);
        return FALSE;
    }

    if (stored_pad != NULL) {
        *stored_pad = gst_object_ref(src_pad);
    }
    LOGI("Linked rtpbin %s pad", name);
    return TRUE;
}

static gboolean pad_has_media(GstPad *pad, const char *media_type) {
    GstCaps *caps = gst_pad_get_current_caps(pad);
    gboolean matches = FALSE;
    if (caps != NULL) {
        const GstStructure *s = gst_caps_get_structure(caps, 0);
        const gchar *media = s != NULL ? gst_structure_get_string(s, "media") : NULL;
        if (media != NULL && g_strcmp0(media, media_type) == 0) {
            matches = TRUE;
        }
        gst_caps_unref(caps);
    }
    return matches;
}

static gint pad_payload_type(GstPad *pad) {
    gint payload_type = -1;
    if (pad == NULL) {
        return payload_type;
    }

    GObjectClass *klass = G_OBJECT_GET_CLASS(pad);
    if (klass != NULL && g_object_class_find_property(klass, "pt") != NULL) {
        g_object_get(pad, "pt", &payload_type, NULL);
    }

    if (payload_type < 0) {
        const gchar *pad_name = GST_OBJECT_NAME(pad);
        if (pad_name != NULL) {
            const gchar *underscore = strrchr(pad_name, '_');
            if (underscore != NULL) {
                payload_type = (gint)g_ascii_strtoll(underscore + 1, NULL, 10);
            }
        }
    }

    return payload_type;
}

static void rtpbin_pad_added_cb(GstElement *element, GstPad *pad, gpointer user_data) {
    (void)element;
    PipelineState *ps = (PipelineState *)user_data;
    gboolean is_video = pad_has_media(pad, "video");
    gboolean is_audio = pad_has_media(pad, "audio");
    if (!is_video && !is_audio) {
        gint payload_type = pad_payload_type(pad);
        if (payload_type == ps->cfg->vid_pt) {
            is_video = TRUE;
        } else if (payload_type == ps->cfg->aud_pt && ps->cfg->aud_pt >= 0) {
            is_audio = TRUE;
        }
    }

    if (is_video) {
        if (!link_pad_to_queue(pad, ps->video_branch_entry, &ps->video_pad, "video")) {
            LOGW("Failed to link newly added video pad from rtpbin");
        }
        return;
    }
    if (is_audio) {
        if (!link_pad_to_queue(pad, ps->audio_branch_entry, &ps->audio_pad, "audio")) {
            LOGW("Failed to link newly added audio pad from rtpbin");
        }
        return;
    }
    LOGW("Ignoring rtpbin pad with unsupported media type");
}

static void rtpbin_pad_removed_cb(GstElement *element, GstPad *pad, gpointer user_data) {
    (void)element;
    PipelineState *ps = (PipelineState *)user_data;
    if (ps->video_pad == pad) {
        gst_object_unref(ps->video_pad);
        ps->video_pad = NULL;
        LOGI("rtpbin video pad removed");
    } else if (ps->audio_pad == pad) {
        gst_object_unref(ps->audio_pad);
        ps->audio_pad = NULL;
        LOGI("rtpbin audio pad removed");
    }
}

static void clear_stored_pad(GstPad **pad) {
    if (pad != NULL && *pad != NULL) {
        gst_object_unref(*pad);
        *pad = NULL;
    }
}

static gboolean set_enum_property_by_nick(GObject *object, const char *property, const char *nick) {
    if (object == NULL || property == NULL || nick == NULL) {
        return FALSE;
    }

    GObjectClass *klass = G_OBJECT_GET_CLASS(object);
    if (klass == NULL) {
        return FALSE;
    }

    GParamSpec *pspec = g_object_class_find_property(klass, property);
    if (pspec == NULL || !G_IS_PARAM_SPEC_ENUM(pspec)) {
        return FALSE;
    }

    GEnumClass *enum_class = G_ENUM_CLASS(g_type_class_ref(pspec->value_type));
    if (enum_class == NULL) {
        return FALSE;
    }

    const GEnumValue *value = g_enum_get_value_by_name(enum_class, nick);
    if (value == NULL) {
        value = g_enum_get_value_by_nick(enum_class, nick);
    }

    if (value == NULL) {
        g_type_class_unref(enum_class);
        return FALSE;
    }

    g_object_set(object, property, value->value, NULL);
    g_type_class_unref(enum_class);
    return TRUE;
}

static gboolean build_video_branch(PipelineState *ps, GstElement *pipeline, const AppCfg *cfg) {
    GstElement *queue_pre = gst_element_factory_make("queue", "video_queue_pre");
    GstElement *depay = gst_element_factory_make("rtph265depay", "video_depay");
    GstElement *parser = gst_element_factory_make("h265parse", "video_parser");
    GstElement *capsfilter = gst_element_factory_make("capsfilter", "video_capsfilter");
    GstElement *queue_post = gst_element_factory_make("queue", "video_queue_post");
    GstElement *appsink = gst_element_factory_make("appsink", "video_appsink");

    CHECK_ELEM(queue_pre, "queue");
    CHECK_ELEM(depay, "rtph265depay");
    CHECK_ELEM(parser, "h265parse");
    CHECK_ELEM(capsfilter, "capsfilter");
    CHECK_ELEM(queue_post, "queue");
    CHECK_ELEM(appsink, "appsink");

    g_object_set(queue_pre, "leaky", cfg->video_queue_leaky, "max-size-buffers", cfg->video_queue_pre_buffers,
                 "max-size-time", (guint64)0, "max-size-bytes", (guint64)0, NULL);
    g_object_set(queue_post, "leaky", cfg->video_queue_leaky, "max-size-buffers", cfg->video_queue_post_buffers,
                 "max-size-time", (guint64)0, "max-size-bytes", (guint64)0, NULL);

    g_object_set(parser, "config-interval", -1, "disable-passthrough", TRUE, NULL);
    if (!set_enum_property_by_nick(G_OBJECT(parser), "stream-format", "byte-stream")) {
        LOGW("Failed to force h265parse stream-format=byte-stream; downstream decoder may misbehave");
    }
    if (!set_enum_property_by_nick(G_OBJECT(parser), "alignment", "au")) {
        LOGW("Failed to force h265parse alignment=au; downstream decoder may misbehave");
    }

    GstCaps *raw_caps = gst_caps_new_simple("video/x-h265", "stream-format", G_TYPE_STRING, "byte-stream",
                                            "alignment", G_TYPE_STRING, "au", NULL);
    if (raw_caps == NULL) {
        LOGE("Failed to allocate caps for video byte-stream enforcement");
        goto fail;
    }
    g_object_set(capsfilter, "caps", raw_caps, NULL);
    gst_app_sink_set_caps(GST_APP_SINK(appsink), raw_caps);
    gst_caps_unref(raw_caps);

    g_object_set(appsink, "drop", TRUE, "max-buffers", 4, "sync", FALSE, NULL);
    gst_app_sink_set_max_buffers(GST_APP_SINK(appsink), 4);
    gst_app_sink_set_drop(GST_APP_SINK(appsink), TRUE);

    gst_bin_add_many(GST_BIN(pipeline), queue_pre, depay, parser, capsfilter, queue_post, appsink, NULL);

    if (!gst_element_link_many(queue_pre, depay, parser, capsfilter, queue_post, appsink, NULL)) {
        LOGE("Failed to link video branch");
        return FALSE;
    }

    ps->video_branch_entry = queue_pre;
    ps->video_sink = appsink;
    return TRUE;

fail:
    ps->video_branch_entry = NULL;
    return FALSE;
}

static gboolean build_audio_branch(PipelineState *ps, GstElement *pipeline, const AppCfg *cfg, int audio_disabled) {
    gboolean disable_audio_branch = cfg->no_audio || audio_disabled;

    GstElement *queue_start = gst_element_factory_make("queue", "audio_queue_start");
    CHECK_ELEM(queue_start, "queue");
    g_object_set(queue_start, "leaky", 2, "max-size-time", (guint64)0, "max-size-bytes", (guint64)0, NULL);

    if (disable_audio_branch) {
        GstElement *fakesink = gst_element_factory_make("fakesink", "audio_fakesink");
        CHECK_ELEM(fakesink, "fakesink");
        g_object_set(fakesink, "sync", FALSE, NULL);

        gst_bin_add_many(GST_BIN(pipeline), queue_start, fakesink, NULL);
        if (!gst_element_link(queue_start, fakesink)) {
            LOGE("Failed to link audio fakesink branch");
            return FALSE;
        }
        ps->audio_branch_entry = queue_start;
        ps->audio_disabled = 1;
        return TRUE;
    }

    GstElement *depay = gst_element_factory_make("rtpopusdepay", "audio_depay");
    GstElement *decoder = gst_element_factory_make("opusdec", "audio_decoder");
    GstElement *aconv = gst_element_factory_make("audioconvert", "audio_convert");
    GstElement *ares = gst_element_factory_make("audioresample", "audio_resample");
    GstElement *queue_sink = gst_element_factory_make("queue", "audio_queue_sink");
    GstElement *alsa = gst_element_factory_make("alsasink", "audio_sink");

    CHECK_ELEM(depay, "rtpopusdepay");
    CHECK_ELEM(decoder, "opusdec");
    CHECK_ELEM(aconv, "audioconvert");
    CHECK_ELEM(ares, "audioresample");
    CHECK_ELEM(queue_sink, "queue");
    CHECK_ELEM(alsa, "alsasink");

    g_object_set(queue_sink, "leaky", 2, NULL);
    g_object_set(alsa, "device", cfg->aud_dev, "sync", FALSE, "async", FALSE, NULL);

    gst_bin_add_many(GST_BIN(pipeline), queue_start, depay, decoder, aconv, ares, queue_sink, alsa, NULL);

    if (!gst_element_link_many(queue_start, depay, decoder, aconv, ares, queue_sink, alsa, NULL)) {
        LOGE("Failed to link audio branch");
        return FALSE;
    }

    ps->audio_branch_entry = queue_start;
    ps->audio_disabled = 0;
    return TRUE;

fail:
    ps->audio_branch_entry = NULL;
    return FALSE;
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
    if (ps->rtpbin != NULL) {
        g_signal_handlers_disconnect_by_data(ps->rtpbin, ps);
        ps->rtpbin = NULL;
    }
    clear_stored_pad(&ps->video_pad);
    clear_stored_pad(&ps->audio_pad);
    ps->video_branch_entry = NULL;
    ps->audio_branch_entry = NULL;
    ps->video_sink = NULL;
    ps->source = NULL;
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

    GstElement *pipeline = gst_pipeline_new("pixelpilot-pipeline");
    CHECK_ELEM(pipeline, "pipeline");
    ps->pipeline = pipeline;
    ps->source = NULL;
    ps->rtpbin = NULL;
    ps->video_branch_entry = NULL;
    ps->audio_branch_entry = NULL;
    ps->video_pad = NULL;
    ps->audio_pad = NULL;
    ps->udp_receiver = NULL;
    ps->appsink_thread = NULL;
    ps->appsink_thread_running = FALSE;
    ps->decoder_initialized = FALSE;
    ps->decoder_running = FALSE;
    ps->cfg = cfg;
    ps->bus_thread_cpu_slot = 0;

    UdpReceiver *receiver = NULL;
    GstElement *source = create_udp_source(cfg, &receiver);
    if (source == NULL) {
        goto fail;
    }

    GstElement *rtpbin = gst_element_factory_make("rtpbin", "rtpbin");
    CHECK_ELEM(rtpbin, "rtpbin");
    g_object_set(rtpbin, "latency", cfg->latency_ms, NULL);

    gst_bin_add_many(GST_BIN(pipeline), source, rtpbin, NULL);

    GstPad *rtp_sink = gst_element_get_request_pad(rtpbin, "recv_rtp_sink_0");
    GstPad *src_pad = gst_element_get_static_pad(source, "src");
    if (rtp_sink == NULL || src_pad == NULL) {
        LOGE("Failed to obtain pads for linking source to rtpbin");
        if (src_pad != NULL) {
            gst_object_unref(src_pad);
        }
        if (rtp_sink != NULL) {
            gst_object_unref(rtp_sink);
        }
        goto fail;
    }
    if (gst_pad_link(src_pad, rtp_sink) != GST_PAD_LINK_OK) {
        LOGE("Failed to link source to rtpbin");
        gst_object_unref(src_pad);
        gst_object_unref(rtp_sink);
        goto fail;
    }
    gst_object_unref(src_pad);
    gst_object_unref(rtp_sink);

    ps->source = source;
    ps->rtpbin = rtpbin;
    ps->udp_receiver = receiver;

    if (cfg->use_gst_udpsrc) {
        LOGI("Using GStreamer udpsrc; UDP receiver stats disabled");
    }

    if (!build_video_branch(ps, pipeline, cfg)) {
        goto fail;
    }

    if (!build_audio_branch(ps, pipeline, cfg, audio_disabled)) {
        goto fail;
    }

    g_signal_connect(rtpbin, "pad-added", G_CALLBACK(rtpbin_pad_added_cb), ps);
    g_signal_connect(rtpbin, "pad-removed", G_CALLBACK(rtpbin_pad_removed_cb), ps);
    g_signal_connect(rtpbin, "request-pt-map", G_CALLBACK(rtpbin_request_pt_map_cb), ps);

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
    ps->audio_disabled = audio_disabled ? 1 : 0;
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
