#include "osd_layout.h"

#include <string.h>

static void placement_defaults(OsdPlacement *p, OSDWidgetPosition anchor) {
    p->anchor = anchor;
    p->offset_x = 0;
    p->offset_y = 0;
}

static void text_defaults(OsdTextConfig *cfg) {
    cfg->line_count = 0;
    cfg->padding = 6;
    cfg->fg = 0xB0FFFFFFu;
    cfg->bg = 0x40202020u;
    cfg->border = 0x60FFFFFFu;
}

static void line_defaults(OsdLineConfig *cfg) {
    cfg->width = 360;
    cfg->height = 80;
    cfg->sample_stride_px = 4;
    cfg->metric[0] = '\0';
    cfg->label[0] = '\0';
    cfg->show_info_box = 1;
    cfg->fg = 0xFFFFFFFFu;
    cfg->grid = 0x20FFFFFFu;
    cfg->bg = 0x20000000u;
}

void osd_layout_defaults(OsdLayout *layout) {
    if (!layout) {
        return;
    }
    memset(layout, 0, sizeof(*layout));

    layout->element_count = 2;

    OsdElementConfig *text = &layout->elements[0];
    text->type = OSD_WIDGET_TEXT;
    strncpy(text->name, "stats", sizeof(text->name) - 1);
    placement_defaults(&text->placement, OSD_POS_TOP_LEFT);
    text_defaults(&text->data.text);

    const char *default_lines[] = {
        "HDMI {display.mode} plane={drm.video_plane_id}",
        "UDP:{udp.port} PTv={udp.vid_pt} PTa={udp.aud_pt} lat={pipeline.latency_ms}ms src={udp.source_ip}:{udp.source_port}",
        "Pipeline: {pipeline.state} restarts={pipeline.restart_count}{pipeline.audio_suffix}",
        "RTP vpkts={udp.video_packets} net-loss={udp.lost_packets} reo={udp.reordered_packets} dup={udp.duplicate_packets} idr={udp.idr_requests} jitter={udp.jitter.latest_ms}/{udp.jitter.avg_ms}ms",
        "IDR backoff={udp.idr.backoff_ms}ms loss={udp.idr.loss_duration_ms}ms since={udp.idr.since_last_request_ms}ms cool={udp.idr.cooldown_ms}ms last={udp.idr.last_loss_ms}ms",
        "Frames={udp.frames.count} incomplete={udp.frames.incomplete} last={udp.frames.last_bytes}KB avg={udp.frames.avg_bytes}KB seq={udp.expected_sequence}"
    };
    for (size_t i = 0; i < sizeof(default_lines) / sizeof(default_lines[0]) && i < OSD_MAX_TEXT_LINES; ++i) {
        strncpy(text->data.text.lines[text->data.text.line_count].raw, default_lines[i],
                sizeof(text->data.text.lines[0].raw) - 1);
        text->data.text.lines[text->data.text.line_count].raw[sizeof(text->data.text.lines[0].raw) - 1] = '\0';
        text->data.text.line_count++;
    }

    OsdElementConfig *plot = &layout->elements[1];
    plot->type = OSD_WIDGET_LINE;
    strncpy(plot->name, "jitter", sizeof(plot->name) - 1);
    placement_defaults(&plot->placement, OSD_POS_BOTTOM_LEFT);
    line_defaults(&plot->data.line);
    strncpy(plot->data.line.metric, "udp.jitter.latest_ms", sizeof(plot->data.line.metric) - 1);
    strncpy(plot->data.line.label, "Jitter (ms)", sizeof(plot->data.line.label) - 1);
}
