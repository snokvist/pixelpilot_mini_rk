#include "drm_fb.h"
#include "logging.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#if defined(__has_include)
#if __has_include(<libdrm/drm.h>)
#include <libdrm/drm.h>
#else
#include <drm/drm.h>
#endif
#else
#include <libdrm/drm.h>
#endif
#if defined(__has_include)
#if __has_include(<libdrm/drm_fourcc.h>)
#include <libdrm/drm_fourcc.h>
#else
#include <drm/drm_fourcc.h>
#endif
#else
#include <drm/drm_fourcc.h>
#endif
#include <xf86drm.h>
#include <xf86drmMode.h>

int create_argb_fb(int fd, int w, int h, uint32_t argb_fill, struct DumbFB *out) {
    struct drm_mode_create_dumb creq = {0};
    creq.width = w;
    creq.height = h;
    creq.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        return -1;
    }

    uint32_t handles[4] = {creq.handle, 0, 0, 0};
    uint32_t pitches[4] = {creq.pitch, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    uint32_t fb_id = 0;

    if (drmModeAddFB2(fd, w, h, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &fb_id, 0) != 0) {
        struct drm_mode_destroy_dumb dreq = {.handle = creq.handle};
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }

    struct drm_mode_map_dumb mreq = {.handle = creq.handle};
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        drmModeRmFB(fd, fb_id);
        struct drm_mode_destroy_dumb dreq = {.handle = creq.handle};
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }

    void *map = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (map == MAP_FAILED) {
        drmModeRmFB(fd, fb_id);
        struct drm_mode_destroy_dumb dreq = {.handle = creq.handle};
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }

    uint32_t *px = (uint32_t *)map;
    size_t count = creq.size / 4;
    for (size_t i = 0; i < count; ++i) {
        px[i] = argb_fill;
    }

    out->fb_id = fb_id;
    out->handle = creq.handle;
    out->pitch = creq.pitch;
    out->size = creq.size;
    out->map = map;
    out->w = w;
    out->h = h;
    return 0;
}

void destroy_dumb_fb(int fd, struct DumbFB *fb) {
    if (!fb) {
        return;
    }
    if (fb->map && fb->map != MAP_FAILED) {
        munmap(fb->map, fb->size);
    }
    if (fb->fb_id) {
        drmModeRmFB(fd, fb->fb_id);
    }
    if (fb->handle) {
        struct drm_mode_destroy_dumb dreq = {.handle = fb->handle};
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }
    memset(fb, 0, sizeof(*fb));
}
