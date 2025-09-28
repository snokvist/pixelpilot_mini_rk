#ifndef DRM_FB_H
#define DRM_FB_H

#include <stdint.h>

struct DumbFB {
    uint32_t fb_id;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
    void *map;
    int w;
    int h;
};

int create_argb_fb(int fd, int w, int h, uint32_t argb_fill, struct DumbFB *out);
void destroy_dumb_fb(int fd, struct DumbFB *fb);

#endif // DRM_FB_H
