#ifndef VIDEO_RECORDER_H
#define VIDEO_RECORDER_H

#include <glib.h>
#include <gst/gst.h>

#include "config.h"

typedef struct VideoRecorder VideoRecorder;

typedef struct {
    gboolean active;
    guint64 bytes_written;
    guint64 elapsed_ns;
    guint64 media_duration_ns;
    char output_path[PATH_MAX];
} VideoRecorderStats;

VideoRecorder *video_recorder_new(const RecordCfg *cfg);
void video_recorder_handle_sample(VideoRecorder *recorder, GstSample *sample, const guint8 *data, size_t size);
void video_recorder_flush(VideoRecorder *recorder);
void video_recorder_free(VideoRecorder *recorder);
void video_recorder_get_stats(const VideoRecorder *recorder, VideoRecorderStats *stats);

#endif // VIDEO_RECORDER_H
