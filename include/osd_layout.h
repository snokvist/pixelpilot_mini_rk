#ifndef OSD_LAYOUT_H
#define OSD_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OSD_WIDGET_TEXT = 0,
    OSD_WIDGET_LINE,
    OSD_WIDGET_BAR
} OsdElementType;

typedef enum {
    OSD_POS_TOP_LEFT = 0,
    OSD_POS_TOP_MID,
    OSD_POS_TOP_RIGHT,
    OSD_POS_MID_LEFT,
    OSD_POS_MID,
    OSD_POS_MID_RIGHT,
    OSD_POS_BOTTOM_LEFT,
    OSD_POS_BOTTOM_MID,
    OSD_POS_BOTTOM_RIGHT
} OSDWidgetPosition;

#define OSD_MAX_ELEMENTS 8
#define OSD_MAX_TEXT_LINES 24
#define OSD_TEXT_MAX_LINE_CHARS 192

typedef struct {
    OSDWidgetPosition anchor;
    int offset_x;
    int offset_y;
} OsdPlacement;

typedef struct {
    char raw[OSD_TEXT_MAX_LINE_CHARS];
} OsdTextTemplate;

typedef struct {
    int line_count;
    OsdTextTemplate lines[OSD_MAX_TEXT_LINES];
    int padding;
    uint32_t fg;
    uint32_t bg;
    uint32_t border;
} OsdTextConfig;

typedef struct {
    int width;
    int height;
    int sample_stride_px;
    char metric[64];
    char label[32];
    int show_info_box;
    uint32_t fg;
    uint32_t grid;
    uint32_t bg;
} OsdLineConfig;

typedef struct {
    int width;
    int height;
    int sample_stride_px;
    int bar_width_px;
    char metric[64];
    char label[32];
    int show_info_box;
    uint32_t fg;
    uint32_t grid;
    uint32_t bg;
} OsdBarConfig;

typedef struct {
    OsdElementType type;
    char name[48];
    OsdPlacement placement;
    union {
        OsdTextConfig text;
        OsdLineConfig line;
        OsdBarConfig bar;
    } data;
} OsdElementConfig;

typedef struct {
    int element_count;
    OsdElementConfig elements[OSD_MAX_ELEMENTS];
} OsdLayout;

void osd_layout_defaults(OsdLayout *layout);

#ifdef __cplusplus
}
#endif

#endif // OSD_LAYOUT_H
