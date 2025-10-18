#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <ctype.h>

static volatile sig_atomic_t g_stop = 0;
static bool g_verbose = false;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [-s SOCKET] [-p PORT] [-b ADDR] [-T TTL_MS] [-v]\n"
        "  -s, --socket   Path to UNIX DGRAM socket (default: /run/pixelpilot/osd.sock)\n"
        "  -p, --port     UDP port to listen on (default: 5005)\n"
        "  -b, --bind     UDP bind address (default: 0.0.0.0)\n"
        "  -T, --ttl      Include ttl_ms in JSON (default: 0 = omit)\n"
        "  -v, --verbose  Enable verbose logging to stdout\n",
        argv0);
}

static int send_json(int fd, const char *sock_path, const char *json)
{
    size_t len = strlen(json);
    ssize_t sent = send(fd, json, len, 0);
    if (sent < 0) {
        fprintf(stderr, "send() to %s failed: %s\n", sock_path, strerror(errno));
        return -1;
    }
    return 0;
}

#define MAX_ENTRIES 8

static void copy_label(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strnlen(src, dst_len - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

struct feed_slot {
    bool active;
    bool is_metric;
    bool has_value;
    char text[64];
    double value;
    uint64_t expiry_ms;
    uint64_t last_emit_ms;
    unsigned long long send_counter;
};

static bool parse_metric(const char *payload, const char *key, double *out) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *pos = strstr(payload, pattern);
    if (!pos) return false;
    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t') {
        pos++;
    }
    errno = 0;
    char *endptr = NULL;
    double value = strtod(pos, &endptr);
    if (errno != 0 || endptr == pos) {
        return false;
    }
    *out = value;
    return true;
}

static size_t parse_string_array(const char *payload, const char *key, char out[][64], size_t max) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *pos = strstr(payload, pattern);
    if (!pos) return 0;
    pos = strchr(pos, '[');
    if (!pos) return 0;
    pos++;
    size_t count = 0;
    while (*pos && count < max) {
        while (*pos && isspace((unsigned char)*pos)) pos++;
        if (*pos == ']') break;
        if (*pos != '"') break;
        pos++;
        size_t len = 0;
        while (pos[len] && pos[len] != '"') {
            if (pos[len] == '\\' && pos[len + 1]) {
                len += 2;
            } else {
                len++;
            }
        }
        size_t copy = 0;
        for (size_t i = 0; i < len && copy + 1 < 64; ++i) {
            if (pos[i] == '\\' && i + 1 < len) {
                out[count][copy++] = pos[i + 1];
                ++i;
            } else {
                out[count][copy++] = pos[i];
            }
        }
        out[count][copy] = '\0';
        pos += len;
        if (*pos == '"') pos++;
        while (*pos && *pos != ',' && *pos != ']') pos++;
        if (*pos == ',') pos++;
        count++;
    }
    return count;
}

static size_t parse_number_array(const char *payload, const char *key, double out[], size_t max) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *pos = strstr(payload, pattern);
    if (!pos) return 0;
    pos = strchr(pos, '[');
    if (!pos) return 0;
    pos++;
    size_t count = 0;
    while (*pos && count < max) {
        while (*pos && isspace((unsigned char)*pos)) pos++;
        if (*pos == ']') break;
        char *endptr = NULL;
        double value = strtod(pos, &endptr);
        if (endptr == pos) {
            break;
        }
        out[count++] = value;
        pos = endptr;
        while (*pos && *pos != ',' && *pos != ']') pos++;
        if (*pos == ',') pos++;
    }
    return count;
}

static bool parse_uint_field(const char *payload, const char *key, uint64_t *out) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *pos = strstr(payload, pattern);
    if (!pos) {
        return false;
    }
    pos += strlen(pattern);
    while (*pos && isspace((unsigned char)*pos)) pos++;
    errno = 0;
    char *endptr = NULL;
    unsigned long long value = strtoull(pos, &endptr, 10);
    if (errno != 0 || endptr == pos) {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static size_t extract_known_metrics(const char *payload, char labels[][64], double values[], size_t max) {
    struct key_map { const char *key; const char *label; };
    static const struct key_map fallback_keys[] = {
        {"rssi", "RSSI"},
        {"link_tx", "Link TX"},
        {"link_rx", "Link RX"},
        {"link_all", "Link ALL"},
        {"link", "Link"}
    };

    size_t count = 0;
    for (size_t i = 0; i < sizeof(fallback_keys)/sizeof(fallback_keys[0]) && count < max; ++i) {
        double value;
        if (parse_metric(payload, fallback_keys[i].key, &value)) {
            bool duplicate = false;
            for (size_t j = 0; j < count; ++j) {
                if (strcmp(labels[j], fallback_keys[i].label) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            copy_label(labels[count], sizeof(labels[count]), fallback_keys[i].label);
            values[count] = value;
            count++;
        }
    }
    return count;
}

static int build_osd_payload(const char *texts[], const double values[],
                             const bool present[], size_t count,
                             int ttl_ms, char *out, size_t out_len) {
    char text_part[256] = "[";
    char value_part[256] = "[";
    size_t text_off = 1;
    size_t value_off = 1;
    bool first = true;
    for (size_t i = 0; i < count; i++) {
        if (!present[i]) continue;
        if (!texts[i]) continue;
        if (!first) {
            if (text_off + 1 >= sizeof(text_part) || value_off + 1 >= sizeof(value_part)) {
                return -1;
            }
            text_part[text_off++] = ',';
            value_part[value_off++] = ',';
        }
        int written_text = snprintf(text_part + text_off, sizeof(text_part) - text_off,
                                    "\"%s\"", texts[i]);
        if (written_text < 0 || text_off + (size_t)written_text >= sizeof(text_part)) {
            return -1;
        }
        text_off += (size_t)written_text;

        int written_value = snprintf(value_part + value_off, sizeof(value_part) - value_off,
                                     "%.2f", values[i]);
        if (written_value < 0 || value_off + (size_t)written_value >= sizeof(value_part)) {
            return -1;
        }
        value_off += (size_t)written_value;
        first = false;
    }

    if (text_off + 1 >= sizeof(text_part) || value_off + 1 >= sizeof(value_part)) {
        return -1;
    }
    text_part[text_off++] = ']';
    text_part[text_off] = '\0';
    value_part[value_off++] = ']';
    value_part[value_off] = '\0';

    if (ttl_ms > 0) {
        return snprintf(out, out_len,
                        "{\"text\":%s,\"value\":%s,\"ttl_ms\":%d}\n",
                        text_part, value_part, ttl_ms);
    }
    return snprintf(out, out_len,
                    "{\"text\":%s,\"value\":%s}\n",
                    text_part, value_part);
}

static int ensure_unix_connection(int *fd, const char *sock_path)
{
    if (*fd >= 0) {
        return 0;
    }

    int new_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (new_fd < 0) {
        fprintf(stderr, "socket(AF_UNIX,SOCK_DGRAM) failed: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Socket path too long: %s\n", sock_path);
        close(new_fd);
        return -1;
    }
    strcpy(addr.sun_path, sock_path);

    if (connect(new_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect(%s) failed: %s\n", sock_path, strerror(errno));
        close(new_fd);
        return -1;
    }

    *fd = new_fd;
    if (g_verbose) {
        fprintf(stdout, "Connected to UNIX socket %s\n", sock_path);
        fflush(stdout);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *sock_path = "/run/pixelpilot/osd.sock";
    const char *bind_addr = "0.0.0.0";
    int udp_port = 5005;
    int ttl_ms = 0;

    static struct option long_opts[] = {
        {"socket", required_argument, 0, 's'},
        {"port",   required_argument, 0, 'p'},
        {"bind",   required_argument, 0, 'b'},
        {"ttl",    required_argument, 0, 'T'},
        {"verbose",no_argument,       0, 'v'},
        {"help",   no_argument,       0, 'h'},
        {0,0,0,0}
    };

    for (;;) {
        int opt, idx=0;
        opt = getopt_long(argc, argv, "s:p:b:T:vh", long_opts, &idx);
        if (opt == -1) break;
        switch (opt) {
            case 's': sock_path = optarg; break;
            case 'p': udp_port = atoi(optarg); break;
            case 'b': bind_addr = optarg; break;
            case 'T': ttl_ms = atoi(optarg); break;
            case 'v': g_verbose = true; break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    int unix_fd = -1;
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        fprintf(stderr, "socket(AF_INET,SOCK_DGRAM) failed: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)udp_port);
    if (strcmp(bind_addr, "*") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid bind address: %s\n", bind_addr);
        close(udp_fd);
        return 1;
    }

    if (bind(udp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind() failed: %s\n", strerror(errno));
        close(udp_fd);
        return 1;
    }

    if (g_verbose) {
        fprintf(stdout, "Listening on %s:%d for UDP metrics\n", bind_addr, udp_port);
        fflush(stdout);
    }

    struct feed_slot slots[MAX_ENTRIES] = {0};

    const uint64_t connect_retry_ms = 1000;
    uint64_t last_connect_attempt_ms = 0;

    char udp_buf[512];
    char json_buf[512];
    while (!g_stop) {
        struct pollfd pfd = {
            .fd = udp_fd,
            .events = POLLIN
        };

        int poll_rc = poll(&pfd, 1, 1000);
        if (poll_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "poll() failed: %s\n", strerror(errno));
            break;
        }

        uint64_t now = now_ms();
        bool packet_updated = false;
        bool state_changed = false;

        if (poll_rc > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = recvfrom(udp_fd, udp_buf, sizeof(udp_buf) - 1, 0, NULL, NULL);
            if (n < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "recvfrom() failed: %s\n", strerror(errno));
            } else {
                udp_buf[n] = '\0';

                char parsed_texts[MAX_ENTRIES][64];
                double parsed_values[MAX_ENTRIES];
                size_t text_count = parse_string_array(udp_buf, "text", parsed_texts, MAX_ENTRIES);
                size_t value_count = parse_number_array(udp_buf, "value", parsed_values, MAX_ENTRIES);
                uint64_t ttl_ms_field = 0;
                bool has_ttl_field = parse_uint_field(udp_buf, "ttl_ms", &ttl_ms_field);

                if (text_count > 0 || value_count > 0) {
                    size_t count = text_count > value_count ? text_count : value_count;
                    for (size_t i = 0; i < count && i < MAX_ENTRIES; ++i) {
                        const char *incoming_text = (i < text_count) ? parsed_texts[i] : "";
                        bool text_nonempty = incoming_text && incoming_text[0] != '\0';
                        bool has_value = (i < value_count);
                        double value = has_value ? parsed_values[i] : 0.0;

                        if (!text_nonempty && !has_value) {
                            continue;
                        }

                        struct feed_slot *slot = &slots[i];
                        bool prev_active = slot->active;
                        bool prev_metric = slot->is_metric;
                        double prev_value = slot->value;
                        char prev_text[64];
                        copy_label(prev_text, sizeof(prev_text), slot->text);

                        if (has_value) {
                            slot->is_metric = true;
                            slot->has_value = true;
                            slot->value = value;
                        } else {
                            slot->has_value = false;
                            slot->value = 0.0;
                        }

                        if (text_nonempty) {
                            copy_label(slot->text, sizeof(slot->text), incoming_text);
                            if (!has_value) {
                                slot->is_metric = false;
                            }
                        }

                        slot->active = true;
                        if (has_ttl_field) {
                            slot->expiry_ms = ttl_ms_field > 0 ? now + ttl_ms_field : 0;
                        } else {
                            slot->expiry_ms = 0;
                        }
                        if (!prev_active) {
                            slot->send_counter = 0;
                            slot->last_emit_ms = 0;
                        }

                        if (!prev_active || prev_metric != slot->is_metric ||
                            strcmp(prev_text, slot->text) != 0 ||
                            (slot->has_value && (!prev_active || fabs(prev_value - slot->value) > 0.0001))) {
                            state_changed = true;
                        }
                        packet_updated = true;
                    }
                } else {
                    char fallback_labels[MAX_ENTRIES][64];
                    double fallback_values[MAX_ENTRIES];
                    size_t fallback_count = extract_known_metrics(udp_buf, fallback_labels, fallback_values, MAX_ENTRIES);
                    if (fallback_count > 0) {
                        for (size_t i = 0; i < fallback_count && i < MAX_ENTRIES; ++i) {
                            struct feed_slot *slot = &slots[i];
                            bool prev_active = slot->active;
                            double prev_value = slot->value;
                            char prev_text[64];
                            copy_label(prev_text, sizeof(prev_text), slot->text);

                            copy_label(slot->text, sizeof(slot->text), fallback_labels[i]);
                            slot->is_metric = true;
                            slot->has_value = true;
                            slot->value = fallback_values[i];
                            slot->active = true;
                            if (has_ttl_field) {
                                slot->expiry_ms = ttl_ms_field > 0 ? now + ttl_ms_field : 0;
                            } else {
                                slot->expiry_ms = 0;
                            }
                            if (!prev_active) {
                                slot->send_counter = 0;
                                slot->last_emit_ms = 0;
                            }

                            if (!prev_active || strcmp(prev_text, slot->text) != 0 ||
                                fabs(prev_value - slot->value) > 0.0001) {
                                state_changed = true;
                            }
                        }
                        packet_updated = true;
                    }
                }
            }
        }

        for (size_t i = 0; i < MAX_ENTRIES; ++i) {
            struct feed_slot *slot = &slots[i];
            if (!slot->active) {
                continue;
            }
            if (slot->expiry_ms > 0 && now >= slot->expiry_ms) {
                slot->active = false;
                slot->has_value = false;
                slot->is_metric = false;
                slot->text[0] = '\0';
                slot->value = 0.0;
                slot->expiry_ms = 0;
                slot->last_emit_ms = 0;
                slot->send_counter = 0;
                state_changed = true;
            }
        }

        if (!packet_updated && !state_changed) {
            continue;
        }

        char text_buf[MAX_ENTRIES][64];
        const char *text_ptrs[MAX_ENTRIES];
        bool present_arr[MAX_ENTRIES];
        double values_arr[MAX_ENTRIES];
        size_t emit_count = MAX_ENTRIES;
        bool any_ttl_slots = false;
        uint64_t max_remaining_ttl_ms = 0;

        for (size_t i = 0; i < MAX_ENTRIES; ++i) {
            struct feed_slot *slot = &slots[i];

            text_ptrs[i] = text_buf[i];
            present_arr[i] = true;

            if (!slot->active) {
                text_buf[i][0] = '\0';
                values_arr[i] = 0.0;
                continue;
            }

            if (slot->expiry_ms > 0) {
                any_ttl_slots = true;
                if (slot->expiry_ms > now) {
                    uint64_t remaining = slot->expiry_ms - now;
                    if (remaining > max_remaining_ttl_ms) {
                        max_remaining_ttl_ms = remaining;
                    }
                }
            }

            double freq_hz = 0.0;
            if (slot->last_emit_ms != 0) {
                uint64_t delta_ms = now - slot->last_emit_ms;
                if (delta_ms > 0) {
                    freq_hz = 1000.0 / (double)delta_ms;
                }
            }

            if (slot->is_metric) {
                snprintf(text_buf[i], sizeof(text_buf[i]),
                         "%.32s #%llu @ %.2f Hz",
                         slot->text[0] ? slot->text : "Metric",
                         slot->send_counter + 1,
                         freq_hz);
            } else {
                copy_label(text_buf[i], sizeof(text_buf[i]), slot->text);
            }

            values_arr[i] = slot->has_value ? slot->value : 0.0;

            slot->last_emit_ms = now;
            slot->send_counter++;
        }

        int message_ttl_ms = ttl_ms;
        if (any_ttl_slots) {
            uint64_t chosen = max_remaining_ttl_ms;
            if (ttl_ms > 0 && (uint64_t)ttl_ms > chosen) {
                chosen = (uint64_t)ttl_ms;
            }
            if (chosen > (uint64_t)INT_MAX) {
                chosen = (uint64_t)INT_MAX;
            }
            if (chosen > 0 && chosen < (uint64_t)INT_MAX) {
                message_ttl_ms = (int)chosen;
            } else if (chosen == 0) {
                message_ttl_ms = 1;
            } else {
                message_ttl_ms = INT_MAX;
            }
        }

        int written = build_osd_payload(text_ptrs, values_arr, present_arr,
                                        emit_count, message_ttl_ms, json_buf, sizeof(json_buf));
        if (written < 0 || (size_t)written >= sizeof(json_buf)) {
            fprintf(stderr, "Failed to build JSON payload\n");
            continue;
        }

        if (unix_fd < 0) {
            if (last_connect_attempt_ms == 0 || (now - last_connect_attempt_ms) >= connect_retry_ms) {
                if (ensure_unix_connection(&unix_fd, sock_path) != 0) {
                    last_connect_attempt_ms = now;
                } else {
                    last_connect_attempt_ms = now;
                }
            }
        }

        if (unix_fd < 0) {
            continue;
        }

        if (send_json(unix_fd, sock_path, json_buf) != 0) {
            close(unix_fd);
            unix_fd = -1;
            last_connect_attempt_ms = now;
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
