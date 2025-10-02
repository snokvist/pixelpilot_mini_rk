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
#include <gst/gstbufferpool.h>

#define RTP_MIN_HEADER 12
#define FRAME_EWMA_ALPHA 0.1
#define JITTER_EWMA_ALPHA 0.1
#define BITRATE_WINDOW_NS 100000000ULL // 100 ms
#define BITRATE_EWMA_ALPHA 0.1
#define UDP_RECEIVER_MAX_PACKET 4096

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
    gboolean fallback_enabled;
    gboolean using_fallback;
    guint64 last_primary_data_ns;

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
                LOGI("UDP receiver: switching to fallback port %d", ur->udp_fallback_port);
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
        if (ur->fallback_enabled && ur->sockfd_secondary >= 0) {
            idx_fallback = nfds;
            fds[nfds].fd = ur->sockfd_secondary;
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

        gboolean primary_ready = (idx_primary >= 0) && (fds[idx_primary].revents & POLLIN);
        gboolean fallback_ready = (idx_fallback >= 0) && (fds[idx_fallback].revents & POLLIN);

        if (idx_primary >= 0 && (fds[idx_primary].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            LOGW("UDP receiver: primary socket error (events=0x%x)", fds[idx_primary].revents);
        }
        if (idx_fallback >= 0 && (fds[idx_fallback].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            LOGW("UDP receiver: fallback socket error (events=0x%x)", fds[idx_fallback].revents);
        }

        int active_fd = -1;
        gboolean active_is_primary = FALSE;

        if (primary_ready) {
            if (ur->using_fallback) {
                LOGI("UDP receiver: switching back to primary port %d", ur->udp_port);
            }
            ur->using_fallback = FALSE;
            active_fd = ur->sockfd_primary;
            active_is_primary = TRUE;
        } else if (fallback_ready && ur->fallback_enabled) {
            if (!ur->using_fallback) {
                ur->using_fallback = TRUE;
                LOGI("UDP receiver: switching to fallback port %d", ur->udp_fallback_port);
            }
            if (ur->using_fallback) {
                active_fd = ur->sockfd_secondary;
            }
        }

        if (active_fd < 0) {
            continue;
        }

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
        g_mutex_lock(&ur->lock);
        if (active_is_primary) {
            ur->last_primary_data_ns = arrival_ns;
        }
        process_rtp(ur, map.data, (gsize)n, arrival_ns);
        g_mutex_unlock(&ur->lock);

        gst_buffer_unmap(gstbuf, &map);

        flow = gst_app_src_push_buffer(ur->appsrc, gstbuf);
        if (flow != GST_FLOW_OK) {
            LOGV("UDP receiver: push_buffer returned %s", gst_flow_get_name(flow));
            if (flow == GST_FLOW_FLUSHING) {
                g_usleep(1000);
            }
        }
    }

    g_mutex_lock(&ur->lock);
    ur->running = FALSE;
    ur->stop_requested = FALSE;
    g_mutex_unlock(&ur->lock);
    return NULL;
}

UdpReceiver *udp_receiver_create(int udp_port, int fallback_port, int vid_pt, int aud_pt, GstAppSrc *appsrc) {
    if (appsrc == NULL) {
        return NULL;
    }
    struct UdpReceiver *ur = g_new0(struct UdpReceiver, 1);
    if (ur == NULL) {
        return NULL;
    }
    ur->udp_port = udp_port;
    ur->udp_fallback_port = fallback_port;
    ur->vid_pt = vid_pt;
    ur->aud_pt = aud_pt;
    ur->sockfd_primary = -1;
    ur->sockfd_secondary = -1;
    ur->fallback_enabled = (fallback_port > 0 && fallback_port != udp_port);
    ur->using_fallback = FALSE;
    ur->last_primary_data_ns = get_time_ns();
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
    ur->cfg = cfg;
    ur->cpu_slot = cpu_slot;
    g_mutex_unlock(&ur->lock);

    if (setup_socket_for_port(ur->udp_port, &ur->sockfd_primary, "primary") != 0) {
        return -1;
    }

    if (ur->fallback_enabled) {
        if (setup_socket_for_port(ur->udp_fallback_port, &ur->sockfd_secondary, "fallback") != 0) {
            LOGW("UDP receiver: failed to bind fallback port %d, disabling fallback", ur->udp_fallback_port);
            ur->sockfd_secondary = -1;
            ur->fallback_enabled = FALSE;
            ur->using_fallback = FALSE;
        }
    }

    ur->using_fallback = FALSE;
    ur->last_primary_data_ns = get_time_ns();

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
