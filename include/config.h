#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h>
#include <sched.h>

#include "osd_layout.h"

#ifndef CPU_SETSIZE
#define CPU_SETSIZE ((int)(sizeof(cpu_set_t) * CHAR_BIT))
#endif

typedef struct {
    char card_path[64];
    char connector_name[32];
    char config_path[PATH_MAX];
    int plane_id;
    int plane_id_override;
    int blank_primary;
    int use_udev;

    int udp_port;
    int vid_pt;
    int aud_pt;
    int latency_ms;
    int max_lateness_ns;
#ifdef ENABLE_PIPELINE_TUNING
    int kmssink_sync;
    int kmssink_qos;
    int video_queue_leaky;
    int video_queue_pre_buffers;
    int video_queue_post_buffers;
    int video_queue_sink_buffers;
#endif
    int use_gst_udpsrc;
    char aud_dev[128];

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
} AppCfg;

int parse_cli(int argc, char **argv, AppCfg *cfg);
void cfg_defaults(AppCfg *cfg);
int cfg_load_file(const char *path, AppCfg *cfg);
int cfg_parse_cpu_list(const char *list, AppCfg *cfg);
int cfg_has_cpu_affinity(const AppCfg *cfg);
void cfg_get_process_affinity(const AppCfg *cfg, cpu_set_t *set_out);
int cfg_get_thread_affinity(const AppCfg *cfg, int slot, cpu_set_t *set_out);

#endif // CONFIG_H
