/*
 * waybeam_hub.c — Minified C port of waybeam_hub.py for SigmaStar Infinity6E
 *
 * autod – Autod Personal Use License
 * Copyright (c) 2025 Joakim Snökvist
 * Licensed for personal, non-commercial use only.
 * Redistribution or commercial use requires prior written approval from Joakim Snökvist.
 * See LICENSE.md for full terms.
 *
 * Single-threaded, poll()-based SSE consumer + UDP OSD producer.
 * Reads config.json + menu.ini for drop-in compatibility with the Python version.
 * Includes a small single-user WebUI bridge for setup/config workflows.
 *
 * Build:  make -f Makefile.hub
 * Cross:  make -f Makefile.hub CC=arm-openipc-linux-gnueabihf-gcc
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * Constants (matching Python waybeam_hub.py)
 * --------------------------------------------------------------------------- */

#define MAX_OSD_SLOTS        8
#define MAX_OSD_TEXT_CHARS    63
#define ASSET_COUNT           8
#define MENU_TEXT_SLOT_START  (MAX_OSD_SLOTS - 3) /* slots 5,6,7 */

#define CRSF_MIN              172
#define CRSF_MAX              1811
#define CRSF_CENTER           992
#define CRSF_DISPLAY_CHANNELS 16
#define CRSF_MIN_CONTROL_CH   4

#define KEY_UP           (-201)
#define KEY_DOWN         (-202)
#define KEY_SELECT       10
#define KEY_MENU_TOGGLE  (-1002)

#define MAX_MENU_ENTRIES   32
#define MAX_ACTIONS        64
#define MAX_RADIO_RULES    16
#define MAX_CONDITIONS_PER_RULE 4
#define MAX_SECTIONS       16
#define MAX_DESTINATIONS   4
#define MAX_KEY_QUEUE      8

#define SSE_LINE_BUF       4096
#define SSE_DATA_BUF       4096
#define JSON_BUF_SIZE      4096
#define INI_LINE_BUF       512
#define CMD_OUTPUT_BUF     256
#define PAYLOAD_BUF        1280
#define WEB_REQ_BUF        4096
#define WEB_HTML_BUF      16384

#define SECTION_NAME_LEN   32
#define ACTION_NAME_LEN    64
#define ACTION_CMD_LEN     256
#define HOST_LEN           64
#define PATH_LEN           256
#define URL_LEN            256

/* ---------------------------------------------------------------------------
 * Logging
 * --------------------------------------------------------------------------- */

#define LOGI(fmt, ...) fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOGW(fmt, ...) fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) fprintf(stderr, "[ERR]  " fmt "\n", ##__VA_ARGS__)
#define LOGV(app, fmt, ...) do { if ((app)->cfg.verbose) fprintf(stderr, "[VERB] " fmt "\n", ##__VA_ARGS__); } while(0)

/* ---------------------------------------------------------------------------
 * Utility
 * --------------------------------------------------------------------------- */

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_reload = 0;

static void sig_handler(int sig) {
  if (sig == SIGHUP) g_reload = 1;
  else g_stop = 1;
}

static double monotonic_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int clamp_i(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int clamp_crsf(int v) { return clamp_i(v, CRSF_MIN, CRSF_MAX); }

/* ---------------------------------------------------------------------------
 * Data structures
 * --------------------------------------------------------------------------- */

typedef struct {
  int crsf_axis_deadband;
  int crsf_action_threshold;
  int crsf_menu_toggle_ch_low_max;
  int crsf_menu_toggle_ch4_min;
  int crsf_nav_debounce_ms;
  int crsf_select_debounce_ms;
  double crsf_menu_toggle_hold_s;
  int crsf_sample_interval_ms;
  int radio_trigger_debounce_ms;
  int radio_reset_debounce_ms;
  double menu_inactivity_timeout_s;
  double source_stale_s;
} tuning_t;

typedef struct {
  int enabled;
  int asset_id;
  double duration_s;
  int opacity;
  double delay_s;
  double fadeout_s;
} splashscreen_t;

typedef struct {
  char host[HOST_LEN];
  int port;
  int interval_ms;
  int initial_off[ASSET_COUNT];
  int initial_off_count;
  char actions_ini[PATH_LEN];
  int action_timeout_ms;
  char action_shell[PATH_LEN];
  int menu_asset_id;
  char webui_host[HOST_LEN];
  int webui_port;
  char webui_html[PATH_LEN];
  char sse_url[URL_LEN];
  char priority[16]; /* "serial" or "joystick" */
  double priority_fallback_s;
  char extra_dest_specs[MAX_DESTINATIONS][HOST_LEN];
  int extra_dest_count;
  int verbose;
  tuning_t tuning;
  splashscreen_t splashscreen;
} config_t;

typedef struct {
  char section[SECTION_NAME_LEN];
  char name[ACTION_NAME_LEN];
  char command[ACTION_CMD_LEN];
} menu_action_t;

/* Menu entry kind */
enum entry_kind {
  ENTRY_SECTION = 0,
  ENTRY_EXIT,
  ENTRY_RETURN,
  ENTRY_ASSET,
  ENTRY_ACTION,
};

typedef struct {
  enum entry_kind kind;
  char section[SECTION_NAME_LEN]; /* for ENTRY_SECTION */
  int asset_id;                   /* for ENTRY_ASSET */
  int action_index;               /* index into actions[] for ENTRY_ACTION */
} menu_entry_t;

typedef struct {
  int condition_count;
  struct {
    int channel_index; /* 0-based */
    int min_value;
    int max_value;
  } conds[MAX_CONDITIONS_PER_RULE];
  int action_index; /* into actions[] */
  char name[ACTION_NAME_LEN];
} radio_rule_t;

typedef struct {
  double enter_started;
  double outside_started;
  int latched_in_range;
  int trigger_count;
} radio_rule_state_t;

typedef struct {
  int channels[CRSF_DISPLAY_CHANNELS];
  int select_pressed;
  int back_pressed;
  double last_select_mono;
  double last_sample_mono;
  double last_update_mono;
  char nav_direction[8]; /* "neutral","up","down" */
  char nav_candidate[8];
  double nav_candidate_since;
  int nav_latched;
  int combo_active;
  double combo_started_mono;
  int combo_latched;
  int link_up;
} source_state_t;

/* SSE client states */
enum sse_state {
  SSE_DISCONNECTED = 0,
  SSE_CONNECTING,
  SSE_READING_HEADERS,
  SSE_STREAMING,
};

typedef struct {
  int fd;
  enum sse_state state;
  char host[HOST_LEN];
  int port;
  char path[PATH_LEN];
  char line_buf[SSE_LINE_BUF];
  int line_len;
  char event_name[64];
  char data_buf[SSE_DATA_BUF];
  int data_len;
  double last_connect_attempt;
} sse_client_t;

typedef struct {
  int server_fd;
  int client_fd;
  char req_buf[WEB_REQ_BUF];
  int req_len;
  char html[WEB_HTML_BUF];
  int html_len;
} webui_server_t;

typedef struct {
  char host[HOST_LEN];
  int port;
  struct sockaddr_in addr;
} udp_dest_t;

typedef struct {
  config_t cfg;
  char config_path[PATH_LEN];

  /* Actions & menu */
  menu_action_t actions[MAX_ACTIONS];
  int action_count;
  char section_order[MAX_SECTIONS][SECTION_NAME_LEN];
  int section_count;
  radio_rule_t radio_rules[MAX_RADIO_RULES];
  int radio_rule_count;

  /* Menu state */
  menu_entry_t top_entries[MAX_MENU_ENTRIES];
  int top_entry_count;
  menu_entry_t sub_entries[MAX_SECTIONS + 2][MAX_MENU_ENTRIES]; /* per-section */
  int sub_entry_count[MAX_SECTIONS + 2];
  char sub_entry_section[MAX_SECTIONS + 2][SECTION_NAME_LEN];
  int sub_section_count;

  char current_section[SECTION_NAME_LEN];
  int selected;
  int asset_enabled[ASSET_COUNT]; /* -1=unknown, 0=off, 1=on */
  int pending_asset_updates[ASSET_COUNT]; /* -1=no update, 0=off, 1=on */
  /* Network */
  int udp_fd;
  udp_dest_t destinations[MAX_DESTINATIONS];
  int dest_count;
  sse_client_t sse;

  /* Source state */
  source_state_t sources[2]; /* 0=serial, 1=joystick */
  int active_source; /* -1=none, 0=serial, 1=joystick */
  radio_rule_state_t radio_states[2][MAX_RADIO_RULES];

  /* Async action execution */
  pid_t action_pid;        /* -1 = no running action */
  int action_pipe_fd;      /* read end of stdout/stderr pipe */
  double action_deadline;  /* monotonic deadline for timeout */
  char action_label[128];  /* "[section] name" for status */
  char action_output[CMD_OUTPUT_BUF];
  int action_output_len;

  /* Splashscreen state machine */
  enum { SPLASH_IDLE=0, SPLASH_DELAY, SPLASH_SHOWING, SPLASH_FADING, SPLASH_DONE } splash_phase;
  double splash_next_event;     /* next phase transition time, 0.0 = not yet armed */
  double splash_fade_end;       /* when fade completes (FADING phase only) */
  double splash_fade_tick_s;    /* interval between fade opacity sends */
  int splash_last_opacity;      /* last sent opacity for dedup */
  int splash_pending_opacity;   /* queued opacity-only update, -1 = none */

  /* Loop state */
  int dirty;
  double last_send_mono;
  double last_menu_activity_mono;
  int key_queue[MAX_KEY_QUEUE];
  int key_count;
  char status[256];
  char last_logged_status[256];
  webui_server_t webui;
} app_state_t;

/* ---------------------------------------------------------------------------
 * JSON Parser (adapted from osd_external.c)
 * --------------------------------------------------------------------------- */

static const char *skip_ws(const char *p) {
  while (p && *p && isspace((unsigned char)*p)) ++p;
  return p;
}

static int is_value_term(int c) {
  return c == ',' || c == '}' || c == ']' || c == '\0' || isspace((unsigned char)c);
}

static const char *parse_string(const char *p, char *out, size_t out_sz) {
  if (!p || *p != '"') return NULL;
  ++p;
  size_t idx = 0;
  while (*p && *p != '"') {
    if (*p == '\\' && p[1] != '\0') ++p;
    if (out && idx + 1 < out_sz) out[idx++] = *p;
    ++p;
  }
  if (*p != '"') return NULL;
  if (out && out_sz > 0) out[idx] = '\0';
  return p + 1;
}

static const char *skip_json_value(const char *p) {
  if (!p) return NULL;
  if (*p == '{') {
    int d = 1; ++p;
    while (*p && d > 0) {
      if (*p == '{') { d++; ++p; }
      else if (*p == '}') { d--; if (d == 0) { ++p; break; } ++p; }
      else if (*p == '"') { p = parse_string(p, NULL, 0); if (!p) return NULL; }
      else ++p;
    }
    return d == 0 ? p : NULL;
  }
  if (*p == '[') {
    int d = 1; ++p;
    while (*p && d > 0) {
      if (*p == '[') { d++; ++p; }
      else if (*p == ']') { d--; if (d == 0) { ++p; break; } ++p; }
      else if (*p == '"') { p = parse_string(p, NULL, 0); if (!p) return NULL; }
      else ++p;
    }
    return d == 0 ? p : NULL;
  }
  if (*p == '"') return parse_string(p, NULL, 0);
  if (strncmp(p, "true", 4) == 0 && is_value_term((unsigned char)p[4])) return p + 4;
  if (strncmp(p, "false", 5) == 0 && is_value_term((unsigned char)p[5])) return p + 5;
  if (strncmp(p, "null", 4) == 0 && is_value_term((unsigned char)p[4])) return p + 4;
  while (*p && *p != ',' && *p != '}' && *p != ']') ++p;
  return p;
}

static const char *parse_json_int(const char *p, int *out) {
  char *end;
  long v = strtol(p, &end, 10);
  if (end == p) return NULL;
  *out = (int)v;
  return end;
}

static const char *parse_json_double(const char *p, double *out) {
  char *end;
  double v = strtod(p, &end);
  if (end == p) return NULL;
  *out = v;
  return end;
}

static const char *parse_json_bool(const char *p, int *out) {
  if (strncmp(p, "true", 4) == 0 && is_value_term((unsigned char)p[4])) { *out = 1; return p + 4; }
  if (strncmp(p, "false", 5) == 0 && is_value_term((unsigned char)p[5])) { *out = 0; return p + 5; }
  return NULL;
}

/* Parse JSON int array into fixed-size buffer, return count */
static const char *parse_json_int_array(const char *p, int *arr, int max_count, int *out_count) {
  *out_count = 0;
  if (!p || *p != '[') return NULL;
  ++p; p = skip_ws(p);
  if (*p == ']') { return p + 1; }
  while (*p) {
    p = skip_ws(p);
    if (strncmp(p, "null", 4) == 0 && is_value_term((unsigned char)p[4])) {
      p += 4;
      if (*out_count < max_count) { arr[*out_count] = CRSF_CENTER; (*out_count)++; }
    } else {
      int val;
      const char *next = parse_json_int(p, &val);
      if (!next) return NULL;
      if (*out_count < max_count) { arr[*out_count] = val; (*out_count)++; }
      p = next;
    }
    p = skip_ws(p);
    if (*p == ',') { ++p; continue; }
    if (*p == ']') return p + 1;
    return NULL;
  }
  return NULL;
}

/* Parse JSON string array into fixed-size buffer */
static const char *parse_json_string_array(const char *p, char arr[][HOST_LEN], int max_count, int *out_count) {
  *out_count = 0;
  if (!p || *p != '[') return NULL;
  ++p; p = skip_ws(p);
  if (*p == ']') { return p + 1; }
  while (*p) {
    p = skip_ws(p);
    if (*p != '"') return NULL;
    char tmp[HOST_LEN];
    const char *next = parse_string(p, tmp, sizeof(tmp));
    if (!next) return NULL;
    if (*out_count < max_count) {
      snprintf(arr[*out_count], HOST_LEN, "%s", tmp);
      (*out_count)++;
    }
    p = next;
    p = skip_ws(p);
    if (*p == ',') { ++p; continue; }
    if (*p == ']') return p + 1;
    return NULL;
  }
  return NULL;
}

/* ---------------------------------------------------------------------------
 * Config JSON parser
 * --------------------------------------------------------------------------- */

static void config_defaults(config_t *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  snprintf(cfg->host, sizeof(cfg->host), "127.0.0.1");
  cfg->port = 5005;
  cfg->interval_ms = 400;
  cfg->action_timeout_ms = 5000;
  cfg->menu_asset_id = 7;
  snprintf(cfg->webui_host, sizeof(cfg->webui_host), "0.0.0.0");
  cfg->webui_port = 8060;
  snprintf(cfg->webui_html, sizeof(cfg->webui_html), "waybeam_hub_c.html");
  snprintf(cfg->sse_url, sizeof(cfg->sse_url), "http://127.0.0.1:8070/sse");
  snprintf(cfg->priority, sizeof(cfg->priority), "serial");
  cfg->priority_fallback_s = 5.0;
  cfg->extra_dest_count = 0;

  /* Splashscreen defaults */
  cfg->splashscreen.enabled = 0;
  cfg->splashscreen.asset_id = -1;
  cfg->splashscreen.duration_s = 5.0;
  cfg->splashscreen.opacity = -1;
  cfg->splashscreen.delay_s = 0.0;
  cfg->splashscreen.fadeout_s = 0.0;

  /* Tuning defaults */
  tuning_t *t = &cfg->tuning;
  t->crsf_axis_deadband = 120;
  t->crsf_action_threshold = 1400;
  t->crsf_menu_toggle_ch_low_max = 500;
  t->crsf_menu_toggle_ch4_min = 1500;
  t->crsf_nav_debounce_ms = 100;
  t->crsf_select_debounce_ms = 250;
  t->crsf_menu_toggle_hold_s = 1.0;
  t->crsf_sample_interval_ms = 40;
  t->radio_trigger_debounce_ms = 100;
  t->radio_reset_debounce_ms = 0;
  t->menu_inactivity_timeout_s = 30.0;
  t->source_stale_s = 1.0;
}

static int parse_tuning_object(const char *p, tuning_t *t) {
  if (!p || *p != '{') return -1;
  ++p;
  while (*p) {
    p = skip_ws(p);
    if (*p == '}') return 0;
    if (*p != '"') return -1;
    char key[64];
    const char *next = parse_string(p, key, sizeof(key));
    if (!next) return -1;
    p = skip_ws(next);
    if (*p != ':') return -1;
    ++p; p = skip_ws(p);

    if (strcmp(key, "crsf_axis_deadband") == 0) { p = parse_json_int(p, &t->crsf_axis_deadband); }
    else if (strcmp(key, "crsf_action_threshold") == 0) { p = parse_json_int(p, &t->crsf_action_threshold); }
    else if (strcmp(key, "crsf_menu_toggle_ch_low_max") == 0) { p = parse_json_int(p, &t->crsf_menu_toggle_ch_low_max); }
    else if (strcmp(key, "crsf_menu_toggle_ch4_min") == 0) { p = parse_json_int(p, &t->crsf_menu_toggle_ch4_min); }
    else if (strcmp(key, "crsf_nav_debounce_ms") == 0) { p = parse_json_int(p, &t->crsf_nav_debounce_ms); }
    else if (strcmp(key, "crsf_select_debounce_ms") == 0) { p = parse_json_int(p, &t->crsf_select_debounce_ms); }
    else if (strcmp(key, "crsf_menu_toggle_hold_s") == 0) { p = parse_json_double(p, &t->crsf_menu_toggle_hold_s); }
    else if (strcmp(key, "crsf_sample_interval_ms") == 0) { p = parse_json_int(p, &t->crsf_sample_interval_ms); }
    else if (strcmp(key, "radio_trigger_debounce_ms") == 0) { p = parse_json_int(p, &t->radio_trigger_debounce_ms); }
    else if (strcmp(key, "radio_reset_debounce_ms") == 0) { p = parse_json_int(p, &t->radio_reset_debounce_ms); }
    else if (strcmp(key, "menu_inactivity_timeout_s") == 0) { p = parse_json_double(p, &t->menu_inactivity_timeout_s); }
    else if (strcmp(key, "source_stale_s") == 0) { p = parse_json_double(p, &t->source_stale_s); }
    else { p = skip_json_value(p); }

    if (!p) return -1;
    p = skip_ws(p);
    if (*p == ',') { ++p; continue; }
    if (*p == '}') return 0;
    return -1;
  }
  return -1;
}

static int parse_splashscreen_object(const char *p, splashscreen_t *s) {
  if (!p || *p != '{') return -1;
  ++p;
  while (*p) {
    p = skip_ws(p);
    if (*p == '}') return 0;
    if (*p != '"') return -1;
    char key[64];
    const char *next = parse_string(p, key, sizeof(key));
    if (!next) return -1;
    p = skip_ws(next);
    if (*p != ':') return -1;
    ++p; p = skip_ws(p);

    if (strcmp(key, "enabled") == 0) { p = parse_json_bool(p, &s->enabled); }
    else if (strcmp(key, "asset_id") == 0) { p = parse_json_int(p, &s->asset_id); }
    else if (strcmp(key, "duration_s") == 0) { p = parse_json_double(p, &s->duration_s); }
    else if (strcmp(key, "opacity") == 0) { p = parse_json_int(p, &s->opacity); }
    else if (strcmp(key, "delay_s") == 0) { p = parse_json_double(p, &s->delay_s); }
    else if (strcmp(key, "fadeout_s") == 0) { p = parse_json_double(p, &s->fadeout_s); }
    else { p = skip_json_value(p); }

    if (!p) return -1;
    p = skip_ws(p);
    if (*p == ',') { ++p; continue; }
    if (*p == '}') return 0;
    return -1;
  }
  return -1;
}

static int parse_config_json(const char *path, config_t *cfg) {
  config_defaults(cfg);

  FILE *f = fopen(path, "r");
  if (!f) { LOGE("Cannot open config: %s", path); return -1; }
  char buf[JSON_BUF_SIZE];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';

  const char *p = skip_ws(buf);
  if (*p != '{') { LOGE("Config is not a JSON object"); return -1; }
  ++p;

  while (*p) {
    p = skip_ws(p);
    if (*p == '}') return 0;
    if (*p != '"') { LOGE("Expected key in config JSON"); return -1; }
    char key[64];
    const char *next = parse_string(p, key, sizeof(key));
    if (!next) return -1;
    p = skip_ws(next);
    if (*p != ':') return -1;
    ++p; p = skip_ws(p);

    if (strcmp(key, "host") == 0) { p = parse_string(p, cfg->host, sizeof(cfg->host)); }
    else if (strcmp(key, "port") == 0) { p = parse_json_int(p, &cfg->port); }
    else if (strcmp(key, "interval_ms") == 0) { p = parse_json_int(p, &cfg->interval_ms); }
    else if (strcmp(key, "actions_ini") == 0) { p = parse_string(p, cfg->actions_ini, sizeof(cfg->actions_ini)); }
    else if (strcmp(key, "action_timeout_ms") == 0) { p = parse_json_int(p, &cfg->action_timeout_ms); }
    else if (strcmp(key, "action_shell") == 0) { p = parse_string(p, cfg->action_shell, sizeof(cfg->action_shell)); }
    else if (strcmp(key, "menu_asset_id") == 0) { p = parse_json_int(p, &cfg->menu_asset_id); }
    else if (strcmp(key, "webui_host") == 0) { p = parse_string(p, cfg->webui_host, sizeof(cfg->webui_host)); }
    else if (strcmp(key, "webui_port") == 0) { p = parse_json_int(p, &cfg->webui_port); }
    else if (strcmp(key, "webui_html") == 0) { p = parse_string(p, cfg->webui_html, sizeof(cfg->webui_html)); }
    else if (strcmp(key, "sse_url") == 0) { p = parse_string(p, cfg->sse_url, sizeof(cfg->sse_url)); }
    else if (strcmp(key, "priority") == 0) { p = parse_string(p, cfg->priority, sizeof(cfg->priority)); }
    else if (strcmp(key, "priority_fallback_s") == 0) { p = parse_json_double(p, &cfg->priority_fallback_s); }
    else if (strcmp(key, "verbose") == 0) { p = parse_json_bool(p, &cfg->verbose); }
    else if (strcmp(key, "initial_off") == 0) {
      p = parse_json_int_array(p, cfg->initial_off, ASSET_COUNT, &cfg->initial_off_count);
    }
    else if (strcmp(key, "extra_destinations") == 0) {
      p = parse_json_string_array(p, cfg->extra_dest_specs, MAX_DESTINATIONS, &cfg->extra_dest_count);
    }
    else if (strcmp(key, "tuning") == 0) {
      if (parse_tuning_object(p, &cfg->tuning) < 0) return -1;
      p = skip_json_value(p);
    }
    else if (strcmp(key, "splashscreen") == 0) {
      if (parse_splashscreen_object(p, &cfg->splashscreen) < 0) return -1;
      p = skip_json_value(p);
    }
    else { p = skip_json_value(p); }

    if (!p) { LOGE("JSON parse error at key '%s'", key); return -1; }
    p = skip_ws(p);
    if (*p == ',') { ++p; continue; }
    if (*p == '}') return 0;
    LOGE("Unexpected char '%c' in config JSON", *p);
    return -1;
  }
  return -1;
}

/* ---------------------------------------------------------------------------
 * INI Parser
 * --------------------------------------------------------------------------- */

static int strcasecmp_section(const char *a, const char *b) {
  while (*a && *b) {
    if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 1;
    a++; b++;
  }
  return *a != *b;
}

static int parse_radio_condition(const char *spec, int *ch_idx, int *min_val, int *max_val) {
  /* Format: "1200<ch1<1500" — manual parse */
  int mn, ch, mx;

  /* Manual parse: digits '<' 'ch' digits '<' digits */
  const char *p = spec;
  while (isspace((unsigned char)*p)) p++;

  char *end;
  mn = (int)strtol(p, &end, 10);
  if (end == p) return -1;
  p = end;
  while (isspace((unsigned char)*p)) p++;
  if (*p != '<') return -1;
  p++;
  while (isspace((unsigned char)*p)) p++;

  /* expect 'ch' or 'CH' */
  if (tolower((unsigned char)p[0]) != 'c' || tolower((unsigned char)p[1]) != 'h') return -1;
  p += 2;
  while (isspace((unsigned char)*p)) p++;

  ch = (int)strtol(p, &end, 10);
  if (end == p) return -1;
  p = end;
  while (isspace((unsigned char)*p)) p++;
  if (*p != '<') return -1;
  p++;
  while (isspace((unsigned char)*p)) p++;

  mx = (int)strtol(p, &end, 10);
  if (end == p) return -1;

  if (ch < 1 || ch > CRSF_DISPLAY_CHANNELS) return -1;
  if (mn >= mx) return -1;

  *ch_idx = ch - 1;
  *min_val = mn;
  *max_val = mx;
  return 0;
}

static int load_actions_ini(const char *path, app_state_t *app) {
  app->action_count = 0;
  app->section_count = 0;
  app->radio_rule_count = 0;

  if (!path || !path[0]) return 0;

  FILE *f = fopen(path, "r");
  if (!f) { LOGE("Cannot open menu INI: %s", path); return -1; }

  char line[INI_LINE_BUF];
  char current_section[SECTION_NAME_LEN] = "";
  int in_radio = 0;

  while (fgets(line, sizeof(line), f)) {
    /* Strip trailing whitespace/newline */
    int len = (int)strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' '))
      line[--len] = '\0';

    /* Skip empty and comment lines */
    const char *p = line;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '\0' || *p == ';' || *p == '#') continue;

    /* Section header */
    if (*p == '[') {
      p++;
      char sec[SECTION_NAME_LEN];
      int si = 0;
      while (*p && *p != ']' && si < (int)sizeof(sec) - 1)
        sec[si++] = *p++;
      sec[si] = '\0';

      /* Trim trailing spaces */
      while (si > 0 && sec[si-1] == ' ') sec[--si] = '\0';

      snprintf(current_section, sizeof(current_section), "%s", sec);
      in_radio = (strcasecmp_section(sec, "RADIO") == 0);

      /* Skip reserved sections (ASSETS, RADIO) */
      if (in_radio) continue;
      if (strcasecmp_section(sec, "ASSETS") == 0) continue;

      /* Register section in order */
      if (app->section_count < MAX_SECTIONS) {
        int found = 0;
        for (int i = 0; i < app->section_count; i++) {
          if (strcmp(app->section_order[i], sec) == 0) { found = 1; break; }
        }
        if (!found) {
          snprintf(app->section_order[app->section_count], SECTION_NAME_LEN, "%s", sec);
          app->section_count++;
        }
      }
      continue;
    }

    /* Key = Value */
    const char *eq = strchr(p, '=');
    if (!eq) continue;

    char name[ACTION_NAME_LEN];
    int ni = 0;
    const char *np = p;
    while (np < eq && ni < (int)sizeof(name) - 1) {
      name[ni++] = *np++;
    }
    name[ni] = '\0';
    /* Trim trailing spaces from name */
    while (ni > 0 && name[ni-1] == ' ') name[--ni] = '\0';

    const char *vp = eq + 1;
    while (isspace((unsigned char)*vp)) vp++;
    char value[ACTION_CMD_LEN];
    snprintf(value, sizeof(value), "%s", vp);
    /* Trim trailing spaces */
    int vlen = (int)strlen(value);
    while (vlen > 0 && value[vlen-1] == ' ') value[--vlen] = '\0';

    if (!name[0] || !value[0]) continue;

    if (in_radio) {
      /* Parse radio rule: name is one or more conditions joined by '&&' */
      if (app->radio_rule_count >= MAX_RADIO_RULES) {
        LOGW("Max radio rules reached, skipping");
        continue;
      }
      if (app->action_count >= MAX_ACTIONS) {
        LOGW("Max actions reached, skipping radio rule");
        continue;
      }

      radio_rule_t rule;
      memset(&rule, 0, sizeof(rule));
      rule.condition_count = 0;

      /* Split name on '&&' and parse each condition */
      char cond_buf[ACTION_NAME_LEN];
      snprintf(cond_buf, sizeof(cond_buf), "%s", name);
      int parse_ok = 1;

      char *cursor = cond_buf;
      while (*cursor) {
        while (isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == '\0') break;

        /* Find next '&&' delimiter */
        char *delim = strstr(cursor, "&&");
        char *token_end;
        if (delim) {
          token_end = delim;
        } else {
          token_end = cursor + strlen(cursor);
        }

        /* Trim trailing whitespace from token */
        char *te = token_end;
        while (te > cursor && isspace((unsigned char)te[-1])) te--;
        char saved = *te;
        *te = '\0';

        if (*cursor != '\0') {
          if (rule.condition_count >= MAX_CONDITIONS_PER_RULE) {
            LOGW("Too many conditions in RADIO rule (max %d): '%s'", MAX_CONDITIONS_PER_RULE, name);
            parse_ok = 0;
            break;
          }

          int ch_idx, min_val, max_val;
          if (parse_radio_condition(cursor, &ch_idx, &min_val, &max_val) < 0) {
            LOGW("Invalid RADIO condition: '%s' in rule '%s'", cursor, name);
            parse_ok = 0;
            break;
          }

          rule.conds[rule.condition_count].channel_index = ch_idx;
          rule.conds[rule.condition_count].min_value = min_val;
          rule.conds[rule.condition_count].max_value = max_val;
          rule.condition_count++;
        }

        *te = saved;
        if (delim)
          cursor = delim + 2; /* skip past '&&' */
        else
          break;
      }

      if (!parse_ok || rule.condition_count == 0) continue;

      /* Build action name from all conditions */
      char action_name[ACTION_NAME_LEN];
      int pos = 0;
      for (int c = 0; c < rule.condition_count; c++) {
        if (c > 0) pos += snprintf(action_name + pos, ACTION_NAME_LEN - pos, " && ");
        pos += snprintf(action_name + pos, ACTION_NAME_LEN - pos, "%d<ch%d<%d",
                        rule.conds[c].min_value, rule.conds[c].channel_index + 1,
                        rule.conds[c].max_value);
      }

      int ai = app->action_count++;
      snprintf(app->actions[ai].section, SECTION_NAME_LEN, "RADIO");
      snprintf(app->actions[ai].name, ACTION_NAME_LEN, "%s", action_name);
      snprintf(app->actions[ai].command, ACTION_CMD_LEN, "%s", value);

      rule.action_index = ai;
      snprintf(rule.name, ACTION_NAME_LEN, "%s", action_name);
      app->radio_rules[app->radio_rule_count++] = rule;
    } else {
      /* Regular action */
      if (app->action_count >= MAX_ACTIONS) {
        LOGW("Max actions reached, skipping");
        continue;
      }
      int ai = app->action_count++;
      snprintf(app->actions[ai].section, SECTION_NAME_LEN, "%s", current_section);
      snprintf(app->actions[ai].name, ACTION_NAME_LEN, "%s", name);
      snprintf(app->actions[ai].command, ACTION_CMD_LEN, "%s", value);
    }
  }

  fclose(f);
  return 0;
}

/* ---------------------------------------------------------------------------
 * Menu Builder
 * --------------------------------------------------------------------------- */

static void build_top_entries(app_state_t *app) {
  int n = 0;

  /* ASSETS section */
  app->top_entries[n].kind = ENTRY_SECTION;
  snprintf(app->top_entries[n].section, SECTION_NAME_LEN, "ASSETS");
  app->top_entries[n].asset_id = -1;
  app->top_entries[n].action_index = -1;
  n++;

  /* Custom sections */
  for (int i = 0; i < app->section_count && n < MAX_MENU_ENTRIES - 1; i++) {
    app->top_entries[n].kind = ENTRY_SECTION;
    snprintf(app->top_entries[n].section, SECTION_NAME_LEN, "%s", app->section_order[i]);
    app->top_entries[n].asset_id = -1;
    app->top_entries[n].action_index = -1;
    n++;
  }

  /* EXIT */
  app->top_entries[n].kind = ENTRY_EXIT;
  app->top_entries[n].section[0] = '\0';
  app->top_entries[n].asset_id = -1;
  app->top_entries[n].action_index = -1;
  n++;

  app->top_entry_count = n;
}

static int find_sub_section_index(app_state_t *app, const char *section) {
  for (int i = 0; i < app->sub_section_count; i++) {
    if (strcmp(app->sub_entry_section[i], section) == 0) return i;
  }
  return -1;
}

static void build_sub_entries_for(app_state_t *app, const char *section) {
  if (app->sub_section_count >= MAX_SECTIONS + 2) return;
  int si = app->sub_section_count++;
  snprintf(app->sub_entry_section[si], SECTION_NAME_LEN, "%s", section);
  int n = 0;

  if (strcasecmp_section(section, "ASSETS") == 0) {
    for (int a = 0; a < ASSET_COUNT && n < MAX_MENU_ENTRIES - 1; a++) {
      app->sub_entries[si][n].kind = ENTRY_ASSET;
      app->sub_entries[si][n].asset_id = a;
      app->sub_entries[si][n].action_index = -1;
      app->sub_entries[si][n].section[0] = '\0';
      n++;
    }
  } else {
    /* Action items for this section */
    for (int a = 0; a < app->action_count && n < MAX_MENU_ENTRIES - 1; a++) {
      if (strcmp(app->actions[a].section, section) == 0) {
        app->sub_entries[si][n].kind = ENTRY_ACTION;
        app->sub_entries[si][n].asset_id = -1;
        app->sub_entries[si][n].action_index = a;
        app->sub_entries[si][n].section[0] = '\0';
        n++;
      }
    }
  }

  /* RETURN at end */
  app->sub_entries[si][n].kind = ENTRY_RETURN;
  app->sub_entries[si][n].asset_id = -1;
  app->sub_entries[si][n].action_index = -1;
  app->sub_entries[si][n].section[0] = '\0';
  n++;

  app->sub_entry_count[si] = n;
}

static void build_all_menus(app_state_t *app) {
  app->sub_section_count = 0;
  build_top_entries(app);
  build_sub_entries_for(app, "ASSETS");
  for (int i = 0; i < app->section_count; i++)
    build_sub_entries_for(app, app->section_order[i]);
}

static void get_current_entries(app_state_t *app, menu_entry_t **entries, int *count) {
  if (app->current_section[0] == '\0') {
    *entries = app->top_entries;
    *count = app->top_entry_count;
    return;
  }
  int si = find_sub_section_index(app, app->current_section);
  if (si >= 0) {
    *entries = app->sub_entries[si];
    *count = app->sub_entry_count[si];
    return;
  }
  /* Fallback: just RETURN */
  static menu_entry_t fallback = { .kind = ENTRY_RETURN };
  *entries = &fallback;
  *count = 1;
}

/* ---------------------------------------------------------------------------
 * Menu Renderer
 * --------------------------------------------------------------------------- */

static int display_entry_text(const app_state_t *app, const menu_entry_t *e, char *out, int out_sz) {
  switch (e->kind) {
    case ENTRY_SECTION:
      return snprintf(out, out_sz, "[%s]", e->section);
    case ENTRY_EXIT:
      return snprintf(out, out_sz, "EXIT");
    case ENTRY_RETURN:
      return snprintf(out, out_sz, "RETURN");
    case ENTRY_ASSET: {
      int state = app->asset_enabled[e->asset_id];
      const char *s = state < 0 ? "?" : (state ? "ON" : "OFF");
      return snprintf(out, out_sz, "ASSET %d %s", e->asset_id, s);
    }
    case ENTRY_ACTION:
      if (e->action_index >= 0 && e->action_index < app->action_count)
        return snprintf(out, out_sz, "%s", app->actions[e->action_index].name);
      return snprintf(out, out_sz, "UNKNOWN");
  }
  return snprintf(out, out_sz, "UNKNOWN");
}

static void build_three_slot_texts(const app_state_t *app, menu_entry_t *entries, int count,
                                   int selected, char texts[3][MAX_OSD_TEXT_CHARS + 1]) {
  texts[0][0] = texts[1][0] = texts[2][0] = '\0';
  if (count <= 0) return;

  int window_start;
  if (count >= 3)
    window_start = clamp_i(selected - 1, 0, count - 3);
  else
    window_start = 0;

  for (int i = 0; i < 3 && (window_start + i) < count; i++) {
    int idx = window_start + i;
    const char *prefix = (idx == selected) ? "> " : "  ";
    char entry_text[MAX_OSD_TEXT_CHARS + 1];
    display_entry_text(app, &entries[idx], entry_text, sizeof(entry_text));
    snprintf(texts[i], MAX_OSD_TEXT_CHARS + 1, "%s%s", prefix, entry_text);
  }
}

/* ---------------------------------------------------------------------------
 * UDP OSD Producer
 * --------------------------------------------------------------------------- */

static int setup_udp_socket(void) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) { LOGE("UDP socket: %s", strerror(errno)); return -1; }
  return fd;
}

static int resolve_destination(udp_dest_t *dest) {
  memset(&dest->addr, 0, sizeof(dest->addr));
  dest->addr.sin_family = AF_INET;
  dest->addr.sin_port = htons((uint16_t)dest->port);
  if (inet_pton(AF_INET, dest->host, &dest->addr.sin_addr) != 1) {
    LOGE("Invalid destination address: %s", dest->host);
    return -1;
  }
  return 0;
}

static int parse_dest_spec(const char *spec, udp_dest_t *dest) {
  const char *colon = strrchr(spec, ':');
  if (!colon || colon == spec) return -1;
  int host_len = (int)(colon - spec);
  if (host_len >= (int)sizeof(dest->host)) host_len = (int)sizeof(dest->host) - 1;
  memcpy(dest->host, spec, host_len);
  dest->host[host_len] = '\0';
  dest->port = atoi(colon + 1);
  if (dest->port <= 0 || dest->port > 65535) return -1;
  return resolve_destination(dest);
}

/* JSON-escape a string into buffer, returns chars written (excluding NUL) */
static int json_escape(const char *src, char *dst, int dst_sz) {
  int w = 0;
  for (const char *p = src; *p && w < dst_sz - 1; p++) {
    if (*p == '"' || *p == '\\') {
      if (w + 2 >= dst_sz) break;
      dst[w++] = '\\';
      dst[w++] = *p;
    } else if ((unsigned char)*p < 0x20) {
      /* skip control chars */
    } else {
      dst[w++] = *p;
    }
  }
  dst[w] = '\0';
  return w;
}

static int build_osd_payload(app_state_t *app, char texts[3][MAX_OSD_TEXT_CHARS + 1],
                             char *buf, int buf_sz) {
  char esc[3][MAX_OSD_TEXT_CHARS * 2 + 1];
  for (int i = 0; i < 3; i++)
    json_escape(texts[i], esc[i], sizeof(esc[i]));

  /* Build texts array: null for slots 0-4, menu text for slots 5,6,7 */
  int n = snprintf(buf, buf_sz,
    "{\"texts\":[null,null,null,null,null,\"%s\",\"%s\",\"%s\"]",
    esc[0], esc[1], esc[2]);
  if (n >= buf_sz) n = buf_sz - 1;

  /* Asset updates */
  int has_updates = 0;
  for (int i = 0; i < ASSET_COUNT; i++) {
    if (app->pending_asset_updates[i] >= 0) { has_updates = 1; break; }
  }
  if (app->splash_pending_opacity >= 0) has_updates = 1;
  if (has_updates) {
    n += snprintf(buf + n, buf_sz - n, ",\"asset_updates\":[");
    if (n >= buf_sz) n = buf_sz - 1;
    int first = 1;
    int splash_asset_emitted = 0;
    splashscreen_t *sp = &app->cfg.splashscreen;
    for (int i = 0; i < ASSET_COUNT; i++) {
      if (app->pending_asset_updates[i] < 0) continue;
      if (!first) { n += snprintf(buf + n, buf_sz - n, ","); if (n >= buf_sz) n = buf_sz - 1; }
      /* Include opacity for splash asset enable */
      if (sp->enabled && i == sp->asset_id && app->pending_asset_updates[i] == 1 &&
          sp->opacity >= 0) {
        int opa = (app->splash_pending_opacity >= 0) ? app->splash_pending_opacity : sp->opacity;
        n += snprintf(buf + n, buf_sz - n,
                      "{\"id\":%d,\"enabled\":true,\"opacity\":%d}",
                      i, opa);
        splash_asset_emitted = 1;
      } else {
        n += snprintf(buf + n, buf_sz - n, "{\"id\":%d,\"enabled\":%s}",
                      i, app->pending_asset_updates[i] ? "true" : "false");
        if (sp->enabled && i == sp->asset_id) splash_asset_emitted = 1;
      }
      if (n >= buf_sz) n = buf_sz - 1;
      first = 0;
    }
    /* Opacity-only update for splash asset (no enable/disable already emitted) */
    if (app->splash_pending_opacity >= 0 && !splash_asset_emitted &&
        sp->enabled && sp->asset_id >= 0 && sp->asset_id < ASSET_COUNT) {
      if (!first) { n += snprintf(buf + n, buf_sz - n, ","); if (n >= buf_sz) n = buf_sz - 1; }
      n += snprintf(buf + n, buf_sz - n,
                    "{\"id\":%d,\"opacity\":%d}",
                    sp->asset_id, app->splash_pending_opacity);
      if (n >= buf_sz) n = buf_sz - 1;
    }
    n += snprintf(buf + n, buf_sz - n, "]");
    if (n >= buf_sz) n = buf_sz - 1;
  }

  n += snprintf(buf + n, buf_sz - n, "}");
  if (n >= buf_sz) n = buf_sz - 1;
  return n;
}

static void send_all_destinations(app_state_t *app, const char *payload, int payload_len) {
  for (int i = 0; i < app->dest_count; i++) {
    ssize_t sent = sendto(app->udp_fd, payload, payload_len, 0,
                          (struct sockaddr *)&app->destinations[i].addr,
                          sizeof(app->destinations[i].addr));
    if (sent < 0) {
      LOGW("sendto %s:%d failed: %s",
           app->destinations[i].host, app->destinations[i].port, strerror(errno));
    }
  }
}

static void send_clear_payload(app_state_t *app) {
  /* Clear menu text slots and disable menu asset */
  char buf[256];
  int n = snprintf(buf, sizeof(buf),
    "{\"texts\":[null,null,null,null,null,\"\",\"\",\"\"],"
    "\"asset_updates\":[{\"id\":%d,\"enabled\":false}]}",
    app->cfg.menu_asset_id);
  send_all_destinations(app, buf, n);
}

/* ---------------------------------------------------------------------------
 * WebUI bridge (single-user HTTP)
 * --------------------------------------------------------------------------- */

static int queue_key(app_state_t *app, int key) {
  if (app->key_count >= MAX_KEY_QUEUE) return -1;
  app->key_queue[app->key_count++] = key;
  return 0;
}

static void webui_send_response(int fd, const char *status, const char *content_type,
                                const char *body, int body_len) {
  char head[256];
  int n = snprintf(head, sizeof(head),
                   "HTTP/1.1 %s\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %d\r\n"
                   "Access-Control-Allow-Origin: *\r\n"
                   "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                   "Access-Control-Allow-Headers: Content-Type\r\n"
                   "Connection: close\r\n\r\n",
                   status, content_type, body_len);
  if (n < 0) return;
  send(fd, head, (size_t)n, MSG_NOSIGNAL);
  if (body_len > 0) send(fd, body, (size_t)body_len, MSG_NOSIGNAL);
}

static int webui_find_content_length(const char *headers) {
  const char *p = headers;
  while ((p = strstr(p, "\n")) != NULL) {
    p++;
    if (strncasecmp(p, "Content-Length:", 15) == 0) {
      p += 15;
      while (*p == ' ' || *p == '\t') p++;
      return atoi(p);
    }
  }
  return 0;
}

static int webui_enqueue_command(app_state_t *app, const char *body) {
  if (strstr(body, "\"menu\"")) return queue_key(app, KEY_MENU_TOGGLE);
  if (strstr(body, "\"up\"")) return queue_key(app, KEY_UP);
  if (strstr(body, "\"down\"")) return queue_key(app, KEY_DOWN);
  if (strstr(body, "\"select\"")) return queue_key(app, KEY_SELECT);
  return -1;
}

static int webui_build_state_json(app_state_t *app, char *out, int out_sz) {
  menu_entry_t *entries;
  int entry_count;
  get_current_entries(app, &entries, &entry_count);
  int menu_visible = (app->cfg.menu_asset_id >= 0 && app->cfg.menu_asset_id < ASSET_COUNT &&
                      app->asset_enabled[app->cfg.menu_asset_id] == 1);

  char status_esc[512];
  char section_esc[128];
  char selected_esc[128];
  char selected_raw[96] = "";
  if (entry_count > 0 && app->selected >= 0 && app->selected < entry_count)
    display_entry_text(app, &entries[app->selected], selected_raw, sizeof(selected_raw));
  json_escape(app->status, status_esc, sizeof(status_esc));
  json_escape(app->current_section[0] ? app->current_section : "ROOT", section_esc, sizeof(section_esc));
  json_escape(selected_raw, selected_esc, sizeof(selected_esc));

  return snprintf(out, out_sz,
                  "{\"type\":\"menu_state\",\"status\":\"%s\","
                  "\"active_source\":\"%s\",\"menu_visible\":%s,"
                  "\"current_section\":\"%s\",\"selected\":%d,\"entry_count\":%d,"
                  "\"selected_label\":\"%s\"}",
                  status_esc,
                  app->active_source == 0 ? "serial" : (app->active_source == 1 ? "joystick" : "none"),
                  menu_visible ? "true" : "false",
                  section_esc,
                  app->selected,
                  entry_count,
                  selected_esc);
}

static void webui_close_client(webui_server_t *web) {
  if (web->client_fd >= 0) close(web->client_fd);
  web->client_fd = -1;
  web->req_len = 0;
}

static int webui_load_html(app_state_t *app) {
  FILE *f = fopen(app->cfg.webui_html, "r");
  if (!f) {
    webui_server_t *web = &app->webui;
    web->html_len = snprintf(web->html, sizeof(web->html),
                             "<html><body><h1>waybeam_hub</h1><p>Missing UI file: %s</p></body></html>",
                             app->cfg.webui_html);
    return -1;
  }
  webui_server_t *web = &app->webui;
  size_t n = fread(web->html, 1, sizeof(web->html) - 1, f);
  fclose(f);
  web->html[n] = '\0';
  web->html_len = (int)n;
  return 0;
}

static int webui_init(app_state_t *app) {
  webui_server_t *web = &app->webui;
  web->server_fd = -1;
  web->client_fd = -1;
  web->req_len = 0;
  webui_load_html(app);

  if (app->cfg.webui_port <= 0 || app->cfg.webui_port > 65535) {
    LOGW("WebUI disabled: invalid port %d", app->cfg.webui_port);
    return -1;
  }

  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    LOGW("WebUI socket failed: %s", strerror(errno));
    return -1;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)app->cfg.webui_port);
  if (inet_pton(AF_INET, app->cfg.webui_host, &addr.sin_addr) != 1) {
    LOGW("WebUI disabled: invalid host %s", app->cfg.webui_host);
    close(fd);
    return -1;
  }
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 2) < 0) {
    LOGW("WebUI disabled: bind/listen failed: %s", strerror(errno));
    close(fd);
    return -1;
  }

  web->server_fd = fd;
  LOGI("WebUI listening on http://%s:%d", app->cfg.webui_host, app->cfg.webui_port);
  return 0;
}

static void webui_shutdown(app_state_t *app) {
  webui_server_t *web = &app->webui;
  webui_close_client(web);
  if (web->server_fd >= 0) close(web->server_fd);
  web->server_fd = -1;
}

static void webui_handle_request(app_state_t *app, const char *req, int req_len) {
  webui_server_t *web = &app->webui;
  const char *hdr_end = strstr(req, "\r\n\r\n");
  if (!hdr_end) {
    webui_send_response(web->client_fd, "400 Bad Request", "application/json",
                        "{\"error\":\"bad request\"}", 23);
    return;
  }

  char method[8] = "";
  char path[128] = "";
  sscanf(req, "%7s %127s", method, path);

  int header_len = (int)(hdr_end - req) + 4;
  int body_len = req_len - header_len;
  const char *body = req + header_len;

  if (strcmp(method, "OPTIONS") == 0) {
    webui_send_response(web->client_fd, "204 No Content", "text/plain", "", 0);
    return;
  }

  if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
    webui_send_response(web->client_fd, "200 OK", "text/html; charset=utf-8", web->html, web->html_len);
    return;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/state") == 0) {
    char json[1024];
    int n = webui_build_state_json(app, json, sizeof(json));
    if (n < 0) n = 0;
    if (n >= (int)sizeof(json)) n = (int)sizeof(json) - 1;
    webui_send_response(web->client_fd, "200 OK", "application/json", json, n);
    return;
  }

  if (strcmp(method, "POST") == 0 && strcmp(path, "/command") == 0) {
    char tmp[512];
    int copy_n = body_len;
    if (copy_n >= (int)sizeof(tmp)) copy_n = (int)sizeof(tmp) - 1;
    memcpy(tmp, body, (size_t)copy_n);
    tmp[copy_n] = '\0';
    if (webui_enqueue_command(app, tmp) == 0)
      webui_send_response(web->client_fd, "200 OK", "application/json", "{\"ok\":true}", 11);
    else
      webui_send_response(web->client_fd, "400 Bad Request", "application/json", "{\"error\":\"invalid command\"}", 27);
    return;
  }

  webui_send_response(web->client_fd, "404 Not Found", "application/json", "{\"error\":\"not found\"}", 21);
}

static void webui_poll(app_state_t *app) {
  webui_server_t *web = &app->webui;
  if (web->server_fd < 0) return;

  if (web->client_fd < 0) {
    struct sockaddr_in cli_addr;
    socklen_t cli_len = sizeof(cli_addr);
    int cfd = accept4(web->server_fd, (struct sockaddr *)&cli_addr, &cli_len, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (cfd >= 0) {
      web->client_fd = cfd;
      web->req_len = 0;
    }
  } else {
    char *buf = web->req_buf;
    ssize_t n = recv(web->client_fd, buf + web->req_len, sizeof(web->req_buf) - 1 - web->req_len, 0);
    if (n <= 0) {
      webui_close_client(web);
      return;
    }
    web->req_len += (int)n;
    buf[web->req_len] = '\0';

    char *hdr_end = strstr(buf, "\r\n\r\n");
    if (!hdr_end) {
      if (web->req_len >= (int)sizeof(web->req_buf) - 1) webui_close_client(web);
      return;
    }

    int content_len = webui_find_content_length(buf);
    int header_len = (int)(hdr_end - buf) + 4;
    if (web->req_len < header_len + content_len) return;

    webui_handle_request(app, buf, header_len + content_len);
    webui_close_client(web);
  }
}

/* ---------------------------------------------------------------------------
 * SSE Client
 * --------------------------------------------------------------------------- */

static int sse_parse_url(const char *url, char *host, int *port, char *path) {
  /* Parse "http://host:port/path" */
  const char *p = url;
  if (strncmp(p, "http://", 7) == 0) p += 7;

  const char *slash = strchr(p, '/');
  const char *colon = NULL;

  /* Find colon for port, but only before the slash */
  for (const char *c = p; c < (slash ? slash : p + strlen(p)); c++) {
    if (*c == ':') { colon = c; break; }
  }

  if (colon) {
    int hlen = (int)(colon - p);
    if (hlen >= HOST_LEN) hlen = HOST_LEN - 1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';
    *port = atoi(colon + 1);
  } else {
    int hlen = (int)((slash ? slash : p + strlen(p)) - p);
    if (hlen >= HOST_LEN) hlen = HOST_LEN - 1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';
    *port = 80;
  }

  if (slash)
    snprintf(path, PATH_LEN, "%s", slash);
  else
    snprintf(path, PATH_LEN, "/");

  return 0;
}

static void sse_disconnect(sse_client_t *sse) {
  if (sse->fd >= 0) {
    close(sse->fd);
    sse->fd = -1;
  }
  sse->state = SSE_DISCONNECTED;
  sse->line_len = 0;
  sse->data_len = 0;
  sse->event_name[0] = '\0';
}

static int sse_try_connect(sse_client_t *sse) {
  sse_disconnect(sse);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)sse->port);
  if (inet_pton(AF_INET, sse->host, &addr.sin_addr) != 1) {
    /* Try resolving hostname */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", sse->port);
    if (getaddrinfo(sse->host, port_str, &hints, &res) != 0) {
      LOGW("SSE: cannot resolve %s", sse->host);
      return -1;
    }
    memcpy(&addr, res->ai_addr, sizeof(addr));
    freeaddrinfo(res);
  }

  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return -1;

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    if (errno != EINPROGRESS) {
      close(fd);
      return -1;
    }
    /* Non-blocking connect in progress — wait with poll() up to 2s */
    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    int rc = poll(&pfd, 1, 2000);
    if (rc <= 0 || !(pfd.revents & POLLOUT)) {
      close(fd);
      return -1;
    }
    /* Check for connect error */
    int err = 0;
    socklen_t elen = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err) {
      close(fd);
      return -1;
    }
  }

  /* Clear non-blocking for simpler read/write, set read timeout */
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

  /* Send HTTP GET for SSE */
  char req[512];
  int rlen = snprintf(req, sizeof(req),
    "GET %s HTTP/1.1\r\n"
    "Host: %s:%d\r\n"
    "Accept: text/event-stream\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: keep-alive\r\n"
    "\r\n",
    sse->path, sse->host, sse->port);

  ssize_t sent = write(fd, req, rlen);
  if (sent != rlen) {
    close(fd);
    return -1;
  }

  /* Set read timeout */
  struct timeval rtv = { .tv_sec = 20, .tv_usec = 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

  sse->fd = fd;
  sse->state = SSE_READING_HEADERS;
  sse->line_len = 0;
  sse->data_len = 0;
  sse->event_name[0] = '\0';
  LOGI("SSE connected to %s:%d%s", sse->host, sse->port, sse->path);
  return 0;
}

/* Parse SSE channels from JSON data line.
 * Returns source index (0=serial, 1=joystick) or -1 on error. */
static int sse_parse_channels(const char *json, int channels[CRSF_DISPLAY_CHANNELS]) {
  const char *p = skip_ws(json);
  if (*p != '{') return -1;
  ++p;

  char stream[32] = "";
  int got_channels = 0;

  while (*p) {
    p = skip_ws(p);
    if (*p == '}') break;
    if (*p != '"') return -1;

    char key[32];
    const char *next = parse_string(p, key, sizeof(key));
    if (!next) return -1;
    p = skip_ws(next);
    if (*p != ':') return -1;
    ++p; p = skip_ws(p);

    if (strcmp(key, "stream") == 0) {
      p = parse_string(p, stream, sizeof(stream));
      if (!p) return -1;
    } else if (strcmp(key, "channels") == 0) {
      int count;
      p = parse_json_int_array(p, channels, CRSF_DISPLAY_CHANNELS, &count);
      if (!p) return -1;
      /* Pad with center if fewer than 16 */
      for (int i = count; i < CRSF_DISPLAY_CHANNELS; i++)
        channels[i] = CRSF_CENTER;
      /* Clamp all values */
      for (int i = 0; i < CRSF_DISPLAY_CHANNELS; i++)
        channels[i] = clamp_crsf(channels[i]);
      if (count >= CRSF_MIN_CONTROL_CH)
        got_channels = 1;
    } else {
      p = skip_json_value(p);
      if (!p) return -1;
    }

    p = skip_ws(p);
    if (*p == ',') { ++p; continue; }
    if (*p == '}') break;
    return -1;
  }

  if (!got_channels) return -1;

  /* Determine source from stream name or event name */
  for (int i = 0; stream[i]; i++)
    stream[i] = (char)tolower((unsigned char)stream[i]);

  if (strcmp(stream, "serial") == 0) return 0;
  if (strcmp(stream, "joystick") == 0) return 1;
  return -1;
}

/* Process one complete SSE line. Called for each line of input.
 * Returns 1 if a channel update happened. */
static int sse_handle_line(sse_client_t *sse, const char *line, app_state_t *app) {
  if (sse->state == SSE_READING_HEADERS) {
    /* Empty line signals end of HTTP headers */
    if (line[0] == '\0') {
      sse->state = SSE_STREAMING;
      LOGI("SSE: streaming started");
    }
    return 0;
  }

  /* SSE_STREAMING */
  if (line[0] == '\0') {
    /* End of event — dispatch */
    if (sse->data_len > 0) {
      sse->data_buf[sse->data_len] = '\0';
      LOGV(app, "SSE raw event=%s data=%.200s",
           sse->event_name, sse->data_buf);
      /* Determine if this is a joystick/serial event */
      const char *event = sse->event_name;
      int channels[CRSF_DISPLAY_CHANNELS];
      int src = sse_parse_channels(sse->data_buf, channels);

      if (src < 0 && event[0]) {
        /* Use event name as stream hint */
        char lower[32];
        int li = 0;
        for (const char *e = event; *e && li < 31; e++)
          lower[li++] = (char)tolower((unsigned char)*e);
        lower[li] = '\0';

        /* Re-parse with stream fallback */
        int ch2[CRSF_DISPLAY_CHANNELS];
        int cnt;
        /* Try to just get channels array */
        const char *p = skip_ws(sse->data_buf);
        if (*p == '{') {
          ++p;
          while (*p) {
            p = skip_ws(p);
            if (*p == '}') break;
            if (*p != '"') break;
            char key[32];
            const char *next = parse_string(p, key, sizeof(key));
            if (!next) break;
            p = skip_ws(next);
            if (*p != ':') break;
            ++p; p = skip_ws(p);
            if (strcmp(key, "channels") == 0) {
              p = parse_json_int_array(p, ch2, CRSF_DISPLAY_CHANNELS, &cnt);
              if (p && cnt >= CRSF_MIN_CONTROL_CH) {
                for (int i = cnt; i < CRSF_DISPLAY_CHANNELS; i++) ch2[i] = CRSF_CENTER;
                for (int i = 0; i < CRSF_DISPLAY_CHANNELS; i++) ch2[i] = clamp_crsf(ch2[i]);
                memcpy(channels, ch2, sizeof(channels));

                if (strcmp(lower, "serial") == 0) src = 0;
                else if (strcmp(lower, "joystick") == 0) src = 1;
              }
              break;
            } else {
              p = skip_json_value(p);
              if (!p) break;
            }
            p = skip_ws(p);
            if (*p == ',') { ++p; continue; }
            break;
          }
        }
      }

      if (src >= 0 && src < 2) {
        source_state_t *ss = &app->sources[src];
        double now = monotonic_s();
        memcpy(ss->channels, channels, sizeof(channels));
        ss->last_update_mono = now;
        ss->link_up = 1;

        LOGV(app, "SSE rx src=%s ch=[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]",
             src == 0 ? "serial" : "joystick",
             channels[0], channels[1], channels[2], channels[3],
             channels[4], channels[5], channels[6], channels[7],
             channels[8], channels[9], channels[10], channels[11],
             channels[12], channels[13], channels[14], channels[15]);

        /* Update radio resets on every sample */
        if (app->radio_rule_count > 0) {
          radio_rule_state_t *rs = app->radio_states[src];
          for (int i = 0; i < app->radio_rule_count; i++) {
            radio_rule_t *rule = &app->radio_rules[i];
            int in_range = 1;
            for (int c = 0; c < rule->condition_count; c++) {
              int ci = rule->conds[c].channel_index;
              if (ci >= CRSF_DISPLAY_CHANNELS) { in_range = 0; break; }
              int val = ss->channels[ci];
              if (!(val > rule->conds[c].min_value && val < rule->conds[c].max_value))
                { in_range = 0; break; }
            }
            if (!in_range) {
              rs[i].enter_started = 0.0;
              if (rs[i].latched_in_range) {
                if (rs[i].outside_started <= 0.0)
                  rs[i].outside_started = now;
                double outside_ms = (now - rs[i].outside_started) * 1000.0;
                if (outside_ms >= app->cfg.tuning.radio_reset_debounce_ms) {
                  rs[i].latched_in_range = 0;
                  rs[i].outside_started = 0.0;
                }
              } else {
                rs[i].outside_started = 0.0;
              }
            } else {
              rs[i].outside_started = 0.0;
            }
          }
        }

        sse->data_len = 0;
        sse->event_name[0] = '\0';
        return 1;
      }
    }
    sse->data_len = 0;
    sse->event_name[0] = '\0';
    return 0;
  }

  if (line[0] == ':') return 0; /* Comment */

  if (strncmp(line, "event:", 6) == 0) {
    const char *val = line + 6;
    while (*val == ' ') val++;
    snprintf(sse->event_name, sizeof(sse->event_name), "%s", val);
    return 0;
  }

  if (strncmp(line, "data:", 5) == 0) {
    const char *val = line + 5;
    while (*val == ' ') val++;
    int vlen = (int)strlen(val);
    if (sse->data_len + vlen + 1 < SSE_DATA_BUF) {
      if (sse->data_len > 0)
        sse->data_buf[sse->data_len++] = '\n';
      memcpy(sse->data_buf + sse->data_len, val, vlen);
      sse->data_len += vlen;
    }
    return 0;
  }

  return 0;
}

/* Read and process available data from SSE socket.
 * Returns: 1=channel update, 0=ok no update, -1=error/disconnect */
static int sse_process(sse_client_t *sse, app_state_t *app) {
  char buf[2048];
  ssize_t n = read(sse->fd, buf, sizeof(buf));
  if (n <= 0) {
    if (n == 0) { LOGI("SSE: connection closed"); return -1; }
    if (errno == EAGAIN || errno == EINTR) return 0;
    LOGW("SSE: read error: %s", strerror(errno));
    return -1;
  }

  int got_update = 0;
  for (ssize_t i = 0; i < n; i++) {
    char c = buf[i];
    if (c == '\r') continue;
    if (c == '\n') {
      sse->line_buf[sse->line_len] = '\0';
      if (sse_handle_line(sse, sse->line_buf, app) > 0)
        got_update = 1;
      sse->line_len = 0;
    } else {
      if (sse->line_len < SSE_LINE_BUF - 1)
        sse->line_buf[sse->line_len++] = c;
    }
  }
  return got_update;
}

/* ---------------------------------------------------------------------------
 * CRSF Navigation (port of poll_source_remote_keys)
 * --------------------------------------------------------------------------- */

static int source_index(const char *name) {
  if (strcmp(name, "serial") == 0) return 0;
  if (strcmp(name, "joystick") == 0) return 1;
  return -1;
}

static const char *source_name(int idx) {
  return idx == 0 ? "serial" : "joystick";
}

static int source_is_fresh(const source_state_t *s, double now, double timeout) {
  return s->last_update_mono > 0.0 && (now - s->last_update_mono) <= timeout;
}

static void refresh_source_links(app_state_t *app, double now) {
  double stale = app->cfg.tuning.source_stale_s;
  for (int i = 0; i < 2; i++) {
    source_state_t *s = &app->sources[i];
    if (s->last_update_mono <= 0.0)
      s->link_up = 0;
    else
      s->link_up = (now - s->last_update_mono) <= stale;
  }
}

static int choose_active_source(app_state_t *app, double now) {
  int pri = source_index(app->cfg.priority);
  if (pri < 0) pri = 0;
  int other = 1 - pri;

  if (source_is_fresh(&app->sources[pri], now, app->cfg.priority_fallback_s))
    return pri;
  if (source_is_fresh(&app->sources[other], now, app->cfg.priority_fallback_s))
    return other;
  return -1;
}

static void poll_source_keys(app_state_t *app, int src_idx, int menu_visible, double now) {
  source_state_t *s = &app->sources[src_idx];
  const tuning_t *t = &app->cfg.tuning;

  if ((now - s->last_sample_mono) * 1000.0 < t->crsf_sample_interval_ms)
    return;
  s->last_sample_mono = now;

  /* Check staleness */
  if (s->link_up && (now - s->last_update_mono) > t->source_stale_s) {
    s->link_up = 0;
    snprintf(s->nav_direction, sizeof(s->nav_direction), "neutral");
    snprintf(s->nav_candidate, sizeof(s->nav_candidate), "neutral");
    s->select_pressed = 0;
    s->back_pressed = 0;
    s->nav_latched = 0;
    s->combo_active = 0;
    s->combo_started_mono = 0.0;
    s->combo_latched = 0;
  }

  if (!s->link_up) return;

  /* Navigation axis: CH2 (index 1) */
  int axis_y = s->channels[1] - CRSF_CENTER;
  const char *nav_direction = "neutral";
  int nav_key = 0;
  if (axis_y >= t->crsf_axis_deadband) {
    nav_direction = "up";
    nav_key = KEY_UP;
  } else if (axis_y <= -t->crsf_axis_deadband) {
    nav_direction = "down";
    nav_key = KEY_DOWN;
  }

  if (strcmp(nav_direction, s->nav_candidate) != 0) {
    snprintf(s->nav_candidate, sizeof(s->nav_candidate), "%s", nav_direction);
    s->nav_candidate_since = now;
  }

  if (strcmp(nav_direction, "neutral") == 0)
    s->nav_latched = 0;

  snprintf(s->nav_direction, sizeof(s->nav_direction), "%s", nav_direction);

  /* Select: CH1 high */
  int select_active = s->channels[0] >= t->crsf_action_threshold;
  s->back_pressed = 0;

  /* Menu toggle combo: CH1-3 low, CH4 high held */
  int combo_active = (
    s->channels[0] < t->crsf_menu_toggle_ch_low_max &&
    s->channels[1] < t->crsf_menu_toggle_ch_low_max &&
    s->channels[2] < t->crsf_menu_toggle_ch_low_max &&
    s->channels[3] > t->crsf_menu_toggle_ch4_min
  );
  s->combo_active = combo_active;

  LOGV(app, "Keys: ch1=%d<%d ch2=%d<%d ch3=%d<%d ch4=%d>%d combo=%d nav=%s sel=%d menu=%d",
       s->channels[0], t->crsf_menu_toggle_ch_low_max,
       s->channels[1], t->crsf_menu_toggle_ch_low_max,
       s->channels[2], t->crsf_menu_toggle_ch_low_max,
       s->channels[3], t->crsf_menu_toggle_ch4_min,
       combo_active, nav_direction, select_active, menu_visible);
  if (combo_active) {
    if (s->combo_started_mono <= 0.0)
      s->combo_started_mono = now;
    double hold = now - s->combo_started_mono;
    if (hold >= t->crsf_menu_toggle_hold_s && !s->combo_latched) {
      if (app->key_count < MAX_KEY_QUEUE)
        app->key_queue[app->key_count++] = KEY_MENU_TOGGLE;
      s->combo_latched = 1;
    }
  } else {
    s->combo_started_mono = 0.0;
    s->combo_latched = 0;
  }

  /* When menu hidden or combo active, suppress nav/select */
  if (!menu_visible || combo_active) {
    s->select_pressed = select_active;
    /* Reset nav state so debounce starts fresh after combo release */
    if (combo_active) {
      snprintf(s->nav_candidate, sizeof(s->nav_candidate), "neutral");
      s->nav_candidate_since = 0.0;
      s->nav_latched = 0;
    }
    return;
  }

  /* Nav keys with debounce */
  if (nav_key && strcmp(s->nav_candidate, nav_direction) == 0 && !s->nav_latched) {
    double age_ms = (now - s->nav_candidate_since) * 1000.0;
    if (age_ms >= t->crsf_nav_debounce_ms) {
      if (app->key_count < MAX_KEY_QUEUE)
        app->key_queue[app->key_count++] = nav_key;
      s->nav_latched = 1;
    }
  }

  /* Select with debounce */
  double sel_elapsed_ms = (now - s->last_select_mono) * 1000.0;
  if (select_active && !s->select_pressed && sel_elapsed_ms >= t->crsf_select_debounce_ms) {
    if (app->key_count < MAX_KEY_QUEUE)
      app->key_queue[app->key_count++] = KEY_SELECT;
    s->last_select_mono = now;
  }
  s->select_pressed = select_active;
}

/* ---------------------------------------------------------------------------
 * Radio Triggers (port of evaluate_radio_rules)
 * --------------------------------------------------------------------------- */

/* Extract first non-empty line from output buffer */
static void extract_first_line(char *buf, int len, char *out, int out_sz) {
  out[0] = '\0';
  for (int i = 0; i < len; i++) {
    if (buf[i] == '\n' || buf[i] == '\r') {
      buf[i] = '\0';
      if (buf[0]) { snprintf(out, out_sz, "%s", buf); return; }
      int rem = len - i - 1;
      if (rem > 0) memmove(buf, buf + i + 1, rem);
      buf[rem] = '\0';
      len = rem;
      i = -1;
    }
  }
  if (buf[0]) snprintf(out, out_sz, "%s", buf);
}

/* Launch an action asynchronously. Returns 0 on success (child started). */
static int action_launch(app_state_t *app, const menu_action_t *action) {
  if (app->action_pid > 0) {
    LOGW("Action already running (pid %d), ignoring launch of [%s] %s",
         (int)app->action_pid, action->section, action->name);
    return -1;
  }

  int pipefd[2];
  if (pipe(pipefd) < 0) {
    snprintf(app->status, sizeof(app->status), "ERR pipe: %s", strerror(errno));
    app->dirty = 1;
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]); close(pipefd[1]);
    snprintf(app->status, sizeof(app->status), "ERR fork: %s", strerror(errno));
    app->dirty = 1;
    return -1;
  }

  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    const char *sh = (app->cfg.action_shell[0]) ? app->cfg.action_shell : "/bin/sh";
    execl(sh, sh, "-c", action->command, (char *)NULL);
    _exit(127);
  }

  close(pipefd[1]);
  /* Set pipe to non-blocking so main loop can drain it incrementally */
  int flags = fcntl(pipefd[0], F_GETFL, 0);
  if (flags >= 0) fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

  app->action_pid = pid;
  app->action_pipe_fd = pipefd[0];
  app->action_output_len = 0;
  app->action_output[0] = '\0';
  double timeout_s = app->cfg.action_timeout_ms / 1000.0;
  if (timeout_s < 0.1) timeout_s = 0.1;
  app->action_deadline = monotonic_s() + timeout_s;
  snprintf(app->action_label, sizeof(app->action_label), "[%s] %s", action->section, action->name);

  snprintf(app->status, sizeof(app->status), "Running %s ...", app->action_label);
  app->dirty = 1;
  return 0;
}

/* Poll a running async action. Called each iteration of the main loop.
 * When the action finishes or times out, updates app->status and resets state. */
static void action_poll(app_state_t *app) {
  if (app->action_pid <= 0) return;

  /* Try to read available output */
  int space = CMD_OUTPUT_BUF - 1 - app->action_output_len;
  if (app->action_pipe_fd >= 0 && space > 0) {
    ssize_t r = read(app->action_pipe_fd,
                     app->action_output + app->action_output_len,
                     (size_t)space);
    if (r > 0) {
      app->action_output_len += (int)r;
      app->action_output[app->action_output_len] = '\0';
    }
  }

  /* Check if child exited */
  int wstatus;
  pid_t wp = waitpid(app->action_pid, &wstatus, WNOHANG);

  if (wp == 0) {
    /* Still running — check timeout */
    if (monotonic_s() >= app->action_deadline) {
      kill(app->action_pid, SIGKILL);
      waitpid(app->action_pid, &wstatus, 0);
      snprintf(app->status, sizeof(app->status), "Action timeout: %s", app->action_label);
      goto cleanup;
    }
    return; /* still running, not timed out */
  }

  /* Child finished (or waitpid error) */
  {
    char condensed[MAX_OSD_TEXT_CHARS + 1];
    extract_first_line(app->action_output, app->action_output_len, condensed, sizeof(condensed));

    int rc = (wp > 0 && WIFEXITED(wstatus)) ? WEXITSTATUS(wstatus) : -1;
    if (rc == 0) {
      if (condensed[0])
        snprintf(app->status, sizeof(app->status), "OK %s: %s", app->action_label, condensed);
      else
        snprintf(app->status, sizeof(app->status), "OK %s", app->action_label);
    } else {
      if (condensed[0])
        snprintf(app->status, sizeof(app->status), "ERR %d %s: %s", rc, app->action_label, condensed);
      else
        snprintf(app->status, sizeof(app->status), "ERR %d %s", rc, app->action_label);
    }
  }

cleanup:
  if (app->action_pipe_fd >= 0) { close(app->action_pipe_fd); app->action_pipe_fd = -1; }
  app->action_pid = -1;
  app->dirty = 1;
}

static void evaluate_radio_rules(app_state_t *app, int src_idx, double now) {
  source_state_t *s = &app->sources[src_idx];
  radio_rule_state_t *rs = app->radio_states[src_idx];
  const tuning_t *t = &app->cfg.tuning;

  for (int i = 0; i < app->radio_rule_count; i++) {
    radio_rule_t *rule = &app->radio_rules[i];
    radio_rule_state_t *st = &rs[i];

    /* Evaluate all conditions — rule is in-range only when ALL are met */
    int in_range = 1;
    for (int c = 0; c < rule->condition_count; c++) {
      int ci = rule->conds[c].channel_index;
      if (ci >= CRSF_DISPLAY_CHANNELS) { in_range = 0; break; }
      int val = s->channels[ci];
      if (!(val > rule->conds[c].min_value && val < rule->conds[c].max_value))
        { in_range = 0; break; }
    }

    LOGV(app, "Radio[%d] '%s' in_range=%d enter=%.1f latched=%d",
         i, rule->name, in_range,
         st->enter_started > 0 ? (now - st->enter_started) * 1000.0 : 0.0,
         st->latched_in_range);

    if (!in_range) {
      st->enter_started = 0.0;
      if (st->latched_in_range) {
        if (st->outside_started <= 0.0)
          st->outside_started = now;
        double outside_ms = (now - st->outside_started) * 1000.0;
        if (outside_ms >= t->radio_reset_debounce_ms) {
          st->latched_in_range = 0;
          st->outside_started = 0.0;
        }
      } else {
        st->outside_started = 0.0;
      }
      continue;
    }

    st->outside_started = 0.0;
    if (st->latched_in_range) continue;

    if (st->enter_started <= 0.0) {
      st->enter_started = now;
      continue;
    }

    double stable_ms = (now - st->enter_started) * 1000.0;
    if (stable_ms < t->radio_trigger_debounce_ms) continue;

    st->latched_in_range = 1;
    st->enter_started = 0.0;
    st->trigger_count++;

    LOGV(app, "Radio[%d] FIRED: '%s' -> action[%d] cmd='%s'",
         i, rule->name, rule->action_index,
         app->actions[rule->action_index].command);

    if (action_launch(app, &app->actions[rule->action_index]) == 0) {
      snprintf(app->status, sizeof(app->status),
               "Radio trigger #%d '%s' -> %s",
               st->trigger_count, rule->name, app->action_label);
    }
    app->dirty = 1;
  }
}

static void reset_radio_states(radio_rule_state_t *rs, int count) {
  for (int i = 0; i < count; i++) {
    rs[i].enter_started = 0.0;
    rs[i].outside_started = 0.0;
    rs[i].latched_in_range = 0;
  }
}

/* ---------------------------------------------------------------------------
 * Initialization helpers
 * --------------------------------------------------------------------------- */

static void init_sources(app_state_t *app) {
  for (int i = 0; i < 2; i++) {
    source_state_t *s = &app->sources[i];
    memset(s, 0, sizeof(*s));
    for (int c = 0; c < CRSF_DISPLAY_CHANNELS; c++)
      s->channels[c] = CRSF_CENTER;
    snprintf(s->nav_direction, sizeof(s->nav_direction), "neutral");
    snprintf(s->nav_candidate, sizeof(s->nav_candidate), "neutral");
  }
  memset(app->radio_states, 0, sizeof(app->radio_states));
}

static void init_asset_state(app_state_t *app) {
  for (int i = 0; i < ASSET_COUNT; i++) {
    app->asset_enabled[i] = -1; /* unknown */
    app->pending_asset_updates[i] = -1; /* no update */
  }

  /* Apply initial_off */
  for (int i = 0; i < app->cfg.initial_off_count; i++) {
    int id = app->cfg.initial_off[i];
    if (id >= 0 && id < ASSET_COUNT) {
      app->asset_enabled[id] = 0;
      app->pending_asset_updates[id] = 0;
    }
  }

  /* Menu asset starts hidden */
  int mid = app->cfg.menu_asset_id;
  if (mid >= 0 && mid < ASSET_COUNT) {
    app->asset_enabled[mid] = 0;
    app->pending_asset_updates[mid] = 0;
  }

  /* Splashscreen: set initial phase */
  splashscreen_t *sp = &app->cfg.splashscreen;
  if (sp->enabled && sp->asset_id >= 0 && sp->asset_id < ASSET_COUNT &&
      sp->asset_id == app->cfg.menu_asset_id) {
    LOGW("Splashscreen asset_id %d collides with menu_asset_id; splash disabled", sp->asset_id);
    sp->enabled = 0;
  }
  if (sp->enabled && sp->asset_id >= 0 && sp->asset_id < ASSET_COUNT &&
      app->splash_phase == SPLASH_IDLE) {
    if (sp->delay_s > 0.0) {
      /* Delay phase: don't enable asset yet */
      app->splash_phase = SPLASH_DELAY;
    } else {
      /* No delay: enable asset immediately */
      app->asset_enabled[sp->asset_id] = 1;
      app->pending_asset_updates[sp->asset_id] = 1;
      app->splash_phase = SPLASH_SHOWING;
    }
  }
}

static void set_asset_enabled(app_state_t *app, int id, int enabled) {
  if (id < 0 || id >= ASSET_COUNT) return;
  LOGV(app, "Asset %d: %d -> %d", id, app->asset_enabled[id], enabled);
  app->asset_enabled[id] = enabled;
  app->pending_asset_updates[id] = enabled;
}

static void setup_destinations(app_state_t *app) {
  app->dest_count = 0;

  /* Primary destination */
  if (app->dest_count < MAX_DESTINATIONS) {
    udp_dest_t *d = &app->destinations[app->dest_count];
    snprintf(d->host, sizeof(d->host), "%s", app->cfg.host);
    d->port = app->cfg.port;
    if (resolve_destination(d) == 0)
      app->dest_count++;
  }

  /* Extra destinations */
  for (int i = 0; i < app->cfg.extra_dest_count && app->dest_count < MAX_DESTINATIONS; i++) {
    const char *spec = app->cfg.extra_dest_specs[i];
    if (!spec[0]) continue;

    /* Dedup check */
    udp_dest_t tmp;
    if (parse_dest_spec(spec, &tmp) < 0) {
      LOGW("Invalid extra destination: %s", spec);
      continue;
    }
    int dup = 0;
    for (int j = 0; j < app->dest_count; j++) {
      if (strcmp(app->destinations[j].host, tmp.host) == 0 &&
          app->destinations[j].port == tmp.port) {
        dup = 1; break;
      }
    }
    if (!dup) {
      app->destinations[app->dest_count++] = tmp;
    }
  }
}

static void resolve_action_shell(config_t *cfg) {
  if (cfg->action_shell[0]) return;
  const char *env_shell = getenv("SHELL");
  if (env_shell && env_shell[0])
    snprintf(cfg->action_shell, sizeof(cfg->action_shell), "%s", env_shell);
  else
    snprintf(cfg->action_shell, sizeof(cfg->action_shell), "/bin/sh");
}

static void resolve_actions_ini(app_state_t *app) {
  if (app->cfg.actions_ini[0]) return;

  /* Try menu.ini next to config file */
  char dir[PATH_LEN];
  snprintf(dir, sizeof(dir), "%s", app->config_path);
  char *slash = strrchr(dir, '/');
  if (slash) {
    slash[1] = '\0';
  } else {
    /* Relative path with no directory component — use current dir */
    snprintf(dir, sizeof(dir), "./");
  }
  char candidate[PATH_LEN];
  snprintf(candidate, sizeof(candidate), "%s%s", dir, "menu.ini");
  if (access(candidate, R_OK) == 0) {
    snprintf(app->cfg.actions_ini, sizeof(app->cfg.actions_ini), "%s", candidate);
  }
}

static void resolve_webui_html(app_state_t *app) {
  if (!app->cfg.webui_html[0]) {
    snprintf(app->cfg.webui_html, sizeof(app->cfg.webui_html), "waybeam_hub_c.html");
  }
  if (app->cfg.webui_html[0] == '/') return;

  char dir[PATH_LEN];
  snprintf(dir, sizeof(dir), "%s", app->config_path);
  char *slash = strrchr(dir, '/');
  if (slash) slash[1] = '\0';
  else snprintf(dir, sizeof(dir), "./");

  char candidate[PATH_LEN];
  snprintf(candidate, sizeof(candidate), "%s%s", dir, app->cfg.webui_html);
  if (access(candidate, R_OK) == 0) {
    snprintf(app->cfg.webui_html, sizeof(app->cfg.webui_html), "%s", candidate);
  }
}

/* ---------------------------------------------------------------------------
 * Reload config + menus (SIGHUP)
 * --------------------------------------------------------------------------- */

static int reload_config(app_state_t *app) {
  LOGI("Reloading config from %s", app->config_path);

  config_t new_cfg;
  if (parse_config_json(app->config_path, &new_cfg) < 0) {
    LOGE("Config reload failed, keeping previous config");
    return -1;
  }

  resolve_action_shell(&new_cfg);
  app->cfg = new_cfg;

  resolve_actions_ini(app);
  resolve_webui_html(app);
  if (load_actions_ini(app->cfg.actions_ini, app) < 0) {
    LOGE("Actions INI reload failed");
    return -1;
  }

  build_all_menus(app);
  setup_destinations(app);

  /* Reset menu position */
  app->current_section[0] = '\0';
  app->selected = 0;

  /* Re-init radio states */
  memset(app->radio_states, 0, sizeof(app->radio_states));

  /* Reset splash state machine (config may have changed splash settings) */
  app->splash_phase = SPLASH_DONE;
  app->splash_next_event = 0.0;
  app->splash_pending_opacity = -1;

  snprintf(app->status, sizeof(app->status), "Config reloaded");
  app->dirty = 1;
  return 0;
}

/* ---------------------------------------------------------------------------
 * Main Event Loop
 * --------------------------------------------------------------------------- */

static int run_controller(app_state_t *app) {
  double now;

  /* Build menus */
  build_all_menus(app);
  init_sources(app);
  init_asset_state(app);
  setup_destinations(app);

  /* UDP socket */
  app->udp_fd = setup_udp_socket();
  if (app->udp_fd < 0) return 1;

  /* SSE client setup */
  sse_parse_url(app->cfg.sse_url, app->sse.host, &app->sse.port, app->sse.path);
  app->sse.fd = -1;
  app->sse.state = SSE_DISCONNECTED;
  app->sse.last_connect_attempt = 0.0;

  webui_init(app);

  app->active_source = -1;
  app->splash_pending_opacity = -1;
  app->splash_last_opacity = -1;
  app->action_pid = -1;
  app->action_pipe_fd = -1;
  app->dirty = 1;
  app->last_send_mono = 0.0;
  app->last_menu_activity_mono = monotonic_s();
  app->key_count = 0;
  app->current_section[0] = '\0';
  app->selected = 0;
  app->status[0] = '\0';
  app->last_logged_status[0] = '\0';

  snprintf(app->status, sizeof(app->status), "Ready (SSE %s, WebUI http://%s:%d)",
           app->cfg.sse_url, app->cfg.webui_host, app->cfg.webui_port);

  LOGI("waybeam_hub started, SSE=%s, destinations=%d", app->cfg.sse_url, app->dest_count);
  for (int i = 0; i < app->dest_count; i++)
    LOGI("  -> %s:%d", app->destinations[i].host, app->destinations[i].port);

  {
    splashscreen_t *sp = &app->cfg.splashscreen;
    if (sp->enabled)
      LOGI("Splashscreen: asset_id=%d, duration=%.1fs, opacity=%d, delay=%.1fs, fadeout=%.1fs",
           sp->asset_id, sp->duration_s, sp->opacity, sp->delay_s, sp->fadeout_s);
    else
      LOGV(app, "Splashscreen: disabled");
  }

  while (!g_stop) {
    now = monotonic_s();

    /* Handle SIGHUP reload */
    if (g_reload) {
      g_reload = 0;
      if (reload_config(app) == 0) {
        resolve_webui_html(app);
        webui_shutdown(app);
        webui_init(app);
      }
      sse_parse_url(app->cfg.sse_url, app->sse.host, &app->sse.port, app->sse.path);
      sse_disconnect(&app->sse);
    }

    /* SSE connect/reconnect */
    if (app->sse.state == SSE_DISCONNECTED) {
      if (now - app->sse.last_connect_attempt >= 1.0) {
        app->sse.last_connect_attempt = now;
        if (sse_try_connect(&app->sse) < 0) {
          /* Will retry next loop */
        }
      }
    }

    /* Poll SSE fd */
    int got_channel_update = 0;
    if (app->sse.fd >= 0) {
      struct pollfd pfd = { .fd = app->sse.fd, .events = POLLIN };
      int rc = poll(&pfd, 1, 50); /* 50ms timeout */
      if (rc > 0 && (pfd.revents & POLLIN)) {
        int src = sse_process(&app->sse, app);
        if (src < 0) {
          LOGW("SSE disconnected, will reconnect");
          sse_disconnect(&app->sse);
          snprintf(app->status, sizeof(app->status), "SSE disconnected, reconnecting");
          app->dirty = 1;
        } else if (src > 0) {
          got_channel_update = 1;
        }
      } else if (rc > 0 && (pfd.revents & (POLLERR | POLLHUP))) {
        sse_disconnect(&app->sse);
        snprintf(app->status, sizeof(app->status), "SSE connection lost, reconnecting");
        app->dirty = 1;
      } else if (rc < 0 && errno != EINTR) {
        sse_disconnect(&app->sse);
      }
    } else {
      /* Not connected; just sleep a bit */
      poll(NULL, 0, 50);
    }

    now = monotonic_s();

    webui_poll(app);

    /* Poll async action */
    action_poll(app);

    /* Refresh source links */
    refresh_source_links(app, now);

    /* Reset radio states for stale sources */
    for (int i = 0; i < 2; i++) {
      if (!app->sources[i].link_up)
        reset_radio_states(app->radio_states[i], app->radio_rule_count);
    }

    /* Choose active source */
    int new_active = choose_active_source(app, now);
    if (new_active != app->active_source) {
      app->active_source = new_active;
      if (new_active < 0)
        snprintf(app->status, sizeof(app->status),
                 "No active SSE input; waiting for %s or fallback", app->cfg.priority);
      else
        snprintf(app->status, sizeof(app->status),
                 "Active input: %s (priority=%s)", source_name(new_active), app->cfg.priority);
      app->dirty = 1;
    }

    /* Get current entries */
    menu_entry_t *entries;
    int entry_count;
    get_current_entries(app, &entries, &entry_count);
    app->selected = clamp_i(app->selected, 0, entry_count > 0 ? entry_count - 1 : 0);

    /* Poll keys and radio rules only when new channel data arrived */
    int menu_visible = (app->cfg.menu_asset_id >= 0 &&
                        app->cfg.menu_asset_id < ASSET_COUNT &&
                        app->asset_enabled[app->cfg.menu_asset_id] == 1);

    if (got_channel_update && app->active_source >= 0) {
      poll_source_keys(app, app->active_source, menu_visible, now);

      /* Evaluate radio rules */
      if (app->radio_rule_count > 0)
        evaluate_radio_rules(app, app->active_source, now);
    }

    /* Menu inactivity timeout */
    now = monotonic_s();
    if (menu_visible && (now - app->last_menu_activity_mono) >= app->cfg.tuning.menu_inactivity_timeout_s) {
      set_asset_enabled(app, app->cfg.menu_asset_id, 0);
      app->current_section[0] = '\0';
      app->selected = 0;
      snprintf(app->status, sizeof(app->status),
               "Menu auto-hidden after %.0fs inactivity", app->cfg.tuning.menu_inactivity_timeout_s);
      app->dirty = 1;
      app->last_menu_activity_mono = now;
    }

    /* Splashscreen state machine */
    now = monotonic_s();
    if (app->splash_next_event > 0.0 && now >= app->splash_next_event) {
      splashscreen_t *sp = &app->cfg.splashscreen;
      switch (app->splash_phase) {
      case SPLASH_DELAY:
        /* Delay expired — enable asset, transition to SHOWING */
        LOGI("Splashscreen: delay elapsed, enabling asset %d", sp->asset_id);
        set_asset_enabled(app, sp->asset_id, 1);
        app->splash_phase = SPLASH_SHOWING;
        app->splash_next_event = now + sp->duration_s;
        app->dirty = 1;
        break;
      case SPLASH_SHOWING:
        /* Duration expired — fade or disable */
        if (sp->fadeout_s > 0.0) {
          LOGI("Splashscreen: starting %.1fs fade on asset %d", sp->fadeout_s, sp->asset_id);
          app->splash_phase = SPLASH_FADING;
          app->splash_fade_end = now + sp->fadeout_s;
          /* Max 8 fade steps, each sends full enabled+opacity update */
          int start_opa = (sp->opacity >= 0) ? sp->opacity : 100;
          double tick_s = sp->fadeout_s / 8.0;
          if (tick_s < 0.1) tick_s = 0.1;
          app->splash_fade_tick_s = tick_s;
          app->splash_next_event = now + tick_s;
          app->splash_last_opacity = start_opa;
        } else {
          LOGI("Splashscreen: hiding asset %d after %.1fs", sp->asset_id, sp->duration_s);
          set_asset_enabled(app, sp->asset_id, 0);
          app->splash_phase = SPLASH_DONE;
          app->splash_next_event = 0.0;
          app->dirty = 1;
        }
        break;
      case SPLASH_FADING: {
        /* Interpolate opacity */
        double remaining = app->splash_fade_end - now;
        if (remaining <= 0.0) {
          LOGI("Splashscreen: fade complete, hiding asset %d", sp->asset_id);
          set_asset_enabled(app, sp->asset_id, 0);
          app->splash_phase = SPLASH_DONE;
          app->splash_next_event = 0.0;
          app->dirty = 1;
        } else {
          int start_opa = (sp->opacity >= 0) ? sp->opacity : 100;
          int cur_opa = (int)(start_opa * (remaining / sp->fadeout_s));
          if (cur_opa < 0) cur_opa = 0;
          if (cur_opa != app->splash_last_opacity) {
            app->splash_pending_opacity = cur_opa;
            app->splash_last_opacity = cur_opa;
            /* Re-set pending enable so payload includes enabled:true + opacity */
            app->pending_asset_updates[sp->asset_id] = 1;
            app->dirty = 1;
          }
          app->splash_next_event = now + app->splash_fade_tick_s;
        }
        break;
      }
      default:
        app->splash_next_event = 0.0;
        break;
      }
    }

    /* Send OSD payload only when state actually changed */
    now = monotonic_s();
    if (app->dirty) {
      /* Re-get entries after possible state changes */
      get_current_entries(app, &entries, &entry_count);
      app->selected = clamp_i(app->selected, 0, entry_count > 0 ? entry_count - 1 : 0);

      char texts[3][MAX_OSD_TEXT_CHARS + 1];
      build_three_slot_texts(app, entries, entry_count, app->selected, texts);

      char payload[PAYLOAD_BUF];
      int plen = build_osd_payload(app, texts, payload, sizeof(payload));
      LOGV(app, "OSD send (%d bytes): %.*s", plen, plen > 200 ? 200 : plen, payload);
      send_all_destinations(app, payload, plen);

      app->last_send_mono = now;

      /* Arm splashscreen timer after first send */
      if (app->splash_next_event <= 0.0 && app->splash_phase != SPLASH_DONE &&
          app->splash_phase != SPLASH_IDLE) {
        splashscreen_t *sp = &app->cfg.splashscreen;
        if (app->splash_phase == SPLASH_DELAY) {
          app->splash_next_event = now + sp->delay_s;
          LOGI("Splashscreen: delaying %.1fs before enabling asset %d", sp->delay_s, sp->asset_id);
        } else if (app->splash_phase == SPLASH_SHOWING) {
          app->splash_next_event = now + sp->duration_s;
          LOGI("Splashscreen: enabled asset %d for %.1fs", sp->asset_id, sp->duration_s);
        }
      }

      /* Clear pending after successful send */
      for (int i = 0; i < ASSET_COUNT; i++)
        app->pending_asset_updates[i] = -1;
      app->splash_pending_opacity = -1;
      app->dirty = 0;
    }

    /* Log status changes */
    if (strcmp(app->status, app->last_logged_status) != 0) {
      LOGI("%s", app->status);
      snprintf(app->last_logged_status, sizeof(app->last_logged_status), "%s", app->status);
    }

    /* Process key queue */
    if (app->key_count <= 0) continue;

    int key = app->key_queue[0];
    memmove(app->key_queue, app->key_queue + 1, (app->key_count - 1) * sizeof(int));
    app->key_count--;

    if (key == KEY_MENU_TOGGLE) {
      int mid = app->cfg.menu_asset_id;
      int current = app->asset_enabled[mid];
      int next = (current <= 0) ? 1 : 0;
      set_asset_enabled(app, mid, next);
      app->current_section[0] = '\0';
      app->selected = 0;
      snprintf(app->status, sizeof(app->status),
               next ? "Menu overlay visible" : "Menu overlay hidden");
      if (next) app->last_menu_activity_mono = monotonic_s();
      app->dirty = 1;
      continue;
    }

    if (key == KEY_UP) {
      if (app->selected > 0) app->selected--;
      app->last_menu_activity_mono = monotonic_s();
      app->dirty = 1;
      continue;
    }

    if (key == KEY_DOWN) {
      get_current_entries(app, &entries, &entry_count);
      if (app->selected + 1 < entry_count) app->selected++;
      app->last_menu_activity_mono = monotonic_s();
      app->dirty = 1;
      continue;
    }

    if (key == KEY_SELECT) {
      get_current_entries(app, &entries, &entry_count);
      if (app->selected >= entry_count) continue;
      menu_entry_t *e = &entries[app->selected];

      if (e->kind == ENTRY_EXIT) {
        set_asset_enabled(app, app->cfg.menu_asset_id, 0);
        app->current_section[0] = '\0';
        app->selected = 0;
        snprintf(app->status, sizeof(app->status), "Menu overlay hidden");
        app->last_menu_activity_mono = monotonic_s();
        app->dirty = 1;
        continue;
      }

      if (e->kind == ENTRY_SECTION) {
        snprintf(app->current_section, sizeof(app->current_section), "%s", e->section);
        app->selected = 0;
        snprintf(app->status, sizeof(app->status), "Opened [%s]", app->current_section);
        app->last_menu_activity_mono = monotonic_s();
        app->dirty = 1;
        continue;
      }

      if (e->kind == ENTRY_RETURN) {
        if (app->current_section[0]) {
          /* Find previous section in top entries to restore selection */
          char prev[SECTION_NAME_LEN];
          snprintf(prev, sizeof(prev), "%s", app->current_section);
          app->current_section[0] = '\0';
          app->selected = 0;
          for (int i = 0; i < app->top_entry_count; i++) {
            if (app->top_entries[i].kind == ENTRY_SECTION &&
                strcmp(app->top_entries[i].section, prev) == 0) {
              app->selected = i;
              break;
            }
          }
          snprintf(app->status, sizeof(app->status), "Returned to ROOT");
          app->last_menu_activity_mono = monotonic_s();
          app->dirty = 1;
        }
        continue;
      }

      if (e->kind == ENTRY_ASSET && e->asset_id >= 0) {
        int current = app->asset_enabled[e->asset_id];
        int next = (current <= 0) ? 1 : 0;
        set_asset_enabled(app, e->asset_id, next);
        snprintf(app->status, sizeof(app->status),
                 "Asset %d %s", e->asset_id, next ? "ON" : "OFF");
        app->last_menu_activity_mono = monotonic_s();
        app->dirty = 1;
        continue;
      }

      if (e->kind == ENTRY_ACTION && e->action_index >= 0 && e->action_index < app->action_count) {
        action_launch(app, &app->actions[e->action_index]);
        app->last_menu_activity_mono = monotonic_s();
        continue;
      }
    }
  }

  /* Cleanup */
  LOGI("Shutting down");
  if (app->action_pid > 0) {
    kill(app->action_pid, SIGKILL);
    waitpid(app->action_pid, NULL, 0);
    if (app->action_pipe_fd >= 0) close(app->action_pipe_fd);
    app->action_pid = -1;
  }
  send_clear_payload(app);
  sse_disconnect(&app->sse);
  webui_shutdown(app);
  if (app->udp_fd >= 0) close(app->udp_fd);
  return 0;
}

/* ---------------------------------------------------------------------------
 * main()
 * --------------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
  const char *config_path = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      config_path = argv[++i];
    } else if (strncmp(argv[i], "--config=", 9) == 0) {
      config_path = argv[i] + 9;
    } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
      /* Handled after config load — just mark for now */
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      fprintf(stderr, "Usage: %s [--config <path>] [-v|--verbose]\n", argv[0]);
      fprintf(stderr, "  SSE-driven OSD menu controller (C port of waybeam_hub.py)\n");
      fprintf(stderr, "  Default config: config.json next to binary\n");
      fprintf(stderr, "  -v, --verbose  Enable verbose logging (overrides config)\n");
      return 0;
    }
  }

  /* Default config path: next to binary */
  char default_config[PATH_LEN];
  snprintf(default_config, sizeof(default_config), "config.json");
  if (!config_path) {
    /* Try to find config.json next to argv[0] */
    char *slash = strrchr(argv[0], '/');
    if (slash) {
      int dirlen = (int)(slash - argv[0]) + 1;
      if (dirlen < (int)sizeof(default_config) - 12) {
        memcpy(default_config, argv[0], dirlen);
        snprintf(default_config + dirlen, sizeof(default_config) - dirlen, "config.json");
      }
    }
    config_path = default_config;
  }

  static app_state_t app;
  memset(&app, 0, sizeof(app));
  snprintf(app.config_path, sizeof(app.config_path), "%s", config_path);

  if (parse_config_json(config_path, &app.cfg) < 0) {
    LOGE("Failed to load config: %s", config_path);
    return 1;
  }

  /* CLI -v overrides config verbose */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
      app.cfg.verbose = 1;
  }

  resolve_action_shell(&app.cfg);
  resolve_actions_ini(&app);
  resolve_webui_html(&app);

  if (load_actions_ini(app.cfg.actions_ini, &app) < 0) {
    LOGE("Failed to load actions INI");
    return 1;
  }

  LOGI("Config loaded: host=%s port=%d sse_url=%s priority=%s webui=%s:%d",
       app.cfg.host, app.cfg.port, app.cfg.sse_url, app.cfg.priority,
       app.cfg.webui_host, app.cfg.webui_port);
  LOGI("Actions: %d actions in %d sections, %d radio rules",
       app.action_count, app.section_count, app.radio_rule_count);
  for (int i = 0; i < app.radio_rule_count; i++) {
    radio_rule_t *r = &app.radio_rules[i];
    LOGV(&app, "  Radio[%d]: '%s' (%d cond%s) -> action[%d] '%s'",
         i, r->name, r->condition_count, r->condition_count > 1 ? "s" : "",
         r->action_index, app.actions[r->action_index].command);
  }

  /* Install signal handlers */
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sig_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);

  return run_controller(&app);
}
