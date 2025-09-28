#include "osd.h"
#include "drm_fb.h"
#include "drm_props.h"
#include "logging.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <libdrm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifndef DRM_PLANE_TYPE_OVERLAY
#define DRM_PLANE_TYPE_OVERLAY 0
#endif
#ifndef DRM_PLANE_TYPE_PRIMARY
#define DRM_PLANE_TYPE_PRIMARY 1
#endif
#ifndef DRM_PLANE_TYPE_CURSOR
#define DRM_PLANE_TYPE_CURSOR 2
#endif

void osd_init(OSD *o) {
    memset(o, 0, sizeof(*o));
}

static void osd_clear(OSD *o, uint32_t argb) {
    if (!o->fb.map) {
        return;
    }
    uint32_t *px = (uint32_t *)o->fb.map;
    size_t count = o->fb.size / 4;
    for (size_t i = 0; i < count; ++i) {
        px[i] = argb;
    }
}

// Font data derived from the public domain VGA 8x8 font by Marcel Sondaar,
// packaged by Daniel Hepper (font8x8). The table below is copied from the
// CC0/public-domain distribution to provide crisp, evenly spaced glyphs for
// the on-screen display.
static const uint8_t font8x8_basic[128][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00}, {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00}, {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00}, {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06}, {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00}, {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00}, {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00}, {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00}, {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00}, {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00}, {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00}, {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00}, {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00}, {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00}, {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00}, {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00}, {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00}, {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00}, {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00}, {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00}, {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00}, {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00}, {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00}, {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00}, {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00}, {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00}, {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},
    {0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},
    {0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00}, {0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},
    {0x38, 0x30, 0x30, 0x3E, 0x33, 0x33, 0x6E, 0x00}, {0x00, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00},
    {0x1C, 0x36, 0x06, 0x0F, 0x06, 0x06, 0x0F, 0x00}, {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    {0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00}, {0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E}, {0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},
    {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, {0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},
    {0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00}, {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},
    {0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F}, {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},
    {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00}, {0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},
    {0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00}, {0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},
    {0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00}, {0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    {0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00}, {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00}, {0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},
    {0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};

static void osd_draw_char(OSD *o, int x, int y, char c, uint32_t argb, int scale) {
    if ((unsigned char)c >= 128) {
        return;
    }
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    const uint8_t *glyph = font8x8_basic[(unsigned char)c];
    uint32_t *fb = (uint32_t *)o->fb.map;
    int pitch = o->fb.pitch / 4;
    for (int row = 0; row < 8; ++row) {
        uint8_t bits = glyph[row];
        if (!bits) {
            continue;
        }
        for (int col = 0; col < 8; ++col) {
            if (!(bits & (1u << col))) {
                continue;
            }
            for (int sy = 0; sy < scale; ++sy) {
                int py = y + row * scale + sy;
                if (py < 0 || py >= o->h) {
                    continue;
                }
                uint32_t *row_px = fb + py * pitch;
                for (int sx = 0; sx < scale; ++sx) {
                    int px = x + col * scale + sx;
                    if (px >= 0 && px < o->w) {
                        row_px[px] = argb;
                    }
                }
            }
        }
    }
}

static void osd_draw_text(OSD *o, int x, int y, const char *s, uint32_t argb, int scale) {
    const int advance = (8 + 1) * scale;
    const int line_advance = (8 + 1) * scale;
    int pen_x = x;
    int pen_y = y;
    for (const char *p = s; *p; ++p) {
        if (*p == '\n') {
            pen_y += line_advance;
            pen_x = x;
            continue;
        }
        osd_draw_char(o, pen_x, pen_y, *p, argb, scale);
        pen_x += advance;
    }
}

typedef struct {
    uint32_t p_fb_id, p_crtc_id, p_crtc_x, p_crtc_y, p_crtc_w, p_crtc_h;
    uint32_t p_src_x, p_src_y, p_src_w, p_src_h;
    uint32_t p_zpos;
    int have_zpos;
    uint64_t zmin, zmax;
    uint32_t p_alpha;
    int have_alpha;
    uint64_t amin, amax;
    uint32_t p_blend;
    int have_blend;
} PlaneProps;

static int plane_get_basic_props(int fd, uint32_t plane_id, PlaneProps *pp) {
    memset(pp, 0, sizeof(*pp));
    if (drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &pp->p_fb_id) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &pp->p_crtc_id) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", &pp->p_crtc_x) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &pp->p_crtc_y) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", &pp->p_crtc_w) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", &pp->p_crtc_h) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", &pp->p_src_x) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", &pp->p_src_y) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", &pp->p_src_w) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", &pp->p_src_h)) {
        return -1;
    }

    pp->have_zpos =
        (drm_get_prop_id_and_range_ci(fd, plane_id, DRM_MODE_OBJECT_PLANE, "ZPOS", &pp->p_zpos, &pp->zmin, &pp->zmax,
                                      "zpos") == 0);
    if (drm_get_prop_id_and_range_ci(fd, plane_id, DRM_MODE_OBJECT_PLANE, "alpha", &pp->p_alpha, &pp->amin, &pp->amax,
                                     "alpha") == 0) {
        pp->have_alpha = 1;
    }
    if (drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "pixel blend mode", &pp->p_blend) == 0) {
        pp->have_blend = 1;
    }
    return 0;
}

static int plane_accepts_linear_argb(int fd, uint32_t plane_id, uint32_t crtc_id) {
    PlaneProps pp;
    if (plane_get_basic_props(fd, plane_id, &pp) != 0) {
        return 0;
    }

    struct DumbFB fb = {0};
    if (create_argb_fb(fd, 64, 32, 0x80FFFFFFu, &fb) != 0) {
        return 0;
    }

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        destroy_dumb_fb(fd, &fb);
        return 0;
    }

    drmModeAtomicAddProperty(req, plane_id, pp.p_fb_id, fb.fb_id);
    drmModeAtomicAddProperty(req, plane_id, pp.p_crtc_id, crtc_id);
    drmModeAtomicAddProperty(req, plane_id, pp.p_crtc_x, 0);
    drmModeAtomicAddProperty(req, plane_id, pp.p_crtc_y, 0);
    drmModeAtomicAddProperty(req, plane_id, pp.p_crtc_w, fb.w);
    drmModeAtomicAddProperty(req, plane_id, pp.p_crtc_h, fb.h);
    drmModeAtomicAddProperty(req, plane_id, pp.p_src_x, 0);
    drmModeAtomicAddProperty(req, plane_id, pp.p_src_y, 0);
    drmModeAtomicAddProperty(req, plane_id, pp.p_src_w, (uint64_t)fb.w << 16);
    drmModeAtomicAddProperty(req, plane_id, pp.p_src_h, (uint64_t)fb.h << 16);

    int ok = (drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_TEST_ONLY, NULL) == 0);

    drmModeAtomicFree(req);
    destroy_dumb_fb(fd, &fb);
    return ok;
}

static int get_plane_type(int fd, uint32_t plane_id, int *out_type) {
    uint32_t type_prop = 0;
    if (drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "type", &type_prop) != 0) {
        return -1;
    }
    drmModeObjectProperties *pr = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!pr) {
        return -1;
    }
    int found = -1;
    for (uint32_t k = 0; k < pr->count_props; ++k) {
        if (pr->props[k] == type_prop) {
            *out_type = (int)pr->prop_values[k];
            found = 0;
            break;
        }
    }
    drmModeFreeObjectProperties(pr);
    return found;
}

static int osd_validate_requested_plane(int fd, uint32_t crtc_id, uint32_t plane_id) {
    if (!plane_accepts_linear_argb(fd, plane_id, crtc_id)) {
        return -1;
    }
    int type = 0;
    if (get_plane_type(fd, plane_id, &type) == 0) {
        if (type == DRM_PLANE_TYPE_CURSOR) {
            return -1;
        }
    }
    return 0;
}

static int osd_pick_plane(int fd, uint32_t crtc_id, int avoid_plane_id, uint32_t requested,
                          uint32_t *out_plane, uint64_t *out_zmax) {
    if (requested) {
        if (osd_validate_requested_plane(fd, crtc_id, requested) == 0) {
            uint32_t pz = 0;
            uint64_t zmin = 0, zmax = 0;
            int have = (drm_get_prop_id_and_range_ci(fd, requested, DRM_MODE_OBJECT_PLANE, "ZPOS", &pz, &zmin, &zmax,
                                                     "zpos") == 0);
            *out_plane = requested;
            *out_zmax = have ? zmax : 0;
            return 0;
        }
        LOGW("OSD: requested plane %u is not LINEAR ARGB-capable; falling back to auto-pick.", requested);
    }

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        return -1;
    }

    int crtc_index = -1;
    for (int i = 0; i < res->count_crtcs; ++i) {
        if ((uint32_t)res->crtcs[i] == crtc_id) {
            crtc_index = i;
            break;
        }
    }
    if (crtc_index < 0) {
        drmModeFreeResources(res);
        return -1;
    }

    drmModePlaneRes *prs = drmModeGetPlaneResources(fd);
    if (!prs) {
        drmModeFreeResources(res);
        return -1;
    }

    uint32_t best_plane = 0;
    int best_score = -1000000;
    uint64_t best_zmax = 0;

    for (uint32_t i = 0; i < prs->count_planes; ++i) {
        drmModePlane *p = drmModeGetPlane(fd, prs->planes[i]);
        if (!p) {
            continue;
        }
        if ((int)p->plane_id == avoid_plane_id) {
            drmModeFreePlane(p);
            continue;
        }
        if ((p->possible_crtcs & (1U << crtc_index)) == 0) {
            drmModeFreePlane(p);
            continue;
        }

        int type = 0;
        if (get_plane_type(fd, p->plane_id, &type) != 0) {
            drmModeFreePlane(p);
            continue;
        }
        if (type == DRM_PLANE_TYPE_CURSOR) {
            drmModeFreePlane(p);
            continue;
        }

        if (!plane_accepts_linear_argb(fd, p->plane_id, crtc_id)) {
            drmModeFreePlane(p);
            continue;
        }

        uint32_t pz = 0;
        uint64_t zmin = 0, zmax = 0;
        int have_z = (drm_get_prop_id_and_range_ci(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "ZPOS", &pz, &zmin, &zmax,
                                                   "zpos") == 0);

        int score = 0;
        if (have_z) {
            score += 100 + (int)zmax;
        }
        if (type == DRM_PLANE_TYPE_OVERLAY) {
            score += 1;
        }

        if (score > best_score) {
            best_score = score;
            best_plane = p->plane_id;
            best_zmax = have_z ? zmax : 0;
        }

        drmModeFreePlane(p);
    }

    drmModeFreePlaneResources(prs);
    drmModeFreeResources(res);

    if (!best_plane) {
        return -1;
    }
    *out_plane = best_plane;
    *out_zmax = best_zmax;
    return 0;
}

static int osd_query_plane_props(int fd, uint32_t plane_id, OSD *o) {
    if (drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &o->p_fb_id) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &o->p_crtc_id) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", &o->p_crtc_x) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &o->p_crtc_y) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", &o->p_crtc_w) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", &o->p_crtc_h) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", &o->p_src_x) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", &o->p_src_y) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", &o->p_src_w) ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", &o->p_src_h)) {
        LOGE("OSD plane props missing (id=%u)", plane_id);
        drm_debug_list_props(fd, plane_id, DRM_MODE_OBJECT_PLANE, "OSD_PLANE");
        return -1;
    }
    o->have_zpos =
        (drm_get_prop_id_and_range_ci(fd, plane_id, DRM_MODE_OBJECT_PLANE, "ZPOS", &o->p_zpos, &o->zmin, &o->zmax,
                                      "zpos") == 0);

    uint32_t p_alpha = 0, p_blend = 0;
    uint64_t amin = 0, amax = 0;
    if (drm_get_prop_id_and_range_ci(fd, plane_id, DRM_MODE_OBJECT_PLANE, "alpha", &p_alpha, &amin, &amax,
                                     "alpha") == 0) {
        o->p_alpha = p_alpha;
        o->alpha_min = amin;
        o->alpha_max = amax;
        o->have_alpha = 1;
    } else {
        o->have_alpha = 0;
    }
    if (drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "pixel blend mode", &p_blend) == 0) {
        o->p_blend = p_blend;
        o->have_blend = 1;
    } else {
        o->have_blend = 0;
    }
    return 0;
}

static int osd_commit_enable(int fd, uint32_t crtc_id, OSD *o) {
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        return -1;
    }
    drmModeAtomicAddProperty(req, o->plane_id, o->p_fb_id, o->fb.fb_id);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_crtc_id, crtc_id);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_crtc_x, 0);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_crtc_y, 0);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_crtc_w, o->w);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_crtc_h, o->h);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_src_x, 0);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_src_y, 0);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_src_w, (uint64_t)o->w << 16);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_src_h, (uint64_t)o->h << 16);
    if (o->have_zpos) {
        drmModeAtomicAddProperty(req, o->plane_id, o->p_zpos, o->zmax);
    }
    if (o->have_alpha) {
        uint64_t aval = o->alpha_max ? o->alpha_max : 65535;
        drmModeAtomicAddProperty(req, o->plane_id, o->p_alpha, aval);
    }
    if (o->have_blend) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, o->p_blend);
        if (prop) {
            uint64_t premul_val = 0;
            int found = 0;
            for (int i = 0; i < prop->count_enums; ++i) {
                if (strcmp(prop->enums[i].name, "Pre-multiplied") == 0) {
                    premul_val = prop->enums[i].value;
                    found = 1;
                    break;
                }
            }
            drmModeFreeProperty(prop);
            if (found) {
                drmModeAtomicAddProperty(req, o->plane_id, o->p_blend, premul_val);
            }
        }
    }
    int ret = drmModeAtomicCommit(fd, req, 0, NULL);
    drmModeAtomicFree(req);
    return ret;
}

static int osd_commit_disable(int fd, OSD *o) {
    if (!o->active) {
        return 0;
    }
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        return -1;
    }
    drmModeAtomicAddProperty(req, o->plane_id, o->p_fb_id, 0);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_crtc_id, 0);
    int ret = drmModeAtomicCommit(fd, req, 0, NULL);
    drmModeAtomicFree(req);
    return ret;
}

static void osd_commit_touch(int fd, uint32_t crtc_id, OSD *o) {
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        return;
    }
    drmModeAtomicAddProperty(req, o->plane_id, o->p_fb_id, o->fb.fb_id);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_crtc_id, crtc_id);
    drmModeAtomicCommit(fd, req, 0, NULL);
    drmModeAtomicFree(req);
}

static void osd_destroy_fb(int fd, OSD *o) {
    destroy_dumb_fb(fd, &o->fb);
    o->fb.map = NULL;
}

int osd_setup(int fd, const AppCfg *cfg, const ModesetResult *ms, int video_plane_id, OSD *o) {
    o->enabled = cfg->osd_enable;
    o->requested_plane_id = (uint32_t)cfg->osd_plane_id;
    o->refresh_ms = cfg->osd_refresh_ms;
    o->crtc_id = ms->crtc_id;

    if (!o->enabled) {
        return 0;
    }

    uint32_t chosen = 0;
    uint64_t zmax = 0;
    if (osd_pick_plane(fd, ms->crtc_id, video_plane_id, o->requested_plane_id, &chosen, &zmax) != 0) {
        LOGW("OSD: failed to find suitable plane. Disabling OSD.");
        o->enabled = 0;
        return -1;
    }
    o->plane_id = chosen;
    LOGI("OSD: using overlay plane id=%u", o->plane_id);
    if (osd_query_plane_props(fd, o->plane_id, o) != 0) {
        LOGW("OSD: plane props missing. Disabling OSD.");
        o->enabled = 0;
        return -1;
    }
    if (o->have_zpos && zmax > 0) {
        o->zmax = zmax;
    }

    o->scale = (ms->mode_w >= 1280) ? 2 : 1;
    o->w = 480 * o->scale;
    o->h = 120 * o->scale;

    if (create_argb_fb(fd, o->w, o->h, 0x80000000u, &o->fb) != 0) {
        LOGW("OSD: create fb failed. Disabling OSD.");
        o->enabled = 0;
        return -1;
    }

    osd_clear(o, 0x80000000u);
    if (osd_commit_enable(fd, ms->crtc_id, o) != 0) {
        LOGW("OSD: atomic enable failed. Disabling OSD.");
        osd_destroy_fb(fd, o);
        o->enabled = 0;
        return -1;
    }

    o->active = 1;
    return 0;
}

void osd_update_stats(int fd, const AppCfg *cfg, const ModesetResult *ms, const PipelineState *ps,
                      int audio_disabled, int restart_count, OSD *o) {
    if (!o->enabled || !o->active) {
        return;
    }

    osd_clear(o, 0x20000000u);

    char line1[128];
    snprintf(line1, sizeof(line1), "HDMI %dx%d@%d plane=%d", ms->mode_w, ms->mode_h, ms->mode_hz, cfg->plane_id);
    osd_draw_text(o, 10 * o->scale, 10 * o->scale, line1, 0xB0FFFFFFu, o->scale);

    char line2[128];
    snprintf(line2, sizeof(line2), "UDP:%d PTv=%d PTa=%d lat=%dms", cfg->udp_port, cfg->vid_pt, cfg->aud_pt, cfg->latency_ms);
    osd_draw_text(o, 10 * o->scale, 30 * o->scale, line2, 0xB0FFFFFFu, o->scale);

    char line3[128];
    snprintf(line3, sizeof(line3), "Pipeline: %s restarts=%d%s", ps->state == PIPELINE_RUNNING ? "RUN" : "STOP",
             restart_count, audio_disabled ? " audio=fakesink" : "");
    osd_draw_text(o, 10 * o->scale, 50 * o->scale, line3, 0xB0FFFFFFu, o->scale);

    UdpReceiverStats stats;
    if (pipeline_get_receiver_stats(ps, &stats) == 0) {
        double jitter_ms = stats.jitter / 90.0;
        double jitter_avg_ms = stats.jitter_avg / 90.0;
        char line4[160];
        snprintf(line4, sizeof(line4),
                 "RTP vpkts=%llu loss=%llu reo=%llu dup=%llu jitter=%.2f/%.2fms br=%.2f/%.2fMbps",
                 (unsigned long long)stats.video_packets, (unsigned long long)stats.lost_packets,
                 (unsigned long long)stats.reordered_packets, (unsigned long long)stats.duplicate_packets, jitter_ms,
                 jitter_avg_ms, stats.bitrate_mbps, stats.bitrate_avg_mbps);
        osd_draw_text(o, 10 * o->scale, 70 * o->scale, line4, 0xB0FFFFFFu, o->scale);

        char line5[160];
        snprintf(line5, sizeof(line5), "Frames=%llu incomplete=%llu last=%lluB avg=%.0fB seq=%u",
                 (unsigned long long)stats.frame_count, (unsigned long long)stats.incomplete_frames,
                 (unsigned long long)stats.last_frame_bytes, stats.frame_size_avg, stats.expected_sequence);
        osd_draw_text(o, 10 * o->scale, 90 * o->scale, line5, 0xB0FFFFFFu, o->scale);
    }

    osd_commit_touch(fd, o->crtc_id, o);
}

int osd_is_enabled(const OSD *o) {
    return o->enabled;
}

int osd_is_active(const OSD *o) {
    return o->active;
}

void osd_disable(int fd, OSD *o) {
    if (!o->active) {
        return;
    }
    if (osd_commit_disable(fd, o) == 0) {
        o->active = 0;
    }
}

void osd_teardown(int fd, OSD *o) {
    if (o->active) {
        osd_commit_disable(fd, o);
        o->active = 0;
    }
    osd_destroy_fb(fd, o);
    memset(o, 0, sizeof(*o));
}
