#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h>
#include <sched.h>

#include "osd_layout.h"

#define SPLASH_MAX_SEQUENCES 32

#ifndef CPU_SETSIZE
#define CPU_SETSIZE ((int)(sizeof(cpu_set_t) * CHAR_BIT))
#endif

typedef enum {
    CUSTOM_SINK_RECEIVER = 0,
    CUSTOM_SINK_UDPSRC,
} CustomSinkMode;

typedef struct {
    char name[64];
    int start_frame;
    int end_frame;
} SplashSequenceCfg;

typedef struct {
    int enable;
    int idle_timeout_ms;
    double fps;
    char input_path[PATH_MAX];
    char default_sequence[64];
    int sequence_count;
    SplashSequenceCfg sequences[SPLASH_MAX_SEQUENCES];
} SplashCfg;

typedef struct {
    char card_path[64];
    char connector_name[32];
    char config_path[PATH_MAX];
    int plane_id;
    int use_udev;

    int udp_port;
    int vid_pt;
    int aud_pt;
    int latency_ms;
    int udpsrc_pt97_filter;
    CustomSinkMode custom_sink;
    char aud_dev[128];

    int idr_request;
    int idr_request_min_ms;
    int idr_request_max_ms;
    int idr_request_sustain_ms;
    int idr_request_reset_ms;
    int idr_request_timeout_ms;

    int no_audio;
    int audio_optional;
    int restart_limit;
    int restart_window_ms;

    int osd_enable;
    int osd_plane_id;
    int osd_refresh_ms;

    int gst_log;

    int cpu_affinity_present;
    cpu_set_t cpu_affinity_mask;
    int cpu_affinity_order[CPU_SETSIZE];
    int cpu_affinity_count;

    OsdLayout osd_layout;

    SplashCfg splash;
} AppCfg;

int parse_cli(int argc, char **argv, AppCfg *cfg);
void cfg_defaults(AppCfg *cfg);
int cfg_load_file(const char *path, AppCfg *cfg);
int cfg_parse_cpu_list(const char *list, AppCfg *cfg);
int cfg_has_cpu_affinity(const AppCfg *cfg);
void cfg_get_process_affinity(const AppCfg *cfg, cpu_set_t *set_out);
int cfg_get_thread_affinity(const AppCfg *cfg, int slot, cpu_set_t *set_out);
int cfg_parse_custom_sink_mode(const char *value, CustomSinkMode *mode_out);
const char *cfg_custom_sink_mode_name(CustomSinkMode mode);

#endif // CONFIG_H
