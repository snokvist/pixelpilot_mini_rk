#include "idr_requester.h"

#include "logging.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define IDR_HOST_MAX INET_ADDRSTRLEN
#define IDR_PATH_MAX 128
#define IDR_BURST_COUNT 1u
#define IDR_BURST_INTERVAL_MS 50u
#define IDR_MAX_INTERVAL_MS 500u
#define IDR_QUIET_RESET_MS 750u
#define IDR_REINIT_THRESHOLD 64u

typedef struct {
    IdrRequester *owner;
    struct sockaddr_in addr;
    char host[IDR_HOST_MAX];
    char path[IDR_PATH_MAX];
    guint timeout_ms;
} IdrHttpTask;

struct IdrRequester {
    GMutex lock;
    GCond cond;
    gboolean cond_initialized;

    gboolean enabled;
    gboolean have_source;
    struct in_addr source_addr;
    char source_host[IDR_HOST_MAX];

    guint16 http_port;
    guint http_timeout_ms;
    char http_path[IDR_PATH_MAX];

    guint64 last_warning_ms;
    guint64 last_request_ms;
    guint next_interval_ms;
    guint attempt_count;
    gboolean active;

    gboolean request_in_flight;
    gboolean shutting_down;

    guint64 total_requests;

    IdrReinitCallback reinit_cb;
    gpointer reinit_user_data;
    gboolean reinit_pending;
};

static guint64 monotonic_ms(void) {
    return (guint64)g_get_monotonic_time() / 1000ull;
}

static void sanitize_path(const char *src, char *dst, size_t dst_sz) {
    const char *fallback = "/request/idr";
    if (dst == NULL || dst_sz == 0) {
        return;
    }
    if (src == NULL || src[0] == '\0') {
        src = fallback;
    }
    if (src[0] == '/') {
        g_strlcpy(dst, src, dst_sz);
        return;
    }
    if (dst_sz == 1) {
        dst[0] = '\0';
        return;
    }
    dst[0] = '/';
    g_strlcpy(dst + 1, src, dst_sz - 1);
}

static gboolean configure_timeout(int fd, guint timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000u;
    tv.tv_usec = (timeout_ms % 1000u) * 1000u;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        return FALSE;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return FALSE;
    }
    return TRUE;
}

static gboolean send_http_request(const IdrHttpTask *task) {
    if (task == NULL) {
        return FALSE;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOGW("IDR requester: failed to create TCP socket: %s", g_strerror(errno));
        return FALSE;
    }

    gboolean success = FALSE;

    do {
        if (!configure_timeout(fd, task->timeout_ms)) {
            LOGW("IDR requester: failed to configure socket timeouts: %s", g_strerror(errno));
            break;
        }

        if (connect(fd, (const struct sockaddr *)&task->addr, sizeof(task->addr)) != 0) {
            LOGW("IDR requester: connect to %s:%u failed: %s",
                 task->host,
                 (unsigned int)ntohs(task->addr.sin_port),
                 g_strerror(errno));
            break;
        }

        char request[256];
        int len = g_snprintf(request,
                              sizeof(request),
                              "GET %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "User-Agent: pixelpilot-idr/1.0\r\n"
                              "Accept: */*\r\n"
                              "Connection: close\r\n\r\n",
                              task->path,
                              task->host);
        if (len <= 0 || (size_t)len >= sizeof(request)) {
            LOGW("IDR requester: failed to compose HTTP request");
            break;
        }

        size_t total = (size_t)len;
        size_t sent = 0;
        while (sent < total) {
            ssize_t n = send(fd, request + sent, total - sent, 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LOGW("IDR requester: send failed: %s", g_strerror(errno));
                goto out;
            }
            sent += (size_t)n;
        }

        success = TRUE;

        char response[256];
        ssize_t nread = recv(fd, response, sizeof(response) - 1, 0);
        if (nread > 0) {
            response[nread] = '\0';
            char *line_end = strchr(response, '\n');
            if (line_end != NULL) {
                *line_end = '\0';
            }
            char *line = response;
            while (*line && g_ascii_isspace(*line)) {
                ++line;
            }
            if (g_str_has_prefix(line, "HTTP/")) {
                char *space = strchr(line, ' ');
                if (space != NULL) {
                    int code = atoi(space + 1);
                    if (code >= 200 && code < 300) {
                        success = TRUE;
                    } else {
                        LOGW("IDR requester: HTTP response %d from %s", code, task->host);
                        success = FALSE;
                    }
                }
            }
        } else if (nread == 0) {
            success = TRUE;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOGW("IDR requester: recv failed: %s", g_strerror(errno));
            success = FALSE;
        }

    out:
        break;
    } while (0);

    close(fd);
    return success;
}

static gpointer idr_requester_http_worker(gpointer data) {
    IdrHttpTask *task = (IdrHttpTask *)data;
    if (task == NULL || task->owner == NULL) {
        if (task != NULL) {
            g_free(task);
        }
        return NULL;
    }

    gboolean ok = send_http_request(task);
    if (!ok) {
        LOGW("IDR requester: HTTP request to %s:%u%s did not succeed",
             task->host,
             (unsigned int)ntohs(task->addr.sin_port),
             task->path);
    }

    g_mutex_lock(&task->owner->lock);
    task->owner->request_in_flight = FALSE;
    if (task->owner->cond_initialized) {
        g_cond_broadcast(&task->owner->cond);
    }
    g_mutex_unlock(&task->owner->lock);

    g_free(task);
    return NULL;
}

IdrRequester *idr_requester_new(const IdrCfg *cfg) {
    IdrRequester *req = g_new0(IdrRequester, 1);
    if (req == NULL) {
        return NULL;
    }

    g_mutex_init(&req->lock);
    g_cond_init(&req->cond);
    req->cond_initialized = TRUE;

    req->enabled = (cfg == NULL) ? TRUE : (cfg->enable != 0);
    req->have_source = FALSE;
    req->source_addr.s_addr = 0;
    req->source_host[0] = '\0';

    guint16 port = 80;
    guint timeout = 200;
    if (cfg != NULL) {
        if (cfg->http_port > 0 && cfg->http_port <= 65535) {
            port = (guint16)cfg->http_port;
        }
        if (cfg->http_timeout_ms > 0) {
            timeout = cfg->http_timeout_ms;
        }
    }
    req->http_port = port;
    req->http_timeout_ms = timeout;
    sanitize_path(cfg != NULL ? cfg->http_path : NULL, req->http_path, sizeof(req->http_path));

    req->last_warning_ms = 0;
    req->last_request_ms = 0;
    req->next_interval_ms = 0;
    req->attempt_count = 0;
    req->active = FALSE;
    req->request_in_flight = FALSE;
    req->shutting_down = FALSE;
    req->total_requests = 0;
    req->reinit_cb = NULL;
    req->reinit_user_data = NULL;
    req->reinit_pending = FALSE;

    return req;
}

void idr_requester_free(IdrRequester *req) {
    if (req == NULL) {
        return;
    }

    g_mutex_lock(&req->lock);
    req->shutting_down = TRUE;
    while (req->request_in_flight) {
        if (req->cond_initialized) {
            g_cond_wait(&req->cond, &req->lock);
        } else {
            g_mutex_unlock(&req->lock);
            g_usleep(1000);
            g_mutex_lock(&req->lock);
        }
    }
    g_mutex_unlock(&req->lock);

    if (req->cond_initialized) {
        g_cond_clear(&req->cond);
    }
    g_mutex_clear(&req->lock);
    g_free(req);
}

void idr_requester_set_enabled(IdrRequester *req, gboolean enabled) {
    if (req == NULL) {
        return;
    }
    g_mutex_lock(&req->lock);
    req->enabled = enabled ? TRUE : FALSE;
    if (!req->enabled) {
        req->active = FALSE;
        req->attempt_count = 0;
        req->next_interval_ms = 0;
        req->last_request_ms = 0;
        req->reinit_pending = FALSE;
    }
    g_mutex_unlock(&req->lock);
}

void idr_requester_note_source(IdrRequester *req, const struct sockaddr *addr, socklen_t len) {
    if (req == NULL || addr == NULL || len < (socklen_t)sizeof(struct sockaddr_in)) {
        return;
    }

    if (addr->sa_family != AF_INET) {
        return;
    }

    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    char host[IDR_HOST_MAX];
    if (inet_ntop(AF_INET, &sin->sin_addr, host, sizeof(host)) == NULL) {
        return;
    }

    g_mutex_lock(&req->lock);
    if (req->shutting_down) {
        g_mutex_unlock(&req->lock);
        return;
    }

    gboolean changed = FALSE;
    if (!req->have_source || req->source_addr.s_addr != sin->sin_addr.s_addr) {
        req->source_addr = sin->sin_addr;
        g_strlcpy(req->source_host, host, sizeof(req->source_host));
        req->have_source = TRUE;
        req->active = FALSE;
        req->attempt_count = 0;
        req->next_interval_ms = 0;
        req->last_request_ms = 0;
        req->reinit_pending = FALSE;
        changed = TRUE;
    }
    g_mutex_unlock(&req->lock);

    if (changed) {
        LOGI("IDR requester: tracking source %s", host);
    }
}

void idr_requester_handle_warning(IdrRequester *req) {
    if (req == NULL) {
        return;
    }

    guint64 now_ms = monotonic_ms();
    IdrHttpTask *task = NULL;
    guint attempt = 0;
    guint64 pending_total = 0;
    char host_copy[IDR_HOST_MAX];
    char path_copy[IDR_PATH_MAX];
    guint16 port_copy = 0;
    gboolean trigger_reinit = FALSE;
    IdrReinitCallback reinit_cb = NULL;
    gpointer reinit_data = NULL;

    g_mutex_lock(&req->lock);
    if (req->shutting_down || !req->enabled || !req->have_source) {
        g_mutex_unlock(&req->lock);
        return;
    }

    if (req->reinit_pending) {
        req->last_warning_ms = now_ms;
        g_mutex_unlock(&req->lock);
        return;
    }

    if (req->active && req->last_warning_ms != 0 && now_ms > req->last_warning_ms) {
        guint64 quiet = now_ms - req->last_warning_ms;
        if (quiet > IDR_QUIET_RESET_MS) {
            req->active = FALSE;
            req->attempt_count = 0;
            req->next_interval_ms = 0;
            req->last_request_ms = 0;
        }
    }

    if (!req->active) {
        req->active = TRUE;
        req->attempt_count = 0;
        req->next_interval_ms = 0;
        req->last_request_ms = 0;
    }

        req->last_warning_ms = now_ms;

    gboolean time_ready = FALSE;
    if (req->attempt_count == 0) {
        time_ready = TRUE;
    } else if (req->last_request_ms == 0) {
        time_ready = TRUE;
    } else if (now_ms >= req->last_request_ms + req->next_interval_ms) {
        time_ready = TRUE;
    }

    if (time_ready && !req->request_in_flight) {
        req->request_in_flight = TRUE;
        req->last_request_ms = now_ms;
        req->attempt_count++;
        attempt = req->attempt_count;

        if (req->attempt_count >= IDR_REINIT_THRESHOLD) {
            trigger_reinit = TRUE;
            reinit_cb = req->reinit_cb;
            reinit_data = req->reinit_user_data;
            req->reinit_pending = TRUE;
            req->request_in_flight = FALSE;
            req->active = FALSE;
            req->attempt_count = 0;
            req->next_interval_ms = 0;
            req->last_request_ms = 0;
            g_strlcpy(host_copy, req->source_host, sizeof(host_copy));
            g_strlcpy(path_copy, req->http_path, sizeof(path_copy));
            port_copy = req->http_port;
        } else if (req->attempt_count < IDR_BURST_COUNT) {
            req->next_interval_ms = IDR_BURST_INTERVAL_MS;
        } else {
            guint step = req->attempt_count - IDR_BURST_COUNT + 1;
            guint64 interval = ((guint64)IDR_BURST_INTERVAL_MS) << step;
            if (interval > IDR_MAX_INTERVAL_MS) {
                interval = IDR_MAX_INTERVAL_MS;
            }
            req->next_interval_ms = (guint)interval;
        }

        if (!trigger_reinit) {
            task = g_new0(IdrHttpTask, 1);
            if (task != NULL) {
                task->owner = req;
                task->addr.sin_family = AF_INET;
                task->addr.sin_addr = req->source_addr;
                task->addr.sin_port = htons(req->http_port);
                task->timeout_ms = req->http_timeout_ms;
                g_strlcpy(task->host, req->source_host, sizeof(task->host));
                g_strlcpy(task->path, req->http_path, sizeof(task->path));

                g_strlcpy(host_copy, task->host, sizeof(host_copy));
                g_strlcpy(path_copy, task->path, sizeof(path_copy));
                port_copy = ntohs(task->addr.sin_port);
                if (req->total_requests == G_MAXUINT64) {
                    pending_total = G_MAXUINT64;
                } else {
                    pending_total = req->total_requests + 1u;
                }
            } else {
                req->request_in_flight = FALSE;
                if (req->attempt_count > 0) {
                    req->attempt_count--;
                }
                req->last_request_ms = 0;
                if (req->cond_initialized) {
                    g_cond_broadcast(&req->cond);
                }
            }
        }
    }

    g_mutex_unlock(&req->lock);

    if (trigger_reinit) {
        LOGW("IDR requester: %s:%u%s exceeded %u attempts; requesting pipeline reinitialization",
             host_copy[0] != '\0' ? host_copy : "(unknown)",
             (unsigned int)port_copy,
             path_copy[0] != '\0' ? path_copy : "",
             (unsigned int)IDR_REINIT_THRESHOLD);
        if (reinit_cb != NULL) {
            reinit_cb(req, reinit_data);
        }
        return;
    }

    if (task == NULL) {
        return;
    }

    guint64 logged_total = pending_total;
    if (logged_total == 0) {
        g_mutex_lock(&req->lock);
        if (req->total_requests == G_MAXUINT64) {
            logged_total = G_MAXUINT64;
        } else {
            logged_total = req->total_requests + 1u;
        }
        g_mutex_unlock(&req->lock);
    }

    LOGW("IDR requester: triggering IDR via http://%s:%u%s (attempt %u, total %" G_GUINT64_FORMAT ")",
         host_copy,
         (unsigned int)port_copy,
         path_copy,
         attempt,
         logged_total);

    GThread *thread = g_thread_new("idr-http", idr_requester_http_worker, task);
    if (thread == NULL) {
        LOGE("IDR requester: failed to create worker thread");
        g_mutex_lock(&req->lock);
        req->request_in_flight = FALSE;
        if (req->attempt_count > 0) {
            req->attempt_count--;
        }
        req->last_request_ms = 0;
        if (req->cond_initialized) {
            g_cond_broadcast(&req->cond);
        }
        g_mutex_unlock(&req->lock);
        g_free(task);
        return;
    }

    if (pending_total > 0) {
        g_mutex_lock(&req->lock);
        req->total_requests = pending_total;
        g_mutex_unlock(&req->lock);
    }

    g_thread_unref(thread);
}

guint64 idr_requester_get_request_count(const IdrRequester *req) {
    if (req == NULL) {
        return 0;
    }

    guint64 total = 0;
    GMutex *lock = (GMutex *)&req->lock;
    g_mutex_lock(lock);
    total = req->total_requests;
    g_mutex_unlock(lock);
    return total;
}

void idr_requester_set_reinit_callback(IdrRequester *req, IdrReinitCallback cb, gpointer user_data) {
    if (req == NULL) {
        return;
    }

    g_mutex_lock(&req->lock);
    req->reinit_cb = cb;
    req->reinit_user_data = user_data;
    g_mutex_unlock(&req->lock);
}

