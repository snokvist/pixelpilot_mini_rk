#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#define SLOT_COUNT 8
#define MAX_TEXT_LEN 64
#define UDP_BUFFER 1024
#define JSON_BUFFER 1024
#define DEFAULT_POLL_INTERVAL_MS 200

static volatile sig_atomic_t g_stop = 0;
static bool g_verbose = false;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  -s, --socket PATH   UNIX datagram socket path (default: /run/pixelpilot/osd.sock)\n"
            "  -b, --bind ADDR     UDP bind address (default: 0.0.0.0)\n"
            "  -p, --port PORT     UDP port (default: 5005)\n"
            "  -T, --ttl  MS       Default ttl_ms to include when none active (default: 0)\n"
            "  -v, --verbose       Verbose logging\n"
            "  -h, --help          Show this help\n",
            prog);
}

struct slot_state {
    bool has_text;
    char text[MAX_TEXT_LEN];
    uint64_t text_expiry; // 0 means no TTL tracking

    bool has_value;
    double value;
    uint64_t value_expiry; // 0 means no TTL tracking
};

static void clear_text(struct slot_state *slot)
{
    if (!slot->has_text) {
        return;
    }
    slot->has_text = false;
    slot->text[0] = '\0';
    slot->text_expiry = 0;
}

static void clear_value(struct slot_state *slot)
{
    if (!slot->has_value) {
        return;
    }
    slot->has_value = false;
    slot->value = 0.0;
    slot->value_expiry = 0;
}

static uint64_t add_ttl(uint64_t base, uint64_t ttl_ms)
{
    if (ttl_ms == 0) {
        return 0;
    }
    if (UINT64_MAX - base <= ttl_ms) {
        return UINT64_MAX;
    }
    return base + ttl_ms;
}

static bool ttl_active(uint64_t expiry, uint64_t now)
{
    return expiry > 0 && expiry > now;
}

static bool parse_uint_field(const char *payload, const char *key, uint64_t *value)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *pos = strstr(payload, pattern);
    if (!pos) {
        return false;
    }
    pos += strlen(pattern);
    while (*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long tmp = strtoull(pos, &end, 10);
    if (errno != 0 || end == pos) {
        return false;
    }
    *value = (uint64_t)tmp;
    return true;
}

static int append_json_string(char *dst, size_t dst_len, const char *src)
{
    size_t off = 0;
    if (dst_len < 3) {
        return -1;
    }
    dst[off++] = '"';
    for (const unsigned char *p = (const unsigned char *)src; *p; ++p) {
        unsigned char c = *p;
        if (c == '\"' || c == '\\') {
            if (off + 2 >= dst_len) {
                return -1;
            }
            dst[off++] = '\\';
            dst[off++] = (char)c;
        } else if (c == '\n') {
            if (off + 2 >= dst_len) {
                return -1;
            }
            dst[off++] = '\\';
            dst[off++] = 'n';
        } else if (c == '\r') {
            if (off + 2 >= dst_len) {
                return -1;
            }
            dst[off++] = '\\';
            dst[off++] = 'r';
        } else if (c == '\t') {
            if (off + 2 >= dst_len) {
                return -1;
            }
            dst[off++] = '\\';
            dst[off++] = 't';
        } else if (c < 0x20) {
            if (off + 6 >= dst_len) {
                return -1;
            }
            int written = snprintf(dst + off, dst_len - off, "\\u%04x", c);
            if (written < 0 || (size_t)written >= dst_len - off) {
                return -1;
            }
            off += (size_t)written;
        } else {
            if (off + 1 >= dst_len) {
                return -1;
            }
            dst[off++] = (char)c;
        }
    }
    if (off + 2 > dst_len) {
        return -1;
    }
    dst[off++] = '"';
    dst[off] = '\0';
    return (int)off;
}

static size_t parse_string_array(const char *payload, const char *key, char out[][MAX_TEXT_LEN], size_t max_count)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(payload, pattern);
    if (!pos) {
        return 0;
    }
    pos = strchr(pos, '[');
    if (!pos) {
        return 0;
    }
    pos++;

    size_t count = 0;
    while (*pos && count < max_count) {
        while (*pos && isspace((unsigned char)*pos)) {
            pos++;
        }
        if (*pos == ']') {
            pos++;
            break;
        }
        if (*pos != '\"') {
            break;
        }
        pos++;
        size_t len = 0;
        const char *start = pos;
        while (pos[len] && pos[len] != '\"') {
            if (pos[len] == '\\' && pos[len + 1]) {
                len += 2;
            } else {
                len++;
            }
        }
        size_t dst_i = 0;
        for (size_t i = 0; i < len && dst_i + 1 < MAX_TEXT_LEN; ++i) {
            char ch = start[i];
            if (ch == '\\' && i + 1 < len) {
                char next = start[i + 1];
                switch (next) {
                    case '\\': out[count][dst_i++] = '\\'; break;
                    case '\"': out[count][dst_i++] = '\"'; break;
                    case 'n': out[count][dst_i++] = '\n'; break;
                    case 'r': out[count][dst_i++] = '\r'; break;
                    case 't': out[count][dst_i++] = '\t'; break;
                    default: out[count][dst_i++] = next; break;
                }
                ++i;
            } else {
                out[count][dst_i++] = ch;
            }
        }
        out[count][dst_i] = '\0';
        pos += len;
        if (*pos == '\"') {
            pos++;
        }
        while (*pos && *pos != ',' && *pos != ']') {
            pos++;
        }
        if (*pos == ',') {
            pos++;
        }
        count++;
    }
    return count;
}

static size_t parse_number_array(const char *payload, const char *key, double out[], size_t max_count)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(payload, pattern);
    if (!pos) {
        return 0;
    }
    pos = strchr(pos, '[');
    if (!pos) {
        return 0;
    }
    pos++;
    size_t count = 0;
    while (*pos && count < max_count) {
        while (*pos && isspace((unsigned char)*pos)) {
            pos++;
        }
        if (*pos == ']') {
            pos++;
            break;
        }
        errno = 0;
        char *end = NULL;
        double value = strtod(pos, &end);
        if (end == pos || errno != 0) {
            break;
        }
        out[count++] = value;
        pos = end;
        while (*pos && *pos != ',' && *pos != ']') {
            pos++;
        }
        if (*pos == ',') {
            pos++;
        }
    }
    return count;
}

static bool parse_metric(const char *payload, const char *key, double *value)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *pos = strstr(payload, pattern);
    if (!pos) {
        return false;
    }
    pos += strlen(pattern);
    while (*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    errno = 0;
    char *end = NULL;
    double v = strtod(pos, &end);
    if (end == pos || errno != 0) {
        return false;
    }
    *value = v;
    return true;
}

static void apply_text_update(struct slot_state *slots,
                              size_t index,
                              const char *text,
                              uint64_t now,
                              uint64_t ttl_ms,
                              bool has_ttl,
                              bool *changed)
{
    if (index >= SLOT_COUNT || !text) {
        return;
    }
    if (text[0] == '\0') {
        // Treat empty strings as no-op to avoid wiping other publishers unintentionally.
        return;
    }

    struct slot_state *slot = &slots[index];
    bool protect = has_ttl ? false : ttl_active(slot->text_expiry, now);
    if (protect) {
        if (g_verbose) {
            fprintf(stdout, "Skip text slot %zu update because TTL protected (%.0f ms left)\n",
                    index + 1,
                    (double)(slot->text_expiry - now));
            fflush(stdout);
        }
        return;
    }

    if (!slot->has_text || strcmp(slot->text, text) != 0) {
        *changed = true;
    }

    slot->has_text = true;
    strncpy(slot->text, text, MAX_TEXT_LEN - 1);
    slot->text[MAX_TEXT_LEN - 1] = '\0';
    slot->text_expiry = add_ttl(now, ttl_ms);
}

static void apply_value_update(struct slot_state *slots,
                               size_t index,
                               double value,
                               uint64_t now,
                               uint64_t ttl_ms,
                               bool has_ttl,
                               bool *changed)
{
    if (index >= SLOT_COUNT) {
        return;
    }
    struct slot_state *slot = &slots[index];
    bool protect = has_ttl ? false : ttl_active(slot->value_expiry, now);
    if (protect) {
        if (g_verbose) {
            fprintf(stdout, "Skip value slot %zu update because TTL protected (%.0f ms left)\n",
                    index + 1,
                    (double)(slot->value_expiry - now));
            fflush(stdout);
        }
        return;
    }

    if (!slot->has_value || fabs(slot->value - value) > 1e-6) {
        *changed = true;
    }
    slot->has_value = true;
    slot->value = value;
    slot->value_expiry = add_ttl(now, ttl_ms);
}

static void apply_text_array(struct slot_state *slots,
                             char texts[][MAX_TEXT_LEN],
                             size_t count,
                             uint64_t now,
                             uint64_t ttl_ms,
                             bool has_ttl,
                             bool *changed)
{
    for (size_t i = 0; i < count && i < SLOT_COUNT; ++i) {
        apply_text_update(slots, i, texts[i], now, ttl_ms, has_ttl, changed);
    }
}

static void apply_value_array(struct slot_state *slots,
                              const double values[],
                              size_t count,
                              uint64_t now,
                              uint64_t ttl_ms,
                              bool has_ttl,
                              bool *changed)
{
    for (size_t i = 0; i < count && i < SLOT_COUNT; ++i) {
        apply_value_update(slots, i, values[i], now, ttl_ms, has_ttl, changed);
    }
}

static bool apply_fallback_metrics(const char *payload,
                                   struct slot_state *slots,
                                   uint64_t now,
                                   uint64_t ttl_ms,
                                   bool has_ttl,
                                   bool *changed)
{
    static const struct {
        const char *key;
        const char *label;
    } metrics[] = {
        {"rssi", "RSSI"},
        {"link_tx", "Link TX"},
        {"link_rx", "Link RX"},
        {"link_all", "Link ALL"},
        {"link", "Link"},
    };

    size_t slot_index = 0;
    bool applied = false;
    for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]) && slot_index < SLOT_COUNT; ++i) {
        double value;
        if (!parse_metric(payload, metrics[i].key, &value)) {
            continue;
        }
        apply_text_update(slots, slot_index, metrics[i].label, now, ttl_ms, has_ttl, changed);
        apply_value_update(slots, slot_index, value, now, ttl_ms, has_ttl, changed);
        slot_index++;
        applied = true;
    }
    return applied;
}

static int ensure_unix_socket(int *fd, const char *path)
{
    if (*fd >= 0) {
        return 0;
    }

    int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "socket(AF_UNIX, SOCK_DGRAM) failed: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Socket path too long: %s\n", path);
        close(sock);
        return -1;
    }
    strcpy(addr.sun_path, path);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect(%s) failed: %s\n", path, strerror(errno));
        close(sock);
        return -1;
    }

    *fd = sock;
    if (g_verbose) {
        fprintf(stdout, "Connected to UNIX socket %s\n", path);
        fflush(stdout);
    }
    return 0;
}

static bool build_payload(const struct slot_state *slots,
                          uint64_t now,
                          int default_ttl_ms,
                          char *out,
                          size_t out_len)
{
    size_t off = 0;
    if (out_len == 0) {
        return false;
    }

    int written = snprintf(out + off, out_len - off, "{\"text\":[");
    if (written < 0 || (size_t)written >= out_len - off) {
        return false;
    }
    off += (size_t)written;

    for (size_t i = 0; i < SLOT_COUNT; ++i) {
        if (i > 0) {
            if (off + 1 >= out_len) {
                return false;
            }
            out[off++] = ',';
        }
        char buffer[MAX_TEXT_LEN * 2];
        int str_len = append_json_string(buffer, sizeof(buffer), slots[i].has_text ? slots[i].text : "");
        if (str_len < 0) {
            return false;
        }
        if ((size_t)str_len >= out_len - off) {
            return false;
        }
        memcpy(out + off, buffer, (size_t)str_len);
        off += (size_t)str_len;
    }

    if (off + 11 >= out_len) {
        return false;
    }
    memcpy(out + off, "],\"value\":[", 11);
    off += 11;

    for (size_t i = 0; i < SLOT_COUNT; ++i) {
        if (i > 0) {
            if (off + 1 >= out_len) {
                return false;
            }
            out[off++] = ',';
        }
        double value = slots[i].has_value ? slots[i].value : 0.0;
        written = snprintf(out + off, out_len - off, "%.2f", value);
        if (written < 0 || (size_t)written >= out_len - off) {
            return false;
        }
        off += (size_t)written;
    }

    uint64_t max_remaining = 0;
    for (size_t i = 0; i < SLOT_COUNT; ++i) {
        if (slots[i].has_text && slots[i].text_expiry > now) {
            uint64_t remain = slots[i].text_expiry - now;
            if (remain > max_remaining) {
                max_remaining = remain;
            }
        }
        if (slots[i].has_value && slots[i].value_expiry > now) {
            uint64_t remain = slots[i].value_expiry - now;
            if (remain > max_remaining) {
                max_remaining = remain;
            }
        }
    }

    if (default_ttl_ms > 0 && (uint64_t)default_ttl_ms > max_remaining) {
        max_remaining = (uint64_t)default_ttl_ms;
    }

    if (max_remaining > 0) {
        uint64_t ttl_to_emit = max_remaining;
        if (ttl_to_emit > (uint64_t)INT_MAX) {
            ttl_to_emit = (uint64_t)INT_MAX;
        }
        written = snprintf(out + off, out_len - off, "],\"ttl_ms\":%llu}\n", (unsigned long long)ttl_to_emit);
        if (written < 0 || (size_t)written >= out_len - off) {
            return false;
        }
        off += (size_t)written;
    } else {
        if (off + 3 >= out_len) {
            return false;
        }
        out[off++] = ']';
        out[off++] = '}';
        out[off++] = '\n';
        out[off] = '\0';
        return true;
    }

    return true;
}

int main(int argc, char **argv)
{
    const char *socket_path = "/run/pixelpilot/osd.sock";
    const char *bind_addr = "0.0.0.0";
    int port = 5005;
    int default_ttl_ms = 0;

    static const struct option long_opts[] = {
        {"socket", required_argument, NULL, 's'},
        {"bind", required_argument, NULL, 'b'},
        {"port", required_argument, NULL, 'p'},
        {"ttl", required_argument, NULL, 'T'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };

    for (;;) {
        int opt_index = 0;
        int c = getopt_long(argc, argv, "s:b:p:T:vh", long_opts, &opt_index);
        if (c == -1) {
            break;
        }
        switch (c) {
            case 's': socket_path = optarg; break;
            case 'b': bind_addr = optarg; break;
            case 'p': port = atoi(optarg); break;
            case 'T': default_ttl_ms = atoi(optarg); if (default_ttl_ms < 0) default_ttl_ms = 0; break;
            case 'v': g_verbose = true; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        fprintf(stderr, "socket(AF_INET, SOCK_DGRAM) failed: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (strcmp(bind_addr, "*") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid bind address: %s\n", bind_addr);
        close(udp_fd);
        return 1;
    }

    if (bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind() failed: %s\n", strerror(errno));
        close(udp_fd);
        return 1;
    }

    if (g_verbose) {
        fprintf(stdout, "Listening on %s:%d\n", bind_addr, port);
        fflush(stdout);
    }

    struct slot_state slots[SLOT_COUNT];
    memset(slots, 0, sizeof(slots));

    int unix_fd = -1;
    uint64_t last_connect_attempt = 0;
    const uint64_t reconnect_interval = 1000;

    char udp_buf[UDP_BUFFER];
    char json_buf[JSON_BUFFER];

    while (!g_stop) {
        struct pollfd pfd = {
            .fd = udp_fd,
            .events = POLLIN,
        };

        int rc = poll(&pfd, 1, DEFAULT_POLL_INTERVAL_MS);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "poll() failed: %s\n", strerror(errno));
            break;
        }

        uint64_t now = now_ms();
        bool ttl_changed = false;
        for (size_t i = 0; i < SLOT_COUNT; ++i) {
            struct slot_state *slot = &slots[i];
            if (slot->has_text && slot->text_expiry > 0 && slot->text_expiry <= now) {
                if (g_verbose) {
                    fprintf(stdout, "Text slot %zu expired\n", i + 1);
                    fflush(stdout);
                }
                clear_text(slot);
                ttl_changed = true;
            }
            if (slot->has_value && slot->value_expiry > 0 && slot->value_expiry <= now) {
                if (g_verbose) {
                    fprintf(stdout, "Value slot %zu expired\n", i + 1);
                    fflush(stdout);
                }
                clear_value(slot);
                ttl_changed = true;
            }
        }

        bool packet_applied = false;
        if (rc > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = recvfrom(udp_fd, udp_buf, sizeof(udp_buf) - 1, 0, NULL, NULL);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                fprintf(stderr, "recvfrom() failed: %s\n", strerror(errno));
                break;
            }
            udp_buf[n] = '\0';

            if (g_verbose) {
                fprintf(stdout, "UDP: %s\n", udp_buf);
                fflush(stdout);
            }

            uint64_t ttl_ms_field = 0;
            bool has_ttl = parse_uint_field(udp_buf, "ttl_ms", &ttl_ms_field);
            if (ttl_ms_field > (uint64_t)INT_MAX) {
                ttl_ms_field = (uint64_t)INT_MAX;
            }

            bool changed = false;
            char texts[SLOT_COUNT][MAX_TEXT_LEN];
            size_t text_count = parse_string_array(udp_buf, "text", texts, SLOT_COUNT);
            if (text_count > 0) {
                apply_text_array(slots, texts, text_count, now, ttl_ms_field, has_ttl, &changed);
            }

            double values[SLOT_COUNT];
            size_t value_count = parse_number_array(udp_buf, "value", values, SLOT_COUNT);
            if (value_count > 0) {
                apply_value_array(slots, values, value_count, now, ttl_ms_field, has_ttl, &changed);
            }

            bool fallback_used = false;
            if (text_count == 0 && value_count == 0) {
                fallback_used = apply_fallback_metrics(udp_buf, slots, now, ttl_ms_field, has_ttl, &changed);
            }

            packet_applied = changed || text_count > 0 || value_count > 0 || fallback_used;
        }

        if (!ttl_changed && !packet_applied) {
            continue;
        }

        if (!build_payload(slots, now, default_ttl_ms, json_buf, sizeof(json_buf))) {
            fprintf(stderr, "Failed to build JSON payload\n");
            continue;
        }

        if (unix_fd < 0) {
            if (last_connect_attempt == 0 || now - last_connect_attempt >= reconnect_interval) {
                if (ensure_unix_socket(&unix_fd, socket_path) != 0) {
                    last_connect_attempt = now;
                } else {
                    last_connect_attempt = now;
                }
            }
        }

        if (unix_fd < 0) {
            continue;
        }

        ssize_t sent = send(unix_fd, json_buf, strlen(json_buf), 0);
        if (sent < 0) {
            fprintf(stderr, "send() failed: %s\n", strerror(errno));
            close(unix_fd);
            unix_fd = -1;
            last_connect_attempt = now;
            continue;
        }

        if (g_verbose) {
            fprintf(stdout, "Forwarded: %s", json_buf);
            fflush(stdout);
        }
    }

    if (unix_fd >= 0) {
        close(unix_fd);
    }
    close(udp_fd);
    return 0;
}
