// SPDX-License-Identifier: MIT
// pixelpilot_mini_rk — HDMI + atomic KMS + udev hotplug + GStreamer runner + OSD

#define _GNU_SOURCE

#include "config.h"
#include "drm_modeset.h"
#include "logging.h"
#include "osd.h"
#include "pipeline.h"
#include "sse_streamer.h"
#include "udev_monitor.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

static volatile sig_atomic_t g_exit_flag = 0;
static volatile sig_atomic_t g_toggle_osd_flag = 0;
static volatile sig_atomic_t g_toggle_record_flag = 0;

static const char *g_instance_pid_path = "/tmp/pixel_pilot_rk.pid";

static void remove_instance_pid(void) {
    if (unlink(g_instance_pid_path) != 0 && errno != ENOENT) {
        LOGW("Failed to remove %s: %s", g_instance_pid_path, strerror(errno));
    }
}

static pid_t read_existing_pid(void) {
    int fd = open(g_instance_pid_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    char buf[64];
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0) {
        return -1;
    }

    buf[len] = '\0';
    char *end = NULL;
    errno = 0;
    long parsed = strtol(buf, &end, 10);
    if (errno != 0 || end == buf || parsed <= 0 || parsed > INT_MAX) {
        return -1;
    }

    return (pid_t)parsed;
}

static int write_pid_file(void) {
    int fd = open(g_instance_pid_path, O_WRONLY | O_CLOEXEC | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            return 1; // caller will handle existing instance
        }
        LOGE("Failed to create %s: %s", g_instance_pid_path, strerror(errno));
        return -1;
    }

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        LOGE("PID buffer overflow while writing %s", g_instance_pid_path);
        close(fd);
        unlink(g_instance_pid_path);
        return -1;
    }

    ssize_t written = write(fd, buf, (size_t)len);
    if (written != (ssize_t)len) {
        LOGE("Failed to write PID file %s: %s", g_instance_pid_path, strerror(errno));
        close(fd);
        unlink(g_instance_pid_path);
        return -1;
    }

    close(fd);
    atexit(remove_instance_pid);
    return 0;
}

static int ensure_single_instance(void) {
    for (;;) {
        int rc = write_pid_file();
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            return -1;
        }

        pid_t existing_pid = read_existing_pid();
        if (existing_pid > 0) {
            errno = 0;
            if (kill(existing_pid, 0) == 0 || errno == EPERM) {
                LOGE("An existing instance of pixel_pilot_rk is already running ... exiting ...");
                return -1;
            }
        }

        if (unlink(g_instance_pid_path) != 0 && errno != ENOENT) {
            LOGE("Failed to clear stale pid file %s: %s", g_instance_pid_path, strerror(errno));
            return -1;
        }
    }
}

static void on_sigint(int sig) {
    (void)sig;
    g_exit_flag = 1;
}

static void on_sigusr(int sig) {
    if (sig == SIGUSR1) {
        g_toggle_osd_flag++;
    } else if (sig == SIGUSR2) {
        g_toggle_record_flag++;
    }
}

static long long ms_since(struct timespec newer, struct timespec older) {
    return (newer.tv_sec - older.tv_sec) * 1000LL + (newer.tv_nsec - older.tv_nsec) / 1000000LL;
}

static int stats_consumers_active(const OSD *osd, const SseStreamer *sse) {
    int osd_active = (osd != NULL && osd_is_active(osd)) ? 1 : 0;
    int sse_active = sse_streamer_requires_stats(sse) ? 1 : 0;
    return osd_active || sse_active;
}

int main(int argc, char **argv) {
    AppCfg cfg;
    if (parse_cli(argc, argv, &cfg) != 0) {
        return 2;
    }

    if (ensure_single_instance() < 0) {
        return 1;
    }

    if (cfg_has_cpu_affinity(&cfg)) {
        cpu_set_t mask;
        cfg_get_process_affinity(&cfg, &mask);
        if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
            LOGW("sched_setaffinity failed: %s", strerror(errno));
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGUSR1, on_sigusr);
    signal(SIGUSR2, on_sigusr);

    int fd = open(cfg.card_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOGE("open %s: %s", cfg.card_path, strerror(errno));
        return 1;
    }

    int audio_disabled = cfg.no_audio ? 1 : 0;
    int restart_count = 0;
    struct timespec window_start = {0, 0};

    ModesetResult ms = {0};
    PipelineState ps = {0};
    ps.state = PIPELINE_STOPPED;
    UdevMonitor um = {0};
    OSD osd;
    osd_init(&osd);
    SseStreamer sse_streamer;
    sse_streamer_init(&sse_streamer);
    if (cfg.sse.enable) {
        if (sse_streamer_start(&sse_streamer, &cfg) != 0) {
            LOGW("Failed to start SSE streamer; continuing without SSE output");
        }
    }

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
                if (osd_setup(fd, &cfg, &ms, cfg.plane_id, &osd) == 0 && osd_is_active(&osd)) {
                    pipeline_set_receiver_stats_enabled(&ps, stats_consumers_active(&osd, &sse_streamer));
                } else {
                    pipeline_set_receiver_stats_enabled(&ps, stats_consumers_active(&osd, &sse_streamer));
                }
            } else {
                pipeline_set_receiver_stats_enabled(&ps, stats_consumers_active(&osd, &sse_streamer));
            }
            if (pipeline_start(&cfg, &ms, fd, audio_disabled, &ps) != 0) {
                LOGE("Failed to start pipeline");
                pipeline_set_receiver_stats_enabled(&ps, stats_consumers_active(&osd, &sse_streamer));
            } else {
                pipeline_set_receiver_stats_enabled(&ps, stats_consumers_active(&osd, &sse_streamer));
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
                            pipeline_set_receiver_stats_enabled(&ps, FALSE);
                            osd_disable(fd, &osd);
                        }
                        connected = 0;
                    } else {
                        if (atomic_modeset_maxhz(fd, &cfg, cfg.osd_enable, &ms) == 0) {
                            connected = 1;
                            if (cfg.osd_enable) {
                                pipeline_set_receiver_stats_enabled(&ps, FALSE);
                                osd_teardown(fd, &osd);
                                if (osd_setup(fd, &cfg, &ms, cfg.plane_id, &osd) == 0 && osd_is_active(&osd)) {
                                    pipeline_set_receiver_stats_enabled(&ps, TRUE);
                                } else {
                                    pipeline_set_receiver_stats_enabled(&ps, FALSE);
                                }
                            }
                            if (ps.state != PIPELINE_STOPPED) {
                                pipeline_stop(&ps, 700);
                            }
                            if (pipeline_start(&cfg, &ms, fd, audio_disabled, &ps) != 0) {
                                LOGE("Failed to start pipeline after hotplug");
                                pipeline_set_receiver_stats_enabled(&ps, FALSE);
                            } else {
                                pipeline_set_receiver_stats_enabled(&ps, osd_is_active(&osd) ? TRUE : FALSE);
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
                            pipeline_set_receiver_stats_enabled(&ps, FALSE);
                        }
                    }
                }
            }
        }

        if (g_toggle_osd_flag) {
            sig_atomic_t toggles = g_toggle_osd_flag;
            g_toggle_osd_flag = 0;
            for (sig_atomic_t i = 0; i < toggles; ++i) {
                cfg.osd_enable = cfg.osd_enable ? 0 : 1;
                if (!cfg.osd_enable) {
                    LOGI("OSD toggle: disabling overlay");
                    pipeline_set_receiver_stats_enabled(&ps, FALSE);
                    if (osd_is_active(&osd)) {
                        osd_disable(fd, &osd);
                    }
                } else {
                    LOGI("OSD toggle: enabling overlay");
                    if (!connected) {
                        LOGI("OSD toggle requested but no display is connected; will enable when possible.");
                        continue;
                    }
                    if (!osd_is_enabled(&osd)) {
                        osd_teardown(fd, &osd);
                        if (osd_setup(fd, &cfg, &ms, cfg.plane_id, &osd) == 0 && osd_is_active(&osd)) {
                            pipeline_set_receiver_stats_enabled(&ps, stats_consumers_active(&osd, &sse_streamer));
                            clock_gettime(CLOCK_MONOTONIC, &last_osd);
                        } else {
                            pipeline_set_receiver_stats_enabled(&ps, stats_consumers_active(&osd, &sse_streamer));
                            LOGW("OSD toggle: setup failed; overlay remains disabled.");
                        }
                    } else if (!osd_is_active(&osd)) {
                        if (osd_enable(fd, &osd) == 0) {
                            pipeline_set_receiver_stats_enabled(&ps, stats_consumers_active(&osd, &sse_streamer));
                            clock_gettime(CLOCK_MONOTONIC, &last_osd);
                        } else {
                            pipeline_set_receiver_stats_enabled(&ps, stats_consumers_active(&osd, &sse_streamer));
                            LOGW("OSD toggle: enable failed; overlay remains disabled.");
                        }
                    } else {
                        pipeline_set_receiver_stats_enabled(&ps, stats_consumers_active(&osd, &sse_streamer));
                    }
                }
            }
        }

        if (g_toggle_record_flag) {
            sig_atomic_t toggles = g_toggle_record_flag;
            g_toggle_record_flag = 0;
            for (sig_atomic_t i = 0; i < toggles; ++i) {
                if (!cfg.record.enable) {
                    if (cfg.record.output_path[0] == '\0') {
                        LOGW("Recording toggle: cannot enable MP4 capture because no output path is configured.");
                        continue;
                    }
                    LOGI("Recording toggle: enabling MP4 capture");
                    if (ps.state == PIPELINE_RUNNING) {
                        if (pipeline_enable_recording(&ps, &cfg.record) != 0) {
                            LOGW("Recording toggle: failed to start MP4 writer; continuing without recording.");
                            continue;
                        }
                    } else {
                        LOGI("Recording toggle: pipeline stopped; MP4 writer will start when the pipeline restarts.");
                    }
                    cfg.record.enable = 1;
                } else {
                    LOGI("Recording toggle: disabling MP4 capture");
                    if (ps.state == PIPELINE_RUNNING) {
                        pipeline_disable_recording(&ps);
                    }
                    cfg.record.enable = 0;
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

        if (sse_streamer_requires_stats(&sse_streamer)) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            static struct timespec last_sse = {0, 0};
            if (last_sse.tv_sec == 0 || ms_since(now, last_sse) >= (long long)cfg.sse.interval_ms) {
                UdpReceiverStats stats;
                int have_stats = (pipeline_get_receiver_stats(&ps, &stats) == 0);
                sse_streamer_publish(&sse_streamer, have_stats ? &stats : NULL, have_stats ? TRUE : FALSE);
                last_sse = now;
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
            if (pipeline_start(&cfg, &ms, fd, audio_disabled, &ps) != 0) {
                LOGE("Restart failed");
                pipeline_set_receiver_stats_enabled(&ps, stats_consumers_active(&osd, &sse_streamer));
            } else {
                pipeline_set_receiver_stats_enabled(&ps, stats_consumers_active(&osd, &sse_streamer));
            }
        }
    }

    if (ps.state != PIPELINE_STOPPED) {
        pipeline_stop(&ps, 700);
    }
    if (osd_is_active(&osd)) {
        pipeline_set_receiver_stats_enabled(&ps, FALSE);
        osd_disable(fd, &osd);
    }
    pipeline_set_receiver_stats_enabled(&ps, FALSE);
    osd_teardown(fd, &osd);
    if (cfg.use_udev) {
        udev_monitor_close(&um);
    }
    close(fd);
    sse_streamer_publish(&sse_streamer, NULL, FALSE);
    sse_streamer_stop(&sse_streamer);
    LOGI("Bye.");
    return 0;
}
