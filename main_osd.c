// SPDX-License-Identifier: MIT
// pixelpilot_mini_rk — HDMI + atomic KMS + udev hotplug + GStreamer runner + OSD
// Build: gcc -O2 -Wall -o pixelpilot_mini_rk main.c $(pkg-config --cflags --libs libdrm libudev)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <signal.h>
#include <poll.h>
#include <time.h>

#include <libudev.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>
#include <libdrm/drm_fourcc.h>

// Some headers may miss these enums
#ifndef DRM_PLANE_TYPE_OVERLAY
#define DRM_PLANE_TYPE_OVERLAY 0
#endif
#ifndef DRM_PLANE_TYPE_PRIMARY
#define DRM_PLANE_TYPE_PRIMARY 1
#endif
#ifndef DRM_PLANE_TYPE_CURSOR
#define DRM_PLANE_TYPE_CURSOR  2
#endif

// ---------------- Logging ----------------
static int g_verbose = 0;
#define LOG_TS_BUF 32
static const char* ts(void){ static char b[LOG_TS_BUF]; struct timespec t; clock_gettime(CLOCK_REALTIME,&t); struct tm tm; localtime_r(&t.tv_sec,&tm); snprintf(b,sizeof(b),"%02d:%02d:%02d.%03ld", tm.tm_hour, tm.tm_min, tm.tm_sec, t.tv_nsec/1000000); return b; }
#define LOGI(fmt,...) fprintf(stderr,"[%s] [I] " fmt "\n", ts(), ##__VA_ARGS__)
#define LOGW(fmt,...) fprintf(stderr,"[%s] [W] " fmt "\n", ts(), ##__VA_ARGS__)
#define LOGE(fmt,...) fprintf(stderr,"[%s] [E] " fmt "\n", ts(), ##__VA_ARGS__)
#define LOGV(fmt,...) do{ if(g_verbose) fprintf(stderr,"[%s] [D] " fmt "\n", ts(), ##__VA_ARGS__);}while(0)

// --------------- Config / CLI ---------------
typedef struct {
    // DRM
    char card_path[64];         // /dev/dri/card0
    char connector_name[32];    // e.g. HDMI-A-1, optional
    int  plane_id;              // video plane to use (default 76)
    int  blank_primary;         // detach primary plane during initial modeset
    int  stay_blue;             // skip gst, show blue only
    int  blue_hold_ms;          // ms to hold blue after commit
    int  use_udev;              // hotplug listener

    // GStreamer
    int  udp_port;              // default 5600
    int  vid_pt;                // default 97 (H265)
    int  aud_pt;                // default 98 (Opus)
    int  latency_ms;            // default 8
    int  kmssink_sync;          // default false
    int  kmssink_qos;           // default true
    int  max_lateness_ns;       // default 20ms
    char aud_dev[128];          // ALSA device

    // Audio behavior
    int  no_audio;              // drop audio branch
    int  audio_optional;        // auto-fallback to fakesink
    int  restart_limit;         // failures to trigger fallback
    int  restart_window_ms;     // window for failures

    // OSD
    int  osd_enable;
    int  osd_plane_id;          // 0=auto, else force exact plane id
    int  osd_refresh_ms;

    int  gst_log;               // set GST_DEBUG=3 if not set
} AppCfg;

static void cfg_defaults(AppCfg* c){
    memset(c,0,sizeof(*c));
    strcpy(c->card_path, "/dev/dri/card0");
    c->connector_name[0] = 0;
    c->plane_id = 76;
    c->blank_primary = 0;
    c->stay_blue = 0;
    c->blue_hold_ms = 0;
    c->use_udev = 1;
    c->udp_port = 5600;
    c->vid_pt = 97;
    c->aud_pt = 98;
    c->latency_ms = 8;
    c->kmssink_sync = 0;
    c->kmssink_qos  = 1;
    c->max_lateness_ns = 20000000;
    strcpy(c->aud_dev, "plughw:CARD=rockchiphdmi0,DEV=0");

    c->no_audio = 0;
    c->audio_optional = 1;
    c->restart_limit = 3;
    c->restart_window_ms = 2000;

    c->osd_enable = 0;
    c->osd_plane_id = 0;
    c->osd_refresh_ms = 500;

    c->gst_log = 0;
}

static void usage(const char* p){
    fprintf(stderr,
    "Usage: %s [options]\n"
    "  --card /dev/dri/cardN        (default: /dev/dri/card0)\n"
    "  --connector NAME             (e.g. HDMI-A-1; default: first CONNECTED)\n"
    "  --plane-id N                 (video plane; default: 76)\n"
    "  --blank-primary              (detach primary plane on commit)\n"
    "  --no-udev                    (disable hotplug listener)\n"
    "  --stay-blue                  (only do modeset & blue FB; no pipeline)\n"
    "  --blue-hold-ms N             (hold blue for N ms after commit)\n"
    "  --udp-port N                 (default: 5600)\n"
    "  --vid-pt N                   (default: 97 H265)\n"
    "  --aud-pt N                   (default: 98 Opus)\n"
    "  --latency-ms N               (default: 8)\n"
    "  --max-lateness NANOSECS      (default: 20000000)\n"
    "  --aud-dev STR                (default: plughw:CARD=rockchiphdmi0,DEV=0)\n"
    "  --no-audio                   (drop audio branch entirely)\n"
    "  --audio-optional             (auto-fallback to fakesink on failures; default)\n"
    "  --audio-required             (disable auto-fallback; keep real audio only)\n"
    "  --osd                        (enable OSD overlay plane with stats)\n"
    "  --osd-plane-id N             (force OSD plane id; default auto)\n"
    "  --osd-refresh-ms N           (default: 500)\n"
    "  --gst-log                    (set GST_DEBUG=3 if not set)\n"
    "  --verbose\n", p);
}

static int parse_cli(int argc, char** argv, AppCfg* c){
    cfg_defaults(c);
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"--card") && i+1<argc) strncpy(c->card_path, argv[++i], sizeof(c->card_path)-1);
        else if (!strcmp(argv[i],"--connector") && i+1<argc) strncpy(c->connector_name, argv[++i], sizeof(c->connector_name)-1);
        else if (!strcmp(argv[i],"--plane-id") && i+1<argc) c->plane_id = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--blank-primary")) c->blank_primary = 1;
        else if (!strcmp(argv[i],"--no-udev")) c->use_udev = 0;
        else if (!strcmp(argv[i],"--stay-blue")) c->stay_blue = 1;
        else if (!strcmp(argv[i],"--blue-hold-ms") && i+1<argc) c->blue_hold_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--udp-port") && i+1<argc) c->udp_port = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--vid-pt") && i+1<argc) c->vid_pt = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--aud-pt") && i+1<argc) c->aud_pt = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--latency-ms") && i+1<argc) c->latency_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--max-lateness") && i+1<argc) c->max_lateness_ns = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--aud-dev") && i+1<argc) strncpy(c->aud_dev, argv[++i], sizeof(c->aud_dev)-1);
        else if (!strcmp(argv[i],"--no-audio")) c->no_audio = 1;
        else if (!strcmp(argv[i],"--audio-optional")) c->audio_optional = 1;
        else if (!strcmp(argv[i],"--audio-required")) c->audio_optional = 0;
        else if (!strcmp(argv[i],"--osd")) c->osd_enable = 1;
        else if (!strcmp(argv[i],"--osd-plane-id") && i+1<argc) c->osd_plane_id = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--osd-refresh-ms") && i+1<argc) c->osd_refresh_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--gst-log")) c->gst_log = 1;
        else if (!strcmp(argv[i],"--verbose")) g_verbose = 1;
        else { usage(argv[0]); return -1; }
    }
    return 0;
}

// -------------- DRM helpers --------------
static const char* conn_type_str(uint32_t t){
    switch (t){
        case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
        case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
        case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
        case DRM_MODE_CONNECTOR_eDP: return "eDP";
        case DRM_MODE_CONNECTOR_VGA: return "VGA";
        default: return "UNKNOWN";
    }
}
static int vrefresh(const drmModeModeInfo* m){
    if (m->vrefresh) return m->vrefresh;
    if (m->htotal && m->vtotal){
        double hz = (double)m->clock * 1000.0 / (m->htotal * m->vtotal);
        return (int)(hz + 0.5);
    }
    return 0;
}
static int better_mode(const drmModeModeInfo* a, const drmModeModeInfo* b){
    int ahz=vrefresh(a), bhz=vrefresh(b);
    if (ahz!=bhz) return ahz>bhz;
    long long aa=(long long)a->hdisplay*a->vdisplay, bb=(long long)b->hdisplay*b->vdisplay;
    if (aa!=bb) return aa>bb;
    int ap=(a->type&DRM_MODE_TYPE_PREFERRED)?1:0, bp=(b->type&DRM_MODE_TYPE_PREFERRED)?1:0;
    if (ap!=bp) return ap>bp;
    return a->clock > b->clock;
}

struct DumbFB { uint32_t fb_id, handle, pitch; uint64_t size; void* map; int w,h; };
static int create_argb_fb(int fd, int w, int h, uint32_t argb_fill, struct DumbFB* out){
    struct drm_mode_create_dumb creq = (struct drm_mode_create_dumb){0};
    creq.width=w; creq.height=h; creq.bpp=32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) return -1;

    uint32_t handles[4] = { creq.handle, 0,0,0 };
    uint32_t pitches[4] = { creq.pitch,  0,0,0 };
    uint32_t offsets[4] = { 0,0,0,0 };
    uint32_t fb_id = 0;

    if (drmModeAddFB2(fd, w, h, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &fb_id, 0) != 0){
        struct drm_mode_destroy_dumb dreq = { .handle = creq.handle };
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }
    struct drm_mode_map_dumb mreq = { .handle = creq.handle };
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0){
        drmModeRmFB(fd, fb_id);
        struct drm_mode_destroy_dumb dreq = { .handle = creq.handle };
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }
    void* map = mmap(0, creq.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (map == MAP_FAILED){
        drmModeRmFB(fd, fb_id);
        struct drm_mode_destroy_dumb dreq = { .handle = creq.handle };
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }
    uint32_t *px = (uint32_t*)map;
    size_t count = (creq.size / 4);
    for (size_t i=0;i<count;i++) px[i] = argb_fill;

    out->fb_id=fb_id; out->handle=creq.handle; out->pitch=creq.pitch; out->size=creq.size; out->map=map; out->w=w; out->h=h;
    return 0;
}
static int create_blue_fb(int fd, int w, int h, struct DumbFB* out){
    struct drm_mode_create_dumb creq = (struct drm_mode_create_dumb){0};
    creq.width=w; creq.height=h; creq.bpp=32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) return -1;

    uint32_t handles[4] = { creq.handle, 0,0,0 };
    uint32_t pitches[4] = { creq.pitch,  0,0,0 };
    uint32_t offsets[4] = { 0,0,0,0 };
    uint32_t fb_id = 0;

    if (drmModeAddFB2(fd, w, h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &fb_id, 0) != 0){
        if (drmModeAddFB(fd, w, h, 24, 32, creq.pitch, creq.handle, &fb_id) != 0){
            struct drm_mode_destroy_dumb dreq = { .handle = creq.handle };
            ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
            return -1;
        }
    }
    struct drm_mode_map_dumb mreq = { .handle = creq.handle };
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0){
        drmModeRmFB(fd, fb_id);
        struct drm_mode_destroy_dumb dreq = { .handle = creq.handle };
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }
    void* map = mmap(0, creq.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (map == MAP_FAILED){
        drmModeRmFB(fd, fb_id);
        struct drm_mode_destroy_dumb dreq = { .handle = creq.handle };
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }
    uint32_t *px = (uint32_t*)map;
    uint32_t blue = 0x000000FFu;  // XRGB8888: blue
    size_t count = (creq.size / 4);
    for (size_t i=0;i<count;i++) px[i] = blue;

    out->fb_id=fb_id; out->handle=creq.handle; out->pitch=creq.pitch; out->size=creq.size; out->map=map; out->w=w; out->h=h;
    return 0;
}
static void destroy_dumb_fb(int fd, struct DumbFB* fb){
    if (!fb) return;
    if (fb->map && fb->map!=MAP_FAILED) munmap(fb->map, fb->size);
    if (fb->fb_id) drmModeRmFB(fd, fb->fb_id);
    if (fb->handle){ struct drm_mode_destroy_dumb dreq = { .handle = fb->handle }; ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq); }
    memset(fb,0,sizeof(*fb));
}

static int get_prop_id(int fd, uint32_t obj_id, uint32_t obj_type, const char* name, uint32_t* out){
    drmModeObjectProperties* props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) return -1;
    int found = 0;
    for (uint32_t i=0;i<props->count_props;i++){
        drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);
        if (!p) continue;
        if (!strcmp(p->name, name)){ *out = p->prop_id; found = 1; drmModeFreeProperty(p); break; }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return found?0:-1;
}
static int get_prop_id_and_range_ci(int fd, uint32_t obj_id, uint32_t obj_type,
                                 const char* name1, uint32_t* out_id,
                                 uint64_t* out_min, uint64_t* out_max,
                                 const char* alt_name2) {
    drmModeObjectProperties* props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) return -1;
    int found = 0;
    for (uint32_t i=0;i<props->count_props;i++){
        drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);
        if (!p) continue;
        if (!strcmp(p->name, name1) || (alt_name2 && !strcmp(p->name, alt_name2))){
            *out_id = p->prop_id; found = 1;
            if ((p->flags & DRM_MODE_PROP_RANGE) && out_min && out_max){
                *out_min = p->values[0]; *out_max = p->values[1];
            }
            drmModeFreeProperty(p); break;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return found?0:-1;
}
static void debug_list_props(int fd, uint32_t obj_id, uint32_t obj_type, const char* tag){
    drmModeObjectProperties* props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props){ LOGV("%s: no props", tag); return; }
    fprintf(stderr,"[DBG] %s props (%u):", tag, props->count_props);
    for (uint32_t i=0;i<props->count_props;i++){
        drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);
        if (!p) continue;
        fprintf(stderr," %s", p->name);
        drmModeFreeProperty(p);
    }
    fprintf(stderr,"\n");
    drmModeFreeObjectProperties(props);
}

static int find_primary_plane_for_crtc(int fd, drmModeRes* res, uint32_t crtc_id){
    int crtc_index = -1;
    for (int i=0;i<res->count_crtcs;i++) if ((uint32_t)res->crtcs[i]==crtc_id) { crtc_index = i; break; }
    if (crtc_index < 0) return -1;

    drmModePlaneRes *prs = drmModeGetPlaneResources(fd);
    if (!prs) return -1;

    int primary_id = -1;
    for (uint32_t i=0;i<prs->count_planes;i++){
        drmModePlane *p = drmModeGetPlane(fd, prs->planes[i]);
        if (!p) continue;
        if ((p->possible_crtcs & (1U << crtc_index)) == 0){ drmModeFreePlane(p); continue; }

        uint32_t type_prop=0;
        if (get_prop_id(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type_prop)==0){
            drmModeObjectProperties* props = drmModeObjectGetProperties(fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
            if (props){
                for (uint32_t k=0;k<props->count_props;k++){
                    if (props->props[k]==type_prop){
                        uint64_t val = props->prop_values[k];
                        if (val == DRM_PLANE_TYPE_PRIMARY) primary_id = (int)p->plane_id;
                        break;
                    }
                }
                drmModeFreeObjectProperties(props);
            }
        }
        drmModeFreePlane(p);
        if (primary_id > 0) break;
    }
    drmModeFreePlaneResources(prs);
    return primary_id;
}

// ------------ Atomic modeset (video plane) ------------
typedef struct {
    uint32_t connector_id;
    uint32_t crtc_id;
    int mode_w, mode_h, mode_hz;
} ModesetResult;

static int atomic_modeset_maxhz(int fd, const AppCfg* cfg, int osd_enabled, ModesetResult* out){
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0)
        LOGW("Failed to enable UNIVERSAL_PLANES");
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0)
        LOGW("Failed to enable ATOMIC");

    drmModeRes* res = drmModeGetResources(fd);
    if (!res){ LOGE("drmModeGetResources failed"); return -1; }

    drmModeConnector* conn = NULL;
    drmModeCrtc*      crtc = NULL;
    drmModeModeInfo   best = (drmModeModeInfo){0};

    for (int i=0;i<res->count_connectors;i++){
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;

        char cname[32];
        snprintf(cname,sizeof(cname),"%s-%u",
                 (c->connector_type==DRM_MODE_CONNECTOR_HDMIA?"HDMI-A":
                  c->connector_type==DRM_MODE_CONNECTOR_HDMIB?"HDMI-B":
                  c->connector_type==DRM_MODE_CONNECTOR_DisplayPort?"DP":
                  c->connector_type==DRM_MODE_CONNECTOR_eDP?"eDP":
                  c->connector_type==DRM_MODE_CONNECTOR_VGA?"VGA":"UNKNOWN"),
                 c->connector_type_id);

        if (c->connection==DRM_MODE_CONNECTED && c->count_modes>0 &&
            (!cfg->connector_name[0] || strcmp(cfg->connector_name,cname)==0))
        {
            best = c->modes[0];
            for (int m=1;m<c->count_modes;m++) if (better_mode(&c->modes[m], &best)) best = c->modes[m];

            drmModeEncoder *enc = NULL;
            if (c->encoder_id) enc = drmModeGetEncoder(fd, c->encoder_id);
            int crtc_id = -1;
            if (enc && enc->crtc_id){
                crtc = drmModeGetCrtc(fd, enc->crtc_id);
                if (crtc) crtc_id = crtc->crtc_id;
            }
            if (crtc_id < 0){
                for (int e=0;e<c->count_encoders && crtc_id<0;e++){
                    drmModeEncoder* e2 = drmModeGetEncoder(fd, c->encoders[e]);
                    if (!e2) continue;
                    for (int ci=0;ci<res->count_crtcs;ci++){
                        if (e2->possible_crtcs & (1<<ci)) {
                            crtc = drmModeGetCrtc(fd, res->crtcs[ci]);
                            if (crtc){ crtc_id = crtc->crtc_id; break; }
                        }
                    }
                    drmModeFreeEncoder(e2);
                }
            }
            if (enc) drmModeFreeEncoder(enc);

            if (crtc_id >= 0 && crtc){ conn = c; break; }
        }
        drmModeFreeConnector(c);
    }
    if (!conn){ LOGI("No CONNECTED connector with modes"); drmModeFreeResources(res); return -2; }

    char cname[32];
    snprintf(cname,sizeof(cname),"%s-%u", conn_type_str(conn->connector_type), conn->connector_type_id);
    int w=best.hdisplay, h=best.vdisplay, hz=vrefresh(&best);
    LOGI("Chosen: %s id=%u  %dx%d@%d  CRTC=%d  plane=%d",
        cname, conn->connector_id, w,h,hz, crtc->crtc_id, cfg->plane_id);

    struct DumbFB fb = {0};
    if (create_blue_fb(fd, w, h, &fb) != 0){
        LOGE("create_blue_fb failed: %s", strerror(errno));
        drmModeFreeConnector(conn); drmModeFreeCrtc(crtc); drmModeFreeResources(res); return -3;
    }
    uint32_t mode_blob=0;
    if (drmModeCreatePropertyBlob(fd, &best, sizeof(best), &mode_blob) != 0){
        LOGE("CreatePropertyBlob MODE_ID failed");
        destroy_dumb_fb(fd,&fb);
        drmModeFreeConnector(conn); drmModeFreeCrtc(crtc); drmModeFreeResources(res); return -4;
    }

    uint32_t crtc_active=0, crtc_mode_id=0;
    if (get_prop_id(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", &crtc_active) ||
        get_prop_id(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", &crtc_mode_id))
    {
        LOGE("CRTC props missing"); debug_list_props(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC, "CRTC");
        drmModeDestroyPropertyBlob(fd, mode_blob); destroy_dumb_fb(fd,&fb);
        drmModeFreeConnector(conn); drmModeFreeCrtc(crtc); drmModeFreeResources(res); return -5;
    }
    uint32_t conn_crtc_id=0;
    if (get_prop_id(fd, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &conn_crtc_id)){
        LOGE("CONNECTOR props missing"); debug_list_props(fd, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CONN");
        drmModeDestroyPropertyBlob(fd, mode_blob); destroy_dumb_fb(fd,&fb);
        drmModeFreeConnector(conn); drmModeFreeCrtc(crtc); drmModeFreeResources(res); return -6;
    }

    uint32_t plane_fb_id=0, plane_crtc_id=0, plane_src_x=0, plane_src_y=0, plane_src_w=0, plane_src_h=0;
    uint32_t plane_crtc_x=0, plane_crtc_y=0, plane_crtc_w=0, plane_crtc_h=0;
    if (get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID",   &plane_fb_id)   ||
        get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &plane_crtc_id) ||
        get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X",   &plane_src_x)   ||
        get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y",   &plane_src_y)   ||
        get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W",   &plane_src_w)   ||
        get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H",   &plane_src_h)   ||
        get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X",  &plane_crtc_x)  ||
        get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y",  &plane_crtc_y)  ||
        get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W",  &plane_crtc_w)  ||
        get_prop_id(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H",  &plane_crtc_h))
    {
        LOGE("Plane props missing for id=%d", cfg->plane_id);
        debug_list_props(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "PLANE");
        drmModeDestroyPropertyBlob(fd, mode_blob); destroy_dumb_fb(fd,&fb);
        drmModeFreeConnector(conn); drmModeFreeCrtc(crtc); drmModeFreeResources(res); return -7;
    }

    uint32_t plane_zpos_id=0; uint64_t zmin=0,zmax=0; int have_zpos=0;
    if (get_prop_id_and_range_ci(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "ZPOS", &plane_zpos_id, &zmin, &zmax, "zpos")==0)
        have_zpos = 1;

    // Optional: blank PRIMARY
    int primary_plane_id = -1;
    uint32_t prim_fb_id=0, prim_crtc_id=0;
    if (cfg->blank_primary) {
        primary_plane_id = find_primary_plane_for_crtc(fd, res, crtc->crtc_id);
        if (primary_plane_id > 0){
            if (get_prop_id(fd, (uint32_t)primary_plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &prim_fb_id) ||
                get_prop_id(fd, (uint32_t)primary_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &prim_crtc_id))
            {
                LOGW("PRIMARY plane props not found; cannot blank");
                primary_plane_id = -1;
            }
        } else {
            LOGW("Could not find PRIMARY plane for this CRTC");
        }
    }

    // Atomic req (video plane blue)
    drmModeAtomicReq* req = drmModeAtomicAlloc();
    if (!req){ LOGE("drmModeAtomicAlloc failed"); drmModeDestroyPropertyBlob(fd,mode_blob); destroy_dumb_fb(fd,&fb); drmModeFreeConnector(conn); drmModeFreeCrtc(crtc); drmModeFreeResources(res); return -8; }

    drmModeAtomicAddProperty(req, crtc->crtc_id, crtc_active, 1);
    drmModeAtomicAddProperty(req, crtc->crtc_id, crtc_mode_id, mode_blob);
    drmModeAtomicAddProperty(req, conn->connector_id, conn_crtc_id, crtc->crtc_id);

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

    if (have_zpos){
        uint64_t v_z = zmax; // top by default
        if (osd_enabled && zmax > zmin) v_z = zmax - 1; // leave top slot for OSD
        drmModeAtomicAddProperty(req, cfg->plane_id, plane_zpos_id, v_z);
    }

    if (primary_plane_id > 0){
        drmModeAtomicAddProperty(req, (uint32_t)primary_plane_id, prim_fb_id, 0);
        drmModeAtomicAddProperty(req, (uint32_t)primary_plane_id, prim_crtc_id, 0);
    }

    int flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    int ret = drmModeAtomicCommit(fd, req, flags, NULL);
    if (ret != 0){
        LOGE("drmModeAtomicCommit failed: %s", strerror(errno));
        drmModeAtomicFree(req); drmModeDestroyPropertyBlob(fd, mode_blob); destroy_dumb_fb(fd,&fb);
        drmModeFreeConnector(conn); drmModeFreeCrtc(crtc); drmModeFreeResources(res); return -9;
    }

    LOGI("Atomic COMMIT: %dx%d@%d on %s via plane %d — BLUE", w,h,hz,cname,cfg->plane_id);

    if (cfg->blue_hold_ms > 0) usleep(cfg->blue_hold_ms * 1000);

    drmModeAtomicFree(req);
    drmModeDestroyPropertyBlob(fd, mode_blob);
    destroy_dumb_fb(fd,&fb);

    if (out){
        out->connector_id = conn->connector_id;
        out->crtc_id = crtc->crtc_id;
        out->mode_w = w; out->mode_h = h; out->mode_hz = hz;
    }

    drmModeFreeConnector(conn);
    drmModeFreeCrtc(crtc);
    drmModeFreeResources(res);
    return 0;
}

static int is_any_connected(int fd, const AppCfg* cfg){
    drmModeRes* res = drmModeGetResources(fd);
    if (!res) return 0;
    int connected = 0;
    for (int i=0;i<res->count_connectors;i++){
        drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        char cname[32];
        snprintf(cname,sizeof(cname),"%s-%u",
                 (c->connector_type==DRM_MODE_CONNECTOR_HDMIA?"HDMI-A":
                  c->connector_type==DRM_MODE_CONNECTOR_HDMIB?"HDMI-B":
                  c->connector_type==DRM_MODE_CONNECTOR_DisplayPort?"DP":
                  c->connector_type==DRM_MODE_CONNECTOR_eDP?"eDP":
                  c->connector_type==DRM_MODE_CONNECTOR_VGA?"VGA":"UNKNOWN"),
                 c->connector_type_id);
        if (c->connection==DRM_MODE_CONNECTED && (!cfg->connector_name[0] || strcmp(cfg->connector_name,cname)==0)){
            connected = 1;
            drmModeFreeConnector(c);
            break;
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    return connected;
}

// ------------- GStreamer runner -------------
typedef enum { GST_STOPPED=0, GST_RUNNING=1, GST_STOPPING=2 } GstState;
typedef struct {
    pid_t pid;     // child pid
    pid_t pgid;    // child process group id
    GstState state;
} GstProc;

static void build_gst_cmd(const AppCfg* cfg, int audio_disabled, char* out, size_t outsz){
    const char* audio_branch;
    char audio_real[512];

    if (cfg->no_audio){
        audio_branch = "";
    } else if (audio_disabled){
        audio_branch = "t. ! queue leaky=downstream max-size-time=0 max-size-bytes=0 ! fakesink sync=false ";
    } else {
        snprintf(audio_real, sizeof(audio_real),
            "t. ! queue leaky=downstream max-size-time=0 max-size-bytes=0 ! "
            "application/x-rtp,payload=%d,clock-rate=48000,encoding-name=OPUS ! "
            "rtpjitterbuffer latency=%d drop-on-latency=true do-lost=true ! "
            "rtpopusdepay ! opusdec ! audioconvert ! audioresample ! "
            "audio/x-raw,format=S16LE,rate=48000,channels=2 ! "
            "queue leaky=downstream ! "
            "alsasink device=%s sync=false ",
            cfg->aud_pt, cfg->latency_ms, cfg->aud_dev
        );
        audio_branch = audio_real;
    }

    snprintf(out, outsz,
      "gst-launch-1.0 -v "
      "udpsrc port=%d buffer-size=262144 ! tee name=t "
        "t. ! queue leaky=downstream max-size-buffers=96 max-size-time=0 max-size-bytes=0 ! "
            "application/x-rtp,payload=%d,clock-rate=90000,encoding-name=H265 ! "
            "rtpjitterbuffer latency=%d drop-on-latency=true do-lost=true post-drop-messages=true ! "
            "rtph265depay ! h265parse config-interval=-1 disable-passthrough=true ! "
            "video/x-h265,stream-format=byte-stream,alignment=au ! "
            "queue leaky=downstream max-size-buffers=8 max-size-time=0 max-size-bytes=0 ! "
            "mppvideodec ! queue leaky=downstream max-size-buffers=8 ! "
            "kmssink plane-id=%d sync=%s qos=%s max-lateness=%d "
        "%s",
      cfg->udp_port,
      cfg->vid_pt, cfg->latency_ms,
      cfg->plane_id,
      cfg->kmssink_sync ? "true":"false",
      cfg->kmssink_qos  ? "true":"false",
      cfg->max_lateness_ns,
      audio_branch
    );
}

static int gst_start(const AppCfg* cfg, int audio_disabled, GstProc* gp){
    if (gp->state != GST_STOPPED && gp->pid > 0) {
        LOGW("gst_start: refused (state=%d pid=%d)", gp->state, gp->pid);
        return -1;
    }
    char cmd[2500]; build_gst_cmd(cfg, audio_disabled, cmd, sizeof(cmd));
    LOGI("Starting pipeline: %s", cmd);

    pid_t pid = fork();
    if (pid < 0){ LOGE("fork failed: %s", strerror(errno)); return -1; }
    if (pid == 0){
        if (cfg->gst_log && getenv("GST_DEBUG")==NULL) setenv("GST_DEBUG","3",1);
        prctl(PR_SET_PDEATHSIG, SIGHUP);
        setpgid(0, 0); // new process group for the whole pipeline
        execl("/bin/sh","sh","-c", cmd, (char*)NULL);
        _exit(127);
    }
    gp->pid = pid;
    gp->pgid = pid; // child's pgid equals its pid
    gp->state = GST_RUNNING;
    return 0;
}

static void gst_stop(GstProc* gp, int wait_ms_total){
    if (gp->pid <= 0) { gp->state = GST_STOPPED; gp->pgid = 0; return; }
    if (gp->state == GST_STOPPING) return;
    gp->state = GST_STOPPING;
    LOGI("Stopping pipeline pid=%d pgid=%d", gp->pid, gp->pgid);
    if (gp->pgid > 0) killpg(gp->pgid, SIGINT); else kill(gp->pid, SIGINT);
    int waited = 0;
    while (waited < wait_ms_total){
        int status; pid_t r = waitpid(gp->pid, &status, WNOHANG);
        if (r == gp->pid){ gp->pid = 0; gp->pgid = 0; gp->state = GST_STOPPED; return; }
        usleep(50*1000); waited += 50;
    }
    LOGW("Pipeline didn’t exit in time, SIGKILL group");
    if (gp->pgid > 0) killpg(gp->pgid, SIGKILL); else kill(gp->pid, SIGKILL);
    int status; (void)waitpid(gp->pid, &status, 0);
    gp->pid = 0; gp->pgid = 0; gp->state = GST_STOPPED;
}

static void gst_poll_child(GstProc* gp){
    if (gp->pid <= 0) return;
    int status = 0;
    pid_t r = waitpid(gp->pid, &status, WNOHANG);
    if (r == gp->pid){
        LOGI("Pipeline exited (status=0x%x)", status);
        gp->pid = 0; gp->pgid = 0; gp->state = GST_STOPPED;
    }
}

// ------------- OSD module -------------
typedef struct {
    int enabled;
    int active;
    uint32_t requested_plane_id;  // forced id if nonzero
    uint32_t plane_id;
    struct DumbFB fb;
    int w, h;            // OSD buffer size
    int scale;           // font scale (1 or 2)
    int refresh_ms;

    // plane props
    uint32_t p_fb_id, p_crtc_id, p_crtc_x, p_crtc_y, p_crtc_w, p_crtc_h;
    uint32_t p_src_x, p_src_y, p_src_w, p_src_h;
    uint32_t p_zpos; int have_zpos; uint64_t zmin, zmax;
    // blending
    uint32_t p_alpha; int have_alpha; uint64_t alpha_min, alpha_max;
    uint32_t p_blend; int have_blend;
} OSD;

static void osd_clear(OSD* o, uint32_t argb){
    if (!o->fb.map) return;
    uint32_t *px = (uint32_t*)o->fb.map;
    size_t count = (o->fb.size / 4);
    for (size_t i=0;i<count;i++) px[i] = argb;
}

// Tiny 5x7 font (subset)
static const uint8_t* font5x7(char c){
    static const uint8_t SPC[7]={0,0,0,0,0,0,0};
    static const uint8_t D0[7]={0x1E,0x21,0x23,0x25,0x29,0x31,0x1E};
    static const uint8_t D1[7]={0x08,0x18,0x08,0x08,0x08,0x08,0x1C};
    static const uint8_t D2[7]={0x1E,0x21,0x01,0x06,0x18,0x20,0x3F};
    static const uint8_t D3[7]={0x1E,0x21,0x01,0x0E,0x01,0x21,0x1E};
    static const uint8_t D4[7]={0x02,0x06,0x0A,0x12,0x3F,0x02,0x02};
    static const uint8_t D5[7]={0x3F,0x20,0x3E,0x01,0x01,0x21,0x1E};
    static const uint8_t D6[7]={0x0E,0x10,0x20,0x3E,0x21,0x21,0x1E};
    static const uint8_t D7[7]={0x3F,0x01,0x02,0x04,0x08,0x08,0x08};
    static const uint8_t D8[7]={0x1E,0x21,0x21,0x1E,0x21,0x21,0x1E};
    static const uint8_t D9[7]={0x1E,0x21,0x21,0x1F,0x01,0x02,0x1C};
    static const uint8_t A[7]={0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t B[7]={0x3E,0x11,0x11,0x1E,0x11,0x11,0x3E};
    static const uint8_t C[7]={0x0E,0x11,0x20,0x20,0x20,0x11,0x0E};
    static const uint8_t D[7]={0x3C,0x12,0x11,0x11,0x11,0x12,0x3C};
    static const uint8_t E[7]={0x3F,0x20,0x20,0x3E,0x20,0x20,0x3F};
    static const uint8_t F[7]={0x3F,0x20,0x20,0x3E,0x20,0x20,0x20};
    static const uint8_t G[7]={0x0E,0x11,0x20,0x27,0x21,0x11,0x0F};
    static const uint8_t H[7]={0x11,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t I[7]={0x1F,0x04,0x04,0x04,0x04,0x04,0x1F};
    static const uint8_t J[7]={0x1F,0x02,0x02,0x02,0x02,0x12,0x0C};
    static const uint8_t K[7]={0x11,0x12,0x14,0x18,0x14,0x12,0x11};
    static const uint8_t L[7]={0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
    static const uint8_t M[7]={0x11,0x1B,0x15,0x15,0x11,0x11,0x11};
    static const uint8_t N[7]={0x11,0x19,0x15,0x13,0x11,0x11,0x11};
    static const uint8_t O[7]={0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t P[7]={0x3E,0x11,0x11,0x1E,0x10,0x10,0x10};
    static const uint8_t Q[7]={0x0E,0x11,0x11,0x11,0x15,0x12,0x0D};
    static const uint8_t R[7]={0x3E,0x11,0x11,0x1E,0x14,0x12,0x11};
    static const uint8_t S_[7]={0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E};
    static const uint8_t T[7]={0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
    static const uint8_t U[7]={0x11,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t V[7]={0x11,0x11,0x11,0x11,0x0A,0x0A,0x04};
    static const uint8_t W[7]={0x11,0x11,0x11,0x15,0x15,0x1B,0x11};
    static const uint8_t X[7]={0x11,0x11,0x0A,0x04,0x0A,0x11,0x11};
    static const uint8_t Y[7]={0x11,0x11,0x0A,0x04,0x04,0x04,0x04};
    static const uint8_t Z[7]={0x1F,0x01,0x02,0x04,0x08,0x10,0x1F};
    static const uint8_t COL[7]={0x00,0x04,0x00,0x00,0x00,0x04,0x00};
    static const uint8_t SLH[7]={0x01,0x02,0x04,0x04,0x08,0x10,0x10};
    static const uint8_t DASH[7]={0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
    static const uint8_t UND[7]={0x00,0x00,0x00,0x00,0x00,0x00,0x1F};
    static const uint8_t DOT[7]={0x00,0x00,0x00,0x00,0x00,0x0C,0x0C};
    static const uint8_t AT [7]={0x0E,0x11,0x17,0x15,0x17,0x10,0x0E};
    static const uint8_t LP [7]={0x06,0x08,0x10,0x10,0x10,0x08,0x06};
    static const uint8_t RP [7]={0x18,0x04,0x02,0x02,0x02,0x04,0x18};

    if (c>='0' && c<='9'){ static const uint8_t* D[]={D0,D1,D2,D3,D4,D5,D6,D7,D8,D9}; return D[c-'0']; }
    if (c>='A' && c<='Z'){ static const uint8_t* Ltrs[]={A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S_,T,U,V,W,X,Y,Z}; return Ltrs[c-'A']; }
    switch (c){
        case ' ': return SPC; case ':': return COL; case '/': return SLH; case '-': return DASH;
        case '_': return UND; case '.': return DOT; case '@': return AT; case '(': return LP; case ')': return RP;
        default: return SPC;
    }
}
static void osd_draw_char(OSD* o, int x, int y, char c, uint32_t argb, int scale){
    const uint8_t* g = font5x7(c>='a'&&c<='z'? (c-'a'+'A'):c);
    int sx = scale, sy = scale;
    for (int row=0; row<7; row++){
        uint8_t bits = g[row];
        for (int col=0; col<5; col++){
            // FIX: read MSB-left (bit 4 is leftmost), not LSB-left
            int bit = (bits >> (4 - col)) & 1;
            if (bit){
                int px = x + col*sx;
                int py = y + row*sy;
                for (int dy=0; dy<sy; dy++){
                    uint32_t* line = (uint32_t*)((uint8_t*)o->fb.map + (py+dy)*o->fb.pitch);
                    for (int dx=0; dx<sx; dx++){
                        if (px+dx < o->w && py+dy < o->h) line[px+dx] = argb;
                    }
                }
            }
        }
    }
}

static void osd_draw_text(OSD* o, int x, int y, const char* s, uint32_t argb, int scale){
    int cursor = 0;
    for (const char* p=s; *p; ++p){
        if (*p=='\n'){ y += 8*scale; cursor = 0; continue; }
        osd_draw_char(o, x + cursor*(6*scale), y, *p, argb, scale);
        cursor++;
    }
}

// ---- Plane capability test via TEST_ONLY atomic commit (LINEAR ARGB) ----

typedef struct {
    uint32_t p_fb_id, p_crtc_id, p_crtc_x, p_crtc_y, p_crtc_w, p_crtc_h;
    uint32_t p_src_x, p_src_y, p_src_w, p_src_h;
    uint32_t p_zpos; int have_zpos; uint64_t zmin, zmax;
    uint32_t p_alpha; int have_alpha; uint64_t amin, amax;
    uint32_t p_blend; int have_blend;
} PlaneProps;

static int plane_get_basic_props(int fd, uint32_t plane_id, PlaneProps* pp){
    memset(pp,0,sizeof(*pp));
    if (get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID",   &pp->p_fb_id)   ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &pp->p_crtc_id) ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X",  &pp->p_crtc_x)  ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y",  &pp->p_crtc_y)  ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W",  &pp->p_crtc_w)  ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H",  &pp->p_crtc_h)  ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X",   &pp->p_src_x)   ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y",   &pp->p_src_y)   ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W",   &pp->p_src_w)   ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H",   &pp->p_src_h))
        return -1;

    pp->have_zpos = (get_prop_id_and_range_ci(fd, plane_id, DRM_MODE_OBJECT_PLANE, "ZPOS", &pp->p_zpos, &pp->zmin, &pp->zmax, "zpos")==0);
    if (get_prop_id_and_range_ci(fd, plane_id, DRM_MODE_OBJECT_PLANE, "alpha", &pp->p_alpha, &pp->amin, &pp->amax, "alpha")==0) pp->have_alpha=1;
    if (get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "pixel blend mode", &pp->p_blend)==0) pp->have_blend=1;
    return 0;
}

// Returns 1 if plane accepts a small LINEAR ARGB FB on the given CRTC (TEST_ONLY), else 0.
static int plane_accepts_linear_argb(int fd, uint32_t plane_id, uint32_t crtc_id){
    PlaneProps pp;
    if (plane_get_basic_props(fd, plane_id, &pp)!=0) return 0;

    // Create tiny ARGB FB
    struct DumbFB fb={0};
    if (create_argb_fb(fd, 64, 32, 0x80FFFFFFu, &fb)!=0) return 0;

    // Build TEST_ONLY atomic
    drmModeAtomicReq* req = drmModeAtomicAlloc();
    if (!req){ destroy_dumb_fb(fd,&fb); return 0; }

    drmModeAtomicAddProperty(req, plane_id, pp.p_fb_id, fb.fb_id);
    drmModeAtomicAddProperty(req, plane_id, pp.p_crtc_id, crtc_id);
    drmModeAtomicAddProperty(req, plane_id, pp.p_crtc_x, 0);
    drmModeAtomicAddProperty(req, plane_id, pp.p_crtc_y, 0);
    drmModeAtomicAddProperty(req, plane_id, pp.p_crtc_w, fb.w);
    drmModeAtomicAddProperty(req, plane_id, pp.p_crtc_h, fb.h);
    drmModeAtomicAddProperty(req, plane_id, pp.p_src_x,  0);
    drmModeAtomicAddProperty(req, plane_id, pp.p_src_y,  0);
    drmModeAtomicAddProperty(req, plane_id, pp.p_src_w,  (uint64_t)fb.w<<16);
    drmModeAtomicAddProperty(req, plane_id, pp.p_src_h,  (uint64_t)fb.h<<16);

    int flags = DRM_MODE_ATOMIC_TEST_ONLY;
    int ok = (drmModeAtomicCommit(fd, req, flags, NULL) == 0);

    drmModeAtomicFree(req);
    destroy_dumb_fb(fd,&fb);
    return ok;
}

static int get_plane_type(int fd, uint32_t plane_id, int* out_type){
    uint32_t type_prop=0;
    if (get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "type", &type_prop)!=0) return -1;
    drmModeObjectProperties* pr = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!pr) return -1;
    int found = -1;
    for (uint32_t k=0;k<pr->count_props;k++){
        if (pr->props[k]==type_prop){
            *out_type = (int)pr->prop_values[k];
            found = 0; break;
        }
    }
    drmModeFreeObjectProperties(pr);
    return found;
}

// Validate a specific plane id for OSD by TEST_ONLY commit; require LINEAR ARGB
static int osd_validate_requested_plane(int fd, uint32_t crtc_id, uint32_t plane_id){
    if (!plane_accepts_linear_argb(fd, plane_id, crtc_id)) return -1;
    int type=0; if (get_plane_type(fd, plane_id, &type)==0){
        if (type==DRM_PLANE_TYPE_CURSOR) return -1;
    }
    return 0;
}

static int osd_pick_plane(int fd, uint32_t crtc_id, int avoid_plane_id, uint32_t requested, uint32_t* out_plane, uint64_t* out_zmax){
    // If the user forced a plane id, validate and use it
    if (requested){
        if (osd_validate_requested_plane(fd, crtc_id, requested)==0){
            // Query zpos range
            uint32_t pz=0; uint64_t zmin=0,zmax=0;
            int have = (get_prop_id_and_range_ci(fd, requested, DRM_MODE_OBJECT_PLANE, "ZPOS", &pz, &zmin, &zmax, "zpos")==0);
            *out_plane = requested; *out_zmax = have? zmax : 0;
            return 0;
        } else {
            LOGW("OSD: requested plane %u is not LINEAR ARGB-capable; falling back to auto-pick.", requested);
        }
    }

    drmModeRes* res = drmModeGetResources(fd);
    if (!res) return -1;

    int crtc_index = -1;
    for (int i=0;i<res->count_crtcs;i++) if ((uint32_t)res->crtcs[i]==crtc_id){ crtc_index=i; break; }
    if (crtc_index<0){ drmModeFreeResources(res); return -1; }

    drmModePlaneRes *prs = drmModeGetPlaneResources(fd);
    if (!prs){ drmModeFreeResources(res); return -1; }

    uint32_t best_plane = 0; int best_score = -1000000;
    uint64_t best_zmax = 0;

    for (uint32_t i=0;i<prs->count_planes;i++){
        drmModePlane *p = drmModeGetPlane(fd, prs->planes[i]);
        if (!p) continue;
        if ((int)p->plane_id == avoid_plane_id){ drmModeFreePlane(p); continue; }
        if ((p->possible_crtcs & (1U<<crtc_index)) == 0){ drmModeFreePlane(p); continue; }

        int type=0;
        if (get_plane_type(fd, p->plane_id, &type)!=0){ drmModeFreePlane(p); continue; }
        if (type == DRM_PLANE_TYPE_CURSOR){ drmModeFreePlane(p); continue; }

        // Must accept LINEAR ARGB on TEST_ONLY
        if (!plane_accepts_linear_argb(fd, p->plane_id, crtc_id)){ drmModeFreePlane(p); continue; }

        // zpos preference
        uint32_t pz=0; uint64_t zmin=0,zmax=0; int have_z = (get_prop_id_and_range_ci(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "ZPOS", &pz, &zmin, &zmax, "zpos")==0);

        int score = 0;
        if (have_z) score += 100 + (int)zmax;  // prefer planes with zpos, higher is better
        // small tie-breaker: prefer overlay slightly (but primary is fine)
        if (type == DRM_PLANE_TYPE_OVERLAY) score += 1;

        if (score > best_score){
            best_score = score;
            best_plane = p->plane_id;
            best_zmax = have_z ? zmax : 0;
        }

        drmModeFreePlane(p);
    }

    drmModeFreePlaneResources(prs);
    drmModeFreeResources(res);

    if (!best_plane) return -1;
    *out_plane = best_plane;
    *out_zmax = best_zmax;
    return 0;
}

static int osd_query_plane_props(int fd, uint32_t plane_id, OSD* o){
    if (get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID",   &o->p_fb_id)   ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &o->p_crtc_id) ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X",  &o->p_crtc_x)  ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y",  &o->p_crtc_y)  ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W",  &o->p_crtc_w)  ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H",  &o->p_crtc_h)  ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X",   &o->p_src_x)   ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y",   &o->p_src_y)   ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W",   &o->p_src_w)   ||
        get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H",   &o->p_src_h))
    {
        LOGE("OSD plane props missing (id=%u)", plane_id);
        debug_list_props(fd, plane_id, DRM_MODE_OBJECT_PLANE, "OSD_PLANE");
        return -1;
    }
    o->have_zpos = (get_prop_id_and_range_ci(fd, plane_id, DRM_MODE_OBJECT_PLANE, "ZPOS", &o->p_zpos, &o->zmin, &o->zmax, "zpos")==0);

    uint32_t p_alpha=0, p_blend=0;
    uint64_t amin=0, amax=0;
    if (get_prop_id_and_range_ci(fd, plane_id, DRM_MODE_OBJECT_PLANE, "alpha", &p_alpha, &amin, &amax, "alpha")==0){
        o->p_alpha = p_alpha; o->alpha_min = amin; o->alpha_max = amax; o->have_alpha = 1;
    } else {
        o->have_alpha = 0;
    }
    if (get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "pixel blend mode", &p_blend)==0){
        o->p_blend = p_blend; o->have_blend = 1;
    } else {
        o->have_blend = 0;
    }
    return 0;
}

static int osd_commit_enable(int fd, uint32_t crtc_id, OSD* o){
    drmModeAtomicReq* req = drmModeAtomicAlloc();
    if (!req) return -1;
    drmModeAtomicAddProperty(req, o->plane_id, o->p_fb_id, o->fb.fb_id);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_crtc_id, crtc_id);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_crtc_x, 0);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_crtc_y, 0);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_crtc_w, o->w);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_crtc_h, o->h);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_src_x,  0);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_src_y,  0);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_src_w,  (uint64_t)o->w << 16);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_src_h,  (uint64_t)o->h << 16);
    if (o->have_zpos) drmModeAtomicAddProperty(req, o->plane_id, o->p_zpos, (uint64_t)o->zmax);
    if (o->have_alpha){
        uint64_t aval = o->alpha_max ? o->alpha_max : 65535;
        drmModeAtomicAddProperty(req, o->plane_id, o->p_alpha, aval);
    }
    if (o->have_blend){
        drmModePropertyRes* prop = drmModeGetProperty(fd, o->p_blend);
        if (prop){
            uint64_t premul_val = 0; int found = 0;
            for (int i=0; i<prop->count_enums; i++){
                if (strcmp(prop->enums[i].name, "Pre-multiplied")==0){ premul_val = prop->enums[i].value; found = 1; break; }
            }
            drmModeFreeProperty(prop);
            if (found) drmModeAtomicAddProperty(req, o->plane_id, o->p_blend, premul_val);
        }
    }
    int ret = drmModeAtomicCommit(fd, req, 0, NULL);
    drmModeAtomicFree(req);
    return ret;
}
static int osd_commit_disable(int fd, OSD* o){
    if (!o->active) return 0;
    drmModeAtomicReq* req = drmModeAtomicAlloc();
    if (!req) return -1;
    drmModeAtomicAddProperty(req, o->plane_id, o->p_fb_id, 0);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_crtc_id, 0);
    int ret = drmModeAtomicCommit(fd, req, 0, NULL);
    drmModeAtomicFree(req);
    return ret;
}
static int osd_commit_touch(int fd, uint32_t crtc_id, OSD* o){
    drmModeAtomicReq* req = drmModeAtomicAlloc();
    if (!req) return -1;
    drmModeAtomicAddProperty(req, o->plane_id, o->p_fb_id, o->fb.fb_id);
    drmModeAtomicAddProperty(req, o->plane_id, o->p_crtc_id, crtc_id);
    int ret = drmModeAtomicCommit(fd, req, 0, NULL);
    drmModeAtomicFree(req);
    return ret;
}
static void osd_destroy(int fd, OSD* o){
    if (o->active) osd_commit_disable(fd, o);
    destroy_dumb_fb(fd, &o->fb);
    memset(o,0,sizeof(*o));
}

static int osd_setup(int fd, const AppCfg* cfg, const ModesetResult* ms, int video_plane_id, OSD* o){
    memset(o,0,sizeof(*o));
    o->enabled = cfg->osd_enable;
    o->requested_plane_id = (uint32_t)cfg->osd_plane_id;
    o->refresh_ms = cfg->osd_refresh_ms;

    if (!o->enabled) return 0;

    uint32_t chosen=0; uint64_t zmax=0;
    if (osd_pick_plane(fd, ms->crtc_id, video_plane_id, o->requested_plane_id, &chosen, &zmax) != 0){
        LOGW("OSD: failed to find suitable plane. Disabling OSD.");
        o->enabled = 0; return -1;
    }
    o->plane_id = chosen;
    LOGI("OSD: using overlay plane id=%u", o->plane_id);
    if (osd_query_plane_props(fd, o->plane_id, o) != 0){
        LOGW("OSD: plane props missing. Disabling OSD.");
        o->enabled = 0; return -1;
    }
    // ensure we know top zmax
    if (o->have_zpos && zmax>0) o->zmax = zmax;

    // Size + scale
    o->scale = (ms->mode_w >= 1280) ? 2 : 1;
    o->w = 480 * o->scale;
    o->h = 120 * o->scale;

    // ARGB premult translucent background
    if (create_argb_fb(fd, o->w, o->h, 0x80000000u, &o->fb) != 0){
        LOGW("OSD: create fb failed. Disabling OSD.");
        o->enabled = 0; return -1;
    }

    // First frame
    osd_clear(o, 0x80000000u);
    char line[256];
    snprintf(line, sizeof(line), "PIXELPILOT MINI RK\n%dx%d@%d  PLANE=%d", ms->mode_w, ms->mode_h, ms->mode_hz, video_plane_id);
    osd_draw_text(o, 8*o->scale, 8*o->scale, line, 0xFFFFFFFFu, o->scale);

    if (osd_commit_enable(fd, ms->crtc_id, o) != 0){
        LOGW("OSD: commit enable failed. Disabling OSD.");
        destroy_dumb_fb(fd, &o->fb);
        o->enabled = 0; return -1;
    }
    o->active = 1;
    LOGI("OSD enabled: plane=%u size=%dx%d zpos=%s alpha=%s blend=%s",
         o->plane_id, o->w, o->h,
         o->have_zpos ? "set" : "n/a",
         o->have_alpha ? "set" : "n/a",
         o->have_blend ? "premult" : "n/a");
    return 0;
}

static void osd_update_stats(int fd, const AppCfg* cfg, const ModesetResult* ms,
                             const GstProc* gp, int audio_disabled, int restart_count, OSD* o)
{
    if (!o->enabled || !o->active) return;
    osd_clear(o, 0x80000000u);

    char s1[64], s2[96], s3[96], s4[96];
    snprintf(s1,sizeof(s1), "MODE %dX%d@%d  PLANE %d", ms->mode_w, ms->mode_h, ms->mode_hz, cfg->plane_id);
    snprintf(s2,sizeof(s2), "UDP %d  LATENCY %dMS", cfg->udp_port, cfg->latency_ms);
    const char* am = cfg->no_audio ? "NONE" : (audio_disabled ? "FAKE" : "REAL");
    snprintf(s3,sizeof(s3), "AUDIO %s  PIPE %s", am, (gp->state==GST_RUNNING?"RUNNING":gp->state==GST_STOPPING?"STOPPING":"STOPPED"));

    time_t t = time(NULL); struct tm tm; localtime_r(&t,&tm);
    char tsbuf[32]; strftime(tsbuf,sizeof(tsbuf),"%H:%M:%S", &tm);
    snprintf(s4,sizeof(s4), "RESTARTS %d  TIME %s", restart_count, tsbuf);

    osd_draw_text(o, 8*o->scale, 8*o->scale,                  s1, 0xFFFFFFFFu, o->scale);
    osd_draw_text(o, 8*o->scale, 8*o->scale+8*o->scale,       s2, 0xFFFFFFFFu, o->scale);
    osd_draw_text(o, 8*o->scale, 8*o->scale+16*o->scale,      s3, 0xFFFFFFFFu, o->scale);
    osd_draw_text(o, 8*o->scale, 8*o->scale+24*o->scale,      s4, 0xFFFFFFFFu, o->scale);

    osd_commit_touch(fd, ms->crtc_id, o);
}

// ------------- udev -------------
typedef struct {
    struct udev* udev;
    struct udev_monitor* mon;
    int fd;
} UMon;

static int udev_open(UMon* m){
    memset(m,0,sizeof(*m));
    m->udev = udev_new(); if (!m->udev){ LOGE("udev_new failed"); return -1; }
    m->mon = udev_monitor_new_from_netlink(m->udev, "udev"); if (!m->mon){ LOGE("udev_monitor_new failed"); udev_unref(m->udev); return -1; }
    udev_monitor_filter_add_match_subsystem_devtype(m->mon, "drm", NULL);
    udev_monitor_enable_receiving(m->mon);
    m->fd = udev_monitor_get_fd(m->mon);
    LOGI("udev monitor ready (fd=%d)", m->fd);
    return 0;
}
static void udev_close(UMon* m){
    if (m->mon) udev_monitor_unref(m->mon);
    if (m->udev) udev_unref(m->udev);
    memset(m,0,sizeof(*m));
}
static int udev_did_hotplug(UMon* m){
    struct udev_device* dev = udev_monitor_receive_device(m->mon);
    if (!dev) return 0;
    const char* subsys = udev_device_get_subsystem(dev);
    const char* act = udev_device_get_action(dev);
    const char* sysname = udev_device_get_sysname(dev);
    const char* hotplug = udev_device_get_property_value(dev, "HOTPLUG");
    LOGV("udev: subsys=%s action=%s sys=%s hotplug=%s", subsys?subsys:"?", act?act:"?", sysname?sysname:"?", hotplug?hotplug:"?");
    udev_device_unref(dev);
    return 1; // react to any drm event
}

// ------------- Signals / time utils -------------
static int g_exit_flag = 0;
static void on_sigint(int sig){ (void)sig; g_exit_flag = 1; }
static long long ms_since(struct timespec newer, struct timespec older){
    return (newer.tv_sec - older.tv_sec)*1000LL + (newer.tv_nsec - older.tv_nsec)/1000000LL;
}

// ------------- Main -------------
int main(int argc, char** argv){
    AppCfg cfg; if (parse_cli(argc, argv, &cfg) != 0) return 2;
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGCHLD, SIG_DFL);

    int fd = open(cfg.card_path, O_RDWR | O_CLOEXEC);
    if (fd < 0){ LOGE("open %s: %s", cfg.card_path, strerror(errno)); return 1; }

    int audio_disabled = 0;
    int restart_count = 0;
    struct timespec window_start = {0,0};

    ModesetResult ms = {0};
    typedef enum { GST_STOPPED=0, GST_RUNNING=1, GST_STOPPING=2 } _dummy;
    GstProc gp = { .pid = 0, .pgid = 0, .state = GST_STOPPED };
    UMon um = {0};
    OSD osd = {0};

    if (cfg.use_udev){
        if (udev_open(&um) != 0){ LOGW("udev disabled (open failed)"); cfg.use_udev = 0; }
    }

    int connected = is_any_connected(fd, &cfg);
    if (connected){
        if (atomic_modeset_maxhz(fd, &cfg, cfg.osd_enable, &ms) == 0){
            if (cfg.osd_enable) osd_setup(fd, &cfg, &ms, cfg.plane_id, &osd);
            if (!cfg.stay_blue){
                if (gst_start(&cfg, audio_disabled, &gp) != 0) LOGE("Failed to start pipeline");
                clock_gettime(CLOCK_MONOTONIC, &window_start);
                restart_count = 0;
            } else {
                LOGI("--stay-blue set, not starting pipeline");
            }
        } else {
            LOGE("Initial modeset failed; will wait for hotplug events");
        }
    } else {
        LOGI("No monitor connected; waiting for hotplug...");
    }

    int backoff_ms = 0;
    const int debounce_ms = 300;
    struct timespec last_hp = {0,0};
    struct timespec last_osd = {0,0}; clock_gettime(CLOCK_MONOTONIC, &last_osd);

    while (!g_exit_flag){
        // Reap pipeline if it died
        gst_poll_child(&gp);

        // Poll udev
        struct pollfd pfds[2]; int nfds = 0;
        int ufd = cfg.use_udev ? um.fd : -1;
        if (ufd >= 0){ pfds[nfds++] = (struct pollfd){ .fd=ufd, .events=POLLIN }; }
        pfds[nfds++] = (struct pollfd){ .fd=STDIN_FILENO, .events=0 };

        int tout = 200;
        (void)poll(pfds, nfds, tout);

        if (ufd >= 0 && nfds>0 && (pfds[0].revents & POLLIN)){
            if (udev_did_hotplug(&um)){
                struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
                if (last_hp.tv_sec != 0 && ms_since(now, last_hp) < debounce_ms){
                    LOGV("Hotplug debounced");
                } else {
                    last_hp = now;
                    int now_connected = is_any_connected(fd, &cfg);
                    LOGI("Hotplug: connected=%d", now_connected);
                    if (!now_connected){
                        if (gp.state != GST_STOPPED) gst_stop(&gp, 700);
                        if (osd.active) { osd_commit_disable(fd, &osd); osd.active=0; }
                        connected = 0;
                    } else {
                        if (atomic_modeset_maxhz(fd, &cfg, cfg.osd_enable, &ms) == 0){
                            connected = 1;
                            if (cfg.osd_enable){ osd_destroy(fd, &osd); osd_setup(fd, &cfg, &ms, cfg.plane_id, &osd); }
                            if (!cfg.stay_blue){
                                if (gp.state != GST_STOPPED) gst_stop(&gp, 700);
                                if (gst_start(&cfg, audio_disabled, &gp) != 0) LOGE("Failed to start pipeline after hotplug");
                                clock_gettime(CLOCK_MONOTONIC, &window_start);
                                restart_count = 0;
                            }
                            backoff_ms = 0;
                        } else {
                            backoff_ms = backoff_ms==0 ? 250 : (backoff_ms*2);
                            if (backoff_ms > 2000) backoff_ms = 2000;
                            LOGW("Modeset failed; retry in %d ms", backoff_ms);
                            usleep(backoff_ms*1000);
                        }
                    }
                }
            }
        }

        // OSD tick
        if (cfg.osd_enable && connected && osd.active){
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            if (ms_since(now, last_osd) >= cfg.osd_refresh_ms){
                osd_update_stats(fd, &cfg, &ms, &gp, audio_disabled, restart_count, &osd);
                last_osd = now;
            }
        }

        // Restart pipeline if needed
        if (!cfg.stay_blue && connected && gp.state == GST_STOPPED){
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            long long elapsed_ms = (now.tv_sec - window_start.tv_sec)*1000LL
                                 + (now.tv_nsec - window_start.tv_nsec)/1000000LL;
            if (elapsed_ms > cfg.restart_window_ms){
                window_start = now;
                restart_count = 0;
            }
            restart_count++;
            if (!cfg.no_audio && cfg.audio_optional && !audio_disabled && restart_count >= cfg.restart_limit){
                audio_disabled = 1;
                LOGW("Audio device likely busy; switching audio branch to fakesink to avoid restart loop.");
            } else if (cfg.no_audio) {
                audio_disabled = 1;
            }
            LOGW("Pipeline not running; restarting%s...", audio_disabled ? " (audio=fakesink)" : "");
            if (gst_start(&cfg, audio_disabled, &gp) != 0) LOGE("Restart failed");
        }
    }

    if (gp.state != GST_STOPPED) gst_stop(&gp, 700);
    if (osd.active) osd_commit_disable(fd, &osd);
    destroy_dumb_fb(fd, &osd.fb);
    if (cfg.use_udev) udev_close(&um);
    close(fd);
    LOGI("Bye.");
    return 0;
}
