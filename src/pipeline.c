#include "pipeline.h"
#include "logging.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void build_gst_cmd(const AppCfg *cfg, int audio_disabled, char *out, size_t outsz) {
    const char *audio_branch;
    char audio_real[512];

    if (cfg->no_audio) {
        audio_branch = "";
    } else if (audio_disabled) {
        audio_branch = "t. ! queue leaky=downstream max-size-time=0 max-size-bytes=0 ! fakesink sync=false ";
    } else {
        snprintf(audio_real, sizeof(audio_real),
                 "t. ! queue leaky=downstream max-size-time=0 max-size-bytes=0 ! "
                 "application/x-rtp,payload=%d,clock-rate=48000,encoding-name=OPUS ! "
                 "rtpjitterbuffer latency=%d drop-on-latency=true do-lost=true ! "
                 "rtpopusdepay ! opusdec ! audioconvert ! audioresample ! "
                 "audio/x-raw,format=S16LE,rate=48000,channels=2 ! "
                 "queue leaky=downstream ! "
                 "alsasink device=%s sync=false ",
                 cfg->aud_pt, cfg->latency_ms, cfg->aud_dev);
        audio_branch = audio_real;
    }

    snprintf(out, outsz,
             "gst-launch-1.0 -v "
             "udpsrc port=%d buffer-size=262144 ! tee name=t "
             "t. ! queue leaky=downstream max-size-buffers=96 max-size-time=0 max-size-bytes=0 ! "
             "application/x-rtp,payload=%d,clock-rate=90000,encoding-name=H265 ! "
             "rtpjitterbuffer latency=%d drop-on-latency=true do-lost=true post-drop-messages=true ! "
             "rtph265depay ! h265parse config-interval=-1 disable-passthrough=true ! "
             "video/x-h265,stream-format=byte-stream,alignment=au ! "
             "queue leaky=downstream max-size-buffers=8 max-size-time=0 max-size-bytes=0 ! "
             "mppvideodec ! queue leaky=downstream max-size-buffers=8 ! "
             "kmssink plane-id=%d sync=%s qos=%s max-lateness=%d "
             "%s",
             cfg->udp_port, cfg->vid_pt, cfg->latency_ms, cfg->plane_id,
             cfg->kmssink_sync ? "true" : "false",
             cfg->kmssink_qos ? "true" : "false",
             cfg->max_lateness_ns,
             audio_branch);
}

int pipeline_start(const AppCfg *cfg, int audio_disabled, PipelineState *ps) {
    if (ps->state != PIPELINE_STOPPED && ps->pid > 0) {
        LOGW("pipeline_start: refused (state=%d pid=%d)", ps->state, ps->pid);
        return -1;
    }
    char cmd[2500];
    build_gst_cmd(cfg, audio_disabled, cmd, sizeof(cmd));
    LOGI("Starting pipeline: %s", cmd);

    pid_t pid = fork();
    if (pid < 0) {
        LOGE("fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        if (cfg->gst_log && getenv("GST_DEBUG") == NULL) {
            setenv("GST_DEBUG", "3", 1);
        }
        prctl(PR_SET_PDEATHSIG, SIGHUP);
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    ps->pid = pid;
    ps->pgid = pid;
    ps->state = PIPELINE_RUNNING;
    return 0;
}

void pipeline_stop(PipelineState *ps, int wait_ms_total) {
    if (ps->pid <= 0) {
        ps->state = PIPELINE_STOPPED;
        ps->pgid = 0;
        return;
    }
    if (ps->state == PIPELINE_STOPPING) {
        return;
    }
    ps->state = PIPELINE_STOPPING;
    LOGI("Stopping pipeline pid=%d pgid=%d", ps->pid, ps->pgid);
    if (ps->pgid > 0) {
        killpg(ps->pgid, SIGINT);
    } else {
        kill(ps->pid, SIGINT);
    }
    int waited = 0;
    while (waited < wait_ms_total) {
        int status;
        pid_t r = waitpid(ps->pid, &status, WNOHANG);
        if (r == ps->pid) {
            ps->pid = 0;
            ps->pgid = 0;
            ps->state = PIPELINE_STOPPED;
            return;
        }
        usleep(50 * 1000);
        waited += 50;
    }
    LOGW("Pipeline didn’t exit in time, SIGKILL group");
    if (ps->pgid > 0) {
        killpg(ps->pgid, SIGKILL);
    } else {
        kill(ps->pid, SIGKILL);
    }
    int status;
    (void)waitpid(ps->pid, &status, 0);
    ps->pid = 0;
    ps->pgid = 0;
    ps->state = PIPELINE_STOPPED;
}

void pipeline_poll_child(PipelineState *ps) {
    if (ps->pid <= 0) {
        return;
    }
    int status = 0;
    pid_t r = waitpid(ps->pid, &status, WNOHANG);
    if (r == ps->pid) {
        LOGI("Pipeline exited (status=0x%x)", status);
        ps->pid = 0;
        ps->pgid = 0;
        ps->state = PIPELINE_STOPPED;
    }
}
