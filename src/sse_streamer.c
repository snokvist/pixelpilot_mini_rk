#include "sse_streamer.h"

#include "logging.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    int fd;
} SseClientContext;

static const guint kDefaultMaxClients = 8;
static const size_t kMaxRequestBytes = 4096;
static const int kHandshakeTimeoutSeconds = 5;

static gboolean streamer_is_shutdown(const SseStreamer *streamer) {
    return g_atomic_int_get((volatile gint *)&streamer->shutdown_flag) != 0;
}

static void streamer_clear_shutdown(SseStreamer *streamer) {
    g_atomic_int_set(&streamer->shutdown_flag, 0);
}

static void streamer_request_shutdown(SseStreamer *streamer) {
    g_atomic_int_set(&streamer->shutdown_flag, 1);
}

static int create_listen_socket(const char *bind_address, int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOGE("SSE streamer: socket(): %s", strerror(errno));
        return -1;
    }

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
        LOGW("SSE streamer: setsockopt(SO_REUSEADDR) failed: %s", strerror(errno));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (bind_address == NULL || bind_address[0] == '\0') {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (inet_pton(AF_INET, bind_address, &addr.sin_addr) != 1) {
            LOGE("SSE streamer: invalid bind address '%s'", bind_address);
            close(fd);
            return -1;
        }
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOGE("SSE streamer: bind(%s:%d): %s", bind_address && bind_address[0] ? bind_address : "0.0.0.0", port,
             strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 4) != 0) {
        LOGE("SSE streamer: listen(): %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static void set_socket_timeouts(int fd, int seconds) {
    struct timeval tv = {0};
    tv.tv_sec = seconds;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        LOGW("SSE streamer: setsockopt(SO_RCVTIMEO) failed: %s", strerror(errno));
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        LOGW("SSE streamer: setsockopt(SO_SNDTIMEO) failed: %s", strerror(errno));
    }
}

static ssize_t send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t rc = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return -2;
            }
            return -1;
        }
        if (rc == 0) {
            break;
        }
        sent += (size_t)rc;
    }
    return (ssize_t)sent;
}

static gboolean read_http_request(int fd, char *method, size_t method_sz, char *path, size_t path_sz) {
    size_t total = 0;
    char buffer[kMaxRequestBytes + 1];
    while (total < kMaxRequestBytes) {
        ssize_t rc = recv(fd, buffer + total, kMaxRequestBytes - total, 0);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return FALSE;
            }
            return FALSE;
        }
        if (rc == 0) {
            return FALSE;
        }
        total += (size_t)rc;
        buffer[total] = '\0';
        if (strstr(buffer, "\r\n\r\n") != NULL) {
            break;
        }
    }
    if (total >= kMaxRequestBytes && strstr(buffer, "\r\n\r\n") == NULL) {
        return FALSE;
    }

    char *line_end = strstr(buffer, "\r\n");
    if (line_end == NULL) {
        return FALSE;
    }
    *line_end = '\0';

    char *space = strchr(buffer, ' ');
    if (space == NULL) {
        return FALSE;
    }
    *space = '\0';
    if (g_strlcpy(method, buffer, method_sz) >= method_sz) {
        return FALSE;
    }

    char *path_start = space + 1;
    while (*path_start == ' ') {
        path_start++;
    }
    char *path_end = strchr(path_start, ' ');
    if (path_end == NULL) {
        return FALSE;
    }
    *path_end = '\0';
    if (g_strlcpy(path, path_start, path_sz) >= path_sz) {
        return FALSE;
    }

    return TRUE;
}

static gboolean try_acquire_client_slot(SseStreamer *streamer) {
    for (;;) {
        gint current = g_atomic_int_get(&streamer->active_clients);
        if (current >= (gint)streamer->max_clients) {
            return FALSE;
        }
        if (g_atomic_int_compare_and_exchange(&streamer->active_clients, current, current + 1)) {
            return TRUE;
        }
    }
}

static void release_client_slot(SseStreamer *streamer) {
    g_atomic_int_add(&streamer->active_clients, -1);
}

static void format_json_payload(char *buf, size_t buf_sz, const SseStatsSnapshot *snap, gboolean have_stats) {
    if (!have_stats || snap == NULL) {
        g_snprintf(buf, buf_sz, "{\"have_stats\":false}");
        return;
    }

    double last_frame_kib = snap->last_frame_bytes / 1024.0;
    double frame_avg_kib = snap->frame_size_avg / 1024.0;

    g_snprintf(buf, buf_sz,
               "{\"have_stats\":true,\"total_packets\":%" G_GUINT64_FORMAT
               ",\"video_packets\":%" G_GUINT64_FORMAT
               ",\"audio_packets\":%" G_GUINT64_FORMAT
               ",\"ignored_packets\":%" G_GUINT64_FORMAT
               ",\"duplicate_packets\":%" G_GUINT64_FORMAT
               ",\"lost_packets\":%" G_GUINT64_FORMAT
               ",\"reordered_packets\":%" G_GUINT64_FORMAT
               ",\"total_bytes\":%" G_GUINT64_FORMAT
               ",\"video_bytes\":%" G_GUINT64_FORMAT
               ",\"audio_bytes\":%" G_GUINT64_FORMAT
               ",\"frame_count\":%" G_GUINT64_FORMAT
               ",\"incomplete_frames\":%" G_GUINT64_FORMAT
               ",\"last_frame_kib\":%.2f,\"avg_frame_kib\":%.2f,\"bitrate_mbps\":%.3f,\"bitrate_avg_mbps\":%.3f,"
                "\"jitter_ms\":%.3f,\"jitter_avg_ms\":%.3f,\"expected_sequence\":%u,\"last_video_timestamp\":%u,"
                "\"last_packet_ns\":%" G_GUINT64_FORMAT ",\"idr_requests\":%" G_GUINT64_FORMAT "}",
               snap->total_packets, snap->video_packets, snap->audio_packets, snap->ignored_packets,
               snap->duplicate_packets, snap->lost_packets, snap->reordered_packets, snap->total_bytes,
               snap->video_bytes, snap->audio_bytes, snap->frame_count, snap->incomplete_frames, last_frame_kib,
                frame_avg_kib, snap->bitrate_mbps, snap->bitrate_avg_mbps, snap->jitter_ms, snap->jitter_avg_ms,
                snap->expected_sequence, snap->last_video_timestamp, snap->last_packet_ns, snap->idr_requests);
}

static void sleep_interval(const SseStreamer *streamer) {
    guint interval = streamer->interval_ms > 0 ? streamer->interval_ms : 1000u;
    while (interval > 0 && !streamer_is_shutdown(streamer)) {
        guint chunk = interval > 100 ? 100 : interval;
        g_usleep(chunk * 1000);
        if (interval >= chunk) {
            interval -= chunk;
        } else {
            interval = 0;
        }
    }
}

static void sse_client_worker(gpointer data, gpointer user_data) {
    SseClientContext *ctx = data;
    SseStreamer *streamer = user_data;
    int fd = ctx->fd;

    char method[8] = {0};
    char path[128] = {0};
    if (!read_http_request(fd, method, sizeof(method), path, sizeof(path))) {
        send_all(fd, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n", 44);
        goto cleanup;
    }

    if (g_ascii_strcasecmp(method, "GET") != 0) {
        send_all(fd, "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n", 58);
        goto cleanup;
    }

    if (g_strcmp0(path, "/stats") != 0 && g_strcmp0(path, "/stats/") != 0) {
        send_all(fd, "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n", 49);
        goto cleanup;
    }

    const char *response_header = "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: text/event-stream\r\n"
                                  "Cache-Control: no-cache\r\n"
                                  "Connection: keep-alive\r\n"
                                  "Access-Control-Allow-Origin: *\r\n\r\n";
    ssize_t header_rc = send_all(fd, response_header, strlen(response_header));
    if (header_rc < 0) {
        goto cleanup;
    }

    while (!streamer_is_shutdown(streamer)) {
        SseStatsSnapshot snapshot = {0};
        gboolean have_stats = FALSE;
        g_mutex_lock(&streamer->lock);
        have_stats = streamer->have_stats;
        if (have_stats) {
            snapshot = streamer->stats;
        }
        g_mutex_unlock(&streamer->lock);

        char json[768];
        format_json_payload(json, sizeof(json), &snapshot, have_stats);

        char event_buf[1024];
        int event_len = g_snprintf(event_buf, sizeof(event_buf), "event: stats\ndata: %s\n\n", json);
        if (event_len <= 0 || event_len >= (int)sizeof(event_buf)) {
            break;
        }
        ssize_t send_rc = send_all(fd, event_buf, (size_t)event_len);
        if (send_rc < 0) {
            break;
        }

        sleep_interval(streamer);
    }

cleanup:
    close(fd);
    g_free(ctx);
    release_client_slot(streamer);
}

static gpointer sse_accept_thread(gpointer data) {
    SseStreamer *streamer = data;
    while (!streamer_is_shutdown(streamer)) {
        struct pollfd pfd = {
            .fd = streamer->listen_fd,
            .events = POLLIN,
            .revents = 0,
        };
        int rc = poll(&pfd, 1, 200);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!streamer_is_shutdown(streamer)) {
                LOGW("SSE streamer: poll failed: %s", strerror(errno));
            }
            continue;
        }
        if (rc == 0) {
            continue;
        }
        if (!(pfd.revents & POLLIN)) {
            continue;
        }
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int fd = accept(streamer->listen_fd, (struct sockaddr *)&addr, &addrlen);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EBADF || errno == EINVAL) {
                break;
            }
            LOGW("SSE streamer: accept failed: %s", strerror(errno));
            continue;
        }

        set_socket_timeouts(fd, kHandshakeTimeoutSeconds);

        if (!try_acquire_client_slot(streamer)) {
            send_all(fd, "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n", 56);
            close(fd);
            continue;
        }

        SseClientContext *ctx = g_new0(SseClientContext, 1);
        if (ctx == NULL) {
            LOGW("SSE streamer: failed to allocate client context");
            release_client_slot(streamer);
            close(fd);
            continue;
        }
        ctx->fd = fd;

        GError *err = NULL;
        if (!g_thread_pool_push(streamer->client_pool, ctx, &err)) {
            LOGW("SSE streamer: failed to queue client: %s", err != NULL ? err->message : "unknown error");
            if (err != NULL) {
                g_error_free(err);
            }
            send_all(fd, "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n", 56);
            close(fd);
            g_free(ctx);
            release_client_slot(streamer);
        }
    }
    return NULL;
}

void sse_streamer_init(SseStreamer *streamer) {
    if (streamer == NULL) {
        return;
    }
    memset(streamer, 0, sizeof(*streamer));
    streamer->configured = FALSE;
    streamer->listen_fd = -1;
    streamer->interval_ms = 1000;
    streamer->port = 0;
    streamer->client_pool = NULL;
    streamer->max_clients = kDefaultMaxClients;
    streamer->active_clients = 0;
    g_mutex_init(&streamer->lock);
    streamer_clear_shutdown(streamer);
}

int sse_streamer_start(SseStreamer *streamer, const AppCfg *cfg) {
    if (streamer == NULL || cfg == NULL) {
        return -1;
    }
    if (!cfg->sse.enable) {
        streamer->configured = FALSE;
        return 0;
    }

    streamer->configured = TRUE;
    streamer->interval_ms = cfg->sse.interval_ms > 0 ? (guint)cfg->sse.interval_ms : 1000u;
    streamer->port = cfg->sse.port;
    if (cfg->sse.bind_address[0] != '\0') {
        g_strlcpy(streamer->bind_address, cfg->sse.bind_address, sizeof(streamer->bind_address));
    } else {
        streamer->bind_address[0] = '\0';
    }

    streamer->listen_fd = create_listen_socket(streamer->bind_address, streamer->port);
    if (streamer->listen_fd < 0) {
        streamer->configured = FALSE;
        return -1;
    }

    streamer_clear_shutdown(streamer);
    streamer->running = TRUE;

    if (streamer->client_pool == NULL) {
        GError *err = NULL;
        streamer->client_pool = g_thread_pool_new(sse_client_worker, streamer, (gint)streamer->max_clients, FALSE, &err);
        if (streamer->client_pool == NULL) {
            LOGE("SSE streamer: failed to create client pool: %s", err != NULL ? err->message : "unknown error");
            if (err != NULL) {
                g_error_free(err);
            }
            close(streamer->listen_fd);
            streamer->listen_fd = -1;
            streamer->running = FALSE;
            streamer->configured = FALSE;
            return -1;
        }
    }

    g_atomic_int_set(&streamer->active_clients, 0);

    streamer->accept_thread = g_thread_new("sse-accept", sse_accept_thread, streamer);
    if (streamer->accept_thread == NULL) {
        LOGE("SSE streamer: failed to start accept thread");
        close(streamer->listen_fd);
        streamer->listen_fd = -1;
        streamer->running = FALSE;
        streamer->configured = FALSE;
        g_thread_pool_free(streamer->client_pool, FALSE, TRUE);
        streamer->client_pool = NULL;
        return -1;
    }
    LOGI("SSE streamer: listening on %s:%d (interval=%ums)",
         streamer->bind_address[0] ? streamer->bind_address : "0.0.0.0", streamer->port, streamer->interval_ms);
    return 0;
}

void sse_streamer_publish(SseStreamer *streamer, const UdpReceiverStats *stats, gboolean have_stats) {
    if (streamer == NULL || !streamer->running) {
        return;
    }
    g_mutex_lock(&streamer->lock);
    streamer->have_stats = have_stats ? TRUE : FALSE;
    if (have_stats && stats != NULL) {
        SseStatsSnapshot snap = {
            .total_packets = stats->total_packets,
            .video_packets = stats->video_packets,
            .audio_packets = stats->audio_packets,
            .ignored_packets = stats->ignored_packets,
            .duplicate_packets = stats->duplicate_packets,
            .lost_packets = stats->lost_packets,
            .reordered_packets = stats->reordered_packets,
            .total_bytes = stats->total_bytes,
            .video_bytes = stats->video_bytes,
            .audio_bytes = stats->audio_bytes,
            .frame_count = stats->frame_count,
            .incomplete_frames = stats->incomplete_frames,
            .last_frame_bytes = stats->last_frame_bytes,
            .frame_size_avg = stats->frame_size_avg,
            .jitter_ms = stats->jitter / 90.0,
            .jitter_avg_ms = stats->jitter_avg / 90.0,
            .bitrate_mbps = stats->bitrate_mbps,
            .bitrate_avg_mbps = stats->bitrate_avg_mbps,
            .last_video_timestamp = stats->last_video_timestamp,
            .expected_sequence = stats->expected_sequence,
            .last_packet_ns = stats->last_packet_ns,
            .idr_requests = stats->idr_requests,
        };
        streamer->stats = snap;
    }
    g_mutex_unlock(&streamer->lock);
}

void sse_streamer_stop(SseStreamer *streamer) {
    if (streamer == NULL) {
        return;
    }
    streamer_request_shutdown(streamer);
    if (streamer->listen_fd >= 0) {
        close(streamer->listen_fd);
        streamer->listen_fd = -1;
    }
    if (streamer->accept_thread != NULL) {
        g_thread_join(streamer->accept_thread);
        streamer->accept_thread = NULL;
    }
    if (streamer->client_pool != NULL) {
        g_thread_pool_free(streamer->client_pool, FALSE, TRUE);
        streamer->client_pool = NULL;
    }
    streamer->running = FALSE;
    streamer->configured = FALSE;
    streamer->have_stats = FALSE;
}

gboolean sse_streamer_requires_stats(const SseStreamer *streamer) {
    return streamer != NULL && streamer->configured;
}
