#define _GNU_SOURCE

#include "udp_receiver.h"
#include "logging.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef GST_USE_UNSTABLE_API
#define GST_USE_UNSTABLE_API
#endif

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/rtsp/gstrtspdefs.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <gst/gstbuffer.h>
#include <gst/gstbufferpool.h>

typedef struct _GstRTSPSrc GstRTSPSrc;

#define RTP_MIN_HEADER 12
#define FRAME_EWMA_ALPHA 0.1
#define JITTER_EWMA_ALPHA 0.1
#define BITRATE_WINDOW_NS 100000000ULL // 100 ms
#define BITRATE_EWMA_ALPHA 0.1
#define UDP_RECEIVER_MAX_PACKET 4096
#define RTSP_RETRY_INTERVAL_NS 3000000000ull // 3 seconds

typedef enum {
    FALLBACK_NONE = 0,
    FALLBACK_UDP_PORT = 1,
    FALLBACK_RTSP = 2,
} FallbackMode;

typedef struct {
    guint16 sequence;
    guint32 timestamp;
    guint8 payload_type;
    guint8 marker;
    guint8 has_padding;
    guint8 has_extension;
    guint8 csrc_count;
    guint8 version;
    guint payload_offset;
    guint payload_size;
} RtpParseResult;

struct UdpReceiver {
    GstAppSrc *appsrc;
    GThread *thread;
    GMutex lock;
    gboolean running;
    gboolean stop_requested;
    gboolean stats_enabled;

    int udp_port;
    int udp_fallback_port;
    int vid_pt;
    int aud_pt;
    int sockfd_primary;
    int sockfd_secondary;
    FallbackMode fallback_mode;
    gboolean fallback_enabled;
    gboolean using_fallback;
    guint64 last_primary_data_ns;
    gboolean discont_pending;

    char rtsp_location[256];
    int rtsp_latency_ms;
    GstRTSPLowerTrans rtsp_protocols;
    guint64 rtsp_retry_ns;
    guint64 rtsp_last_attempt_ns;
    GstElement *rtsp_pipeline;
    GstElement *rtsp_queue;
    GstAppSink *rtsp_sink;
    GstBus *rtsp_bus;
    GstPad *rtsp_pad;
    gboolean rtsp_pad_linked;

    GstBufferPool *pool;
    gsize buffer_size;

    gboolean seq_initialized;
    guint16 expected_seq;
    guint16 last_seq;
    gboolean have_last_seq;
    gboolean frame_active;
    guint32 frame_timestamp;
    guint64 frame_bytes;
    gboolean frame_missing;

    gboolean transit_initialized;
    double last_transit;

    guint64 bitrate_window_start_ns;
    guint64 bitrate_window_bytes;

    UdpReceiverStats stats;

    UdpReceiverPacketSample history[UDP_RECEIVER_HISTORY];

    const AppCfg *cfg;
    int cpu_slot;
};

static gboolean parse_rtp(const guint8 *data, gsize len, RtpParseResult *out) {
    if (len < RTP_MIN_HEADER) {
        return FALSE;
    }

    guint8 vpxcc = data[0];
    guint8 mpayload = data[1];
    guint8 version = vpxcc >> 6;
    if (version != 2) {
        return FALSE;
    }

    guint8 csrc_count = vpxcc & 0x0F;
    gboolean padding = (vpxcc & 0x20u) != 0;
    gboolean extension = (vpxcc & 0x10u) != 0;
    gboolean marker = (mpayload & 0x80u) != 0;
    guint8 payload_type = mpayload & 0x7Fu;

    guint payload_offset = RTP_MIN_HEADER + csrc_count * 4u;
    if (len < payload_offset) {
        return FALSE;
    }

    if (extension) {
        if (len < payload_offset + 4u) {
            return FALSE;
        }
        guint16 ext_len = ((guint16)data[payload_offset + 2] << 8) | data[payload_offset + 3];
        guint32 ext_bytes = 4u + ((guint32)ext_len * 4u);
        if (len < payload_offset + ext_bytes) {
            return FALSE;
        }
        payload_offset += ext_bytes;
    }

    guint payload_size = len > payload_offset ? (guint)(len - payload_offset) : 0u;
    if (padding) {
        guint8 pad = data[len - 1];
        if (pad <= payload_size) {
            payload_size -= pad;
        } else {
            payload_size = 0;
        }
    }

    out->sequence = ((guint16)data[2] << 8) | data[3];
    out->timestamp = ((guint32)data[4] << 24) | ((guint32)data[5] << 16) | ((guint32)data[6] << 8) | data[7];
    out->payload_type = payload_type;
    out->marker = marker ? 1 : 0;
    out->has_padding = padding ? 1 : 0;
    out->has_extension = extension ? 1 : 0;
    out->csrc_count = csrc_count;
    out->version = version;
    out->payload_offset = payload_offset;
    out->payload_size = payload_size;
    return TRUE;
}

static inline guint16 seq_next(guint16 seq) {
    return (guint16)(seq + 1u);
}

static inline gint16 seq_delta(guint16 a, guint16 b) {
    return (gint16)(a - b);
}

static inline guint64 get_time_ns(void) {
    return (guint64)g_get_monotonic_time() * 1000ull;
}

static GstRTSPLowerTrans parse_rtsp_protocols_string(const char *value) {
    GstRTSPLowerTrans protocols = 0;
    if (value == NULL || *value == '\0') {
        return GST_RTSP_LOWER_TRANS_UDP;
    }

    const char *p = value;
    while (*p != '\0') {
        while (*p == ',' || g_ascii_isspace(*p)) {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        const char *start = p;
        while (*p != '\0' && *p != ',') {
            ++p;
        }
        gsize len = (gsize)(p - start);
        gchar *token = g_strndup(start, len);
        if (token != NULL) {
            gchar *trimmed = g_strstrip(token);
            if (g_ascii_strcasecmp(trimmed, "udp") == 0) {
                protocols |= GST_RTSP_LOWER_TRANS_UDP;
            } else if (g_ascii_strcasecmp(trimmed, "tcp") == 0) {
                protocols |= GST_RTSP_LOWER_TRANS_TCP;
            } else if (g_ascii_strcasecmp(trimmed, "udp-mcast") == 0 ||
                       g_ascii_strcasecmp(trimmed, "udpmcast") == 0) {
                protocols |= GST_RTSP_LOWER_TRANS_UDP_MCAST;
            } else if (g_ascii_strcasecmp(trimmed, "any") == 0) {
                protocols |= GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP |
                             GST_RTSP_LOWER_TRANS_UDP_MCAST;
            }
            g_free(token);
        }
        if (*p == ',') {
            ++p;
        }
    }

    if (protocols == 0) {
        protocols = GST_RTSP_LOWER_TRANS_UDP;
    }
    return protocols;
}

static GstCaps *pad_query_caps_or_current(GstPad *pad) {
    if (pad == NULL) {
        return NULL;
    }
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (caps != NULL) {
        return caps;
    }
    return gst_pad_query_caps(pad, NULL);
}

static gboolean caps_is_rtp_video(const GstCaps *caps) {
    if (caps == NULL) {
        return FALSE;
    }
    const GstStructure *s = gst_caps_get_structure(caps, 0);
    if (s == NULL) {
        return FALSE;
    }
    const gchar *name = gst_structure_get_name(s);
    if (g_strcmp0(name, "application/x-rtp") != 0) {
        return FALSE;
    }
    const gchar *media = gst_structure_get_string(s, "media");
    if (media == NULL) {
        return TRUE;
    }
    return g_strcmp0(media, "video") == 0;
}

static gboolean rtsp_select_stream_cb(GstRTSPSrc *src, guint num, GstCaps *caps, gpointer user_data) {
    (void)src;
    (void)num;
    (void)user_data;
    return caps_is_rtp_video(caps);
}

static void rtsp_pad_added_cb(GstElement *element, GstPad *pad, gpointer user_data) {
    (void)element;
    struct UdpReceiver *ur = (struct UdpReceiver *)user_data;
    if (ur == NULL || ur->rtsp_queue == NULL) {
        return;
    }

    GstCaps *caps = pad_query_caps_or_current(pad);
    gboolean accept = caps_is_rtp_video(caps);
    if (caps != NULL) {
        gst_caps_unref(caps);
    }
    if (!accept) {
        return;
    }

    GstPad *sink_pad = gst_element_get_static_pad(ur->rtsp_queue, "sink");
    if (sink_pad == NULL) {
        return;
    }
    if (gst_pad_is_linked(sink_pad)) {
        gst_object_unref(sink_pad);
        return;
    }

    GstPadLinkReturn ret = gst_pad_link(pad, sink_pad);
    gst_object_unref(sink_pad);
    if (ret == GST_PAD_LINK_OK) {
        if (ur->rtsp_pad != NULL) {
            gst_object_unref(ur->rtsp_pad);
        }
        ur->rtsp_pad = gst_object_ref(pad);
        ur->rtsp_pad_linked = TRUE;
        LOGI("UDP receiver: RTSP fallback pad linked");
    } else {
        LOGE("UDP receiver: failed to link RTSP fallback pad (ret=%d)", ret);
    }
}

static void rtsp_pad_removed_cb(GstElement *element, GstPad *pad, gpointer user_data) {
    (void)element;
    struct UdpReceiver *ur = (struct UdpReceiver *)user_data;
    if (ur == NULL) {
        return;
    }
    if (ur->rtsp_pad != NULL && ur->rtsp_pad == pad) {
        gst_object_unref(ur->rtsp_pad);
        ur->rtsp_pad = NULL;
        ur->rtsp_pad_linked = FALSE;
        LOGI("UDP receiver: RTSP fallback pad removed");
    }
}

static gboolean rtsp_pipeline_start(struct UdpReceiver *ur, guint64 now_ns) {
    if (ur == NULL) {
        return FALSE;
    }
    if (ur->rtsp_pipeline != NULL && ur->rtsp_sink != NULL) {
        return TRUE;
    }
    if (ur->rtsp_location[0] == '\0') {
        return FALSE;
    }
    if (now_ns == 0) {
        now_ns = get_time_ns();
    }
    if (ur->rtsp_last_attempt_ns != 0 &&
        now_ns - ur->rtsp_last_attempt_ns < ur->rtsp_retry_ns) {
        return FALSE;
    }

    ur->rtsp_last_attempt_ns = now_ns;

    GstElement *pipeline = gst_pipeline_new("udp-rtsp-fallback");
    GstElement *src = gst_element_factory_make("rtspsrc", "udp_receiver_rtspsrc");
    GstElement *queue = gst_element_factory_make("queue", "udp_receiver_rtspqueue");
    GstElement *sink = gst_element_factory_make("appsink", "udp_receiver_rtspappsink");
    if (pipeline == NULL || src == NULL || queue == NULL || sink == NULL) {
        LOGE("UDP receiver: failed to create RTSP fallback elements");
        if (pipeline != NULL) {
            gst_object_unref(pipeline);
        }
        if (src != NULL) {
            gst_object_unref(src);
        }
        if (queue != NULL) {
            gst_object_unref(queue);
        }
        if (sink != NULL) {
            gst_object_unref(sink);
        }
        return FALSE;
    }

    g_object_set(src, "location", ur->rtsp_location, "latency", ur->rtsp_latency_ms, "protocols",
                 ur->rtsp_protocols, "do-rtsp-keep-alive", TRUE, "keep-alive",
                 GST_RTSP_KEEP_ALIVE_OPTIONS, NULL);
    g_object_set(queue, "leaky", 2, "max-size-buffers", 16, "max-size-bytes", (guint64)0,
                 "max-size-time", (guint64)0, NULL);
    g_object_set(sink, "emit-signals", FALSE, "sync", FALSE, "max-buffers", 8, "drop", TRUE, NULL);

    gst_bin_add_many(GST_BIN(pipeline), src, queue, sink, NULL);
    if (!gst_element_link(queue, sink)) {
        LOGE("UDP receiver: failed to link RTSP fallback queue to appsink");
        gst_object_unref(pipeline);
        return FALSE;
    }

    g_signal_connect(src, "pad-added", G_CALLBACK(rtsp_pad_added_cb), ur);
    g_signal_connect(src, "pad-removed", G_CALLBACK(rtsp_pad_removed_cb), ur);
    g_signal_connect(src, "select-stream", G_CALLBACK(rtsp_select_stream_cb), ur);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_ASYNC) {
        GstState state = GST_STATE_NULL;
        GstState pending = GST_STATE_NULL;
        ret = gst_element_get_state(pipeline, &state, &pending, GST_SECOND);
    }
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOGE("UDP receiver: failed to start RTSP fallback pipeline");
        if (bus != NULL) {
            gst_object_unref(bus);
        }
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return FALSE;
    }

    ur->rtsp_pipeline = pipeline;
    ur->rtsp_queue = queue;
    ur->rtsp_sink = GST_APP_SINK(sink);
    ur->rtsp_bus = bus;
    ur->rtsp_pad = NULL;
    ur->rtsp_pad_linked = FALSE;

    LOGI("UDP receiver: RTSP fallback connecting to %s", ur->rtsp_location);
    return TRUE;
}

static void rtsp_pipeline_stop(struct UdpReceiver *ur) {
    if (ur == NULL) {
        return;
    }
    if (ur->rtsp_pipeline != NULL) {
        gst_element_set_state(ur->rtsp_pipeline, GST_STATE_NULL);
        gst_object_unref(ur->rtsp_pipeline);
        ur->rtsp_pipeline = NULL;
    }
    if (ur->rtsp_bus != NULL) {
        gst_object_unref(ur->rtsp_bus);
        ur->rtsp_bus = NULL;
    }
    if (ur->rtsp_pad != NULL) {
        gst_object_unref(ur->rtsp_pad);
        ur->rtsp_pad = NULL;
    }
    ur->rtsp_queue = NULL;
    ur->rtsp_sink = NULL;
    ur->rtsp_pad_linked = FALSE;
}

static gboolean rtsp_pipeline_poll_bus(struct UdpReceiver *ur) {
    if (ur == NULL || ur->rtsp_bus == NULL) {
        return TRUE;
    }
    gboolean ok = TRUE;
    GstMessage *msg = NULL;
    while ((msg = gst_bus_pop_filtered(ur->rtsp_bus, GST_MESSAGE_ERROR | GST_MESSAGE_EOS)) != NULL) {
        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            LOGE("UDP receiver: RTSP fallback error: %s (debug=%s)",
                 err != NULL ? err->message : "unknown", dbg != NULL ? dbg : "none");
            if (err != NULL) {
                g_error_free(err);
            }
            g_free(dbg);
            ok = FALSE;
            break;
        }
        case GST_MESSAGE_EOS:
            LOGI("UDP receiver: RTSP fallback reached EOS");
            ok = FALSE;
            break;
        default:
            break;
        }
        gst_message_unref(msg);
    }
    return ok;
}

static void history_push(struct UdpReceiver *ur, const UdpReceiverPacketSample *sample) {
    ur->history[ur->stats.history_head] = *sample;
    ur->stats.history_head = (ur->stats.history_head + 1u) % UDP_RECEIVER_HISTORY;
    if (ur->stats.history_count < UDP_RECEIVER_HISTORY) {
        ur->stats.history_count++;
    }
}

static void reset_stats_locked(struct UdpReceiver *ur) {
    ur->seq_initialized = FALSE;
    ur->have_last_seq = FALSE;
    ur->expected_seq = 0;
    ur->last_seq = 0;
    ur->frame_active = FALSE;
    ur->frame_timestamp = 0;
    ur->frame_bytes = 0;
    ur->frame_missing = FALSE;
    ur->transit_initialized = FALSE;
    ur->last_transit = 0.0;
    ur->bitrate_window_start_ns = 0;
    ur->bitrate_window_bytes = 0;
    memset(&ur->stats, 0, sizeof(ur->stats));
    memset(ur->history, 0, sizeof(ur->history));
}

static void update_bitrate(struct UdpReceiver *ur, guint64 arrival_ns, guint32 bytes) {
    if (ur->bitrate_window_start_ns == 0) {
        ur->bitrate_window_start_ns = arrival_ns;
    }
    ur->bitrate_window_bytes += bytes;
    guint64 elapsed_ns = arrival_ns - ur->bitrate_window_start_ns;
    if (elapsed_ns >= BITRATE_WINDOW_NS && elapsed_ns > 0) {
        double instant_mbps = ((double)ur->bitrate_window_bytes * 8.0) / ((double)elapsed_ns / 1e9) / 1e6;
        ur->stats.bitrate_mbps = instant_mbps;
        if (ur->stats.bitrate_avg_mbps == 0.0) {
            ur->stats.bitrate_avg_mbps = instant_mbps;
        } else {
            ur->stats.bitrate_avg_mbps += (instant_mbps - ur->stats.bitrate_avg_mbps) * BITRATE_EWMA_ALPHA;
        }
        ur->bitrate_window_start_ns = arrival_ns;
        ur->bitrate_window_bytes = 0;
    }
}

static void finalize_frame(struct UdpReceiver *ur) {
    if (!ur->frame_active) {
        return;
    }
    ur->stats.frame_count++;
    ur->stats.last_frame_bytes = ur->frame_bytes;
    if (ur->stats.frame_size_avg == 0.0) {
        ur->stats.frame_size_avg = (double)ur->frame_bytes;
    } else {
        ur->stats.frame_size_avg += ((double)ur->frame_bytes - ur->stats.frame_size_avg) * FRAME_EWMA_ALPHA;
    }
    if (ur->frame_missing) {
        ur->stats.incomplete_frames++;
    }
    ur->frame_active = FALSE;
    ur->frame_bytes = 0;
    ur->frame_missing = FALSE;
}

static void process_rtp(struct UdpReceiver *ur, const guint8 *data, gsize len, guint64 arrival_ns) {
    RtpParseResult rtp;
    if (!parse_rtp(data, len, &rtp)) {
        return;
    }

    gboolean is_video = (rtp.payload_type == ur->vid_pt);
    gboolean is_audio = (rtp.payload_type == ur->aud_pt);

    if (!ur->stats_enabled) {
        return;
    }

    UdpReceiverPacketSample sample = {0};
    sample.sequence = rtp.sequence;
    sample.timestamp = rtp.timestamp;
    sample.payload_type = rtp.payload_type;
    sample.marker = rtp.marker;
    sample.size = (guint32)len;
    sample.arrival_ns = arrival_ns;

    ur->stats.total_packets++;
    ur->stats.total_bytes += len;

    if (is_video) {
        ur->stats.video_packets++;
        ur->stats.video_bytes += len;
        ur->stats.last_video_timestamp = rtp.timestamp;

        if (!ur->frame_active || ur->frame_timestamp != rtp.timestamp) {
            if (ur->frame_active) {
                finalize_frame(ur);
            }
            ur->frame_active = TRUE;
            ur->frame_timestamp = rtp.timestamp;
            ur->frame_bytes = len;
            ur->frame_missing = FALSE;
        } else {
            ur->frame_bytes += len;
        }

        if (!ur->seq_initialized) {
            ur->seq_initialized = TRUE;
            ur->expected_seq = seq_next(rtp.sequence);
            ur->stats.expected_sequence = ur->expected_seq;
        } else {
            gint16 delta = seq_delta(rtp.sequence, ur->expected_seq);
            if (delta == 0) {
                ur->expected_seq = seq_next(rtp.sequence);
            } else if (delta > 0) {
                ur->stats.lost_packets += delta;
                ur->expected_seq = seq_next(rtp.sequence);
                ur->frame_missing = TRUE;
                sample.flags |= UDP_SAMPLE_FLAG_LOSS;
            } else { // delta < 0
                ur->stats.reordered_packets++;
                sample.flags |= UDP_SAMPLE_FLAG_REORDER;
            }
            ur->stats.expected_sequence = ur->expected_seq;
        }

        if (ur->have_last_seq && rtp.sequence == ur->last_seq) {
            ur->stats.duplicate_packets++;
            sample.flags |= UDP_SAMPLE_FLAG_DUPLICATE;
        }
        ur->last_seq = rtp.sequence;
        ur->have_last_seq = TRUE;

        double arrival_rtp = (double)arrival_ns / 1e9 * 90000.0;
        double transit = arrival_rtp - (double)rtp.timestamp;
        if (!ur->transit_initialized) {
            ur->transit_initialized = TRUE;
            ur->last_transit = transit;
            ur->stats.jitter = 0.0;
            ur->stats.jitter_avg = 0.0;
        } else {
            double d = transit - ur->last_transit;
            ur->last_transit = transit;
            ur->stats.jitter += (fabs(d) - ur->stats.jitter) / 16.0;
            if (ur->stats.jitter_avg == 0.0) {
                ur->stats.jitter_avg = ur->stats.jitter;
            } else {
                ur->stats.jitter_avg += (ur->stats.jitter - ur->stats.jitter_avg) * JITTER_EWMA_ALPHA;
            }
        }
    } else if (is_audio) {
        ur->stats.audio_packets++;
        ur->stats.audio_bytes += len;
    } else {
        ur->stats.ignored_packets++;
    }

    if (rtp.marker && is_video) {
        sample.flags |= UDP_SAMPLE_FLAG_FRAME_END;
        finalize_frame(ur);
    }

    update_bitrate(ur, arrival_ns, (guint32)len);
    history_push(ur, &sample);
}

static gboolean buffer_pool_start(struct UdpReceiver *ur) {
    if (ur->pool != NULL) {
        gst_buffer_pool_set_active(ur->pool, FALSE);
        gst_object_unref(ur->pool);
        ur->pool = NULL;
    }

    GstBufferPool *pool = gst_buffer_pool_new();
    if (pool == NULL) {
        return FALSE;
    }

    GstStructure *config = gst_buffer_pool_get_config(pool);
    gst_buffer_pool_config_set_params(config, NULL, UDP_RECEIVER_MAX_PACKET, 4, 0);
    if (!gst_buffer_pool_set_config(pool, config)) {
        gst_object_unref(pool);
        return FALSE;
    }

    if (!gst_buffer_pool_set_active(pool, TRUE)) {
        gst_object_unref(pool);
        return FALSE;
    }

    ur->pool = pool;
    ur->buffer_size = UDP_RECEIVER_MAX_PACKET;
    return TRUE;
}

static void buffer_pool_stop(struct UdpReceiver *ur) {
    if (ur->pool != NULL) {
        gst_buffer_pool_set_active(ur->pool, FALSE);
        gst_object_unref(ur->pool);
        ur->pool = NULL;
        ur->buffer_size = 0;
    }
}

static gpointer receiver_thread(gpointer data) {
    struct UdpReceiver *ur = (struct UdpReceiver *)data;
    const gsize max_pkt = ur->buffer_size > 0 ? ur->buffer_size : UDP_RECEIVER_MAX_PACKET;
    const guint64 fallback_delay_ns = 2000000000ull; // 2 seconds
    const gint poll_timeout_ms = 100;

    cpu_set_t thread_mask;
    if (cfg_get_thread_affinity(ur->cfg, ur->cpu_slot, &thread_mask)) {
        int err = pthread_setaffinity_np(pthread_self(), sizeof(thread_mask), &thread_mask);
        if (err != 0) {
            LOGW("UDP receiver: pthread_setaffinity_np failed: %s", g_strerror(err));
        }
    }

    while (TRUE) {
        g_mutex_lock(&ur->lock);
        gboolean stop = ur->stop_requested;
        g_mutex_unlock(&ur->lock);
        if (stop) {
            break;
        }

        guint64 now_ns = get_time_ns();
        if (ur->fallback_enabled && !ur->using_fallback) {
            if (now_ns - ur->last_primary_data_ns >= fallback_delay_ns) {
                ur->using_fallback = TRUE;
                if (ur->fallback_mode == FALLBACK_RTSP) {
                    LOGI("UDP receiver: switching to RTSP fallback stream");
                    ur->rtsp_last_attempt_ns = 0;
                } else {
                    LOGI("UDP receiver: switching to fallback port %d", ur->udp_fallback_port);
                }
                g_mutex_lock(&ur->lock);
                reset_stats_locked(ur);
                ur->discont_pending = TRUE;
                g_mutex_unlock(&ur->lock);
            }
        }

        struct pollfd fds[2];
        int nfds = 0;
        int idx_primary = -1;
        int idx_fallback = -1;
        if (ur->sockfd_primary >= 0) {
            idx_primary = nfds;
            fds[nfds].fd = ur->sockfd_primary;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
        if (ur->fallback_mode == FALLBACK_UDP_PORT && ur->fallback_enabled && ur->sockfd_secondary >= 0) {
            idx_fallback = nfds;
            fds[nfds].fd = ur->sockfd_secondary;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        int pret = 0;
        if (nfds > 0) {
            pret = poll(fds, nfds, poll_timeout_ms);
            if (pret < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LOGE("UDP receiver: poll failed: %s", g_strerror(errno));
                break;
            }
        } else {
            g_usleep(5000);
        }

        gboolean primary_ready = FALSE;
        gboolean fallback_ready = FALSE;

        if (idx_primary >= 0 && pret > 0) {
            if (fds[idx_primary].revents & POLLIN) {
                primary_ready = TRUE;
            }
            if (fds[idx_primary].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                LOGW("UDP receiver: primary socket error (events=0x%x)", fds[idx_primary].revents);
            }
        }

        if (ur->fallback_mode == FALLBACK_UDP_PORT && idx_fallback >= 0 && pret > 0) {
            if (fds[idx_fallback].revents & POLLIN) {
                fallback_ready = TRUE;
            }
            if (fds[idx_fallback].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                LOGW("UDP receiver: fallback socket error (events=0x%x)", fds[idx_fallback].revents);
            }
        }

        int active_fd = -1;
        gboolean active_is_primary = FALSE;
        gboolean switching_from_fallback = FALSE;

        if (primary_ready) {
            active_fd = ur->sockfd_primary;
            active_is_primary = TRUE;
            switching_from_fallback = ur->using_fallback;
        } else if (fallback_ready && ur->fallback_enabled && ur->using_fallback) {
            active_fd = ur->sockfd_secondary;
        }

        if (active_fd >= 0) {
            GstBuffer *gstbuf = NULL;
            GstFlowReturn flow = gst_buffer_pool_acquire_buffer(ur->pool, &gstbuf, NULL);
            if (flow != GST_FLOW_OK || gstbuf == NULL) {
                LOGE("UDP receiver: failed to acquire buffer from pool (flow=%s)",
                     gst_flow_get_name(flow));
                g_usleep(1000);
                continue;
            }

            GstMapInfo map;
            if (!gst_buffer_map(gstbuf, &map, GST_MAP_WRITE)) {
                LOGE("UDP receiver: buffer map failed");
                gst_buffer_unref(gstbuf);
                g_usleep(1000);
                continue;
            }

            struct sockaddr_in src;
            socklen_t slen = sizeof(src);
            ssize_t n = recvfrom(active_fd, map.data, max_pkt, 0, (struct sockaddr *)&src, &slen);
            if (n < 0) {
                int err = errno;
                gst_buffer_unmap(gstbuf, &map);
                gst_buffer_unref(gstbuf);
                if (err == EINTR) {
                    continue;
                }
                if (err == EAGAIN || err == EWOULDBLOCK) {
                    g_usleep(1000);
                    continue;
                }
                if (!ur->stop_requested) {
                    LOGE("UDP receiver: recvfrom failed: %s", g_strerror(err));
                }
                break;
            }

            gst_buffer_set_size(gstbuf, (gsize)n);

            guint64 arrival_ns = get_time_ns();
            gboolean mark_discont = FALSE;
            g_mutex_lock(&ur->lock);
            if (ur->discont_pending) {
                mark_discont = TRUE;
                ur->discont_pending = FALSE;
            }
            if (active_is_primary) {
                ur->last_primary_data_ns = arrival_ns;
            }
            process_rtp(ur, map.data, (gsize)n, arrival_ns);
            g_mutex_unlock(&ur->lock);

            gst_buffer_unmap(gstbuf, &map);

            if (switching_from_fallback && ur->using_fallback) {
                LOGI("UDP receiver: switching back to primary port %d", ur->udp_port);
                g_mutex_lock(&ur->lock);
                reset_stats_locked(ur);
                ur->discont_pending = TRUE;
                g_mutex_unlock(&ur->lock);
                ur->using_fallback = FALSE;
                if (ur->fallback_mode == FALLBACK_RTSP) {
                    rtsp_pipeline_stop(ur);
                    ur->rtsp_last_attempt_ns = 0;
                }
            }

            if (mark_discont) {
                GST_BUFFER_FLAG_SET(gstbuf, GST_BUFFER_FLAG_DISCONT);
                GST_BUFFER_FLAG_SET(gstbuf, GST_BUFFER_FLAG_RESYNC);
            }

            GstFlowReturn push_flow = gst_app_src_push_buffer(ur->appsrc, gstbuf);
            if (push_flow != GST_FLOW_OK) {
                LOGV("UDP receiver: push_buffer returned %s", gst_flow_get_name(push_flow));
                if (push_flow == GST_FLOW_FLUSHING) {
                    g_usleep(1000);
                }
            }
            continue;
        }

        if (ur->fallback_mode == FALLBACK_RTSP && ur->fallback_enabled && ur->using_fallback) {
            if (!rtsp_pipeline_start(ur, now_ns)) {
                g_usleep(5000);
                continue;
            }
            if (!rtsp_pipeline_poll_bus(ur)) {
                rtsp_pipeline_stop(ur);
                ur->rtsp_last_attempt_ns = get_time_ns();
                g_mutex_lock(&ur->lock);
                ur->discont_pending = TRUE;
                g_mutex_unlock(&ur->lock);
                g_usleep(10000);
                continue;
            }
            if (ur->rtsp_sink == NULL || !ur->rtsp_pad_linked) {
                g_usleep(5000);
                continue;
            }

            GstSample *sample = gst_app_sink_try_pull_sample(ur->rtsp_sink, GST_MSECOND * 50);
            if (sample == NULL) {
                continue;
            }

            GstBuffer *inbuf = gst_sample_get_buffer(sample);
            if (inbuf == NULL) {
                gst_sample_unref(sample);
                continue;
            }

            GstMapInfo inmap;
            if (!gst_buffer_map(inbuf, &inmap, GST_MAP_READ)) {
                gst_sample_unref(sample);
                continue;
            }

            GstBuffer *outbuf = NULL;
            GstFlowReturn flow = gst_buffer_pool_acquire_buffer(ur->pool, &outbuf, NULL);
            if (flow != GST_FLOW_OK || outbuf == NULL) {
                gst_buffer_unmap(inbuf, &inmap);
                gst_sample_unref(sample);
                g_usleep(1000);
                continue;
            }

            GstMapInfo outmap;
            if (!gst_buffer_map(outbuf, &outmap, GST_MAP_WRITE)) {
                gst_buffer_unmap(inbuf, &inmap);
                gst_buffer_unref(outbuf);
                gst_sample_unref(sample);
                g_usleep(1000);
                continue;
            }

            gsize copy_size = inmap.size;
            if (copy_size > outmap.size) {
                LOGW("UDP receiver: RTSP packet truncated (%zu > %zu)", (size_t)inmap.size,
                     (size_t)outmap.size);
                copy_size = outmap.size;
            }
            memcpy(outmap.data, inmap.data, copy_size);
            gst_buffer_set_size(outbuf, copy_size);

            guint64 arrival_ns = get_time_ns();
            gboolean mark_discont = FALSE;
            g_mutex_lock(&ur->lock);
            if (ur->discont_pending) {
                mark_discont = TRUE;
                ur->discont_pending = FALSE;
            }
            process_rtp(ur, outmap.data, copy_size, arrival_ns);
            g_mutex_unlock(&ur->lock);

            gst_buffer_unmap(outbuf, &outmap);
            gst_buffer_unmap(inbuf, &inmap);
            gst_sample_unref(sample);

            if (mark_discont) {
                GST_BUFFER_FLAG_SET(outbuf, GST_BUFFER_FLAG_DISCONT);
                GST_BUFFER_FLAG_SET(outbuf, GST_BUFFER_FLAG_RESYNC);
            }

            GstFlowReturn push_flow = gst_app_src_push_buffer(ur->appsrc, outbuf);
            if (push_flow != GST_FLOW_OK) {
                LOGV("UDP receiver: push_buffer returned %s", gst_flow_get_name(push_flow));
                if (push_flow == GST_FLOW_FLUSHING) {
                    g_usleep(1000);
                }
            }
            continue;
        }

        if (pret == 0) {
            continue;
        }
    }

    g_mutex_lock(&ur->lock);
    ur->running = FALSE;
    ur->stop_requested = FALSE;
    g_mutex_unlock(&ur->lock);
    return NULL;
}


UdpReceiver *udp_receiver_create(const AppCfg *cfg, GstAppSrc *appsrc) {
    if (cfg == NULL || appsrc == NULL) {
        return NULL;
    }
    struct UdpReceiver *ur = g_new0(struct UdpReceiver, 1);
    if (ur == NULL) {
        return NULL;
    }
    ur->udp_port = cfg->udp_port;
    ur->udp_fallback_port = cfg->udp_fallback_port;
    ur->vid_pt = cfg->vid_pt;
    ur->aud_pt = cfg->aud_pt;
    ur->sockfd_primary = -1;
    ur->sockfd_secondary = -1;
    ur->fallback_mode = FALLBACK_NONE;
    ur->fallback_enabled = FALSE;
    ur->using_fallback = FALSE;
    ur->last_primary_data_ns = get_time_ns();
    ur->discont_pending = TRUE;
    g_mutex_init(&ur->lock);
    ur->appsrc = GST_APP_SRC(gst_object_ref(appsrc));
    ur->stats_enabled = FALSE;
    ur->pool = NULL;
    ur->buffer_size = 0;
    ur->cfg = NULL;
    ur->cpu_slot = 0;

    ur->rtsp_location[0] = '\0';
    ur->rtsp_latency_ms = cfg->splash_rtsp_latency_ms > 0 ? cfg->splash_rtsp_latency_ms : 100;
    ur->rtsp_protocols = parse_rtsp_protocols_string(cfg->splash_rtsp_protocols);
    ur->rtsp_retry_ns = RTSP_RETRY_INTERVAL_NS;
    ur->rtsp_last_attempt_ns = 0;
    ur->rtsp_pipeline = NULL;
    ur->rtsp_queue = NULL;
    ur->rtsp_sink = NULL;
    ur->rtsp_bus = NULL;
    ur->rtsp_pad = NULL;
    ur->rtsp_pad_linked = FALSE;

    if (cfg->splash_rtsp_enable) {
        const char *rtsp_url = cfg->splash_rtsp_url[0] != '\0'
                                   ? cfg->splash_rtsp_url
                                   : DEFAULT_SPLASH_RTSP_URL;
        ur->fallback_mode = FALLBACK_RTSP;
        ur->fallback_enabled = TRUE;
        g_strlcpy(ur->rtsp_location, rtsp_url, sizeof(ur->rtsp_location));
    } else if (cfg->udp_fallback_port > 0 && cfg->udp_fallback_port != cfg->udp_port) {
        ur->fallback_mode = FALLBACK_UDP_PORT;
        ur->fallback_enabled = TRUE;
    }

    return ur;
}

static int setup_socket_for_port(int port, int *out_fd, const char *label) {
    if (out_fd == NULL) {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        LOGE("UDP receiver: socket(%s): %s", label != NULL ? label : "udp", g_strerror(errno));
        return -1;
    }

    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        LOGW("UDP receiver: setsockopt(%s, SO_REUSEADDR) failed: %s", label != NULL ? label : "udp",
             g_strerror(errno));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("UDP receiver: bind(%s:%d): %s", label != NULL ? label : "udp", port, g_strerror(errno));
        close(fd);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000; // 500 ms timeout to allow graceful shutdown
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        LOGW("UDP receiver: setsockopt(%s, SO_RCVTIMEO) failed: %s",
             label != NULL ? label : "udp", g_strerror(errno));
    }

    int buf_bytes = 4 * 1024 * 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_bytes, sizeof(buf_bytes)) < 0) {
        LOGW("UDP receiver: setsockopt(%s, SO_RCVBUF) failed: %s", label != NULL ? label : "udp",
             g_strerror(errno));
    }

    *out_fd = fd;
    return 0;
}

int udp_receiver_start(UdpReceiver *ur, const AppCfg *cfg, int cpu_slot) {
    if (ur == NULL) {
        return -1;
    }
    g_mutex_lock(&ur->lock);
    if (ur->running) {
        g_mutex_unlock(&ur->lock);
        return 0;
    }
    reset_stats_locked(ur);
    ur->discont_pending = TRUE;
    ur->cfg = cfg;
    ur->cpu_slot = cpu_slot;
    g_mutex_unlock(&ur->lock);

    if (setup_socket_for_port(ur->udp_port, &ur->sockfd_primary, "primary") != 0) {
        return -1;
    }

    if (ur->fallback_mode == FALLBACK_UDP_PORT && ur->fallback_enabled) {
        if (setup_socket_for_port(ur->udp_fallback_port, &ur->sockfd_secondary, "fallback") != 0) {
            LOGW("UDP receiver: failed to bind fallback port %d, disabling fallback", ur->udp_fallback_port);
            ur->sockfd_secondary = -1;
            ur->fallback_enabled = FALSE;
            ur->using_fallback = FALSE;
        }
    } else {
        ur->sockfd_secondary = -1;
        if (ur->fallback_mode == FALLBACK_RTSP && ur->fallback_enabled) {
            LOGI("UDP receiver: RTSP fallback enabled (url=%s, latency=%d ms)", ur->rtsp_location,
                 ur->rtsp_latency_ms);
        }
    }

    ur->using_fallback = FALSE;
    ur->last_primary_data_ns = get_time_ns();
    if (ur->fallback_mode == FALLBACK_RTSP) {
        ur->rtsp_last_attempt_ns = 0;
        rtsp_pipeline_stop(ur);
    }

    if (!buffer_pool_start(ur)) {
        LOGE("UDP receiver: failed to start buffer pool");
        if (ur->sockfd_primary >= 0) {
            close(ur->sockfd_primary);
            ur->sockfd_primary = -1;
        }
        if (ur->sockfd_secondary >= 0) {
            close(ur->sockfd_secondary);
            ur->sockfd_secondary = -1;
        }
        return -1;
    }

    g_mutex_lock(&ur->lock);
    ur->stop_requested = FALSE;
    ur->running = TRUE;
    g_mutex_unlock(&ur->lock);

    ur->thread = g_thread_new("udp-receiver", receiver_thread, ur);
    if (ur->thread == NULL) {
        LOGE("UDP receiver: failed to create thread");
        g_mutex_lock(&ur->lock);
        ur->running = FALSE;
        g_mutex_unlock(&ur->lock);
        if (ur->sockfd_primary >= 0) {
            close(ur->sockfd_primary);
            ur->sockfd_primary = -1;
        }
        if (ur->sockfd_secondary >= 0) {
            close(ur->sockfd_secondary);
            ur->sockfd_secondary = -1;
        }
        buffer_pool_stop(ur);
        return -1;
    }
    return 0;
}

void udp_receiver_stop(UdpReceiver *ur) {
    if (ur == NULL) {
        return;
    }
    g_mutex_lock(&ur->lock);
    if (!ur->running) {
        g_mutex_unlock(&ur->lock);
        return;
    }
    ur->stop_requested = TRUE;
    g_mutex_unlock(&ur->lock);

    if (ur->sockfd_primary >= 0) {
        shutdown(ur->sockfd_primary, SHUT_RDWR);
    }
    if (ur->sockfd_secondary >= 0) {
        shutdown(ur->sockfd_secondary, SHUT_RDWR);
    }

    if (ur->thread != NULL) {
        g_thread_join(ur->thread);
        ur->thread = NULL;
    }

    if (ur->sockfd_primary >= 0) {
        close(ur->sockfd_primary);
        ur->sockfd_primary = -1;
    }
    if (ur->sockfd_secondary >= 0) {
        close(ur->sockfd_secondary);
        ur->sockfd_secondary = -1;
    }

    rtsp_pipeline_stop(ur);

    gst_app_src_end_of_stream(ur->appsrc);

    buffer_pool_stop(ur);

    g_mutex_lock(&ur->lock);
    ur->running = FALSE;
    ur->stop_requested = FALSE;
    g_mutex_unlock(&ur->lock);
}

void udp_receiver_destroy(UdpReceiver *ur) {
    if (ur == NULL) {
        return;
    }
    udp_receiver_stop(ur);
    rtsp_pipeline_stop(ur);
    if (ur->appsrc != NULL) {
        gst_object_unref(ur->appsrc);
        ur->appsrc = NULL;
    }
    buffer_pool_stop(ur);
    g_mutex_clear(&ur->lock);
    g_free(ur);
}

void udp_receiver_get_stats(UdpReceiver *ur, UdpReceiverStats *stats) {
    if (ur == NULL || stats == NULL) {
        return;
    }
    g_mutex_lock(&ur->lock);
    *stats = ur->stats;
    memcpy(stats->history, ur->history, sizeof(ur->history));
    g_mutex_unlock(&ur->lock);
}

void udp_receiver_set_stats_enabled(UdpReceiver *ur, gboolean enabled) {
    if (ur == NULL) {
        return;
    }

    g_mutex_lock(&ur->lock);
    gboolean new_state = enabled ? TRUE : FALSE;
    gboolean changed = (ur->stats_enabled != new_state);
    ur->stats_enabled = new_state;
    if (changed && new_state) {
        reset_stats_locked(ur);
    }
    g_mutex_unlock(&ur->lock);
}
