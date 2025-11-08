#include "drm_fb.h"
#include "drm_modeset.h"
#include "drm_props.h"
#include "logging.h"

#include <errno.h>
#include <math.h>
#include <string.h>
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

typedef struct {
    uint32_t p_fb_id;
    uint32_t p_crtc_id;
    uint32_t p_crtc_x;
    uint32_t p_crtc_y;
    uint32_t p_crtc_w;
    uint32_t p_crtc_h;
    uint32_t p_src_x;
    uint32_t p_src_y;
    uint32_t p_src_w;
    uint32_t p_src_h;
} PlaneBasicProps;

static int plane_get_basic_props(int fd, uint32_t plane_id, PlaneBasicProps *out) {
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &out->p_fb_id) != 0 ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &out->p_crtc_id) != 0 ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", &out->p_crtc_x) != 0 ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &out->p_crtc_y) != 0 ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", &out->p_crtc_w) != 0 ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", &out->p_crtc_h) != 0 ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", &out->p_src_x) != 0 ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", &out->p_src_y) != 0 ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", &out->p_src_w) != 0 ||
        drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", &out->p_src_h) != 0) {
        return -1;
    }
    return 0;
}

static int plane_accepts_linear_nv12(int fd, uint32_t plane_id, uint32_t crtc_id) {
    PlaneBasicProps props;
    if (plane_get_basic_props(fd, plane_id, &props) != 0) {
        return 0;
    }

    struct DumbFB fb = {0};
    if (create_nv12_linear_fb(fd, 128, 72, &fb) != 0) {
        return 0;
    }

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        destroy_dumb_fb(fd, &fb);
        return 0;
    }

    drmModeAtomicAddProperty(req, plane_id, props.p_fb_id, fb.fb_id);
    drmModeAtomicAddProperty(req, plane_id, props.p_crtc_id, crtc_id);
    drmModeAtomicAddProperty(req, plane_id, props.p_crtc_x, 0);
    drmModeAtomicAddProperty(req, plane_id, props.p_crtc_y, 0);
    drmModeAtomicAddProperty(req, plane_id, props.p_crtc_w, fb.w);
    drmModeAtomicAddProperty(req, plane_id, props.p_crtc_h, fb.h);
    drmModeAtomicAddProperty(req, plane_id, props.p_src_x, 0);
    drmModeAtomicAddProperty(req, plane_id, props.p_src_y, 0);
    drmModeAtomicAddProperty(req, plane_id, props.p_src_w, (uint64_t)fb.w << 16);
    drmModeAtomicAddProperty(req, plane_id, props.p_src_h, (uint64_t)fb.h << 16);

    int ok = (drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_TEST_ONLY, NULL) == 0);

    drmModeAtomicFree(req);
    destroy_dumb_fb(fd, &fb);
    return ok;
}

static int get_plane_type(int fd, uint32_t plane_id, int *out_type) {
    if (!out_type) {
        return -1;
    }
    uint32_t type_prop = 0;
    if (drm_get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "type", &type_prop) != 0) {
        return -1;
    }
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props) {
        return -1;
    }
    int result = -1;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        if (props->props[i] == type_prop) {
            *out_type = (int)props->prop_values[i];
            result = 0;
            break;
        }
    }
    drmModeFreeObjectProperties(props);
    return result;
}

static int find_crtc_index(const drmModeRes *res, uint32_t crtc_id) {
    if (!res) {
        return -1;
    }
    for (int i = 0; i < res->count_crtcs; ++i) {
        if ((uint32_t)res->crtcs[i] == crtc_id) {
            return i;
        }
    }
    return -1;
}

static int pick_nv12_plane(int fd, const drmModeRes *res, uint32_t crtc_id, uint32_t preferred,
                           uint32_t avoid, uint32_t *out_plane) {
    if (!out_plane) {
        return -1;
    }

    if (preferred && plane_accepts_linear_nv12(fd, preferred, crtc_id)) {
        *out_plane = preferred;
        return 0;
    }

    int crtc_index = find_crtc_index(res, crtc_id);
    if (crtc_index < 0) {
        return -1;
    }

    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    if (!planes) {
        return -1;
    }

    uint32_t chosen = 0;
    int best_score = -1000000;
    for (int pass = 0; pass < 2 && chosen == 0; ++pass) {
        for (uint32_t i = 0; i < planes->count_planes; ++i) {
            uint32_t plane_id = planes->planes[i];
            if (plane_id == 0) {
                continue;
            }
            if (preferred && plane_id == preferred) {
                continue;
            }
            if (pass == 0 && avoid && plane_id == avoid) {
                continue;
            }

            drmModePlane *p = drmModeGetPlane(fd, plane_id);
            if (!p) {
                continue;
            }
            int usable = 0;
            if ((p->possible_crtcs & (1U << crtc_index)) != 0) {
                if (plane_accepts_linear_nv12(fd, plane_id, crtc_id)) {
                    int score = 0;
                    int type = 0;
                    if (get_plane_type(fd, plane_id, &type) == 0) {
                        if (type == DRM_PLANE_TYPE_PRIMARY) {
                            score += 200;
                        } else if (type == DRM_PLANE_TYPE_OVERLAY) {
                            score += 100;
                        }
                    }
                    score += (int)p->count_formats;
                    if (score > best_score) {
                        best_score = score;
                        chosen = plane_id;
                        usable = 1;
                    }
                }
            }
            drmModeFreePlane(p);
            if (usable) {
                break;
            }
        }
    }

    drmModeFreePlaneResources(planes);

    if (chosen) {
        *out_plane = chosen;
        return 0;
    }
    return -1;
}

static int better_mode(const drmModeModeInfo *a, const drmModeModeInfo *b) {
    int ap = mode_is_preferred(a);
    int bp = mode_is_preferred(b);
    if (ap != bp) {
        return ap > bp;
    }

    int ahz = vrefresh(a), bhz = vrefresh(b);
    if (ahz != bhz) {
        return ahz > bhz;
    }
    long long aa = (long long)a->hdisplay * a->vdisplay;
    long long bb = (long long)b->hdisplay * b->vdisplay;
    if (aa != bb) {
        return aa > bb;
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
    uint32_t preferred_plane = (cfg->plane_id > 0) ? (uint32_t)cfg->plane_id : 0;
    uint32_t avoid_plane = (osd_enabled && cfg->osd_plane_id > 0) ? (uint32_t)cfg->osd_plane_id : 0;
    uint32_t video_plane_id = 0;
    if (pick_nv12_plane(fd, res, crtc->crtc_id, preferred_plane, avoid_plane, &video_plane_id) != 0) {
        LOGE("Failed to find NV12-capable plane for CRTC %d", crtc->crtc_id);
        drmModeFreeConnector(conn);
        drmModeFreeCrtc(crtc);
        drmModeFreeResources(res);
        return -3;
    }
    if (preferred_plane && video_plane_id != preferred_plane) {
        LOGW("Configured video plane %u is not usable on CRTC %d; falling back to %u", preferred_plane, crtc->crtc_id,
             video_plane_id);
    }
    LOGI("Chosen: %s id=%u  %dx%d@%d  CRTC=%d  plane=%u", cname, conn->connector_id, w, h, hz, crtc->crtc_id, video_plane_id);

    uint32_t mode_blob = 0;
    if (drmModeCreatePropertyBlob(fd, &best, sizeof(best), &mode_blob) != 0) {
        LOGE("drmModeCreatePropertyBlob failed: %s", strerror(errno));
        drmModeFreeConnector(conn);
        drmModeFreeCrtc(crtc);
        drmModeFreeResources(res);
        return -4;
    }

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        LOGE("drmModeAtomicAlloc failed");
        drmModeDestroyPropertyBlob(fd, mode_blob);
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
    int have_zpos = (drm_get_prop_id_and_range_ci(fd, video_plane_id, DRM_MODE_OBJECT_PLANE, "ZPOS", &plane_zpos_id,
                                                  &zmin, &zmax, "zpos") == 0);

    drm_get_prop_id(fd, video_plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &plane_fb_id);
    drm_get_prop_id(fd, video_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &plane_crtc_id);
    drm_get_prop_id(fd, video_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", &plane_crtc_x);
    drm_get_prop_id(fd, video_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &plane_crtc_y);
    drm_get_prop_id(fd, video_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", &plane_crtc_w);
    drm_get_prop_id(fd, video_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", &plane_crtc_h);
    drm_get_prop_id(fd, video_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", &plane_src_x);
    drm_get_prop_id(fd, video_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", &plane_src_y);
    drm_get_prop_id(fd, video_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", &plane_src_w);
    drm_get_prop_id(fd, video_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", &plane_src_h);

    // Disable the video plane until the decoder attaches a real framebuffer.
    // Some hardware (e.g. Rockchip VOP2 cluster planes) reject linear dummy
    // buffers, so keep the plane idle instead of binding a placeholder FB.
    drmModeAtomicAddProperty(req, video_plane_id, plane_fb_id, 0);
    drmModeAtomicAddProperty(req, video_plane_id, plane_crtc_id, 0);
    drmModeAtomicAddProperty(req, video_plane_id, plane_crtc_x, 0);
    drmModeAtomicAddProperty(req, video_plane_id, plane_crtc_y, 0);
    drmModeAtomicAddProperty(req, video_plane_id, plane_crtc_w, 0);
    drmModeAtomicAddProperty(req, video_plane_id, plane_crtc_h, 0);
    drmModeAtomicAddProperty(req, video_plane_id, plane_src_x, 0);
    drmModeAtomicAddProperty(req, video_plane_id, plane_src_y, 0);
    drmModeAtomicAddProperty(req, video_plane_id, plane_src_w, 0);
    drmModeAtomicAddProperty(req, video_plane_id, plane_src_h, 0);

    if (have_zpos) {
        uint64_t v_z = zmax;
        if (osd_enabled && zmax > zmin) {
            v_z = zmax - 1;
        }
        drmModeAtomicAddProperty(req, video_plane_id, plane_zpos_id, v_z);
    }

    int flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    int ret = drmModeAtomicCommit(fd, req, flags, NULL);
    if (ret != 0) {
        LOGE("drmModeAtomicCommit failed: %s", strerror(errno));
        drmModeAtomicFree(req);
        drmModeDestroyPropertyBlob(fd, mode_blob);
        drmModeFreeConnector(conn);
        drmModeFreeCrtc(crtc);
        drmModeFreeResources(res);
        return -9;
    }

    LOGI("Atomic COMMIT: %dx%d@%d on %s via plane %u", w, h, hz, cname, video_plane_id);

    drmModeAtomicFree(req);
    drmModeDestroyPropertyBlob(fd, mode_blob);

    if (out) {
        out->connector_id = conn->connector_id;
        out->crtc_id = crtc->crtc_id;
        out->video_plane_id = video_plane_id;
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
