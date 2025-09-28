#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    char card_path[64];
    char connector_name[32];
    int plane_id;
    int blank_primary;
    int use_udev;

    int udp_port;
    int vid_pt;
    int aud_pt;
    int latency_ms;
    int kmssink_sync;
    int kmssink_qos;
    int max_lateness_ns;
    char aud_dev[128];

    int no_audio;
    int audio_optional;
    int restart_limit;
    int restart_window_ms;

    int osd_enable;
    int osd_plane_id;
    int osd_refresh_ms;

    int gst_log;
} AppCfg;

int parse_cli(int argc, char **argv, AppCfg *cfg);
void cfg_defaults(AppCfg *cfg);

#endif // CONFIG_H
