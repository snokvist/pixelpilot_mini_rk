#include "logging.h"

#include <stdatomic.h>
#include <time.h>

static _Atomic int g_verbose = 0;

const char *log_timestamp(void) {
    static _Thread_local char buf[32];
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    struct tm tm;
    localtime_r(&t.tv_sec, &tm);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03ld",
             tm.tm_hour, tm.tm_min, tm.tm_sec, t.tv_nsec / 1000000);
    return buf;
}

int log_is_verbose(void) {
    return atomic_load_explicit(&g_verbose, memory_order_relaxed);
}

void log_set_verbose(int enabled) {
    atomic_store_explicit(&g_verbose, enabled ? 1 : 0, memory_order_relaxed);
}
