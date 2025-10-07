#ifndef PIPELINE_H
#define PIPELINE_H

#include <glib.h>
#include <gst/gst.h>

#include "udp_receiver.h"
#include "video_decoder.h"
#include "idr_requester.h"

typedef enum {
    PIPELINE_STOPPED = 0,
    PIPELINE_RUNNING = 1,
    PIPELINE_STOPPING = 2
} PipelineStateEnum;

struct Splash;
struct VideoRecorder;

typedef struct {
    PipelineStateEnum state;
    GstElement *pipeline;
    GstElement *video_sink;
    GstElement *input_selector;
    UdpReceiver *udp_receiver;
    IdrRequester *idr_requester;
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
    struct Splash *splash;
    GThread *splash_loop_thread;
    gboolean splash_loop_running;
    GstPad *selector_udp_pad;
    GstPad *selector_splash_pad;
    gboolean splash_active;
    gboolean splash_available;
    guint splash_idle_timeout_ms;
    guint64 pipeline_start_ns;
    guint64 last_udp_activity_ns;
    struct VideoRecorder *recorder;
    GMutex recorder_lock;
    int wake_fd;
} PipelineState;

#include "config.h"

int pipeline_start(const AppCfg *cfg, const ModesetResult *ms, int drm_fd, int audio_disabled, PipelineState *ps);
void pipeline_stop(PipelineState *ps, int wait_ms_total);
void pipeline_poll_child(PipelineState *ps);
int pipeline_get_receiver_stats(const PipelineState *ps, UdpReceiverStats *stats);
void pipeline_set_receiver_stats_enabled(PipelineState *ps, gboolean enabled);
gboolean pipeline_is_recording(const PipelineState *ps);
int pipeline_enable_recording(PipelineState *ps, const RecordCfg *cfg);
void pipeline_disable_recording(PipelineState *ps);

#endif // PIPELINE_H
