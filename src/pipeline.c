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

static GstCaps *build_appsrc_caps(const AppCfg *cfg) {
    GstCaps *caps = gst_caps_new_empty();
    if (caps == NULL) {
        return NULL;
    }

    GstStructure *video = gst_structure_new("application/x-rtp", "media", G_TYPE_STRING, "video", "payload",
                                            G_TYPE_INT, cfg->vid_pt, "clock-rate", G_TYPE_INT, 90000, "encoding-name",
                                            G_TYPE_STRING, "H265", NULL);
    if (video == NULL) {
        gst_caps_unref(caps);
        return NULL;
    }
    gst_caps_append_structure(caps, video);

    if (cfg->aud_pt >= 0) {
        GstStructure *audio =
            gst_structure_new("application/x-rtp", "media", G_TYPE_STRING, "audio", "payload", G_TYPE_INT,
                              cfg->aud_pt, "clock-rate", G_TYPE_INT, 48000, "encoding-name", G_TYPE_STRING, "OPUS",
                              NULL);
        if (audio == NULL) {
            gst_caps_unref(caps);
            return NULL;
        }
        gst_caps_append_structure(caps, audio);
    }

    return caps;
}

static GstElement *create_udp_app_source(const AppCfg *cfg, UdpReceiver **receiver_out) {
    GstElement *appsrc_elem = gst_element_factory_make("appsrc", "udp_appsrc");
    UdpReceiver *receiver = NULL;
    GstCaps *caps = NULL;
    CHECK_ELEM(appsrc_elem, "appsrc");

    caps = build_appsrc_caps(cfg);
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

static GstCaps *make_rtp_caps(int payload_type, int clock_rate, const char *encoding_name) {
    return gst_caps_new_simple("application/x-rtp", "payload", G_TYPE_INT, payload_type, "clock-rate",
                               G_TYPE_INT, clock_rate, "encoding-name", G_TYPE_STRING, encoding_name, NULL);
}

static GstCaps *make_raw_audio_caps(void) {
    return gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE", "rate", G_TYPE_INT, 48000,
                               "channels", G_TYPE_INT, 2, NULL);
}

static gboolean link_demux_branch(GstElement *demux, GstElement *branch, GstPad **stored_src_pad, const char *name,
                                 gint payload_type) {
    if (payload_type < 0) {
        LOGE("Invalid payload type for %s branch", name);
        return FALSE;
    }
    GstPad *src_pad = gst_element_get_request_pad(demux, "src_%u");
    if (src_pad == NULL) {
        LOGE("Failed to request demux pad for %s branch", name);
        return FALSE;
    }
    g_object_set(src_pad, "pt", payload_type, NULL);

    GstPad *sink_pad = gst_element_get_static_pad(branch, "sink");
    if (sink_pad == NULL) {
        LOGE("Failed to get sink pad for %s branch", name);
        gst_element_release_request_pad(demux, src_pad);
        gst_object_unref(src_pad);
        return FALSE;
    }
    if (gst_pad_link(src_pad, sink_pad) != GST_PAD_LINK_OK) {
        LOGE("Failed to link demux to %s branch", name);
        gst_object_unref(sink_pad);
        gst_element_release_request_pad(demux, src_pad);
        gst_object_unref(src_pad);
        return FALSE;
    }
    gst_object_unref(sink_pad);
    if (stored_src_pad != NULL) {
        *stored_src_pad = src_pad;
    } else {
        gst_element_release_request_pad(demux, src_pad);
        gst_object_unref(src_pad);
    }
    return TRUE;
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

    GstCaps *caps_rtp_cfg = make_rtp_caps(cfg->vid_pt, 90000, "H265");
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

    if (!link_demux_branch(demux, queue_pre, &ps->video_pad, "video", cfg->vid_pt)) {
        return FALSE;
    }

    ps->video_sink = sink;
    return TRUE;

fail:
    return FALSE;
}

static gboolean build_audio_branch(PipelineState *ps, GstElement *pipeline, GstElement *demux, const AppCfg *cfg,
                                   int audio_disabled) {
    if (cfg->no_audio) {
        return TRUE;
    }

    GstElement *queue_start = gst_element_factory_make("queue", "audio_queue_start");
    CHECK_ELEM(queue_start, "queue");
    g_object_set(queue_start, "leaky", 2, "max-size-time", (guint64)0, "max-size-bytes", (guint64)0, NULL);

    if (audio_disabled) {
        GstElement *fakesink = gst_element_factory_make("fakesink", "audio_fakesink");
        CHECK_ELEM(fakesink, "fakesink");
        g_object_set(fakesink, "sync", FALSE, NULL);

        gst_bin_add_many(GST_BIN(pipeline), queue_start, fakesink, NULL);
        if (!gst_element_link(queue_start, fakesink)) {
            LOGE("Failed to link audio fakesink branch");
            return FALSE;
        }
        if (!link_demux_branch(demux, queue_start, &ps->audio_pad, "audio", cfg->aud_pt)) {
            return FALSE;
        }
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
    GstCaps *caps_rtp_cfg = make_rtp_caps(cfg->aud_pt, 48000, "OPUS");
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

    if (!link_demux_branch(demux, queue_start, &ps->audio_pad, "audio", cfg->aud_pt)) {
        return FALSE;
    }

    ps->audio_disabled = 0;
    return TRUE;

fail:
    return FALSE;
}

static void release_request_pad(GstElement *demux, GstPad **pad) {
    if (demux != NULL && pad != NULL && *pad != NULL) {
        gst_element_release_request_pad(demux, *pad);
        gst_object_unref(*pad);
        *pad = NULL;
    }
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
    release_request_pad(ps->demux, &ps->video_pad);
    release_request_pad(ps->demux, &ps->audio_pad);
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
