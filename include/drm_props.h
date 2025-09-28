#ifndef DRM_PROPS_H
#define DRM_PROPS_H

#include <stdint.h>

int drm_get_prop_id(int fd, uint32_t obj_id, uint32_t obj_type, const char *name, uint32_t *out);
int drm_get_prop_id_and_range_ci(int fd, uint32_t obj_id, uint32_t obj_type,
                                 const char *name1, uint32_t *out_id,
                                 uint64_t *out_min, uint64_t *out_max,
                                 const char *alt_name2);
void drm_debug_list_props(int fd, uint32_t obj_id, uint32_t obj_type, const char *tag);

#endif // DRM_PROPS_H
