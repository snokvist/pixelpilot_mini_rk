#include "osd_external.h"
#include "logging.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

static uint64_t timespec_to_ns(const struct timespec *ts) {
    if (!ts) {
        return 0;
    }
    return (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return timespec_to_ns(&ts);
}

static void osd_external_reset_locked(OsdExternalBridge *bridge) {
    if (!bridge) {
        return;
    }
    memset(bridge->snapshot.text, 0, sizeof(bridge->snapshot.text));
    for (size_t i = 0; i < ARRAY_SIZE(bridge->snapshot.value); ++i) {
        bridge->snapshot.value[i] = 0.0;
    }
    bridge->snapshot.last_update_ns = 0;
    bridge->snapshot.expiry_ns = 0;
}

static void osd_external_expire_locked(OsdExternalBridge *bridge, uint64_t now_ns) {
    if (!bridge) {
        return;
    }
    if (bridge->expiry_ns > 0 && now_ns >= bridge->expiry_ns) {
        osd_external_reset_locked(bridge);
        bridge->expiry_ns = 0;
    }
    bridge->snapshot.expiry_ns = bridge->expiry_ns;
}

static int should_log_error(OsdExternalBridge *bridge, uint64_t now_ns) {
    if (!bridge) {
        return 0;
    }
    const uint64_t interval_ns = 2000000000ull; // 2 seconds
    if (bridge->last_error_log_ns == 0 || now_ns - bridge->last_error_log_ns >= interval_ns) {
        bridge->last_error_log_ns = now_ns;
        return 1;
    }
    return 0;
}

typedef struct {
    int has_text;
    int text_count;
    char text[OSD_EXTERNAL_MAX_TEXT][OSD_EXTERNAL_TEXT_LEN];
    int has_value;
    int value_count;
    double value[OSD_EXTERNAL_MAX_VALUES];
    int has_ttl;
    uint64_t ttl_ms;
} OsdExternalMessage;

static const char *skip_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) {
        ++p;
    }
    return p;
}

static const char *parse_string(const char *p, char *out, size_t out_sz) {
    if (!p || *p != '"') {
        return NULL;
    }
    ++p;
    size_t idx = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1] != '\0') {
            ++p;
        }
        if (idx + 1 < out_sz) {
            out[idx++] = *p;
        }
        ++p;
    }
    if (*p != '"') {
        return NULL;
    }
    if (out && out_sz > 0) {
        out[idx] = '\0';
    }
    return p + 1;
}

static const char *parse_string_array(const char *p, OsdExternalMessage *msg) {
    if (!p || *p != '[') {
        return NULL;
    }
    ++p;
    p = skip_ws(p);
    int idx = 0;
    if (*p == ']') {
        msg->text_count = 0;
        return p + 1;
    }
    while (*p) {
        p = skip_ws(p);
        if (*p != '"') {
            return NULL;
        }
        char tmp[OSD_EXTERNAL_TEXT_LEN];
        const char *next = parse_string(p, tmp, sizeof(tmp));
        if (!next) {
            return NULL;
        }
        if (idx < OSD_EXTERNAL_MAX_TEXT) {
            strncpy(msg->text[idx], tmp, sizeof(msg->text[idx]) - 1);
            msg->text[idx][sizeof(msg->text[idx]) - 1] = '\0';
            idx++;
        }
        p = skip_ws(next);
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == ']') {
            break;
        }
        return NULL;
    }
    if (*p != ']') {
        return NULL;
    }
    msg->text_count = idx;
    return p + 1;
}

static const char *parse_number_array(const char *p, OsdExternalMessage *msg) {
    if (!p || *p != '[') {
        return NULL;
    }
    ++p;
    p = skip_ws(p);
    int idx = 0;
    if (*p == ']') {
        msg->value_count = 0;
        return p + 1;
    }
    while (*p) {
        char *end = NULL;
        double v = strtod(p, &end);
        if (end == p) {
            return NULL;
        }
        if (idx < OSD_EXTERNAL_MAX_VALUES) {
            msg->value[idx++] = v;
        }
        p = skip_ws(end);
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == ']') {
            break;
        }
        return NULL;
    }
    if (*p != ']') {
        return NULL;
    }
    msg->value_count = idx;
    return p + 1;
}

static int parse_message(const char *payload, OsdExternalMessage *msg) {
    if (!payload || !msg) {
        return -1;
    }
    memset(msg, 0, sizeof(*msg));
    const char *p = skip_ws(payload);
    if (*p != '{') {
        return -1;
    }
    ++p;
    while (*p) {
        p = skip_ws(p);
        if (*p == '}') {
            return 0;
        }
        if (*p != '"') {
            return -1;
        }
        char key[32];
        const char *next = parse_string(p, key, sizeof(key));
        if (!next) {
            return -1;
        }
        p = skip_ws(next);
        if (*p != ':') {
            return -1;
        }
        ++p;
        p = skip_ws(p);
        if (strcmp(key, "text") == 0) {
            msg->has_text = 1;
            p = parse_string_array(p, msg);
            if (!p) {
                return -1;
            }
        } else if (strcmp(key, "value") == 0) {
            msg->has_value = 1;
            p = parse_number_array(p, msg);
            if (!p) {
                return -1;
            }
        } else if (strcmp(key, "ttl_ms") == 0) {
            char *end = NULL;
            long long ttl = strtoll(p, &end, 10);
            if (end == p) {
                return -1;
            }
            if (ttl < 0) {
                ttl = 0;
            }
            msg->has_ttl = 1;
            msg->ttl_ms = (uint64_t)ttl;
            p = end;
        } else {
            // Skip unknown value (best effort: handle nested objects/arrays by counting braces)
            int depth = 0;
            if (*p == '{') {
                depth = 1;
                ++p;
                while (*p && depth > 0) {
                    if (*p == '{') {
                        depth++;
                    } else if (*p == '}') {
                        depth--;
                    } else if (*p == '"') {
                        const char *tmp = parse_string(p, NULL, 0);
                        if (!tmp) {
                            return -1;
                        }
                        p = tmp;
                        continue;
                    }
                    ++p;
                }
                if (depth != 0) {
                    return -1;
                }
            } else if (*p == '[') {
                depth = 1;
                ++p;
                while (*p && depth > 0) {
                    if (*p == '[') {
                        depth++;
                    } else if (*p == ']') {
                        depth--;
                    } else if (*p == '"') {
                        const char *tmp = parse_string(p, NULL, 0);
                        if (!tmp) {
                            return -1;
                        }
                        p = tmp;
                        continue;
                    }
                    ++p;
                }
                if (depth != 0) {
                    return -1;
                }
            } else if (*p == '"') {
                p = parse_string(p, NULL, 0);
                if (!p) {
                    return -1;
                }
            } else {
                while (*p && *p != ',' && *p != '}') {
                    ++p;
                }
            }
        }
        p = skip_ws(p);
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == '}') {
            return 0;
        }
        return -1;
    }
    return -1;
}

static void apply_message(OsdExternalBridge *bridge, const OsdExternalMessage *msg, uint64_t now_ns) {
    if (!bridge || !msg) {
        return;
    }
    pthread_mutex_lock(&bridge->lock);
    if (msg->has_text) {
        if (msg->text_count == 0) {
            memset(bridge->snapshot.text, 0, sizeof(bridge->snapshot.text));
        } else {
            for (int i = 0; i < msg->text_count && i < OSD_EXTERNAL_MAX_TEXT; ++i) {
                strncpy(bridge->snapshot.text[i], msg->text[i], sizeof(bridge->snapshot.text[i]) - 1);
                bridge->snapshot.text[i][sizeof(bridge->snapshot.text[i]) - 1] = '\0';
            }
        }
    }
    if (msg->has_value) {
        if (msg->value_count == 0) {
            for (int i = 0; i < OSD_EXTERNAL_MAX_VALUES; ++i) {
                bridge->snapshot.value[i] = 0.0;
            }
        } else {
            for (int i = 0; i < msg->value_count && i < OSD_EXTERNAL_MAX_VALUES; ++i) {
                bridge->snapshot.value[i] = msg->value[i];
            }
        }
    }
    bridge->snapshot.last_update_ns = now_ns;
    if (msg->has_ttl && msg->ttl_ms > 0) {
        uint64_t ttl_ns = msg->ttl_ms * 1000000ull;
        if (ttl_ns / 1000000ull != msg->ttl_ms) {
            ttl_ns = 0;
        }
        if (ttl_ns > 0) {
            bridge->expiry_ns = now_ns + ttl_ns;
        } else {
            bridge->expiry_ns = 0;
        }
    } else if (msg->has_ttl && msg->ttl_ms == 0) {
        bridge->expiry_ns = now_ns;
    } else {
        bridge->expiry_ns = 0;
    }
    bridge->snapshot.expiry_ns = bridge->expiry_ns;
    pthread_mutex_unlock(&bridge->lock);
}

static void *osd_external_thread(void *arg) {
    OsdExternalBridge *bridge = (OsdExternalBridge *)arg;
    if (!bridge) {
        return NULL;
    }
    pthread_mutex_lock(&bridge->lock);
    bridge->status = OSD_EXTERNAL_STATUS_LISTENING;
    pthread_mutex_unlock(&bridge->lock);
    while (1) {
        uint64_t now_ns = monotonic_ns();
        pthread_mutex_lock(&bridge->lock);
        int stop = bridge->stop_flag;
        osd_external_expire_locked(bridge, now_ns);
        pthread_mutex_unlock(&bridge->lock);
        if (stop) {
            break;
        }
        struct pollfd pfd = {
            .fd = bridge->sock_fd,
            .events = POLLIN,
        };
        int rc = poll(&pfd, 1, 500);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            uint64_t err_ns = monotonic_ns();
            pthread_mutex_lock(&bridge->lock);
            if (should_log_error(bridge, err_ns)) {
                LOGW("OSD external feed: poll failed: %s", strerror(errno));
            }
            bridge->status = OSD_EXTERNAL_STATUS_ERROR;
            pthread_mutex_unlock(&bridge->lock);
            break;
        }
        if (rc == 0) {
            continue;
        }
        if (!(pfd.revents & POLLIN)) {
            continue;
        }
        char buf[2048];
        ssize_t len = recv(bridge->sock_fd, buf, sizeof(buf) - 1, 0);
        if (len < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            uint64_t err_ns = monotonic_ns();
            pthread_mutex_lock(&bridge->lock);
            if (should_log_error(bridge, err_ns)) {
                LOGW("OSD external feed: recv failed: %s", strerror(errno));
            }
            bridge->status = OSD_EXTERNAL_STATUS_ERROR;
            pthread_mutex_unlock(&bridge->lock);
            break;
        }
        buf[len] = '\0';
        OsdExternalMessage msg;
        if (parse_message(buf, &msg) != 0) {
            uint64_t err_ns = monotonic_ns();
            pthread_mutex_lock(&bridge->lock);
            if (should_log_error(bridge, err_ns)) {
                LOGW("OSD external feed: ignoring malformed payload: %s", buf);
            }
            pthread_mutex_unlock(&bridge->lock);
            continue;
        }
        apply_message(bridge, &msg, monotonic_ns());
    }
    pthread_mutex_lock(&bridge->lock);
    bridge->thread_started = 0;
    bridge->stop_flag = 0;
    bridge->status = (bridge->sock_fd >= 0) ? bridge->status : OSD_EXTERNAL_STATUS_DISABLED;
    pthread_mutex_unlock(&bridge->lock);
    return NULL;
}

void osd_external_init(OsdExternalBridge *bridge) {
    if (!bridge) {
        return;
    }
    memset(bridge, 0, sizeof(*bridge));
    bridge->sock_fd = -1;
    pthread_mutex_init(&bridge->lock, NULL);
    bridge->status = OSD_EXTERNAL_STATUS_DISABLED;
}

static void close_socket(OsdExternalBridge *bridge) {
    if (!bridge) {
        return;
    }
    if (bridge->sock_fd >= 0) {
        close(bridge->sock_fd);
        bridge->sock_fd = -1;
    }
    if (bridge->socket_path[0] != '\0') {
        unlink(bridge->socket_path);
        bridge->socket_path[0] = '\0';
    }
}

int osd_external_start(OsdExternalBridge *bridge, const char *socket_path) {
    if (!bridge) {
        return -1;
    }
    osd_external_stop(bridge);
    if (!socket_path || socket_path[0] == '\0') {
        return 0;
    }
    size_t path_len = strnlen(socket_path, UNIX_PATH_MAX - 1);
    if (path_len == 0 || path_len >= UNIX_PATH_MAX) {
        LOGW("OSD external feed: socket path too long: %s", socket_path);
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOGW("OSD external feed: socket() failed: %s", strerror(errno));
        pthread_mutex_lock(&bridge->lock);
        bridge->status = OSD_EXTERNAL_STATUS_ERROR;
        pthread_mutex_unlock(&bridge->lock);
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    unlink(addr.sun_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOGW("OSD external feed: bind(%s) failed: %s", socket_path, strerror(errno));
        close(fd);
        pthread_mutex_lock(&bridge->lock);
        bridge->status = OSD_EXTERNAL_STATUS_ERROR;
        pthread_mutex_unlock(&bridge->lock);
        return -1;
    }
    pthread_mutex_lock(&bridge->lock);
    bridge->sock_fd = fd;
    strncpy(bridge->socket_path, socket_path, sizeof(bridge->socket_path) - 1);
    bridge->socket_path[sizeof(bridge->socket_path) - 1] = '\0';
    bridge->stop_flag = 0;
    bridge->status = OSD_EXTERNAL_STATUS_LISTENING;
    pthread_mutex_unlock(&bridge->lock);
    if (pthread_create(&bridge->thread, NULL, osd_external_thread, bridge) != 0) {
        LOGW("OSD external feed: pthread_create failed: %s", strerror(errno));
        close_socket(bridge);
        pthread_mutex_lock(&bridge->lock);
        bridge->status = OSD_EXTERNAL_STATUS_ERROR;
        pthread_mutex_unlock(&bridge->lock);
        return -1;
    }
    pthread_mutex_lock(&bridge->lock);
    bridge->thread_started = 1;
    pthread_mutex_unlock(&bridge->lock);
    LOGI("OSD external feed: listening on %s", socket_path);
    return 0;
}

void osd_external_stop(OsdExternalBridge *bridge) {
    if (!bridge) {
        return;
    }
    pthread_mutex_lock(&bridge->lock);
    int running = bridge->thread_started;
    if (running) {
        bridge->stop_flag = 1;
    }
    pthread_mutex_unlock(&bridge->lock);
    if (running) {
        pthread_join(bridge->thread, NULL);
    }
    close_socket(bridge);
    pthread_mutex_lock(&bridge->lock);
    bridge->status = OSD_EXTERNAL_STATUS_DISABLED;
    bridge->thread_started = 0;
    bridge->stop_flag = 0;
    bridge->expiry_ns = 0;
    bridge->last_error_log_ns = 0;
    osd_external_reset_locked(bridge);
    pthread_mutex_unlock(&bridge->lock);
}

void osd_external_get_snapshot(OsdExternalBridge *bridge, OsdExternalFeedSnapshot *out) {
    if (!bridge || !out) {
        return;
    }
    uint64_t now_ns = monotonic_ns();
    pthread_mutex_lock(&bridge->lock);
    osd_external_expire_locked(bridge, now_ns);
    *out = bridge->snapshot;
    out->status = bridge->status;
    pthread_mutex_unlock(&bridge->lock);
}

const char *osd_external_status_name(OsdExternalStatus status) {
    switch (status) {
    case OSD_EXTERNAL_STATUS_DISABLED:
        return "disabled";
    case OSD_EXTERNAL_STATUS_LISTENING:
        return "listening";
    case OSD_EXTERNAL_STATUS_ERROR:
        return "error";
    default:
        return "unknown";
    }
}
