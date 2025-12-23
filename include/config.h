#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h>
#include <sched.h>

#include "osd_layout.h"

#define SPLASH_MAX_SEQUENCES 32

#ifndef OSD_REFRESH_MIN_MS
#define OSD_REFRESH_MIN_MS 50
#endif

#ifndef CPU_SETSIZE
#define CPU_SETSIZE ((int)(sizeof(cpu_set_t) * CHAR_BIT))
#endif

typedef enum {
    CUSTOM_SINK_RECEIVER = 0,
    CUSTOM_SINK_UDPSRC,
} CustomSinkMode;

typedef enum {
    RECORD_MODE_STANDARD = 0,
    RECORD_MODE_SEQUENTIAL,
    RECORD_MODE_FRAGMENTED,
} RecordMode;

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
    int enable;
    char output_path[PATH_MAX];
    RecordMode mode;
} RecordCfg;

typedef struct {
    int enable;
    double matrix[9];
} VideoCtmCfg;

typedef struct {
    int enable;
    char bind_address[64];
    int port;
    unsigned int interval_ms;
} SseCfg;

typedef struct {
    int enable;
    int http_port;
    unsigned int http_timeout_ms;
    char http_path[128];
    int endpoint_force;
    char endpoint_host[64];
    int endpoint_port;
    int stats_trigger;
    unsigned int loss_window_ms;
    unsigned int loss_threshold;
    double jitter_threshold_ms;
    unsigned int jitter_cooldown_ms;
} IdrCfg;

typedef struct {
    char card_path[64];
    char connector_name[32];
    char config_path[PATH_MAX];
    int plane_id;
    int use_udev;
    int mode_w;
    int mode_h;
    int mode_hz;

    int udp_port;
    int vid_pt;
    int aud_pt;
    int appsink_max_buffers;
    int udpsrc_pt97_filter;
    CustomSinkMode custom_sink;
    char aud_dev[128];
    int audio_queue_start_buffers;
    int audio_queue_play_buffers;
    int audio_queue_sink_buffers;
    int audio_record_queue_buffers;
    unsigned int audio_sink_buffer_time_us;
    unsigned int audio_sink_latency_time_us;

    int no_audio;
    int audio_optional;
    int restart_limit;
    int restart_window_ms;

    int osd_enable;
    int osd_plane_id;
    int osd_refresh_ms;
    int osd_margin;

    int gst_log;

    int cpu_affinity_present;
    cpu_set_t cpu_affinity_mask;
    int cpu_affinity_order[CPU_SETSIZE];
    int cpu_affinity_count;

    OsdLayout osd_layout;

    struct {
        int enable;
        int enable_set;
        char bind_address[64];
        int udp_port;
    } osd_external;

    SplashCfg splash;
    RecordCfg record;
    SseCfg sse;
    IdrCfg idr;
    VideoCtmCfg video_ctm;
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
int cfg_parse_record_mode(const char *value, RecordMode *mode_out);
const char *cfg_record_mode_name(RecordMode mode);
int cfg_parse_host_and_port(const char *value, char *host_out, size_t host_len, int *port_out);
int cfg_set_drm_mode_from_string(const char *value, AppCfg *cfg);

#endif // CONFIG_H
