#ifndef RECORDER_H
#define RECORDER_H

#include <gst/gst.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Recorder Recorder;

Recorder *recorder_open(const RecordCfg *cfg,
                        int video_width,
                        int video_height,
                        gboolean audio_enabled,
                        guint audio_rate,
                        guint audio_channels,
                        guint audio_bytes_per_sample);
void recorder_close(Recorder *rec);
void recorder_free(Recorder *rec);
void recorder_push_video(Recorder *rec, const guint8 *data, gsize size, GstClockTime duration_ns);
void recorder_push_audio(Recorder *rec, const guint8 *data, gsize size, guint samples);
const char *recorder_output_path(const Recorder *rec);

#ifdef __cplusplus
}
#endif

#endif // RECORDER_H
