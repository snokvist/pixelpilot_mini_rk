#ifndef PIPELINE_H
#define PIPELINE_H

#include <glib.h>
#include <gst/gst.h>

#include "config.h"
#include "udp_receiver.h"

typedef enum {
    PIPELINE_STOPPED = 0,
    PIPELINE_RUNNING = 1,
    PIPELINE_STOPPING = 2
} PipelineStateEnum;

typedef struct {
    PipelineStateEnum state;
    GstElement *pipeline;
    GstElement *rtpbin;
    GstElement *video_queue;
    GstElement *audio_queue;
    GstElement *audio_sink;
    GThread *bus_thread;
    GMutex lock;
    GCond cond;
    gboolean initialized;
    gboolean bus_thread_running;
    gboolean stop_requested;
    gboolean encountered_error;
    gboolean audio_disabled;
    const AppCfg *cfg;
} PipelineState;

int pipeline_start(const AppCfg *cfg, int audio_disabled, PipelineState *ps);
void pipeline_stop(PipelineState *ps, int wait_ms_total);
void pipeline_poll_child(PipelineState *ps);
int pipeline_get_receiver_stats(const PipelineState *ps, UdpReceiverStats *stats);
void pipeline_set_receiver_stats_enabled(PipelineState *ps, gboolean enabled);

#endif // PIPELINE_H
