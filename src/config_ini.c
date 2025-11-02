#include "config.h"

#include "logging.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_INI_LINE 512

static void ini_copy_string(char *dst, size_t dst_sz, const char *src) {
    if (dst == NULL || dst_sz == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    size_t copy_len = strlen(src);
    if (copy_len >= dst_sz) {
        copy_len = dst_sz - 1;
    }

    if (copy_len > 0) {
        memcpy(dst, src, copy_len);
    }
    dst[copy_len] = '\0';
}

typedef struct {
    OsdLayout layout;
    int type_set[OSD_MAX_ELEMENTS];
    int order_overridden;
    char order[OSD_MAX_ELEMENTS][48];
    int order_count;
} OsdLayoutBuilder;

static void builder_init(OsdLayoutBuilder *b, const OsdLayout *defaults) {
    memset(b, 0, sizeof(*b));
    if (defaults) {
        b->layout = *defaults;
        for (int i = 0; i < b->layout.element_count && i < OSD_MAX_ELEMENTS; ++i) {
            b->type_set[i] = 1;
        }
    } else {
        osd_layout_defaults(&b->layout);
        for (int i = 0; i < b->layout.element_count; ++i) {
            b->type_set[i] = 1;
        }
    }
}

static OsdElementConfig *builder_find(OsdLayoutBuilder *b, const char *name, int *index_out) {
    for (int i = 0; i < b->layout.element_count; ++i) {
        if (strcmp(b->layout.elements[i].name, name) == 0) {
            if (index_out) {
                *index_out = i;
            }
            return &b->layout.elements[i];
        }
    }
    return NULL;
}

static OsdElementConfig *builder_ensure(OsdLayoutBuilder *b, const char *name, int *index_out) {
    OsdElementConfig *elem = builder_find(b, name, index_out);
    if (elem) {
        return elem;
    }
    if (b->layout.element_count >= OSD_MAX_ELEMENTS) {
        return NULL;
    }
    int idx = b->layout.element_count++;
    elem = &b->layout.elements[idx];
    memset(elem, 0, sizeof(*elem));
    ini_copy_string(elem->name, sizeof(elem->name), name);
    elem->placement.anchor = OSD_POS_TOP_LEFT;
    elem->placement.offset_x = 0;
    elem->placement.offset_y = 0;
    elem->type = OSD_WIDGET_TEXT;
    elem->data.text.line_count = 0;
    elem->data.text.padding = 6;
    elem->data.text.fg = 0xB0FFFFFFu;
    elem->data.text.bg = 0x40202020u;
    elem->data.text.border = 0x60FFFFFFu;
    b->type_set[idx] = 0;
    if (index_out) {
        *index_out = idx;
    }
    return elem;
}

static void builder_reset_text(OsdElementConfig *elem) {
    elem->type = OSD_WIDGET_TEXT;
    elem->data.text.line_count = 0;
    elem->data.text.padding = 6;
    elem->data.text.fg = 0xB0FFFFFFu;
    elem->data.text.bg = 0x40202020u;
    elem->data.text.border = 0x60FFFFFFu;
}

static void builder_reset_line(OsdElementConfig *elem) {
    elem->type = OSD_WIDGET_LINE;
    elem->data.line.width = 360;
    elem->data.line.height = 80;
    elem->data.line.sample_stride_px = 4;
    elem->data.line.metric[0] = '\0';
    elem->data.line.label[0] = '\0';
    elem->data.line.show_info_box = 1;
    elem->data.line.has_y_min = 0;
    elem->data.line.has_y_max = 0;
    elem->data.line.y_min = 0.0;
    elem->data.line.y_max = 0.0;
    elem->data.line.fg = 0xFFFFFFFFu;
    elem->data.line.grid = 0x20FFFFFFu;
    elem->data.line.bg = 0x20000000u;
}

static void builder_reset_bar(OsdElementConfig *elem) {
    elem->type = OSD_WIDGET_BAR;
    elem->data.bar.width = 360;
    elem->data.bar.height = 80;
    elem->data.bar.sample_stride_px = 12;
    elem->data.bar.bar_width_px = 8;
    elem->data.bar.metric[0] = '\0';
    elem->data.bar.label[0] = '\0';
    elem->data.bar.show_info_box = 1;
    elem->data.bar.has_y_min = 0;
    elem->data.bar.has_y_max = 0;
    elem->data.bar.y_min = 0.0;
    elem->data.bar.y_max = 0.0;
    elem->data.bar.fg = 0xFF4CAF50u;
    elem->data.bar.grid = 0x20FFFFFFu;
    elem->data.bar.bg = 0x20000000u;
    elem->data.bar.mode = OSD_BAR_MODE_HISTORY;
    elem->data.bar.series_count = 0;
    for (int i = 0; i < OSD_BAR_MAX_SERIES; ++i) {
        elem->data.bar.metrics[i][0] = '\0';
    }
}

static int builder_finalize(OsdLayoutBuilder *b, OsdLayout *out_layout) {
    if (!out_layout) {
        return -1;
    }
    if (b->order_overridden) {
        OsdLayout final = {0};
        for (int i = 0; i < b->order_count; ++i) {
            int idx = 0;
            OsdElementConfig *elem = builder_find(b, b->order[i], &idx);
            if (!elem) {
                LOGE("config: osd element '%s' listed in order but not defined", b->order[i]);
                return -1;
            }
            if (!b->type_set[idx]) {
                LOGE("config: osd element '%s' missing type definition", b->order[i]);
                return -1;
            }
            if (final.element_count >= OSD_MAX_ELEMENTS) {
                LOGE("config: too many osd elements in order list (max %d)", OSD_MAX_ELEMENTS);
                return -1;
            }
            final.elements[final.element_count++] = *elem;
        }
        *out_layout = final;
    } else {
        for (int i = 0; i < b->layout.element_count; ++i) {
            if (!b->type_set[i]) {
                LOGE("config: osd element '%s' missing type definition", b->layout.elements[i].name);
                return -1;
            }
        }
        *out_layout = b->layout;
    }
    for (int i = 0; i < out_layout->element_count; ++i) {
        OsdElementConfig *elem = &out_layout->elements[i];
        if (elem->type == OSD_WIDGET_TEXT) {
            if (elem->data.text.padding <= 0) {
                elem->data.text.padding = 6;
            }
        } else if (elem->type == OSD_WIDGET_LINE) {
            if (elem->data.line.width <= 0) {
                elem->data.line.width = 360;
            }
            if (elem->data.line.height <= 0) {
                elem->data.line.height = 80;
            }
            if (elem->data.line.sample_stride_px <= 0) {
                elem->data.line.sample_stride_px = 4;
            }
        } else if (elem->type == OSD_WIDGET_BAR) {
            if (elem->data.bar.width <= 0) {
                elem->data.bar.width = 360;
            }
            if (elem->data.bar.height <= 0) {
                elem->data.bar.height = 80;
            }
            if (elem->data.bar.sample_stride_px <= 0) {
                elem->data.bar.sample_stride_px = 12;
            }
            if (elem->data.bar.bar_width_px <= 0) {
                elem->data.bar.bar_width_px = 8;
            }
        } else {
            LOGE("config: osd element '%s' has unsupported type", elem->name);
            return -1;
        }
    }
    return 0;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) {
        ++s;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) {
        *(--end) = '\0';
    }
    return s;
}

static int parse_bool(const char *value, int *out) {
    if (!value || !out) {
        return -1;
    }
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0 || strcmp(value, "1") == 0 || strcasecmp(value, "on") == 0) {
        *out = 1;
        return 0;
    }
    if (strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0 || strcmp(value, "0") == 0 || strcasecmp(value, "off") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int parse_double(const char *value, double *out) {
    if (value == NULL || out == NULL) {
        return -1;
    }
    char *end = NULL;
    double v = strtod(value, &end);
    if (end == value || end == NULL) {
        return -1;
    }
    *out = v;
    return 0;
}

static SplashSequenceCfg *splash_find_sequence(SplashCfg *splash, const char *name) {
    if (splash == NULL || name == NULL || *name == '\0') {
        return NULL;
    }
    for (int i = 0; i < splash->sequence_count; ++i) {
        if (strcmp(splash->sequences[i].name, name) == 0) {
            return &splash->sequences[i];
        }
    }
    return NULL;
}

static SplashSequenceCfg *splash_ensure_sequence(SplashCfg *splash, const char *name) {
    if (splash == NULL || name == NULL || *name == '\0') {
        return NULL;
    }
    SplashSequenceCfg *seq = splash_find_sequence(splash, name);
    if (seq != NULL) {
        return seq;
    }
    if (splash->sequence_count >= SPLASH_MAX_SEQUENCES) {
        return NULL;
    }
    seq = &splash->sequences[splash->sequence_count++];
    memset(seq, 0, sizeof(*seq));
    ini_copy_string(seq->name, sizeof(seq->name), name);
    seq->start_frame = -1;
    seq->end_frame = -1;
    return seq;
}

static int parse_splash_section(AppCfg *cfg, const char *key, const char *value) {
    if (strcasecmp(key, "enable") == 0) {
        int v = 0;
        if (parse_bool(value, &v) != 0) {
            return -1;
        }
        cfg->splash.enable = v;
        return 0;
    }
    if (strcasecmp(key, "input") == 0 || strcasecmp(key, "input-path") == 0) {
        ini_copy_string(cfg->splash.input_path, sizeof(cfg->splash.input_path), value);
        return 0;
    }
    if (strcasecmp(key, "fps") == 0) {
        double fps = 0.0;
        if (parse_double(value, &fps) != 0) {
            return -1;
        }
        cfg->splash.fps = fps;
        return 0;
    }
    if (strcasecmp(key, "idle-timeout-ms") == 0) {
        cfg->splash.idle_timeout_ms = atoi(value);
        if (cfg->splash.idle_timeout_ms < 0) {
            cfg->splash.idle_timeout_ms = 0;
        }
        return 0;
    }
    if (strcasecmp(key, "default-sequence") == 0) {
        ini_copy_string(cfg->splash.default_sequence, sizeof(cfg->splash.default_sequence), value);
        return 0;
    }
    return -1;
}

static int parse_splash_sequence(AppCfg *cfg, const char *section, const char *key, const char *value) {
    const char *prefix = "splash.sequence.";
    size_t prefix_len = strlen(prefix);
    if (strncasecmp(section, prefix, prefix_len) != 0) {
        return -1;
    }
    const char *name = section + prefix_len;
    SplashSequenceCfg *seq = splash_ensure_sequence(&cfg->splash, name);
    if (seq == NULL) {
        LOGE("config: too many splash sequences defined (max %d)", SPLASH_MAX_SEQUENCES);
        return -1;
    }
    if (strcasecmp(key, "start") == 0 || strcasecmp(key, "start-frame") == 0) {
        seq->start_frame = atoi(value);
        return 0;
    }
    if (strcasecmp(key, "end") == 0 || strcasecmp(key, "end-frame") == 0) {
        seq->end_frame = atoi(value);
        return 0;
    }
    return -1;
}

static int parse_named_color(const char *value, uint32_t *out) {
    static const struct {
        const char *name;
        uint32_t argb;
    } table[] = {
        {"white", 0xFFFFFFFFu},
        {"black", 0xFF000000u},
        {"blue", 0xFF2196F3u},
        {"green", 0xFF4CAF50u},
        {"red", 0xFFF44336u},
        {"yellow", 0xFFFFEB3Bu},
        {"orange", 0xFFFF9800u},
        {"purple", 0xFF9C27B0u},
        {"cyan", 0xFF00BCD4u},
        {"magenta", 0xFFE91E63u},
        {"grey", 0xFF9E9E9Eu},
        {"gray", 0xFF9E9E9Eu},
        {"light-grey", 0xFFBDBDBDu},
        {"light-gray", 0xFFBDBDBDu},
        {"dark-grey", 0xFF424242u},
        {"dark-gray", 0xFF424242u},
        {"transparent", 0x00000000u},
        {"clear", 0x00000000u},
        {"transparent-black", 0x80000000u},
        {"transparent-grey", 0x80202020u},
        {"transparent-gray", 0x80202020u},
        {"transperant-grey", 0x80202020u},
        {"transperant-gray", 0x80202020u},
        {"transparent-white", 0x80FFFFFFu},
        {"transparent-blue", 0x802196F3u},
        {"transparent-green", 0x804CAF50u},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (strcasecmp(value, table[i].name) == 0) {
            *out = table[i].argb;
            return 0;
        }
    }
    return -1;
}

static int parse_color(const char *value, uint32_t *out) {
    if (!value || !out) {
        return -1;
    }
    if (parse_named_color(value, out) == 0) {
        return 0;
    }
    const char *p = value;
    if (p[0] == '#') {
        ++p;
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    size_t len = strlen(p);
    if (len != 8 && len != 6) {
        return -1;
    }
    char digits[9];
    if (len == 6) {
        digits[0] = 'F';
        digits[1] = 'F';
        memcpy(digits + 2, p, 6);
        digits[8] = '\0';
    } else {
        memcpy(digits, p, 8);
        digits[8] = '\0';
    }
    char *endptr = NULL;
    errno = 0;
    unsigned long v = strtoul(digits, &endptr, 16);
    if (errno != 0 || !endptr || *endptr != '\0') {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static int parse_anchor(const char *value, OSDWidgetPosition *pos) {
    if (!value || !pos) {
        return -1;
    }
    static const struct {
        const char *name;
        OSDWidgetPosition pos;
    } table[] = {
        {"top-left", OSD_POS_TOP_LEFT},     {"top-mid", OSD_POS_TOP_MID},       {"top-right", OSD_POS_TOP_RIGHT},
        {"mid-left", OSD_POS_MID_LEFT},     {"center", OSD_POS_MID},            {"mid", OSD_POS_MID},
        {"mid-mid", OSD_POS_MID},           {"mid-right", OSD_POS_MID_RIGHT},   {"bottom-left", OSD_POS_BOTTOM_LEFT},
        {"bottom-mid", OSD_POS_BOTTOM_MID}, {"bottom-right", OSD_POS_BOTTOM_RIGHT}};
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (strcasecmp(value, table[i].name) == 0) {
            *pos = table[i].pos;
            return 0;
        }
    }
    return -1;
}

static int parse_size(const char *value, int *w, int *h) {
    if (!value || !w || !h) {
        return -1;
    }
    int width = 0, height = 0;
    if (sscanf(value, "%dx%d", &width, &height) != 2) {
        return -1;
    }
    *w = width;
    *h = height;
    return 0;
}

static int parse_osd_section(OsdLayoutBuilder *builder, const char *key, const char *value) {
    if (strcasecmp(key, "elements") == 0) {
        builder->order_count = 0;
        builder->order_overridden = 1;
        char buf[256];
        ini_copy_string(buf, sizeof(buf), value);
        char *token = strtok(buf, ",");
        while (token) {
            char *name = trim(token);
            if (*name) {
                if (builder->order_count >= OSD_MAX_ELEMENTS) {
                    LOGE("config: osd elements list exceeds limit %d", OSD_MAX_ELEMENTS);
                    return -1;
                }
                ini_copy_string(builder->order[builder->order_count], sizeof(builder->order[0]), name);
                builder->order_count++;
            }
            token = strtok(NULL, ",");
        }
        return 0;
    }
    return -1;
}

static int parse_osd_element_text(OsdElementConfig *elem, const char *key, const char *value) {
    if (strcasecmp(key, "line") == 0) {
        if (elem->data.text.line_count >= OSD_MAX_TEXT_LINES) {
            LOGE("config: osd text element '%s' has too many lines (max %d)", elem->name, OSD_MAX_TEXT_LINES);
            return -1;
        }
        ini_copy_string(elem->data.text.lines[elem->data.text.line_count].raw,
                        sizeof(elem->data.text.lines[0].raw), value);
        elem->data.text.line_count++;
        return 0;
    }
    if (strcasecmp(key, "padding") == 0) {
        elem->data.text.padding = atoi(value);
        return 0;
    }
    if (strcasecmp(key, "foreground") == 0 || strcasecmp(key, "text-color") == 0) {
        uint32_t color = 0;
        if (parse_color(value, &color) != 0) {
            return -1;
        }
        elem->data.text.fg = color;
        return 0;
    }
    if (strcasecmp(key, "background") == 0) {
        uint32_t color = 0;
        if (parse_color(value, &color) != 0) {
            return -1;
        }
        elem->data.text.bg = color;
        return 0;
    }
    if (strcasecmp(key, "border") == 0) {
        uint32_t color = 0;
        if (parse_color(value, &color) != 0) {
            return -1;
        }
        elem->data.text.border = color;
        return 0;
    }
    return -1;
}

static int parse_osd_element_line(OsdElementConfig *elem, const char *key, const char *value) {
    if (strcasecmp(key, "metric") == 0) {
        ini_copy_string(elem->data.line.metric, sizeof(elem->data.line.metric), value);
        return 0;
    }
    if (strcasecmp(key, "label") == 0) {
        ini_copy_string(elem->data.line.label, sizeof(elem->data.line.label), value);
        return 0;
    }
    if (strcasecmp(key, "info-box") == 0 || strcasecmp(key, "show-info-box") == 0 || strcasecmp(key, "info_box") == 0) {
        int enabled = 0;
        if (parse_bool(value, &enabled) != 0) {
            return -1;
        }
        elem->data.line.show_info_box = enabled;
        return 0;
    }
    if (strcasecmp(key, "sample-spacing") == 0 || strcasecmp(key, "sample-stride") == 0 ||
        strcasecmp(key, "sample_stride") == 0 || strcasecmp(key, "sample-spacing-px") == 0) {
        elem->data.line.sample_stride_px = atoi(value);
        return 0;
    }
    if (strcasecmp(key, "size") == 0) {
        int w = 0, h = 0;
        if (parse_size(value, &w, &h) != 0) {
            return -1;
        }
        elem->data.line.width = w;
        elem->data.line.height = h;
        return 0;
    }
    if (strcasecmp(key, "foreground") == 0 || strcasecmp(key, "line-color") == 0) {
        uint32_t color = 0;
        if (parse_color(value, &color) != 0) {
            return -1;
        }
        elem->data.line.fg = color;
        return 0;
    }
    if (strcasecmp(key, "grid") == 0) {
        uint32_t color = 0;
        if (parse_color(value, &color) != 0) {
            return -1;
        }
        elem->data.line.grid = color;
        return 0;
    }
    if (strcasecmp(key, "background") == 0) {
        uint32_t color = 0;
        if (parse_color(value, &color) != 0) {
            return -1;
        }
        elem->data.line.bg = color;
        return 0;
    }
    if (strcasecmp(key, "y-min") == 0 || strcasecmp(key, "y_min") == 0 || strcasecmp(key, "ymin") == 0) {
        double v = 0.0;
        if (parse_double(value, &v) != 0) {
            return -1;
        }
        elem->data.line.has_y_min = 1;
        elem->data.line.y_min = v;
        return 0;
    }
    if (strcasecmp(key, "y-max") == 0 || strcasecmp(key, "y_max") == 0 || strcasecmp(key, "ymax") == 0) {
        double v = 0.0;
        if (parse_double(value, &v) != 0) {
            return -1;
        }
        elem->data.line.has_y_max = 1;
        elem->data.line.y_max = v;
        return 0;
    }
    return -1;
}

static void bar_config_parse_metrics(OsdBarConfig *cfg, const char *value) {
    if (!cfg) {
        return;
    }

    for (int i = 0; i < OSD_BAR_MAX_SERIES; ++i) {
        cfg->metrics[i][0] = '\0';
    }
    cfg->series_count = 0;

    if (!value) {
        cfg->metric[0] = '\0';
        return;
    }

    const char *p = value;
    while (*p && cfg->series_count < OSD_BAR_MAX_SERIES) {
        while (*p && (*p == ',' || isspace((unsigned char)*p))) {
            p++;
        }
        const char *start = p;
        while (*p && *p != ',') {
            p++;
        }
        const char *end = p;
        while (end > start && isspace((unsigned char)*(end - 1))) {
            end--;
        }
        while (start < end && isspace((unsigned char)*start)) {
            start++;
        }
        size_t len = (size_t)(end - start);
        if (len > 0) {
            if (len >= sizeof(cfg->metrics[0])) {
                len = sizeof(cfg->metrics[0]) - 1;
            }
            memcpy(cfg->metrics[cfg->series_count], start, len);
            cfg->metrics[cfg->series_count][len] = '\0';
            cfg->series_count++;
        }
        if (*p == ',') {
            p++;
        }
    }

    if (cfg->series_count == 0) {
        ini_copy_string(cfg->metric, sizeof(cfg->metric), value);
        if (cfg->metric[0] != '\0') {
            ini_copy_string(cfg->metrics[0], sizeof(cfg->metrics[0]), cfg->metric);
            cfg->series_count = 1;
        }
        return;
    }

    ini_copy_string(cfg->metric, sizeof(cfg->metric), cfg->metrics[0]);
}

static int parse_osd_element_bar(OsdElementConfig *elem, const char *key, const char *value) {
    if (strcasecmp(key, "metric") == 0) {
        bar_config_parse_metrics(&elem->data.bar, value);
        return 0;
    }
    if (strcasecmp(key, "metrics") == 0) {
        bar_config_parse_metrics(&elem->data.bar, value);
        return 0;
    }
    if (strcasecmp(key, "label") == 0) {
        ini_copy_string(elem->data.bar.label, sizeof(elem->data.bar.label), value);
        return 0;
    }
    if (strcasecmp(key, "show-info-box") == 0 || strcasecmp(key, "show_info_box") == 0 ||
        strcasecmp(key, "info-box") == 0 || strcasecmp(key, "info_box") == 0) {
        int v = 0;
        if (parse_bool(value, &v) != 0) {
            return -1;
        }
        elem->data.bar.show_info_box = v;
        return 0;
    }
    if (strcasecmp(key, "size") == 0) {
        int w = 0, h = 0;
        if (parse_size(value, &w, &h) != 0) {
            return -1;
        }
        elem->data.bar.width = w;
        elem->data.bar.height = h;
        return 0;
    }
    if (strcasecmp(key, "sample-spacing") == 0 || strcasecmp(key, "sample-stride") == 0 ||
        strcasecmp(key, "sample_stride") == 0 || strcasecmp(key, "sample-spacing-px") == 0) {
        elem->data.bar.sample_stride_px = atoi(value);
        return 0;
    }
    if (strcasecmp(key, "bar-width") == 0 || strcasecmp(key, "bar_width") == 0 ||
        strcasecmp(key, "bar-width-px") == 0) {
        elem->data.bar.bar_width_px = atoi(value);
        return 0;
    }
    if (strcasecmp(key, "foreground") == 0 || strcasecmp(key, "bar-color") == 0) {
        uint32_t color = 0;
        if (parse_color(value, &color) != 0) {
            return -1;
        }
        elem->data.bar.fg = color;
        return 0;
    }
    if (strcasecmp(key, "grid") == 0) {
        uint32_t color = 0;
        if (parse_color(value, &color) != 0) {
            return -1;
        }
        elem->data.bar.grid = color;
        return 0;
    }
    if (strcasecmp(key, "background") == 0) {
        uint32_t color = 0;
        if (parse_color(value, &color) != 0) {
            return -1;
        }
        elem->data.bar.bg = color;
        return 0;
    }
    if (strcasecmp(key, "mode") == 0) {
        if (strcasecmp(value, "instant") == 0 || strcasecmp(value, "instantaneous") == 0 ||
            strcasecmp(value, "static") == 0 || strcasecmp(value, "single") == 0) {
            elem->data.bar.mode = OSD_BAR_MODE_INSTANT;
            return 0;
        }
        if (strcasecmp(value, "history") == 0 || strcasecmp(value, "scroll") == 0 ||
            strcasecmp(value, "trailing") == 0) {
            elem->data.bar.mode = OSD_BAR_MODE_HISTORY;
            return 0;
        }
        return -1;
    }
    if (strcasecmp(key, "y-min") == 0 || strcasecmp(key, "y_min") == 0 || strcasecmp(key, "ymin") == 0) {
        double v = 0.0;
        if (parse_double(value, &v) != 0) {
            return -1;
        }
        elem->data.bar.has_y_min = 1;
        elem->data.bar.y_min = v;
        return 0;
    }
    if (strcasecmp(key, "y-max") == 0 || strcasecmp(key, "y_max") == 0 || strcasecmp(key, "ymax") == 0) {
        double v = 0.0;
        if (parse_double(value, &v) != 0) {
            return -1;
        }
        elem->data.bar.has_y_max = 1;
        elem->data.bar.y_max = v;
        return 0;
    }
    return -1;
}

static int parse_osd_element(OsdLayoutBuilder *builder, const char *section_name, const char *key, const char *value) {
    const char *prefix = "osd.element.";
    size_t prefix_len = strlen(prefix);
    if (strncmp(section_name, prefix, prefix_len) != 0) {
        return -1;
    }
    const char *name = section_name + prefix_len;
    int idx = 0;
    OsdElementConfig *elem = builder_ensure(builder, name, &idx);
    if (!elem) {
        LOGE("config: too many osd elements; increase OSD_MAX_ELEMENTS");
        return -1;
    }
    if (strcasecmp(key, "type") == 0) {
        if (strcasecmp(value, "text") == 0) {
            builder_reset_text(elem);
            builder->type_set[idx] = 1;
            return 0;
        }
        if (strcasecmp(value, "line") == 0) {
            builder_reset_line(elem);
            builder->type_set[idx] = 1;
            return 0;
        }
        if (strcasecmp(value, "bar") == 0) {
            builder_reset_bar(elem);
            builder->type_set[idx] = 1;
            return 0;
        }
        LOGE("config: unknown osd element type '%s' for '%s'", value, name);
        return -1;
    }
    if (strcasecmp(key, "anchor") == 0) {
        OSDWidgetPosition pos;
        if (parse_anchor(value, &pos) != 0) {
            return -1;
        }
        elem->placement.anchor = pos;
        return 0;
    }
    if (strcasecmp(key, "offset") == 0) {
        int ox = 0, oy = 0;
        if (sscanf(value, "%d,%d", &ox, &oy) != 2) {
            return -1;
        }
        elem->placement.offset_x = ox;
        elem->placement.offset_y = oy;
        return 0;
    }
    if (elem->type == OSD_WIDGET_TEXT) {
        return parse_osd_element_text(elem, key, value);
    }
    if (elem->type == OSD_WIDGET_LINE) {
        return parse_osd_element_line(elem, key, value);
    }
    if (elem->type == OSD_WIDGET_BAR) {
        return parse_osd_element_bar(elem, key, value);
    }
    return -1;
}

static int apply_general_key(AppCfg *cfg, const char *section, const char *key, const char *value) {
    if (strcasecmp(section, "drm") == 0) {
        if (strcasecmp(key, "card") == 0) {
        ini_copy_string(cfg->card_path, sizeof(cfg->card_path), value);
            return 0;
        }
        if (strcasecmp(key, "connector") == 0) {
        ini_copy_string(cfg->connector_name, sizeof(cfg->connector_name), value);
            return 0;
        }
        if (strcasecmp(key, "video-plane-id") == 0) {
            cfg->plane_id = atoi(value);
            return 0;
        }
        if (strcasecmp(key, "osd-plane-id") == 0) {
            cfg->osd_plane_id = atoi(value);
            return 0;
        }
        if (strcasecmp(key, "use-udev") == 0) {
            int v = 0;
            if (parse_bool(value, &v) != 0) {
                return -1;
            }
            cfg->use_udev = v;
            return 0;
        }
        return -1;
    }
    if (strcasecmp(section, "udp") == 0) {
        if (strcasecmp(key, "port") == 0) {
            cfg->udp_port = atoi(value);
            return 0;
        }
        if (strcasecmp(key, "video-pt") == 0) {
            cfg->vid_pt = atoi(value);
            return 0;
        }
        if (strcasecmp(key, "audio-pt") == 0) {
            cfg->aud_pt = atoi(value);
            return 0;
        }
        return -1;
    }
    if (strcasecmp(section, "pipeline") == 0) {
        if (strcasecmp(key, "appsink-max-buffers") == 0) {
            cfg->appsink_max_buffers = atoi(value);
            if (cfg->appsink_max_buffers <= 0) {
                LOGE("config: pipeline.appsink-max-buffers '%s' must be positive", value);
                cfg->appsink_max_buffers = 1;
            }
            return 0;
        }
        if (strcasecmp(key, "custom-sink") == 0) {
            CustomSinkMode mode;
            if (cfg_parse_custom_sink_mode(value, &mode) != 0) {
                LOGE("Invalid custom-sink mode '%s' in INI", value);
                return -1;
            }
            cfg->custom_sink = mode;
            return 0;
        }
        if (strcasecmp(key, "pt97-filter") == 0) {
            int v = 0;
            if (parse_bool(value, &v) != 0) {
                return -1;
            }
            cfg->udpsrc_pt97_filter = v;
            return 0;
        }
        if (strcasecmp(key, "use-gst-udpsrc") == 0) {
            int v = 0;
            if (parse_bool(value, &v) != 0) {
                return -1;
            }
            LOGW("INI key pipeline.use-gst-udpsrc is deprecated; use pipeline.custom-sink instead");
            cfg->custom_sink = v ? CUSTOM_SINK_UDPSRC : CUSTOM_SINK_RECEIVER;
            return 0;
        }
        return -1;
    }
    if (strcasecmp(section, "audio") == 0) {
        if (strcasecmp(key, "device") == 0) {
            ini_copy_string(cfg->aud_dev, sizeof(cfg->aud_dev), value);
            return 0;
        }
        if (strcasecmp(key, "disable") == 0) {
            int v = 0;
            if (parse_bool(value, &v) != 0) {
                return -1;
            }
            cfg->no_audio = v;
            return 0;
        }
        if (strcasecmp(key, "optional") == 0) {
            int v = 0;
            if (parse_bool(value, &v) != 0) {
                return -1;
            }
            cfg->audio_optional = v;
            return 0;
        }
        return -1;
    }
    if (strcasecmp(section, "restart") == 0 || strcasecmp(section, "restarts") == 0) {
        if (strcasecmp(key, "limit") == 0) {
            cfg->restart_limit = atoi(value);
            return 0;
        }
        if (strcasecmp(key, "window-ms") == 0) {
            cfg->restart_window_ms = atoi(value);
            return 0;
        }
        return -1;
    }
    if (strcasecmp(section, "osd") == 0) {
        if (strcasecmp(key, "enable") == 0) {
            int v = 0;
            if (parse_bool(value, &v) != 0) {
                return -1;
            }
            cfg->osd_enable = v;
            return 0;
        }
        if (strcasecmp(key, "refresh-ms") == 0) {
            cfg->osd_refresh_ms = atoi(value);
            return 0;
        }
        if (strcasecmp(key, "plane-id") == 0) {
            cfg->osd_plane_id = atoi(value);
            return 0;
        }
        return -1;
    }
    if (strcasecmp(section, "osd.external") == 0 || strcasecmp(section, "osd_external") == 0) {
        if (strcasecmp(key, "enable") == 0) {
            int v = 0;
            if (parse_bool(value, &v) != 0) {
                return -1;
            }
            cfg->osd_external.enable = v;
            cfg->osd_external.enable_set = 1;
            return 0;
        }
        if (strcasecmp(key, "udp-port") == 0 || strcasecmp(key, "port") == 0) {
            cfg->osd_external.udp_port = atoi(value);
            if (cfg->osd_external.udp_port > 0 && cfg->osd_external.udp_port <= 65535) {
                if (!cfg->osd_external.enable_set) {
                    cfg->osd_external.enable = 1;
                }
            } else {
                LOGW("Ignoring invalid osd.external port value: %s", value);
                cfg->osd_external.udp_port = 0;
            }
            return 0;
        }
        if (strcasecmp(key, "bind") == 0 || strcasecmp(key, "address") == 0 ||
            strcasecmp(key, "host") == 0) {
            ini_copy_string(cfg->osd_external.bind_address, sizeof(cfg->osd_external.bind_address), value);
            return 0;
        }
        return -1;
    }
    if (strcasecmp(section, "record") == 0) {
        if (strcasecmp(key, "enable") == 0) {
            int v = 0;
            if (parse_bool(value, &v) != 0) {
                return -1;
            }
            cfg->record.enable = v;
            return 0;
        }
        if (strcasecmp(key, "path") == 0 || strcasecmp(key, "output") == 0 ||
            strcasecmp(key, "output-path") == 0) {
            ini_copy_string(cfg->record.output_path, sizeof(cfg->record.output_path), value);
            return 0;
        }
        if (strcasecmp(key, "mode") == 0) {
            RecordMode mode;
            if (cfg_parse_record_mode(value, &mode) != 0) {
                return -1;
            }
            cfg->record.mode = mode;
            return 0;
        }
        return -1;
    }
    if (strcasecmp(section, "sse") == 0) {
        if (strcasecmp(key, "enable") == 0) {
            int v = 0;
            if (parse_bool(value, &v) != 0) {
                return -1;
            }
            cfg->sse.enable = v;
            return 0;
        }
        if (strcasecmp(key, "bind") == 0 || strcasecmp(key, "address") == 0 ||
            strcasecmp(key, "bind-address") == 0) {
            ini_copy_string(cfg->sse.bind_address, sizeof(cfg->sse.bind_address), value);
            return 0;
        }
        if (strcasecmp(key, "port") == 0) {
            int port = atoi(value);
            if (port <= 0 || port > 65535) {
                LOGE("config: SSE port '%s' out of range", value);
                return -1;
            }
            cfg->sse.port = port;
            return 0;
        }
        if (strcasecmp(key, "interval-ms") == 0) {
            int v = atoi(value);
            if (v <= 0) {
                v = 1;
            }
            cfg->sse.interval_ms = (unsigned int)v;
            return 0;
        }
        return -1;
    }
    if (strcasecmp(section, "idr") == 0) {
        if (strcasecmp(key, "enable") == 0) {
            int v = 0;
            if (parse_bool(value, &v) != 0) {
                return -1;
            }
            cfg->idr.enable = v;
            return 0;
        }
        if (strcasecmp(key, "port") == 0) {
            int port = atoi(value);
            if (port <= 0 || port > 65535) {
                LOGE("config: IDR port '%s' out of range", value);
                return -1;
            }
            cfg->idr.http_port = port;
            return 0;
        }
        if (strcasecmp(key, "path") == 0 || strcasecmp(key, "request") == 0) {
            ini_copy_string(cfg->idr.http_path, sizeof(cfg->idr.http_path), value);
            return 0;
        }
        if (strcasecmp(key, "timeout-ms") == 0) {
            int timeout = atoi(value);
            if (timeout <= 0) {
                timeout = 1;
            }
            cfg->idr.http_timeout_ms = (unsigned int)timeout;
            return 0;
        }
        return -1;
    }
    if (strcasecmp(section, "gst") == 0) {
        if (strcasecmp(key, "log") == 0) {
            int v = 0;
            if (parse_bool(value, &v) != 0) {
                return -1;
            }
            cfg->gst_log = v;
            return 0;
        }
        return -1;
    }
    if (strcasecmp(section, "cpu") == 0) {
        if (strcasecmp(key, "affinity") == 0) {
            return cfg_parse_cpu_list(value, cfg);
        }
        return -1;
    }
    return -1;
}

int cfg_load_file(const char *path, AppCfg *cfg) {
    if (!path || !cfg) {
        return -1;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOGE("config: failed to open %s", path);
        return -1;
    }

    OsdLayoutBuilder builder;
    builder_init(&builder, &cfg->osd_layout);

    char line[MAX_INI_LINE];
    char current_section[128] = "";
    int lineno = 0;

    while (fgets(line, sizeof(line), fp)) {
        ++lineno;
        char *trimmed = trim(line);
        if (*trimmed == '\0' || *trimmed == '#' || *trimmed == ';') {
            continue;
        }
        if (*trimmed == '[') {
            char *end = strchr(trimmed, ']');
            if (!end) {
                LOGE("config:%d: missing closing ']'", lineno);
                fclose(fp);
                return -1;
            }
            *end = '\0';
            ini_copy_string(current_section, sizeof(current_section), trimmed + 1);
            continue;
        }
        char *eq = strchr(trimmed, '=');
        if (!eq) {
            LOGE("config:%d: expected key=value", lineno);
            fclose(fp);
            return -1;
        }
        *eq = '\0';
        char *key = trim(trimmed);
        char *value = trim(eq + 1);
        if (*value == '"' && value[strlen(value) - 1] == '"' && strlen(value) >= 2) {
            value[strlen(value) - 1] = '\0';
            value = value + 1;
        }

        if (strncasecmp(current_section, "splash.sequence.", strlen("splash.sequence.")) == 0) {
            if (parse_splash_sequence(cfg, current_section, key, value) != 0) {
                LOGE("config:%d: failed to parse splash sequence setting %s", lineno, key);
                fclose(fp);
                return -1;
            }
            continue;
        }
        if (strcasecmp(current_section, "splash") == 0) {
            if (parse_splash_section(cfg, key, value) == 0) {
                continue;
            }
        }
        if (strncasecmp(current_section, "osd.element.", strlen("osd.element.")) == 0) {
            if (parse_osd_element(&builder, current_section, key, value) != 0) {
                LOGE("config:%d: failed to parse osd element setting %s", lineno, key);
                fclose(fp);
                return -1;
            }
            continue;
        }
        if (strcasecmp(current_section, "osd") == 0) {
            if (parse_osd_section(&builder, key, value) == 0) {
                continue;
            }
        }
        if (apply_general_key(cfg, current_section, key, value) != 0) {
            LOGE("config:%d: unknown setting %s in section [%s]", lineno, key, current_section);
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);

    if (builder_finalize(&builder, &cfg->osd_layout) != 0) {
        return -1;
    }

    if (cfg->splash.sequence_count > SPLASH_MAX_SEQUENCES) {
        cfg->splash.sequence_count = SPLASH_MAX_SEQUENCES;
    }
    for (int i = 0; i < cfg->splash.sequence_count; ++i) {
        SplashSequenceCfg *seq = &cfg->splash.sequences[i];
        if (seq->start_frame < 0 || seq->end_frame < 0) {
            LOGE("config: splash sequence '%s' missing start/end", seq->name);
            return -1;
        }
        if (seq->end_frame < seq->start_frame) {
            LOGE("config: splash sequence '%s' has end before start", seq->name);
            return -1;
        }
    }

    return 0;
}
