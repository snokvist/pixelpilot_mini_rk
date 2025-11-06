#ifndef VIDEO_GAMMA_H
#define VIDEO_GAMMA_H

#include <glib.h>
#include <stdint.h>

#include "config.h"

typedef struct {
    gboolean enabled;
    double lift;
    double gamma_pow;
    double gain;
    double channel_mul[3];

    int drm_fd;
    uint32_t crtc_id;
    uint32_t prop_id;
    uint32_t lut_size;
    uint32_t blob_id;
    gboolean applied;
} VideoGamma;

void video_gamma_init(VideoGamma *gamma, const AppCfg *cfg);
void video_gamma_reset(VideoGamma *gamma);
void video_gamma_configure_crtc(VideoGamma *gamma, int drm_fd, uint32_t crtc_id);

#endif // VIDEO_GAMMA_H
