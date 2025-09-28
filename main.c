// SPDX-License-Identifier: MIT
// pixelpilot_mini_rk — HDMI + atomic KMS + udev hotplug + GStreamer runner (plane-only kmssink)
// - Max refresh → max resolution
// - Plane-only kmssink (no crtc-id/connector-id)
// - udev hotplug with debounce
// - Gst child in its own process group + state machine to avoid double-spawn
// - Audio optional fallback (to fakesink) to avoid restart loops when ALSA is busy
//
// Build:
//   gcc -O2 -Wall -o pixelpilot_mini_rk main.c $(pkg-config --cflags --libs libdrm libudev)

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
    int  plane_id;              // overlay plane to use (default 76)
    int  blank_primary;         // detach primary plane
    int  stay_blue;             // show blue FB only, no gst pipeline
    int  blue_hold_ms;          // hold blue after commit (default 0)
    int  use_udev;              // enable hotplug listener

    // GStreamer (video + audio)
    int  udp_port;              // default 5600
    int  vid_pt;                // default 97 (H265)
    int  aud_pt;                // default 98 (Opus)
    int  latency_ms;            // default 8
    int  kmssink_sync;          // default false
    int  kmssink_qos;           // default true
    int  max_lateness_ns;       // default 20000000
    char aud_dev[128];          // ALSA device

    // Audio behavior
    int  no_audio;              // 1 = never add audio branch
    int  audio_optional;        // 1 = auto-fallback to fakesink on repeated failures (default on)
    int  restart_limit;         // failures within window to trigger fallback (default 3)
    int  restart_window_ms;     // window length (default 2000 ms)

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
    c->audio_optional = 1;   // default: allow fallback
    c->restart_limit = 3;
    c->restart_window_ms = 2000;

    c->gst_log = 0;
}

static void usage(const char* p){
    fprintf(stderr,
    "Usage: %s [options]\n"
    "  --card /dev/dri/cardN        (default: /dev/dri/card0)\n"
    "  --connector NAME             (e.g. HDMI-A-1; default: first CONNECTED)\n"
    "  --plane-id N                 (default: 76)\n"
    "  --blank-primary              (detach primary plane on commit)\n"
    "  --no-udev                    (disable hotplug listener)\n"
    "  --stay-blue                  (only do modeset & blue FB; no pipeline)\n"
    "  --blue-hold-ms N             (hold blue for N ms after commit; default 0)\n"
    "  --udp-port N                 (default: 5600)\n"
    "  --vid-pt N                   (default: 97 H265)\n"
    "  --aud-pt N                   (default: 98 Opus)\n"
    "  --latency-ms N               (default: 8)\n"
    "  --max-lateness NANOSECS      (default: 20000000)\n"
    "  --aud-dev STR                (default: plughw:CARD=rockchiphdmi0,DEV=0)\n"
    "  --no-audio                   (drop audio branch entirely)\n"
    "  --audio-optional             (auto-fallback to fakesink on failures; default)\n"
    "  --audio-required             (disable auto-fallback; keep real audio only)\n"
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
    uint32_t blue = 0x000000FFu;  // XRGB8888 → 0x00RRGGBB: BLUE
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
static int get_prop_id_and_range(int fd, uint32_t obj_id, uint32_t obj_type,
                                 const char* name, uint32_t* out_id,
                                 uint64_t* out_min, uint64_t* out_max) {
    drmModeObjectProperties* props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) return -1;
    int found = 0;
    for (uint32_t i=0;i<props->count_props;i++){
        drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);
        if (!p) continue;
        if (!strcmp(p->name, name)){
            *out_id = p->prop_id; found = 1;
            if ((p->flags & DRM_MODE_PROP_RANGE) && out_min && out_max){
                *out_min = p->values[0]; *out_max = p->values[1];
            }
            drmModeFreeProperty(p);
            break;
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

// Single probe+commit function; returns 0 and fills chosen ids if success.
typedef struct {
    uint32_t connector_id;
    uint32_t crtc_id;
    int mode_w, mode_h, mode_hz;
} ModesetResult;

static int atomic_modeset_maxhz(int fd, const AppCfg* cfg, ModesetResult* out){
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0)
        LOGW("Failed to enable UNIVERSAL_PLANES");
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0)
        LOGW("Failed to enable ATOMIC (driver may not support)");

    drmModeRes* res = drmModeGetResources(fd);
    if (!res){ LOGE("drmModeGetResources failed"); return -1; }

    drmModeConnector* conn = NULL;
    drmModeCrtc*      crtc = NULL;
    drmModeModeInfo   best = (drmModeModeInfo){0};

    // Pick connector & best mode
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

    // Build blue fb + mode blob
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

    // Lookup props
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
    if (get_prop_id_and_range(fd, cfg->plane_id, DRM_MODE_OBJECT_PLANE, "ZPOS", &plane_zpos_id, &zmin, &zmax)==0)
        have_zpos = 1;

    // Optional primary blank
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

    // Atomic req
    drmModeAtomicReq* req = drmModeAtomicAlloc();
    if (!req){ LOGE("drmModeAtomicAlloc failed"); drmModeDestroyPropertyBlob(fd,mode_blob); destroy_dumb_fb(fd,&fb); drmModeFreeConnector(conn); drmModeFreeCrtc(crtc); drmModeFreeResources(res); return -8; }

    // CRTC: ACTIVE + MODE_ID
    drmModeAtomicAddProperty(req, crtc->crtc_id, crtc_active, 1);
    drmModeAtomicAddProperty(req, crtc->crtc_id, crtc_mode_id, mode_blob);

    // CONNECTOR -> CRTC
    drmModeAtomicAddProperty(req, conn->connector_id, conn_crtc_id, crtc->crtc_id);

    // PLANE: fullscreen BLUE
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
    if (have_zpos) drmModeAtomicAddProperty(req, cfg->plane_id, plane_zpos_id, (uint64_t)zmax);

    // Blank primary if requested
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

    LOGI("Atomic COMMIT: %dx%d@%d on %s via plane %d%s — BLUE",
         w,h,hz,cname,cfg->plane_id, have_zpos?" ZPOS=max":"");

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

// Quick probe for “connected?”
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

static int gst_is_running(const GstProc* gp){
    if (gp->pid <= 0) return 0;
    if (kill(gp->pid, 0) == 0 && gp->state == GST_RUNNING) return 1;
    return 0;
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
    // For KMS, the driver often emits "change" with HOTPLUG=1; we react to any drm action.
    return 1;
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
    signal(SIGCHLD, SIG_DFL); // we reap via waitpid(WNOHANG)

    int fd = open(cfg.card_path, O_RDWR | O_CLOEXEC);
    if (fd < 0){ LOGE("open %s: %s", cfg.card_path, strerror(errno)); return 1; }

    // Restart-rate tracker to decide audio fallback
    int audio_disabled = 0;
    int restart_count = 0;
    struct timespec window_start = {0,0};

    ModesetResult ms = {0};
    GstProc gp = { .pid = 0, .pgid = 0, .state = GST_STOPPED };
    UMon um = {0};

    if (cfg.use_udev){
        if (udev_open(&um) != 0){ LOGW("udev disabled (open failed)"); cfg.use_udev = 0; }
    }

    // Initial: if connected → modeset (blue) → start gst (unless stay-blue)
    int connected = is_any_connected(fd, &cfg);
    if (connected){
        if (atomic_modeset_maxhz(fd, &cfg, &ms) == 0){
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

    // Event loop
    int backoff_ms = 0;
    const int debounce_ms = 300;
    struct timespec last_hp = {0,0};

    while (!g_exit_flag){
        // Reap child if needed
        gst_poll_child(&gp);

        struct pollfd pfds[2]; int nfds = 0;
        int ufd = cfg.use_udev ? um.fd : -1;
        if (ufd >= 0){ pfds[nfds++] = (struct pollfd){ .fd=ufd, .events=POLLIN }; }
        pfds[nfds++] = (struct pollfd){ .fd=STDIN_FILENO, .events=0 }; // keep poll active

        int tout = 500; // ms
        (void)poll(pfds, nfds, tout);

        // udev hotplug
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
                        // disconnect → stop pipeline
                        if (gp.state != GST_STOPPED) gst_stop(&gp, 700);
                        connected = 0;
                    } else {
                        // connect: re-modeset, restart pipeline (reset restart window)
                        if (atomic_modeset_maxhz(fd, &cfg, &ms) == 0){
                            connected = 1;
                            if (!cfg.stay_blue){
                                if (gp.state != GST_STOPPED) gst_stop(&gp, 700);
                                if (gst_start(&cfg, audio_disabled, &gp) != 0) LOGE("Failed to start pipeline after hotplug");
                                clock_gettime(CLOCK_MONOTONIC, &window_start);
                                restart_count = 0;
                            }
                            backoff_ms = 0;
                        } else {
                            // retry with backoff
                            backoff_ms = backoff_ms==0 ? 250 : (backoff_ms*2);
                            if (backoff_ms > 2000) backoff_ms = 2000;
                            LOGW("Modeset failed; retry in %d ms", backoff_ms);
                            usleep(backoff_ms*1000);
                        }
                    }
                }
            }
        }

        // Pipeline supervision: if not running, (re)start
        if (!cfg.stay_blue && connected && gp.state == GST_STOPPED){
            // Update restart-rate window
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            long long elapsed_ms = (now.tv_sec - window_start.tv_sec)*1000LL
                                 + (now.tv_nsec - window_start.tv_nsec)/1000000LL;

            if (elapsed_ms > cfg.restart_window_ms){
                window_start = now;
                restart_count = 0;
            }
            restart_count++;

            // If we're failing a lot and audio is optional, disable audio branch
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

    // Shutdown
    if (gp.state != GST_STOPPED) gst_stop(&gp, 700);
    if (cfg.use_udev) udev_close(&um);
    close(fd);
    LOGI("Bye.");
    return 0;
}
