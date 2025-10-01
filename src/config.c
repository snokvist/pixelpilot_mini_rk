#define _GNU_SOURCE

#include "config.h"
#include "logging.h"

#include <ctype.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --card /dev/dri/cardN        (default: /dev/dri/card0)\n"
            "  --connector NAME             (e.g. HDMI-A-1; default: first CONNECTED)\n"
            "  --plane-id N                 (video plane; default: auto)\n"
            "  --blank-primary              (detach primary plane on commit; default)\n"
            "  --keep-primary               (leave primary plane attached on commit)\n"
            "  --no-udev                    (disable hotplug listener)\n"
            "  --config PATH                (load settings from ini file)\n"
            "  --udp-port N                 (default: 5600)\n"
            "  --vid-pt N                   (default: 97 H265)\n"
            "  --aud-pt N                   (default: 98 Opus)\n"
            "  --latency-ms N               (default: 8)\n"
            "  --video-queue-leaky MODE     (0=none,1=upstream,2=downstream; default: 2)\n"
            "  --video-queue-pre-buffers N  (default: 96)\n"
            "  --video-queue-post-buffers N (default: 8)\n"
            "  --video-queue-sink-buffers N (default: 8)\n"
            "  --gst-udpsrc                 (use GStreamer's udpsrc instead of appsrc bridge)\n"
            "  --no-gst-udpsrc              (force legacy appsrc/UEP receiver)\n"
            "  --max-lateness NANOSECS      (default: 20000000)\n"
            "  --aud-dev STR                (default: plughw:CARD=rockchiphdmi0,DEV=0)\n"
            "  --no-audio                   (drop audio branch entirely)\n"
            "  --audio-optional             (auto-fallback to fakesink on failures; default)\n"
            "  --audio-required             (disable auto-fallback; keep real audio only)\n"
            "  --osd                        (enable OSD overlay plane with stats)\n"
            "  --osd-plane-id N             (force OSD plane id; default auto)\n"
            "  --osd-refresh-ms N           (default: 500)\n"
            "  --gst-log                    (set GST_DEBUG=3 if not set)\n"
            "  --cpu-list LIST              (comma-separated CPU IDs for affinity)\n"
            "  --verbose\n",
            prog);
}

static int clamp_with_warning(const char *name, int value, int min, int max) {
    if (value < min) {
        LOGW("%s below minimum (%d < %d); clamping", name, value, min);
        return min;
    }
    if (value > max) {
        LOGW("%s above maximum (%d > %d); clamping", name, value, max);
        return max;
    }
    return value;
}

static void apply_guardrails(AppCfg *cfg) {
    if (cfg->plane_id < 0) {
        LOGW("plane-id %d invalid; falling back to auto-detect", cfg->plane_id);
        cfg->plane_id = 0;
    }

    cfg->video_queue_pre_buffers =
        clamp_with_warning("video-queue-pre-buffers", cfg->video_queue_pre_buffers, 4, 128);
    cfg->video_queue_post_buffers =
        clamp_with_warning("video-queue-post-buffers", cfg->video_queue_post_buffers, 2, 32);
    cfg->video_queue_sink_buffers =
        clamp_with_warning("video-queue-sink-buffers", cfg->video_queue_sink_buffers, 2, 32);
}

void cfg_defaults(AppCfg *c) {
    memset(c, 0, sizeof(*c));
    strcpy(c->card_path, "/dev/dri/card0");
    c->plane_id = 0;
    c->blank_primary = 1;
    c->use_udev = 1;
    c->config_path[0] = '\0';

    c->udp_port = 5600;
    c->vid_pt = 97;
    c->aud_pt = 98;
    c->latency_ms = 8;
    c->kmssink_sync = 0;
    c->kmssink_qos = 1;
    c->max_lateness_ns = 20000000;
    c->video_queue_leaky = 2;
    c->video_queue_pre_buffers = 96;
    c->video_queue_post_buffers = 8;
    c->video_queue_sink_buffers = 8;
    c->use_gst_udpsrc = 0;
    strcpy(c->aud_dev, "plughw:CARD=rockchiphdmi0,DEV=0");

    c->no_audio = 0;
    c->audio_optional = 1;
    c->restart_limit = 3;
    c->restart_window_ms = 2000;

    c->osd_enable = 0;
    c->osd_plane_id = 0;
    c->osd_refresh_ms = 500;

    c->gst_log = 0;

    c->cpu_affinity_present = 0;
    CPU_ZERO(&c->cpu_affinity_mask);
    c->cpu_affinity_count = 0;
    memset(c->cpu_affinity_order, 0, sizeof(c->cpu_affinity_order));

    osd_layout_defaults(&c->osd_layout);
}

int cfg_parse_cpu_list(const char *list, AppCfg *cfg) {
    if (list == NULL || *list == '\0') {
        LOGE("--cpu-list requires at least one CPU id");
        return -1;
    }

    cpu_set_t mask;
    CPU_ZERO(&mask);

    int order[CPU_SETSIZE];
    int count = 0;

    const char *p = list;
    while (*p != '\0') {
        while (isspace((unsigned char)*p)) {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        char *end = NULL;
        long cpu = strtol(p, &end, 10);
        if (end == p) {
            LOGE("Invalid token in --cpu-list: '%s'", p);
            return -1;
        }
        if (cpu < 0 || cpu >= CPU_SETSIZE) {
            LOGE("CPU index %ld out of range (0-%d)", cpu, CPU_SETSIZE - 1);
            return -1;
        }
        if (!CPU_ISSET((int)cpu, &mask)) {
            if (count >= CPU_SETSIZE) {
                LOGE("Too many CPUs specified in --cpu-list (max %d)", CPU_SETSIZE);
                return -1;
            }
            order[count++] = (int)cpu;
        }
        CPU_SET((int)cpu, &mask);
        p = end;
        while (isspace((unsigned char)*p)) {
            ++p;
        }
        if (*p == ',') {
            ++p;
        } else if (*p != '\0') {
            LOGE("Unexpected character '%c' in --cpu-list", *p);
            return -1;
        }
    }

    if (count == 0) {
        LOGE("--cpu-list did not contain any CPUs");
        return -1;
    }

    cfg->cpu_affinity_present = 1;
    cfg->cpu_affinity_mask = mask;
    cfg->cpu_affinity_count = count;
    memset(cfg->cpu_affinity_order, 0, sizeof(cfg->cpu_affinity_order));
    memcpy(cfg->cpu_affinity_order, order, count * sizeof(int));
    return 0;
}

int parse_cli(int argc, char **argv, AppCfg *cfg) {
    cfg_defaults(cfg);

    const char *config_file = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            config_file = argv[++i];
            strncpy(cfg->config_path, config_file, sizeof(cfg->config_path) - 1);
            cfg->config_path[sizeof(cfg->config_path) - 1] = '\0';
            break;
        }
    }

    if (config_file) {
        if (cfg_load_file(config_file, cfg) != 0) {
            return -1;
        }
    }

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            ++i;
            continue;
        } else if (!strcmp(argv[i], "--card") && i + 1 < argc) {
            strncpy(cfg->card_path, argv[++i], sizeof(cfg->card_path) - 1);
        } else if (!strcmp(argv[i], "--connector") && i + 1 < argc) {
            strncpy(cfg->connector_name, argv[++i], sizeof(cfg->connector_name) - 1);
        } else if (!strcmp(argv[i], "--plane-id") && i + 1 < argc) {
            cfg->plane_id = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--blank-primary")) {
            cfg->blank_primary = 1;
        } else if (!strcmp(argv[i], "--keep-primary")) {
            cfg->blank_primary = 0;
        } else if (!strcmp(argv[i], "--no-udev")) {
            cfg->use_udev = 0;
        } else if (!strcmp(argv[i], "--udp-port") && i + 1 < argc) {
            cfg->udp_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--vid-pt") && i + 1 < argc) {
            cfg->vid_pt = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--aud-pt") && i + 1 < argc) {
            cfg->aud_pt = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--latency-ms") && i + 1 < argc) {
            cfg->latency_ms = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--video-queue-leaky") && i + 1 < argc) {
            cfg->video_queue_leaky = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--video-queue-pre-buffers") && i + 1 < argc) {
            cfg->video_queue_pre_buffers = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--video-queue-post-buffers") && i + 1 < argc) {
            cfg->video_queue_post_buffers = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--video-queue-sink-buffers") && i + 1 < argc) {
            cfg->video_queue_sink_buffers = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--gst-udpsrc")) {
            cfg->use_gst_udpsrc = 1;
        } else if (!strcmp(argv[i], "--no-gst-udpsrc")) {
            cfg->use_gst_udpsrc = 0;
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
        } else if (!strcmp(argv[i], "--cpu-list") && i + 1 < argc) {
            if (cfg_parse_cpu_list(argv[++i], cfg) != 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--verbose")) {
            log_set_verbose(1);
        } else {
            usage(argv[0]);
            return -1;
        }
    }
    apply_guardrails(cfg);
    return 0;
}

int cfg_has_cpu_affinity(const AppCfg *cfg) {
    return cfg != NULL && cfg->cpu_affinity_present && cfg->cpu_affinity_count > 0;
}

void cfg_get_process_affinity(const AppCfg *cfg, cpu_set_t *set_out) {
    if (set_out == NULL) {
        return;
    }
    if (cfg_has_cpu_affinity(cfg)) {
        *set_out = cfg->cpu_affinity_mask;
    } else {
        CPU_ZERO(set_out);
    }
}

int cfg_get_thread_affinity(const AppCfg *cfg, int slot, cpu_set_t *set_out) {
    if (!cfg_has_cpu_affinity(cfg) || set_out == NULL) {
        return 0;
    }
    CPU_ZERO(set_out);
    if (cfg->cpu_affinity_count == 1) {
        int cpu = cfg->cpu_affinity_order[0];
        CPU_SET(cpu, set_out);
    } else {
        int idx = slot % cfg->cpu_affinity_count;
        int cpu = cfg->cpu_affinity_order[idx];
        CPU_SET(cpu, set_out);
    }
    return 1;
}
