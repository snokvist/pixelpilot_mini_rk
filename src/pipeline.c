#define _GNU_SOURCE

#include "pipeline.h"
#include "logging.h"

#include <errno.h>
#include <glib.h>
#include <gst/app/gstappsrc.h>
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

    g_object_set(appsrc_elem, "is-live", TRUE, "format", GST_FORMAT_BYTES, "stream-type",
                 GST_APP_STREAM_TYPE_STREAM, "max-bytes", (guint64)(4 * 1024 * 1024), NULL);

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

static GstCaps *make_rtp_caps(const char *media, int payload_type, int clock_rate, const char *encoding_name) {
    return gst_caps_new_simple("application/x-rtp", "media", G_TYPE_STRING, media, "payload", G_TYPE_INT,
                               payload_type, "clock-rate", G_TYPE_INT, clock_rate, "encoding-name", G_TYPE_STRING,
                               encoding_name, NULL);
}

static GstCaps *make_raw_audio_caps(void) {
    return gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE", "rate", G_TYPE_INT, 48000,
                               "channels", G_TYPE_INT, 2, NULL);
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

static gint get_pad_payload_type(GstPad *pad) {
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
        if (pad_name != NULL && g_str_has_prefix(pad_name, "src_")) {
            payload_type = (gint)g_ascii_strtoll(pad_name + 4, NULL, 10);
        }
    }

    return payload_type;
}

static gboolean link_pad_to_branch(GstPad *src_pad, GstElement *branch, GstPad **stored_pad, const char *name) {
    if (branch == NULL) {
        LOGE("Cannot link %s branch: branch element is NULL", name);
        return FALSE;
    }
    if (stored_pad != NULL && *stored_pad != NULL) {
        return TRUE;
    }

    GstPad *sink_pad = gst_element_get_static_pad(branch, "sink");
    if (sink_pad == NULL) {
        LOGE("Failed to get sink pad for %s branch", name);
        return FALSE;
    }

    GstPadLinkReturn link_ret = gst_pad_link(src_pad, sink_pad);
    gst_object_unref(sink_pad);
    if (link_ret != GST_PAD_LINK_OK) {
        LOGE("Failed to link demux pad to %s branch (ret=%d)", name, link_ret);
        return FALSE;
    }

    if (stored_pad != NULL) {
        *stored_pad = gst_object_ref(src_pad);
    }
    return TRUE;
}

static gboolean try_link_demux_pad(PipelineState *ps, GstPad *pad) {
    gint payload_type = get_pad_payload_type(pad);
    if (payload_type < 0) {
        LOGW("Ignoring demux pad with unknown payload type (pad=%s)", GST_OBJECT_NAME(pad));
        return FALSE;
    }

    if (payload_type == ps->cfg->vid_pt) {
        if (ps->video_branch_entry == NULL) {
            LOGW("Video branch not ready for payload type %d", payload_type);
            return FALSE;
        }
        if (ps->video_pad != NULL) {
            return TRUE;
        }
        if (link_pad_to_branch(pad, ps->video_branch_entry, &ps->video_pad, "video")) {
            LOGI("Linked demux video pad (PT=%d)", payload_type);
            return TRUE;
        }
        return FALSE;
    }

    if (payload_type == ps->cfg->aud_pt && ps->cfg->aud_pt >= 0) {
        if (ps->audio_branch_entry == NULL) {
            LOGW("Audio branch not ready for payload type %d", payload_type);
            return FALSE;
        }
        if (ps->audio_pad != NULL) {
            return TRUE;
        }
        if (link_pad_to_branch(pad, ps->audio_branch_entry, &ps->audio_pad, "audio")) {
            LOGI("Linked demux audio pad (PT=%d)", payload_type);
            return TRUE;
        }
        return FALSE;
    }

    LOGW("Unhandled demux pad with payload type %d", payload_type);
    return FALSE;
}

static void demux_pad_added_cb(GstElement *element, GstPad *pad, gpointer user_data) {
    PipelineState *ps = (PipelineState *)user_data;
    if (!try_link_demux_pad(ps, pad)) {
        LOGW("Failed to link newly added demux pad %s", GST_OBJECT_NAME(pad));
    }
}

static void demux_pad_removed_cb(GstElement *element, GstPad *pad, gpointer user_data) {
    PipelineState *ps = (PipelineState *)user_data;
    if (ps->video_pad == pad) {
        gst_object_unref(ps->video_pad);
        ps->video_pad = NULL;
        LOGI("Demux video pad removed");
    } else if (ps->audio_pad == pad) {
        gst_object_unref(ps->audio_pad);
        ps->audio_pad = NULL;
        LOGI("Demux audio pad removed");
    }
}

static GstCaps *demux_request_pt_map_cb(GstElement *element, guint payload_type, gpointer user_data) {
    PipelineState *ps = (PipelineState *)user_data;
    GstCaps *caps = caps_for_payload(ps, (gint)payload_type);
    if (caps == NULL) {
        LOGW("No caps mapping available for payload type %u", payload_type);
    }
    return caps;
}

static void connect_existing_demux_pads(PipelineState *ps) {
    GstIterator *it = gst_element_iterate_src_pads(ps->demux);
    if (it == NULL) {
        return;
    }

    GValue item = G_VALUE_INIT;
    gboolean done = FALSE;
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK: {
            GstPad *pad = g_value_get_object(&item);
            if (pad != NULL) {
                gst_object_ref(pad);
                try_link_demux_pad(ps, pad);
                gst_object_unref(pad);
            }
            g_value_reset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
            gst_iterator_resync(it);
            break;
        case GST_ITERATOR_ERROR:
        case GST_ITERATOR_DONE:
            done = TRUE;
            break;
        }
    }

    g_value_unset(&item);
    gst_iterator_free(it);
}

static void clear_stored_pad(GstPad **pad) {
    if (pad != NULL && *pad != NULL) {
        gst_object_unref(*pad);
        *pad = NULL;
    }
}

static gboolean build_video_branch(PipelineState *ps, GstElement *pipeline, GstElement *demux, const AppCfg *cfg) {
    GstElement *queue_pre = gst_element_factory_make("queue", "video_queue_pre");
    GstElement *caps_rtp = gst_element_factory_make("capsfilter", "video_caps_rtp");
    GstElement *jitter = gst_element_factory_make("rtpjitterbuffer", "video_jitter");
    GstElement *depay = gst_element_factory_make("rtph265depay", "video_depay");
    GstElement *parser = gst_element_factory_make("h265parse", "video_parser");
    GstElement *caps_stream = gst_element_factory_make("capsfilter", "video_caps_stream");
    GstElement *queue_post = gst_element_factory_make("queue", "video_queue_post");
    GstElement *decoder = gst_element_factory_make("mppvideodec", "video_decoder");
    GstElement *queue_sink = gst_element_factory_make("queue", "video_queue_sink");
    GstElement *sink = gst_element_factory_make("kmssink", "video_sink");

    CHECK_ELEM(queue_pre, "queue");
    CHECK_ELEM(caps_rtp, "capsfilter");
    CHECK_ELEM(jitter, "rtpjitterbuffer");
    CHECK_ELEM(depay, "rtph265depay");
    CHECK_ELEM(parser, "h265parse");
    CHECK_ELEM(caps_stream, "capsfilter");
    CHECK_ELEM(queue_post, "queue");
    CHECK_ELEM(decoder, "mppvideodec");
    CHECK_ELEM(queue_sink, "queue");
    CHECK_ELEM(sink, "kmssink");

    g_object_set(queue_pre, "leaky", cfg->video_queue_leaky, "max-size-buffers", cfg->video_queue_pre_buffers,
                 "max-size-time", (guint64)0, "max-size-bytes", (guint64)0, NULL);
    g_object_set(queue_post, "leaky", cfg->video_queue_leaky, "max-size-buffers", cfg->video_queue_post_buffers,
                 "max-size-time", (guint64)0, "max-size-bytes", (guint64)0, NULL);
    g_object_set(queue_sink, "leaky", cfg->video_queue_leaky, "max-size-buffers", cfg->video_queue_sink_buffers,
                 "max-size-time", (guint64)0, "max-size-bytes", (guint64)0, NULL);

    GstCaps *caps_rtp_cfg = make_rtp_caps("video", cfg->vid_pt, 90000, "H265");
    g_object_set(jitter, "latency", cfg->latency_ms, "drop-on-latency", cfg->video_drop_on_latency ? TRUE : FALSE, "do-lost", TRUE,
                 "post-drop-messages", TRUE, NULL);
    g_object_set(parser, "config-interval", -1, "disable-passthrough", TRUE, NULL);

    GstCaps *caps_stream_cfg =
        gst_caps_new_simple("video/x-h265", "stream-format", G_TYPE_STRING, "byte-stream", "alignment",
                             G_TYPE_STRING, "au", NULL);

    g_object_set(caps_rtp, "caps", caps_rtp_cfg, NULL);
    g_object_set(caps_stream, "caps", caps_stream_cfg, NULL);
    gst_caps_unref(caps_rtp_cfg);
    gst_caps_unref(caps_stream_cfg);

    g_object_set(sink, "plane-id", cfg->plane_id, "sync", cfg->kmssink_sync ? TRUE : FALSE, "qos",
                 cfg->kmssink_qos ? TRUE : FALSE, "max-lateness", (gint64)cfg->max_lateness_ns, NULL);

    gst_bin_add_many(GST_BIN(pipeline), queue_pre, caps_rtp, jitter, depay, parser, caps_stream, queue_post, decoder,
                     queue_sink, sink, NULL);

    if (!gst_element_link_many(queue_pre, caps_rtp, jitter, depay, parser, caps_stream, queue_post, decoder,
                               queue_sink, sink, NULL)) {
        LOGE("Failed to link video branch");
        return FALSE;
    }

    ps->video_branch_entry = queue_pre;
    ps->video_sink = sink;
    return TRUE;

fail:
    ps->video_branch_entry = NULL;
    return FALSE;
}

static gboolean build_audio_branch(PipelineState *ps, GstElement *pipeline, GstElement *demux, const AppCfg *cfg,
                                   int audio_disabled) {
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

    GstElement *caps_rtp = gst_element_factory_make("capsfilter", "audio_caps_rtp");
    GstElement *jitter = gst_element_factory_make("rtpjitterbuffer", "audio_jitter");
    GstElement *depay = gst_element_factory_make("rtpopusdepay", "audio_depay");
    GstElement *decoder = gst_element_factory_make("opusdec", "audio_decoder");
    GstElement *aconv = gst_element_factory_make("audioconvert", "audio_convert");
    GstElement *ares = gst_element_factory_make("audioresample", "audio_resample");
    GstElement *caps_raw = gst_element_factory_make("capsfilter", "audio_caps_raw");
    GstElement *queue_sink = gst_element_factory_make("queue", "audio_queue_sink");
    GstElement *alsa = gst_element_factory_make("alsasink", "audio_sink");

    CHECK_ELEM(caps_rtp, "capsfilter");
    CHECK_ELEM(jitter, "rtpjitterbuffer");
    CHECK_ELEM(depay, "rtpopusdepay");
    CHECK_ELEM(decoder, "opusdec");
    CHECK_ELEM(aconv, "audioconvert");
    CHECK_ELEM(ares, "audioresample");
    CHECK_ELEM(caps_raw, "capsfilter");
    CHECK_ELEM(queue_sink, "queue");
    CHECK_ELEM(alsa, "alsasink");

    g_object_set(jitter, "latency", cfg->latency_ms, "drop-on-latency", TRUE, "do-lost", TRUE, NULL);
    GstCaps *caps_rtp_cfg = make_rtp_caps("audio", cfg->aud_pt, 48000, "OPUS");
    GstCaps *caps_raw_cfg = make_raw_audio_caps();
    g_object_set(caps_rtp, "caps", caps_rtp_cfg, NULL);
    g_object_set(caps_raw, "caps", caps_raw_cfg, NULL);
    gst_caps_unref(caps_rtp_cfg);
    gst_caps_unref(caps_raw_cfg);
    g_object_set(queue_sink, "leaky", 2, NULL);
    g_object_set(alsa, "device", cfg->aud_dev, "sync", FALSE, NULL);

    gst_bin_add_many(GST_BIN(pipeline), queue_start, caps_rtp, jitter, depay, decoder, aconv, ares, caps_raw,
                     queue_sink, alsa, NULL);

    if (!gst_element_link_many(queue_start, caps_rtp, jitter, depay, decoder, aconv, ares, caps_raw, queue_sink, alsa,
                               NULL)) {
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
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *structure = gst_message_get_structure(msg);
            if (structure != NULL && gst_structure_has_name(structure, "drop-msg")) {
                guint seqnum = 0;
                guint num_too_late = 0;
                guint num_drop_on_latency = 0;
                GstClockTime timestamp = GST_CLOCK_TIME_NONE;
                const gchar *reason = gst_structure_get_string(structure, "reason");
                gst_structure_get_uint(structure, "seqnum", &seqnum);
                gst_structure_get_uint(structure, "num-too-late", &num_too_late);
                gst_structure_get_uint(structure, "num-drop-on-latency", &num_drop_on_latency);
                gst_structure_get_clock_time(structure, "timestamp", &timestamp);

                if (ps->udp_receiver != NULL) {
                    udp_receiver_record_pipeline_drop(ps->udp_receiver, seqnum, (guint64)timestamp, reason,
                                                      num_too_late, num_drop_on_latency);
                }

                if (num_too_late > 0 || num_drop_on_latency > 0) {
                    const gchar *src_name = GST_OBJECT_NAME(GST_MESSAGE_SRC(msg));
                    unsigned long long total_late = 0;
                    unsigned long long total_latency = 0;
                    if (ps->udp_receiver != NULL) {
                        UdpReceiverStats stats_snapshot;
                        udp_receiver_get_stats(ps->udp_receiver, &stats_snapshot);
                        total_late = (unsigned long long)stats_snapshot.pipeline_dropped_too_late;
                        total_latency = (unsigned long long)stats_snapshot.pipeline_dropped_on_latency;
                    }
                    LOGW("rtpjitterbuffer drop (%s): reason=%s seq=%u late=%u latency=%u totals late=%llu latency=%llu",
                         src_name != NULL ? src_name : "unknown", reason != NULL ? reason : "unknown", seqnum,
                         num_too_late, num_drop_on_latency, total_late, total_latency);
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
    if (ps->udp_receiver != NULL) {
        udp_receiver_destroy(ps->udp_receiver);
        ps->udp_receiver = NULL;
    }
    if (ps->demux != NULL) {
        g_signal_handlers_disconnect_by_data(ps->demux, ps);
    }
    clear_stored_pad(&ps->video_pad);
    clear_stored_pad(&ps->audio_pad);
    ps->video_branch_entry = NULL;
    ps->audio_branch_entry = NULL;
    ps->video_sink = NULL;
    ps->demux = NULL;
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

int pipeline_start(const AppCfg *cfg, int audio_disabled, PipelineState *ps) {
    if (ps->state != PIPELINE_STOPPED) {
        LOGW("pipeline_start: refused (state=%d)", ps->state);
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
    ps->demux = NULL;
    ps->video_branch_entry = NULL;
    ps->audio_branch_entry = NULL;
    ps->video_pad = NULL;
    ps->audio_pad = NULL;
    ps->udp_receiver = NULL;
    ps->cfg = cfg;
    ps->bus_thread_cpu_slot = 0;

    UdpReceiver *receiver = NULL;
    GstElement *source = create_udp_app_source(cfg, &receiver);
    if (source == NULL) {
        goto fail;
    }
    GstElement *queue_ingress = gst_element_factory_make("queue", "udp_ingress_queue");
    CHECK_ELEM(queue_ingress, "queue");
    g_object_set(queue_ingress, "leaky", 2, "max-size-buffers", 0, "max-size-bytes", (guint64)0, "max-size-time",
                 (guint64)0, NULL);

    GstElement *demux = gst_element_factory_make("rtpptdemux", "rtp_demux");
    CHECK_ELEM(demux, "rtpptdemux");

    gst_bin_add_many(GST_BIN(pipeline), source, queue_ingress, demux, NULL);
    if (!gst_element_link_many(source, queue_ingress, demux, NULL)) {
        LOGE("Failed to link source chain to demux");
        goto fail;
    }

    ps->source = source;
    ps->demux = demux;
    ps->udp_receiver = receiver;

    if (!build_video_branch(ps, pipeline, demux, cfg)) {
        goto fail;
    }

    if (!build_audio_branch(ps, pipeline, demux, cfg, audio_disabled)) {
        goto fail;
    }

    g_signal_connect(demux, "pad-added", G_CALLBACK(demux_pad_added_cb), ps);
    g_signal_connect(demux, "pad-removed", G_CALLBACK(demux_pad_removed_cb), ps);
    g_signal_connect(demux, "request-pt-map", G_CALLBACK(demux_request_pt_map_cb), ps);
    connect_existing_demux_pads(ps);

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
