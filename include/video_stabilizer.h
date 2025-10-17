#ifndef VIDEO_STABILIZER_H
#define VIDEO_STABILIZER_H

#include <glib.h>
#include <stdint.h>

#ifndef PIXELPILOT_HAVE_RGA
#define PIXELPILOT_HAVE_RGA 0
#endif

typedef struct {
    int enable;
    float strength;
    float max_translation_px;
    float max_rotation_deg;
    int diagnostics;
    int demo_enable;
    float demo_amplitude_px;
    float demo_frequency_hz;
} StabilizerConfig;

typedef struct {
    gboolean enable;
    int acquire_fence_fd;
    gboolean has_transform;
    float transform[9];
    float translate_x;
    float translate_y;
    float rotate_deg;
} StabilizerParams;

typedef struct VideoStabilizer VideoStabilizer;

VideoStabilizer *video_stabilizer_new(void);
void video_stabilizer_free(VideoStabilizer *stabilizer);

int video_stabilizer_init(VideoStabilizer *stabilizer, const StabilizerConfig *config);
void video_stabilizer_shutdown(VideoStabilizer *stabilizer);
int video_stabilizer_configure(VideoStabilizer *stabilizer, uint32_t width, uint32_t height,
                               uint32_t hor_stride, uint32_t ver_stride);
int video_stabilizer_update(VideoStabilizer *stabilizer, const StabilizerConfig *config);
int video_stabilizer_process(VideoStabilizer *stabilizer, int in_fd, int out_fd,
                             const StabilizerParams *params, int *release_fence_fd);
gboolean video_stabilizer_is_available(const VideoStabilizer *stabilizer);

#endif // VIDEO_STABILIZER_H
