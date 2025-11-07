#include "osd_external.h"
#include "logging.h"

#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

static uint64_t ttl_ms_to_ns(uint64_t ttl_ms) {
    if (ttl_ms == 0) {
        return 0;
    }
    uint64_t ttl_ns = ttl_ms * 1000000ull;
    if (ttl_ns / 1000000ull != ttl_ms) {
        return 0;
    }
    return ttl_ns;
}

static void osd_external_update_expiry_locked(OsdExternalBridge *bridge) {
    if (!bridge) {
        return;
    }
    uint64_t next_expiry = 0;
    for (size_t i = 0; i < OSD_EXTERNAL_MAX_TEXT; ++i) {
        OsdExternalSlotState *slot = &bridge->slots[i];
        if (slot->text_active && slot->text_expiry_ns > 0) {
            if (next_expiry == 0 || slot->text_expiry_ns < next_expiry) {
                next_expiry = slot->text_expiry_ns;
            }
        }
        if (i < OSD_EXTERNAL_MAX_VALUES && slot->value_active && slot->value_expiry_ns > 0) {
            if (next_expiry == 0 || slot->value_expiry_ns < next_expiry) {
                next_expiry = slot->value_expiry_ns;
            }
        }
    }
    bridge->expiry_ns = next_expiry;
    bridge->snapshot.expiry_ns = next_expiry;
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
    memset(&bridge->snapshot.ctm, 0, sizeof(bridge->snapshot.ctm));
    bridge->ctm_serial_counter = 0;
    bridge->expiry_ns = 0;
    memset(bridge->slots, 0, sizeof(bridge->slots));
}

static void osd_external_expire_locked(OsdExternalBridge *bridge, uint64_t now_ns) {
    if (!bridge) {
        return;
    }
    int changed = 0;
    for (size_t i = 0; i < OSD_EXTERNAL_MAX_TEXT; ++i) {
        OsdExternalSlotState *slot = &bridge->slots[i];
        if (slot->text_active && slot->text_expiry_ns > 0 && now_ns >= slot->text_expiry_ns) {
            slot->text_active = 0;
            slot->text_expiry_ns = 0;
            if (bridge->snapshot.text[i][0] != '\0') {
                bridge->snapshot.text[i][0] = '\0';
                changed = 1;
            }
            if (!slot->value_active) {
                slot->is_metric = 0;
            }
        }
        if (slot->value_active && slot->value_expiry_ns > 0 && now_ns >= slot->value_expiry_ns && i < OSD_EXTERNAL_MAX_VALUES) {
            slot->value_active = 0;
            slot->value_expiry_ns = 0;
            bridge->snapshot.value[i] = 0.0;
            if (!slot->text_active) {
                slot->is_metric = 0;
            }
            changed = 1;
        }
        if (!slot->text_active && !slot->value_active) {
            slot->text_expiry_ns = 0;
            slot->value_expiry_ns = 0;
            slot->is_metric = 0;
        }
    }
    osd_external_update_expiry_locked(bridge);
    if (changed) {
        bridge->snapshot.last_update_ns = now_ns;
    }
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
    int has_ctm;
    struct {
        int enable_present;
        int enable;
        int backend_present;
        char backend[16];
        int matrix_present;
        int matrix_count;
        double matrix[9];
        int sharpness_present;
        double sharpness;
        int gamma_value_present;
        double gamma_value;
        int gamma_lift_present;
        double gamma_lift;
        int gamma_gain_present;
        double gamma_gain;
        int gamma_r_mult_present;
        double gamma_r_mult;
        int gamma_g_mult_present;
        double gamma_g_mult;
        int gamma_b_mult_present;
        double gamma_b_mult;
    } ctm;
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
            snprintf(msg->text[idx], sizeof(msg->text[idx]), "%s", tmp);
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

static const char *parse_bool_value(const char *p, int *out) {
    if (!p || !out) {
        return NULL;
    }
    if (strncmp(p, "true", 4) == 0) {
        *out = 1;
        return p + 4;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = 0;
        return p + 5;
    }
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) {
        return NULL;
    }
    *out = (v != 0);
    return end;
}

static const char *parse_double_value(const char *p, double *out) {
    if (!p || !out) {
        return NULL;
    }
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) {
        return NULL;
    }
    *out = v;
    return end;
}

static const char *parse_double_array_into(const char *p, double *out, size_t max_count, int *count_out) {
    if (!p || !out || max_count == 0) {
        return NULL;
    }
    if (*p != '[') {
        return NULL;
    }
    ++p;
    p = skip_ws(p);
    int idx = 0;
    if (*p == ']') {
        if (count_out) {
            *count_out = 0;
        }
        return p + 1;
    }
    while (*p) {
        double value = 0.0;
        const char *next = parse_double_value(p, &value);
        if (!next) {
            return NULL;
        }
        if (idx < (int)max_count) {
            out[idx] = value;
        }
        idx++;
        p = skip_ws(next);
        if (*p == ',') {
            ++p;
            p = skip_ws(p);
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
    if (count_out) {
        *count_out = idx;
    }
    return p + 1;
}

static const char *parse_ctm_object(const char *p, OsdExternalMessage *msg) {
    if (!p || !msg) {
        return NULL;
    }
    if (*p != '{') {
        return NULL;
    }
    ++p;
    p = skip_ws(p);
    while (*p) {
        if (*p == '}') {
            return p + 1;
        }
        if (*p != '"') {
            return NULL;
        }
        char key[32];
        const char *next = parse_string(p, key, sizeof(key));
        if (!next) {
            return NULL;
        }
        p = skip_ws(next);
        if (*p != ':') {
            return NULL;
        }
        ++p;
        p = skip_ws(p);
        if (strcmp(key, "enable") == 0) {
            int flag = 0;
            const char *after = parse_bool_value(p, &flag);
            if (!after) {
                return NULL;
            }
            msg->has_ctm = 1;
            msg->ctm.enable_present = 1;
            msg->ctm.enable = flag ? 1 : 0;
            p = after;
        } else if (strcmp(key, "backend") == 0) {
            if (*p != '"') {
                return NULL;
            }
            char backend[16];
            const char *after = parse_string(p, backend, sizeof(backend));
            if (!after) {
                return NULL;
            }
            for (size_t i = 0; backend[i] != '\0'; ++i) {
                backend[i] = (char)tolower((unsigned char)backend[i]);
            }
            msg->has_ctm = 1;
            msg->ctm.backend_present = 1;
            snprintf(msg->ctm.backend, sizeof(msg->ctm.backend), "%s", backend);
            p = after;
        } else if (strcmp(key, "matrix") == 0) {
            int count = 0;
            const char *after = parse_double_array_into(p, msg->ctm.matrix, sizeof(msg->ctm.matrix) / sizeof(msg->ctm.matrix[0]), &count);
            if (!after) {
                return NULL;
            }
            msg->has_ctm = 1;
            msg->ctm.matrix_present = 1;
            msg->ctm.matrix_count = count;
            p = after;
        } else if (strcmp(key, "sharpness") == 0) {
            double value = 0.0;
            const char *after = parse_double_value(p, &value);
            if (!after) {
                return NULL;
            }
            msg->has_ctm = 1;
            msg->ctm.sharpness_present = 1;
            msg->ctm.sharpness = value;
            p = after;
        } else if (strcmp(key, "gamma") == 0 || strcmp(key, "gamma_value") == 0) {
            double value = 0.0;
            const char *after = parse_double_value(p, &value);
            if (!after) {
                return NULL;
            }
            msg->has_ctm = 1;
            msg->ctm.gamma_value_present = 1;
            msg->ctm.gamma_value = value;
            p = after;
        } else if (strcmp(key, "gamma_lift") == 0 || strcmp(key, "gamma-lift") == 0) {
            double value = 0.0;
            const char *after = parse_double_value(p, &value);
            if (!after) {
                return NULL;
            }
            msg->has_ctm = 1;
            msg->ctm.gamma_lift_present = 1;
            msg->ctm.gamma_lift = value;
            p = after;
        } else if (strcmp(key, "gamma_gain") == 0 || strcmp(key, "gamma-gain") == 0) {
            double value = 0.0;
            const char *after = parse_double_value(p, &value);
            if (!after) {
                return NULL;
            }
            msg->has_ctm = 1;
            msg->ctm.gamma_gain_present = 1;
            msg->ctm.gamma_gain = value;
            p = after;
        } else if (strcmp(key, "gamma_r_mult") == 0 || strcmp(key, "gamma-r-mult") == 0) {
            double value = 0.0;
            const char *after = parse_double_value(p, &value);
            if (!after) {
                return NULL;
            }
            msg->has_ctm = 1;
            msg->ctm.gamma_r_mult_present = 1;
            msg->ctm.gamma_r_mult = value;
            p = after;
        } else if (strcmp(key, "gamma_g_mult") == 0 || strcmp(key, "gamma-g-mult") == 0) {
            double value = 0.0;
            const char *after = parse_double_value(p, &value);
            if (!after) {
                return NULL;
            }
            msg->has_ctm = 1;
            msg->ctm.gamma_g_mult_present = 1;
            msg->ctm.gamma_g_mult = value;
            p = after;
        } else if (strcmp(key, "gamma_b_mult") == 0 || strcmp(key, "gamma-b-mult") == 0) {
            double value = 0.0;
            const char *after = parse_double_value(p, &value);
            if (!after) {
                return NULL;
            }
            msg->has_ctm = 1;
            msg->ctm.gamma_b_mult_present = 1;
            msg->ctm.gamma_b_mult = value;
            p = after;
        } else {
            // Unknown key: reuse skip logic from parse_message
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
                            return NULL;
                        }
                        p = tmp;
                        continue;
                    }
                    ++p;
                }
                if (depth != 0) {
                    return NULL;
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
                            return NULL;
                        }
                        p = tmp;
                        continue;
                    }
                    ++p;
                }
                if (depth != 0) {
                    return NULL;
                }
            } else if (*p == '"') {
                p = parse_string(p, NULL, 0);
                if (!p) {
                    return NULL;
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
            p = skip_ws(p);
            continue;
        }
        if (*p == '}') {
            return p + 1;
        }
        return NULL;
    }
    return NULL;
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
        } else if (strcmp(key, "ctm") == 0) {
            p = parse_ctm_object(p, msg);
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
    osd_external_expire_locked(bridge, now_ns);

    size_t slot_count = OSD_EXTERNAL_MAX_TEXT;
    size_t value_limit = OSD_EXTERNAL_MAX_VALUES;
    if (value_limit > slot_count) {
        value_limit = slot_count;
    }

    int has_ttl_field = msg->has_ttl;
    uint64_t ttl_ns = ttl_ms_to_ns(msg->ttl_ms);
    int changed = 0;

    if (msg->has_text && msg->text_count == 0) {
        for (size_t i = 0; i < OSD_EXTERNAL_MAX_TEXT; ++i) {
            OsdExternalSlotState *slot = &bridge->slots[i];
            if (slot->text_active || bridge->snapshot.text[i][0] != '\0') {
                changed = 1;
            }
            slot->text_active = 0;
            slot->text_expiry_ns = 0;
            bridge->snapshot.text[i][0] = '\0';
            if (!slot->value_active) {
                slot->is_metric = 0;
            }
        }
    }

    if (msg->has_value && msg->value_count == 0) {
        for (size_t i = 0; i < value_limit; ++i) {
            OsdExternalSlotState *slot = &bridge->slots[i];
            if (slot->value_active || bridge->snapshot.value[i] != 0.0) {
                changed = 1;
            }
            slot->value_active = 0;
            slot->value_expiry_ns = 0;
            bridge->snapshot.value[i] = 0.0;
            if (!slot->text_active) {
                slot->is_metric = 0;
            }
        }
    }

    size_t count = msg->text_count > msg->value_count ? msg->text_count : msg->value_count;
    if (count > slot_count) {
        count = slot_count;
    }

    for (size_t i = 0; i < count; ++i) {
        OsdExternalSlotState *slot = &bridge->slots[i];
        const char *incoming_text = (i < (size_t)msg->text_count) ? msg->text[i] : "";
        int text_nonempty = incoming_text && incoming_text[0] != '\0';
        int value_present = (i < (size_t)msg->value_count);
        double incoming_value = (value_present && i < value_limit) ? msg->value[i] : 0.0;

        if (text_nonempty) {
            int ttl_guard = slot->text_active && slot->text_expiry_ns > now_ns && !has_ttl_field;
            if (!ttl_guard) {
                if (!slot->text_active || strncmp(bridge->snapshot.text[i], incoming_text, sizeof(bridge->snapshot.text[i])) != 0) {
                    changed = 1;
                }
                snprintf(bridge->snapshot.text[i], sizeof(bridge->snapshot.text[i]), "%s", incoming_text);
                slot->text_active = 1;
                if (has_ttl_field) {
                    if (ttl_ns > 0 && ttl_ns <= UINT64_MAX - now_ns) {
                        slot->text_expiry_ns = now_ns + ttl_ns;
                    } else if (ttl_ns > 0) {
                        slot->text_expiry_ns = 0;
                    } else {
                        slot->text_expiry_ns = 0;
                    }
                } else {
                    slot->text_expiry_ns = 0;
                }
                if (!value_present || i >= value_limit) {
                    if (slot->value_active) {
                        changed = 1;
                    }
                    slot->value_active = 0;
                    slot->value_expiry_ns = 0;
                    if (i < value_limit) {
                        bridge->snapshot.value[i] = 0.0;
                    }
                    slot->is_metric = 0;
                }
            }
        }

        if (value_present && i < value_limit) {
            int ttl_guard = slot->value_active && slot->value_expiry_ns > now_ns && !has_ttl_field;
            int locked_by_text = slot->text_active && slot->text_expiry_ns > now_ns && !slot->is_metric && !has_ttl_field;
            if (!ttl_guard && !locked_by_text) {
                if (!slot->value_active || bridge->snapshot.value[i] != incoming_value) {
                    changed = 1;
                }
                bridge->snapshot.value[i] = incoming_value;
                slot->value_active = 1;
                slot->is_metric = 1;
                if (has_ttl_field) {
                    if (ttl_ns > 0 && ttl_ns <= UINT64_MAX - now_ns) {
                        slot->value_expiry_ns = now_ns + ttl_ns;
                    } else if (ttl_ns > 0) {
                        slot->value_expiry_ns = 0;
                    } else {
                        slot->value_expiry_ns = 0;
                    }
                } else {
                    slot->value_expiry_ns = 0;
                }
            }
        }

        if (!slot->text_active && !slot->value_active) {
            slot->is_metric = 0;
            slot->text_expiry_ns = 0;
            slot->value_expiry_ns = 0;
            if (bridge->snapshot.text[i][0] != '\0') {
                bridge->snapshot.text[i][0] = '\0';
                changed = 1;
            }
            if (i < value_limit && bridge->snapshot.value[i] != 0.0) {
                bridge->snapshot.value[i] = 0.0;
                changed = 1;
            }
        }
    }

    osd_external_update_expiry_locked(bridge);
    if (changed) {
        bridge->snapshot.last_update_ns = now_ns;
    }
    if (msg->has_ctm) {
        memset(&bridge->snapshot.ctm, 0, sizeof(bridge->snapshot.ctm));
        bridge->snapshot.ctm.present = 1;
        bridge->snapshot.ctm.serial = ++bridge->ctm_serial_counter;
        if (msg->ctm.enable_present) {
            bridge->snapshot.ctm.enable_present = 1;
            bridge->snapshot.ctm.enable = msg->ctm.enable ? 1 : 0;
        }
        if (msg->ctm.backend_present) {
            bridge->snapshot.ctm.backend_present = 1;
            snprintf(bridge->snapshot.ctm.backend, sizeof(bridge->snapshot.ctm.backend), "%s", msg->ctm.backend);
        }
        if (msg->ctm.matrix_present) {
            bridge->snapshot.ctm.matrix_present = 1;
            bridge->snapshot.ctm.matrix_count = msg->ctm.matrix_count;
            int limit = msg->ctm.matrix_count;
            if (limit > (int)(sizeof(bridge->snapshot.ctm.matrix) / sizeof(bridge->snapshot.ctm.matrix[0]))) {
                limit = (int)(sizeof(bridge->snapshot.ctm.matrix) / sizeof(bridge->snapshot.ctm.matrix[0]));
            }
            for (int i = 0; i < limit; ++i) {
                bridge->snapshot.ctm.matrix[i] = msg->ctm.matrix[i];
            }
        }
        if (msg->ctm.sharpness_present) {
            bridge->snapshot.ctm.sharpness_present = 1;
            bridge->snapshot.ctm.sharpness = msg->ctm.sharpness;
        }
        if (msg->ctm.gamma_value_present) {
            bridge->snapshot.ctm.gamma_value_present = 1;
            bridge->snapshot.ctm.gamma_value = msg->ctm.gamma_value;
        }
        if (msg->ctm.gamma_lift_present) {
            bridge->snapshot.ctm.gamma_lift_present = 1;
            bridge->snapshot.ctm.gamma_lift = msg->ctm.gamma_lift;
        }
        if (msg->ctm.gamma_gain_present) {
            bridge->snapshot.ctm.gamma_gain_present = 1;
            bridge->snapshot.ctm.gamma_gain = msg->ctm.gamma_gain;
        }
        if (msg->ctm.gamma_r_mult_present) {
            bridge->snapshot.ctm.gamma_r_mult_present = 1;
            bridge->snapshot.ctm.gamma_r_mult = msg->ctm.gamma_r_mult;
        }
        if (msg->ctm.gamma_g_mult_present) {
            bridge->snapshot.ctm.gamma_g_mult_present = 1;
            bridge->snapshot.ctm.gamma_g_mult = msg->ctm.gamma_g_mult;
        }
        if (msg->ctm.gamma_b_mult_present) {
            bridge->snapshot.ctm.gamma_b_mult_present = 1;
            bridge->snapshot.ctm.gamma_b_mult = msg->ctm.gamma_b_mult;
        }
        bridge->snapshot.last_update_ns = now_ns;
    }
    pthread_mutex_unlock(&bridge->lock);
}

static void *osd_external_thread(void *arg) {
    OsdExternalBridge *bridge = (OsdExternalBridge *)arg;
    if (!bridge) {
        return NULL;
    }
    pthread_mutex_lock(&bridge->lock);
    bridge->snapshot.status = OSD_EXTERNAL_STATUS_LISTENING;
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
            bridge->snapshot.status = OSD_EXTERNAL_STATUS_ERROR;
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
            bridge->snapshot.status = OSD_EXTERNAL_STATUS_ERROR;
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
    bridge->snapshot.status = (bridge->sock_fd >= 0) ? bridge->snapshot.status : OSD_EXTERNAL_STATUS_DISABLED;
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
    bridge->snapshot.status = OSD_EXTERNAL_STATUS_DISABLED;
}

static void close_socket(OsdExternalBridge *bridge) {
    if (!bridge) {
        return;
    }
    if (bridge->sock_fd >= 0) {
        close(bridge->sock_fd);
        bridge->sock_fd = -1;
    }
    bridge->bind_address[0] = '\0';
    bridge->udp_port = 0;
}

int osd_external_start(OsdExternalBridge *bridge, const char *bind_address, int udp_port) {
    if (!bridge) {
        return -1;
    }
    osd_external_stop(bridge);
    if (!bind_address || bind_address[0] == '\0') {
        bind_address = "0.0.0.0";
    }
    if (udp_port <= 0 || udp_port > 65535) {
        LOGW("OSD external feed: invalid UDP port %d", udp_port);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOGW("OSD external feed: socket() failed: %s", strerror(errno));
        pthread_mutex_lock(&bridge->lock);
        bridge->snapshot.status = OSD_EXTERNAL_STATUS_ERROR;
        pthread_mutex_unlock(&bridge->lock);
        return -1;
    }
    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        LOGW("OSD external feed: setsockopt(SO_REUSEADDR) failed: %s", strerror(errno));
        // continue despite failure
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)udp_port);
    if (strcmp(bind_address, "*") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, bind_address, &addr.sin_addr) != 1) {
        LOGW("OSD external feed: inet_pton failed for %s", bind_address);
        close(fd);
        pthread_mutex_lock(&bridge->lock);
        bridge->snapshot.status = OSD_EXTERNAL_STATUS_ERROR;
        pthread_mutex_unlock(&bridge->lock);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOGW("OSD external feed: bind(%s:%d) failed: %s", bind_address, udp_port, strerror(errno));
        close(fd);
        pthread_mutex_lock(&bridge->lock);
        bridge->snapshot.status = OSD_EXTERNAL_STATUS_ERROR;
        pthread_mutex_unlock(&bridge->lock);
        return -1;
    }
    pthread_mutex_lock(&bridge->lock);
    bridge->sock_fd = fd;
    snprintf(bridge->bind_address, sizeof(bridge->bind_address), "%s", bind_address);
    bridge->udp_port = udp_port;
    bridge->stop_flag = 0;
    bridge->snapshot.status = OSD_EXTERNAL_STATUS_LISTENING;
    pthread_mutex_unlock(&bridge->lock);
    if (pthread_create(&bridge->thread, NULL, osd_external_thread, bridge) != 0) {
        LOGW("OSD external feed: pthread_create failed: %s", strerror(errno));
        close_socket(bridge);
        pthread_mutex_lock(&bridge->lock);
        bridge->snapshot.status = OSD_EXTERNAL_STATUS_ERROR;
        pthread_mutex_unlock(&bridge->lock);
        return -1;
    }
    pthread_mutex_lock(&bridge->lock);
    bridge->thread_started = 1;
    pthread_mutex_unlock(&bridge->lock);
    LOGI("OSD external feed: listening on %s:%d", bridge->bind_address, bridge->udp_port);
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
    int fd_to_close = bridge->sock_fd;
    bridge->sock_fd = -1;
    pthread_mutex_unlock(&bridge->lock);
    if (fd_to_close >= 0) {
        shutdown(fd_to_close, SHUT_RDWR);
        close(fd_to_close);
    }
    if (running) {
        pthread_join(bridge->thread, NULL);
    }
    close_socket(bridge);
    pthread_mutex_lock(&bridge->lock);
    bridge->snapshot.status = OSD_EXTERNAL_STATUS_DISABLED;
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
