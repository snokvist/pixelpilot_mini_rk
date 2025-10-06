#ifndef VIDEO_RECORDER_H
#define VIDEO_RECORDER_H

#include <gst/gst.h>

#include "config.h"

typedef struct VideoRecorder VideoRecorder;

VideoRecorder *video_recorder_new(const RecordCfg *cfg);
void video_recorder_handle_sample(VideoRecorder *recorder, GstSample *sample, const guint8 *data, size_t size);
void video_recorder_flush(VideoRecorder *recorder);
void video_recorder_free(VideoRecorder *recorder);

#endif // VIDEO_RECORDER_H
