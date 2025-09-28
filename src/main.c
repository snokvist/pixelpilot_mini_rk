// SPDX-License-Identifier: MIT
// pixelpilot_mini_rk — HDMI + atomic KMS + udev hotplug + GStreamer runner + OSD

#define _GNU_SOURCE

#include "config.h"
#include "drm_modeset.h"
#include "logging.h"
#include "osd.h"
#include "pipeline.h"
#include "udev_monitor.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_exit_flag = 0;

static void on_sigint(int sig) {
    (void)sig;
    g_exit_flag = 1;
}

static long long ms_since(struct timespec newer, struct timespec older) {
    return (newer.tv_sec - older.tv_sec) * 1000LL + (newer.tv_nsec - older.tv_nsec) / 1000000LL;
}

int main(int argc, char **argv) {
    AppCfg cfg;
    if (parse_cli(argc, argv, &cfg) != 0) {
        return 2;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGCHLD, SIG_DFL);

    int fd = open(cfg.card_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOGE("open %s: %s", cfg.card_path, strerror(errno));
        return 1;
    }

    int audio_disabled = cfg.no_audio ? 1 : 0;
    int restart_count = 0;
    struct timespec window_start = {0, 0};

    ModesetResult ms = {0};
    PipelineState ps = {.pid = 0, .pgid = 0, .state = PIPELINE_STOPPED};
    UdevMonitor um = {0};
    OSD osd;
    osd_init(&osd);

    if (cfg.use_udev) {
        if (udev_monitor_open(&um) != 0) {
            LOGW("udev disabled (open failed)");
            cfg.use_udev = 0;
        }
    }

    int connected = is_any_connected(fd, &cfg);
    if (connected) {
        if (atomic_modeset_maxhz(fd, &cfg, cfg.osd_enable, &ms) == 0) {
            if (cfg.osd_enable) {
                osd_setup(fd, &cfg, &ms, cfg.plane_id, &osd);
            }
            if (pipeline_start(&cfg, audio_disabled, &ps) != 0) {
                LOGE("Failed to start pipeline");
            }
            clock_gettime(CLOCK_MONOTONIC, &window_start);
            restart_count = 0;
        } else {
            LOGE("Initial modeset failed; will wait for hotplug events");
        }
    } else {
        LOGI("No monitor connected; waiting for hotplug...");
    }

    int backoff_ms = 0;
    const int debounce_ms = 300;
    struct timespec last_hp = {0, 0};
    struct timespec last_osd;
    clock_gettime(CLOCK_MONOTONIC, &last_osd);

    while (!g_exit_flag) {
        pipeline_poll_child(&ps);

        struct pollfd pfds[2];
        int nfds = 0;
        int ufd = cfg.use_udev ? um.fd : -1;
        if (ufd >= 0) {
            pfds[nfds++] = (struct pollfd){.fd = ufd, .events = POLLIN};
        }
        pfds[nfds++] = (struct pollfd){.fd = STDIN_FILENO, .events = 0};

        int tout = 200;
        (void)poll(pfds, nfds, tout);

        if (ufd >= 0 && nfds > 0 && (pfds[0].revents & POLLIN)) {
            if (udev_monitor_did_hotplug(&um)) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                if (last_hp.tv_sec != 0 && ms_since(now, last_hp) < debounce_ms) {
                    LOGV("Hotplug debounced");
                } else {
                    last_hp = now;
                    int now_connected = is_any_connected(fd, &cfg);
                    LOGI("Hotplug: connected=%d", now_connected);
                    if (!now_connected) {
                        if (ps.state != PIPELINE_STOPPED) {
                            pipeline_stop(&ps, 700);
                        }
                        if (osd_is_active(&osd)) {
                            osd_disable(fd, &osd);
                        }
                        connected = 0;
                    } else {
                        if (atomic_modeset_maxhz(fd, &cfg, cfg.osd_enable, &ms) == 0) {
                            connected = 1;
                            if (cfg.osd_enable) {
                                osd_teardown(fd, &osd);
                                osd_setup(fd, &cfg, &ms, cfg.plane_id, &osd);
                            }
                            if (ps.state != PIPELINE_STOPPED) {
                                pipeline_stop(&ps, 700);
                            }
                            if (pipeline_start(&cfg, audio_disabled, &ps) != 0) {
                                LOGE("Failed to start pipeline after hotplug");
                            }
                            clock_gettime(CLOCK_MONOTONIC, &window_start);
                            restart_count = 0;
                            backoff_ms = 0;
                        } else {
                            backoff_ms = backoff_ms == 0 ? 250 : (backoff_ms * 2);
                            if (backoff_ms > 2000) {
                                backoff_ms = 2000;
                            }
                            LOGW("Modeset failed; retry in %d ms", backoff_ms);
                            usleep(backoff_ms * 1000);
                        }
                    }
                }
            }
        }

        if (cfg.osd_enable && connected && osd_is_active(&osd)) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (ms_since(now, last_osd) >= cfg.osd_refresh_ms) {
                osd_update_stats(fd, &cfg, &ms, &ps, audio_disabled, restart_count, &osd);
                last_osd = now;
            }
        }

        if (connected && ps.state == PIPELINE_STOPPED) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long elapsed_ms = ms_since(now, window_start);
            if (elapsed_ms > cfg.restart_window_ms) {
                window_start = now;
                restart_count = 0;
            }
            restart_count++;
            if (!cfg.no_audio && cfg.audio_optional && !audio_disabled && restart_count >= cfg.restart_limit) {
                audio_disabled = 1;
                LOGW("Audio device likely busy; switching audio branch to fakesink to avoid restart loop.");
            }
            LOGW("Pipeline not running; restarting%s...", audio_disabled ? " (audio=fakesink)" : "");
            if (pipeline_start(&cfg, audio_disabled, &ps) != 0) {
                LOGE("Restart failed");
            }
        }
    }

    if (ps.state != PIPELINE_STOPPED) {
        pipeline_stop(&ps, 700);
    }
    if (osd_is_active(&osd)) {
        osd_disable(fd, &osd);
    }
    osd_teardown(fd, &osd);
    if (cfg.use_udev) {
        udev_monitor_close(&um);
    }
    close(fd);
    LOGI("Bye.");
    return 0;
}
