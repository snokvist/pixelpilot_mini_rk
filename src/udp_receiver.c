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
#include <sys/uio.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(PIXELPILOT_DISABLE_NEON)
#define PIXELPILOT_NEON_AVAILABLE 0
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(PIXELPILOT_HAS_NEON)
#define PIXELPILOT_NEON_AVAILABLE 1
#else
#define PIXELPILOT_NEON_AVAILABLE 0
#endif

#if PIXELPILOT_NEON_AVAILABLE
#include <arm_neon.h>
#endif

#ifndef GST_USE_UNSTABLE_API
#define GST_USE_UNSTABLE_API
#endif

#include <gst/app/gstappsrc.h>
#include <gst/gstbuffer.h>
#include <gst/gstbufferpool.h>
#include <gst/gstevent.h>
#include <gst/gstsegment.h>

#define RTP_MIN_HEADER 12
#define FRAME_EWMA_ALPHA 0.1
#define JITTER_EWMA_ALPHA 0.1
#define BITRATE_WINDOW_NS 100000000ULL // 100 ms
#define BITRATE_EWMA_ALPHA 0.1
#define UDP_RECEIVER_MAX_PACKET 4096
#define UDP_RECEIVER_BATCH 8

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
    int vid_pt;
    int aud_pt;
    int sockfd;
    gboolean discont_pending;

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

static inline void neon_copy_bytes(guint8 *dst, const guint8 *src, size_t size) {
#if PIXELPILOT_NEON_AVAILABLE
    if (G_UNLIKELY(size == 0)) {
        return;
    }

    size_t offset = 0;
    while (offset + 64 <= size) {
        vst1q_u8(dst + offset, vld1q_u8(src + offset));
        vst1q_u8(dst + offset + 16, vld1q_u8(src + offset + 16));
        vst1q_u8(dst + offset + 32, vld1q_u8(src + offset + 32));
        vst1q_u8(dst + offset + 48, vld1q_u8(src + offset + 48));
        offset += 64;
    }
    while (offset + 16 <= size) {
        vst1q_u8(dst + offset, vld1q_u8(src + offset));
        offset += 16;
    }
    if (offset + 8 <= size) {
        vst1_u8(dst + offset, vld1_u8(src + offset));
        offset += 8;
    }
    if (offset < size) {
        memcpy(dst + offset, src + offset, size - offset);
    }
#else
    if (size > 0) {
        memcpy(dst, src, size);
    }
#endif
}

static inline void copy_sample(UdpReceiverPacketSample *dst, const UdpReceiverPacketSample *src) {
    neon_copy_bytes((guint8 *)dst, (const guint8 *)src, sizeof(*dst));
}

static inline void copy_history(UdpReceiverPacketSample *dst,
                                const UdpReceiverPacketSample *src,
                                size_t count) {
    neon_copy_bytes((guint8 *)dst, (const guint8 *)src, count * sizeof(*dst));
}

static void history_push(struct UdpReceiver *ur, const UdpReceiverPacketSample *sample) {
    copy_sample(&ur->history[ur->stats.history_head], sample);
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

static void log_udp_neon_status_once(void) {
    static gsize once_init = 0;
    if (g_once_init_enter(&once_init)) {
#if defined(PIXELPILOT_DISABLE_NEON)
        LOGI("UDP receiver: NEON history acceleration disabled by build flag");
#elif PIXELPILOT_NEON_AVAILABLE
        LOGI("UDP receiver: NEON history acceleration enabled");
#else
        LOGI("UDP receiver: NEON history acceleration unavailable on this build");
#endif
        g_once_init_leave(&once_init, 1);
    }
}

static void push_stream_reset_events(struct UdpReceiver *ur) {
    if (ur == NULL || ur->appsrc == NULL) {
        return;
    }

    GstEvent *event = gst_event_new_flush_start();
    if (!gst_element_send_event(GST_ELEMENT(ur->appsrc), event)) {
        LOGW("UDP receiver: failed to push flush-start event");
    }

    event = gst_event_new_flush_stop(TRUE);
    if (!gst_element_send_event(GST_ELEMENT(ur->appsrc), event)) {
        LOGW("UDP receiver: failed to push flush-stop event");
    }

    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    event = gst_event_new_segment(&segment);
    if (!gst_element_send_event(GST_ELEMENT(ur->appsrc), event)) {
        LOGW("UDP receiver: failed to push new-segment event");
    }
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

static void process_rtp(struct UdpReceiver *ur,
                        const guint8 *data,
                        gsize len,
                        guint64 arrival_ns,
                        const RtpParseResult *parsed) {
    if (!ur->stats_enabled) {
        return;
    }

    RtpParseResult local;
    if (parsed == NULL) {
        if (!parse_rtp(data, len, &local)) {
            return;
        }
        parsed = &local;
    }

    gboolean is_video = (parsed->payload_type == ur->vid_pt);
    gboolean is_audio = (parsed->payload_type == ur->aud_pt);

    UdpReceiverPacketSample sample = {0};
    sample.sequence = parsed->sequence;
    sample.timestamp = parsed->timestamp;
    sample.payload_type = parsed->payload_type;
    sample.marker = parsed->marker;
    sample.size = (guint32)len;
    sample.arrival_ns = arrival_ns;

    ur->stats.total_packets++;
    ur->stats.total_bytes += len;

    if (is_video) {
        ur->stats.video_packets++;
        ur->stats.video_bytes += len;
        ur->stats.last_video_timestamp = parsed->timestamp;

        if (!ur->frame_active || ur->frame_timestamp != parsed->timestamp) {
            if (ur->frame_active) {
                finalize_frame(ur);
            }
            ur->frame_active = TRUE;
            ur->frame_timestamp = parsed->timestamp;
            ur->frame_bytes = len;
            ur->frame_missing = FALSE;
        } else {
            ur->frame_bytes += len;
        }

        if (!ur->seq_initialized) {
            ur->seq_initialized = TRUE;
            ur->expected_seq = seq_next(parsed->sequence);
            ur->stats.expected_sequence = ur->expected_seq;
        } else {
            gint16 delta = seq_delta(parsed->sequence, ur->expected_seq);
            if (delta == 0) {
                ur->expected_seq = seq_next(parsed->sequence);
            } else if (delta > 0) {
                ur->stats.lost_packets += delta;
                ur->expected_seq = seq_next(parsed->sequence);
                ur->frame_missing = TRUE;
                sample.flags |= UDP_SAMPLE_FLAG_LOSS;
            } else { // delta < 0
                ur->stats.reordered_packets++;
                sample.flags |= UDP_SAMPLE_FLAG_REORDER;
            }
            ur->stats.expected_sequence = ur->expected_seq;
        }

        if (ur->have_last_seq && parsed->sequence == ur->last_seq) {
            ur->stats.duplicate_packets++;
            sample.flags |= UDP_SAMPLE_FLAG_DUPLICATE;
        }
        ur->last_seq = parsed->sequence;
        ur->have_last_seq = TRUE;

        double arrival_rtp = (double)arrival_ns / 1e9 * 90000.0;
        double transit = arrival_rtp - (double)parsed->timestamp;
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

    if (parsed->marker && is_video) {
        sample.flags |= UDP_SAMPLE_FLAG_FRAME_END;
        finalize_frame(ur);
    }

    update_bitrate(ur, arrival_ns, (guint32)len);
    history_push(ur, &sample);
}

static gboolean handle_received_packet(struct UdpReceiver *ur,
                                       GstBuffer *gstbuf,
                                       GstMapInfo *map,
                                       gssize bytes_read) {
    if (bytes_read <= 0) {
        gst_buffer_unmap(gstbuf, map);
        gst_buffer_unref(gstbuf);
        return FALSE;
    }

    gst_buffer_set_size(gstbuf, (gsize)bytes_read);

    gboolean drop_packet = FALSE;
    RtpParseResult preview;
    gboolean have_preview = FALSE;
    gboolean preview_initialized = FALSE;
    gboolean filter_non_video = FALSE;
    if (ur->cfg != NULL && ur->cfg->no_audio) {
        filter_non_video = TRUE;
    }
    if (ur->aud_pt < 0) {
        filter_non_video = TRUE;
    }
    if (filter_non_video) {
        have_preview = parse_rtp(map->data, (gsize)bytes_read, &preview);
        preview_initialized = TRUE;
    }

    guint64 arrival_ns = get_time_ns();
    gboolean mark_discont = FALSE;
    g_mutex_lock(&ur->lock);
    if (ur->discont_pending) {
        mark_discont = TRUE;
        ur->discont_pending = FALSE;
    }
    if (!preview_initialized && ur->stats_enabled) {
        have_preview = parse_rtp(map->data, (gsize)bytes_read, &preview);
        preview_initialized = TRUE;
    }
    if (filter_non_video && have_preview && preview.payload_type != ur->vid_pt) {
        drop_packet = TRUE;
    } else {
        process_rtp(ur, map->data, (gsize)bytes_read, arrival_ns, have_preview ? &preview : NULL);
    }
    g_mutex_unlock(&ur->lock);

    gst_buffer_unmap(gstbuf, map);

    if (drop_packet) {
        gst_buffer_unref(gstbuf);
        return TRUE;
    }

    if (mark_discont) {
        GST_BUFFER_FLAG_SET(gstbuf, GST_BUFFER_FLAG_DISCONT);
        GST_BUFFER_FLAG_SET(gstbuf, GST_BUFFER_FLAG_RESYNC);
    }

    GstFlowReturn flow = gst_app_src_push_buffer(ur->appsrc, gstbuf);
    if (flow != GST_FLOW_OK) {
        LOGV("UDP receiver: push_buffer returned %s", gst_flow_get_name(flow));
        if (flow == GST_FLOW_FLUSHING) {
            g_usleep(1000);
        }
    }

    return TRUE;
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

        struct pollfd fds[1];
        int nfds = 0;
        int idx_socket = -1;
        if (ur->sockfd >= 0) {
            idx_socket = nfds;
            fds[nfds].fd = ur->sockfd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        if (nfds == 0) {
            g_usleep(1000);
            continue;
        }

        int pret = poll(fds, nfds, poll_timeout_ms);
        if (pret < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOGE("UDP receiver: poll failed: %s", g_strerror(errno));
            break;
        }
        if (pret == 0) {
            continue;
        }

        if (idx_socket >= 0 && (fds[idx_socket].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            LOGW("UDP receiver: socket error (events=0x%x)", fds[idx_socket].revents);
        }

        if (idx_socket < 0 || !(fds[idx_socket].revents & POLLIN)) {
            continue;
        }

        int active_fd = ur->sockfd;

        GstBuffer *buffers[UDP_RECEIVER_BATCH] = {0};
        GstMapInfo maps[UDP_RECEIVER_BATCH];
        struct mmsghdr msgs[UDP_RECEIVER_BATCH];
        struct iovec iovecs[UDP_RECEIVER_BATCH];
        guint prepared = 0;

        while (prepared < UDP_RECEIVER_BATCH) {
            GstBuffer *gstbuf = NULL;
            GstFlowReturn flow = gst_buffer_pool_acquire_buffer(ur->pool, &gstbuf, NULL);
            if (flow != GST_FLOW_OK || gstbuf == NULL) {
                LOGE("UDP receiver: failed to acquire buffer from pool (flow=%s)",
                     gst_flow_get_name(flow));
                if (gstbuf != NULL) {
                    gst_buffer_unref(gstbuf);
                }
                break;
            }
            if (!gst_buffer_map(gstbuf, &maps[prepared], GST_MAP_WRITE)) {
                LOGE("UDP receiver: buffer map failed");
                gst_buffer_unref(gstbuf);
                break;
            }

            buffers[prepared] = gstbuf;
            memset(&msgs[prepared], 0, sizeof(msgs[prepared]));
            iovecs[prepared].iov_base = maps[prepared].data;
            iovecs[prepared].iov_len = max_pkt;
            msgs[prepared].msg_hdr.msg_iov = &iovecs[prepared];
            msgs[prepared].msg_hdr.msg_iovlen = 1;
            prepared++;
        }

        if (prepared == 0) {
            g_usleep(1000);
            continue;
        }

        int flags = 0;
#ifdef MSG_WAITFORONE
        flags |= MSG_WAITFORONE;
#endif
        int received = recvmmsg(active_fd, msgs, prepared, flags, NULL);
        if (received < 0) {
            int err = errno;
            for (guint i = 0; i < prepared; i++) {
                if (buffers[i] != NULL) {
                    gst_buffer_unmap(buffers[i], &maps[i]);
                    gst_buffer_unref(buffers[i]);
                }
            }
            if (err == EINTR) {
                continue;
            }
            if (err == EAGAIN || err == EWOULDBLOCK) {
                g_usleep(1000);
                continue;
            }
            if (!ur->stop_requested) {
                LOGE("UDP receiver: recvmmsg failed: %s", g_strerror(err));
            }
            break;
        }

        for (int i = received; i < (int)prepared; i++) {
            if (buffers[i] != NULL) {
                gst_buffer_unmap(buffers[i], &maps[i]);
                gst_buffer_unref(buffers[i]);
            }
        }

        for (int i = 0; i < received; i++) {
            if (msgs[i].msg_hdr.msg_flags & MSG_TRUNC) {
                LOGW("UDP receiver: truncated UDP packet dropped");
                gst_buffer_unmap(buffers[i], &maps[i]);
                gst_buffer_unref(buffers[i]);
                continue;
            }

            gssize n = (gssize)msgs[i].msg_len;
            handle_received_packet(ur, buffers[i], &maps[i], n);
        }
    }

    g_mutex_lock(&ur->lock);
    ur->running = FALSE;
    ur->stop_requested = FALSE;
    g_mutex_unlock(&ur->lock);
    return NULL;
}

UdpReceiver *udp_receiver_create(int udp_port, int vid_pt, int aud_pt, GstAppSrc *appsrc) {
    if (appsrc == NULL) {
        return NULL;
    }
    struct UdpReceiver *ur = g_new0(struct UdpReceiver, 1);
    if (ur == NULL) {
        return NULL;
    }
    ur->udp_port = udp_port;
    ur->vid_pt = vid_pt;
    ur->aud_pt = aud_pt;
    ur->sockfd = -1;
    ur->discont_pending = TRUE;
    g_mutex_init(&ur->lock);
    ur->appsrc = GST_APP_SRC(gst_object_ref(appsrc));
    ur->stats_enabled = FALSE;
    ur->pool = NULL;
    ur->buffer_size = 0;
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

    if (setup_socket_for_port(ur->udp_port, &ur->sockfd, "udp") != 0) {
        return -1;
    }

    if (!buffer_pool_start(ur)) {
        LOGE("UDP receiver: failed to start buffer pool");
        if (ur->sockfd >= 0) {
            close(ur->sockfd);
            ur->sockfd = -1;
        }
        return -1;
    }

    push_stream_reset_events(ur);

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
        if (ur->sockfd >= 0) {
            close(ur->sockfd);
            ur->sockfd = -1;
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

    if (ur->sockfd >= 0) {
        shutdown(ur->sockfd, SHUT_RDWR);
    }

    if (ur->thread != NULL) {
        g_thread_join(ur->thread);
        ur->thread = NULL;
    }

    if (ur->sockfd >= 0) {
        close(ur->sockfd);
        ur->sockfd = -1;
    }

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
    neon_copy_bytes((guint8 *)stats, (const guint8 *)&ur->stats, sizeof(*stats));
    copy_history(stats->history, ur->history, UDP_RECEIVER_HISTORY);
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
        log_udp_neon_status_once();
        reset_stats_locked(ur);
    }
    g_mutex_unlock(&ur->lock);
}
