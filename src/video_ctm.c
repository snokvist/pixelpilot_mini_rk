#include "video_ctm.h"

#include "logging.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>

#if defined(HAVE_LIBRGA) && defined(HAVE_GBM_GLES2)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#endif

#if defined(HAVE_LIBRGA) && defined(HAVE_GBM_GLES2)
#define VIDEO_CTM_GPU_IMAGE_CACHE_SIZE 8
#define VIDEO_CTM_DEFAULT_TRANSFER_GAMMA 2.2f

typedef struct {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t offset;
    uint32_t fourcc;
    EGLImageKHR image;
    uint64_t last_used;
} VideoCtmGpuImageEntry;

typedef struct VideoCtmGpuState {
    int drm_fd;
    struct gbm_device *gbm;
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    GLuint program;
    GLuint vbo;
    GLuint fbo;
    GLuint tex_y;
    GLuint tex_uv;
    GLuint tex_dst;
    EGLImageKHR dst_image;
    struct gbm_bo *dst_bo;
    int dst_fd;
    uint32_t dst_stride;
    uint32_t dst_width;
    uint32_t dst_height;
    PFNEGLCREATEIMAGEKHRPROC egl_create_image;
    PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image;
    PFNEGLGETPLATFORMDISPLAYEXTPROC egl_get_platform_display;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_image_target_texture;
    PFNEGLCREATESYNCKHRPROC egl_create_sync;
    PFNEGLDESTROYSYNCKHRPROC egl_destroy_sync;
    PFNEGLCLIENTWAITSYNCKHRPROC egl_client_wait_sync;
    GLint loc_matrix;
    GLint loc_apply_matrix;
    GLint loc_tex_y;
    GLint loc_tex_uv;
    GLint loc_texel;
    GLint loc_sharp_strength;
    GLint loc_gamma;
    GLint loc_gamma_inv;
    GLint loc_gamma_gain;
    GLint loc_gamma_lift;
    GLint loc_gamma_mult;
    GLint loc_apply_gamma_adjust;
    float applied_matrix[9];
    gboolean matrix_valid;
    gboolean matrix_enabled_valid;
    gboolean applied_matrix_enabled;
    float applied_sharp_strength;
    gboolean sharp_strength_valid;
    float applied_gamma;
    float applied_gamma_inv;
    float applied_gamma_gain;
    float applied_gamma_lift;
    float applied_gamma_mult[3];
    gboolean gamma_valid;
    gboolean gamma_adjust_valid;
    gboolean applied_gamma_adjust;
    gboolean flip_valid;
    gboolean applied_flip;
    VideoCtmGpuImageEntry image_cache[VIDEO_CTM_GPU_IMAGE_CACHE_SIZE];
    uint64_t frame_counter;
} VideoCtmGpuState;

static gboolean video_ctm_gpu_upload_sharpness(VideoCtm *ctm);
static gboolean video_ctm_gpu_upload_gamma(VideoCtm *ctm);
static gboolean video_ctm_gamma_active(const VideoCtm *ctm);
static void video_ctm_gpu_update_vertices(VideoCtm *ctm);

static uint64_t video_ctm_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void video_ctm_metrics_clear(VideoCtm *ctm) {
    if (ctm == NULL) {
        return;
    }
    memset(&ctm->metrics, 0, sizeof(ctm->metrics));
}

static guint64 video_ctm_metrics_add_sat(guint64 base, guint64 value) {
    if (G_MAXUINT64 - base < value) {
        return G_MAXUINT64;
    }
    return base + value;
}

static const GLfloat kCtmQuadVerticesDefault[] = {
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
};

static const GLfloat kCtmQuadVerticesFlipped[] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
};

static void video_ctm_gpu_image_cache_touch(VideoCtmGpuState *gpu, VideoCtmGpuImageEntry *entry) {
    if (gpu == NULL || entry == NULL) {
        return;
    }
    entry->last_used = ++gpu->frame_counter;
}

static void video_ctm_gpu_image_cache_reset(VideoCtmGpuState *gpu) {
    if (gpu == NULL) {
        return;
    }
    for (size_t i = 0; i < VIDEO_CTM_GPU_IMAGE_CACHE_SIZE; ++i) {
        VideoCtmGpuImageEntry *entry = &gpu->image_cache[i];
        if (entry->image != EGL_NO_IMAGE_KHR && gpu->egl_destroy_image != NULL &&
            gpu->display != EGL_NO_DISPLAY) {
            gpu->egl_destroy_image(gpu->display, entry->image);
        }
        entry->fd = -1;
        entry->width = 0;
        entry->height = 0;
        entry->pitch = 0;
        entry->offset = 0;
        entry->fourcc = 0;
        entry->image = EGL_NO_IMAGE_KHR;
        entry->last_used = 0;
    }
    gpu->frame_counter = 0;
}

static void video_ctm_gpu_update_vertices(VideoCtm *ctm) {
    VideoCtmGpuState *gpu = (ctm != NULL) ? ctm->gpu_state : NULL;
    if (gpu == NULL || gpu->vbo == 0) {
        return;
    }
    gboolean desired_flip = ctm->flip ? TRUE : FALSE;
    if (gpu->flip_valid && gpu->applied_flip == desired_flip) {
        return;
    }
    glBindBuffer(GL_ARRAY_BUFFER, gpu->vbo);
    const GLfloat *vertices = desired_flip ? kCtmQuadVerticesFlipped : kCtmQuadVerticesDefault;
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(kCtmQuadVerticesDefault), vertices);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    gpu->applied_flip = desired_flip;
    gpu->flip_valid = TRUE;
}

static gboolean video_ctm_gpu_image_matches(const VideoCtmGpuImageEntry *entry,
                                            int fd,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t pitch,
                                            uint32_t offset,
                                            uint32_t fourcc) {
    return entry != NULL && entry->fd == fd && entry->width == width && entry->height == height &&
           entry->pitch == pitch && entry->offset == offset && entry->fourcc == fourcc &&
           entry->image != EGL_NO_IMAGE_KHR;
}

static EGLImageKHR video_ctm_gpu_acquire_image(VideoCtm *ctm,
                                               int fd,
                                               uint32_t width,
                                               uint32_t height,
                                               uint32_t pitch,
                                               uint32_t offset,
                                               uint32_t fourcc) {
    VideoCtmGpuState *gpu = (ctm != NULL) ? ctm->gpu_state : NULL;
    if (gpu == NULL || gpu->egl_create_image == NULL || gpu->display == EGL_NO_DISPLAY || fd < 0) {
        return EGL_NO_IMAGE_KHR;
    }

    for (size_t i = 0; i < VIDEO_CTM_GPU_IMAGE_CACHE_SIZE; ++i) {
        VideoCtmGpuImageEntry *entry = &gpu->image_cache[i];
        if (video_ctm_gpu_image_matches(entry, fd, width, height, pitch, offset, fourcc)) {
            video_ctm_gpu_image_cache_touch(gpu, entry);
            return entry->image;
        }
    }

    EGLint attrs[] = {
        EGL_WIDTH, (EGLint)width,
        EGL_HEIGHT, (EGLint)height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch,
        EGL_NONE,
    };

    EGLImageKHR image = gpu->egl_create_image(gpu->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        LOGW("Video CTM: failed to import plane fd=%d fourcc=0x%08x", fd, fourcc);
        return EGL_NO_IMAGE_KHR;
    }

    VideoCtmGpuImageEntry *slot = NULL;
    for (size_t i = 0; i < VIDEO_CTM_GPU_IMAGE_CACHE_SIZE; ++i) {
        VideoCtmGpuImageEntry *entry = &gpu->image_cache[i];
        if (entry->image == EGL_NO_IMAGE_KHR || entry->fd < 0) {
            slot = entry;
            break;
        }
    }
    if (slot == NULL) {
        slot = &gpu->image_cache[0];
        for (size_t i = 1; i < VIDEO_CTM_GPU_IMAGE_CACHE_SIZE; ++i) {
            if (gpu->image_cache[i].last_used < slot->last_used) {
                slot = &gpu->image_cache[i];
            }
        }
        if (slot->image != EGL_NO_IMAGE_KHR && gpu->egl_destroy_image != NULL &&
            gpu->display != EGL_NO_DISPLAY) {
            gpu->egl_destroy_image(gpu->display, slot->image);
        }
    }

    slot->fd = fd;
    slot->width = width;
    slot->height = height;
    slot->pitch = pitch;
    slot->offset = offset;
    slot->fourcc = fourcc;
    slot->image = image;
    video_ctm_gpu_image_cache_touch(gpu, slot);
    return image;
}

static GLuint video_ctm_gpu_compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    if (shader == 0) {
        return 0;
    }
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        GLint log_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
        if (log_len > 1) {
            char *log = g_malloc((size_t)log_len);
            if (log) {
                glGetShaderInfoLog(shader, log_len, NULL, log);
                LOGW("Video CTM: shader compile failed: %s", log);
                g_free(log);
            }
        }
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint video_ctm_gpu_create_program(void) {
    static const char *vs_source =
        "attribute vec2 a_position;\n"
        "attribute vec2 a_texcoord;\n"
        "varying vec2 v_texcoord;\n"
        "void main() {\n"
        "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
        "    v_texcoord = a_texcoord;\n"
        "}\n";
    /*
     * Fragment processing overview (estimated relative GPU cost):
     *   1. Sample Y/UV planes and compute optional 5-tap sharpen (high when
     *      enabled due to extra texture fetches, otherwise low).
     *   2. Convert YUV to RGB and clamp into the displayable range (low).
     *   3. Conditionally decode transfer gamma only when matrix/gamma paths
     *      require linear light (medium, skips entirely for the identity path).
     *   4. Apply optional color matrix in linear space (medium when enabled).
     *   5. Apply optional gamma gain/lift/mult adjustments and fold the final
     *      re-encode into a single pow() (medium/high when enabled).
     */
    static const char *fs_source =
        "precision mediump float;\n"
        "uniform sampler2D u_tex_y;\n"
        "uniform sampler2D u_tex_uv;\n"
        "uniform mat3 u_matrix;\n"
        "uniform bool u_apply_matrix;\n"
        "uniform vec2 u_texel;\n"
        "uniform float u_sharp_strength;\n"
        "uniform float u_gamma;\n"
        "uniform float u_gamma_inv;\n"
        "uniform float u_gamma_gain;\n"
        "uniform float u_gamma_lift;\n"
        "uniform vec3 u_gamma_mult;\n"
        "uniform bool u_apply_gamma_adjust;\n"
        "varying vec2 v_texcoord;\n"
        "void main() {\n"
        "    float y_center = texture2D(u_tex_y, v_texcoord).r;\n"
        "    vec2 uv = texture2D(u_tex_uv, v_texcoord).rg - vec2(0.5, 0.5);\n"
        "    float y_sharp = y_center;\n"
        "    if (abs(u_sharp_strength) > 1e-6) {\n"
        "        vec2 texel = u_texel;\n"
        "        float y_up = texture2D(u_tex_y, v_texcoord + vec2(0.0, texel.y)).r;\n"
        "        float y_down = texture2D(u_tex_y, v_texcoord - vec2(0.0, texel.y)).r;\n"
        "        float y_left = texture2D(u_tex_y, v_texcoord - vec2(texel.x, 0.0)).r;\n"
        "        float y_right = texture2D(u_tex_y, v_texcoord + vec2(texel.x, 0.0)).r;\n"
        "        float y_blur = (y_center * 4.0 + y_up + y_down + y_left + y_right) * 0.125;\n"
        "        float high_pass = y_center - y_blur;\n"
        "        y_sharp = clamp(y_center + u_sharp_strength * high_pass, 0.0, 1.0);\n"
        "    }\n"
        "    float y_adj = y_sharp * 1.16438356;\n"
        "    float u = uv.x;\n"
        "    float v = uv.y;\n"
        "    vec3 rgb = vec3(\n"
        "        y_adj + 1.59602678 * v,\n"
        "        y_adj - 0.39176229 * u - 0.81296765 * v,\n"
        "        y_adj + 2.01723214 * u);\n"
        "    vec3 clamped_rgb = clamp(rgb, 0.0, 1.0);\n"
        "    bool needs_matrix = u_apply_matrix;\n"
        "    bool needs_gamma_adjust = u_apply_gamma_adjust;\n"
        "    float safe_gamma = max(u_gamma, 1e-4);\n"
        "    float safe_gamma_inv = 1.0 / safe_gamma;\n"
        "    bool decode_gamma = needs_matrix || needs_gamma_adjust;\n"
        "    vec3 working = clamped_rgb;\n"
        "    if (decode_gamma) {\n"
        "        working = pow(clamped_rgb, vec3(safe_gamma));\n"
        "    }\n"
        "    vec3 transformed = working;\n"
        "    if (needs_matrix) {\n"
        "        transformed = clamp(u_matrix * working, 0.0, 1.0);\n"
        "    }\n"
        "    vec3 adjusted = transformed;\n"
        "    float output_gamma_exp = decode_gamma ? safe_gamma_inv : 1.0;\n"
        "    if (needs_gamma_adjust) {\n"
        "        vec3 balanced = adjusted * u_gamma_mult;\n"
        "        vec3 lifted = clamp(balanced * u_gamma_gain + vec3(u_gamma_lift), 0.0, 1.0);\n"
        "        adjusted = lifted;\n"
        "        output_gamma_exp *= u_gamma_inv;\n"
        "    } else {\n"
        "        adjusted = clamp(adjusted, 0.0, 1.0);\n"
        "    }\n"
        "    vec3 gamma_corrected = adjusted;\n"
        "    if (abs(output_gamma_exp - 1.0) > 1e-5) {\n"
        "        gamma_corrected = pow(adjusted, vec3(output_gamma_exp));\n"
        "    }\n"
        "    gl_FragColor = vec4(gamma_corrected, 1.0);\n"
        "}\n";

    GLuint vs = video_ctm_gpu_compile_shader(GL_VERTEX_SHADER, vs_source);
    if (vs == 0) {
        return 0;
    }
    GLuint fs = video_ctm_gpu_compile_shader(GL_FRAGMENT_SHADER, fs_source);
    if (fs == 0) {
        glDeleteShader(vs);
        return 0;
    }
    GLuint program = glCreateProgram();
    if (program == 0) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_texcoord");
    glLinkProgram(program);
    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        GLint log_len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
        if (log_len > 1) {
            char *log = g_malloc((size_t)log_len);
            if (log) {
                glGetProgramInfoLog(program, log_len, NULL, log);
                LOGW("Video CTM: program link failed: %s", log);
                g_free(log);
            }
        }
        glDeleteProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }
    glDetachShader(program, vs);
    glDetachShader(program, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

static void video_ctm_gpu_destroy(VideoCtm *ctm) {
    if (ctm == NULL || ctm->gpu_state == NULL) {
        return;
    }
    VideoCtmGpuState *gpu = ctm->gpu_state;
    video_ctm_gpu_image_cache_reset(gpu);
    if (gpu->display != EGL_NO_DISPLAY && gpu->context != EGL_NO_CONTEXT && gpu->surface != EGL_NO_SURFACE) {
        eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context);
    }
    if (gpu->dst_image != EGL_NO_IMAGE_KHR && gpu->egl_destroy_image) {
        gpu->egl_destroy_image(gpu->display, gpu->dst_image);
        gpu->dst_image = EGL_NO_IMAGE_KHR;
    }
    if (gpu->fbo != 0) {
        glDeleteFramebuffers(1, &gpu->fbo);
    }
    if (gpu->vbo != 0) {
        glDeleteBuffers(1, &gpu->vbo);
    }
    if (gpu->tex_y != 0) {
        glDeleteTextures(1, &gpu->tex_y);
    }
    if (gpu->tex_uv != 0) {
        glDeleteTextures(1, &gpu->tex_uv);
    }
    if (gpu->tex_dst != 0) {
        glDeleteTextures(1, &gpu->tex_dst);
    }
    if (gpu->program != 0) {
        glDeleteProgram(gpu->program);
    }
    if (gpu->display != EGL_NO_DISPLAY && gpu->context != EGL_NO_CONTEXT) {
        eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (gpu->context != EGL_NO_CONTEXT) {
        eglDestroyContext(gpu->display, gpu->context);
    }
    if (gpu->surface != EGL_NO_SURFACE) {
        eglDestroySurface(gpu->display, gpu->surface);
    }
    if (gpu->display != EGL_NO_DISPLAY) {
        eglTerminate(gpu->display);
    }
    if (gpu->dst_bo != NULL) {
        if (gpu->dst_fd >= 0) {
            close(gpu->dst_fd);
        }
        gbm_bo_destroy(gpu->dst_bo);
    } else if (gpu->dst_fd >= 0) {
        close(gpu->dst_fd);
    }
    if (gpu->gbm != NULL) {
        gbm_device_destroy(gpu->gbm);
    }
    if (gpu->drm_fd >= 0) {
        close(gpu->drm_fd);
    }
    g_free(gpu);
    ctm->gpu_state = NULL;
}

static gboolean video_ctm_gpu_init(VideoCtm *ctm) {
    if (ctm == NULL) {
        return FALSE;
    }
    if (ctm->gpu_state != NULL) {
        return TRUE;
    }
    if (ctm->render_fd < 0) {
        return FALSE;
    }
    VideoCtmGpuState *gpu = g_new0(VideoCtmGpuState, 1);
    if (gpu == NULL) {
        return FALSE;
    }
    gpu->drm_fd = -1;
    gpu->display = EGL_NO_DISPLAY;
    gpu->context = EGL_NO_CONTEXT;
    gpu->surface = EGL_NO_SURFACE;
    gpu->dst_fd = -1;
    gpu->dst_image = EGL_NO_IMAGE_KHR;
    ctm->gpu_state = gpu;
    gpu->drm_fd = fcntl(ctm->render_fd, F_DUPFD_CLOEXEC, 0);
    if (gpu->drm_fd < 0) {
        video_ctm_gpu_destroy(ctm);
        return FALSE;
    }
    gpu->gbm = gbm_create_device(gpu->drm_fd);
    if (gpu->gbm == NULL) {
        video_ctm_gpu_destroy(ctm);
        return FALSE;
    }
    gpu->egl_get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (gpu->egl_get_platform_display) {
        gpu->display = gpu->egl_get_platform_display(EGL_PLATFORM_GBM_KHR, gpu->gbm, NULL);
    } else {
        gpu->display = eglGetDisplay((EGLNativeDisplayType)gpu->gbm);
    }
    if (gpu->display == EGL_NO_DISPLAY) {
        video_ctm_gpu_destroy(ctm);
        return FALSE;
    }
    if (!eglInitialize(gpu->display, NULL, NULL)) {
        video_ctm_gpu_destroy(ctm);
        return FALSE;
    }
    static const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config = NULL;
    EGLint num_configs = 0;
    if (!eglChooseConfig(gpu->display, config_attrs, &config, 1, &num_configs) || num_configs == 0) {
        video_ctm_gpu_destroy(ctm);
        return FALSE;
    }
    static const EGLint surface_attrs[] = {
        EGL_WIDTH, 1,
        EGL_HEIGHT, 1,
        EGL_NONE
    };
    gpu->surface = eglCreatePbufferSurface(gpu->display, config, surface_attrs);
    if (gpu->surface == EGL_NO_SURFACE) {
        video_ctm_gpu_destroy(ctm);
        return FALSE;
    }
    static const EGLint ctx_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    gpu->context = eglCreateContext(gpu->display, config, EGL_NO_CONTEXT, ctx_attrs);
    if (gpu->context == EGL_NO_CONTEXT) {
        video_ctm_gpu_destroy(ctm);
        return FALSE;
    }
    if (!eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context)) {
        video_ctm_gpu_destroy(ctm);
        return FALSE;
    }
    gpu->egl_create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    gpu->egl_destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    gpu->gl_image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    gpu->egl_create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    gpu->egl_destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    gpu->egl_client_wait_sync = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
    if (!gpu->egl_create_image || !gpu->egl_destroy_image || !gpu->gl_image_target_texture) {
        video_ctm_gpu_destroy(ctm);
        return FALSE;
    }
    video_ctm_gpu_image_cache_reset(gpu);
    gpu->program = video_ctm_gpu_create_program();
    if (gpu->program == 0) {
        video_ctm_gpu_destroy(ctm);
        return FALSE;
    }
    glUseProgram(gpu->program);
    gpu->loc_tex_y = glGetUniformLocation(gpu->program, "u_tex_y");
    gpu->loc_tex_uv = glGetUniformLocation(gpu->program, "u_tex_uv");
    gpu->loc_matrix = glGetUniformLocation(gpu->program, "u_matrix");
    gpu->loc_apply_matrix = glGetUniformLocation(gpu->program, "u_apply_matrix");
    gpu->loc_texel = glGetUniformLocation(gpu->program, "u_texel");
    gpu->loc_sharp_strength = glGetUniformLocation(gpu->program, "u_sharp_strength");
    gpu->loc_gamma = glGetUniformLocation(gpu->program, "u_gamma");
    gpu->loc_gamma_inv = glGetUniformLocation(gpu->program, "u_gamma_inv");
    gpu->loc_gamma_gain = glGetUniformLocation(gpu->program, "u_gamma_gain");
    gpu->loc_gamma_lift = glGetUniformLocation(gpu->program, "u_gamma_lift");
    gpu->loc_gamma_mult = glGetUniformLocation(gpu->program, "u_gamma_mult");
    gpu->loc_apply_gamma_adjust = glGetUniformLocation(gpu->program, "u_apply_gamma_adjust");
    glUniform1i(gpu->loc_tex_y, 0);
    glUniform1i(gpu->loc_tex_uv, 1);
    if (gpu->loc_apply_matrix >= 0) {
        glUniform1i(gpu->loc_apply_matrix, 0);
    }
    if (gpu->loc_apply_gamma_adjust >= 0) {
        glUniform1i(gpu->loc_apply_gamma_adjust, 0);
    }
    if (gpu->loc_gamma >= 0) {
        glUniform1f(gpu->loc_gamma, VIDEO_CTM_DEFAULT_TRANSFER_GAMMA);
    }
    if (gpu->loc_gamma_gain >= 0) {
        glUniform1f(gpu->loc_gamma_gain, 1.0f);
    }
    if (gpu->loc_gamma_lift >= 0) {
        glUniform1f(gpu->loc_gamma_lift, 0.0f);
    }
    if (gpu->loc_gamma_mult >= 0) {
        const GLfloat mult[3] = {1.0f, 1.0f, 1.0f};
        glUniform3fv(gpu->loc_gamma_mult, 1, mult);
    }
    glGenBuffers(1, &gpu->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gpu->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCtmQuadVerticesDefault), NULL, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glGenTextures(1, &gpu->tex_y);
    glBindTexture(GL_TEXTURE_2D, gpu->tex_y);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &gpu->tex_uv);
    glBindTexture(GL_TEXTURE_2D, gpu->tex_uv);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &gpu->tex_dst);
    glBindTexture(GL_TEXTURE_2D, gpu->tex_dst);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &gpu->fbo);
    gpu->dst_image = EGL_NO_IMAGE_KHR;
    gpu->dst_bo = NULL;
    gpu->dst_fd = -1;
    gpu->dst_stride = 0;
    gpu->dst_width = 0;
    gpu->dst_height = 0;
    gpu->matrix_valid = FALSE;
    gpu->matrix_enabled_valid = FALSE;
    gpu->applied_matrix_enabled = FALSE;
    gpu->sharp_strength_valid = FALSE;
    gpu->applied_sharp_strength = 0.0f;
    gpu->gamma_valid = FALSE;
    gpu->applied_gamma = 1.0f;
    gpu->applied_gamma_inv = 1.0f;
    gpu->applied_gamma_gain = 1.0f;
    gpu->applied_gamma_lift = 0.0f;
    gpu->applied_gamma_mult[0] = 1.0f;
    gpu->applied_gamma_mult[1] = 1.0f;
    gpu->applied_gamma_mult[2] = 1.0f;
    gpu->gamma_adjust_valid = FALSE;
    gpu->applied_gamma_adjust = FALSE;
    gpu->flip_valid = FALSE;
    gpu->applied_flip = FALSE;
    (void)video_ctm_gpu_upload_sharpness(ctm);
    (void)video_ctm_gpu_upload_gamma(ctm);
    video_ctm_gpu_update_vertices(ctm);
    return TRUE;
}

static gboolean video_ctm_gpu_ensure_target(VideoCtm *ctm, uint32_t width, uint32_t height) {
    VideoCtmGpuState *gpu = ctm != NULL ? ctm->gpu_state : NULL;
    if (gpu == NULL) {
        return FALSE;
    }
    if (gpu->dst_bo != NULL && gpu->dst_width == width && gpu->dst_height == height) {
        return TRUE;
    }
    if (gpu->dst_image != EGL_NO_IMAGE_KHR && gpu->egl_destroy_image) {
        gpu->egl_destroy_image(gpu->display, gpu->dst_image);
        gpu->dst_image = EGL_NO_IMAGE_KHR;
    }
    if (gpu->dst_bo != NULL) {
        if (gpu->dst_fd >= 0) {
            close(gpu->dst_fd);
        }
        gbm_bo_destroy(gpu->dst_bo);
        gpu->dst_bo = NULL;
        gpu->dst_fd = -1;
    }
    gpu->dst_bo = gbm_bo_create(gpu->gbm, width, height, GBM_FORMAT_ABGR8888,
                                 GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (gpu->dst_bo == NULL) {
        return FALSE;
    }
    gpu->dst_fd = gbm_bo_get_fd(gpu->dst_bo);
    if (gpu->dst_fd < 0) {
        gbm_bo_destroy(gpu->dst_bo);
        gpu->dst_bo = NULL;
        return FALSE;
    }
    gpu->dst_stride = gbm_bo_get_stride(gpu->dst_bo);
    gpu->dst_width = width;
    gpu->dst_height = height;
    EGLint attrs[] = {
        EGL_WIDTH, (EGLint)width,
        EGL_HEIGHT, (EGLint)height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, gpu->dst_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)gpu->dst_stride,
        EGL_NONE
    };
    gpu->dst_image = gpu->egl_create_image(gpu->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (gpu->dst_image == EGL_NO_IMAGE_KHR) {
        close(gpu->dst_fd);
        gpu->dst_fd = -1;
        gbm_bo_destroy(gpu->dst_bo);
        gpu->dst_bo = NULL;
        return FALSE;
    }
    glBindTexture(GL_TEXTURE_2D, gpu->tex_dst);
    gpu->gl_image_target_texture(GL_TEXTURE_2D, gpu->dst_image);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, gpu->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gpu->tex_dst, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        gpu->egl_destroy_image(gpu->display, gpu->dst_image);
        gpu->dst_image = EGL_NO_IMAGE_KHR;
        close(gpu->dst_fd);
        gpu->dst_fd = -1;
        gbm_bo_destroy(gpu->dst_bo);
        gpu->dst_bo = NULL;
        return FALSE;
    }
    gpu->matrix_valid = FALSE;
    return TRUE;
}

static EGLImageKHR video_ctm_gpu_bind_external_target(VideoCtm *ctm, int dst_fd, uint32_t width,
                                                      uint32_t height, uint32_t pitch, uint32_t fourcc) {
    VideoCtmGpuState *gpu = ctm != NULL ? ctm->gpu_state : NULL;
    if (gpu == NULL || gpu->egl_create_image == NULL || gpu->gl_image_target_texture == NULL) {
        return EGL_NO_IMAGE_KHR;
    }
    if (pitch == 0) {
        pitch = width * 4u;
    }
    EGLint attrs[] = {
        EGL_WIDTH, (EGLint)width,
        EGL_HEIGHT, (EGLint)height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)(fourcc != 0 ? fourcc : DRM_FORMAT_XRGB8888),
        EGL_DMA_BUF_PLANE0_FD_EXT, dst_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch,
        EGL_NONE
    };
    EGLImageKHR image = gpu->egl_create_image(gpu->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        return EGL_NO_IMAGE_KHR;
    }
    glBindTexture(GL_TEXTURE_2D, gpu->tex_dst);
    gpu->gl_image_target_texture(GL_TEXTURE_2D, image);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, gpu->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gpu->tex_dst, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (gpu->egl_destroy_image != NULL) {
            gpu->egl_destroy_image(gpu->display, image);
        }
        return EGL_NO_IMAGE_KHR;
    }
    return image;
}

static gboolean video_ctm_gpu_upload_sharpness(VideoCtm *ctm) {
    VideoCtmGpuState *gpu = ctm != NULL ? ctm->gpu_state : NULL;
    if (gpu == NULL || gpu->loc_sharp_strength < 0) {
        return TRUE;
    }
    float sharp = (float)ctm->sharpness;
    if (gpu->sharp_strength_valid && sharp == gpu->applied_sharp_strength) {
        return TRUE;
    }
    glUseProgram(gpu->program);
    glUniform1f(gpu->loc_sharp_strength, sharp);
    gpu->applied_sharp_strength = sharp;
    gpu->sharp_strength_valid = TRUE;
    return TRUE;
}

static gboolean video_ctm_gpu_upload_gamma(VideoCtm *ctm) {
    VideoCtmGpuState *gpu = ctm != NULL ? ctm->gpu_state : NULL;
    if (gpu == NULL) {
        return TRUE;
    }
    const float transfer_gamma = VIDEO_CTM_DEFAULT_TRANSFER_GAMMA;
    float gamma_pow = (float)ctm->gamma_value;
    if (!(gamma_pow > 0.0f)) {
        gamma_pow = 1.0f;
    }
    float gamma_pow_inv = 1.0f / gamma_pow;
    float gain = (float)ctm->gamma_gain;
    if (!isfinite(gain)) {
        gain = 1.0f;
    }
    float lift = (float)ctm->gamma_lift;
    if (!isfinite(lift)) {
        lift = 0.0f;
    }
    float mult[3] = {
        (float)ctm->gamma_r_mult,
        (float)ctm->gamma_g_mult,
        (float)ctm->gamma_b_mult,
    };
    for (size_t i = 0; i < 3; ++i) {
        if (!isfinite(mult[i])) {
            mult[i] = 1.0f;
        }
    }
    gboolean gamma_values_match = FALSE;
    if (gpu->gamma_valid) {
        gamma_values_match = (transfer_gamma == gpu->applied_gamma &&
                              gamma_pow_inv == gpu->applied_gamma_inv &&
                              gain == gpu->applied_gamma_gain &&
                              lift == gpu->applied_gamma_lift &&
                              mult[0] == gpu->applied_gamma_mult[0] &&
                              mult[1] == gpu->applied_gamma_mult[1] &&
                              mult[2] == gpu->applied_gamma_mult[2]);
    }
    gboolean gamma_adjust = video_ctm_gamma_active(ctm);
    gboolean adjust_matches = (gpu->gamma_adjust_valid && gpu->applied_gamma_adjust == gamma_adjust);
    if (gamma_values_match && adjust_matches) {
        return TRUE;
    }
    glUseProgram(gpu->program);
    if (!gamma_values_match) {
        if (gpu->loc_gamma >= 0) {
            glUniform1f(gpu->loc_gamma, transfer_gamma);
        }
        if (gpu->loc_gamma_inv >= 0) {
            glUniform1f(gpu->loc_gamma_inv, gamma_pow_inv);
        }
        if (gpu->loc_gamma_gain >= 0) {
            glUniform1f(gpu->loc_gamma_gain, gain);
        }
        if (gpu->loc_gamma_lift >= 0) {
            glUniform1f(gpu->loc_gamma_lift, lift);
        }
        if (gpu->loc_gamma_mult >= 0) {
            glUniform3fv(gpu->loc_gamma_mult, 1, mult);
        }
        gpu->applied_gamma = transfer_gamma;
        gpu->applied_gamma_inv = gamma_pow_inv;
        gpu->applied_gamma_gain = gain;
        gpu->applied_gamma_lift = lift;
        gpu->applied_gamma_mult[0] = mult[0];
        gpu->applied_gamma_mult[1] = mult[1];
        gpu->applied_gamma_mult[2] = mult[2];
        gpu->gamma_valid = TRUE;
    }
    if (!adjust_matches) {
        if (gpu->loc_apply_gamma_adjust >= 0) {
            glUniform1i(gpu->loc_apply_gamma_adjust, gamma_adjust ? 1 : 0);
        }
        gpu->applied_gamma_adjust = gamma_adjust;
        gpu->gamma_adjust_valid = TRUE;
    }
    return TRUE;
}

static gboolean video_ctm_gamma_active(const VideoCtm *ctm) {
    if (ctm == NULL) {
        return FALSE;
    }
    if (fabs(ctm->gamma_value - 1.0) > 1e-6) {
        return TRUE;
    }
    if (fabs(ctm->gamma_lift) > 1e-6) {
        return TRUE;
    }
    if (fabs(ctm->gamma_gain - 1.0) > 1e-6) {
        return TRUE;
    }
    if (fabs(ctm->gamma_r_mult - 1.0) > 1e-6) {
        return TRUE;
    }
    if (fabs(ctm->gamma_g_mult - 1.0) > 1e-6) {
        return TRUE;
    }
    if (fabs(ctm->gamma_b_mult - 1.0) > 1e-6) {
        return TRUE;
    }
    return FALSE;
}

static gboolean video_ctm_matrix_is_identity(const VideoCtm *ctm) {
    if (ctm == NULL) {
        return TRUE;
    }
    const double epsilon = 1e-6;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            double expected = (row == col) ? 1.0 : 0.0;
            double actual = ctm->matrix[row * 3 + col];
            if (fabs(actual - expected) > epsilon) {
                return FALSE;
            }
        }
    }
    return TRUE;
}

static gboolean video_ctm_gpu_upload_matrix(VideoCtm *ctm) {
    VideoCtmGpuState *gpu = ctm != NULL ? ctm->gpu_state : NULL;
    if (gpu == NULL) {
        return FALSE;
    }
    float matrix[9];
    gboolean matrix_changed = !gpu->matrix_valid;
    for (int i = 0; i < 9; ++i) {
        matrix[i] = (float)ctm->matrix[i];
        if (!matrix_changed && matrix[i] != gpu->applied_matrix[i]) {
            matrix_changed = TRUE;
        }
    }
    gboolean matrix_enabled = !video_ctm_matrix_is_identity(ctm);
    gboolean matrix_flag_changed = (!gpu->matrix_enabled_valid ||
                                    gpu->applied_matrix_enabled != matrix_enabled);
    if (!matrix_changed && !matrix_flag_changed) {
        return TRUE;
    }
    glUseProgram(gpu->program);
    if (matrix_changed) {
        glUniformMatrix3fv(gpu->loc_matrix, 1, GL_FALSE, matrix);
        memcpy(gpu->applied_matrix, matrix, sizeof(matrix));
        gpu->matrix_valid = TRUE;
    }
    if (matrix_flag_changed) {
        if (gpu->loc_apply_matrix >= 0) {
            glUniform1i(gpu->loc_apply_matrix, matrix_enabled ? 1 : 0);
        }
        gpu->applied_matrix_enabled = matrix_enabled;
        gpu->matrix_enabled_valid = TRUE;
    }
    return TRUE;
}

static gboolean video_ctm_gpu_draw(VideoCtm *ctm, int src_fd, uint32_t width, uint32_t height,
                                   uint32_t hor_stride, uint32_t ver_stride) {
    VideoCtmGpuState *gpu = ctm != NULL ? ctm->gpu_state : NULL;
    if (gpu == NULL) {
        return FALSE;
    }
    uint64_t draw_start_ns = video_ctm_monotonic_ns();
    ctm->metrics.pending_gpu_valid = FALSE;
    if (!eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context)) {
        return FALSE;
    }
    if (!video_ctm_gpu_upload_matrix(ctm)) {
        return FALSE;
    }
    if (!video_ctm_gpu_upload_sharpness(ctm)) {
        return FALSE;
    }
    if (!video_ctm_gpu_upload_gamma(ctm)) {
        return FALSE;
    }
    uint32_t chroma_width = (width + 1u) / 2u;
    uint32_t chroma_height = (height + 1u) / 2u;
    uint32_t uv_offset = hor_stride * ver_stride;
    EGLImageKHR y_image = video_ctm_gpu_acquire_image(ctm, src_fd, width, height, hor_stride, 0, DRM_FORMAT_R8);
    if (y_image == EGL_NO_IMAGE_KHR) {
        return FALSE;
    }
    EGLImageKHR uv_image =
        video_ctm_gpu_acquire_image(ctm, src_fd, chroma_width, chroma_height, hor_stride, uv_offset, DRM_FORMAT_GR88);
    if (uv_image == EGL_NO_IMAGE_KHR) {
        return FALSE;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gpu->tex_y);
    gpu->gl_image_target_texture(GL_TEXTURE_2D, y_image);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gpu->tex_uv);
    gpu->gl_image_target_texture(GL_TEXTURE_2D, uv_image);
    glBindFramebuffer(GL_FRAMEBUFFER, gpu->fbo);
    glViewport(0, 0, (GLint)width, (GLint)height);
    glUseProgram(gpu->program);
    if (gpu->loc_texel >= 0) {
        GLfloat inv_w = (width != 0u) ? (1.0f / (GLfloat)width) : 0.0f;
        GLfloat inv_h = (height != 0u) ? (1.0f / (GLfloat)height) : 0.0f;
        glUniform2f(gpu->loc_texel, inv_w, inv_h);
    }
    video_ctm_gpu_update_vertices(ctm);
    glBindBuffer(GL_ARRAY_BUFFER, gpu->vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * (GLint)sizeof(GLfloat), (const GLvoid *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * (GLint)sizeof(GLfloat), (const GLvoid *)(2 * sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    uint64_t wait_start_ns = video_ctm_monotonic_ns();
    if (gpu->egl_create_sync != NULL && gpu->egl_client_wait_sync != NULL && gpu->egl_destroy_sync != NULL) {
        EGLSyncKHR fence = gpu->egl_create_sync(gpu->display, EGL_SYNC_FENCE_KHR, NULL);
        if (fence != EGL_NO_SYNC_KHR) {
            glFlush();
            EGLint wait = gpu->egl_client_wait_sync(gpu->display, fence, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, EGL_FOREVER_KHR);
            if (wait != EGL_CONDITION_SATISFIED_KHR) {
                LOGW("Video CTM: eglClientWaitSyncKHR returned %d; forcing glFinish", wait);
                glFinish();
            }
            gpu->egl_destroy_sync(gpu->display, fence);
        } else {
            glFinish();
        }
    } else {
        glFinish();
    }
    uint64_t wait_end_ns = video_ctm_monotonic_ns();
    if (wait_end_ns < wait_start_ns) {
        wait_end_ns = wait_start_ns;
    }
    if (wait_start_ns < draw_start_ns) {
        wait_start_ns = draw_start_ns;
    }
    ctm->metrics.pending_gpu_issue_ns = wait_start_ns - draw_start_ns;
    ctm->metrics.pending_gpu_wait_ns = wait_end_ns - wait_start_ns;
    ctm->metrics.pending_gpu_total_ns = wait_end_ns - draw_start_ns;
    ctm->metrics.pending_gpu_valid = TRUE;
    return TRUE;
}

static int video_ctm_gpu_process(VideoCtm *ctm, int src_fd, int dst_fd, uint32_t width, uint32_t height,
                                 uint32_t hor_stride, uint32_t ver_stride, uint32_t fourcc,
                                 uint32_t dst_pitch, uint32_t dst_fourcc) {
    if (ctm == NULL || ctm->gpu_state == NULL) {
        return -1;
    }
    if (fourcc != DRM_FORMAT_NV12) {
        return -1;
    }
    uint64_t frame_start_ns = video_ctm_monotonic_ns();
    gboolean dst_is_rgb = (dst_fourcc == DRM_FORMAT_XRGB8888 || dst_fourcc == DRM_FORMAT_ARGB8888 ||
                           dst_fourcc == DRM_FORMAT_ABGR8888 || dst_fourcc == DRM_FORMAT_XBGR8888);
    uint64_t convert_ns = 0;
    uint64_t frame_ns = 0;
    if (dst_is_rgb) {
        EGLImageKHR target_image = video_ctm_gpu_bind_external_target(ctm, dst_fd, width, height, dst_pitch,
                                                                      dst_fourcc != 0 ? dst_fourcc : DRM_FORMAT_XRGB8888);
        if (target_image == EGL_NO_IMAGE_KHR) {
            return -1;
        }
        if (!video_ctm_gpu_draw(ctm, src_fd, width, height, hor_stride, ver_stride)) {
            VideoCtmGpuState *gpu = ctm->gpu_state;
            if (gpu != NULL && gpu->egl_destroy_image != NULL) {
                gpu->egl_destroy_image(gpu->display, target_image);
            }
            ctm->metrics.pending_gpu_valid = FALSE;
            glBindFramebuffer(GL_FRAMEBUFFER, gpu != NULL ? gpu->fbo : 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return -1;
        }
        VideoCtmGpuState *gpu = ctm->gpu_state;
        uint64_t frame_end_ns = video_ctm_monotonic_ns();
        if (gpu != NULL) {
            glBindFramebuffer(GL_FRAMEBUFFER, gpu->fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (gpu->egl_destroy_image != NULL) {
                gpu->egl_destroy_image(gpu->display, target_image);
            }
        }
        frame_ns = frame_end_ns - frame_start_ns;
        if (frame_end_ns < frame_start_ns) {
            frame_ns = 0;
        }
    } else {
        if (!video_ctm_gpu_ensure_target(ctm, width, height)) {
            return -1;
        }
        if (!video_ctm_gpu_draw(ctm, src_fd, width, height, hor_stride, ver_stride)) {
            ctm->metrics.pending_gpu_valid = FALSE;
            return -1;
        }
        VideoCtmGpuState *gpu = ctm->gpu_state;
        int rgba_stride_px = (int)(gpu->dst_stride / 4u);
        if (rgba_stride_px <= 0) {
            rgba_stride_px = (int)width;
        }
        rga_buffer_t src = wrapbuffer_fd(gpu->dst_fd, (int)width, (int)height, RK_FORMAT_RGBA_8888,
                                         rgba_stride_px, (int)height);
        int dst_stride_px = (int)hor_stride;
        if (dst_pitch != 0 && dst_fourcc == DRM_FORMAT_NV12) {
            dst_stride_px = (int)dst_pitch;
        }
        if (dst_stride_px <= 0) {
            dst_stride_px = (int)width;
        }
        rga_buffer_t dst = wrapbuffer_fd(dst_fd, (int)width, (int)height, RK_FORMAT_YCbCr_420_SP,
                                         dst_stride_px, (int)ver_stride);
        uint64_t convert_start_ns = video_ctm_monotonic_ns();
        IM_STATUS ret = imcvtcolor(src, dst, src.format, dst.format, IM_COLOR_SPACE_DEFAULT);
        uint64_t convert_end_ns = video_ctm_monotonic_ns();
        if (ret != IM_STATUS_SUCCESS) {
            LOGW("Video CTM: RGBA->NV12 conversion after GPU pass failed: %s", imStrError(ret));
            ctm->metrics.pending_gpu_valid = FALSE;
            return -1;
        }
        convert_ns = convert_end_ns - convert_start_ns;
        frame_ns = convert_end_ns - frame_start_ns;
        if (convert_end_ns < convert_start_ns) {
            convert_ns = 0;
        }
        if (convert_end_ns < frame_start_ns) {
            frame_ns = 0;
        }
    }
    guint64 issue_ns = 0;
    guint64 wait_ns = 0;
    guint64 total_ns = 0;
    if (ctm->metrics.pending_gpu_valid) {
        issue_ns = ctm->metrics.pending_gpu_issue_ns;
        wait_ns = ctm->metrics.pending_gpu_wait_ns;
        total_ns = ctm->metrics.pending_gpu_total_ns;
    }
    ctm->metrics.pending_gpu_valid = FALSE;
    ctm->metrics.last_gpu_issue_ns = issue_ns;
    ctm->metrics.last_gpu_wait_ns = wait_ns;
    ctm->metrics.last_gpu_total_ns = total_ns;
    ctm->metrics.last_convert_ns = convert_ns;
    ctm->metrics.last_frame_ns = frame_ns;
    if (ctm->metrics.frame_count < G_MAXUINT64) {
        ctm->metrics.frame_count++;
    }
    ctm->metrics.sum_gpu_issue_ns = video_ctm_metrics_add_sat(ctm->metrics.sum_gpu_issue_ns, issue_ns);
    ctm->metrics.sum_gpu_wait_ns = video_ctm_metrics_add_sat(ctm->metrics.sum_gpu_wait_ns, wait_ns);
    ctm->metrics.sum_gpu_total_ns = video_ctm_metrics_add_sat(ctm->metrics.sum_gpu_total_ns, total_ns);
    ctm->metrics.sum_convert_ns = video_ctm_metrics_add_sat(ctm->metrics.sum_convert_ns, convert_ns);
    ctm->metrics.sum_frame_ns = video_ctm_metrics_add_sat(ctm->metrics.sum_frame_ns, frame_ns);
    if (issue_ns > ctm->metrics.max_gpu_issue_ns) {
        ctm->metrics.max_gpu_issue_ns = issue_ns;
    }
    if (wait_ns > ctm->metrics.max_gpu_wait_ns) {
        ctm->metrics.max_gpu_wait_ns = wait_ns;
    }
    if (total_ns > ctm->metrics.max_gpu_total_ns) {
        ctm->metrics.max_gpu_total_ns = total_ns;
    }
    if (convert_ns > ctm->metrics.max_convert_ns) {
        ctm->metrics.max_convert_ns = convert_ns;
    }
    if (frame_ns > ctm->metrics.max_frame_ns) {
        ctm->metrics.max_frame_ns = frame_ns;
    }
    return 0;
}
#endif
static void video_ctm_set_identity(VideoCtm *ctm) {
    if (ctm == NULL) {
        return;
    }
    for (int i = 0; i < 9; ++i) {
        ctm->matrix[i] = (i % 4 == 0) ? 1.0 : 0.0;
    }
}

void video_ctm_init(VideoCtm *ctm, const AppCfg *cfg) {
    if (ctm == NULL) {
        return;
    }
    memset(ctm, 0, sizeof(*ctm));
    video_ctm_metrics_clear(ctm);
    video_ctm_set_identity(ctm);
    ctm->sharpness = 0.0;
    ctm->gamma_value = 1.0;
    ctm->gamma_lift = 0.0;
    ctm->gamma_gain = 1.0;
    ctm->gamma_r_mult = 1.0;
    ctm->gamma_g_mult = 1.0;
    ctm->gamma_b_mult = 1.0;
    ctm->flip = FALSE;
    ctm->backend = VIDEO_CTM_BACKEND_AUTO;
    ctm->render_fd = -1;

    gboolean config_enable = FALSE;
    if (cfg != NULL) {
        config_enable = cfg->video_ctm.enable != 0;
        ctm->backend = cfg->video_ctm.backend;
        for (int i = 0; i < 9; ++i) {
            ctm->matrix[i] = cfg->video_ctm.matrix[i];
        }
        ctm->sharpness = cfg->video_ctm.sharpness;
        if (cfg->video_ctm.gamma_value > 0.0) {
            ctm->gamma_value = cfg->video_ctm.gamma_value;
        } else if (cfg->video_ctm.gamma_value != 0.0) {
            LOGW("Video CTM: ignoring non-positive gamma %.3f", cfg->video_ctm.gamma_value);
        }
        ctm->gamma_lift = cfg->video_ctm.gamma_lift;
        ctm->gamma_gain = cfg->video_ctm.gamma_gain;
        ctm->gamma_r_mult = cfg->video_ctm.gamma_r_mult;
        ctm->gamma_g_mult = cfg->video_ctm.gamma_g_mult;
        ctm->gamma_b_mult = cfg->video_ctm.gamma_b_mult;
        ctm->flip = cfg->video_ctm.flip != 0;
        if (!isfinite(ctm->gamma_lift)) {
            LOGW("Video CTM: ignoring non-finite gamma lift");
            ctm->gamma_lift = 0.0;
        }
        if (!isfinite(ctm->gamma_gain)) {
            LOGW("Video CTM: ignoring non-finite gamma gain");
            ctm->gamma_gain = 1.0;
        }
        if (!isfinite(ctm->gamma_r_mult)) {
            LOGW("Video CTM: ignoring non-finite gamma r-mult");
            ctm->gamma_r_mult = 1.0;
        }
        if (!isfinite(ctm->gamma_g_mult)) {
            LOGW("Video CTM: ignoring non-finite gamma g-mult");
            ctm->gamma_g_mult = 1.0;
        }
        if (!isfinite(ctm->gamma_b_mult)) {
            LOGW("Video CTM: ignoring non-finite gamma b-mult");
            ctm->gamma_b_mult = 1.0;
        }
    }
    ctm->enabled = config_enable;
    if (ctm->backend < VIDEO_CTM_BACKEND_AUTO || ctm->backend > VIDEO_CTM_BACKEND_GPU) {
        ctm->backend = VIDEO_CTM_BACKEND_AUTO;
    }
#if defined(HAVE_LIBRGA) && defined(HAVE_GBM_GLES2)
    ctm->gpu_state = NULL;
#else
    if (ctm->enabled) {
        LOGW("Video CTM requested but the GPU backend is unavailable at build time; disabling color transform");
        ctm->enabled = FALSE;
    }
#endif
}

void video_ctm_reset(VideoCtm *ctm) {
    if (ctm == NULL) {
        return;
    }
    video_ctm_metrics_clear(ctm);
    ctm->src_fourcc = 0;
    ctm->dst_fourcc = 0;
    ctm->dst_pitch = 0;
#if defined(HAVE_LIBRGA) && defined(HAVE_GBM_GLES2)
    if (ctm->gpu_state != NULL) {
        video_ctm_gpu_destroy(ctm);
    }
#endif
}

void video_ctm_set_render_fd(VideoCtm *ctm, int drm_fd) {
    if (ctm == NULL) {
        return;
    }
    ctm->render_fd = drm_fd;
}

 

int video_ctm_prepare(VideoCtm *ctm, uint32_t width, uint32_t height, uint32_t hor_stride,
                      uint32_t ver_stride, uint32_t fourcc, uint32_t dst_pitch,
                      uint32_t dst_fourcc) {
    if (ctm == NULL || !ctm->enabled) {
        return 0;
    }
    if (dst_fourcc == 0) {
        dst_fourcc = DRM_FORMAT_NV12;
    }
    ctm->src_fourcc = fourcc;
    ctm->dst_fourcc = dst_fourcc;
    ctm->dst_pitch = dst_pitch;

    gboolean dst_supported = (dst_fourcc == DRM_FORMAT_NV12 || dst_fourcc == DRM_FORMAT_XRGB8888 ||
                              dst_fourcc == DRM_FORMAT_ARGB8888 || dst_fourcc == DRM_FORMAT_ABGR8888 ||
                              dst_fourcc == DRM_FORMAT_XBGR8888);
    if (!dst_supported) {
        LOGW("Video CTM: unsupported destination DRM format 0x%08x", dst_fourcc);
        ctm->enabled = FALSE;
        return -1;
    }

#if !defined(HAVE_LIBRGA) || !defined(HAVE_GBM_GLES2)
    (void)width;
    (void)height;
    (void)hor_stride;
    (void)ver_stride;
    (void)fourcc;
    if (ctm->enabled) {
        LOGW("Video CTM: GPU backend requested but libmali support is unavailable at build time");
        ctm->enabled = FALSE;
    }
    return -1;
#else
    (void)hor_stride;
    (void)ver_stride;
    if (fourcc != DRM_FORMAT_NV12) {
        LOGW("Video CTM: unsupported DRM format 0x%08x; disabling transform", fourcc);
        ctm->enabled = FALSE;
        return -1;
    }
    if (width == 0 || height == 0) {
        LOGW("Video CTM: refusing to prepare zero-sized buffer (%ux%u)", width, height);
        ctm->enabled = FALSE;
        return -1;
    }
    if (ctm->render_fd < 0) {
        LOGW("Video CTM: GPU backend requires a render node fd; disabling transform");
        ctm->enabled = FALSE;
        return -1;
    }
    if (ctm->backend == VIDEO_CTM_BACKEND_GPU || ctm->backend == VIDEO_CTM_BACKEND_AUTO) {
        if (!video_ctm_gpu_init(ctm)) {
            LOGW("Video CTM: failed to initialise GPU backend; disabling transform");
            ctm->enabled = FALSE;
            return -1;
        }
        return 0;
    }
    return -1;
#endif
}

int video_ctm_process(VideoCtm *ctm, int src_fd, int dst_fd, uint32_t width, uint32_t height,
                      uint32_t hor_stride, uint32_t ver_stride, uint32_t fourcc,
                      uint32_t dst_pitch, uint32_t dst_fourcc) {
    if (ctm == NULL || !ctm->enabled) {
        return -1;
    }
#if !defined(HAVE_LIBRGA) || !defined(HAVE_GBM_GLES2)
    (void)src_fd;
    (void)dst_fd;
    (void)width;
    (void)height;
    (void)hor_stride;
    (void)ver_stride;
    (void)fourcc;
    return -1;
#else
    uint32_t eff_hor_stride = hor_stride != 0 ? hor_stride : width;
    uint32_t eff_ver_stride = ver_stride != 0 ? ver_stride : height;
    if (fourcc != DRM_FORMAT_NV12) {
        LOGW("Video CTM: unsupported DRM format 0x%08x during processing", fourcc);
        ctm->enabled = FALSE;
        return -1;
    }
    uint32_t effective_dst_fourcc = dst_fourcc != 0 ? dst_fourcc : DRM_FORMAT_NV12;
    uint32_t effective_dst_pitch = dst_pitch;
    if (ctm->gpu_state == NULL || ctm->dst_fourcc != effective_dst_fourcc ||
        ctm->dst_pitch != effective_dst_pitch) {
        if (video_ctm_prepare(ctm, width, height, eff_hor_stride, eff_ver_stride, fourcc, effective_dst_pitch,
                              effective_dst_fourcc) != 0) {
            return -1;
        }
    }
    if (ctm->gpu_state == NULL) {
        return -1;
    }
    int ret = video_ctm_gpu_process(ctm, src_fd, dst_fd, width, height, eff_hor_stride, eff_ver_stride,
                                    fourcc, ctm->dst_pitch, ctm->dst_fourcc);
    if (ret != 0) {
        LOGW("Video CTM: GPU processing failed; disabling transform");
        video_ctm_gpu_destroy(ctm);
        ctm->enabled = FALSE;
        return -1;
    }
    return 0;
#endif
}

void video_ctm_apply_update(VideoCtm *ctm, const VideoCtmUpdate *update) {
    if (ctm == NULL || update == NULL || update->fields == 0) {
        return;
    }

    uint32_t fields = update->fields;
#if defined(HAVE_LIBRGA) && defined(HAVE_GBM_GLES2)
    VideoCtmGpuState *gpu = ctm->gpu_state;
#else
    VideoCtmGpuState *gpu = NULL;
#endif

    if ((fields & VIDEO_CTM_UPDATE_MATRIX) != 0u) {
        for (int i = 0; i < 9; ++i) {
            ctm->matrix[i] = update->matrix[i];
        }
#if defined(HAVE_LIBRGA) && defined(HAVE_GBM_GLES2)
        if (gpu != NULL) {
            gpu->matrix_valid = FALSE;
        }
#endif
    }

    if ((fields & VIDEO_CTM_UPDATE_SHARPNESS) != 0u) {
        ctm->sharpness = update->sharpness;
#if defined(HAVE_LIBRGA) && defined(HAVE_GBM_GLES2)
        if (gpu != NULL) {
            gpu->sharp_strength_valid = FALSE;
        }
#endif
    }

    gboolean gamma_dirty = FALSE;
    if ((fields & VIDEO_CTM_UPDATE_GAMMA) != 0u) {
        if (update->gamma_value > 0.0 && isfinite(update->gamma_value)) {
            ctm->gamma_value = update->gamma_value;
            gamma_dirty = TRUE;
        } else {
            LOGW("Video CTM: ignoring non-positive gamma %.3f", update->gamma_value);
        }
    }
    if ((fields & VIDEO_CTM_UPDATE_GAMMA_LIFT) != 0u) {
        if (isfinite(update->gamma_lift)) {
            ctm->gamma_lift = update->gamma_lift;
            gamma_dirty = TRUE;
        } else {
            LOGW("Video CTM: ignoring non-finite gamma lift");
        }
    }
    if ((fields & VIDEO_CTM_UPDATE_GAMMA_GAIN) != 0u) {
        if (isfinite(update->gamma_gain)) {
            ctm->gamma_gain = update->gamma_gain;
            gamma_dirty = TRUE;
        } else {
            LOGW("Video CTM: ignoring non-finite gamma gain");
        }
    }
    if ((fields & VIDEO_CTM_UPDATE_GAMMA_R_MULT) != 0u) {
        if (isfinite(update->gamma_r_mult)) {
            ctm->gamma_r_mult = update->gamma_r_mult;
            gamma_dirty = TRUE;
        } else {
            LOGW("Video CTM: ignoring non-finite gamma r-mult");
        }
    }
    if ((fields & VIDEO_CTM_UPDATE_GAMMA_G_MULT) != 0u) {
        if (isfinite(update->gamma_g_mult)) {
            ctm->gamma_g_mult = update->gamma_g_mult;
            gamma_dirty = TRUE;
        } else {
            LOGW("Video CTM: ignoring non-finite gamma g-mult");
        }
    }
    if ((fields & VIDEO_CTM_UPDATE_GAMMA_B_MULT) != 0u) {
        if (isfinite(update->gamma_b_mult)) {
            ctm->gamma_b_mult = update->gamma_b_mult;
            gamma_dirty = TRUE;
        } else {
            LOGW("Video CTM: ignoring non-finite gamma b-mult");
        }
    }

#if defined(HAVE_LIBRGA) && defined(HAVE_GBM_GLES2)
    if (gamma_dirty && gpu != NULL) {
        gpu->gamma_valid = FALSE;
    }
#else
    (void)gamma_dirty;
#endif

    if ((fields & VIDEO_CTM_UPDATE_FLIP) != 0u) {
        ctm->flip = update->flip ? TRUE : FALSE;
#if defined(HAVE_LIBRGA) && defined(HAVE_GBM_GLES2)
        if (gpu != NULL) {
            gpu->flip_valid = FALSE;
        }
#endif
    }
}

void video_ctm_get_metrics(const VideoCtm *ctm, VideoCtmMetrics *out_metrics) {
    if (out_metrics == NULL) {
        return;
    }
    memset(out_metrics, 0, sizeof(*out_metrics));
    if (ctm == NULL) {
        return;
    }
    const guint64 frame_count = ctm->metrics.frame_count;
    out_metrics->frame_count = frame_count;
    const double ns_to_ms = 1.0 / 1000000.0;
    out_metrics->last_gpu_issue_ms = (double)ctm->metrics.last_gpu_issue_ns * ns_to_ms;
    out_metrics->last_gpu_wait_ms = (double)ctm->metrics.last_gpu_wait_ns * ns_to_ms;
    out_metrics->last_gpu_total_ms = (double)ctm->metrics.last_gpu_total_ns * ns_to_ms;
    out_metrics->last_convert_ms = (double)ctm->metrics.last_convert_ns * ns_to_ms;
    out_metrics->last_frame_ms = (double)ctm->metrics.last_frame_ns * ns_to_ms;
    out_metrics->max_gpu_issue_ms = (double)ctm->metrics.max_gpu_issue_ns * ns_to_ms;
    out_metrics->max_gpu_wait_ms = (double)ctm->metrics.max_gpu_wait_ns * ns_to_ms;
    out_metrics->max_gpu_total_ms = (double)ctm->metrics.max_gpu_total_ns * ns_to_ms;
    out_metrics->max_convert_ms = (double)ctm->metrics.max_convert_ns * ns_to_ms;
    out_metrics->max_frame_ms = (double)ctm->metrics.max_frame_ns * ns_to_ms;
    if (frame_count > 0) {
        double denom = (double)frame_count;
        out_metrics->avg_gpu_issue_ms = (double)ctm->metrics.sum_gpu_issue_ns * ns_to_ms / denom;
        out_metrics->avg_gpu_wait_ms = (double)ctm->metrics.sum_gpu_wait_ns * ns_to_ms / denom;
        out_metrics->avg_gpu_total_ms = (double)ctm->metrics.sum_gpu_total_ns * ns_to_ms / denom;
        out_metrics->avg_convert_ms = (double)ctm->metrics.sum_convert_ns * ns_to_ms / denom;
        out_metrics->avg_frame_ms = (double)ctm->metrics.sum_frame_ns * ns_to_ms / denom;
    }
}
