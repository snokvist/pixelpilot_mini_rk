#ifndef OSD_EXTERNAL_H
#define OSD_EXTERNAL_H

#include <pthread.h>
#include <stdint.h>
#include <sys/un.h>

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX 108
#endif

#define OSD_EXTERNAL_MAX_TEXT 8
#define OSD_EXTERNAL_TEXT_LEN 64
#define OSD_EXTERNAL_MAX_VALUES 8

typedef enum {
    OSD_EXTERNAL_STATUS_DISABLED = 0,
    OSD_EXTERNAL_STATUS_LISTENING,
    OSD_EXTERNAL_STATUS_ERROR,
} OsdExternalStatus;

typedef struct {
    char text[OSD_EXTERNAL_MAX_TEXT][OSD_EXTERNAL_TEXT_LEN];
    double value[OSD_EXTERNAL_MAX_VALUES];
    uint64_t last_update_ns;
    uint64_t expiry_ns;
    OsdExternalStatus status;
} OsdExternalFeedSnapshot;

typedef struct {
    pthread_t thread;
    int thread_started;
    int stop_flag;
    int sock_fd;
    char socket_path[UNIX_PATH_MAX];
    pthread_mutex_t lock;
    OsdExternalFeedSnapshot snapshot;
    uint64_t expiry_ns;
    uint64_t last_error_log_ns;
} OsdExternalBridge;

void osd_external_init(OsdExternalBridge *bridge);
int osd_external_start(OsdExternalBridge *bridge, const char *socket_path);
void osd_external_stop(OsdExternalBridge *bridge);
void osd_external_get_snapshot(OsdExternalBridge *bridge, OsdExternalFeedSnapshot *out);
const char *osd_external_status_name(OsdExternalStatus status);

#endif // OSD_EXTERNAL_H
