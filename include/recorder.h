#ifndef RECORDER_H
#define RECORDER_H

#include <glib.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Recorder Recorder;

Recorder *recorder_new(void);
void recorder_free(Recorder *rec);

gboolean recorder_start(Recorder *rec, const AppCfg *cfg, gboolean enable_audio);
void recorder_stop(Recorder *rec);

gboolean recorder_is_running(Recorder *rec);
GstAppSrc *recorder_get_audio_appsrc(Recorder *rec);
void recorder_push_video_buffer(Recorder *rec, GstBuffer *buffer);

#ifdef __cplusplus
}
#endif

#endif // RECORDER_H
