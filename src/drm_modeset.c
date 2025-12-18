#include "drm_modeset.h"
#include "drm_props.h"
#include "logging.h"

#include <errno.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>

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
#else
#include <libdrm/drm.h>
#include <libdrm/drm_fourcc.h>
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

static inline int mode_is_preferred(const drmModeModeInfo *m) {
    return (m->type & DRM_MODE_TYPE_PREFERRED) ? 1 : 0;
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

    int ap = mode_is_preferred(a);
    int bp = mode_is_preferred(b);
    if (ap != bp) {
        return ap > bp;
    }

    return a->clock > b->clock;
}

typedef struct {
    drmModeConnector *conn;
    drmModeCrtc *crtc;
    drmModeModeInfo mode;
    int matched_request;
} DrmConnectorSelection;

static void release_selection(DrmConnectorSelection *sel) {
    if (sel == NULL) {
        return;
    }
    if (sel->conn != NULL) {
        drmModeFreeConnector(sel->conn);
        sel->conn = NULL;
    }
    if (sel->crtc != NULL) {
        drmModeFreeCrtc(sel->crtc);
        sel->crtc = NULL;
    }
    memset(&sel->mode, 0, sizeof(sel->mode));
    sel->matched_request = 0;
}

typedef struct {
    int w;
    int h;
    int hz;
    int present;
} RequestedMode;

static RequestedMode requested_mode_from_cfg(const AppCfg *cfg) {
    RequestedMode req = {0, 0, 0, 0};
    if (cfg == NULL) {
        return req;
    }
    if (cfg->mode_w > 0 && cfg->mode_h > 0) {
        req.w = cfg->mode_w;
        req.h = cfg->mode_h;
        req.hz = (cfg->mode_hz > 0) ? cfg->mode_hz : 0;
        req.present = 1;
    }
    return req;
}

static int mode_matches_request(const drmModeModeInfo *mode, const RequestedMode *req) {
    if (mode == NULL || req == NULL || !req->present) {
        return 0;
    }
    if ((int)mode->hdisplay != req->w || (int)mode->vdisplay != req->h) {
        return 0;
    }
    if (req->hz > 0 && vrefresh(mode) != req->hz) {
        return 0;
    }
    return 1;
}

static void format_requested_mode(const RequestedMode *req, char *buf, size_t buf_sz) {
    if (buf == NULL || buf_sz == 0) {
        return;
    }
    if (req == NULL || !req->present) {
        g_strlcpy(buf, "auto", buf_sz);
        return;
    }
    if (req->hz > 0) {
        g_snprintf(buf, buf_sz, "%dx%d@%d", req->w, req->h, req->hz);
    } else {
        g_snprintf(buf, buf_sz, "%dx%d", req->w, req->h);
    }
}

static drmModeModeInfo pick_mode_for_connector(const drmModeConnector *conn,
                                               const RequestedMode *req,
                                               int *matched_request) {
    drmModeModeInfo best = conn->modes[0];
    int selected_index = 0;
    if (matched_request != NULL) {
        *matched_request = 0;
    }

    if (req != NULL && req->present) {
        int found = 0;
        for (int m = 0; m < conn->count_modes; ++m) {
            const drmModeModeInfo *cand = &conn->modes[m];
            if (!mode_matches_request(cand, req)) {
                continue;
            }
            if (!found || better_mode(cand, &conn->modes[selected_index])) {
                selected_index = m;
            }
            found = 1;
        }
        if (found) {
            if (matched_request != NULL) {
                *matched_request = 1;
            }
            return conn->modes[selected_index];
        }
    }

    for (int m = 1; m < conn->count_modes; ++m) {
        if (better_mode(&conn->modes[m], &best)) {
            best = conn->modes[m];
        }
    }
    return best;
}

static int pick_best_connector(int fd, const AppCfg *cfg, DrmConnectorSelection *out) {
    if (cfg == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    RequestedMode req = requested_mode_from_cfg(cfg);

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        LOGE("drmModeGetResources failed");
        return -1;
    }

    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) {
            continue;
        }

        char cname[32];
        snprintf(cname, sizeof(cname), "%s-%u", conn_type_str(c->connector_type), c->connector_type_id);

        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 &&
            (!cfg->connector_name[0] || strcmp(cfg->connector_name, cname) == 0)) {
            int matched = 0;
            drmModeModeInfo best = pick_mode_for_connector(c, &req, &matched);

            drmModeEncoder *enc = NULL;
            if (c->encoder_id) {
                enc = drmModeGetEncoder(fd, c->encoder_id);
            }
            int crtc_id = -1;
            drmModeCrtc *crtc = NULL;
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
                out->conn = c;
                out->crtc = crtc;
                out->mode = best;
                out->matched_request = matched;
                drmModeFreeResources(res);
                return 0;
            }
            if (crtc) {
                drmModeFreeCrtc(crtc);
            }
        }
        drmModeFreeConnector(c);
    }

    drmModeFreeResources(res);
    LOGI("No CONNECTED connector with modes");
    return -2;
}

int atomic_modeset_maxhz(int fd, const AppCfg *cfg, int osd_enabled, ModesetResult *out) {
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
        LOGW("Failed to enable UNIVERSAL_PLANES");
    }
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        LOGW("Failed to enable ATOMIC");
    }

    RequestedMode req = requested_mode_from_cfg(cfg);
    DrmConnectorSelection selection;
    int pick = pick_best_connector(fd, cfg, &selection);
    if (pick != 0) {
        release_selection(&selection);
        return pick;
    }
    drmModeConnector *conn = selection.conn;
    drmModeCrtc *crtc = selection.crtc;
    drmModeModeInfo best = selection.mode;

    char cname[32];
    snprintf(cname, sizeof(cname), "%s-%u", conn_type_str(conn->connector_type), conn->connector_type_id);
    int w = best.hdisplay;
    int h = best.vdisplay;
    int hz = vrefresh(&best);
    LOGI("Chosen: %s id=%u  %dx%d@%d  CRTC=%d  plane=%d", cname, conn->connector_id, w, h, hz, crtc->crtc_id, cfg->plane_id);
    if (req.present && !selection.matched_request) {
        char req_buf[32];
        format_requested_mode(&req, req_buf, sizeof(req_buf));
        LOGW("Requested mode %s not found; using %dx%d@%d instead", req_buf, w, h, hz);
    }

    uint32_t mode_blob = 0;
    if (drmModeCreatePropertyBlob(fd, &best, sizeof(best), &mode_blob) != 0) {
        LOGE("drmModeCreatePropertyBlob failed: %s", strerror(errno));
        release_selection(&selection);
        return -4;
    }

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        LOGE("drmModeAtomicAlloc failed");
        drmModeDestroyPropertyBlob(fd, mode_blob);
        release_selection(&selection);
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

    // Disable the video plane until the decoder attaches a real framebuffer.
    // Some hardware (e.g. Rockchip VOP2 cluster planes) reject linear dummy
    // buffers, so keep the plane idle instead of binding a placeholder FB.
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_fb_id, 0);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_crtc_id, 0);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_crtc_x, 0);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_crtc_y, 0);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_crtc_w, 0);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_crtc_h, 0);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_src_x, 0);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_src_y, 0);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_src_w, 0);
    drmModeAtomicAddProperty(req, cfg->plane_id, plane_src_h, 0);

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
        drmModeDestroyPropertyBlob(fd, mode_blob);
        release_selection(&selection);
        return -9;
    }

    LOGI("Atomic COMMIT: %dx%d@%d on %s via plane %d", w, h, hz, cname, cfg->plane_id);

    drmModeAtomicFree(req);
    drmModeDestroyPropertyBlob(fd, mode_blob);

    if (out) {
        out->connector_id = conn->connector_id;
        out->crtc_id = crtc->crtc_id;
        out->mode_w = w;
        out->mode_h = h;
        out->mode_hz = hz;
    }

    release_selection(&selection);
    return 0;
}

int probe_maxhz_mode(int fd, const AppCfg *cfg, ModesetResult *out) {
    DrmConnectorSelection selection;
    int ret = pick_best_connector(fd, cfg, &selection);
    if (ret != 0) {
        release_selection(&selection);
        return ret;
    }

    if (out != NULL && selection.conn != NULL && selection.crtc != NULL) {
        out->connector_id = selection.conn->connector_id;
        out->crtc_id = selection.crtc->crtc_id;
        out->mode_w = selection.mode.hdisplay;
        out->mode_h = selection.mode.vdisplay;
        out->mode_hz = vrefresh(&selection.mode);
    }

    release_selection(&selection);
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
