#ifndef PIPELINE_H
#define PIPELINE_H

#include <sys/types.h>

typedef enum {
    PIPELINE_STOPPED = 0,
    PIPELINE_RUNNING = 1,
    PIPELINE_STOPPING = 2
} PipelineStateEnum;

typedef struct {
    pid_t pid;
    pid_t pgid;
    PipelineStateEnum state;
} PipelineState;

#include "config.h"

int pipeline_start(const AppCfg *cfg, int audio_disabled, PipelineState *ps);
void pipeline_stop(PipelineState *ps, int wait_ms_total);
void pipeline_poll_child(PipelineState *ps);

#endif // PIPELINE_H
