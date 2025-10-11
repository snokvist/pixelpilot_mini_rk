#include "sse_streamer.h"

#include "logging.h"
#include "pipeline.h"

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

#define SSE_MAX_HEADER_SIZE 4096
#define SSE_CLIENT_TIMEOUT_SEC 5

typedef enum {
    SSE_REQUEST_OK = 0,
    SSE_REQUEST_TIMEOUT = -1,
    SSE_REQUEST_TOO_LARGE = -2,
    SSE_REQUEST_IO_ERROR = -3,
    SSE_REQUEST_CLOSED = -4,
} SseRequestReadResult;

typedef struct {
    SseStreamer *streamer;
    int fd;
} SseClientContext;

static guint64 bytes_to_mbytes(guint64 bytes) {
    const guint64 bytes_per_mbyte = 1024ull * 1024ull;
    return bytes / bytes_per_mbyte;
}

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

static void set_socket_timeout(int fd, int seconds) {
    struct timeval tv = {0};
    tv.tv_sec = seconds;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        LOGW("SSE streamer: setsockopt(SO_RCVTIMEO) failed: %s", strerror(errno));
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        LOGW("SSE streamer: setsockopt(SO_SNDTIMEO) failed: %s", strerror(errno));
    }
}

static SseRequestReadResult read_http_request(int fd, char *buffer, size_t buf_sz, ssize_t *out_len) {
    if (buffer == NULL || buf_sz == 0) {
        return SSE_REQUEST_IO_ERROR;
    }
    size_t total = 0;
    while (total < buf_sz - 1) {
        ssize_t rc = recv(fd, buffer + total, (buf_sz - 1) - total, 0);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return SSE_REQUEST_TIMEOUT;
            }
            return SSE_REQUEST_IO_ERROR;
        }
        if (rc == 0) {
            return SSE_REQUEST_CLOSED;
        }
        total += (size_t)rc;
        buffer[total] = '\0';
        if (g_strstr_len(buffer, (gssize)total, "\r\n\r\n") != NULL ||
            g_strstr_len(buffer, (gssize)total, "\n\n") != NULL) {
            if (out_len != NULL) {
                *out_len = (ssize_t)total;
            }
            return SSE_REQUEST_OK;
        }
    }
    return SSE_REQUEST_TOO_LARGE;
}

static gboolean parse_request_line(const char *request, char *method, size_t method_sz, char *path, size_t path_sz) {
    if (request == NULL || method == NULL || path == NULL || method_sz == 0 || path_sz == 0) {
        return FALSE;
    }
    const char *line_end = strstr(request, "\r\n");
    if (line_end == NULL) {
        line_end = strchr(request, '\n');
    }
    size_t line_len = line_end != NULL ? (size_t)(line_end - request) : strlen(request);
    if (line_len >= SSE_MAX_HEADER_SIZE) {
        line_len = SSE_MAX_HEADER_SIZE - 1;
    }
    char line_buf[SSE_MAX_HEADER_SIZE];
    memcpy(line_buf, request, line_len);
    line_buf[line_len] = '\0';

    char *first_space = strchr(line_buf, ' ');
    if (first_space == NULL) {
        return FALSE;
    }
    *first_space = '\0';
    char *path_start = first_space + 1;
    if (*path_start == '\0') {
        return FALSE;
    }
    char *second_space = strchr(path_start, ' ');
    if (second_space == NULL) {
        return FALSE;
    }
    *second_space = '\0';

    g_strlcpy(method, line_buf, method_sz);
    g_strlcpy(path, path_start, path_sz);
    return TRUE;
}

static ssize_t send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t rc = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
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

static void send_simple_response(int fd, const char *response) {
    if (response == NULL) {
        return;
    }
    if (send_all(fd, response, strlen(response)) < 0) {
        int err = errno;
        if (err != EPIPE && err != ECONNRESET) {
            LOGW("SSE streamer: failed to send response: %s", strerror(err));
        }
    }
}

static void format_json_payload(char *buf, size_t buf_sz, const SseStatsSnapshot *snap, gboolean have_stats) {
    if (!have_stats || snap == NULL) {
        g_snprintf(buf, buf_sz, "{\"have_stats\":false}");
        return;
    }

    double last_frame_kib = snap->last_frame_bytes / 1024.0;
    double frame_avg_kib = snap->frame_size_avg / 1024.0;
    double recording_elapsed_s = snap->recording_elapsed_ns / 1e9;
    double recording_media_s = snap->recording_media_ns / 1e9;
    guint64 total_mbytes = bytes_to_mbytes(snap->total_bytes);
    guint64 video_mbytes = bytes_to_mbytes(snap->video_bytes);
    guint64 audio_mbytes = bytes_to_mbytes(snap->audio_bytes);
    guint64 recording_mbytes = bytes_to_mbytes(snap->recording_bytes);
    const char *recording_enabled = snap->recording_enabled ? "true" : "false";
    const char *recording_active = snap->recording_active ? "true" : "false";
    char *escaped_path = NULL;
    if (snap->recording_path[0] != '\0') {
        escaped_path = g_strescape(snap->recording_path, NULL);
    }

    g_snprintf(buf, buf_sz,
               "{"
               "\"have_stats\":true,"
               "\"total_packets\":%" G_GUINT64_FORMAT ","
               "\"video_packets\":%" G_GUINT64_FORMAT ","
               "\"audio_packets\":%" G_GUINT64_FORMAT ","
               "\"ignored_packets\":%" G_GUINT64_FORMAT ","
               "\"duplicate_packets\":%" G_GUINT64_FORMAT ","
               "\"lost_packets\":%" G_GUINT64_FORMAT ","
               "\"reordered_packets\":%" G_GUINT64_FORMAT ","
               "\"total_mbytes\":%" G_GUINT64_FORMAT ","
               "\"video_mbytes\":%" G_GUINT64_FORMAT ","
               "\"audio_mbytes\":%" G_GUINT64_FORMAT ","
               "\"frame_count\":%" G_GUINT64_FORMAT ","
               "\"incomplete_frames\":%" G_GUINT64_FORMAT ","
               "\"last_frame_kib\":%.2f,"
               "\"avg_frame_kib\":%.2f,"
               "\"bitrate_mbps\":%.3f,"
               "\"bitrate_avg_mbps\":%.3f,"
               "\"jitter_ms\":%.3f,"
               "\"jitter_avg_ms\":%.3f,"
               "\"expected_sequence\":%u,"
               "\"idr_requests\":%" G_GUINT64_FORMAT ","
               "\"recording_enabled\":%s,"
               "\"recording_active\":%s,"
               "\"recording_duration_s\":%.3f,"
               "\"recording_media_s\":%.3f,"
               "\"recording_mbytes\":%" G_GUINT64_FORMAT ","
               "\"recording_path\":\"%s\"}",
               snap->total_packets, snap->video_packets, snap->audio_packets, snap->ignored_packets,
               snap->duplicate_packets, snap->lost_packets, snap->reordered_packets, total_mbytes, video_mbytes,
               audio_mbytes, snap->frame_count, snap->incomplete_frames, last_frame_kib, frame_avg_kib,
               snap->bitrate_mbps, snap->bitrate_avg_mbps, snap->jitter_ms, snap->jitter_avg_ms, snap->expected_sequence,
               snap->idr_requests, recording_enabled, recording_active, recording_elapsed_s, recording_media_s,
               recording_mbytes, escaped_path != NULL ? escaped_path : "");

    g_free(escaped_path);
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

static gpointer sse_client_thread(gpointer data) {
    SseClientContext *ctx = data;
    SseStreamer *streamer = ctx->streamer;
    int fd = ctx->fd;

    char request[SSE_MAX_HEADER_SIZE];
    ssize_t req_len = 0;
    SseRequestReadResult read_rc = read_http_request(fd, request, sizeof(request), &req_len);
    if (read_rc != SSE_REQUEST_OK) {
        switch (read_rc) {
        case SSE_REQUEST_TIMEOUT:
            send_simple_response(fd, "HTTP/1.1 408 Request Timeout\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            break;
        case SSE_REQUEST_TOO_LARGE:
            send_simple_response(fd, "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            break;
        case SSE_REQUEST_IO_ERROR:
            LOGW("SSE streamer: failed to read HTTP request: %s", strerror(errno));
            break;
        case SSE_REQUEST_CLOSED:
            break;
        default:
            break;
        }
        goto cleanup;
    }
    request[req_len] = '\0';

    char method[8] = {0};
    char path[256] = {0};
    if (!parse_request_line(request, method, sizeof(method), path, sizeof(path))) {
        send_simple_response(fd, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        goto cleanup;
    }

    if (g_strcmp0(method, "GET") != 0) {
        send_simple_response(fd, "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        goto cleanup;
    }

    if (g_strcmp0(path, "/stats") != 0 && g_strcmp0(path, "/stats/") != 0) {
        send_simple_response(fd, "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        goto cleanup;
    }

    const char *response_header = "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: text/event-stream\r\n"
                                  "Cache-Control: no-cache\r\n"
                                  "Connection: keep-alive\r\n"
                                  "Access-Control-Allow-Origin: *\r\n\r\n";
    if (send_all(fd, response_header, strlen(response_header)) < 0) {
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

        char json[4096];
        format_json_payload(json, sizeof(json), &snapshot, have_stats);

        char event_buf[4608];
        int event_len = g_snprintf(event_buf, sizeof(event_buf), "event: stats\ndata: %s\n\n", json);
        if (event_len <= 0 || event_len >= (int)sizeof(event_buf)) {
            break;
        }
        if (send_all(fd, event_buf, (size_t)event_len) < 0) {
            break;
        }

        sleep_interval(streamer);
    }

cleanup:
    close(fd);
    g_atomic_int_set(&streamer->active_client, 0);
    g_free(ctx);
    return NULL;
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

        set_socket_timeout(fd, SSE_CLIENT_TIMEOUT_SEC);

        if (!g_atomic_int_compare_and_exchange(&streamer->active_client, 0, 1)) {
            LOGI("SSE streamer: rejecting additional client while stream is active");
            send_simple_response(fd, "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nRetry-After: 1\r\nContent-Length: 0\r\n\r\n");
            close(fd);
            continue;
        }

        SseClientContext *ctx = g_new0(SseClientContext, 1);
        if (ctx == NULL) {
            LOGW("SSE streamer: failed to allocate client context");
            close(fd);
            g_atomic_int_set(&streamer->active_client, 0);
            continue;
        }
        ctx->streamer = streamer;
        ctx->fd = fd;
        GThread *client = g_thread_new("sse-client", sse_client_thread, ctx);
        if (client == NULL) {
            LOGW("SSE streamer: failed to spawn client thread");
            close(fd);
            g_free(ctx);
            g_atomic_int_set(&streamer->active_client, 0);
            continue;
        }
        g_thread_unref(client);
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
    g_mutex_init(&streamer->lock);
    streamer_clear_shutdown(streamer);
    g_atomic_int_set(&streamer->active_client, 0);
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
    streamer->accept_thread = g_thread_new("sse-accept", sse_accept_thread, streamer);
    if (streamer->accept_thread == NULL) {
        LOGE("SSE streamer: failed to start accept thread");
        close(streamer->listen_fd);
        streamer->listen_fd = -1;
        streamer->running = FALSE;
        streamer->configured = FALSE;
        return -1;
    }
    LOGI("SSE streamer: listening on %s:%d (interval=%ums)",
         streamer->bind_address[0] ? streamer->bind_address : "0.0.0.0", streamer->port, streamer->interval_ms);
    return 0;
}

void sse_streamer_publish(SseStreamer *streamer, const UdpReceiverStats *stats, gboolean have_stats,
                         gboolean recording_enabled, const struct PipelineRecordingStats *recording) {
    if (streamer == NULL || !streamer->running) {
        return;
    }
    g_mutex_lock(&streamer->lock);
    streamer->have_stats = have_stats ? TRUE : FALSE;
    SseStatsSnapshot snap = {0};
    if (have_stats && stats != NULL) {
        snap.total_packets = stats->total_packets;
        snap.video_packets = stats->video_packets;
        snap.audio_packets = stats->audio_packets;
        snap.ignored_packets = stats->ignored_packets;
        snap.duplicate_packets = stats->duplicate_packets;
        snap.lost_packets = stats->lost_packets;
        snap.reordered_packets = stats->reordered_packets;
        snap.total_bytes = stats->total_bytes;
        snap.video_bytes = stats->video_bytes;
        snap.audio_bytes = stats->audio_bytes;
        snap.frame_count = stats->frame_count;
        snap.incomplete_frames = stats->incomplete_frames;
        snap.last_frame_bytes = stats->last_frame_bytes;
        snap.frame_size_avg = stats->frame_size_avg;
        snap.jitter_ms = stats->jitter / 90.0;
        snap.jitter_avg_ms = stats->jitter_avg / 90.0;
        snap.bitrate_mbps = stats->bitrate_mbps;
        snap.bitrate_avg_mbps = stats->bitrate_avg_mbps;
        snap.expected_sequence = stats->expected_sequence;
        snap.idr_requests = stats->idr_requests;
    }
    snap.recording_enabled = recording_enabled ? TRUE : FALSE;
    if (recording != NULL) {
        snap.recording_active = recording->active ? TRUE : FALSE;
        snap.recording_bytes = recording->bytes_written;
        snap.recording_elapsed_ns = recording->elapsed_ns;
        snap.recording_media_ns = recording->media_duration_ns;
        g_strlcpy(snap.recording_path, recording->output_path, sizeof(snap.recording_path));
    } else {
        snap.recording_active = FALSE;
        snap.recording_bytes = 0;
        snap.recording_elapsed_ns = 0;
        snap.recording_media_ns = 0;
        snap.recording_path[0] = '\0';
    }
    streamer->stats = snap;
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
    streamer->running = FALSE;
    streamer->configured = FALSE;
    streamer->have_stats = FALSE;
    g_atomic_int_set(&streamer->active_client, 0);
}

gboolean sse_streamer_requires_stats(const SseStreamer *streamer) {
    return streamer != NULL && streamer->configured;
}
