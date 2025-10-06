#define _GNU_SOURCE

#include "pipeline.h"
#include "logging.h"
#include "splashlib.h"
#include "recorder.h"

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

static void release_selector_pad(GstElement *selector, GstPad **pad_slot) {
    if (pad_slot == NULL || *pad_slot == NULL) {
        return;
    }

    GstPad *pad = *pad_slot;
    if (selector != NULL) {
        gst_element_release_request_pad(selector, pad);
    }

    gst_object_unref(pad);
    *pad_slot = NULL;
}

// Simple context for filtering RTP payload types when udpsrc is active. Any
// packet whose payload type does not match the configured video PT is dropped
// before it reaches the depayloader so audio bursts do not flood the log.
typedef struct {
    gint video_pt;
} UdpsrcPadFilterCtx;

static GstPadProbeReturn udpsrc_pad_filter_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    UdpsrcPadFilterCtx *ctx = (UdpsrcPadFilterCtx *)user_data;
    if (ctx == NULL || info == NULL) {
        return GST_PAD_PROBE_OK;
    }

    if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
        return GST_PAD_PROBE_OK;
    }

    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer == NULL) {
        return GST_PAD_PROBE_OK;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        return GST_PAD_PROBE_OK;
    }

    gboolean drop = TRUE;
    if (ctx->video_pt < 0 || ctx->video_pt > 127) {
        drop = FALSE;
    } else if (map.size >= 2) {
        guint8 payload_type = map.data[1] & 0x7Fu;
        if ((gint)payload_type == ctx->video_pt) {
            drop = FALSE;
        }
    }

    gst_buffer_unmap(buffer, &map);

    return drop ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
}

static guint64 monotonic_time_ns(void) {
    return (guint64)g_get_monotonic_time() * 1000ull;
}

static gpointer splash_loop_thread_func(gpointer data) {
    PipelineState *ps = (PipelineState *)data;
    Splash *splash = NULL;

    g_mutex_lock(&ps->lock);
    ps->splash_loop_running = TRUE;
    splash = ps->splash;
    g_mutex_unlock(&ps->lock);

    if (splash != NULL) {
        splash_run(splash);
    }

    g_mutex_lock(&ps->lock);
    ps->splash_loop_running = FALSE;
    g_mutex_unlock(&ps->lock);
    return NULL;
}

static gboolean pipeline_prepare_splash(PipelineState *ps,
                                        const AppCfg *cfg,
                                        GstElement *pipeline,
                                        GstElement *selector) {
    ps->splash_available = FALSE;
    if (ps == NULL || cfg == NULL || pipeline == NULL || selector == NULL) {
        return FALSE;
    }

    if (!cfg->splash.enable) {
        return TRUE;
    }
    if (cfg->splash.sequence_count <= 0 || cfg->splash.input_path[0] == '\0') {
        LOGW("Splash fallback enabled but missing input or sequences; disabling");
        return TRUE;
    }

    Splash *splash = splash_new();
    if (splash == NULL) {
        LOGE("Failed to allocate splash player state");
        return FALSE;
    }
    ps->splash = splash;

    GstElement *splash_appsrc = NULL;
    GstElement *splash_queue = NULL;
    GstPad *splash_sink_pad = NULL;

    int seq_count = cfg->splash.sequence_count;
    if (seq_count > SPLASH_MAX_SEQUENCES) {
        seq_count = SPLASH_MAX_SEQUENCES;
    }

    SplashSeq seqs[SPLASH_MAX_SEQUENCES];
    for (int i = 0; i < seq_count; ++i) {
        seqs[i].name = cfg->splash.sequences[i].name;
        seqs[i].start_frame = cfg->splash.sequences[i].start_frame;
        seqs[i].end_frame = cfg->splash.sequences[i].end_frame;
    }

    if (!splash_set_sequences(splash, seqs, seq_count)) {
        LOGE("Failed to load splash sequences");
        goto fail;
    }

    double fps = cfg->splash.fps > 0.0 ? cfg->splash.fps : 30.0;
    SplashConfig splash_cfg = {0};
    splash_cfg.input_path = cfg->splash.input_path;
    splash_cfg.fps = fps;
    splash_cfg.outputs = SPLASH_OUTPUT_APPSRC;

    if (!splash_apply_config(splash, &splash_cfg)) {
        LOGE("Failed to configure splash playback");
        goto fail;
    }

    splash_clear_next(splash);
    int order[SPLASH_MAX_SEQUENCES];
    int order_count = 0;
    int start_index = 0;
    if (cfg->splash.default_sequence[0] != '\0') {
        int idx = splash_find_index_by_name(splash, cfg->splash.default_sequence);
        if (idx >= 0) {
            start_index = idx;
        } else {
            LOGW("Splash default-sequence '%s' not found; starting from first sequence", cfg->splash.default_sequence);
        }
    }
    for (int i = 0; i < seq_count; ++i) {
        int idx = (start_index + i) % seq_count;
        order[order_count++] = idx;
    }
    if (order_count > 0) {
        SplashRepeatMode repeat_mode = order_count > 1 ? SPLASH_REPEAT_FULL : SPLASH_REPEAT_LAST;
        if (!splash_enqueue_with_repeat(splash, order, order_count, repeat_mode)) {
            LOGW("Failed to configure splash repeat order; disabling repeat loop");
            splash_set_repeat_order(splash, NULL, 0);
        }
    }

    splash_appsrc = splash_get_appsrc(splash);
    if (splash_appsrc == NULL) {
        LOGE("Failed to obtain splash appsrc element");
        goto fail;
    }

    splash_queue = gst_element_factory_make("queue", "splash_queue");
    if (splash_queue == NULL) {
        LOGE("Failed to create splash queue element");
        gst_object_unref(splash_appsrc);
        goto fail;
    }

    g_object_set(splash_queue, "leaky", 2, "max-size-buffers", 16, "max-size-bytes", (guint64)0,
                 "max-size-time", (guint64)0, NULL);

    gst_bin_add_many(GST_BIN(pipeline), splash_appsrc, splash_queue, NULL);
    if (!gst_element_link(splash_appsrc, splash_queue)) {
        LOGE("Failed to link splash appsrc to queue");
        gst_bin_remove(GST_BIN(pipeline), splash_queue);
        gst_bin_remove(GST_BIN(pipeline), splash_appsrc);
        splash_queue = NULL;
        splash_appsrc = NULL;
        goto fail;
    }

    splash_sink_pad = gst_element_get_request_pad(selector, "sink_%u");
    if (splash_sink_pad == NULL) {
        LOGE("Failed to request selector pad for splash branch");
        gst_bin_remove(GST_BIN(pipeline), splash_queue);
        gst_bin_remove(GST_BIN(pipeline), splash_appsrc);
        splash_queue = NULL;
        splash_appsrc = NULL;
        goto fail;
    }

    GstPad *queue_src = gst_element_get_static_pad(splash_queue, "src");
    if (queue_src == NULL) {
        LOGE("Failed to get splash queue src pad");
        release_selector_pad(selector, &splash_sink_pad);
        gst_bin_remove(GST_BIN(pipeline), splash_queue);
        gst_bin_remove(GST_BIN(pipeline), splash_appsrc);
        splash_queue = NULL;
        splash_appsrc = NULL;
        goto fail;
    }

    if (gst_pad_link(queue_src, splash_sink_pad) != GST_PAD_LINK_OK) {
        LOGE("Failed to link splash queue into selector");
        gst_object_unref(queue_src);
        release_selector_pad(selector, &splash_sink_pad);
        gst_bin_remove(GST_BIN(pipeline), splash_queue);
        gst_bin_remove(GST_BIN(pipeline), splash_appsrc);
        splash_queue = NULL;
        splash_appsrc = NULL;
        goto fail;
    }
    gst_object_unref(queue_src);

    ps->selector_splash_pad = splash_sink_pad;
    splash_sink_pad = NULL;

    ps->splash_loop_thread = g_thread_new("splash-loop", splash_loop_thread_func, ps);
    if (ps->splash_loop_thread == NULL) {
        LOGE("Failed to start splash main loop thread");
        goto fail_thread;
    }

    if (!splash_start(splash)) {
        LOGE("Failed to start splash playback");
        goto fail_thread;
    }

    ps->splash_available = TRUE;
    ps->splash_active = FALSE;
    LOGI("Splash fallback ready with %d sequence(s)", order_count);
    return TRUE;

fail_thread:
    if (ps->splash_loop_thread != NULL) {
        splash_quit(ps->splash);
        g_thread_join(ps->splash_loop_thread);
        ps->splash_loop_thread = NULL;
        ps->splash_loop_running = FALSE;
    }
    release_selector_pad(selector, &ps->selector_splash_pad);
    release_selector_pad(selector, &splash_sink_pad);
    if (splash_queue != NULL) {
        if (GST_OBJECT_PARENT(splash_queue) == GST_OBJECT(pipeline)) {
            gst_bin_remove(GST_BIN(pipeline), splash_queue);
        } else {
            gst_object_unref(splash_queue);
        }
        splash_queue = NULL;
    }
    if (splash_appsrc != NULL) {
        if (GST_OBJECT_PARENT(splash_appsrc) == GST_OBJECT(pipeline)) {
            gst_bin_remove(GST_BIN(pipeline), splash_appsrc);
        } else {
            gst_object_unref(splash_appsrc);
        }
        splash_appsrc = NULL;
    }
fail:
    release_selector_pad(selector, &ps->selector_splash_pad);
    release_selector_pad(selector, &splash_sink_pad);
    if (splash_queue != NULL) {
        if (GST_OBJECT_PARENT(splash_queue) == GST_OBJECT(pipeline)) {
            gst_bin_remove(GST_BIN(pipeline), splash_queue);
        } else {
            gst_object_unref(splash_queue);
        }
        splash_queue = NULL;
    }
    if (splash_appsrc != NULL) {
        if (GST_OBJECT_PARENT(splash_appsrc) == GST_OBJECT(pipeline)) {
            gst_bin_remove(GST_BIN(pipeline), splash_appsrc);
        } else {
            gst_object_unref(splash_appsrc);
        }
        splash_appsrc = NULL;
    }
    if (ps->splash != NULL) {
        splash_stop(ps->splash);
        splash_free(ps->splash);
        ps->splash = NULL;
    }
    ps->splash_available = FALSE;
    ps->splash_active = FALSE;
    return FALSE;
}

static void pipeline_teardown_splash(PipelineState *ps) {
    if (ps == NULL) {
        return;
    }
    if (ps->splash != NULL) {
        splash_stop(ps->splash);
    }
    if (ps->splash_loop_thread != NULL) {
        splash_quit(ps->splash);
        g_thread_join(ps->splash_loop_thread);
        ps->splash_loop_thread = NULL;
    }
    if (ps->splash != NULL) {
        splash_free(ps->splash);
        ps->splash = NULL;
    }
    ps->splash_loop_running = FALSE;
    release_selector_pad(ps->input_selector, &ps->selector_udp_pad);
    release_selector_pad(ps->input_selector, &ps->selector_splash_pad);
    ps->input_selector = NULL;
    ps->splash_available = FALSE;
    ps->splash_active = FALSE;
}

static void pipeline_update_splash(PipelineState *ps) {
    if (ps == NULL) {
        return;
    }
    if (!ps->splash_available || ps->udp_receiver == NULL) {
        return;
    }

    guint idle_ms = ps->splash_idle_timeout_ms;
    if (idle_ms == 0) {
        return;
    }

    guint64 last_packet = udp_receiver_get_last_packet_time(ps->udp_receiver);
    guint64 reference_ns = last_packet != 0 ? last_packet : ps->pipeline_start_ns;
    if (reference_ns == 0) {
        return;
    }

    guint64 now_ns = monotonic_time_ns();
    guint64 diff_ms = now_ns > reference_ns ? (now_ns - reference_ns) / 1000000ull : 0;

    g_mutex_lock(&ps->lock);
    gboolean active = ps->splash_active;
    ps->last_udp_activity_ns = last_packet;
    g_mutex_unlock(&ps->lock);

    if (!active && diff_ms >= idle_ms) {
        g_mutex_lock(&ps->lock);
        if (!ps->splash_active && ps->selector_splash_pad != NULL && ps->input_selector != NULL) {
            LOGI("Activating splash fallback after %u ms without video", idle_ms);
            g_object_set(ps->input_selector, "active-pad", ps->selector_splash_pad, NULL);
            ps->splash_active = TRUE;
        }
        g_mutex_unlock(&ps->lock);
    } else if (active && last_packet != 0 && diff_ms < idle_ms) {
        g_mutex_lock(&ps->lock);
        if (ps->splash_active && ps->selector_udp_pad != NULL && ps->input_selector != NULL) {
            LOGI("Video stream detected; returning to UDP input");
            g_object_set(ps->input_selector, "active-pad", ps->selector_udp_pad, NULL);
            ps->splash_active = FALSE;
        }
        g_mutex_unlock(&ps->lock);
    }
}

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
    GstElement *udp_queue = NULL;
    GstElement *selector = NULL;
    GstElement *parser = NULL;
    GstElement *capsfilter = NULL;
    GstElement *appsink = NULL;
    GstCaps *raw_caps = NULL;
    GstPad *udp_sink_pad = NULL;
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

    release_selector_pad(ps->input_selector, &ps->selector_udp_pad);
    release_selector_pad(ps->input_selector, &ps->selector_splash_pad);
    ps->input_selector = NULL;
    ps->splash_active = FALSE;
    ps->splash_available = FALSE;

    pipeline = gst_pipeline_new("pixelpilot-receiver");
    CHECK_ELEM(pipeline, "pipeline");

    appsrc = create_udp_app_source(cfg, TRUE, &receiver);
    if (appsrc == NULL) {
        goto fail;
    }

    depay = gst_element_factory_make("rtph265depay", "video_depay");
    CHECK_ELEM(depay, "rtph265depay");
    udp_queue = gst_element_factory_make("queue", "udp_queue");
    CHECK_ELEM(udp_queue, "queue");
    selector = gst_element_factory_make("input-selector", "video_selector");
    CHECK_ELEM(selector, "input-selector");
    parser = gst_element_factory_make("h265parse", "video_parser");
    CHECK_ELEM(parser, "h265parse");
    capsfilter = gst_element_factory_make("capsfilter", "video_capsfilter");
    CHECK_ELEM(capsfilter, "capsfilter");
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

    g_object_set(udp_queue, "leaky", 2, "max-size-time", (guint64)0, "max-size-bytes", (guint64)0,
                 "max-size-buffers", 16, NULL);
    g_object_set(selector, "sync-streams", FALSE, NULL);

    gst_bin_add_many(GST_BIN(pipeline), appsrc, depay, udp_queue, selector, parser, capsfilter, appsink, NULL);
    if (!gst_element_link_many(appsrc, depay, udp_queue, NULL)) {
        LOGE("Failed to link UDP receiver passthrough source chain");
        goto fail;
    }
    udp_sink_pad = gst_element_get_request_pad(selector, "sink_%u");
    if (udp_sink_pad == NULL) {
        LOGE("Failed to request selector pad for UDP branch");
        goto fail;
    }

    GstPad *udp_src_pad = gst_element_get_static_pad(udp_queue, "src");
    if (udp_src_pad == NULL) {
        LOGE("Failed to get UDP queue src pad");
        goto fail;
    }
    if (gst_pad_link(udp_src_pad, udp_sink_pad) != GST_PAD_LINK_OK) {
        LOGE("Failed to link UDP queue into selector");
        gst_object_unref(udp_src_pad);
        goto fail;
    }
    gst_object_unref(udp_src_pad);
    if (!gst_element_link_many(selector, parser, capsfilter, appsink, NULL)) {
        LOGE("Failed to link UDP receiver passthrough pipeline");
        goto fail;
    }

    ps->selector_udp_pad = udp_sink_pad;
    udp_sink_pad = NULL;

    if (!pipeline_prepare_splash(ps, cfg, pipeline, selector)) {
        goto fail;
    }

    if (ps->selector_udp_pad != NULL) {
        g_object_set(selector, "active-pad", ps->selector_udp_pad, NULL);
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
                     GST_APP_STREAM_TYPE_STREAM, "do-timestamp", TRUE, "max-bytes", (guint64)(1024 * 1024), NULL);
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

        g_object_set(audio_queue_start, "leaky", 2, "max-size-time", (guint64)0, "max-size-bytes", (guint64)0, NULL);
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
    ps->input_selector = selector;
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
    if (selector != NULL && GST_OBJECT_PARENT(selector) == NULL) {
        gst_object_unref(selector);
    }
    if (udp_queue != NULL && GST_OBJECT_PARENT(udp_queue) == NULL) {
        gst_object_unref(udp_queue);
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
    ps->input_selector = NULL;
    release_selector_pad(selector, &ps->selector_splash_pad);
    release_selector_pad(selector, &ps->selector_udp_pad);
    release_selector_pad(selector, &udp_sink_pad);
    ps->splash_available = FALSE;
    ps->splash_active = FALSE;
    return FALSE;
}


static gboolean setup_gst_udpsrc_pipeline(PipelineState *ps, const AppCfg *cfg) {
    if (ps == NULL || cfg == NULL) {
        return FALSE;
    }

    gchar *desc = g_strdup_printf(
        "udpsrc name=udp_source port=%d caps=\"application/x-rtp, media=(string)video, encoding-name=(string)H265, clock-rate=(int)90000, payload=(int)%d\" ! "
        "rtph265depay name=video_depay ! "
        "h265parse name=video_parser config-interval=-1 ! "
        "video/x-h265, stream-format=\"byte-stream\" ! "
        "appsink drop=true name=out_appsink",
        cfg->udp_port, cfg->vid_pt);

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

    GstElement *udpsrc = gst_bin_get_by_name(GST_BIN(pipeline), "udp_source");
    if (udpsrc == NULL) {
        LOGE("Failed to find udpsrc element in pipeline");
        gst_object_unref(appsink);
        gst_object_unref(pipeline);
        return FALSE;
    }

    GstPad *src_pad = gst_element_get_static_pad(udpsrc, "src");
    if (src_pad == NULL) {
        LOGE("Failed to get udpsrc src pad for payload filtering");
        gst_object_unref(udpsrc);
        gst_object_unref(appsink);
        gst_object_unref(pipeline);
        return FALSE;
    }

    UdpsrcPadFilterCtx *ctx = g_new0(UdpsrcPadFilterCtx, 1);
    if (ctx == NULL) {
        LOGE("Failed to allocate udpsrc payload filter context");
        gst_object_unref(src_pad);
        gst_object_unref(udpsrc);
        gst_object_unref(appsink);
        gst_object_unref(pipeline);
        return FALSE;
    }

    ctx->video_pt = cfg->vid_pt;
    gulong probe_id = gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER, udpsrc_pad_filter_probe, ctx, g_free);
    gst_object_unref(src_pad);
    gst_object_unref(udpsrc);
    if (probe_id == 0) {
        LOGE("Failed to install udpsrc payload filter probe");
        gst_object_unref(appsink);
        gst_object_unref(pipeline);
        return FALSE;
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
        GstBuffer *record_buf = NULL;
        Recorder *recorder = NULL;
        gboolean recorder_active = FALSE;

        if (buffer != NULL) {
            g_mutex_lock(&ps->lock);
            recorder_active = ps->recorder_running;
            recorder = ps->recorder;
            g_mutex_unlock(&ps->lock);
            if (recorder_active && recorder != NULL) {
                record_buf = gst_buffer_ref(buffer);
            }
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
        if (record_buf != NULL && recorder != NULL) {
            const GstCaps *sample_caps = gst_sample_get_caps(sample);
            recorder_push_video_buffer(recorder, record_buf, sample_caps);
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

    if (ps->udp_receiver != NULL) {
        udp_receiver_set_audio_record_appsrc(ps->udp_receiver, NULL);
    }

    if (ps->recorder != NULL) {
        recorder_stop(ps->recorder);
    }

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
    pipeline_teardown_splash(ps);
    ps->video_sink = NULL;
    if (ps->pipeline != NULL) {
        gst_object_unref(ps->pipeline);
        ps->pipeline = NULL;
    }
    if (ps->recorder != NULL) {
        recorder_free(ps->recorder);
        ps->recorder = NULL;
    }
    ps->recorder_running = FALSE;
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

    if (cfg->splash.idle_timeout_ms > 0) {
        ps->splash_idle_timeout_ms = (guint)cfg->splash.idle_timeout_ms;
    } else {
        ps->splash_idle_timeout_ms = 0;
    }
    ps->pipeline_start_ns = monotonic_time_ns();
    ps->last_udp_activity_ns = 0;
    ps->splash_active = FALSE;
    ps->recorder_running = FALSE;

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

    ps->pipeline_start_ns = monotonic_time_ns();

    gboolean want_record = cfg->record.enable && cfg->record.output_path[0] != '\0';
    gboolean record_audio = want_record && cfg->record.audio && cfg->aud_pt >= 0 && ps->udp_receiver != NULL;
    if (want_record && cfg->record.audio && cfg->aud_pt >= 0 && ps->udp_receiver == NULL) {
        LOGW("Recorder audio requested but UDP receiver unavailable; recording without audio");
    }
    if (want_record) {
        if (ps->recorder == NULL) {
            ps->recorder = recorder_new();
            if (ps->recorder == NULL) {
                LOGE("Failed to allocate recorder state");
                goto fail;
            }
        }
        GstCaps *record_caps = NULL;
        if (ps->video_sink != NULL) {
            GstPad *sink_pad = gst_element_get_static_pad(ps->video_sink, "sink");
            if (sink_pad != NULL) {
                record_caps = gst_pad_get_current_caps(sink_pad);
                if (record_caps == NULL) {
                    record_caps = gst_pad_query_caps(sink_pad, NULL);
                }
                gst_object_unref(sink_pad);
            }
        }

        if (!recorder_start(ps->recorder, cfg, record_audio, record_caps)) {
            LOGE("Failed to start recorder pipeline");
            if (record_caps != NULL) {
                gst_caps_unref(record_caps);
            }
            goto fail;
        }
        if (record_caps != NULL) {
            gst_caps_unref(record_caps);
        }
        ps->recorder_running = TRUE;
        if (ps->udp_receiver != NULL) {
            GstAppSrc *record_audio_src = NULL;
            if (record_audio) {
                record_audio_src = recorder_get_audio_appsrc(ps->recorder);
                if (record_audio_src == NULL) {
                    LOGW("Recorder audio appsrc unavailable; recording video only");
                }
            }
            udp_receiver_set_audio_record_appsrc(ps->udp_receiver,
                                                 record_audio && record_audio_src != NULL ? record_audio_src : NULL);
        }
    } else if (ps->udp_receiver != NULL) {
        udp_receiver_set_audio_record_appsrc(ps->udp_receiver, NULL);
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

    Recorder *recorder_to_stop = NULL;

    g_mutex_lock(&ps->lock);
    ps->state = PIPELINE_STOPPING;
    ps->stop_requested = TRUE;
    ps->decoder_running = FALSE;
    if (ps->recorder_running) {
        recorder_to_stop = ps->recorder;
    }
    ps->recorder_running = FALSE;
    g_mutex_unlock(&ps->lock);

    if (ps->udp_receiver != NULL) {
        udp_receiver_set_audio_record_appsrc(ps->udp_receiver, NULL);
    }

    if (recorder_to_stop != NULL) {
        recorder_stop(recorder_to_stop);
    }

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

    pipeline_update_splash(ps);
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
