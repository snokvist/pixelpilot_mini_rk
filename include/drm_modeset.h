#ifndef DRM_MODESET_H
#define DRM_MODESET_H

#include <stdint.h>

#if defined(__has_include)
#if __has_include(<libdrm/xf86drmMode.h>)
#include <libdrm/xf86drmMode.h>
#else
#include <xf86drmMode.h>
#endif
#else
#include <libdrm/xf86drmMode.h>
#endif

#include "config.h"

typedef struct {
    uint32_t connector_id;
    uint32_t crtc_id;
    int mode_w;
    int mode_h;
    int mode_hz;
    uint32_t prev_fb_id;
    int prev_x;
    int prev_y;
    int prev_mode_valid;
    drmModeModeInfo prev_mode;
    uint32_t *prev_connectors;
    int prev_connector_count;
} ModesetResult;

int atomic_modeset_maxhz(int fd, const AppCfg *cfg, int osd_enabled, ModesetResult *out);
int atomic_modeset_restore(int fd, const ModesetResult *ms);
void modeset_result_cleanup(ModesetResult *ms);
int is_any_connected(int fd, const AppCfg *cfg);

#endif // DRM_MODESET_H
