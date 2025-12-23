#ifndef OSD_EXTERNAL_H
#define OSD_EXTERNAL_H

#include <pthread.h>
#include <stdint.h>

#ifndef OSD_EXTERNAL_BIND_ADDR_LEN
#define OSD_EXTERNAL_BIND_ADDR_LEN 64
#endif

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
    char zoom_command[OSD_EXTERNAL_TEXT_LEN];
    OsdExternalStatus status;
} OsdExternalFeedSnapshot;

typedef struct {
    int text_active;
    int value_active;
    int is_metric;
    uint64_t text_expiry_ns;
    uint64_t value_expiry_ns;
} OsdExternalSlotState;

typedef struct {
    pthread_t thread;
    int thread_started;
    int stop_flag;
    int sock_fd;
    char bind_address[OSD_EXTERNAL_BIND_ADDR_LEN];
    int udp_port;
    pthread_mutex_t lock;
    OsdExternalFeedSnapshot snapshot;
    uint64_t expiry_ns;
    uint64_t last_error_log_ns;
    OsdExternalSlotState slots[OSD_EXTERNAL_MAX_TEXT];
} OsdExternalBridge;

void osd_external_init(OsdExternalBridge *bridge);
int osd_external_start(OsdExternalBridge *bridge, const char *bind_address, int udp_port);
void osd_external_stop(OsdExternalBridge *bridge);
void osd_external_get_snapshot(OsdExternalBridge *bridge, OsdExternalFeedSnapshot *out);
const char *osd_external_status_name(OsdExternalStatus status);

#endif // OSD_EXTERNAL_H
