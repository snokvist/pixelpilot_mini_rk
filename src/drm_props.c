#include "drm_props.h"
#include "logging.h"

#include <string.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

int drm_get_prop_id(int fd, uint32_t obj_id, uint32_t obj_type, const char *name, uint32_t *out) {
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) {
        return -1;
    }
    int found = 0;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (!p) {
            continue;
        }
        if (!strcmp(p->name, name)) {
            *out = p->prop_id;
            found = 1;
            drmModeFreeProperty(p);
            break;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return found ? 0 : -1;
}

int drm_get_prop_id_and_range_ci(int fd, uint32_t obj_id, uint32_t obj_type,
                                 const char *name1, uint32_t *out_id,
                                 uint64_t *out_min, uint64_t *out_max,
                                 const char *alt_name2) {
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) {
        return -1;
    }
    int found = 0;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (!p) {
            continue;
        }
        if (!strcmp(p->name, name1) || (alt_name2 && !strcmp(p->name, alt_name2))) {
            *out_id = p->prop_id;
            found = 1;
            if ((p->flags & DRM_MODE_PROP_RANGE) && out_min && out_max) {
                *out_min = p->values[0];
                *out_max = p->values[1];
            }
            drmModeFreeProperty(p);
            break;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return found ? 0 : -1;
}

void drm_debug_list_props(int fd, uint32_t obj_id, uint32_t obj_type, const char *tag) {
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) {
        LOGV("%s: no props", tag);
        return;
    }
    fprintf(stderr, "[DBG] %s props (%u):", tag, props->count_props);
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (!p) {
            continue;
        }
        fprintf(stderr, " %s", p->name);
        drmModeFreeProperty(p);
    }
    fprintf(stderr, "\n");
    drmModeFreeObjectProperties(props);
}
