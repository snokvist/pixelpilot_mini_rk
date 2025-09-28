#ifndef OSD_H
#define OSD_H

#include <stdint.h>

#include "config.h"
#include "drm_fb.h"
#include "drm_modeset.h"
#include "pipeline.h"

typedef struct OSD {
    int enabled;
    int active;
    uint32_t requested_plane_id;
    uint32_t plane_id;
    struct DumbFB fb;
    int w;
    int h;
    int scale;
    int refresh_ms;
    uint32_t crtc_id;

    uint32_t p_fb_id, p_crtc_id, p_crtc_x, p_crtc_y, p_crtc_w, p_crtc_h;
    uint32_t p_src_x, p_src_y, p_src_w, p_src_h;
    uint32_t p_zpos;
    int have_zpos;
    uint64_t zmin, zmax;
    uint32_t p_alpha;
    int have_alpha;
    uint64_t alpha_min, alpha_max;
    uint32_t p_blend;
    int have_blend;
} OSD;

void osd_init(OSD *osd);
int osd_setup(int fd, const AppCfg *cfg, const ModesetResult *ms, int video_plane_id, OSD *osd);
void osd_update_stats(int fd, const AppCfg *cfg, const ModesetResult *ms, const PipelineState *ps,
                      int audio_disabled, int restart_count, OSD *osd);
int osd_is_enabled(const OSD *osd);
int osd_is_active(const OSD *osd);
void osd_disable(int fd, OSD *osd);
void osd_teardown(int fd, OSD *osd);

#endif // OSD_H
