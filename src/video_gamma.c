#include "video_gamma.h"

#include "drm_props.h"
#include "logging.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#if defined(__has_include)
#if __has_include(<drm/drm_color_mgmt.h>)
#include <drm/drm_color_mgmt.h>
#elif __has_include(<libdrm/drm_color_mgmt.h>)
#include <libdrm/drm_color_mgmt.h>
#endif
#if __has_include(<drm/drm_mode.h>)
#include <drm/drm_mode.h>
#elif __has_include(<libdrm/drm_mode.h>)
#include <libdrm/drm_mode.h>
#endif
#else
#include <drm/drm_mode.h>
#endif

#include <xf86drm.h>
#include <xf86drmMode.h>

static double clamp01(double v) {
    if (v < 0.0) {
        return 0.0;
    }
    if (v > 1.0) {
        return 1.0;
    }
    return v;
}

void video_gamma_init(VideoGamma *gamma, const AppCfg *cfg) {
    if (gamma == NULL) {
        return;
    }

    memset(gamma, 0, sizeof(*gamma));
    gamma->gamma_pow = 1.0;
    gamma->gain = 1.0;
    gamma->channel_mul[0] = 1.0;
    gamma->channel_mul[1] = 1.0;
    gamma->channel_mul[2] = 1.0;
    gamma->drm_fd = -1;

    if (cfg != NULL) {
        gamma->enabled = cfg->video_gamma.enable ? TRUE : FALSE;
        gamma->lift = cfg->video_gamma.lift;
        if (cfg->video_gamma.gamma_pow > 0.0) {
            gamma->gamma_pow = cfg->video_gamma.gamma_pow;
        }
        if (cfg->video_gamma.gain >= 0.0) {
            gamma->gain = cfg->video_gamma.gain;
        }
        gamma->channel_mul[0] = cfg->video_gamma.channel_mul[0];
        gamma->channel_mul[1] = cfg->video_gamma.channel_mul[1];
        gamma->channel_mul[2] = cfg->video_gamma.channel_mul[2];
    }
}

void video_gamma_reset(VideoGamma *gamma) {
    if (gamma == NULL) {
        return;
    }

    if (gamma->applied && gamma->drm_fd >= 0 && gamma->prop_id != 0 && gamma->crtc_id != 0) {
        if (drmModeObjectSetProperty(gamma->drm_fd, gamma->crtc_id, DRM_MODE_OBJECT_CRTC, gamma->prop_id, 0) != 0) {
            LOGW("Video gamma: failed to clear GAMMA_LUT property: %s", strerror(errno));
        }
    }

    if (gamma->blob_id != 0 && gamma->drm_fd >= 0) {
        drmModeDestroyPropertyBlob(gamma->drm_fd, gamma->blob_id);
    }

    gamma->blob_id = 0;
    gamma->applied = FALSE;
    gamma->drm_fd = -1;
    gamma->crtc_id = 0;
    gamma->prop_id = 0;
    gamma->lut_size = 0;
}

static gboolean video_gamma_query_props(VideoGamma *gamma) {
    if (gamma == NULL || gamma->drm_fd < 0 || gamma->crtc_id == 0) {
        return FALSE;
    }

    if (drm_get_prop_id(gamma->drm_fd, gamma->crtc_id, DRM_MODE_OBJECT_CRTC, "GAMMA_LUT", &gamma->prop_id) != 0) {
        LOGW("Video gamma: CRTC %u does not expose GAMMA_LUT", gamma->crtc_id);
        return FALSE;
    }

    drmModeObjectProperties *props = drmModeObjectGetProperties(gamma->drm_fd, gamma->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (!props) {
        LOGW("Video gamma: failed to query properties for CRTC %u", gamma->crtc_id);
        return FALSE;
    }

    gboolean found_size = FALSE;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyRes *p = drmModeGetProperty(gamma->drm_fd, props->props[i]);
        if (!p) {
            continue;
        }
        if (strcmp(p->name, "GAMMA_LUT_SIZE") == 0) {
            if (i < props->count_props) {
                uint64_t value = props->prop_values[i];
                if (value > 0 && value <= UINT32_MAX) {
                    gamma->lut_size = (uint32_t)value;
                    found_size = TRUE;
                }
            }
        }
        drmModeFreeProperty(p);
        if (found_size) {
            break;
        }
    }

    drmModeFreeObjectProperties(props);

    if (!found_size || gamma->lut_size == 0) {
        LOGW("Video gamma: unable to determine GAMMA_LUT_SIZE for CRTC %u", gamma->crtc_id);
        gamma->prop_id = 0;
        return FALSE;
    }

    return TRUE;
}

static gboolean video_gamma_apply(VideoGamma *gamma) {
    if (gamma == NULL || gamma->drm_fd < 0 || gamma->prop_id == 0 || gamma->lut_size < 2) {
        return FALSE;
    }

    struct drm_color_lut *table = g_new0(struct drm_color_lut, gamma->lut_size);
    if (table == NULL) {
        LOGE("Video gamma: failed to allocate LUT (%u entries)", gamma->lut_size);
        return FALSE;
    }

    const double inv = (gamma->lut_size > 1) ? 1.0 / (double)(gamma->lut_size - 1) : 0.0;

    for (uint32_t i = 0; i < gamma->lut_size; ++i) {
        double x = (double)i * inv;
        double shaped = pow(x, gamma->gamma_pow);
        double base = gamma->lift + gamma->gain * shaped;
        double r = clamp01(base * gamma->channel_mul[0]);
        double g = clamp01(base * gamma->channel_mul[1]);
        double b = clamp01(base * gamma->channel_mul[2]);
        table[i].red = (uint16_t)lrint(r * 65535.0);
        table[i].green = (uint16_t)lrint(g * 65535.0);
        table[i].blue = (uint16_t)lrint(b * 65535.0);
    }

    uint32_t blob_id = 0;
    size_t blob_size = sizeof(struct drm_color_lut) * (size_t)gamma->lut_size;
    if (drmModeCreatePropertyBlob(gamma->drm_fd, table, blob_size, &blob_id) != 0) {
        LOGW("Video gamma: failed to create GAMMA_LUT blob: %s", strerror(errno));
        g_free(table);
        return FALSE;
    }

    if (drmModeObjectSetProperty(gamma->drm_fd, gamma->crtc_id, DRM_MODE_OBJECT_CRTC, gamma->prop_id, blob_id) != 0) {
        LOGW("Video gamma: failed to set GAMMA_LUT on CRTC %u: %s", gamma->crtc_id, strerror(errno));
        drmModeDestroyPropertyBlob(gamma->drm_fd, blob_id);
        g_free(table);
        return FALSE;
    }

    gamma->blob_id = blob_id;
    gamma->applied = TRUE;

    LOGI("Video gamma: applied %u-entry GAMMA_LUT on CRTC %u (lift=%.3f gamma=%.3f gain=%.3f rgb=%.3f/%.3f/%.3f)",
         gamma->lut_size, gamma->crtc_id, gamma->lift, gamma->gamma_pow, gamma->gain, gamma->channel_mul[0],
         gamma->channel_mul[1], gamma->channel_mul[2]);

    g_free(table);
    return TRUE;
}

void video_gamma_configure_crtc(VideoGamma *gamma, int drm_fd, uint32_t crtc_id) {
    if (gamma == NULL) {
        return;
    }

    /* Release any previously applied LUT before reconfiguring. */
    video_gamma_reset(gamma);

    if (!gamma->enabled) {
        return;
    }

    if (drm_fd < 0 || crtc_id == 0) {
        LOGW("Video gamma: invalid DRM target");
        return;
    }

    gamma->drm_fd = drm_fd;
    gamma->crtc_id = crtc_id;

    if (!video_gamma_query_props(gamma)) {
        video_gamma_reset(gamma);
        return;
    }

    if (!video_gamma_apply(gamma)) {
        video_gamma_reset(gamma);
    }
}
