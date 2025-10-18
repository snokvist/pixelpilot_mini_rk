#include "video_stabilizer.h"

#include "logging.h"

#include <errno.h>
#include <glib.h>
#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if PIXELPILOT_HAVE_RGA
#include <rga/RgaApi.h>
#include <rga/rga.h>
#ifndef HAVE_RGA_SET_RECT_PROTO
extern int rga_set_rect(rga_rect_t *rect, int x, int y, int w, int h, int sw, int sh, int format);
#endif
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
    guint64 frames_processed;
    guint64 demo_start_us;
    gboolean diag_logged_disabled;
    gboolean diag_logged_unconfigured;
    gboolean diag_logged_no_params;
    gboolean diag_logged_translation_clamp;
    gboolean diag_logged_stride_clamp;
    gboolean diag_logged_base_crop;
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
    cfg->diagnostics = cfg->diagnostics ? 1 : 0;
    cfg->demo_enable = cfg->demo_enable ? 1 : 0;
    if (cfg->demo_amplitude_px < 0.0f) {
        cfg->demo_amplitude_px = 0.0f;
    }
    if (cfg->demo_frequency_hz <= 0.0f) {
        cfg->demo_frequency_hz = 0.5f;
    }
    cfg->manual_enable = cfg->manual_enable ? 1 : 0;
    if (!isfinite(cfg->manual_offset_x_px)) {
        cfg->manual_offset_x_px = 0.0f;
    }
    if (!isfinite(cfg->manual_offset_y_px)) {
        cfg->manual_offset_y_px = 0.0f;
    }
    if (!isfinite(cfg->guard_band_x_px)) {
        cfg->guard_band_x_px = -1.0f;
    }
    if (!isfinite(cfg->guard_band_y_px)) {
        cfg->guard_band_y_px = -1.0f;
    }
    if (cfg->estimator_enable < 0) {
        cfg->estimator_enable = cfg->enable ? 1 : 0;
    } else {
        cfg->estimator_enable = cfg->estimator_enable ? 1 : 0;
    }
    if (cfg->estimator_diagnostics < 0) {
        cfg->estimator_diagnostics = cfg->diagnostics ? 1 : 0;
    } else {
        cfg->estimator_diagnostics = cfg->estimator_diagnostics ? 1 : 0;
    }
    if (cfg->estimator_search_radius_px <= 0) {
        cfg->estimator_search_radius_px = (int)ceilf(cfg->max_translation_px > 0.0f ? cfg->max_translation_px : 16.0f);
    }
    if (cfg->estimator_downsample_factor <= 0) {
        cfg->estimator_downsample_factor = 4;
    }
    if (cfg->estimator_max_sample_width_px < -1) {
        cfg->estimator_max_sample_width_px = -1;
    }
    if (cfg->estimator_max_sample_height_px < -1) {
        cfg->estimator_max_sample_height_px = -1;
    }
    if (!isfinite(cfg->estimator_smoothing_factor) || cfg->estimator_smoothing_factor < 0.0f) {
        cfg->estimator_smoothing_factor = 0.6f;
    }
    if (cfg->estimator_smoothing_factor > 0.98f) {
        cfg->estimator_smoothing_factor = 0.98f;
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
    stabilizer->frames_processed = 0;
    stabilizer->demo_start_us = 0;
    stabilizer->diag_logged_disabled = FALSE;
    stabilizer->diag_logged_unconfigured = FALSE;
    stabilizer->diag_logged_no_params = FALSE;
    stabilizer->diag_logged_translation_clamp = FALSE;
    stabilizer->diag_logged_stride_clamp = FALSE;
    stabilizer->diag_logged_base_crop = FALSE;
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
    stabilizer->diag_logged_disabled = FALSE;
    stabilizer->diag_logged_unconfigured = FALSE;
    stabilizer->diag_logged_no_params = FALSE;
    stabilizer->diag_logged_translation_clamp = FALSE;
    stabilizer->diag_logged_stride_clamp = FALSE;
    stabilizer->diag_logged_base_crop = FALSE;
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
    stabilizer->frames_processed = 0;
    stabilizer->demo_start_us = 0;
    stabilizer->diag_logged_no_params = FALSE;
    stabilizer->diag_logged_translation_clamp = FALSE;
    stabilizer->diag_logged_stride_clamp = FALSE;
    stabilizer->diag_logged_base_crop = FALSE;
    return 0;
}

static guint64 stabilizer_now_us(void) {
    return (guint64)g_get_monotonic_time();
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
        if (stabilizer->config.diagnostics && !stabilizer->diag_logged_disabled) {
            LOGI("Video stabilizer bypassed (enable=%d available=%d)", stabilizer->config.enable,
                 stabilizer->available);
            stabilizer->diag_logged_disabled = TRUE;
        }
        return 1; // bypass
    }
    if (!stabilizer->configured) {
        LOGW("Video stabilizer invoked without frame configuration");
        if (stabilizer->config.diagnostics && !stabilizer->diag_logged_unconfigured) {
            LOGI("Video stabilizer diagnostics: waiting for configure() before processing");
            stabilizer->diag_logged_unconfigured = TRUE;
        }
        return -EINVAL;
    }
    gboolean force_demo =
        stabilizer->config.demo_enable && stabilizer->config.demo_amplitude_px > 0.0f;
    gboolean manual_override = stabilizer->config.manual_enable;
    if (params && !params->enable && !force_demo && !manual_override) {
        if (stabilizer->config.diagnostics && !stabilizer->diag_logged_no_params) {
            LOGI("Video stabilizer bypassed: per-frame parameters disabled");
            stabilizer->diag_logged_no_params = TRUE;
        }
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

    const int width = (int)stabilizer->width;
    const int height = (int)stabilizer->height;
    const int hor_stride = (int)stabilizer->hor_stride;
    const int ver_stride = (int)stabilizer->ver_stride;
    if (hor_stride < width || ver_stride < height) {
        LOGW("Video stabilizer: invalid strides %d x %d for %d x %d frame", hor_stride, ver_stride, width,
             height);
        return -EINVAL;
    }
    const int stride_margin_x = hor_stride > width ? (hor_stride - width) : 0;
    const int stride_margin_y = ver_stride > height ? (ver_stride - height) : 0;

    float guard_cfg_x = stabilizer->config.guard_band_x_px;
    float guard_cfg_y = stabilizer->config.guard_band_y_px;
    if (guard_cfg_x < 0.0f) {
        guard_cfg_x = ceilf(stabilizer->config.max_translation_px > 0.0f ? stabilizer->config.max_translation_px : 0.0f);
    }
    if (guard_cfg_y < 0.0f) {
        guard_cfg_y = ceilf(stabilizer->config.max_translation_px > 0.0f ? stabilizer->config.max_translation_px : 0.0f);
    }
    int requested_guard_x = (int)lroundf(guard_cfg_x);
    int requested_guard_y = (int)lroundf(guard_cfg_y);
    if (requested_guard_x < 0) {
        requested_guard_x = 0;
    }
    if (requested_guard_y < 0) {
        requested_guard_y = 0;
    }
    int guard_band_x = requested_guard_x;
    int guard_band_y = requested_guard_y;
    if (guard_band_x < 0) {
        guard_band_x = 0;
    }
    if (guard_band_y < 0) {
        guard_band_y = 0;
    }

    int max_guard_x = width / 2;
    int max_guard_y = height / 2;
    if (max_guard_x < 0) {
        max_guard_x = 0;
    }
    if (max_guard_y < 0) {
        max_guard_y = 0;
    }
    if (guard_band_x > max_guard_x) {
        guard_band_x = max_guard_x;
    }
    if (guard_band_y > max_guard_y) {
        guard_band_y = max_guard_y;
    }
    if ((guard_band_x & 1) != 0) {
        guard_band_x &= ~1;
    }
    if ((guard_band_y & 1) != 0) {
        guard_band_y &= ~1;
    }

    int src_width = width - (guard_band_x * 2);
    int src_height = height - (guard_band_y * 2);
    if (src_width <= 0) {
        src_width = 2;
        guard_band_x = (width - src_width) / 2;
        guard_band_x &= ~1;
    }
    if (src_height <= 0) {
        src_height = 2;
        guard_band_y = (height - src_height) / 2;
        guard_band_y &= ~1;
    }
    if ((src_width & 1) != 0) {
        src_width--;
    }
    if ((src_height & 1) != 0) {
        src_height--;
    }
    if (src_width <= 0) {
        src_width = width;
        guard_band_x = 0;
    }
    if (src_height <= 0) {
        src_height = height;
        guard_band_y = 0;
    }

    int crop_x = guard_band_x;
    int crop_y = guard_band_y;
    gboolean using_demo = FALSE;
    gboolean using_manual = FALSE;
    gboolean had_params = params && params->enable;
    gboolean have_transform = FALSE;
    gboolean have_requested_offsets = FALSE;
    gboolean limited_by_translation = FALSE;
    gboolean limited_by_stride = FALSE;
    int requested_tx = 0;
    int requested_ty = 0;
    const char *request_mode = NULL;

    int left_limit_x = guard_band_x;
    int right_limit_x = guard_band_x + stride_margin_x;
    int left_limit_y = guard_band_y;
    int right_limit_y = guard_band_y + stride_margin_y;

    if (stabilizer->config.diagnostics && (guard_band_x > 0 || guard_band_y > 0) &&
        !stabilizer->diag_logged_base_crop) {
        LOGI("Video stabilizer base crop guard=(%d,%d) requested=(%d,%d) src=(%d,%d) stride extra %d x %d",
             guard_band_x, guard_band_y, requested_guard_x, requested_guard_y, src_width, src_height, stride_margin_x,
             stride_margin_y);
        stabilizer->diag_logged_base_crop = TRUE;
    }

    src.fd = in_fd;
    src.mmuFlag = 1;
    src.format = RK_FORMAT_YCbCr_420_SP;
    src.blend = 0;

    dst.fd = out_fd;
    dst.mmuFlag = 1;
    dst.format = RK_FORMAT_YCbCr_420_SP;
    dst.blend = 0;

    if (params && params->enable && stabilizer_apply_transform(params)) {
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
            requested_tx = tx;
            requested_ty = ty;
            have_requested_offsets = TRUE;
            request_mode = "parameter";
        }
        tx = CLAMP(tx, -(int)stabilizer->config.max_translation_px, (int)stabilizer->config.max_translation_px);
        ty = CLAMP(ty, -(int)stabilizer->config.max_translation_px, (int)stabilizer->config.max_translation_px);
        if (tx != requested_tx || ty != requested_ty) {
            limited_by_translation = TRUE;
        }
        int stride_tx = tx;
        if (stride_tx < -left_limit_x) {
            stride_tx = -left_limit_x;
        }
        if (stride_tx > right_limit_x) {
            stride_tx = right_limit_x;
        }
        int stride_ty = ty;
        if (stride_ty < -left_limit_y) {
            stride_ty = -left_limit_y;
        }
        if (stride_ty > right_limit_y) {
            stride_ty = right_limit_y;
        }
        if (have_requested_offsets && (stride_tx != tx || stride_ty != ty)) {
            limited_by_stride = TRUE;
        }
        crop_x = guard_band_x + stride_tx;
        crop_y = guard_band_y + stride_ty;
        have_transform = TRUE;
    }

    if (!have_transform && stabilizer->config.manual_enable) {
        int tx = (int)lroundf(stabilizer->config.manual_offset_x_px);
        int ty = (int)lroundf(stabilizer->config.manual_offset_y_px);
        requested_tx = tx;
        requested_ty = ty;
        have_requested_offsets = TRUE;
        request_mode = "manual";
        tx = CLAMP(tx, -(int)stabilizer->config.max_translation_px, (int)stabilizer->config.max_translation_px);
        ty = CLAMP(ty, -(int)stabilizer->config.max_translation_px, (int)stabilizer->config.max_translation_px);
        if (tx != requested_tx || ty != requested_ty) {
            limited_by_translation = TRUE;
        }
        int stride_tx = tx;
        if (stride_tx < -left_limit_x) {
            stride_tx = -left_limit_x;
        }
        if (stride_tx > right_limit_x) {
            stride_tx = right_limit_x;
        }
        int stride_ty = ty;
        if (stride_ty < -left_limit_y) {
            stride_ty = -left_limit_y;
        }
        if (stride_ty > right_limit_y) {
            stride_ty = right_limit_y;
        }
        if (stride_tx != tx || stride_ty != ty) {
            limited_by_stride = TRUE;
        }
        crop_x = guard_band_x + stride_tx;
        crop_y = guard_band_y + stride_ty;
        using_manual = TRUE;
        have_transform = TRUE;
    }

    if (!have_transform && force_demo) {
        if (stabilizer->demo_start_us == 0) {
            stabilizer->demo_start_us = stabilizer_now_us();
        }
        guint64 now = stabilizer_now_us();
        double t = (double)(now - stabilizer->demo_start_us) / 1000000.0;
        double freq = stabilizer->config.demo_frequency_hz > 0.0f ? stabilizer->config.demo_frequency_hz : 0.5f;
        double amp = (double)stabilizer->config.demo_amplitude_px;
        int tx = (int)lround(amp * sin(2.0 * M_PI * freq * t));
        int ty = (int)lround(amp * cos(2.0 * M_PI * freq * t));
        tx = CLAMP(tx, -(int)stabilizer->config.max_translation_px, (int)stabilizer->config.max_translation_px);
        ty = CLAMP(ty, -(int)stabilizer->config.max_translation_px, (int)stabilizer->config.max_translation_px);
        int stride_tx = tx;
        if (stride_tx < -left_limit_x) {
            stride_tx = -left_limit_x;
        }
        if (stride_tx > right_limit_x) {
            stride_tx = right_limit_x;
        }
        int stride_ty = ty;
        if (stride_ty < -left_limit_y) {
            stride_ty = -left_limit_y;
        }
        if (stride_ty > right_limit_y) {
            stride_ty = right_limit_y;
        }
        crop_x = guard_band_x + stride_tx;
        crop_y = guard_band_y + stride_ty;
        using_demo = TRUE;
        have_transform = TRUE;
    }

    if ((crop_x & 1) != 0) {
        crop_x &= ~1;
    }
    if ((crop_y & 1) != 0) {
        crop_y &= ~1;
    }

    if (!have_transform && (guard_band_x > 0 || guard_band_y > 0)) {
        have_transform = TRUE;
    }

    if (stabilizer->config.diagnostics && have_requested_offsets) {
        if (limited_by_translation && !stabilizer->diag_logged_translation_clamp) {
            LOGI("Video stabilizer %s offsets (%d,%d) limited to ±%.0f px", request_mode ? request_mode : "requested",
                 requested_tx, requested_ty, stabilizer->config.max_translation_px);
            stabilizer->diag_logged_translation_clamp = TRUE;
        }
        if (limited_by_stride && !stabilizer->diag_logged_stride_clamp) {
            LOGI("Video stabilizer %s offsets (%d,%d) constrained by guard %d x %d (stride extra %d x %d); crop=(%d,%d) src=(%d,%d)",
                 request_mode ? request_mode : "requested", requested_tx, requested_ty, guard_band_x, guard_band_y,
                 stride_margin_x, stride_margin_y, crop_x, crop_y, src_width, src_height);
            stabilizer->diag_logged_stride_clamp = TRUE;
        }
    }

    if (rga_set_rect(&src.rect, crop_x, crop_y, src_width, src_height, hor_stride, ver_stride,
                     RK_FORMAT_YCbCr_420_SP) != 0) {
        LOGW("Video stabilizer: failed to set source rect (stride=%d/%d)", hor_stride, ver_stride);
        return -EINVAL;
    }
    if (rga_set_rect(&dst.rect, 0, 0, width, height, hor_stride, ver_stride, RK_FORMAT_YCbCr_420_SP) != 0) {
        LOGW("Video stabilizer: failed to set destination rect (stride=%d/%d)", hor_stride, ver_stride);
        return -EINVAL;
    }

    int ret = c_RkRgaBlit(&src, &dst, NULL);
    if (ret != 0) {
        LOGW("Video stabilizer: RGA blit failed (%d)", ret);
        return -EIO;
    }
    c_RkRgaFlush();
    stabilizer->frames_processed++;
    if (stabilizer->config.diagnostics) {
        if (have_transform) {
            if (stabilizer->frames_processed == 1 || stabilizer->frames_processed % 60 == 0) {
                LOGI("Video stabilizer applied crop=(%d,%d) demo=%s manual=%s params=%s frame=%" G_GUINT64_FORMAT,
                     crop_x, crop_y, using_demo ? "yes" : "no", using_manual ? "yes" : "no",
                     had_params ? "yes" : "no", stabilizer->frames_processed);
            }
            stabilizer->diag_logged_no_params = FALSE;
        } else if (!stabilizer->diag_logged_no_params) {
            LOGI("Video stabilizer diagnostics: no transform provided; output matches input");
            stabilizer->diag_logged_no_params = TRUE;
        }
    }
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
