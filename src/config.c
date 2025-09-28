#include "config.h"
#include "logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog) {
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
            "  --verbose\n",
            prog);
}

void cfg_defaults(AppCfg *c) {
    memset(c, 0, sizeof(*c));
    strcpy(c->card_path, "/dev/dri/card0");
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
    c->kmssink_qos = 1;
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

int parse_cli(int argc, char **argv, AppCfg *cfg) {
    cfg_defaults(cfg);
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--card") && i + 1 < argc) {
            strncpy(cfg->card_path, argv[++i], sizeof(cfg->card_path) - 1);
        } else if (!strcmp(argv[i], "--connector") && i + 1 < argc) {
            strncpy(cfg->connector_name, argv[++i], sizeof(cfg->connector_name) - 1);
        } else if (!strcmp(argv[i], "--plane-id") && i + 1 < argc) {
            cfg->plane_id = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--blank-primary")) {
            cfg->blank_primary = 1;
        } else if (!strcmp(argv[i], "--no-udev")) {
            cfg->use_udev = 0;
        } else if (!strcmp(argv[i], "--stay-blue")) {
            cfg->stay_blue = 1;
        } else if (!strcmp(argv[i], "--blue-hold-ms") && i + 1 < argc) {
            cfg->blue_hold_ms = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--udp-port") && i + 1 < argc) {
            cfg->udp_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--vid-pt") && i + 1 < argc) {
            cfg->vid_pt = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--aud-pt") && i + 1 < argc) {
            cfg->aud_pt = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--latency-ms") && i + 1 < argc) {
            cfg->latency_ms = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--max-lateness") && i + 1 < argc) {
            cfg->max_lateness_ns = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--aud-dev") && i + 1 < argc) {
            strncpy(cfg->aud_dev, argv[++i], sizeof(cfg->aud_dev) - 1);
        } else if (!strcmp(argv[i], "--no-audio")) {
            cfg->no_audio = 1;
        } else if (!strcmp(argv[i], "--audio-optional")) {
            cfg->audio_optional = 1;
        } else if (!strcmp(argv[i], "--audio-required")) {
            cfg->audio_optional = 0;
        } else if (!strcmp(argv[i], "--osd")) {
            cfg->osd_enable = 1;
        } else if (!strcmp(argv[i], "--osd-plane-id") && i + 1 < argc) {
            cfg->osd_plane_id = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--osd-refresh-ms") && i + 1 < argc) {
            cfg->osd_refresh_ms = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--gst-log")) {
            cfg->gst_log = 1;
        } else if (!strcmp(argv[i], "--verbose")) {
            log_set_verbose(1);
        } else {
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}
