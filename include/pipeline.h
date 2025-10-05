#ifndef PIPELINE_H
#define PIPELINE_H

#include <glib.h>
#include <gst/gst.h>

#include "udp_receiver.h"
#include "video_decoder.h"

typedef enum {
    PIPELINE_STOPPED = 0,
    PIPELINE_RUNNING = 1,
    PIPELINE_STOPPING = 2
} PipelineStateEnum;

typedef struct {
    PipelineStateEnum state;
    GstElement *pipeline;
    GstElement *source;
    GstElement *rtpbin;
    GstElement *video_sink;
    GstElement *video_branch_entry;
    GstElement *audio_branch_entry;
    GstPad *video_pad;
    GstPad *audio_pad;
    UdpReceiver *udp_receiver;
    GThread *bus_thread;
    GThread *appsink_thread;
    GMutex lock;
    GCond cond;
    gboolean initialized;
    gboolean bus_thread_running;
    gboolean stop_requested;
    gboolean encountered_error;
    int audio_disabled;
    const AppCfg *cfg;
    int bus_thread_cpu_slot;
    gboolean appsink_thread_running;
    VideoDecoder *decoder;
    gboolean decoder_initialized;
    gboolean decoder_running;
} PipelineState;

#include "config.h"

int pipeline_start(const AppCfg *cfg, const ModesetResult *ms, int drm_fd, int audio_disabled, PipelineState *ps);
void pipeline_stop(PipelineState *ps, int wait_ms_total);
void pipeline_poll_child(PipelineState *ps);
int pipeline_get_receiver_stats(const PipelineState *ps, UdpReceiverStats *stats);
void pipeline_set_receiver_stats_enabled(PipelineState *ps, gboolean enabled);

#endif // PIPELINE_H
