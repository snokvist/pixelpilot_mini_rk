#include "udp_receiver.h"
#include "logging.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
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

#define RTP_MIN_HEADER 12
#define FRAME_EWMA_ALPHA 0.1
#define JITTER_EWMA_ALPHA 0.1
#define BITRATE_WINDOW_NS 100000000ULL // 100 ms
#define BITRATE_EWMA_ALPHA 0.1

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

    int udp_port;
    int vid_pt;
    int aud_pt;
    int sockfd;

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

static gpointer receiver_thread(gpointer data) {
    struct UdpReceiver *ur = (struct UdpReceiver *)data;
    const size_t max_pkt = 4096;
    guint8 *buffer = g_malloc(max_pkt);
    if (buffer == NULL) {
        LOGE("UDP receiver: allocation failed");
        return NULL;
    }

    while (TRUE) {
        g_mutex_lock(&ur->lock);
        gboolean stop = ur->stop_requested;
        g_mutex_unlock(&ur->lock);
        if (stop) {
            break;
        }

        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(ur->sockfd, buffer, max_pkt, 0, (struct sockaddr *)&src, &slen);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                g_usleep(1000);
                continue;
            }
            if (!ur->stop_requested) {
                LOGE("UDP receiver: recvfrom failed: %s", g_strerror(errno));
            }
            break;
        }

        guint64 arrival_ns = get_time_ns();
        g_mutex_lock(&ur->lock);
        process_rtp(ur, buffer, (gsize)n, arrival_ns);
        g_mutex_unlock(&ur->lock);

        GstBuffer *gstbuf = gst_buffer_new_allocate(NULL, (gsize)n, NULL);
        if (gstbuf == NULL) {
            continue;
        }
        gst_buffer_fill(gstbuf, 0, buffer, (gsize)n);
        GstFlowReturn flow = gst_app_src_push_buffer(ur->appsrc, gstbuf);
        if (flow != GST_FLOW_OK) {
            LOGV("UDP receiver: push_buffer returned %s", gst_flow_get_name(flow));
            gst_buffer_unref(gstbuf);
            if (flow == GST_FLOW_FLUSHING) {
                g_usleep(1000);
            }
        }
    }

    g_mutex_lock(&ur->lock);
    ur->running = FALSE;
    ur->stop_requested = FALSE;
    g_mutex_unlock(&ur->lock);

    g_free(buffer);
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
    g_mutex_init(&ur->lock);
    ur->appsrc = GST_APP_SRC(gst_object_ref(appsrc));
    return ur;
}

static int setup_socket(struct UdpReceiver *ur) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        LOGE("UDP receiver: socket(): %s", g_strerror(errno));
        return -1;
    }

    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        LOGW("UDP receiver: setsockopt(SO_REUSEADDR) failed: %s", g_strerror(errno));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)ur->udp_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("UDP receiver: bind(%d): %s", ur->udp_port, g_strerror(errno));
        close(fd);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000; // 500 ms timeout to allow graceful shutdown
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        LOGW("UDP receiver: setsockopt(SO_RCVTIMEO) failed: %s", g_strerror(errno));
    }

    int buf_bytes = 4 * 1024 * 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_bytes, sizeof(buf_bytes)) < 0) {
        LOGW("UDP receiver: setsockopt(SO_RCVBUF) failed: %s", g_strerror(errno));
    }

    ur->sockfd = fd;
    return 0;
}

int udp_receiver_start(UdpReceiver *ur) {
    if (ur == NULL) {
        return -1;
    }
    g_mutex_lock(&ur->lock);
    if (ur->running) {
        g_mutex_unlock(&ur->lock);
        return 0;
    }
    g_mutex_unlock(&ur->lock);

    if (setup_socket(ur) != 0) {
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
        close(ur->sockfd);
        ur->sockfd = -1;
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
