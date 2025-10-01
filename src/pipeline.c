#define _GNU_SOURCE

#include "pipeline.h"
#include "logging.h"

#include <gst/gst.h>
#include <stdlib.h>
#include <string.h>

#define CHECK_ELEM(elem, name)                                                                    \
    do {                                                                                          \
        if ((elem) == NULL) {                                                                     \
            LOGE("Failed to create GStreamer element '%s'", (name));                             \
            goto fail;                                                                            \
        }                                                                                         \
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

static void cleanup_pipeline(PipelineState *ps) {
    if (!ps) {
        return;
    }
    if (ps->pipeline != NULL) {
        gst_object_unref(ps->pipeline);
        ps->pipeline = NULL;
    }
    ps->rtpbin = NULL;
    ps->video_queue = NULL;
    ps->audio_queue = NULL;
    ps->audio_sink = NULL;
}

static GstCaps *caps_for_payload(const PipelineState *ps, guint pt) {
    if (!ps || !ps->cfg) {
        return NULL;
    }

    const AppCfg *cfg = ps->cfg;
    if (pt == (guint)cfg->vid_pt) {
        return gst_caps_new_simple("application/x-rtp", "media", G_TYPE_STRING, "video", "encoding-name",
                                   G_TYPE_STRING, "H265", "clock-rate", G_TYPE_INT, 90000, NULL);
    }

    if (cfg->aud_pt >= 0 && pt == (guint)cfg->aud_pt) {
        return gst_caps_new_simple("application/x-rtp", "media", G_TYPE_STRING, "audio", "encoding-name",
                                   G_TYPE_STRING, "OPUS", "clock-rate", G_TYPE_INT, 48000, NULL);
    }

    return NULL;
}

static GstCaps *on_request_pt_map(GstElement *rtpbin, guint session, guint pt, gpointer user_data) {
    (void)rtpbin;
    (void)session;
    PipelineState *ps = (PipelineState *)user_data;
    GstCaps *caps = caps_for_payload(ps, pt);
    if (!caps) {
        LOGW("[pt-map] Unknown payload type %u", pt);
    }
    return caps;
}

static void try_link_pad(GstElement *queue, GstPad *newpad, const char *media) {
    if (!queue || !newpad) {
        return;
    }
    GstPad *sinkpad = gst_element_get_static_pad(queue, "sink");
    if (!sinkpad) {
        LOGE("[link] Failed to get sink pad for %s branch", media);
        return;
    }
    if (!gst_pad_is_linked(sinkpad)) {
        if (gst_pad_link(newpad, sinkpad) == GST_PAD_LINK_OK) {
            LOGI("[link] %s pad linked", media);
        } else {
            LOGE("[link] Failed to link %s pad", media);
        }
    }
    gst_object_unref(sinkpad);
}

static void on_pad_added(GstElement *rtpbin, GstPad *newpad, gpointer user_data) {
    (void)rtpbin;
    PipelineState *ps = (PipelineState *)user_data;
    GstCaps *caps = gst_pad_get_current_caps(newpad);
    const GstStructure *s = caps ? gst_caps_get_structure(caps, 0) : NULL;
    const gchar *media = s ? gst_structure_get_string(s, "media") : NULL;

    if (media && g_str_equal(media, "video")) {
        try_link_pad(ps->video_queue, newpad, "video");
    } else if (media && g_str_equal(media, "audio")) {
        try_link_pad(ps->audio_queue, newpad, "audio");
    } else {
        LOGW("[link] Unknown media on new pad");
    }

    if (caps) {
        gst_caps_unref(caps);
    }
}

static gpointer bus_thread_func(gpointer data) {
    PipelineState *ps = (PipelineState *)data;
    GstBus *bus = NULL;

    if (!ps || !ps->pipeline) {
        goto done;
    }

    bus = gst_element_get_bus(ps->pipeline);
    if (!bus) {
        LOGE("Failed to get pipeline bus");
        goto done;
    }

    while (TRUE) {
        GstMessage *msg = gst_bus_timed_pop(bus, GST_MSECOND * 100);
        if (!msg) {
            g_mutex_lock(&ps->lock);
            gboolean stop = ps->stop_requested;
            g_mutex_unlock(&ps->lock);
            if (stop) {
                break;
            }
            continue;
        }

        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            LOGE("Pipeline error: %s (debug=%s)", err ? err->message : "unknown", dbg ? dbg : "none");
            if (err) {
                g_error_free(err);
            }
            g_free(dbg);
            g_mutex_lock(&ps->lock);
            ps->encountered_error = TRUE;
            ps->stop_requested = TRUE;
            g_mutex_unlock(&ps->lock);
            gst_message_unref(msg);
            goto done;
        }
        case GST_MESSAGE_EOS:
            LOGI("Pipeline reached EOS");
            g_mutex_lock(&ps->lock);
            ps->stop_requested = TRUE;
            g_mutex_unlock(&ps->lock);
            gst_message_unref(msg);
            goto done;
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(ps->pipeline)) {
                GstState old_state, new_state, pending;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending);
                LOGV("Pipeline state changed: %s -> %s", gst_element_state_get_name(old_state),
                     gst_element_state_get_name(new_state));
            }
            break;
        default:
            break;
        }
        gst_message_unref(msg);

        g_mutex_lock(&ps->lock);
        gboolean stop = ps->stop_requested;
        g_mutex_unlock(&ps->lock);
        if (stop) {
            break;
        }
    }

done:
    if (bus) {
        gst_object_unref(bus);
    }
    g_mutex_lock(&ps->lock);
    ps->bus_thread_running = FALSE;
    g_cond_signal(&ps->cond);
    g_mutex_unlock(&ps->lock);
    return NULL;
}

int pipeline_start(const AppCfg *cfg, int audio_disabled, PipelineState *ps) {
    if (!cfg || !ps) {
        return -1;
    }

    ensure_gst_initialized(cfg);

    if (!ps->initialized) {
        g_mutex_init(&ps->lock);
        g_cond_init(&ps->cond);
        ps->initialized = TRUE;
    }

    if (ps->state != PIPELINE_STOPPED) {
        pipeline_stop(ps, 500);
    }

    GstElement *pipeline = NULL;
    GstElement *udpsrc = NULL;
    GstElement *rtpbin = NULL;
    GstElement *v_queue = NULL;
    GstElement *v_depay = NULL;
    GstElement *v_parse = NULL;
    GstElement *v_dec = NULL;
    GstElement *v_postq = NULL;
    GstElement *v_sink = NULL;
    GstElement *a_queue = NULL;
    GstElement *a_depay = NULL;
    GstElement *a_dec = NULL;
    GstElement *a_conv = NULL;
    GstElement *a_res = NULL;
    GstElement *a_sink = NULL;
    GstCaps *rtp_caps = NULL;
    GstPad *udp_src_pad = NULL;
    GstPad *rtp_sink_pad = NULL;

    gboolean disable_audio = cfg->no_audio || audio_disabled || cfg->aud_pt < 0;

    pipeline = gst_pipeline_new("p");
    CHECK_ELEM(pipeline, "pipeline");

    udpsrc = gst_element_factory_make("udpsrc", "src");
    CHECK_ELEM(udpsrc, "udpsrc");
    g_object_set(udpsrc, "port", cfg->udp_port, "buffer-size", 262144, NULL);
    rtp_caps = gst_caps_from_string("application/x-rtp");
    if (!rtp_caps) {
        LOGE("Failed to create RTP caps");
        goto fail;
    }
    g_object_set(udpsrc, "caps", rtp_caps, NULL);

    rtpbin = gst_element_factory_make("rtpbin", "rtpbin");
    CHECK_ELEM(rtpbin, "rtpbin");
    g_object_set(rtpbin, "latency", cfg->latency_ms, NULL);

    v_queue = gst_element_factory_make("queue", "v_q");
    v_depay = gst_element_factory_make("rtph265depay", "v_depay");
    v_parse = gst_element_factory_make("h265parse", "v_parse");
    v_dec = gst_element_factory_make("mppvideodec", "v_dec");
    v_postq = gst_element_factory_make("queue", "v_postq");
    v_sink = gst_element_factory_make("kmssink", "v_sink");
    CHECK_ELEM(v_queue, "queue");
    CHECK_ELEM(v_depay, "rtph265depay");
    CHECK_ELEM(v_parse, "h265parse");
    CHECK_ELEM(v_dec, "mppvideodec");
    CHECK_ELEM(v_postq, "queue");
    CHECK_ELEM(v_sink, "kmssink");

    g_object_set(v_queue, "leaky", 1, "max-size-buffers", 96, "max-size-time", (guint64)0, "max-size-bytes",
                 (guint64)0, NULL);
    g_object_set(v_parse, "config-interval", -1, "disable-passthrough", TRUE, NULL);
    g_object_set(v_postq, "leaky", 1, "max-size-buffers", 4, "max-size-time", (guint64)0, "max-size-bytes",
                 (guint64)0, NULL);
    g_object_set(v_sink, "plane-id", cfg->plane_id, "sync", TRUE, NULL);

    a_queue = gst_element_factory_make("queue", "a_q");
    CHECK_ELEM(a_queue, "queue");
    g_object_set(a_queue, "leaky", 1, "max-size-buffers", 96, "max-size-time", (guint64)0, "max-size-bytes",
                 (guint64)0, NULL);

    if (!disable_audio) {
        a_depay = gst_element_factory_make("rtpopusdepay", "a_depay");
        a_dec = gst_element_factory_make("opusdec", "a_dec");
        a_conv = gst_element_factory_make("audioconvert", "a_conv");
        a_res = gst_element_factory_make("audioresample", "a_res");
        a_sink = gst_element_factory_make("alsasink", "a_sink");
        CHECK_ELEM(a_depay, "rtpopusdepay");
        CHECK_ELEM(a_dec, "opusdec");
        CHECK_ELEM(a_conv, "audioconvert");
        CHECK_ELEM(a_res, "audioresample");
        CHECK_ELEM(a_sink, "alsasink");
        if (cfg->aud_dev[0] != '\0') {
            g_object_set(a_sink, "device", cfg->aud_dev, NULL);
        }
        g_object_set(a_sink, "sync", FALSE, "async", FALSE, NULL);
    } else {
        a_sink = gst_element_factory_make("fakesink", "a_sink");
        CHECK_ELEM(a_sink, "fakesink");
        g_object_set(a_sink, "sync", FALSE, NULL);
    }

    gst_bin_add_many(GST_BIN(pipeline), udpsrc, rtpbin, v_queue, v_depay, v_parse, v_dec, v_postq, v_sink, a_queue,
                     NULL);
    if (!disable_audio) {
        gst_bin_add_many(GST_BIN(pipeline), a_depay, a_dec, a_conv, a_res, a_sink, NULL);
    } else {
        gst_bin_add(GST_BIN(pipeline), a_sink);
    }

    if (!gst_element_link_many(v_queue, v_depay, v_parse, v_dec, v_postq, v_sink, NULL)) {
        LOGE("Failed to link video branch");
        goto fail;
    }

    if (!disable_audio) {
        if (!gst_element_link_many(a_queue, a_depay, a_dec, a_conv, a_res, a_sink, NULL)) {
            LOGE("Failed to link audio branch");
            goto fail;
        }
    } else {
        if (!gst_element_link(a_queue, a_sink)) {
            LOGE("Failed to link audio fakesink branch");
            goto fail;
        }
    }

    rtp_sink_pad = gst_element_get_request_pad(rtpbin, "recv_rtp_sink_0");
    udp_src_pad = gst_element_get_static_pad(udpsrc, "src");
    if (!rtp_sink_pad || !udp_src_pad) {
        LOGE("Failed to get pads for udpsrc->rtpbin link");
        goto fail;
    }
    if (gst_pad_link(udp_src_pad, rtp_sink_pad) != GST_PAD_LINK_OK) {
        LOGE("Failed to link udpsrc to rtpbin");
        goto fail;
    }

    ps->cfg = cfg;
    ps->pipeline = pipeline;
    ps->rtpbin = rtpbin;
    ps->video_queue = v_queue;
    ps->audio_queue = a_queue;
    ps->audio_sink = a_sink;
    ps->audio_disabled = disable_audio;

    g_signal_connect(rtpbin, "request-pt-map", G_CALLBACK(on_request_pt_map), ps);
    g_signal_connect(rtpbin, "pad-added", G_CALLBACK(on_pad_added), ps);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        LOGE("Failed to set pipeline to PLAYING");
        goto fail;
    }

    ps->stop_requested = FALSE;
    ps->encountered_error = FALSE;
    ps->state = PIPELINE_RUNNING;

    g_mutex_lock(&ps->lock);
    ps->bus_thread_running = TRUE;
    g_mutex_unlock(&ps->lock);

    ps->bus_thread = g_thread_new("gst-bus", bus_thread_func, ps);
    if (!ps->bus_thread) {
        LOGE("Failed to start bus thread");
        goto fail;
    }

    if (udp_src_pad) {
        gst_object_unref(udp_src_pad);
    }
    if (rtp_sink_pad) {
        gst_object_unref(rtp_sink_pad);
    }
    if (rtp_caps) {
        gst_caps_unref(rtp_caps);
    }
    return 0;

fail:
    if (udp_src_pad) {
        gst_object_unref(udp_src_pad);
    }
    if (rtp_sink_pad) {
        gst_object_unref(rtp_sink_pad);
    }
    if (rtp_caps) {
        gst_caps_unref(rtp_caps);
    }
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }
    ps->pipeline = NULL;
    ps->rtpbin = NULL;
    ps->video_queue = NULL;
    ps->audio_queue = NULL;
    ps->audio_sink = NULL;
    ps->state = PIPELINE_STOPPED;
    ps->bus_thread_running = FALSE;
    if (ps->bus_thread) {
        g_thread_unref(ps->bus_thread);
        ps->bus_thread = NULL;
    }
    return -1;
}

void pipeline_stop(PipelineState *ps, int wait_ms_total) {
    if (!ps || ps->state == PIPELINE_STOPPED) {
        return;
    }

    g_mutex_lock(&ps->lock);
    ps->state = PIPELINE_STOPPING;
    ps->stop_requested = TRUE;
    g_mutex_unlock(&ps->lock);

    if (ps->pipeline) {
        gst_element_send_event(ps->pipeline, gst_event_new_eos());
        gst_element_set_state(ps->pipeline, GST_STATE_NULL);
    }

    if (ps->bus_thread) {
        if (wait_ms_total > 0) {
            gint64 deadline = g_get_monotonic_time() + (gint64)wait_ms_total * G_TIME_SPAN_MILLISECOND;
            g_mutex_lock(&ps->lock);
            while (ps->bus_thread_running) {
                if (!g_cond_wait_until(&ps->cond, &ps->lock, deadline)) {
                    break;
                }
            }
            g_mutex_unlock(&ps->lock);
        }
        g_thread_join(ps->bus_thread);
        ps->bus_thread = NULL;
    }

    cleanup_pipeline(ps);
    g_mutex_lock(&ps->lock);
    ps->stop_requested = FALSE;
    ps->encountered_error = FALSE;
    ps->bus_thread_running = FALSE;
    g_mutex_unlock(&ps->lock);
    ps->state = PIPELINE_STOPPED;
}

void pipeline_poll_child(PipelineState *ps) {
    if (!ps || !ps->bus_thread) {
        return;
    }

    g_mutex_lock(&ps->lock);
    gboolean running = ps->bus_thread_running;
    gboolean had_error = ps->encountered_error;
    g_mutex_unlock(&ps->lock);

    if (!running) {
        g_thread_join(ps->bus_thread);
        ps->bus_thread = NULL;
        if (ps->pipeline) {
            gst_element_set_state(ps->pipeline, GST_STATE_NULL);
        }
        cleanup_pipeline(ps);
        g_mutex_lock(&ps->lock);
        ps->stop_requested = FALSE;
        ps->bus_thread_running = FALSE;
        ps->encountered_error = had_error;
        g_mutex_unlock(&ps->lock);
        ps->state = PIPELINE_STOPPED;
        if (had_error) {
            LOGI("Pipeline exited due to error");
        } else {
            LOGI("Pipeline exited cleanly");
        }
    }
}

void pipeline_set_receiver_stats_enabled(PipelineState *ps, gboolean enabled) {
    (void)ps;
    (void)enabled;
}

int pipeline_get_receiver_stats(const PipelineState *ps, UdpReceiverStats *stats) {
    (void)ps;
    (void)stats;
    return -1;
}
