#define _GNU_SOURCE

#include "config.h"
#include "logging.h"

#include <ctype.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --card /dev/dri/cardN        (default: /dev/dri/card0)\n"
            "  --connector NAME             (e.g. HDMI-A-1; default: first CONNECTED)\n"
            "  --plane-id N                 (video plane; default: 76)\n"
            "  --no-udev                    (disable hotplug listener)\n"
            "  --config PATH                (load settings from ini file)\n"
            "  --udp-port N                 (default: 5600)\n"
            "  --vid-pt N                   (default: 97 H265)\n"
            "  --aud-pt N                   (default: 98 Opus)\n"
            "  --latency-ms N               (default: 8)\n"
            "  --custom-sink MODE           (receiver|udpsrc; default: receiver)\n"
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

typedef struct {
    const char *name;
    CustomSinkMode mode;
} CustomSinkAlias;

static const CustomSinkAlias kCustomSinkAliases[] = {
    {"receiver", CUSTOM_SINK_RECEIVER},
    {"udp-receiver", CUSTOM_SINK_RECEIVER},
    {"appsrc", CUSTOM_SINK_RECEIVER},
    {"udpsrc", CUSTOM_SINK_UDPSRC},
    {"gst-udpsrc", CUSTOM_SINK_UDPSRC},
    {"gst", CUSTOM_SINK_UDPSRC},
};

int cfg_parse_custom_sink_mode(const char *value, CustomSinkMode *mode_out) {
    if (value == NULL || mode_out == NULL) {
        return -1;
    }
    for (size_t i = 0; i < sizeof(kCustomSinkAliases) / sizeof(kCustomSinkAliases[0]); ++i) {
        if (strcasecmp(value, kCustomSinkAliases[i].name) == 0) {
            *mode_out = kCustomSinkAliases[i].mode;
            return 0;
        }
    }
    return -1;
}

const char *cfg_custom_sink_mode_name(CustomSinkMode mode) {
    switch (mode) {
    case CUSTOM_SINK_RECEIVER:
        return "receiver";
    case CUSTOM_SINK_UDPSRC:
        return "udpsrc";
    default:
        return "unknown";
    }
}

void cfg_defaults(AppCfg *c) {
    memset(c, 0, sizeof(*c));
    strcpy(c->card_path, "/dev/dri/card0");
    c->plane_id = 76;
    c->use_udev = 1;
    c->config_path[0] = '\0';

    c->udp_port = 5600;
    c->vid_pt = 97;
    c->aud_pt = 98;
    c->latency_ms = 8;
    c->udpsrc_pt97_filter = 1;
    c->custom_sink = CUSTOM_SINK_RECEIVER;
    strcpy(c->aud_dev, "plughw:CARD=rockchiphdmi0,DEV=0");

    c->idr_request = 0;
    c->idr_request_min_ms = 50;
    c->idr_request_max_ms = 1000;
    c->idr_request_sustain_ms = 5000;
    c->idr_request_reset_ms = 2000;
    c->idr_request_timeout_ms = 200;

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

    c->splash.enable = 0;
    c->splash.idle_timeout_ms = 2000;
    c->splash.fps = 30.0;
    c->splash.input_path[0] = '\0';
    c->splash.default_sequence[0] = '\0';
    c->splash.sequence_count = 0;
    for (int i = 0; i < SPLASH_MAX_SEQUENCES; ++i) {
        c->splash.sequences[i].name[0] = '\0';
        c->splash.sequences[i].start_frame = -1;
        c->splash.sequences[i].end_frame = -1;
    }
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

static void cli_copy_string(char *dst, size_t dst_size, const char *src) {
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    size_t copy_len = strlen(src);
    if (copy_len >= dst_size) {
        copy_len = dst_size - 1;
    }
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

int parse_cli(int argc, char **argv, AppCfg *cfg) {
    cfg_defaults(cfg);

    const char *config_file = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            config_file = argv[++i];
            cli_copy_string(cfg->config_path, sizeof(cfg->config_path), config_file);
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
            cli_copy_string(cfg->card_path, sizeof(cfg->card_path), argv[++i]);
        } else if (!strcmp(argv[i], "--connector") && i + 1 < argc) {
            cli_copy_string(cfg->connector_name, sizeof(cfg->connector_name), argv[++i]);
        } else if (!strcmp(argv[i], "--plane-id") && i + 1 < argc) {
            cfg->plane_id = atoi(argv[++i]);
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
        } else if (!strcmp(argv[i], "--custom-sink") && i + 1 < argc) {
            const char *mode_str = argv[++i];
            CustomSinkMode mode;
            if (cfg_parse_custom_sink_mode(mode_str, &mode) != 0) {
                LOGE("Unknown custom sink mode '%s'", mode_str);
                return -1;
            }
            cfg->custom_sink = mode;
        } else if (!strcmp(argv[i], "--custom-sink")) {
            LOGE("--custom-sink requires an argument (receiver|udpsrc)");
            return -1;
        } else if (!strcmp(argv[i], "--gst-udpsrc")) {
            LOGW("--gst-udpsrc is deprecated; use --custom-sink udpsrc instead");
            cfg->custom_sink = CUSTOM_SINK_UDPSRC;
        } else if (!strcmp(argv[i], "--no-gst-udpsrc")) {
            LOGW("--no-gst-udpsrc is deprecated; use --custom-sink receiver instead");
            cfg->custom_sink = CUSTOM_SINK_RECEIVER;
        } else if (!strcmp(argv[i], "--aud-dev") && i + 1 < argc) {
            cli_copy_string(cfg->aud_dev, sizeof(cfg->aud_dev), argv[++i]);
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
