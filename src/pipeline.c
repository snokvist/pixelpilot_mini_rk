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

static void pipeline_select_active_pad(PipelineState *ps, GstPad *pad) {
    if (ps == NULL || ps->video_selector == NULL || pad == NULL) {
        return;
    }
    g_object_set(ps->video_selector, "active-pad", pad, NULL);
}

static void pipeline_use_splash(PipelineState *ps) {
    if (ps == NULL || !ps->splash_enabled) {
        return;
    }
    if (ps->selector_splash_pad != NULL) {
        pipeline_select_active_pad(ps, ps->selector_splash_pad);
    }
}

static void pipeline_use_network(PipelineState *ps) {
    if (ps == NULL) {
        return;
    }
    if (ps->selector_network_pad != NULL) {
        pipeline_select_active_pad(ps, ps->selector_network_pad);
    }
}

static gboolean pipeline_handle_splash_eos(PipelineState *ps, GstObject *src) {
    if (ps == NULL || src == NULL || !ps->splash_enabled || ps->splash_bin == NULL) {
        return FALSE;
    }
    if (!gst_object_has_ancestor(src, GST_OBJECT(ps->splash_bin))) {
        return FALSE;
    }
    gboolean ok = gst_element_seek_simple(ps->splash_bin, GST_FORMAT_TIME,
                                          GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 0);
    if (!ok) {
        LOGW("Failed to restart splash video");
    }
    gst_element_set_state(ps->splash_bin, GST_STATE_PLAYING);
    pipeline_use_splash(ps);
    return TRUE;
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
        } else {
            pipeline_use_network(ps);
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
        pipeline_use_splash(ps);
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

static void splash_decode_pad_added_cb(GstElement *element, GstPad *pad, gpointer user_data);

static gboolean build_splash_branch(PipelineState *ps, GstElement *pipeline, const AppCfg *cfg) {
    if (ps == NULL || pipeline == NULL || cfg == NULL) {
        return FALSE;
    }

    GstElement *bin = gst_bin_new("splash_bin");
    GstElement *filesrc = gst_element_factory_make("filesrc", "splash_filesrc");
    GstElement *decode = gst_element_factory_make("decodebin", "splash_decode");
    GstElement *convert = gst_element_factory_make("videoconvert", "splash_convert");
    GstElement *queue = gst_element_factory_make("queue", "splash_queue");

    CHECK_ELEM(bin, "bin");
    CHECK_ELEM(filesrc, "filesrc");
    CHECK_ELEM(decode, "decodebin");
    CHECK_ELEM(convert, "videoconvert");
    CHECK_ELEM(queue, "queue");

    g_object_set(filesrc, "location", cfg->splash_path, NULL);
    g_object_set(queue, "leaky", 2, "max-size-time", (guint64)0, "max-size-bytes", (guint64)0, NULL);

    gst_bin_add_many(GST_BIN(bin), filesrc, decode, convert, queue, NULL);

    if (!gst_element_link(filesrc, decode)) {
        LOGE("Failed to link splash filesrc to decodebin");
        goto fail;
    }
    if (!gst_element_link(convert, queue)) {
        LOGE("Failed to link splash convert branch");
        goto fail;
    }

    GstPad *queue_src = gst_element_get_static_pad(queue, "src");
    if (queue_src == NULL) {
        LOGE("Failed to get splash queue src pad");
        goto fail;
    }
    GstPad *ghost = gst_ghost_pad_new("src", queue_src);
    gst_object_unref(queue_src);
    if (ghost == NULL) {
        LOGE("Failed to create splash ghost pad");
        goto fail;
    }
    gst_element_add_pad(bin, ghost);

    ps->splash_bin = bin;
    ps->splash_decode = decode;
    ps->splash_convert = convert;
    ps->splash_queue = queue;

    g_signal_connect(decode, "pad-added", G_CALLBACK(splash_decode_pad_added_cb), ps);

    if (!gst_bin_add(GST_BIN(pipeline), bin)) {
        LOGE("Failed to add splash bin to pipeline");
        goto fail_disconnect;
    }

    GstPad *bin_src = gst_element_get_static_pad(bin, "src");
    if (bin_src == NULL) {
        LOGE("Failed to get splash bin src pad");
        goto fail_disconnect;
    }
    GstPad *selector_pad = gst_element_get_request_pad(ps->video_selector, "sink_%u");
    if (selector_pad == NULL) {
        LOGE("Failed to request selector pad for splash branch");
        gst_object_unref(bin_src);
        goto fail_disconnect;
    }
    GstPadLinkReturn link_ret = gst_pad_link(bin_src, selector_pad);
    gst_object_unref(bin_src);
    if (link_ret != GST_PAD_LINK_OK) {
        LOGE("Failed to link splash branch to selector (ret=%d)", link_ret);
        gst_element_release_request_pad(ps->video_selector, selector_pad);
        gst_object_unref(selector_pad);
        goto fail_disconnect;
    }
    ps->selector_splash_pad = selector_pad;

    ps->splash_enabled = TRUE;
    return TRUE;

fail_disconnect:
    g_signal_handlers_disconnect_by_data(decode, ps);
    if (gst_bin_remove(GST_BIN(pipeline), bin)) {
        /* ownership transferred back */
        bin = NULL;
    }
fail:
    if (ps->selector_splash_pad != NULL) {
        gst_element_release_request_pad(ps->video_selector, ps->selector_splash_pad);
        gst_object_unref(ps->selector_splash_pad);
        ps->selector_splash_pad = NULL;
    }
    ps->splash_bin = NULL;
    ps->splash_decode = NULL;
    ps->splash_convert = NULL;
    ps->splash_queue = NULL;
    ps->splash_enabled = FALSE;
    if (bin != NULL) {
        gst_object_unref(bin);
    }
    return FALSE;
}

static void splash_decode_pad_added_cb(GstElement *element, GstPad *pad, gpointer user_data) {
    (void)element;
    PipelineState *ps = (PipelineState *)user_data;
    if (ps == NULL || pad == NULL) {
        return;
    }

    GstCaps *caps = gst_pad_get_current_caps(pad);
    gboolean is_video = FALSE;
    if (caps != NULL) {
        const GstStructure *s = gst_caps_get_structure(caps, 0);
        if (s != NULL) {
            const gchar *name = gst_structure_get_name(s);
            if (name != NULL && g_str_has_prefix(name, "video/")) {
                is_video = TRUE;
            }
        }
        gst_caps_unref(caps);
    } else {
        GstCaps *query_caps = gst_pad_query_caps(pad, NULL);
        if (query_caps != NULL) {
            const GstStructure *s = gst_caps_get_structure(query_caps, 0);
            if (s != NULL) {
                const gchar *name = gst_structure_get_name(s);
                if (name != NULL && g_str_has_prefix(name, "video/")) {
                    is_video = TRUE;
                }
            }
            gst_caps_unref(query_caps);
        }
    }

    if (!is_video) {
        return;
    }

    if (ps->splash_convert == NULL) {
        return;
    }

    GstPad *convert_sink = gst_element_get_static_pad(ps->splash_convert, "sink");
    if (convert_sink == NULL) {
        LOGW("Splash convert sink pad missing");
        return;
    }
    if (gst_pad_is_linked(convert_sink)) {
        gst_object_unref(convert_sink);
        return;
    }
    GstPadLinkReturn link_ret = gst_pad_link(pad, convert_sink);
    gst_object_unref(convert_sink);
    if (link_ret != GST_PAD_LINK_OK) {
        LOGW("Failed to link splash decode output (ret=%d)", link_ret);
        return;
    }

    if (ps->video_pad == NULL) {
        pipeline_use_splash(ps);
    }
}

static gboolean build_video_branch(PipelineState *ps, GstElement *pipeline, const AppCfg *cfg) {
    GstElement *queue_pre = gst_element_factory_make("queue", "video_queue_pre");
    GstElement *depay = gst_element_factory_make("rtph265depay", "video_depay");
    GstElement *parser = gst_element_factory_make("h265parse", "video_parser");
    GstElement *decoder = gst_element_factory_make("mppvideodec", "video_decoder");
    GstElement *queue_post = gst_element_factory_make("queue", "video_queue_post");
    GstElement *queue_sink = gst_element_factory_make("queue", "video_queue_sink");
    GstElement *sink = gst_element_factory_make("kmssink", "video_sink");
    GstElement *selector = NULL;

    gboolean use_splash = ps->splash_enabled;
    GstPad *queue_post_src = NULL;
    GstPad *selector_pad = NULL;

    CHECK_ELEM(queue_pre, "queue");
    CHECK_ELEM(depay, "rtph265depay");
    CHECK_ELEM(parser, "h265parse");
    CHECK_ELEM(decoder, "mppvideodec");
    CHECK_ELEM(queue_post, "queue");
    CHECK_ELEM(queue_sink, "queue");
    CHECK_ELEM(sink, "kmssink");

    if (use_splash) {
        selector = gst_element_factory_make("input-selector", "video_selector");
        CHECK_ELEM(selector, "input-selector");
        g_object_set(selector, "cache-buffers", FALSE, "sync-streams", FALSE, NULL);
    }

    g_object_set(queue_pre, "leaky", cfg->video_queue_leaky, "max-size-buffers", cfg->video_queue_pre_buffers,
                 "max-size-time", (guint64)0, "max-size-bytes", (guint64)0, NULL);
    g_object_set(queue_post, "leaky", cfg->video_queue_leaky, "max-size-buffers", cfg->video_queue_post_buffers,
                 "max-size-time", (guint64)0, "max-size-bytes", (guint64)0, NULL);
    g_object_set(queue_sink, "leaky", cfg->video_queue_leaky, "max-size-buffers", cfg->video_queue_sink_buffers,
                 "max-size-time", (guint64)0, "max-size-bytes", (guint64)0, NULL);

    g_object_set(parser, "config-interval", -1, "disable-passthrough", TRUE, NULL);
    g_object_set(sink, "plane-id", cfg->plane_id, "sync", cfg->kmssink_sync ? TRUE : FALSE, "qos",
                 cfg->kmssink_qos ? TRUE : FALSE, "max-lateness", (gint64)cfg->max_lateness_ns, NULL);

    if (selector != NULL) {
        gst_bin_add_many(GST_BIN(pipeline), queue_pre, depay, parser, decoder, queue_post, selector, queue_sink, sink, NULL);
    } else {
        gst_bin_add_many(GST_BIN(pipeline), queue_pre, depay, parser, decoder, queue_post, queue_sink, sink, NULL);
    }

    if (!gst_element_link_many(queue_pre, depay, parser, decoder, queue_post, NULL)) {
        LOGE("Failed to link video decode branch");
        goto fail;
    }

    if (selector != NULL) {
        queue_post_src = gst_element_get_static_pad(queue_post, "src");
        if (queue_post_src == NULL) {
            LOGE("Failed to get queue_post src pad");
            goto fail;
        }
        selector_pad = gst_element_get_request_pad(selector, "sink_%u");
        if (selector_pad == NULL) {
            LOGE("Failed to request selector pad for video branch");
            goto fail;
        }
        GstPadLinkReturn link_ret = gst_pad_link(queue_post_src, selector_pad);
        gst_object_unref(queue_post_src);
        queue_post_src = NULL;
        if (link_ret != GST_PAD_LINK_OK) {
            LOGE("Failed to link video branch to selector (ret=%d)", link_ret);
            goto fail;
        }
        ps->video_selector = selector;
        ps->selector_network_pad = selector_pad;
        if (!gst_element_link_many(selector, queue_sink, sink, NULL)) {
            LOGE("Failed to link selector to sink");
            goto fail;
        }
        if (!build_splash_branch(ps, pipeline, cfg)) {
            LOGW("Splash screen pipeline disabled (setup failed)");
        }
    } else {
        if (!gst_element_link_many(queue_post, queue_sink, sink, NULL)) {
            LOGE("Failed to link video branch");
            goto fail;
        }
        ps->video_selector = NULL;
        ps->selector_network_pad = NULL;
        ps->selector_splash_pad = NULL;
        ps->splash_bin = NULL;
        ps->splash_convert = NULL;
        ps->splash_queue = NULL;
        ps->splash_decode = NULL;
        ps->splash_enabled = FALSE;
    }

    ps->video_branch_entry = queue_pre;
    ps->video_sink = sink;
    if (ps->splash_enabled && ps->selector_splash_pad != NULL) {
        pipeline_use_splash(ps);
    }
    return TRUE;

fail:
    if (queue_post_src != NULL) {
        gst_object_unref(queue_post_src);
    }
    if (selector != NULL && selector_pad != NULL) {
        gst_element_release_request_pad(selector, selector_pad);
        gst_object_unref(selector_pad);
    }
    if (selector != NULL) {
        ps->video_selector = NULL;
    }
    ps->selector_network_pad = NULL;
    ps->selector_splash_pad = NULL;
    ps->splash_enabled = FALSE;
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
        case GST_MESSAGE_EOS: {
            GstObject *src = GST_MESSAGE_SRC(msg);
            if (pipeline_handle_splash_eos(ps, src)) {
                gst_message_unref(msg);
                continue;
            }
            LOGI("Pipeline reached EOS");
            g_mutex_lock(&ps->lock);
            ps->stop_requested = TRUE;
            g_mutex_unlock(&ps->lock);
            running = FALSE;
            break;
        }
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
    if (ps->udp_receiver != NULL) {
        udp_receiver_destroy(ps->udp_receiver);
        ps->udp_receiver = NULL;
    }
    if (ps->rtpbin != NULL) {
        g_signal_handlers_disconnect_by_data(ps->rtpbin, ps);
        ps->rtpbin = NULL;
    }
    if (ps->splash_decode != NULL) {
        g_signal_handlers_disconnect_by_data(ps->splash_decode, ps);
    }
    if (ps->video_selector != NULL) {
        if (ps->selector_network_pad != NULL) {
            gst_element_release_request_pad(ps->video_selector, ps->selector_network_pad);
            gst_object_unref(ps->selector_network_pad);
            ps->selector_network_pad = NULL;
        }
        if (ps->selector_splash_pad != NULL) {
            gst_element_release_request_pad(ps->video_selector, ps->selector_splash_pad);
            gst_object_unref(ps->selector_splash_pad);
            ps->selector_splash_pad = NULL;
        }
    }
    ps->video_selector = NULL;
    ps->splash_bin = NULL;
    ps->splash_decode = NULL;
    ps->splash_convert = NULL;
    ps->splash_queue = NULL;
    ps->splash_enabled = FALSE;
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
    ps->rtpbin = NULL;
    ps->video_branch_entry = NULL;
    ps->audio_branch_entry = NULL;
    ps->video_pad = NULL;
    ps->audio_pad = NULL;
    ps->udp_receiver = NULL;
    ps->video_selector = NULL;
    ps->selector_network_pad = NULL;
    ps->selector_splash_pad = NULL;
    ps->splash_bin = NULL;
    ps->splash_decode = NULL;
    ps->splash_convert = NULL;
    ps->splash_queue = NULL;
    ps->splash_enabled = (cfg->splash_enable && cfg->splash_path[0] != '\0');
    if (cfg->splash_enable && cfg->splash_path[0] == '\0') {
        LOGW("Splash screen enabled but no splash-path configured; disabling splash playback");
        ps->splash_enabled = FALSE;
    }
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
