#include "idr_requester.h"

#include "logging.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>

#define IDR_INITIAL_INTERVAL_MS 500u
#define IDR_MAX_INTERVAL_MS 8000u
#define IDR_QUIET_RESET_MS 2000u
#define IDR_HOST_MAX 64
#define IDR_URL_MAX 128

typedef struct {
    IdrRequester *owner;
    char url[IDR_URL_MAX];
} IdrCurlTask;

struct IdrRequester {
    GMutex lock;
    GCond cond;
    gboolean cond_initialized;

    gboolean have_source;
    gboolean source_is_ipv6;
    char source_host[IDR_HOST_MAX];

    guint64 last_warning_ms;
    guint64 last_request_ms;
    guint64 next_interval_ms;
    guint attempt_count;

    gboolean active;
    gboolean request_in_flight;
    gboolean shutting_down;
};

static guint64 monotonic_ms(void) {
    return (guint64)g_get_monotonic_time() / 1000ull;
}

static gpointer idr_requester_curl_worker(gpointer data) {
    IdrCurlTask *task = (IdrCurlTask *)data;
    if (task == NULL || task->owner == NULL) {
        if (task != NULL) {
            g_free(task);
        }
        return NULL;
    }

    gchar *argv[] = {"curl", "--max-time", "0.2", "--connect-timeout", "0.2", "-s", "-o", "/dev/null", task->url, NULL};
    GError *error = NULL;
    int status = 0;
    gboolean ok = g_spawn_sync(NULL,
                               argv,
                               NULL,
                               G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                               NULL,
                               NULL,
                               NULL,
                               NULL,
                               &status,
                               &error);
    if (!ok) {
        static gsize curl_missing_once = 0;
        if (error != NULL) {
            if (error->domain == G_SPAWN_ERROR && error->code == G_SPAWN_ERROR_NOENT) {
                if (g_once_init_enter(&curl_missing_once)) {
                    LOGW("IDR requester: curl executable not found; cannot request IDR frames automatically");
                    g_once_init_leave(&curl_missing_once, 1);
                }
            } else {
                LOGW("IDR requester: curl invocation failed: %s", error->message);
            }
            g_error_free(error);
        } else {
            LOGW("IDR requester: curl invocation failed (unknown error)");
        }
    } else if (!g_spawn_check_exit_status(status, NULL)) {
        LOGW("IDR requester: curl exited with status %d", status);
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

IdrRequester *idr_requester_new(void) {
    IdrRequester *req = g_new0(IdrRequester, 1);
    if (req == NULL) {
        return NULL;
    }
    g_mutex_init(&req->lock);
    g_cond_init(&req->cond);
    req->cond_initialized = TRUE;
    req->have_source = FALSE;
    req->source_is_ipv6 = FALSE;
    req->source_host[0] = '\0';
    req->last_warning_ms = 0;
    req->last_request_ms = 0;
    req->next_interval_ms = IDR_INITIAL_INTERVAL_MS;
    req->attempt_count = 0;
    req->active = FALSE;
    req->request_in_flight = FALSE;
    req->shutting_down = FALSE;
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

void idr_requester_note_source(IdrRequester *req, const struct sockaddr *addr, socklen_t len) {
    if (req == NULL || addr == NULL || len <= 0) {
        return;
    }

    char host[IDR_HOST_MAX];
    memset(host, 0, sizeof(host));
    gboolean is_ipv6 = FALSE;

    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, host, sizeof(host)) == NULL) {
            return;
        }
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)addr;
        if (inet_ntop(AF_INET6, &sin6->sin6_addr, host, sizeof(host)) == NULL) {
            return;
        }
        is_ipv6 = TRUE;
    } else {
        return;
    }

    g_mutex_lock(&req->lock);
    if (req->shutting_down) {
        g_mutex_unlock(&req->lock);
        return;
    }

    gboolean changed = FALSE;
    if (!req->have_source || req->source_is_ipv6 != is_ipv6 || g_strcmp0(req->source_host, host) != 0) {
        g_strlcpy(req->source_host, host, sizeof(req->source_host));
        req->source_is_ipv6 = is_ipv6;
        req->have_source = TRUE;
        req->next_interval_ms = IDR_INITIAL_INTERVAL_MS;
        req->last_request_ms = 0;
        req->attempt_count = 0;
        req->active = FALSE;
        changed = TRUE;
    }
    g_mutex_unlock(&req->lock);

    if (changed) {
        if (is_ipv6) {
            LOGI("IDR requester: tracking source [%s]", host);
        } else {
            LOGI("IDR requester: tracking source %s", host);
        }
    }
}

void idr_requester_handle_warning(IdrRequester *req) {
    if (req == NULL) {
        return;
    }

    guint64 now_ms = monotonic_ms();
    IdrCurlTask *task = NULL;
    gboolean launch = FALSE;
    guint attempt = 0;

    g_mutex_lock(&req->lock);
    if (req->shutting_down) {
        g_mutex_unlock(&req->lock);
        return;
    }

    if (req->active && req->last_warning_ms != 0 && now_ms > req->last_warning_ms) {
        guint64 quiet = now_ms - req->last_warning_ms;
        if (quiet > IDR_QUIET_RESET_MS) {
            req->active = FALSE;
            req->next_interval_ms = IDR_INITIAL_INTERVAL_MS;
            req->last_request_ms = 0;
            req->attempt_count = 0;
        }
    }

    if (!req->active) {
        req->active = TRUE;
        req->next_interval_ms = IDR_INITIAL_INTERVAL_MS;
        req->last_request_ms = 0;
        req->attempt_count = 0;
    }

    req->last_warning_ms = now_ms;

    if (!req->have_source) {
        g_mutex_unlock(&req->lock);
        return;
    }

    gboolean time_ready = FALSE;
    if (req->last_request_ms == 0) {
        time_ready = TRUE;
    } else if (now_ms > req->last_request_ms) {
        guint64 elapsed = now_ms - req->last_request_ms;
        if (elapsed >= req->next_interval_ms) {
            time_ready = TRUE;
        }
    }

    if (time_ready && !req->request_in_flight) {
        req->request_in_flight = TRUE;
        req->last_request_ms = now_ms;
        attempt = ++req->attempt_count;

        guint64 next_interval = req->next_interval_ms * 2u;
        if (next_interval > IDR_MAX_INTERVAL_MS) {
            next_interval = IDR_MAX_INTERVAL_MS;
        }
        if (next_interval < IDR_INITIAL_INTERVAL_MS) {
            next_interval = IDR_INITIAL_INTERVAL_MS;
        }
        req->next_interval_ms = next_interval;

        task = g_new0(IdrCurlTask, 1);
        if (task != NULL) {
            task->owner = req;
            if (req->source_is_ipv6) {
                g_snprintf(task->url, sizeof(task->url), "http://[%s]/request/idr", req->source_host);
            } else {
                g_snprintf(task->url, sizeof(task->url), "http://%s/request/idr", req->source_host);
            }
            launch = TRUE;
        } else {
            req->request_in_flight = FALSE;
            req->attempt_count--;
            req->last_request_ms = 0;
            if (req->cond_initialized) {
                g_cond_broadcast(&req->cond);
            }
        }
    }

    char url_copy[IDR_URL_MAX];
    url_copy[0] = '\0';
    if (launch && task != NULL) {
        g_strlcpy(url_copy, task->url, sizeof(url_copy));
    }
    g_mutex_unlock(&req->lock);

    if (!launch || task == NULL) {
        if (task != NULL) {
            g_free(task);
        }
        return;
    }

    LOGW("IDR requester: triggering IDR via %s (attempt %u)", url_copy, attempt);

    GThread *thread = g_thread_new("idr-request", idr_requester_curl_worker, task);
    if (thread == NULL) {
        LOGE("IDR requester: failed to create curl helper thread");
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

    g_thread_unref(thread);
}
