#ifndef SSE_STREAMER_H
#define SSE_STREAMER_H

#include <glib.h>

#include "config.h"
#include "udp_receiver.h"

typedef struct {
    guint64 total_packets;
    guint64 video_packets;
    guint64 audio_packets;
    guint64 ignored_packets;
    guint64 duplicate_packets;
    guint64 lost_packets;
    guint64 reordered_packets;
    guint64 total_bytes;
    guint64 video_bytes;
    guint64 audio_bytes;
    guint64 frame_count;
    guint64 incomplete_frames;
    guint64 last_frame_bytes;
    double frame_size_avg;
    double jitter_ms;
    double jitter_avg_ms;
    double bitrate_mbps;
    double bitrate_avg_mbps;
    guint32 last_video_timestamp;
    guint16 expected_sequence;
    guint64 last_packet_ns;
    guint64 idr_requests;
} SseStatsSnapshot;

typedef struct {
    gboolean configured;
    int listen_fd;
    guint interval_ms;
    char bind_address[64];
    int port;
    GThread *accept_thread;
    GMutex lock;
    gboolean running;
    volatile gint shutdown_flag;
    volatile gint active_client;
    gboolean have_stats;
    SseStatsSnapshot stats;
} SseStreamer;

void sse_streamer_init(SseStreamer *streamer);
int sse_streamer_start(SseStreamer *streamer, const AppCfg *cfg);
void sse_streamer_publish(SseStreamer *streamer, const UdpReceiverStats *stats, gboolean have_stats);
void sse_streamer_stop(SseStreamer *streamer);
gboolean sse_streamer_requires_stats(const SseStreamer *streamer);

#endif // SSE_STREAMER_H
