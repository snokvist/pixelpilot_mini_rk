#include "drm_modeset.h"
#include "drm_fb.h"
#include "drm_props.h"
#include "logging.h"

#include <errno.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include <libdrm/drm.h>
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

static int find_primary_plane_for_crtc(int fd, drmModeRes *res, uint32_t crtc_id) {
    int crtc_index = -1;
    for (int i = 0; i < res->count_crtcs; ++i) {
        if ((uint32_t)res->crtcs[i] == crtc_id) {
            crtc_index = i;
            break;
        }
    }
    if (crtc_index < 0) {
        return -1;
    }

    drmModePlaneRes *prs = drmModeGetPlaneResources(fd);
    if (!prs) {
        return -1;
    }

    int primary_id = -1;
    for (uint32_t i = 0; i < prs->count_planes; ++i) {
        drmModePlane *p = drmModeGetPlane(fd, prs->planes[i]);
        if (!p) {
            continue;
        }
        if ((p->possible_crtcs & (1U << crtc_index)) == 0) {
            drmModeFreePlane(p);
            continue;
        }

        uint32_t type_prop = 0;
        if (drm_get_prop_id(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type_prop) == 0) {
            drmModeObjectProperties *props =
                drmModeObjectGetProperties(fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
            if (props) {
                for (uint32_t k = 0; k < props->count_props; ++k) {
                    if (props->props[k] == type_prop) {
                        uint64_t val = props->prop_values[k];
                        if (val == DRM_PLANE_TYPE_PRIMARY) {
                            primary_id = (int)p->plane_id;
                        }
                        break;
                    }
                }
                drmModeFreeObjectProperties(props);
            }
        }
        drmModeFreePlane(p);
        if (primary_id > 0) {
            break;
        }
    }
    drmModeFreePlaneResources(prs);
    return primary_id;
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
    drm_get_prop_id(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", &crtc_active);
    drm_get_prop_id(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", &crtc_mode_id);
    drmModeAtomicAddProperty(req, crtc->crtc_id, crtc_active, 1);
    drmModeAtomicAddProperty(req, crtc->crtc_id, crtc_mode_id, mode_blob);

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

    int primary_plane_id = cfg->blank_primary ? find_primary_plane_for_crtc(fd, res, crtc->crtc_id) : -1;
    if (primary_plane_id > 0) {
        uint32_t prim_fb_id = 0, prim_crtc_id = 0;
        drm_get_prop_id(fd, (uint32_t)primary_plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &prim_fb_id);
        drm_get_prop_id(fd, (uint32_t)primary_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &prim_crtc_id);
        drmModeAtomicAddProperty(req, (uint32_t)primary_plane_id, prim_fb_id, 0);
        drmModeAtomicAddProperty(req, (uint32_t)primary_plane_id, prim_crtc_id, 0);
    }

    int flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    int ret = drmModeAtomicCommit(fd, req, flags, NULL);
    if (ret != 0) {
        LOGE("drmModeAtomicCommit failed: %s", strerror(errno));
        drmModeAtomicFree(req);
        drmModeDestroyPropertyBlob(fd, mode_blob);
        destroy_dumb_fb(fd, &fb);
        drmModeFreeConnector(conn);
        drmModeFreeCrtc(crtc);
        drmModeFreeResources(res);
        return -9;
    }

    LOGI("Atomic COMMIT: %dx%d@%d on %s via plane %d", w, h, hz, cname, cfg->plane_id);

    drmModeAtomicFree(req);
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
