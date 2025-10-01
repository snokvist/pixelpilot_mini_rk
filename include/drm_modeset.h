#ifndef DRM_MODESET_H
#define DRM_MODESET_H

#include <stdint.h>

#include "config.h"

typedef struct {
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t video_plane_id;
    int mode_w;
    int mode_h;
    int mode_hz;
} ModesetResult;

int atomic_modeset_maxhz(int fd, const AppCfg *cfg, int osd_enabled, ModesetResult *out);
int is_any_connected(int fd, const AppCfg *cfg);

#endif // DRM_MODESET_H
