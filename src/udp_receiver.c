#define _GNU_SOURCE

#include "udp_receiver.h"
#include "logging.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
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
#define UDP_MAX_PACKET 4096
#define UDP_POOL_SIZE 64
#define UDP_RECV_BATCH 8
#define UDP_RECV_TIMEOUT_NS 500000000L

struct UdpReceiver;

typedef struct {
    struct UdpReceiver *owner;
    guint8 *data;
    gsize capacity;
    struct iovec iov;
    struct sockaddr_in addr;
} UdpBufferSlot;

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
    gboolean stats_enabled;
    gint stop_requested;
    gint stats_sequence;

    int udp_port;
    int vid_pt;
    int aud_pt;
    int sockfd;

    GAsyncQueue *buffer_pool;
    guint buffer_pool_size;
    gsize buffer_capacity;
    gint pool_outstanding;
    gint pool_shutdown;
    GMutex pool_mutex;
    GCond pool_cond;

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

static inline void stats_write_begin(struct UdpReceiver *ur) {
    g_atomic_int_inc(&ur->stats_sequence);
}

static inline void stats_write_end(struct UdpReceiver *ur) {
    g_atomic_int_inc(&ur->stats_sequence);
}

static void reset_stats(struct UdpReceiver *ur) {
    stats_write_begin(ur);
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
    stats_write_end(ur);
}

static UdpBufferSlot *udp_buffer_slot_new(struct UdpReceiver *ur) {
    UdpBufferSlot *slot = g_new0(UdpBufferSlot, 1);
    if (slot == NULL) {
        return NULL;
    }
    slot->owner = ur;
    slot->capacity = ur->buffer_capacity;
    slot->data = g_malloc(slot->capacity);
    if (slot->data == NULL) {
        g_free(slot);
        return NULL;
    }
    slot->iov.iov_base = slot->data;
    slot->iov.iov_len = slot->capacity;
    memset(&slot->addr, 0, sizeof(slot->addr));
    return slot;
}

static void udp_buffer_slot_free(UdpBufferSlot *slot) {
    if (slot == NULL) {
        return;
    }
    g_free(slot->data);
    g_free(slot);
}

static void udp_buffer_slot_recycle(UdpBufferSlot *slot) {
    if (slot == NULL || slot->owner == NULL) {
        udp_buffer_slot_free(slot);
        return;
    }
    slot->iov.iov_base = slot->data;
    slot->iov.iov_len = slot->capacity;
    memset(&slot->addr, 0, sizeof(slot->addr));
    g_async_queue_push(slot->owner->buffer_pool, slot);
}

static void udp_buffer_slot_release(gpointer data) {
    UdpBufferSlot *slot = (UdpBufferSlot *)data;
    if (slot == NULL) {
        return;
    }
    struct UdpReceiver *ur = slot->owner;
    if (G_UNLIKELY(ur == NULL)) {
        udp_buffer_slot_free(slot);
        return;
    }
    udp_buffer_slot_recycle(slot);
    gint previous = g_atomic_int_add(&ur->pool_outstanding, -1);
    if (previous == 1 && g_atomic_int_get(&ur->pool_shutdown)) {
        g_mutex_lock(&ur->pool_mutex);
        g_cond_signal(&ur->pool_cond);
        g_mutex_unlock(&ur->pool_mutex);
    }
}

static gboolean udp_buffer_pool_prepare(struct UdpReceiver *ur) {
    if (ur->buffer_pool == NULL) {
        ur->buffer_pool = g_async_queue_new();
    }
    if (ur->buffer_pool == NULL) {
        return FALSE;
    }
    g_atomic_int_set(&ur->pool_outstanding, 0);
    g_atomic_int_set(&ur->pool_shutdown, 0);
    for (guint i = 0; i < ur->buffer_pool_size; ++i) {
        UdpBufferSlot *slot = udp_buffer_slot_new(ur);
        if (slot == NULL) {
            return FALSE;
        }
        g_async_queue_push(ur->buffer_pool, slot);
    }
    return TRUE;
}

static void udp_buffer_pool_flush(struct UdpReceiver *ur) {
    if (ur->buffer_pool == NULL) {
        return;
    }
    g_atomic_int_set(&ur->pool_outstanding, 0);
    while (TRUE) {
        UdpBufferSlot *slot = g_async_queue_try_pop(ur->buffer_pool);
        if (slot == NULL) {
            break;
        }
        slot->owner = NULL;
        udp_buffer_slot_free(slot);
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

    if (!ur->stats_enabled) {
        return;
    }

    stats_write_begin(ur);
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
    stats_write_end(ur);
}

static gpointer receiver_thread(gpointer data) {
    struct UdpReceiver *ur = (struct UdpReceiver *)data;

    cpu_set_t thread_mask;
    if (cfg_get_thread_affinity(ur->cfg, ur->cpu_slot, &thread_mask)) {
        int err = pthread_setaffinity_np(pthread_self(), sizeof(thread_mask), &thread_mask);
        if (err != 0) {
            LOGW("UDP receiver: pthread_setaffinity_np failed: %s", g_strerror(err));
        }
    }

    struct mmsghdr msgs[UDP_RECV_BATCH];
    UdpBufferSlot *slots[UDP_RECV_BATCH];

    while (!g_atomic_int_get(&ur->stop_requested)) {
        guint prepared = 0;
        for (; prepared < UDP_RECV_BATCH; ++prepared) {
            UdpBufferSlot *slot = g_async_queue_try_pop(ur->buffer_pool);
            if (slot == NULL) {
                slot = udp_buffer_slot_new(ur);
                if (slot == NULL) {
                    break;
                }
            }
            slots[prepared] = slot;
            memset(&msgs[prepared], 0, sizeof(struct mmsghdr));
            msgs[prepared].msg_hdr.msg_iov = &slot->iov;
            msgs[prepared].msg_hdr.msg_iovlen = 1;
            msgs[prepared].msg_hdr.msg_name = &slot->addr;
            msgs[prepared].msg_hdr.msg_namelen = sizeof(slot->addr);
        }

        if (prepared == 0) {
            g_usleep(1000);
            continue;
        }

        struct timespec timeout = {
            .tv_sec = 0,
            .tv_nsec = UDP_RECV_TIMEOUT_NS,
        };

        int received = recvmmsg(ur->sockfd, msgs, prepared, MSG_WAITFORONE, &timeout);
        if (received <= 0) {
            int err = (received < 0) ? errno : 0;
            for (guint i = 0; i < prepared; ++i) {
                udp_buffer_slot_recycle(slots[i]);
            }
            if (received == 0) {
                continue;
            }
            if (err == EINTR) {
                continue;
            }
            if (err == EAGAIN || err == EWOULDBLOCK) {
                g_usleep(1000);
                continue;
            }
            if (!g_atomic_int_get(&ur->stop_requested)) {
                LOGE("UDP receiver: recvmmsg failed: %s", g_strerror(err));
            }
            break;
        }

        guint64 arrival_base = get_time_ns();
        for (int i = 0; i < received; ++i) {
            UdpBufferSlot *slot = slots[i];
            slots[i] = NULL;
            ssize_t len = msgs[i].msg_len;
            if (len <= 0 || (msgs[i].msg_hdr.msg_flags & MSG_TRUNC)) {
                udp_buffer_slot_recycle(slot);
                continue;
            }

            GstBuffer *gstbuf = gst_buffer_new_wrapped_full(0, slot->data, slot->capacity, 0, (gsize)len, slot, udp_buffer_slot_release);
            if (gstbuf == NULL) {
                udp_buffer_slot_recycle(slot);
                continue;
            }

            g_atomic_int_inc(&ur->pool_outstanding);
            guint64 arrival_ns = (received > 1) ? get_time_ns() : arrival_base;
            process_rtp(ur, slot->data, (gsize)len, arrival_ns);

            GstFlowReturn flow = gst_app_src_push_buffer(ur->appsrc, gstbuf);
            if (flow != GST_FLOW_OK) {
                LOGV("UDP receiver: push_buffer returned %s", gst_flow_get_name(flow));
                if (flow == GST_FLOW_FLUSHING) {
                    g_usleep(1000);
                }
            }
        }

        for (guint i = (guint)received; i < prepared; ++i) {
            udp_buffer_slot_recycle(slots[i]);
        }
    }

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
    ur->stats_enabled = FALSE;
    ur->buffer_pool = NULL;
    ur->buffer_pool_size = UDP_POOL_SIZE;
    ur->buffer_capacity = UDP_MAX_PACKET;
    g_atomic_int_set(&ur->pool_outstanding, 0);
    g_atomic_int_set(&ur->pool_shutdown, 0);
    g_atomic_int_set(&ur->stop_requested, 0);
    g_atomic_int_set(&ur->stats_sequence, 0);
    g_mutex_init(&ur->pool_mutex);
    g_cond_init(&ur->pool_cond);
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

    int buf_bytes = 4 * 1024 * 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_bytes, sizeof(buf_bytes)) < 0) {
        LOGW("UDP receiver: setsockopt(SO_RCVBUF) failed: %s", g_strerror(errno));
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOGW("UDP receiver: fcntl(O_NONBLOCK) failed: %s", g_strerror(errno));
    }

    ur->sockfd = fd;
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
    reset_stats(ur);
    ur->cfg = cfg;
    ur->cpu_slot = cpu_slot;
    g_mutex_unlock(&ur->lock);

    if (!udp_buffer_pool_prepare(ur)) {
        LOGE("UDP receiver: failed to prepare buffer pool");
        udp_buffer_pool_flush(ur);
        return -1;
    }

    if (setup_socket(ur) != 0) {
        udp_buffer_pool_flush(ur);
        return -1;
    }

    g_mutex_lock(&ur->lock);
    g_atomic_int_set(&ur->stop_requested, 0);
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
        udp_buffer_pool_flush(ur);
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
    g_atomic_int_set(&ur->stop_requested, 1);
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

    g_atomic_int_set(&ur->pool_shutdown, 1);
    g_mutex_lock(&ur->pool_mutex);
    while (g_atomic_int_get(&ur->pool_outstanding) != 0) {
        g_cond_wait(&ur->pool_cond, &ur->pool_mutex);
    }
    g_mutex_unlock(&ur->pool_mutex);
    udp_buffer_pool_flush(ur);
    g_atomic_int_set(&ur->pool_shutdown, 0);

    gst_app_src_end_of_stream(ur->appsrc);

    g_mutex_lock(&ur->lock);
    ur->running = FALSE;
    g_mutex_unlock(&ur->lock);
    g_atomic_int_set(&ur->stop_requested, 0);
}

void udp_receiver_destroy(UdpReceiver *ur) {
    if (ur == NULL) {
        return;
    }
    udp_receiver_stop(ur);
    udp_buffer_pool_flush(ur);
    if (ur->buffer_pool != NULL) {
        g_async_queue_unref(ur->buffer_pool);
        ur->buffer_pool = NULL;
    }
    if (ur->appsrc != NULL) {
        gst_object_unref(ur->appsrc);
        ur->appsrc = NULL;
    }
    g_mutex_clear(&ur->lock);
    g_mutex_clear(&ur->pool_mutex);
    g_cond_clear(&ur->pool_cond);
    g_free(ur);
}

void udp_receiver_get_stats(UdpReceiver *ur, UdpReceiverStats *stats) {
    if (ur == NULL || stats == NULL) {
        return;
    }
    UdpReceiverStats snapshot;
    UdpReceiverPacketSample history[UDP_RECEIVER_HISTORY];

    while (TRUE) {
        gint start = g_atomic_int_get(&ur->stats_sequence);
        if (start & 1) {
            continue;
        }
        snapshot = ur->stats;
        memcpy(history, ur->history, sizeof(history));
        gint end = g_atomic_int_get(&ur->stats_sequence);
        if (start == end) {
            break;
        }
    }

    *stats = snapshot;
    memcpy(stats->history, history, sizeof(history));
}

void udp_receiver_set_stats_enabled(UdpReceiver *ur, gboolean enabled) {
    if (ur == NULL) {
        return;
    }

    gboolean new_state = enabled ? TRUE : FALSE;
    gboolean activate = FALSE;

    g_mutex_lock(&ur->lock);
    if (ur->stats_enabled == new_state) {
        g_mutex_unlock(&ur->lock);
        return;
    }
    if (new_state) {
        activate = TRUE;
    } else {
        ur->stats_enabled = FALSE;
    }
    g_mutex_unlock(&ur->lock);

    if (activate) {
        reset_stats(ur);
        g_mutex_lock(&ur->lock);
        ur->stats_enabled = TRUE;
        g_mutex_unlock(&ur->lock);
    }
}
