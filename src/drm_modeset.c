#include "drm_modeset.h"
#include "drm_fb.h"
#include "drm_props.h"
#include "logging.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<libdrm/drm.h>)
#include <libdrm/drm.h>
#else
#include <drm/drm.h>
#endif
#if __has_include(<libdrm/drm_fourcc.h>)
#include <libdrm/drm_fourcc.h>
#else
#include <drm/drm_fourcc.h>
#endif
#if __has_include(<libdrm/drm_mode.h>)
#include <libdrm/drm_mode.h>
#else
#include <drm/drm_mode.h>
#endif
#else
#include <libdrm/drm.h>
#include <libdrm/drm_fourcc.h>
#include <libdrm/drm_mode.h>
#endif
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

#define GAMMA_COMPONENT_MAX 65535u

static char *trim_leading_ws(char *s) {
    if (!s) {
        return NULL;
    }
    while (*s && isspace((unsigned char)*s)) {
        ++s;
    }
    return s;
}

static void trim_trailing_ws(char *s) {
    if (!s) {
        return;
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static int parse_gamma_component(const char *token, uint16_t *out) {
    if (!token || !out) {
        return -1;
    }

    errno = 0;
    char *end = NULL;
    if (strchr(token, '.') || strchr(token, 'e') || strchr(token, 'E')) {
        double v = strtod(token, &end);
        if (end == token || errno == ERANGE) {
            return -1;
        }
        if (v < 0.0) {
            v = 0.0;
        } else if (v > 1.0) {
            v = 1.0;
        }
        unsigned int scaled = (unsigned int)lrint(v * (double)GAMMA_COMPONENT_MAX);
        if (scaled > GAMMA_COMPONENT_MAX) {
            scaled = GAMMA_COMPONENT_MAX;
        }
        *out = (uint16_t)scaled;
        return 0;
    }

    long v = strtol(token, &end, 0);
    if (end == token || errno == ERANGE) {
        return -1;
    }
    if (v < 0) {
        v = 0;
    } else if (v > (long)GAMMA_COMPONENT_MAX) {
        v = (long)GAMMA_COMPONENT_MAX;
    }
    *out = (uint16_t)v;
    return 0;
}

static int load_gamma_lut_data(const AppCfg *cfg, size_t expected_size, struct drm_color_lut **out_data) {
    if (!cfg || !out_data || expected_size == 0) {
        return -1;
    }

    const char *path = cfg->gamma.lut_path;
    if (!path || path[0] == '\0') {
        return -1;
    }

    if (strcasecmp(path, "identity") == 0 || strcasecmp(path, "builtin:identity") == 0) {
        struct drm_color_lut *lut = calloc(expected_size, sizeof(*lut));
        if (!lut) {
            LOGE("gamma: failed to allocate identity LUT buffer (%zu entries)", expected_size);
            return -1;
        }
        if (expected_size == 1) {
            uint16_t value = (uint16_t)GAMMA_COMPONENT_MAX;
            lut[0].red = value;
            lut[0].green = value;
            lut[0].blue = value;
            lut[0].reserved = 0;
        } else {
            for (size_t i = 0; i < expected_size; ++i) {
                double ratio = (double)i / (double)(expected_size - 1);
                uint16_t value = (uint16_t)lrint(ratio * (double)GAMMA_COMPONENT_MAX);
                if (value > GAMMA_COMPONENT_MAX) {
                    value = (uint16_t)GAMMA_COMPONENT_MAX;
                }
                lut[i].red = value;
                lut[i].green = value;
                lut[i].blue = value;
                lut[i].reserved = 0;
            }
        }
        LOGI("gamma: using built-in identity LUT (%zu entries)", expected_size);
        *out_data = lut;
        return 0;
    }

    char resolved[PATH_MAX];
    resolved[0] = '\0';
    FILE *fp = fopen(path, "r");
    if (fp) {
        strncpy(resolved, path, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    } else if (cfg->config_path[0] != '\0' && path[0] != '/' && path[0] != '\0') {
        char base[PATH_MAX];
        strncpy(base, cfg->config_path, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        char *slash = strrchr(base, '/');
        if (slash) {
            slash[1] = '\0';
            snprintf(resolved, sizeof(resolved), "%s%s", base, path);
            fp = fopen(resolved, "r");
        }
        if (!fp) {
            resolved[0] = '\0';
        }
    }

    if (!fp) {
        LOGE("gamma: failed to open LUT file '%s': %s", path, strerror(errno));
        return -1;
    }

    if (resolved[0] == '\0') {
        strncpy(resolved, path, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }

    struct drm_color_lut *lut = calloc(expected_size, sizeof(*lut));
    if (!lut) {
        LOGE("gamma: failed to allocate LUT buffer (%zu entries)", expected_size);
        fclose(fp);
        return -1;
    }

    char line[512];
    size_t index = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim_leading_ws(line);
        if (!p || *p == '\0') {
            continue;
        }
        char *comment = strpbrk(p, "#;");
        if (comment) {
            *comment = '\0';
        }
        trim_trailing_ws(p);
        if (*p == '\0') {
            continue;
        }

        uint16_t values[3] = {0, 0, 0};
        int count = 0;
        char *save = NULL;
        char *token = strtok_r(p, ", \t", &save);
        while (token && count < 3) {
            if (*token == '\0') {
                token = strtok_r(NULL, ", \t", &save);
                continue;
            }
            if (parse_gamma_component(token, &values[count]) != 0) {
                LOGE("gamma: invalid component '%s' in %s", token, resolved);
                free(lut);
                fclose(fp);
                return -1;
            }
            ++count;
            token = strtok_r(NULL, ", \t", &save);
        }

        if (count == 0) {
            continue;
        }
        if (count == 1) {
            values[1] = values[0];
            values[2] = values[0];
            count = 3;
        } else if (count == 2) {
            values[2] = values[1];
            count = 3;
        }
        if (count != 3) {
            LOGE("gamma: expected 3 values per entry in %s", resolved);
            free(lut);
            fclose(fp);
            return -1;
        }
        if (index >= expected_size) {
            LOGE("gamma: too many entries in %s (expected %zu)", resolved, expected_size);
            free(lut);
            fclose(fp);
            return -1;
        }

        lut[index].red = values[0];
        lut[index].green = values[1];
        lut[index].blue = values[2];
        lut[index].reserved = 0;
        ++index;
    }

    fclose(fp);

    if (index != expected_size) {
        LOGE("gamma: %s contained %zu entries; expected %zu", resolved, index, expected_size);
        free(lut);
        return -1;
    }

    LOGI("gamma: loaded %zu-entry LUT from %s", index, resolved);
    *out_data = lut;
    return 0;
}

static const char *conn_type_str(uint32_t t) {
    switch (t) {
    case DRM_MODE_CONNECTOR_HDMIA:
        return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB:
        return "HDMI-B";
    case DRM_MODE_CONNECTOR_DisplayPort:
        return "DP";
    case DRM_MODE_CONNECTOR_eDP:
        return "eDP";
    case DRM_MODE_CONNECTOR_VGA:
        return "VGA";
    default:
        return "UNKNOWN";
    }
}

static int vrefresh(const drmModeModeInfo *m) {
    if (m->vrefresh) {
        return m->vrefresh;
    }
    if (m->htotal && m->vtotal) {
        double hz = (double)m->clock * 1000.0 / (m->htotal * m->vtotal);
        return (int)(hz + 0.5);
    }
    return 0;
}

static int better_mode(const drmModeModeInfo *a, const drmModeModeInfo *b) {
    int ahz = vrefresh(a), bhz = vrefresh(b);
    if (ahz != bhz) {
        return ahz > bhz;
    }
    long long aa = (long long)a->hdisplay * a->vdisplay;
    long long bb = (long long)b->hdisplay * b->vdisplay;
    if (aa != bb) {
        return aa > bb;
    }
    int ap = (a->type & DRM_MODE_TYPE_PREFERRED) ? 1 : 0;
    int bp = (b->type & DRM_MODE_TYPE_PREFERRED) ? 1 : 0;
    if (ap != bp) {
        return ap > bp;
    }
    return a->clock > b->clock;
}

int atomic_modeset_maxhz(int fd, const AppCfg *cfg, int osd_enabled, ModesetResult *out) {
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
        LOGW("Failed to enable UNIVERSAL_PLANES");
    }
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        LOGW("Failed to enable ATOMIC");
    }

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        LOGE("drmModeGetResources failed");
        return -1;
    }

    drmModeConnector *conn = NULL;
    drmModeCrtc *crtc = NULL;
    drmModeModeInfo best = {0};

    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) {
            continue;
        }

        char cname[32];
        snprintf(cname, sizeof(cname), "%s-%u", conn_type_str(c->connector_type), c->connector_type_id);

        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 &&
            (!cfg->connector_name[0] || strcmp(cfg->connector_name, cname) == 0)) {
            best = c->modes[0];
            for (int m = 1; m < c->count_modes; ++m) {
                if (better_mode(&c->modes[m], &best)) {
                    best = c->modes[m];
                }
            }

            drmModeEncoder *enc = NULL;
            if (c->encoder_id) {
                enc = drmModeGetEncoder(fd, c->encoder_id);
            }
            int crtc_id = -1;
            if (enc && enc->crtc_id) {
                crtc = drmModeGetCrtc(fd, enc->crtc_id);
                if (crtc) {
                    crtc_id = crtc->crtc_id;
                }
            }
            if (crtc_id < 0) {
                for (int e = 0; e < c->count_encoders && crtc_id < 0; ++e) {
                    drmModeEncoder *e2 = drmModeGetEncoder(fd, c->encoders[e]);
                    if (!e2) {
                        continue;
                    }
                    for (int ci = 0; ci < res->count_crtcs; ++ci) {
                        if (e2->possible_crtcs & (1 << ci)) {
                            crtc = drmModeGetCrtc(fd, res->crtcs[ci]);
                            if (crtc) {
                                crtc_id = crtc->crtc_id;
                                break;
                            }
                        }
                    }
                    drmModeFreeEncoder(e2);
                }
            }
            if (enc) {
                drmModeFreeEncoder(enc);
            }

            if (crtc_id >= 0 && crtc) {
                conn = c;
                break;
            }
        }
        drmModeFreeConnector(c);
    }

    if (!conn) {
        LOGI("No CONNECTED connector with modes");
        drmModeFreeResources(res);
        return -2;
    }

    char cname[32];
    snprintf(cname, sizeof(cname), "%s-%u", conn_type_str(conn->connector_type), conn->connector_type_id);
    int w = best.hdisplay;
    int h = best.vdisplay;
    int hz = vrefresh(&best);
    LOGI("Chosen: %s id=%u  %dx%d@%d  CRTC=%d  plane=%d", cname, conn->connector_id, w, h, hz, crtc->crtc_id, cfg->plane_id);

    struct DumbFB fb = {0};
    if (create_argb_fb(fd, w, h, 0xFF000000u, &fb) != 0) {
        LOGE("create_argb_fb failed: %s", strerror(errno));
        drmModeFreeConnector(conn);
        drmModeFreeCrtc(crtc);
        drmModeFreeResources(res);
        return -3;
    }

    uint32_t mode_blob = 0;
    if (drmModeCreatePropertyBlob(fd, &best, sizeof(best), &mode_blob) != 0) {
        LOGE("drmModeCreatePropertyBlob failed: %s", strerror(errno));
        destroy_dumb_fb(fd, &fb);
        drmModeFreeConnector(conn);
        drmModeFreeCrtc(crtc);
        drmModeFreeResources(res);
        return -4;
    }

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        LOGE("drmModeAtomicAlloc failed");
        drmModeDestroyPropertyBlob(fd, mode_blob);
        destroy_dumb_fb(fd, &fb);
        drmModeFreeConnector(conn);
        drmModeFreeCrtc(crtc);
        drmModeFreeResources(res);
        return -5;
    }

    uint32_t crtc_active = 0, crtc_mode_id = 0;
    uint32_t gamma_lut_prop = 0, gamma_lut_blob = 0;
    drm_get_prop_id(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", &crtc_active);
    drm_get_prop_id(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", &crtc_mode_id);
    drmModeAtomicAddProperty(req, crtc->crtc_id, crtc_active, 1);
    drmModeAtomicAddProperty(req, crtc->crtc_id, crtc_mode_id, mode_blob);

    if (cfg->gamma.enable) {
        if (cfg->gamma.lut_path[0] == '\0') {
            LOGW("gamma: enable requested but no gamma-lut path provided; skipping upload");
        } else {
            int gamma_size = drmModeCrtcGetGammaSize(fd, crtc->crtc_id);
            if (gamma_size <= 0) {
                LOGW("gamma: CRTC %d reported LUT size %d; skipping upload", crtc->crtc_id, gamma_size);
            } else if (drm_get_prop_id(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC, "GAMMA_LUT", &gamma_lut_prop) != 0) {
                LOGW("gamma: CRTC %d lacks GAMMA_LUT property; skipping upload", crtc->crtc_id);
            } else {
                struct drm_color_lut *gamma_data = NULL;
                if (load_gamma_lut_data(cfg, (size_t)gamma_size, &gamma_data) == 0) {
                    size_t blob_size = (size_t)gamma_size * sizeof(struct drm_color_lut);
                    if (drmModeCreatePropertyBlob(fd, gamma_data, blob_size, &gamma_lut_blob) != 0) {
                        LOGE("gamma: failed to create LUT blob: %s", strerror(errno));
                        gamma_lut_blob = 0;
                    } else {
                        int add_ret = drmModeAtomicAddProperty(req, crtc->crtc_id, gamma_lut_prop, gamma_lut_blob);
                        if (add_ret < 0) {
                            LOGE("gamma: failed to queue LUT property: %s", strerror(-add_ret));
                            drmModeDestroyPropertyBlob(fd, gamma_lut_blob);
                            gamma_lut_blob = 0;
                        }
                    }
                }
                free(gamma_data);
            }
        }
    }

    uint32_t conn_crtc_id = 0;
    drm_get_prop_id(fd, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &conn_crtc_id);
    drmModeAtomicAddProperty(req, conn->connector_id, conn_crtc_id, crtc->crtc_id);

    uint32_t plane_fb_id = 0, plane_crtc_id = 0, plane_crtc_x = 0, plane_crtc_y = 0;
    uint32_t plane_crtc_w = 0, plane_crtc_h = 0, plane_src_x = 0, plane_src_y = 0;
    uint32_t plane_src_w = 0, plane_src_h = 0;
    uint32_t plane_zpos_id = 0;
    uint64_t zmin = 0, zmax = 0;
    int have_zpos = (drm_get_prop_id_and_range_ci(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "ZPOS",
                                                  &plane_zpos_id, &zmin, &zmax, "zpos") == 0);

    drm_get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &plane_fb_id);
    drm_get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &plane_crtc_id);
    drm_get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", &plane_crtc_x);
    drm_get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &plane_crtc_y);
    drm_get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", &plane_crtc_w);
    drm_get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", &plane_crtc_h);
    drm_get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", &plane_src_x);
    drm_get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", &plane_src_y);
    drm_get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", &plane_src_w);
    drm_get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", &plane_src_h);

    drmModeAtomicAddProperty(req, cfg->plane_id, plane_fb_id, fb.fb_id);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_crtc_id, crtc->crtc_id);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_crtc_x, 0);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_crtc_y, 0);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_crtc_w, w);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_crtc_h, h);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_src_x, 0);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_src_y, 0);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_src_w, (uint64_t)w << 16);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_src_h, (uint64_t)h << 16);

    if (have_zpos) {
        uint64_t v_z = zmax;
        if (osd_enabled && zmax > zmin) {
            v_z = zmax - 1;
        }
        drmModeAtomicAddProperty(req, cfg->plane_id, plane_zpos_id, v_z);
    }

    int flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    int ret = drmModeAtomicCommit(fd, req, flags, NULL);
    if (ret != 0) {
        LOGE("drmModeAtomicCommit failed: %s", strerror(errno));
        drmModeAtomicFree(req);
        if (gamma_lut_blob) {
            drmModeDestroyPropertyBlob(fd, gamma_lut_blob);
        }
        drmModeDestroyPropertyBlob(fd, mode_blob);
        destroy_dumb_fb(fd, &fb);
        drmModeFreeConnector(conn);
        drmModeFreeCrtc(crtc);
        drmModeFreeResources(res);
        return -9;
    }

    LOGI("Atomic COMMIT: %dx%d@%d on %s via plane %d", w, h, hz, cname, cfg->plane_id);

    drmModeAtomicFree(req);
    if (gamma_lut_blob) {
        drmModeDestroyPropertyBlob(fd, gamma_lut_blob);
    }
    drmModeDestroyPropertyBlob(fd, mode_blob);
    destroy_dumb_fb(fd, &fb);

    if (out) {
        out->connector_id = conn->connector_id;
        out->crtc_id = crtc->crtc_id;
        out->mode_w = w;
        out->mode_h = h;
        out->mode_hz = hz;
    }

    drmModeFreeConnector(conn);
    drmModeFreeCrtc(crtc);
    drmModeFreeResources(res);
    return 0;
}

int is_any_connected(int fd, const AppCfg *cfg) {
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        return 0;
    }
    int connected = 0;
    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) {
            continue;
        }
        char cname[32];
        snprintf(cname, sizeof(cname), "%s-%u", conn_type_str(c->connector_type), c->connector_type_id);
        if (c->connection == DRM_MODE_CONNECTED &&
            (!cfg->connector_name[0] || strcmp(cfg->connector_name, cname) == 0)) {
            connected = 1;
            drmModeFreeConnector(c);
            break;
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    return connected;
}
