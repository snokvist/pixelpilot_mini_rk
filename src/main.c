// SPDX-License-Identifier: MIT
// pixelpilot_mini_rk — HDMI + atomic KMS + udev hotplug + GStreamer runner + OSD

#define _GNU_SOURCE

#include "config.h"
#include "drm_modeset.h"
#include "logging.h"
#include "osd.h"
#include "osd_external.h"
#include "pipeline.h"
#include "sse_streamer.h"
#include "udev_monitor.h"

#include <glib.h>
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
static volatile sig_atomic_t g_reinit_flag = 0;

static const char *g_instance_pid_path = "/tmp/pixelpilot_mini_rk.pid";

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

static gboolean parse_zoom_command(const char *cmd, gboolean *out_enabled, VideoDecoderZoomRequest *out_request) {
    if (out_enabled == NULL || out_request == NULL) {
        return FALSE;
    }
    if (cmd == NULL) {
        return FALSE;
    }

    const char *p = cmd;
    while (*p != '\0' && g_ascii_isspace(*p)) {
        ++p;
    }
    if (*p == '\0') {
        *out_enabled = FALSE;
        memset(out_request, 0, sizeof(*out_request));
        return TRUE;
    }
    if (g_ascii_strncasecmp(p, "zoom=", 5) == 0) {
        p += 5;
    }
    while (*p != '\0' && g_ascii_isspace(*p)) {
        ++p;
    }
    if (*p == '\0') {
        return FALSE;
    }
    if (g_ascii_strcasecmp(p, "off") == 0) {
        *out_enabled = FALSE;
        memset(out_request, 0, sizeof(*out_request));
        return TRUE;
    }

    char buf[OSD_EXTERNAL_TEXT_LEN];
    g_strlcpy(buf, p, sizeof(buf));
    GStrv tokens = g_strsplit(buf, ",", 0);
    guint32 values[4] = {0};
    int idx = 0;
    gboolean ok = TRUE;

    for (char **it = tokens; ok && it != NULL && *it != NULL; ++it) {
        if (idx >= 4) {
            ok = FALSE;
            break;
        }
        char *token = g_strstrip(*it);
        if (token[0] == '\0') {
            ok = FALSE;
            break;
        }
        errno = 0;
        char *end = NULL;
        long long v = g_ascii_strtoll(token, &end, 10);
        if (errno != 0 || end == token || *end != '\0' || v < 0 || v > G_MAXUINT32) {
            ok = FALSE;
            break;
        }
        values[idx++] = (guint32)v;
    }
    g_strfreev(tokens);

    if (!ok || idx != 4) {
        return FALSE;
    }

    if (values[0] == 0 || values[1] == 0) {
        return FALSE;
    }

    out_request->scale_x_percent = values[0];
    out_request->scale_y_percent = values[1];
    out_request->center_x_percent = values[2];
    out_request->center_y_percent = values[3];
    *out_enabled = TRUE;
    return TRUE;
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
                LOGE("An existing instance of pixelpilot_mini_rk is already running ... exiting ...");
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

static void on_sighup(int sig) {
    (void)sig;
    g_reinit_flag++;
}

static long long ms_since(struct timespec newer, struct timespec older) {
    return (newer.tv_sec - older.tv_sec) * 1000LL + (newer.tv_nsec - older.tv_nsec) / 1000000LL;
}

static gboolean modeset_result_equals(const ModesetResult *a, const ModesetResult *b) {
    if (a == NULL || b == NULL) {
        return FALSE;
    }
    return a->connector_id == b->connector_id && a->crtc_id == b->crtc_id && a->mode_w == b->mode_w &&
           a->mode_h == b->mode_h && a->mode_hz == b->mode_hz;
}

static int stats_consumers_active(const OSD *osd, const SseStreamer *sse) {
    int osd_active = (osd != NULL && osd_is_active(osd)) ? 1 : 0;
    int sse_active = sse_streamer_requires_stats(sse) ? 1 : 0;
    return osd_active || sse_active;
}

static void stats_cache_invalidate(int *cached_state) {
    if (cached_state != NULL) {
        *cached_state = -1;
    }
}

static void pipeline_maybe_set_stats(const AppCfg *cfg, PipelineState *ps, int *cached_state, gboolean desired) {
    if (cfg != NULL && cfg->idr.enable && cfg->idr.stats_trigger) {
        desired = TRUE;
    }
    int desired_int = desired ? 1 : 0;
    if (*cached_state == desired_int) {
        return;
    }
    pipeline_set_receiver_stats_enabled(ps, desired);
    *cached_state = desired_int;
}

static void pipeline_restart_now(AppCfg *cfg,
                                 ModesetResult *ms,
                                 int fd,
                                 int *audio_disabled,
                                 PipelineState *ps,
                                 OSD *osd,
                                 SseStreamer *sse_streamer,
                                 int *stats_enabled_cached,
                                 struct timespec *window_start,
                                 int *restart_count,
                                 const char *reason) {
    if (cfg == NULL || ms == NULL || audio_disabled == NULL || ps == NULL || stats_enabled_cached == NULL) {
        return;
    }

    const char *why = (reason != NULL) ? reason : "unspecified";
    LOGW("Pipeline restart requested (%s)", why);

    if (ps->state != PIPELINE_STOPPED) {
        pipeline_stop(ps, 700);
    }

    stats_cache_invalidate(stats_enabled_cached);
    pipeline_maybe_set_stats(cfg, ps, stats_enabled_cached, stats_consumers_active(osd, sse_streamer));

    if (pipeline_start(cfg, ms, fd, *audio_disabled, ps) != 0) {
        LOGE("Failed to restart pipeline (%s)", why);
        pipeline_maybe_set_stats(cfg, ps, stats_enabled_cached, stats_consumers_active(osd, sse_streamer));
        return;
    }

    if (osd != NULL && osd_is_enabled(osd)) {
        osd_ensure_above_video(fd, (uint32_t)cfg->plane_id, osd);
    }

    stats_cache_invalidate(stats_enabled_cached);
    pipeline_maybe_set_stats(cfg, ps, stats_enabled_cached, stats_consumers_active(osd, sse_streamer));
    if (window_start != NULL) {
        clock_gettime(CLOCK_MONOTONIC, window_start);
    }
    if (restart_count != NULL) {
        *restart_count = 0;
    }
}

static void configure_pip_cfg(const AppCfg *base_cfg, AppCfg *pip_cfg) {
    if (base_cfg == NULL || pip_cfg == NULL) {
        return;
    }

    *pip_cfg = *base_cfg;
    pip_cfg->udp_port = base_cfg->pip.udp_port;
    pip_cfg->plane_id = base_cfg->pip.plane_id;
    pip_cfg->plane_format = base_cfg->pip.format;
    pip_cfg->viewport = base_cfg->pip.viewport;
    pip_cfg->strict_plane_selection = 1;
    pip_cfg->no_audio = 1;
    pip_cfg->record.enable = 0;
    pip_cfg->osd_enable = 0;
    pip_cfg->sse.enable = 0;
    pip_cfg->osd_external.enable = 0;
}

static void start_pip_pipeline(AppCfg *cfg,
                               const ModesetResult *ms,
                               int fd,
                               PipelineState *pip_ps) {
    if (cfg == NULL || ms == NULL || pip_ps == NULL || !cfg->pip.enable) {
        return;
    }
    if (pip_ps->state != PIPELINE_STOPPED) {
        return;
    }

    AppCfg pip_cfg;
    configure_pip_cfg(cfg, &pip_cfg);
    int pip_start_rc = pipeline_start(&pip_cfg, ms, fd, 1, pip_ps);
    if (pip_start_rc != 0) {
        LOGE("Failed to start PiP pipeline");
        if (pip_start_rc == -2) {
            if (cfg->pip.format == DECODER_PLANE_FORMAT_AUTO) {
                cfg->pip.format = DECODER_PLANE_FORMAT_NV12;
                LOGW("PiP fallback: auto format selected unsupported yuv420_8bit path; switching to nv12");
            } else {
                cfg->pip.enable = 0;
                LOGW("PiP disabled: requested format '%s' is not implemented on this build",
                     cfg_decoder_plane_format_name(pip_cfg.plane_format));
            }
        }
        return;
    }

    LOGI("PiP started: udp=%d plane=%d viewport=%dx%d+%d+%d",
         pip_cfg.udp_port,
         pip_cfg.plane_id,
         pip_cfg.viewport.width,
         pip_cfg.viewport.height,
         pip_cfg.viewport.x,
         pip_cfg.viewport.y);
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
    signal(SIGHUP, on_sighup);

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
    PipelineState pip_ps = {0};
    pip_ps.state = PIPELINE_STOPPED;
    int stats_enabled_cached = -1;
    UdevMonitor um = {0};
    OSD osd;
    osd_init(&osd);
    OsdExternalBridge ext_bridge;
    osd_external_init(&ext_bridge);
    SseStreamer sse_streamer;
    sse_streamer_init(&sse_streamer);
    if (cfg.sse.enable) {
        if (sse_streamer_start(&sse_streamer, &cfg) != 0) {
            LOGW("Failed to start SSE streamer; continuing without SSE output");
        }
    }

    if (cfg.osd_external.enable) {
        if (cfg.osd_external.udp_port <= 0 || cfg.osd_external.udp_port > 65535) {
            LOGW("External OSD feed enabled but invalid UDP port configured; disabling listener");
            cfg.osd_external.enable = 0;
        } else {
            const char *bind_addr = cfg.osd_external.bind_address;
            char default_bind[] = "0.0.0.0";
            if (!bind_addr || bind_addr[0] == '\0') {
                bind_addr = default_bind;
            }
            if (osd_external_start(&ext_bridge, bind_addr, cfg.osd_external.udp_port) != 0) {
                LOGW("Failed to start external OSD feed listener; continuing without external data");
            }
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
                    pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, stats_consumers_active(&osd, &sse_streamer));
                } else {
                    pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, stats_consumers_active(&osd, &sse_streamer));
                }
            } else {
                pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, stats_consumers_active(&osd, &sse_streamer));
            }
            stats_cache_invalidate(&stats_enabled_cached);
            if (pipeline_start(&cfg, &ms, fd, audio_disabled, &ps) != 0) {
                LOGE("Failed to start pipeline");
                pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, stats_consumers_active(&osd, &sse_streamer));
            } else {
                if (osd_is_enabled(&osd)) {
                    osd_ensure_above_video(fd, (uint32_t)cfg.plane_id, &osd);
                }
                pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, stats_consumers_active(&osd, &sse_streamer));
            }
            start_pip_pipeline(&cfg, &ms, fd, &pip_ps);
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
    char last_zoom_command[OSD_EXTERNAL_TEXT_LEN] = "";
    struct timespec last_pip_retry = {0, 0};

    while (!g_exit_flag) {
        pipeline_poll_child(&ps);
        pipeline_poll_child(&pip_ps);

        const char *pending_restart_reason = NULL;
        if (pipeline_consume_reinit_request(&ps) || pipeline_consume_reinit_request(&pip_ps)) {
            pending_restart_reason = "IDR recovery loop";
        }
        if (g_reinit_flag > 0) {
            g_reinit_flag = 0;
            if (pending_restart_reason == NULL) {
                pending_restart_reason = "SIGHUP";
            } else {
                LOGW("SIGHUP received while a restart is already pending; combining requests");
            }
        }
        if (pending_restart_reason != NULL) {
            if (connected) {
                pipeline_restart_now(&cfg,
                                     &ms,
                                     fd,
                                     &audio_disabled,
                                     &ps,
                                     &osd,
                                     &sse_streamer,
                                     &stats_enabled_cached,
                                     &window_start,
                                     &restart_count,
                                     pending_restart_reason);
                if (cfg.pip.enable) {
                    if (pip_ps.state != PIPELINE_STOPPED) {
                        pipeline_stop(&pip_ps, 700);
                    }
                    start_pip_pipeline(&cfg, &ms, fd, &pip_ps);
                }
                backoff_ms = 0;
            } else {
                LOGW("Pipeline restart requested (%s) but no display is connected; ignoring.",
                     pending_restart_reason);
            }
        }

        struct pollfd pfds[2];
        int nfds = 0;
        int ufd = cfg.use_udev ? um.fd : -1;
        if (ufd >= 0) {
            pfds[nfds++] = (struct pollfd){.fd = ufd, .events = POLLIN};
        }
        pfds[nfds++] = (struct pollfd){.fd = STDIN_FILENO, .events = 0};

        int poll_timeout_ms = 200;
        if (cfg.osd_enable && osd_is_active(&osd)) {
            int hint_ms = osd_refresh_hint_ms(&osd, cfg.osd_refresh_ms);
            if (hint_ms > 0 && hint_ms < poll_timeout_ms) {
                poll_timeout_ms = hint_ms;
            }
            if (poll_timeout_ms < 1) {
                poll_timeout_ms = 1;
            }
        }
        (void)poll(pfds, nfds, poll_timeout_ms);

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
                            stats_cache_invalidate(&stats_enabled_cached);
                        }
                        if (pip_ps.state != PIPELINE_STOPPED) {
                            pipeline_stop(&pip_ps, 700);
                        }
                        if (osd_is_active(&osd)) {
                            pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, FALSE);
                            osd_disable(fd, &osd);
                        }
                        connected = 0;
                        memset(&ms, 0, sizeof(ms));
                    } else {
                        ModesetResult probed = {0};
                        gboolean probe_ok = (probe_maxhz_mode(fd, &cfg, &probed) == 0);
                        gboolean needs_modeset = TRUE;
                        if (probe_ok && modeset_result_equals(&ms, &probed) && ps.state == PIPELINE_RUNNING) {
                            LOGI("Hotplug: display unchanged; skipping reinitialization");
                            connected = 1;
                            needs_modeset = FALSE;
                        }

                        if (needs_modeset && atomic_modeset_maxhz(fd, &cfg, cfg.osd_enable, &ms) == 0) {
                            connected = 1;
                            if (cfg.osd_enable) {
                                pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, FALSE);
                                osd_teardown(fd, &osd);
                                if (osd_setup(fd, &cfg, &ms, cfg.plane_id, &osd) == 0 && osd_is_active(&osd)) {
                                    pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, TRUE);
                                } else {
                                    pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, FALSE);
                                }
                            }
                            if (ps.state != PIPELINE_STOPPED) {
                                pipeline_stop(&ps, 700);
                                stats_cache_invalidate(&stats_enabled_cached);
                            }
                            stats_cache_invalidate(&stats_enabled_cached);
                            if (pipeline_start(&cfg, &ms, fd, audio_disabled, &ps) != 0) {
                                LOGE("Failed to start pipeline after hotplug");
                                pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, FALSE);
                            } else {
                                if (osd_is_enabled(&osd)) {
                                    osd_ensure_above_video(fd, (uint32_t)cfg.plane_id, &osd);
                                }
                                pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, osd_is_active(&osd) ? TRUE : FALSE);
                            }
                            start_pip_pipeline(&cfg, &ms, fd, &pip_ps);
                            clock_gettime(CLOCK_MONOTONIC, &window_start);
                            restart_count = 0;
                            backoff_ms = 0;
                        } else if (needs_modeset) {
                            backoff_ms = backoff_ms == 0 ? 250 : (backoff_ms * 2);
                            if (backoff_ms > 2000) {
                                backoff_ms = 2000;
                            }
                            LOGW("Modeset failed; retry in %d ms", backoff_ms);
                            usleep(backoff_ms * 1000);
                            pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, FALSE);
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
                    pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, FALSE);
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
                            pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, stats_consumers_active(&osd, &sse_streamer));
                            clock_gettime(CLOCK_MONOTONIC, &last_osd);
                        } else {
                            pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, stats_consumers_active(&osd, &sse_streamer));
                            LOGW("OSD toggle: setup failed; overlay remains disabled.");
                        }
                    } else if (!osd_is_active(&osd)) {
                        if (osd_enable(fd, &osd) == 0) {
                            pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, stats_consumers_active(&osd, &sse_streamer));
                            clock_gettime(CLOCK_MONOTONIC, &last_osd);
                        } else {
                            pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, stats_consumers_active(&osd, &sse_streamer));
                            LOGW("OSD toggle: enable failed; overlay remains disabled.");
                        }
                    } else {
                        pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, stats_consumers_active(&osd, &sse_streamer));
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
            int refresh_hint_ms = osd_refresh_hint_ms(&osd, cfg.osd_refresh_ms);
            if (ms_since(now, last_osd) >= refresh_hint_ms) {
                OsdExternalFeedSnapshot ext_snapshot;
                osd_external_get_snapshot(&ext_bridge, &ext_snapshot);
                const char *zoom_text = ext_snapshot.zoom_command;
                if (g_strcmp0(zoom_text, last_zoom_command) != 0) {
                    gboolean zoom_enabled = FALSE;
                    VideoDecoderZoomRequest zoom_request = {0};
                    if (parse_zoom_command(zoom_text, &zoom_enabled, &zoom_request)) {
                        if (zoom_enabled) {
                            pipeline_apply_zoom_command(&ps, TRUE, &zoom_request);
                        } else {
                            pipeline_apply_zoom_command(&ps, FALSE, NULL);
                        }
                    } else if (zoom_text != NULL && zoom_text[0] != '\0') {
                        LOGW("External zoom command ignored: expected 'zoom=SCALE_X,SCALE_Y,CENTER_X,CENTER_Y' or 'zoom=off' (got '%s')",
                             zoom_text);
                    } else if (last_zoom_command[0] != '\0') {
                        pipeline_apply_zoom_command(&ps, FALSE, NULL);
                    }
                    g_strlcpy(last_zoom_command, zoom_text != NULL ? zoom_text : "", sizeof(last_zoom_command));
                }
                int updated = osd_update_stats(fd, &cfg, &ms, &ps, audio_disabled, restart_count, &ext_snapshot, &now, &osd);
                if (updated) {
                    last_osd = now;
                }
            }
        }

        if (sse_streamer_requires_stats(&sse_streamer)) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            static struct timespec last_sse = {0, 0};
            if (last_sse.tv_sec == 0 || ms_since(now, last_sse) >= (long long)cfg.sse.interval_ms) {
                UdpReceiverStats stats;
                PipelineRecordingStats rec_stats;
                int have_stats = (pipeline_get_receiver_stats(&ps, &stats) == 0);
                if (pipeline_get_recording_stats(&ps, &rec_stats) != 0) {
                    memset(&rec_stats, 0, sizeof(rec_stats));
                }
                sse_streamer_publish(&sse_streamer, have_stats ? &stats : NULL, have_stats ? TRUE : FALSE,
                                     cfg.record.enable ? TRUE : FALSE, &rec_stats);
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
            stats_cache_invalidate(&stats_enabled_cached);
            if (pipeline_start(&cfg, &ms, fd, audio_disabled, &ps) != 0) {
                LOGE("Restart failed");
                pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, stats_consumers_active(&osd, &sse_streamer));
            } else {
                pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, stats_consumers_active(&osd, &sse_streamer));
            }
        }

        if (connected && cfg.pip.enable && ps.state == PIPELINE_RUNNING && pip_ps.state == PIPELINE_STOPPED) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (last_pip_retry.tv_sec == 0 || ms_since(now, last_pip_retry) >= 2000) {
                LOGW("PiP pipeline not running; retrying start on configured plane");
                start_pip_pipeline(&cfg, &ms, fd, &pip_ps);
                last_pip_retry = now;
            }
        }
    }

    if (ps.state != PIPELINE_STOPPED) {
        pipeline_stop(&ps, 700);
        stats_cache_invalidate(&stats_enabled_cached);
    }
    if (pip_ps.state != PIPELINE_STOPPED) {
        pipeline_stop(&pip_ps, 700);
    }
    if (osd_is_active(&osd)) {
        pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, FALSE);
        osd_disable(fd, &osd);
    }
    pipeline_maybe_set_stats(&cfg, &ps, &stats_enabled_cached, FALSE);
    osd_teardown(fd, &osd);
    if (cfg.use_udev) {
        udev_monitor_close(&um);
    }
    osd_external_stop(&ext_bridge);
    close(fd);
    PipelineRecordingStats rec_stats = {0};
    sse_streamer_publish(&sse_streamer, NULL, FALSE, cfg.record.enable ? TRUE : FALSE, &rec_stats);
    sse_streamer_stop(&sse_streamer);
    LOGI("Bye.");
    return 0;
}
