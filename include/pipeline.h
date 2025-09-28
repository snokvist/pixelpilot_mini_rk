#ifndef PIPELINE_H
#define PIPELINE_H

#include <glib.h>
#include <gst/gst.h>

typedef enum {
    PIPELINE_STOPPED = 0,
    PIPELINE_RUNNING = 1,
    PIPELINE_STOPPING = 2
} PipelineStateEnum;

typedef struct {
    PipelineStateEnum state;
    GstElement *pipeline;
    GstElement *source;
    GstElement *tee;
    GstElement *video_sink;
    GstPad *video_pad;
    GstPad *audio_pad;
    GThread *bus_thread;
    GMutex lock;
    GCond cond;
    gboolean initialized;
    gboolean bus_thread_running;
    gboolean stop_requested;
    gboolean encountered_error;
    int audio_disabled;
} PipelineState;

#include "config.h"

int pipeline_start(const AppCfg *cfg, int audio_disabled, PipelineState *ps);
void pipeline_stop(PipelineState *ps, int wait_ms_total);
void pipeline_poll_child(PipelineState *ps);

#endif // PIPELINE_H
