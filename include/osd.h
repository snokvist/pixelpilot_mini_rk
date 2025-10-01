#ifndef OSD_H
#define OSD_H

#include <stdint.h>

#include "config.h"
#include "drm_fb.h"
#include "drm_modeset.h"
#include "osd_layout.h"
#include "pipeline.h"

#define OSD_PLOT_MAX_SAMPLES 1024

typedef struct {
    int x;
    int y;
    int w;
    int h;
} OSDRect;

typedef struct {
    int last_line_count;
} OsdTextState;

typedef struct {
    double samples[OSD_PLOT_MAX_SAMPLES];
    int capacity;
    int size;
    int cursor;
    double sum;
    double latest;
    double min_v;
    double max_v;
    double avg;
    double scale_min;
    double scale_max;
    double step_px;
    int clear_on_next_draw;
    int background_ready;
    int prev_valid;
    int prev_x;
    int prev_y;
    int rescale_countdown;
    int width;
    int height;
    int x;
    int y;
    OSDRect plot_rect;
    OSDRect header_rect;
    OSDRect label_rect;
    OSDRect footer_rect;
} OsdLineState;

typedef struct {
    double samples[OSD_PLOT_MAX_SAMPLES];
    int capacity;
    int size;
    int cursor;
    double sum;
    double latest;
    double min_v;
    double max_v;
    double avg;
    double scale_min;
    double scale_max;
    double step_px;
    int clear_on_next_draw;
    int background_ready;
    int rescale_countdown;
    int width;
    int height;
    int bar_width;
    int x;
    int y;
    OSDRect plot_rect;
    OSDRect header_rect;
    OSDRect label_rect;
    OSDRect footer_rect;
} OsdBarState;

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

    /* Layout helpers */
    int margin_px;

    OsdLayout layout;
    int element_count;
    struct {
        OsdElementType type;
        OSDRect rect;
        union {
            OsdTextState text;
            OsdLineState line;
            OsdBarState bar;
        } data;
    } elements[OSD_MAX_ELEMENTS];
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
