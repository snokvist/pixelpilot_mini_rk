#include "video_stabilizer.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

static int create_nv12_buffer(int drm_fd, uint32_t width, uint32_t height, uint32_t *handle_out, int *prime_fd_out,
                              uint32_t *pitch_out) {
    struct drm_mode_create_dumb dmcd;
    memset(&dmcd, 0, sizeof(dmcd));
    dmcd.bpp = 8;
    dmcd.width = width;
    dmcd.height = height * 2;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dmcd) != 0) {
        return -1;
    }
    struct drm_prime_handle dph;
    memset(&dph, 0, sizeof(dph));
    dph.handle = dmcd.handle;
    dph.fd = -1;
    if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph) != 0) {
        struct drm_mode_destroy_dumb destroy_req = {.handle = dmcd.handle};
        ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
        return -1;
    }
    if (handle_out) {
        *handle_out = dmcd.handle;
    }
    if (prime_fd_out) {
        *prime_fd_out = dph.fd;
    }
    if (pitch_out) {
        *pitch_out = dmcd.pitch;
    }
    return 0;
}

static void destroy_buffer(int drm_fd, uint32_t handle, int prime_fd) {
    if (prime_fd >= 0) {
        close(prime_fd);
    }
    if (handle != 0) {
        struct drm_mode_destroy_dumb destroy_req = {.handle = handle};
        ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
    }
}

int main(void) {
    StabilizerConfig cfg = {0};
    cfg.enable = 0;
    cfg.strength = 1.0f;
    cfg.max_translation_px = 16.0f;
    cfg.max_rotation_deg = 5.0f;
    cfg.diagnostics = 0;
    cfg.demo_enable = 0;
    cfg.demo_amplitude_px = 0.0f;
    cfg.demo_frequency_hz = 0.5f;

    VideoStabilizer *stabilizer = video_stabilizer_new();
    assert(stabilizer != NULL);
    assert(video_stabilizer_init(stabilizer, &cfg) == 0);

    StabilizerParams params;
    memset(&params, 0, sizeof(params));
    params.enable = TRUE;
    params.acquire_fence_fd = -1;

    int release_fd = -1;
    int ret = video_stabilizer_process(stabilizer, -1, -1, &params, &release_fd);
    assert(ret == 1);
    assert(release_fd == -1);

    cfg.enable = 1;
    cfg.demo_enable = 1;
    cfg.demo_amplitude_px = 2.0f;
    cfg.demo_frequency_hz = 1.0f;
    video_stabilizer_update(stabilizer, &cfg);

    if (!video_stabilizer_is_available(stabilizer)) {
        printf("librga unavailable; skipping DMA smoke test\n");
        video_stabilizer_free(stabilizer);
        return 0;
    }

    const char *drm_candidates[] = {"/dev/dri/renderD128", "/dev/dri/renderD129", "/dev/dri/card0"};
    int drm_fd = -1;
    for (size_t i = 0; i < G_N_ELEMENTS(drm_candidates); ++i) {
        drm_fd = open(drm_candidates[i], O_RDWR | O_CLOEXEC);
        if (drm_fd >= 0) {
            break;
        }
    }
    if (drm_fd < 0) {
        printf("DRM device unavailable; skipping DMA smoke test\n");
        video_stabilizer_free(stabilizer);
        return 0;
    }

    uint32_t src_handle = 0;
    int src_prime = -1;
    uint32_t pitch = 0;
    if (create_nv12_buffer(drm_fd, 64, 64, &src_handle, &src_prime, &pitch) != 0) {
        printf("Failed to allocate source buffer: %s\n", g_strerror(errno));
        close(drm_fd);
        video_stabilizer_free(stabilizer);
        return 1;
    }

    uint32_t dst_handle = 0;
    int dst_prime = -1;
    if (create_nv12_buffer(drm_fd, 64, 64, &dst_handle, &dst_prime, NULL) != 0) {
        printf("Failed to allocate destination buffer: %s\n", g_strerror(errno));
        destroy_buffer(drm_fd, src_handle, src_prime);
        close(drm_fd);
        video_stabilizer_free(stabilizer);
        return 1;
    }

    if (video_stabilizer_configure(stabilizer, 64, 64, pitch, 64) != 0) {
        printf("Failed to configure stabilizer geometry\n");
        destroy_buffer(drm_fd, dst_handle, dst_prime);
        destroy_buffer(drm_fd, src_handle, src_prime);
        close(drm_fd);
        video_stabilizer_free(stabilizer);
        return 1;
    }

    memset(&params, 0, sizeof(params));
    params.enable = TRUE;
    params.acquire_fence_fd = -1;
    params.translate_x = 2.0f;
    params.translate_y = -1.5f;

    release_fd = -1;
    ret = video_stabilizer_process(stabilizer, src_prime, dst_prime, &params, &release_fd);
    if (ret != 0) {
        printf("video_stabilizer_process failed (%d)\n", ret);
        if (release_fd >= 0) {
            close(release_fd);
        }
        destroy_buffer(drm_fd, dst_handle, dst_prime);
        destroy_buffer(drm_fd, src_handle, src_prime);
        close(drm_fd);
        video_stabilizer_free(stabilizer);
        return 1;
    }
    if (release_fd >= 0) {
        close(release_fd);
    }

    memset(&params, 0, sizeof(params));
    params.enable = FALSE;
    params.acquire_fence_fd = -1;

    release_fd = -1;
    ret = video_stabilizer_process(stabilizer, src_prime, dst_prime, &params, &release_fd);
    if (ret != 0) {
        printf("demo-mode stabilizer_process failed (%d)\n", ret);
        if (release_fd >= 0) {
            close(release_fd);
        }
        destroy_buffer(drm_fd, dst_handle, dst_prime);
        destroy_buffer(drm_fd, src_handle, src_prime);
        close(drm_fd);
        video_stabilizer_free(stabilizer);
        return 1;
    }
    if (release_fd >= 0) {
        close(release_fd);
    }

    destroy_buffer(drm_fd, dst_handle, dst_prime);
    destroy_buffer(drm_fd, src_handle, src_prime);
    close(drm_fd);
    video_stabilizer_free(stabilizer);
    printf("Video stabilizer smoke test completed successfully\n");
    return 0;
}
