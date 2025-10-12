// osd_feed_test.c
// Send dummy data for {ext.text1..8} and {ext.value1..8} to a UNIX DGRAM socket.
// - Text rows show TICK that increments by 1 each second.
// - Values follow sine curves (0.5 Hz) with per-channel phase offsets.
// Build:  gcc -O2 -Wall -std=c11 osd_feed_test.c -o osd_feed_test -lm

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
#include <math.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <getopt.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [-s SOCKET] [-i MS] [-n COUNT] [-T TTL_MS] [--clear]\n"
        "  -s, --socket   Path to UNIX DGRAM socket (default: /run/pixelpilot/osd.sock)\n"
        "  -i, --interval Send every N milliseconds (default: 0 = send once)\n"
        "  -n, --count    Send this many messages (default: 0 = infinite if interval>0, else 1)\n"
        "  -T, --ttl      Include ttl_ms in JSON (default: 0 = omit)\n"
        "      --clear    Send empty arrays to clear the snapshot (overrides dummy data)\n",
        argv0);
}

static int send_json(int fd, const char *sock_path, const char *json)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Socket path too long: %s\n", sock_path);
        return -1;
    }
    strcpy(addr.sun_path, sock_path);

    size_t len = strlen(json);
    ssize_t sent = sendto(fd, json, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    if (sent < 0) {
        fprintf(stderr, "sendto() failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *sock_path = "/run/pixelpilot/osd.sock";
    int interval_ms = 0;
    int count = 0;            // 0 = single shot if interval==0, infinite if interval>0
    int ttl_ms = 0;           // 0 = omit
    int clear_flag = 0;

    // --clear handled via custom return code 1000
    static struct option long_opts[] = {
        {"socket",   required_argument, 0, 's'},
        {"interval", required_argument, 0, 'i'},
        {"count",    required_argument, 0, 'n'},
        {"ttl",      required_argument, 0, 'T'},
        {"clear",    no_argument,       0, 1000},
        {"help",     no_argument,       0, 'h'},
        {0,0,0,0}
    };

    for (;;) {
        int opt, idx=0;
        opt = getopt_long(argc, argv, "s:i:n:T:h", long_opts, &idx);
        if (opt == -1) break;
        switch (opt) {
            case 's': sock_path = optarg; break;
            case 'i': interval_ms = atoi(optarg); break;
            case 'n': count = atoi(optarg); break;
            case 'T': ttl_ms = atoi(optarg); break;
            case 1000: clear_flag = 1; break;  // --clear
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket(AF_UNIX,SOCK_DGRAM) failed: %s\n", strerror(errno));
        return 1;
    }

    int sent_messages = 0;
    uint64_t t0_ms = now_ms();

    // Sine parameters
    const double freq_hz = 0.5;                 // 0.5 Hz → 2 s period
    const double omega   = 2.0 * 3.14159265358979323846 * freq_hz; // 2πf
    const double amplitude = 1.0;               // [-1, +1]
    const double offset    = 0.0;               // center at 0
    double phase[8];
    for (int i = 0; i < 8; i++) {
        phase[i] = i * (3.14159265358979323846 / 4.0); // 45° steps
    }

    for (;;) {
        if (g_stop) break;

        // Elapsed seconds (high-res), tick is integer seconds since start
        uint64_t now = now_ms();
        double elapsed_s = (now - t0_ms) / 1000.0;
        int tick = (int)floor(elapsed_s);  // increments by 1 each second

        // Build payload
        char buf[2048];

        if (clear_flag) {
            if (ttl_ms > 0)
                snprintf(buf, sizeof(buf), "{\"text\":[],\"value\":[],\"ttl_ms\":%d}\n", ttl_ms);
            else
                snprintf(buf, sizeof(buf), "{\"text\":[],\"value\":[]}\n");
        } else {
            char text_part[1024] = {0};
            char value_part[512] = {0};

            // Text rows (<=64 chars each as per receiver note)
            int off = 0;
            off += snprintf(text_part + off, sizeof(text_part) - off,
                            "[\"ROW1 TICK=%d\",\"ROW2 TICK=%d\",\"ROW3 TICK=%d\",\"ROW4 TICK=%d\",",
                            tick, tick, tick, tick);
            off += snprintf(text_part + off, sizeof(text_part) - off,
                            "\"ROW5 TICK=%d\",\"ROW6 TICK=%d\",\"ROW7 TICK=%d\",\"ROW8 TICK=%d\"]",
                            tick, tick, tick, tick);

            // Sine wave values for 8 channels with phase offsets
            double v[8];
            for (int i = 0; i < 8; i++) {
                v[i] = offset + amplitude * sin(omega * elapsed_s + phase[i]);
            }
            snprintf(value_part, sizeof(value_part),
                     "[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f]",
                     v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7]);

            if (ttl_ms > 0)
                snprintf(buf, sizeof(buf),
                         "{\"text\":%s,\"value\":%s,\"ttl_ms\":%d}\n",
                         text_part, value_part, ttl_ms);
            else
                snprintf(buf, sizeof(buf),
                         "{\"text\":%s,\"value\":%s}\n",
                         text_part, value_part);
        }

        if (send_json(fd, sock_path, buf) != 0) {
            // Non-fatal; prints error inside send_json
        } else {
            fprintf(stdout, "Sent: %s", buf);
            fflush(stdout);
        }

        sent_messages++;
        if (interval_ms <= 0) break;               // single shot by default
        if (count > 0 && sent_messages >= count) break;
        if (g_stop) break;

        struct timespec ts;
        ts.tv_sec  = interval_ms / 1000;
        ts.tv_nsec = (interval_ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }

    close(fd);
    return 0;
}
