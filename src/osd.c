#include "osd.h"
#include "drm_fb.h"
#include "drm_props.h"
#include "logging.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <float.h>

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

static int clampi(int v, int min_v, int max_v) {
    if (v < min_v) {
        return min_v;
    }
    if (v > max_v) {
        return max_v;
    }
    return v;
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

static void osd_fill_rect(OSD *o, int x, int y, int w, int h, uint32_t argb) {
    if (!o->fb.map || w <= 0 || h <= 0) {
        return;
    }
    int x0 = clampi(x, 0, o->w);
    int y0 = clampi(y, 0, o->h);
    int x1 = clampi(x + w, 0, o->w);
    int y1 = clampi(y + h, 0, o->h);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    uint32_t *fb = (uint32_t *)o->fb.map;
    int pitch = o->fb.pitch / 4;
    for (int py = y0; py < y1; ++py) {
        uint32_t *row = fb + py * pitch;
        for (int px = x0; px < x1; ++px) {
            row[px] = argb;
        }
    }
}

static void osd_store_rect(OSDRect *r, int x, int y, int w, int h) {
    if (!r) {
        return;
    }
    if (w < 0) {
        w = 0;
    }
    if (h < 0) {
        h = 0;
    }
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
}

static void osd_clear_rect(OSD *o, const OSDRect *r) {
    if (!r || r->w <= 0 || r->h <= 0) {
        return;
    }
    osd_fill_rect(o, r->x, r->y, r->w, r->h, 0x00000000u);
}

static void osd_draw_hline(OSD *o, int x, int y, int w, uint32_t argb) {
    osd_fill_rect(o, x, y, w, o->scale > 0 ? o->scale : 1, argb);
}

static void osd_draw_vline(OSD *o, int x, int y, int h, uint32_t argb) {
    osd_fill_rect(o, x, y, o->scale > 0 ? o->scale : 1, h, argb);
}

static void osd_draw_line(OSD *o, int x0, int y0, int x1, int y1, uint32_t argb) {
    if (!o->fb.map) {
        return;
    }
    int dx = x1 > x0 ? (x1 - x0) : (x0 - x1);
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? (y0 - y1) : (y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    int cx = x0;
    int cy = y0;
    while (1) {
        osd_fill_rect(o, cx, cy, o->scale > 0 ? o->scale : 1, o->scale > 0 ? o->scale : 1, argb);
        if (cx == x1 && cy == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            cx += sx;
        }
        if (e2 <= dx) {
            err += dx;
            cy += sy;
        }
        if (cx < 0 || cy < 0 || cx >= o->w || cy >= o->h) {
            if (cx < 0 && sx < 0) {
                cx = 0;
            }
            if (cy < 0 && sy < 0) {
                cy = 0;
            }
            if (cx >= o->w && sx > 0) {
                cx = o->w - 1;
            }
            if (cy >= o->h && sy > 0) {
                cy = o->h - 1;
            }
        }
    }
}

static void osd_draw_rect(OSD *o, int x, int y, int w, int h, uint32_t argb) {
    if (w <= 0 || h <= 0) {
        return;
    }
    osd_draw_hline(o, x, y, w, argb);
    osd_draw_hline(o, x, y + h - (o->scale > 0 ? o->scale : 1), w, argb);
    osd_draw_vline(o, x, y, h, argb);
    osd_draw_vline(o, x + w - (o->scale > 0 ? o->scale : 1), y, h, argb);
}

static void osd_compute_anchor(const OSD *o, int rect_w, int rect_h, OSDWidgetPosition pos, int *out_x, int *out_y) {
    int margin = o->margin_px;
    int inner_w = o->w - 2 * margin;
    int inner_h = o->h - 2 * margin;
    if (inner_w < 0) {
        inner_w = 0;
    }
    if (inner_h < 0) {
        inner_h = 0;
    }

    int x = margin;
    int y = margin;

    switch (pos) {
    case OSD_POS_TOP_LEFT:
        x = margin;
        y = margin;
        break;
    case OSD_POS_TOP_MID:
        x = margin + (inner_w - rect_w) / 2;
        y = margin;
        break;
    case OSD_POS_TOP_RIGHT:
        x = o->w - margin - rect_w;
        y = margin;
        break;
    case OSD_POS_MID_LEFT:
        x = margin;
        y = margin + (inner_h - rect_h) / 2;
        break;
    case OSD_POS_MID_MID:
        x = margin + (inner_w - rect_w) / 2;
        y = margin + (inner_h - rect_h) / 2;
        break;
    case OSD_POS_MID_RIGHT:
        x = o->w - margin - rect_w;
        y = margin + (inner_h - rect_h) / 2;
        break;
    case OSD_POS_BOTTOM_LEFT:
        x = margin;
        y = o->h - margin - rect_h;
        break;
    case OSD_POS_BOTTOM_MID:
        x = margin + (inner_w - rect_w) / 2;
        y = o->h - margin - rect_h;
        break;
    case OSD_POS_BOTTOM_RIGHT:
    default:
        x = o->w - margin - rect_w;
        y = o->h - margin - rect_h;
        break;
    }

    if (x < margin) {
        x = margin;
    }
    if (y < margin) {
        y = margin;
    }

    if (x + rect_w > o->w - margin) {
        x = o->w - margin - rect_w;
    }
    if (y + rect_h > o->h - margin) {
        y = o->h - margin - rect_h;
    }

    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }

    *out_x = x;
    *out_y = y;
}

static void osd_plot_reset(OSD *o, const AppCfg *cfg) {
    o->plot_window_seconds = 60;
    o->plot_position = OSD_POS_BOTTOM_LEFT;
    o->plot_sum = 0.0;
    o->plot_size = 0;
    o->plot_cursor = 0;
    o->plot_latest = 0.0;
    o->plot_min = DBL_MAX;
    o->plot_max = 0.0;
    o->plot_avg = 0.0;
    memset(o->plot_samples, 0, sizeof(o->plot_samples));
    o->plot_clear_on_next_draw = 0;

    int margin = o->margin_px;
    int desired_columns = cfg->osd_refresh_ms > 0
                              ? (int)((o->plot_window_seconds * 1000 + cfg->osd_refresh_ms - 1) / cfg->osd_refresh_ms)
                              : o->plot_window_seconds;
    if (desired_columns < 2) {
        desired_columns = 2;
    }

    int capacity = desired_columns;
    if (capacity > OSD_PLOT_MAX_SAMPLES) {
        capacity = OSD_PLOT_MAX_SAMPLES;
    }
    if (capacity < 2) {
        capacity = 2;
    }

    o->plot_capacity = capacity;
    int scale = o->scale > 0 ? o->scale : 1;
    int target_plot_w = 360 * scale;
    int available_w = o->w - 2 * margin;
    int min_plot_w = 160 * scale;
    if (available_w <= 0) {
        target_plot_w = min_plot_w;
    } else {
        if (target_plot_w > available_w) {
            target_plot_w = available_w;
        }
        if (target_plot_w < min_plot_w) {
            target_plot_w = available_w < min_plot_w ? available_w : min_plot_w;
        }
        if (target_plot_w <= 0) {
            target_plot_w = available_w;
        }
    }
    if (target_plot_w <= 0) {
        target_plot_w = min_plot_w;
    }
    o->plot_w = target_plot_w;
    int plot_target_h = 80 * scale;
    if (plot_target_h > o->h - 2 * margin) {
        plot_target_h = o->h - 2 * margin;
    }
    if (plot_target_h < 48 * scale) {
        plot_target_h = 48 * scale;
    }
    o->plot_h = plot_target_h;
    osd_compute_anchor(o, o->plot_w, o->plot_h, o->plot_position, &o->plot_x, &o->plot_y);
    osd_store_rect(&o->plot_rect, 0, 0, 0, 0);
    osd_store_rect(&o->plot_label_rect, 0, 0, 0, 0);
    osd_store_rect(&o->plot_stats_rect, 0, 0, 0, 0);
}

static void osd_plot_push(OSD *o, double value) {
    if (o->plot_capacity <= 0) {
        return;
    }
    if (o->plot_cursor >= o->plot_capacity) {
        o->plot_cursor = 0;
    }
    if (o->plot_cursor == 0 && o->plot_size >= o->plot_capacity) {
        o->plot_clear_on_next_draw = 1;
        o->plot_size = 0;
        o->plot_sum = 0.0;
        o->plot_min = DBL_MAX;
        o->plot_max = 0.0;
        o->plot_avg = 0.0;
        o->plot_latest = 0.0;
    }

    o->plot_samples[o->plot_cursor] = value;
    o->plot_cursor++;
    if (o->plot_size < o->plot_cursor) {
        o->plot_size = o->plot_cursor;
    }
    o->plot_sum += value;
    if (o->plot_size == 1 || value < o->plot_min) {
        o->plot_min = value;
    }
    if (value > o->plot_max) {
        o->plot_max = value;
    }
    o->plot_avg = o->plot_size > 0 ? (o->plot_sum / (double)o->plot_size) : 0.0;
    o->plot_latest = value;
}

static void osd_plot_draw(OSD *o) {
    if (o->plot_capacity <= 0) {
        return;
    }
    uint32_t bg = 0x40202020u;
    uint32_t border = 0x60FFFFFFu;
    uint32_t axis = 0x60FFFFFFu;
    uint32_t grid = 0x30909090u;
    uint32_t plot_color = 0xB0FF4040u;
    uint32_t avg_color = 0x80FFD070u;

    osd_clear_rect(o, &o->plot_rect);

    int base_x = o->plot_x;
    int base_y = o->plot_y;
    int plot_w = o->plot_w;
    int plot_h = o->plot_h;

    osd_fill_rect(o, base_x, base_y, plot_w, plot_h, bg);
    osd_draw_rect(o, base_x, base_y, plot_w, plot_h, border);
    osd_store_rect(&o->plot_rect, base_x, base_y, plot_w, plot_h);

    int limit = o->plot_size;
    if (o->plot_clear_on_next_draw) {
        o->plot_clear_on_next_draw = 0;
        limit = 0;
    }

    double max_v = (limit > 0) ? o->plot_max : 0.0;
    double min_v = (limit > 0 && o->plot_min != DBL_MAX) ? o->plot_min : 0.0;
    if (max_v < 0.1) {
        max_v = 0.1;
    }
    if (max_v < min_v) {
        max_v = min_v;
    }
    double range = max_v - min_v;
    if (range < 0.1) {
        range = max_v * 0.5;
        if (range < 0.1) {
            range = 0.1;
        }
    }

    // Horizontal grid lines (quartiles)
    int grid_lines = 4;
    for (int i = 1; i < grid_lines; ++i) {
        int gy = base_y + (plot_h * i) / grid_lines;
        osd_draw_hline(o, base_x, gy, plot_w, grid);
    }

    // Vertical grid lines based on window seconds (every 10s if possible)
    int desired_secs = 10;
    double px_per_sec = (o->plot_window_seconds > 0 && plot_w > 1)
                            ? (double)(plot_w - 1) / (double)o->plot_window_seconds
                            : 0.0;
    if (px_per_sec > 0.0) {
        int step_px = (int)(px_per_sec * desired_secs + 0.5);
        if (step_px < (o->scale > 0 ? o->scale : 1)) {
            step_px = (o->scale > 0 ? o->scale : 1);
        }
        for (int gx = step_px; gx < plot_w; gx += step_px) {
            osd_draw_vline(o, base_x + gx, base_y, plot_h, grid);
        }
    }

    // Axis lines
    osd_draw_hline(o, base_x, base_y + plot_h - (o->scale > 0 ? o->scale : 1), plot_w, axis);
    osd_draw_vline(o, base_x, base_y, plot_h, axis);

    if (limit <= 0) {
        return;
    }

    // Average line
    if (o->plot_avg > 0.0) {
        double norm = (o->plot_avg - min_v) / range;
        if (norm < 0.0) {
            norm = 0.0;
        }
        if (norm > 1.0) {
            norm = 1.0;
        }
        int ay = base_y + plot_h - 1 - (int)(norm * (plot_h - 1));
        osd_draw_hline(o, base_x, ay, plot_w, avg_color);
    }

    int prev_x = -1;
    int prev_y = -1;
    int scale = o->scale > 0 ? o->scale : 1;
    double step = (o->plot_capacity > 1) ? (double)(plot_w - 1) / (double)(o->plot_capacity - 1) : 0.0;

    for (int i = 0; i < limit; ++i) {
        double value = o->plot_samples[i];
        double norm = (value - min_v) / range;
        if (norm < 0.0) {
            norm = 0.0;
        }
        if (norm > 1.0) {
            norm = 1.0;
        }
        int x = base_x + (int)(i * step + 0.5);
        if (x >= base_x + plot_w) {
            x = base_x + plot_w - 1;
        }
        int y = base_y + plot_h - 1 - (int)(norm * (plot_h - 1));
        if (prev_x >= 0 && x >= prev_x) {
            osd_draw_line(o, prev_x, prev_y, x, y, plot_color);
        }
        osd_fill_rect(o, x, y, scale, scale, plot_color);
        prev_x = x;
        prev_y = y;
    }
}

static void osd_plot_draw_label(OSD *o, const char *text) {
    osd_clear_rect(o, &o->plot_label_rect);
    if (text == NULL || text[0] == '\0') {
        osd_store_rect(&o->plot_label_rect, 0, 0, 0, 0);
        return;
    }
    int scale = o->scale > 0 ? o->scale : 1;
    int pad = 4 * scale;
    int line_height = 8 * scale;
    int text_w = (int)strlen(text) * (8 + 1) * scale;
    int box_w = text_w + 2 * pad;
    int box_h = line_height + 2 * pad;
    int x = o->plot_x + pad;
    int y = o->plot_y + pad;
    if (x + box_w > o->plot_x + o->plot_w - pad) {
        x = o->plot_x + o->plot_w - pad - box_w;
    }
    if (y + box_h > o->plot_y + o->plot_h - pad) {
        y = o->plot_y + o->plot_h - pad - box_h;
    }
    if (x < o->margin_px) {
        x = o->margin_px;
    }
    if (y < o->margin_px) {
        y = o->margin_px;
    }
    uint32_t bg = 0x50202020u;
    uint32_t border = 0x60FFFFFFu;
    uint32_t text_color = 0xB0FFFFFFu;
    osd_fill_rect(o, x, y, box_w, box_h, bg);
    osd_draw_rect(o, x, y, box_w, box_h, border);
    osd_draw_text(o, x + pad, y + pad, text, text_color, o->scale);
    osd_store_rect(&o->plot_label_rect, x, y, box_w, box_h);
}

static void osd_plot_draw_footer(OSD *o, const char **lines, int line_count) {
    osd_clear_rect(o, &o->plot_stats_rect);
    if (lines == NULL || line_count <= 0) {
        osd_store_rect(&o->plot_stats_rect, 0, 0, 0, 0);
        return;
    }
    int scale = o->scale > 0 ? o->scale : 1;
    int pad = 6 * scale;
    int line_advance = (8 + 1) * scale;
    int max_line_w = 0;
    for (int i = 0; i < line_count; ++i) {
        if (lines[i] == NULL) {
            continue;
        }
        int len = (int)strlen(lines[i]);
        int w = len * (8 + 1) * scale;
        if (w > max_line_w) {
            max_line_w = w;
        }
    }
    if (max_line_w <= 0) {
        osd_store_rect(&o->plot_stats_rect, 0, 0, 0, 0);
        return;
    }
    int box_w = max_line_w + 2 * pad;
    int box_h = line_count * line_advance + 2 * pad;
    int x = o->plot_x;
    int y = o->plot_y + o->plot_h + scale * 4;
    if (y + box_h > o->h - o->margin_px) {
        y = o->plot_y + o->plot_h - box_h - scale * 4;
        if (y < o->margin_px) {
            y = o->margin_px;
        }
    }
    if (x + box_w > o->w - o->margin_px) {
        x = o->w - o->margin_px - box_w;
        if (x < o->margin_px) {
            x = o->margin_px;
        }
    }
    uint32_t bg = 0x40202020u;
    uint32_t border = 0x60FFFFFFu;
    uint32_t text_color = 0xB0FFFFFFu;
    osd_fill_rect(o, x, y, box_w, box_h, bg);
    osd_draw_rect(o, x, y, box_w, box_h, border);
    int draw_y = y + pad;
    for (int i = 0; i < line_count; ++i) {
        if (lines[i] == NULL) {
            continue;
        }
        osd_draw_text(o, x + pad, draw_y, lines[i], text_color, o->scale);
        draw_y += line_advance;
    }
    osd_store_rect(&o->plot_stats_rect, x, y, box_w, box_h);
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
    o->w = 960 * o->scale;
    o->h = 360 * o->scale;
    o->margin_px = 12 * o->scale;

    if (o->w > ms->mode_w) {
        o->w = ms->mode_w / 2;
    }
    if (o->h > ms->mode_h) {
        o->h = ms->mode_h / 2;
    }

    o->margin_px = clampi(o->margin_px, 8, o->w / 4);

    if (create_argb_fb(fd, o->w, o->h, 0x80000000u, &o->fb) != 0) {
        LOGW("OSD: create fb failed. Disabling OSD.");
        o->enabled = 0;
        return -1;
    }

    osd_plot_reset(o, cfg);
    osd_store_rect(&o->text_rect, 0, 0, 0, 0);

    osd_clear(o, 0x00000000u);
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

    int margin = o->margin_px;
    int line_advance = (8 + 1) * o->scale;
    uint32_t text_color = 0xB0FFFFFFu;
    uint32_t text_bg = 0x40202020u;
    uint32_t text_border = 0x60FFFFFFu;
    int pad = 6 * o->scale;
    if (pad < 4) {
        pad = 4;
    }

    char text_lines[12][192];
    const char *line_ptrs[12];
    int line_count = 0;
    char plot_lines[3][128];
    const char *plot_line_ptrs[3];
    int plot_line_count = 0;
    int plot_line_cap = (int)(sizeof(plot_line_ptrs) / sizeof(plot_line_ptrs[0]));

    snprintf(text_lines[line_count], sizeof(text_lines[line_count]), "HDMI %dx%d@%d plane=%d", ms->mode_w,
             ms->mode_h, ms->mode_hz, cfg->plane_id);
    line_ptrs[line_count] = text_lines[line_count];
    line_count++;

    snprintf(text_lines[line_count], sizeof(text_lines[line_count]), "UDP:%d PTv=%d PTa=%d lat=%dms", cfg->udp_port,
             cfg->vid_pt, cfg->aud_pt, cfg->latency_ms);
    line_ptrs[line_count] = text_lines[line_count];
    line_count++;

    snprintf(text_lines[line_count], sizeof(text_lines[line_count]), "Pipeline: %s restarts=%d%s",
             ps->state == PIPELINE_RUNNING ? "RUN" : "STOP", restart_count, audio_disabled ? " audio=fakesink" : "");
    line_ptrs[line_count] = text_lines[line_count];
    line_count++;

    UdpReceiverStats stats;
    int have_stats = (pipeline_get_receiver_stats(ps, &stats) == 0);
    if (have_stats) {
        double jitter_ms = stats.jitter / 90.0;
        double jitter_avg_ms = stats.jitter_avg / 90.0;
        snprintf(text_lines[line_count], sizeof(text_lines[line_count]),
                 "RTP vpkts=%llu loss=%llu reo=%llu dup=%llu jitter=%.2f/%.2fms br=%.2f/%.2fMbps",
                 (unsigned long long)stats.video_packets, (unsigned long long)stats.lost_packets,
                 (unsigned long long)stats.reordered_packets, (unsigned long long)stats.duplicate_packets, jitter_ms,
                 jitter_avg_ms, stats.bitrate_mbps, stats.bitrate_avg_mbps);
        line_ptrs[line_count] = text_lines[line_count];
        line_count++;

        snprintf(text_lines[line_count], sizeof(text_lines[line_count]),
                 "Frames=%llu incomplete=%llu last=%lluB avg=%.0fB seq=%u",
                 (unsigned long long)stats.frame_count, (unsigned long long)stats.incomplete_frames,
                 (unsigned long long)stats.last_frame_bytes, stats.frame_size_avg, stats.expected_sequence);
        line_ptrs[line_count] = text_lines[line_count];
        line_count++;

        osd_plot_push(o, stats.bitrate_mbps);
    } else {
        snprintf(text_lines[line_count], sizeof(text_lines[line_count]), "UDP statistics unavailable");
        line_ptrs[line_count] = text_lines[line_count];
        line_count++;
    }

    if (o->plot_size > 0) {
        if (plot_line_count < plot_line_cap) {
            snprintf(plot_lines[plot_line_count], sizeof(plot_lines[plot_line_count]), "Latest %.2f Mbps  Avg %.2f",
                     o->plot_latest, o->plot_avg);
            plot_line_ptrs[plot_line_count] = plot_lines[plot_line_count];
            plot_line_count++;
        }
        if (plot_line_count < plot_line_cap) {
            double min_v = (o->plot_min == DBL_MAX) ? 0.0 : o->plot_min;
            snprintf(plot_lines[plot_line_count], sizeof(plot_lines[plot_line_count]), "Min %.2f  Max %.2f",
                     min_v, o->plot_max);
            plot_line_ptrs[plot_line_count] = plot_lines[plot_line_count];
            plot_line_count++;
        }
        if (plot_line_count < plot_line_cap) {
            snprintf(plot_lines[plot_line_count], sizeof(plot_lines[plot_line_count]), "Window %ds", o->plot_window_seconds);
            plot_line_ptrs[plot_line_count] = plot_lines[plot_line_count];
            plot_line_count++;
        }
    } else {
        if (plot_line_count < plot_line_cap) {
            const char *waiting = have_stats ? "Collecting bitrate samples..." : "Bitrate stats unavailable";
            snprintf(plot_lines[plot_line_count], sizeof(plot_lines[plot_line_count]), "%s", waiting);
            plot_line_ptrs[plot_line_count] = plot_lines[plot_line_count];
            plot_line_count++;
        }
    }

    osd_clear_rect(o, &o->text_rect);

    int text_box_x = margin;
    int text_box_y = margin;
    int max_line_width = 0;
    for (int i = 0; i < line_count; ++i) {
        int len = (int)strlen(line_ptrs[i]);
        int width = len * (8 + 1) * o->scale;
        if (width > max_line_width) {
            max_line_width = width;
        }
    }
    int text_box_w = max_line_width + 2 * pad;
    int text_box_h = line_count * line_advance + 2 * pad;
    if (text_box_w < 0) {
        text_box_w = 0;
    }
    if (text_box_h < 0) {
        text_box_h = 0;
    }
    if (text_box_x + text_box_w > o->w) {
        text_box_w = o->w - text_box_x;
        if (text_box_w < 0) {
            text_box_w = 0;
        }
    }
    if (text_box_y + text_box_h > o->h) {
        text_box_h = o->h - text_box_y;
        if (text_box_h < 0) {
            text_box_h = 0;
        }
    }

    if (text_box_w > 0 && text_box_h > 0) {
        osd_fill_rect(o, text_box_x, text_box_y, text_box_w, text_box_h, text_bg);
        osd_draw_rect(o, text_box_x, text_box_y, text_box_w, text_box_h, text_border);
        int draw_x = text_box_x + pad;
        int draw_y = text_box_y + pad;
        for (int i = 0; i < line_count; ++i) {
            osd_draw_text(o, draw_x, draw_y, line_ptrs[i], text_color, o->scale);
            draw_y += line_advance;
        }
    }
    osd_store_rect(&o->text_rect, text_box_x, text_box_y, text_box_w, text_box_h);

    osd_plot_draw(o);
    osd_plot_draw_label(o, "Mbit/s");
    osd_plot_draw_footer(o, plot_line_ptrs, plot_line_count);

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
