#include "osd.h"
#include "drm_fb.h"
#include "drm_props.h"
#include "logging.h"

#include <inttypes.h>
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

typedef struct {
    const AppCfg *cfg;
    const ModesetResult *ms;
    const PipelineState *ps;
    int audio_disabled;
    int restart_count;
    int have_stats;
    UdpReceiverStats stats;
} OsdRenderContext;

static const char *osd_pipeline_state_name(const PipelineState *ps) {
    if (!ps) {
        return "UNKNOWN";
    }
    switch (ps->state) {
    case PIPELINE_RUNNING:
        return "RUN";
    case PIPELINE_STOPPING:
        return "STOPPING";
    case PIPELINE_STOPPED:
    default:
        return "STOP";
    }
}

static int osd_token_format(const OsdRenderContext *ctx, const char *token, char *buf, size_t buf_sz) {
    if (!token || !buf || buf_sz == 0) {
        return -1;
    }

    const AppCfg *cfg = ctx->cfg;
    if (strcmp(token, "display.mode") == 0) {
        if (ctx->ms) {
            snprintf(buf, buf_sz, "%dx%d@%d", ctx->ms->mode_w, ctx->ms->mode_h, ctx->ms->mode_hz);
        } else {
            snprintf(buf, buf_sz, "n/a");
        }
        return 0;
    }
    if (strcmp(token, "display.width") == 0) {
        if (ctx->ms) {
            snprintf(buf, buf_sz, "%d", ctx->ms->mode_w);
        } else {
            snprintf(buf, buf_sz, "0");
        }
        return 0;
    }
    if (strcmp(token, "display.height") == 0) {
        if (ctx->ms) {
            snprintf(buf, buf_sz, "%d", ctx->ms->mode_h);
        } else {
            snprintf(buf, buf_sz, "0");
        }
        return 0;
    }
    if (strcmp(token, "display.refresh_hz") == 0) {
        if (ctx->ms) {
            snprintf(buf, buf_sz, "%d", ctx->ms->mode_hz);
        } else {
            snprintf(buf, buf_sz, "0");
        }
        return 0;
    }
    if (strcmp(token, "drm.video_plane_id") == 0) {
        snprintf(buf, buf_sz, "%d", cfg ? cfg->plane_id : 0);
        return 0;
    }
    if (strcmp(token, "drm.osd_plane_id") == 0) {
        snprintf(buf, buf_sz, "%d", cfg ? cfg->osd_plane_id : 0);
        return 0;
    }
    if (strcmp(token, "osd.refresh_ms") == 0) {
        snprintf(buf, buf_sz, "%d", cfg ? cfg->osd_refresh_ms : 0);
        return 0;
    }
    if (strcmp(token, "udp.port") == 0) {
        snprintf(buf, buf_sz, "%d", cfg ? cfg->udp_port : 0);
        return 0;
    }
    if (strcmp(token, "udp.vid_pt") == 0) {
        snprintf(buf, buf_sz, "%d", cfg ? cfg->vid_pt : 0);
        return 0;
    }
    if (strcmp(token, "udp.aud_pt") == 0) {
        snprintf(buf, buf_sz, "%d", cfg ? cfg->aud_pt : 0);
        return 0;
    }
    if (strcmp(token, "pipeline.latency_ms") == 0) {
        snprintf(buf, buf_sz, "%d", cfg ? cfg->latency_ms : 0);
        return 0;
    }
    if (strcmp(token, "pipeline.state") == 0) {
        snprintf(buf, buf_sz, "%s", osd_pipeline_state_name(ctx->ps));
        return 0;
    }
    if (strcmp(token, "pipeline.restart_count") == 0) {
        snprintf(buf, buf_sz, "%d", ctx->restart_count);
        return 0;
    }
    if (strcmp(token, "pipeline.audio_suffix") == 0) {
        if (ctx->audio_disabled) {
            snprintf(buf, buf_sz, " audio=fakesink");
        } else {
            buf[0] = '\0';
        }
        return 0;
    }
    if (strcmp(token, "pipeline.audio_status") == 0) {
        snprintf(buf, buf_sz, "%s", ctx->audio_disabled ? "fakesink" : "normal");
        return 0;
    }

    if (strcmp(token, "udp.stats.available") == 0) {
        snprintf(buf, buf_sz, "%s", ctx->have_stats ? "yes" : "no");
        return 0;
    }

    if (!ctx->have_stats) {
        if (strncmp(token, "udp.", 4) == 0) {
            snprintf(buf, buf_sz, "n/a");
            return 0;
        }
    }

    if (strcmp(token, "udp.video_packets") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.video_packets);
        return 0;
    }
    if (strcmp(token, "udp.audio_packets") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.audio_packets);
        return 0;
    }
    if (strcmp(token, "udp.total_packets") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.total_packets);
        return 0;
    }
    if (strcmp(token, "udp.ignored_packets") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.ignored_packets);
        return 0;
    }
    if (strcmp(token, "udp.duplicate_packets") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.duplicate_packets);
        return 0;
    }
    if (strcmp(token, "udp.lost_packets") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.lost_packets);
        return 0;
    }
    if (strcmp(token, "udp.reordered_packets") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.reordered_packets);
        return 0;
    }
    if (strcmp(token, "udp.total_bytes") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.total_bytes);
        return 0;
    }
    if (strcmp(token, "udp.video_bytes") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.video_bytes);
        return 0;
    }
    if (strcmp(token, "udp.audio_bytes") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.audio_bytes);
        return 0;
    }
    if (strcmp(token, "udp.bitrate.latest_mbps") == 0) {
        snprintf(buf, buf_sz, "%.2f", ctx->stats.bitrate_mbps);
        return 0;
    }
    if (strcmp(token, "udp.bitrate.avg_mbps") == 0) {
        snprintf(buf, buf_sz, "%.2f", ctx->stats.bitrate_avg_mbps);
        return 0;
    }
    if (strcmp(token, "udp.jitter.latest_ms") == 0) {
        snprintf(buf, buf_sz, "%.2f", ctx->stats.jitter / 90.0);
        return 0;
    }
    if (strcmp(token, "udp.jitter.avg_ms") == 0) {
        snprintf(buf, buf_sz, "%.2f", ctx->stats.jitter_avg / 90.0);
        return 0;
    }
    if (strcmp(token, "udp.pipeline.drop_total") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.pipeline_dropped_total);
        return 0;
    }
    if (strcmp(token, "udp.pipeline.drop_too_late") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.pipeline_dropped_too_late);
        return 0;
    }
    if (strcmp(token, "udp.pipeline.drop_on_latency") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.pipeline_dropped_on_latency);
        return 0;
    }
    if (strcmp(token, "udp.pipeline.last_drop_reason") == 0) {
        if (ctx->stats.pipeline_last_drop_reason[0]) {
            snprintf(buf, buf_sz, "%s", ctx->stats.pipeline_last_drop_reason);
        } else {
            snprintf(buf, buf_sz, "n/a");
        }
        return 0;
    }
    if (strcmp(token, "udp.pipeline.last_drop_seqnum") == 0) {
        snprintf(buf, buf_sz, "%u", ctx->stats.pipeline_last_drop_seqnum);
        return 0;
    }
    if (strcmp(token, "udp.pipeline.last_drop_timestamp_ns") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.pipeline_last_drop_timestamp);
        return 0;
    }
    if (strcmp(token, "udp.frames.count") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.frame_count);
        return 0;
    }
    if (strcmp(token, "udp.frames.incomplete") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.incomplete_frames);
        return 0;
    }
    if (strcmp(token, "udp.frames.last_bytes") == 0) {
        snprintf(buf, buf_sz, "%llu", (unsigned long long)ctx->stats.last_frame_bytes);
        return 0;
    }
    if (strcmp(token, "udp.frames.avg_bytes") == 0) {
        snprintf(buf, buf_sz, "%.0f", ctx->stats.frame_size_avg);
        return 0;
    }
    if (strcmp(token, "udp.expected_sequence") == 0) {
        snprintf(buf, buf_sz, "%u", ctx->stats.expected_sequence);
        return 0;
    }
    if (strcmp(token, "udp.last_video_timestamp") == 0) {
        snprintf(buf, buf_sz, "%u", ctx->stats.last_video_timestamp);
        return 0;
    }

    snprintf(buf, buf_sz, "{%s}", token);
    return -1;
}

static int osd_metric_sample(const OsdRenderContext *ctx, const char *key, double *out_value) {
    if (!key || !out_value) {
        return 0;
    }
    if (!ctx->have_stats && strncmp(key, "udp.", 4) == 0) {
        return 0;
    }

    if (strcmp(key, "udp.bitrate.latest_mbps") == 0) {
        *out_value = ctx->stats.bitrate_mbps;
        return 1;
    }
    if (strcmp(key, "udp.bitrate.avg_mbps") == 0) {
        *out_value = ctx->stats.bitrate_avg_mbps;
        return 1;
    }
    if (strcmp(key, "udp.jitter.latest_ms") == 0) {
        *out_value = ctx->stats.jitter / 90.0;
        return 1;
    }
    if (strcmp(key, "udp.jitter.avg_ms") == 0) {
        *out_value = ctx->stats.jitter_avg / 90.0;
        return 1;
    }
    if (strcmp(key, "udp.pipeline.drop_total") == 0) {
        *out_value = (double)ctx->stats.pipeline_dropped_total;
        return 1;
    }
    if (strcmp(key, "udp.pipeline.drop_on_latency") == 0) {
        *out_value = (double)ctx->stats.pipeline_dropped_on_latency;
        return 1;
    }
    if (strcmp(key, "udp.pipeline.drop_too_late") == 0) {
        *out_value = (double)ctx->stats.pipeline_dropped_too_late;
        return 1;
    }
    if (strcmp(key, "udp.frames.avg_bytes") == 0) {
        *out_value = ctx->stats.frame_size_avg;
        return 1;
    }
    if (strcmp(key, "udp.frames.count") == 0) {
        *out_value = (double)ctx->stats.frame_count;
        return 1;
    }
    if (strcmp(key, "udp.video_packets") == 0) {
        *out_value = (double)ctx->stats.video_packets;
        return 1;
    }
    if (strcmp(key, "udp.duplicate_packets") == 0) {
        *out_value = (double)ctx->stats.duplicate_packets;
        return 1;
    }
    if (strcmp(key, "udp.lost_packets") == 0) {
        *out_value = (double)ctx->stats.lost_packets;
        return 1;
    }
    if (strcmp(key, "udp.reordered_packets") == 0) {
        *out_value = (double)ctx->stats.reordered_packets;
        return 1;
    }

    if (strcmp(key, "pipeline.restart_count") == 0) {
        *out_value = (double)ctx->restart_count;
        return 1;
    }
    if (strcmp(key, "pipeline.latency_ms") == 0) {
        *out_value = (double)(ctx->cfg ? ctx->cfg->latency_ms : 0);
        return 1;
    }

    return 0;
}

static void osd_expand_template(const OsdRenderContext *ctx, const char *tmpl, char *out, size_t out_sz) {
    if (!tmpl || !out || out_sz == 0) {
        return;
    }
    size_t pos = 0;
    const char *p = tmpl;
    while (*p && pos + 1 < out_sz) {
        if (*p == '{') {
            const char *end = strchr(p, '}');
            if (!end) {
                out[pos++] = *p++;
                continue;
            }
            size_t key_len = (size_t)(end - (p + 1));
            char key[128];
            if (key_len >= sizeof(key)) {
                key_len = sizeof(key) - 1;
            }
            memcpy(key, p + 1, key_len);
            key[key_len] = '\0';
            char value[256];
            if (osd_token_format(ctx, key, value, sizeof(value)) != 0) {
                snprintf(value, sizeof(value), "{%s}", key);
            }
            size_t value_len = strnlen(value, sizeof(value));
            if (value_len > out_sz - pos - 1) {
                value_len = out_sz - pos - 1;
            }
            memcpy(out + pos, value, value_len);
            pos += value_len;
            p = end + 1;
        } else {
            out[pos++] = *p++;
        }
    }
    out[pos] = '\0';
}

static void osd_format_metric_value(const char *metric_key, double value, char *buf, size_t buf_sz) {
    if (!metric_key || !buf || buf_sz == 0) {
        return;
    }
    if (strstr(metric_key, "mbps") || strstr(metric_key, "ms")) {
        snprintf(buf, buf_sz, "%.2f", value);
    } else if (strstr(metric_key, "avg") || strstr(metric_key, "ratio")) {
        snprintf(buf, buf_sz, "%.2f", value);
    } else {
        snprintf(buf, buf_sz, "%.0f", value);
    }
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

static void osd_compute_placement(const OSD *o, int rect_w, int rect_h, const OsdPlacement *placement,
                                  int *out_x, int *out_y) {
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

    OSDWidgetPosition pos = placement ? placement->anchor : OSD_POS_TOP_LEFT;
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
    case OSD_POS_MID:
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

    if (placement) {
        x += placement->offset_x;
        y += placement->offset_y;
    }

    *out_x = x;
    *out_y = y;
}

static void osd_line_reset(OSD *o, const AppCfg *cfg, int idx) {
    if (idx < 0 || idx >= o->layout.element_count) {
        return;
    }
    OsdElementConfig *elem_cfg = &o->layout.elements[idx];
    OsdElementType type = elem_cfg->type;
    o->elements[idx].type = type;
    if (type != OSD_WIDGET_LINE) {
        return;
    }

    int scale = o->scale > 0 ? o->scale : 1;
    OsdLineState *state = &o->elements[idx].data.line;
    memset(state, 0, sizeof(*state));

    int window_seconds = elem_cfg->data.line.window_seconds > 0 ? elem_cfg->data.line.window_seconds : 60;
    int refresh_ms = cfg->osd_refresh_ms > 0 ? cfg->osd_refresh_ms : 500;
    int desired_columns = (window_seconds * 1000 + refresh_ms - 1) / refresh_ms;
    if (desired_columns < 2) {
        desired_columns = 2;
    }
    state->capacity = desired_columns;
    if (state->capacity > OSD_PLOT_MAX_SAMPLES) {
        state->capacity = OSD_PLOT_MAX_SAMPLES;
    }
    if (state->capacity < 2) {
        state->capacity = 2;
    }

    state->size = 0;
    state->cursor = 0;
    state->sum = 0.0;
    state->latest = 0.0;
    state->min_v = DBL_MAX;
    state->max_v = 0.0;
    state->avg = 0.0;
    state->scale_min = 0.0;
    state->scale_max = 1.0;
    state->step_px = 0.0;
    state->clear_on_next_draw = 0;
    state->background_ready = 0;
    state->prev_valid = 0;
    state->rescale_countdown = 0;

    int width = elem_cfg->data.line.width;
    int height = elem_cfg->data.line.height;
    if (width <= 0) {
        width = 360;
    }
    if (height <= 0) {
        height = 80;
    }
    width *= scale;
    height *= scale;

    int margin = o->margin_px;
    if (width > o->w - 2 * margin) {
        width = o->w - 2 * margin;
    }
    if (width < scale * 80) {
        width = scale * 80;
    }
    if (width <= 0) {
        width = scale * 80;
    }

    if (height > o->h - 2 * margin) {
        height = o->h - 2 * margin;
    }
    if (height < scale * 40) {
        height = scale * 40;
    }
    if (height <= 0) {
        height = scale * 40;
    }

    state->width = width;
    state->height = height;
    osd_compute_placement(o, width, height, &elem_cfg->placement, &state->x, &state->y);
    osd_store_rect(&state->plot_rect, 0, 0, 0, 0);
    osd_store_rect(&state->label_rect, 0, 0, 0, 0);
    osd_store_rect(&state->footer_rect, 0, 0, 0, 0);

    if (state->capacity > 1 && state->width > 1) {
        state->step_px = (double)(state->width - 1) / (double)(state->capacity - 1);
    } else {
        state->step_px = 0.0;
    }
}

static void osd_line_push(OsdLineState *state, double value) {
    if (!state || state->capacity <= 0) {
        return;
    }
    if (state->cursor >= state->capacity) {
        state->cursor = 0;
    }
    if (state->cursor == 0 && state->size >= state->capacity) {
        state->clear_on_next_draw = 1;
        state->background_ready = 0;
        state->prev_valid = 0;
        state->size = 0;
        state->sum = 0.0;
        state->min_v = DBL_MAX;
        state->max_v = 0.0;
        state->avg = 0.0;
        state->latest = 0.0;
    }

    state->samples[state->cursor] = value;
    state->cursor++;
    if (state->size < state->cursor) {
        state->size = state->cursor;
    }
    state->sum += value;
    if (state->size == 1 || value < state->min_v) {
        state->min_v = value;
    }
    if (value > state->max_v) {
        state->max_v = value;
    }
    state->avg = state->size > 0 ? (state->sum / (double)state->size) : 0.0;
    state->latest = value;
}

#define OSD_PLOT_RESCALE_DELAY 12

static void osd_line_compute_scale(const OsdLineState *state, double *out_min, double *out_max) {
    double min_v = 0.0;
    double max_v = 1.0;
    if (state->size > 0) {
        if (state->min_v != DBL_MAX) {
            min_v = state->min_v;
        }
        max_v = state->max_v;
        if (max_v < 0.1) {
            max_v = 0.1;
        }
        if (min_v > max_v) {
            min_v = max_v * 0.5;
        }
    }

    double span = max_v - min_v;
    if (span <= 0.0) {
        span = (max_v > 0.0) ? (max_v * 0.5) : 0.5;
    }
    double pad = span * 0.1;
    if (pad < 0.05) {
        pad = 0.05;
    }
    min_v -= pad;
    max_v += pad;
    if (min_v < 0.0) {
        min_v = 0.0;
    }
    if (max_v <= min_v) {
        max_v = min_v + 0.1;
    }
    *out_min = min_v;
    *out_max = max_v;
}

static int osd_line_value_to_y(const OsdLineState *state, double value) {
    double min_v = state->scale_min;
    double max_v = state->scale_max;
    if (max_v <= min_v) {
        max_v = min_v + 0.1;
    }
    double norm = (value - min_v) / (max_v - min_v);
    if (norm < 0.0) {
        norm = 0.0;
    }
    if (norm > 1.0) {
        norm = 1.0;
    }
    int plot_h = state->height;
    int base_y = state->y;
    return base_y + plot_h - 1 - (int)(norm * (plot_h - 1) + 0.5);
}

static void osd_line_draw_background(OSD *o, int idx) {
    OsdLineState *state = &o->elements[idx].data.line;
    const OsdLineConfig *cfg = &o->layout.elements[idx].data.line;
    uint32_t bg = cfg->bg ? cfg->bg : 0x40202020u;
    uint32_t border = 0x60FFFFFFu;
    uint32_t axis = cfg->fg ? cfg->fg : 0x60FFFFFFu;
    uint32_t grid = cfg->grid ? cfg->grid : 0x30909090u;

    osd_clear_rect(o, &state->plot_rect);

    int base_x = state->x;
    int base_y = state->y;
    int plot_w = state->width;
    int plot_h = state->height;

    osd_fill_rect(o, base_x, base_y, plot_w, plot_h, bg);
    osd_draw_rect(o, base_x, base_y, plot_w, plot_h, border);
    osd_store_rect(&state->plot_rect, base_x, base_y, plot_w, plot_h);

    int grid_lines = 4;
    for (int i = 1; i < grid_lines; ++i) {
        int gy = base_y + (plot_h * i) / grid_lines;
        osd_draw_hline(o, base_x, gy, plot_w, grid);
    }

    int desired_secs = 10;
    int window_seconds = cfg->window_seconds > 0 ? cfg->window_seconds : 60;
    double px_per_sec = (window_seconds > 0 && plot_w > 1) ? (double)(plot_w - 1) / (double)window_seconds : 0.0;
    if (px_per_sec > 0.0) {
        int step_px = (int)(px_per_sec * desired_secs + 0.5);
        int scale = o->scale > 0 ? o->scale : 1;
        if (step_px < scale) {
            step_px = scale;
        }
        for (int gx = step_px; gx < plot_w; gx += step_px) {
            osd_draw_vline(o, base_x + gx, base_y, plot_h, grid);
        }
    }

    int axis_thickness = o->scale > 0 ? o->scale : 1;
    osd_draw_hline(o, base_x, base_y + plot_h - axis_thickness, plot_w, axis);
    osd_draw_vline(o, base_x, base_y, plot_h, axis);
}

static void osd_line_draw_all(OSD *o, int idx, uint32_t color) {
    OsdLineState *state = &o->elements[idx].data.line;
    int limit = state->size;
    if (limit <= 0) {
        state->prev_valid = 0;
        return;
    }
    int base_x = state->x;
    double step = state->step_px;
    if (step <= 0.0) {
        step = 0.0;
    }
    int prev_x = -1;
    int prev_y = -1;
    int scale = o->scale > 0 ? o->scale : 1;

    for (int i = 0; i < limit; ++i) {
        double value = state->samples[i];
        int x = base_x + (int)(i * step + 0.5);
        if (x >= base_x + state->width) {
            x = base_x + state->width - 1;
        }
        int y = osd_line_value_to_y(state, value);
        if (prev_x >= 0 && x >= prev_x) {
            osd_draw_line(o, prev_x, prev_y, x, y, color);
        }
        osd_fill_rect(o, x, y, scale, scale, color);
        prev_x = x;
        prev_y = y;
    }

    if (prev_x >= 0 && prev_y >= 0) {
        state->prev_valid = 1;
        state->prev_x = prev_x;
        state->prev_y = prev_y;
    } else {
        state->prev_valid = 0;
    }
}

static void osd_line_draw_latest(OSD *o, int idx, uint32_t color) {
    OsdLineState *state = &o->elements[idx].data.line;
    int limit = state->size;
    if (limit <= 0) {
        state->prev_valid = 0;
        return;
    }
    int index = limit - 1;
    double value = state->samples[index];
    int base_x = state->x;
    double step = state->step_px;
    int x = base_x + (int)(index * step + 0.5);
    if (x >= base_x + state->width) {
        x = base_x + state->width - 1;
    }
    int y = osd_line_value_to_y(state, value);
    int scale = o->scale > 0 ? o->scale : 1;

    if (state->prev_valid) {
        osd_draw_line(o, state->prev_x, state->prev_y, x, y, color);
    }
    osd_fill_rect(o, x, y, scale, scale, color);
    state->prev_valid = 1;
    state->prev_x = x;
    state->prev_y = y;
}

static void osd_line_draw(OSD *o, int idx) {
    OsdLineState *state = &o->elements[idx].data.line;
    const OsdLineConfig *cfg = &o->layout.elements[idx].data.line;
    if (state->capacity <= 0) {
        return;
    }

    double scale_min = state->scale_min;
    double scale_max = state->scale_max;
    double new_min = scale_min;
    double new_max = scale_max;
    osd_line_compute_scale(state, &new_min, &new_max);

    int need_background = (!state->background_ready || state->clear_on_next_draw);
    if (state->size > 0) {
        double actual_min = (state->min_v != DBL_MAX) ? state->min_v : new_min;
        double actual_max = state->max_v;
        if (!need_background) {
            if (actual_min < scale_min || actual_max > scale_max) {
                need_background = 1;
            } else {
                double span = scale_max - scale_min;
                double used = actual_max - actual_min;
                if (span > 0.0 && used >= 0.0) {
                    double utilization = (span > 0.0) ? (used / span) : 1.0;
                    if (utilization < 0.35) {
                        if (state->rescale_countdown > 0) {
                            state->rescale_countdown--;
                        } else {
                            need_background = 1;
                        }
                    } else {
                        state->rescale_countdown = OSD_PLOT_RESCALE_DELAY;
                    }
                }
            }
        }
    }

    uint32_t fg = cfg->fg ? cfg->fg : 0xB0FF4040u;

    if (need_background) {
        osd_line_draw_background(o, idx);
        state->scale_min = new_min;
        state->scale_max = new_max;
        state->background_ready = 1;
        state->clear_on_next_draw = 0;
        state->prev_valid = 0;
        state->rescale_countdown = OSD_PLOT_RESCALE_DELAY;
        osd_line_draw_all(o, idx, fg);
        return;
    }

    if (state->size <= 0) {
        state->prev_valid = 0;
        return;
    }

    osd_line_draw_latest(o, idx, fg);
}

static void osd_line_draw_label(OSD *o, int idx, const OsdLineConfig *cfg) {
    OsdLineState *state = &o->elements[idx].data.line;
    osd_clear_rect(o, &state->label_rect);
    if (!cfg || !cfg->show_info_box) {
        osd_store_rect(&state->label_rect, 0, 0, 0, 0);
        return;
    }
    const char *text = cfg->label;
    if (text == NULL || text[0] == '\0') {
        osd_store_rect(&state->label_rect, 0, 0, 0, 0);
        return;
    }
    int scale = o->scale > 0 ? o->scale : 1;
    int pad = 4 * scale;
    int gap = 4 * scale;
    int line_height = 8 * scale;
    int text_w = (int)strlen(text) * (8 + 1) * scale;
    if (text_w < 0) {
        text_w = 0;
    }
    int box_w = text_w + 2 * pad;
    int box_h = line_height + 2 * pad;

    int x = state->x;
    if (x + box_w > state->x + state->width) {
        x = state->x + state->width - box_w;
    }
    if (x < o->margin_px) {
        x = o->margin_px;
    }
    if (x + box_w > o->w - o->margin_px) {
        x = o->w - o->margin_px - box_w;
        if (x < o->margin_px) {
            x = o->margin_px;
        }
    }

    int y = state->y - box_h - gap;
    if (y < o->margin_px) {
        y = state->y + state->height + gap;
        if (y + box_h > o->h - o->margin_px) {
            y = state->y + state->height - box_h - gap;
            if (y < o->margin_px) {
                y = o->margin_px;
            }
        }
    }

    uint32_t bg = 0x50202020u;
    uint32_t border = 0x60FFFFFFu;
    uint32_t text_color = 0xB0FFFFFFu;
    osd_fill_rect(o, x, y, box_w, box_h, bg);
    osd_draw_rect(o, x, y, box_w, box_h, border);
    osd_draw_text(o, x + pad, y + pad, text, text_color, o->scale);
    osd_store_rect(&state->label_rect, x, y, box_w, box_h);
}

static void osd_line_draw_footer(OSD *o, int idx, const char **lines, int line_count) {
    OsdLineState *state = &o->elements[idx].data.line;
    osd_clear_rect(o, &state->footer_rect);
    if (lines == NULL || line_count <= 0) {
        osd_store_rect(&state->footer_rect, 0, 0, 0, 0);
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
        osd_store_rect(&state->footer_rect, 0, 0, 0, 0);
        return;
    }
    int box_w = max_line_w + 2 * pad;
    int box_h = line_count * line_advance + 2 * pad;
    int x = state->x + state->width - box_w;
    if (x < o->margin_px) {
        x = o->margin_px;
    }
    if (x + box_w > o->w - o->margin_px) {
        x = o->w - o->margin_px - box_w;
        if (x < o->margin_px) {
            x = o->margin_px;
        }
    }
    int y = state->y + state->height + scale * 4;
    if (y + box_h > o->h - o->margin_px) {
        y = state->y + state->height - box_h - scale * 4;
        if (y < o->margin_px) {
            y = o->margin_px;
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
    osd_store_rect(&state->footer_rect, x, y, box_w, box_h);
}

static void osd_render_text_element(OSD *o, int idx, const OsdRenderContext *ctx) {
    OsdElementConfig *cfg = &o->layout.elements[idx];
    OsdTextConfig *text_cfg = &cfg->data.text;
    osd_clear_rect(o, &o->elements[idx].rect);

    if (text_cfg->line_count <= 0) {
        osd_store_rect(&o->elements[idx].rect, 0, 0, 0, 0);
        return;
    }

    char lines[OSD_MAX_TEXT_LINES][OSD_TEXT_MAX_LINE_CHARS];
    const char *line_ptrs[OSD_MAX_TEXT_LINES];
    int actual_lines = 0;
    for (int i = 0; i < text_cfg->line_count && i < OSD_MAX_TEXT_LINES; ++i) {
        osd_expand_template(ctx, text_cfg->lines[i].raw, lines[actual_lines], sizeof(lines[actual_lines]));
        line_ptrs[actual_lines] = lines[actual_lines];
        actual_lines++;
    }

    if (actual_lines <= 0) {
        osd_store_rect(&o->elements[idx].rect, 0, 0, 0, 0);
        return;
    }

    int scale = o->scale > 0 ? o->scale : 1;
    int padding = text_cfg->padding > 0 ? text_cfg->padding : 6;
    int pad_px = padding * scale;
    if (pad_px < 4) {
        pad_px = 4;
    }
    int line_advance = (8 + 1) * scale;
    int max_line_width = 0;
    for (int i = 0; i < actual_lines; ++i) {
        int len = (int)strlen(line_ptrs[i]);
        int width = len * (8 + 1) * scale;
        if (width > max_line_width) {
            max_line_width = width;
        }
    }
    int box_w = max_line_width + 2 * pad_px;
    int box_h = actual_lines * line_advance + 2 * pad_px;

    int draw_x = 0;
    int draw_y = 0;
    osd_compute_placement(o, box_w, box_h, &cfg->placement, &draw_x, &draw_y);

    uint32_t fg = text_cfg->fg ? text_cfg->fg : 0xB0FFFFFFu;
    uint32_t bg = text_cfg->bg ? text_cfg->bg : 0x40202020u;
    uint32_t border = text_cfg->border ? text_cfg->border : 0x60FFFFFFu;

    osd_fill_rect(o, draw_x, draw_y, box_w, box_h, bg);
    osd_draw_rect(o, draw_x, draw_y, box_w, box_h, border);

    int text_x = draw_x + pad_px;
    int text_y = draw_y + pad_px;
    for (int i = 0; i < actual_lines; ++i) {
        osd_draw_text(o, text_x, text_y, line_ptrs[i], fg, o->scale);
        text_y += line_advance;
    }

    osd_store_rect(&o->elements[idx].rect, draw_x, draw_y, box_w, box_h);
    o->elements[idx].data.text.last_line_count = actual_lines;
}

static void osd_render_line_element(OSD *o, int idx, const OsdRenderContext *ctx) {
    OsdElementConfig *elem_cfg = &o->layout.elements[idx];
    OsdLineState *state = &o->elements[idx].data.line;

    OSDRect prev_rect = o->elements[idx].rect;
    int repositioned = (prev_rect.x != state->x) || (prev_rect.y != state->y) || (prev_rect.w != state->width) ||
                       (prev_rect.h != state->height);
    if (repositioned) {
        if (prev_rect.w > 0 && prev_rect.h > 0) {
            osd_clear_rect(o, &prev_rect);
        }
        state->background_ready = 0;
        state->clear_on_next_draw = 1;
        state->prev_valid = 0;
    }

    double value = 0.0;
    int have_value = osd_metric_sample(ctx, elem_cfg->data.line.metric, &value);
    if (have_value) {
        osd_line_push(state, value);
    }

    osd_line_draw(o, idx);
    osd_line_draw_label(o, idx, &elem_cfg->data.line);

    char footer_lines[3][128];
    const char *footer_ptrs[3];
    int footer_count = 0;

    if (state->size > 0) {
        char latest_buf[32];
        char avg_buf[32];
        double min_v = (state->min_v == DBL_MAX) ? 0.0 : state->min_v;
        char min_buf[32];
        char max_buf[32];
        osd_format_metric_value(elem_cfg->data.line.metric, state->latest, latest_buf, sizeof(latest_buf));
        osd_format_metric_value(elem_cfg->data.line.metric, state->avg, avg_buf, sizeof(avg_buf));
        osd_format_metric_value(elem_cfg->data.line.metric, min_v, min_buf, sizeof(min_buf));
        osd_format_metric_value(elem_cfg->data.line.metric, state->max_v, max_buf, sizeof(max_buf));

        snprintf(footer_lines[footer_count], sizeof(footer_lines[footer_count]), "Latest %s  Avg %s", latest_buf, avg_buf);
        footer_ptrs[footer_count] = footer_lines[footer_count];
        footer_count++;
        if (footer_count < 3) {
            snprintf(footer_lines[footer_count], sizeof(footer_lines[footer_count]), "Min %s  Max %s", min_buf, max_buf);
            footer_ptrs[footer_count] = footer_lines[footer_count];
            footer_count++;
        }
        if (footer_count < 3) {
            int window_seconds = elem_cfg->data.line.window_seconds > 0 ? elem_cfg->data.line.window_seconds : 60;
            snprintf(footer_lines[footer_count], sizeof(footer_lines[footer_count]), "Window %ds", window_seconds);
            footer_ptrs[footer_count] = footer_lines[footer_count];
            footer_count++;
        }
    } else {
        const char *msg = ctx->have_stats && have_value ? "Collecting samples..." : "Metric unavailable";
        snprintf(footer_lines[0], sizeof(footer_lines[0]), "%s", msg);
        footer_ptrs[footer_count] = footer_lines[0];
        footer_count++;
    }

    osd_line_draw_footer(o, idx, footer_ptrs, footer_count);
    osd_store_rect(&o->elements[idx].rect, state->x, state->y, state->width, state->height);
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
    o->w = (ms->mode_w > 0) ? ms->mode_w : (960 * o->scale);
    o->h = (ms->mode_h > 0) ? ms->mode_h : (360 * o->scale);
    if (o->w <= 0) {
        o->w = 960 * o->scale;
    }
    if (o->h <= 0) {
        o->h = 360 * o->scale;
    }
    o->margin_px = clampi(12 * o->scale, 8, o->h / 4);

    if (create_argb_fb(fd, o->w, o->h, 0x80000000u, &o->fb) != 0) {
        LOGW("OSD: create fb failed. Disabling OSD.");
        o->enabled = 0;
        return -1;
    }

    o->layout = cfg->osd_layout;
    if (o->layout.element_count > OSD_MAX_ELEMENTS) {
        o->layout.element_count = OSD_MAX_ELEMENTS;
    }
    o->element_count = o->layout.element_count;
    for (int i = 0; i < o->element_count; ++i) {
        o->elements[i].type = o->layout.elements[i].type;
        osd_store_rect(&o->elements[i].rect, 0, 0, 0, 0);
        if (o->elements[i].type == OSD_WIDGET_LINE) {
            osd_line_reset(o, cfg, i);
        } else if (o->elements[i].type == OSD_WIDGET_TEXT) {
            o->elements[i].data.text.last_line_count = 0;
        }
    }

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

    OsdRenderContext ctx = {
        .cfg = cfg,
        .ms = ms,
        .ps = ps,
        .audio_disabled = audio_disabled,
        .restart_count = restart_count,
        .have_stats = 0,
    };
    if (ps && pipeline_get_receiver_stats(ps, &ctx.stats) == 0) {
        ctx.have_stats = 1;
    }

    for (int i = 0; i < o->element_count; ++i) {
        OsdElementType type = o->layout.elements[i].type;
        o->elements[i].type = type;
        switch (type) {
        case OSD_WIDGET_TEXT:
            osd_render_text_element(o, i, &ctx);
            break;
        case OSD_WIDGET_LINE:
            osd_render_line_element(o, i, &ctx);
            break;
        case OSD_WIDGET_BAR:
        default:
            osd_clear_rect(o, &o->elements[i].rect);
            osd_store_rect(&o->elements[i].rect, 0, 0, 0, 0);
            break;
        }
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
