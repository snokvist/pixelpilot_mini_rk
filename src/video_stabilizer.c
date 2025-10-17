#include "video_stabilizer.h"

#include "logging.h"

#include <errno.h>
#include <glib.h>
#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if PIXELPILOT_HAVE_RGA
#include <rga/RgaApi.h>
#include <rga/rga.h>
#endif

struct VideoStabilizer {
    StabilizerConfig config;
    gboolean initialized;
    gboolean configured;
    gboolean available;
    uint32_t width;
    uint32_t height;
    uint32_t hor_stride;
    uint32_t ver_stride;
};

VideoStabilizer *video_stabilizer_new(void) {
    return g_new0(VideoStabilizer, 1);
}

void video_stabilizer_free(VideoStabilizer *stabilizer) {
    if (stabilizer == NULL) {
        return;
    }
    video_stabilizer_shutdown(stabilizer);
    g_free(stabilizer);
}

static void stabilizer_copy_defaults(StabilizerConfig *cfg) {
    if (cfg == NULL) {
        return;
    }
    if (cfg->strength <= 0.0f) {
        cfg->strength = 1.0f;
    }
    if (cfg->max_translation_px <= 0.0f) {
        cfg->max_translation_px = 32.0f;
    }
    if (cfg->max_rotation_deg <= 0.0f) {
        cfg->max_rotation_deg = 5.0f;
    }
}

int video_stabilizer_init(VideoStabilizer *stabilizer, const StabilizerConfig *config) {
    if (stabilizer == NULL) {
        return -EINVAL;
    }
    memset(stabilizer, 0, sizeof(*stabilizer));

    StabilizerConfig cfg = {0};
    if (config) {
        cfg = *config;
    }
    stabilizer_copy_defaults(&cfg);

    stabilizer->config = cfg;
    stabilizer->initialized = TRUE;
#if PIXELPILOT_HAVE_RGA
    stabilizer->available = cfg.enable ? TRUE : FALSE;
#else
    if (cfg.enable) {
        LOGW("Video stabilizer requested but librga support is not available at build time");
    }
    stabilizer->available = FALSE;
#endif
    return 0;
}

void video_stabilizer_shutdown(VideoStabilizer *stabilizer) {
    if (stabilizer == NULL) {
        return;
    }
    stabilizer->initialized = FALSE;
    stabilizer->configured = FALSE;
    stabilizer->available = FALSE;
    stabilizer->width = 0;
    stabilizer->height = 0;
    stabilizer->hor_stride = 0;
    stabilizer->ver_stride = 0;
}

int video_stabilizer_update(VideoStabilizer *stabilizer, const StabilizerConfig *config) {
    if (stabilizer == NULL) {
        return -EINVAL;
    }
    if (!stabilizer->initialized) {
        return video_stabilizer_init(stabilizer, config);
    }
    StabilizerConfig cfg = stabilizer->config;
    if (config) {
        cfg = *config;
    }
    stabilizer_copy_defaults(&cfg);
    stabilizer->config = cfg;
#if PIXELPILOT_HAVE_RGA
    stabilizer->available = cfg.enable ? TRUE : FALSE;
#else
    if (cfg.enable) {
        LOGW("Video stabilizer requested but librga support is not available at build time");
    }
    stabilizer->available = FALSE;
#endif
    return 0;
}

int video_stabilizer_configure(VideoStabilizer *stabilizer, uint32_t width, uint32_t height,
                               uint32_t hor_stride, uint32_t ver_stride) {
    if (stabilizer == NULL) {
        return -EINVAL;
    }
    if (!stabilizer->initialized) {
        return -EINVAL;
    }
    stabilizer->width = width;
    stabilizer->height = height;
    stabilizer->hor_stride = hor_stride;
    stabilizer->ver_stride = ver_stride;
    stabilizer->configured = TRUE;
    return 0;
}

static int wait_for_fence(int fence_fd) {
    if (fence_fd < 0) {
        return 0;
    }
    struct pollfd pfd = {.fd = fence_fd, .events = POLLIN};
    while (TRUE) {
        int ret = poll(&pfd, 1, -1);
        if (ret == 0) {
            continue;
        }
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        break;
    }
    return 0;
}

#if PIXELPILOT_HAVE_RGA
static int stabilizer_apply_transform(const StabilizerParams *params) {
    if (params == NULL) {
        return 0;
    }
    if (params->has_transform) {
        return 1;
    }
    if (params->translate_x != 0.0f || params->translate_y != 0.0f || params->rotate_deg != 0.0f) {
        return 1;
    }
    return 0;
}
#endif

int video_stabilizer_process(VideoStabilizer *stabilizer, int in_fd, int out_fd,
                             const StabilizerParams *params, int *release_fence_fd) {
    if (release_fence_fd) {
        *release_fence_fd = -1;
    }
    if (stabilizer == NULL || !stabilizer->initialized) {
        return -EINVAL;
    }
    if (!stabilizer->config.enable || !stabilizer->available) {
        return 1; // bypass
    }
    if (!stabilizer->configured) {
        LOGW("Video stabilizer invoked without frame configuration");
        return -EINVAL;
    }
    if (params && !params->enable) {
        return 1;
    }
    int fence_fd = params ? params->acquire_fence_fd : -1;
    if (fence_fd >= 0) {
        int wait_ret = wait_for_fence(fence_fd);
        if (wait_ret != 0) {
            LOGW("Video stabilizer: acquire fence wait failed: %s", g_strerror(-wait_ret));
            close(fence_fd);
            return wait_ret;
        }
        close(fence_fd);
        fence_fd = -1;
    }

#if PIXELPILOT_HAVE_RGA
    rga_info_t src;
    rga_info_t dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));

    src.fd = in_fd;
    src.mmuFlag = 1;
    src.format = RK_FORMAT_YCbCr_420_SP;
    src.blend = 0;
    src.rect.x = 0;
    src.rect.y = 0;
    src.rect.w = (int)stabilizer->width;
    src.rect.h = (int)stabilizer->height;
    src.rect.wstride = (int)stabilizer->hor_stride;
    src.rect.hstride = (int)stabilizer->ver_stride;

    dst.fd = out_fd;
    dst.mmuFlag = 1;
    dst.format = RK_FORMAT_YCbCr_420_SP;
    dst.blend = 0;
    dst.rect.x = 0;
    dst.rect.y = 0;
    dst.rect.w = (int)stabilizer->width;
    dst.rect.h = (int)stabilizer->height;
    dst.rect.wstride = (int)stabilizer->hor_stride;
    dst.rect.hstride = (int)stabilizer->ver_stride;

    if (stabilizer_apply_transform(params)) {
        if (params && params->has_transform) {
            LOGW("Video stabilizer: affine matrix transforms not implemented; falling back to translation-only");
        }
        if (params && params->rotate_deg != 0.0f) {
            LOGW("Video stabilizer: rotation %.2f deg requested but not currently supported", params->rotate_deg);
        }
        int tx = 0;
        int ty = 0;
        if (params) {
            tx = (int)lroundf(params->translate_x * stabilizer->config.strength);
            ty = (int)lroundf(params->translate_y * stabilizer->config.strength);
            if (tx > (int)stabilizer->config.max_translation_px) {
                tx = (int)stabilizer->config.max_translation_px;
            }
            if (tx < -(int)stabilizer->config.max_translation_px) {
                tx = -(int)stabilizer->config.max_translation_px;
            }
            if (ty > (int)stabilizer->config.max_translation_px) {
                ty = (int)stabilizer->config.max_translation_px;
            }
            if (ty < -(int)stabilizer->config.max_translation_px) {
                ty = -(int)stabilizer->config.max_translation_px;
            }
        }
        src.rect.x = CLAMP(tx, -(int)stabilizer->hor_stride / 4, (int)stabilizer->hor_stride / 4);
        src.rect.y = CLAMP(ty, -(int)stabilizer->ver_stride / 4, (int)stabilizer->ver_stride / 4);
        src.rect.x = CLAMP(src.rect.x, 0, (int)stabilizer->hor_stride - (int)stabilizer->width);
        src.rect.y = CLAMP(src.rect.y, 0, (int)stabilizer->ver_stride - (int)stabilizer->height);
    }

    int ret = c_RkRgaBlit(&src, &dst, NULL);
    if (ret != 0) {
        LOGW("Video stabilizer: RGA blit failed (%d)", ret);
        return -EIO;
    }
    c_RkRgaFlush();
    if (release_fence_fd) {
        *release_fence_fd = -1;
    }
    return 0;
#else
    (void)in_fd;
    (void)out_fd;
    (void)params;
    return 1;
#endif
}

gboolean video_stabilizer_is_available(const VideoStabilizer *stabilizer) {
    return stabilizer != NULL && stabilizer->available;
}
