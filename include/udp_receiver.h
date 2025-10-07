#ifndef UDP_RECEIVER_H
#define UDP_RECEIVER_H

#include <glib.h>
#include <gst/gst.h>
#include <stddef.h>
#include <stdint.h>

#include <gst/app/gstappsrc.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UDP_RECEIVER_HISTORY 512

#define UDP_SAMPLE_FLAG_LOSS 0x01
#define UDP_SAMPLE_FLAG_REORDER 0x02
#define UDP_SAMPLE_FLAG_DUPLICATE 0x04
#define UDP_SAMPLE_FLAG_FRAME_END 0x08

typedef struct {
    guint16 sequence;
    guint32 timestamp;
    guint8 payload_type;
    guint8 marker;
    guint8 flags;
    guint32 size;
    guint64 arrival_ns;
} UdpReceiverPacketSample;

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
    double jitter;
    double jitter_avg;
    double bitrate_mbps;
    double bitrate_avg_mbps;
    guint32 last_video_timestamp;
    guint16 expected_sequence;
    size_t history_count;
    size_t history_head;
    UdpReceiverPacketSample history[UDP_RECEIVER_HISTORY];
    guint64 last_packet_ns;
    guint64 idr_requests;
} UdpReceiverStats;

typedef struct UdpReceiver UdpReceiver;
struct IdrRequester;

UdpReceiver *udp_receiver_create(int udp_port, int vid_pt, int aud_pt, GstAppSrc *video_appsrc, struct IdrRequester *requester);
void udp_receiver_set_audio_appsrc(UdpReceiver *ur, GstAppSrc *audio_appsrc);
void udp_receiver_set_wake_fd(UdpReceiver *ur, int wake_fd);
int udp_receiver_start(UdpReceiver *ur, const AppCfg *cfg, int cpu_slot);
void udp_receiver_stop(UdpReceiver *ur);
void udp_receiver_destroy(UdpReceiver *ur);
void udp_receiver_get_stats(UdpReceiver *ur, UdpReceiverStats *stats);
void udp_receiver_set_stats_enabled(UdpReceiver *ur, gboolean enabled);
guint64 udp_receiver_get_last_packet_time(UdpReceiver *ur);
#ifdef __cplusplus
}
#endif

#endif // UDP_RECEIVER_H
