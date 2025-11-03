#include "video_ctm.h"

#include "logging.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_mode.h>

#if defined(HAVE_LIBRGA)
#include <drm_fourcc.h>
#endif

#if defined(HAVE_LIBRGA) && defined(HAVE_GBM_GLES2)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#endif

#if defined(HAVE_LIBRGA)
/* Fixed-point lookup tables (10 fractional bits) avoid per-pixel multiplications. */
#define CTM_LUT_SHIFT 10
#define CTM_LUT_SCALE (1 << CTM_LUT_SHIFT)
#define CTM_LUT_ROUND (1 << (CTM_LUT_SHIFT - 1))
#endif

#if defined(HAVE_LIBRGA) && defined(HAVE_GBM_GLES2)
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
    GLint loc_matrix;
    GLint loc_tex_y;
    GLint loc_tex_uv;
    float applied_matrix[9];
    gboolean matrix_valid;
} VideoCtmGpuState;

static const GLfloat kCtmQuadVertices[] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
};

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
    static const char *fs_source =
        "precision mediump float;\n"
        "uniform sampler2D u_tex_y;\n"
        "uniform sampler2D u_tex_uv;\n"
        "uniform mat3 u_matrix;\n"
        "varying vec2 v_texcoord;\n"
        "void main() {\n"
        "    float y = texture2D(u_tex_y, v_texcoord).r;\n"
        "    vec2 uv = texture2D(u_tex_uv, v_texcoord).rg - vec2(0.5, 0.5);\n"
        "    float y_adj = y * 1.16438356;\n"
        "    float u = uv.x;\n"
        "    float v = uv.y;\n"
        "    vec3 rgb = vec3(\n"
        "        y_adj + 1.59602678 * v,\n"
        "        y_adj - 0.39176229 * u - 0.81296765 * v,\n"
        "        y_adj + 2.01723214 * u);\n"
        "    vec3 transformed = clamp(u_matrix * rgb, 0.0, 1.0);\n"
        "    gl_FragColor = vec4(transformed, 1.0);\n"
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
    ctm->gpu_active = FALSE;
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
    if (!gpu->egl_create_image || !gpu->egl_destroy_image || !gpu->gl_image_target_texture) {
        video_ctm_gpu_destroy(ctm);
        return FALSE;
    }
    gpu->program = video_ctm_gpu_create_program();
    if (gpu->program == 0) {
        video_ctm_gpu_destroy(ctm);
        return FALSE;
    }
    glUseProgram(gpu->program);
    gpu->loc_tex_y = glGetUniformLocation(gpu->program, "u_tex_y");
    gpu->loc_tex_uv = glGetUniformLocation(gpu->program, "u_tex_uv");
    gpu->loc_matrix = glGetUniformLocation(gpu->program, "u_matrix");
    glUniform1i(gpu->loc_tex_y, 0);
    glUniform1i(gpu->loc_tex_uv, 1);
    glGenBuffers(1, &gpu->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gpu->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCtmQuadVertices), kCtmQuadVertices, GL_STATIC_DRAW);
    glGenTextures(1, &gpu->tex_y);
    glBindTexture(GL_TEXTURE_2D, gpu->tex_y);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &gpu->tex_uv);
    glBindTexture(GL_TEXTURE_2D, gpu->tex_uv);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
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

static gboolean video_ctm_gpu_upload_matrix(VideoCtm *ctm) {
    VideoCtmGpuState *gpu = ctm != NULL ? ctm->gpu_state : NULL;
    if (gpu == NULL) {
        return FALSE;
    }
    float matrix[9];
    for (int i = 0; i < 9; ++i) {
        matrix[i] = (float)ctm->matrix[i];
    }
    if (gpu->matrix_valid && memcmp(matrix, gpu->applied_matrix, sizeof(matrix)) == 0) {
        return TRUE;
    }
    glUseProgram(gpu->program);
    glUniformMatrix3fv(gpu->loc_matrix, 1, GL_FALSE, matrix);
    memcpy(gpu->applied_matrix, matrix, sizeof(matrix));
    gpu->matrix_valid = TRUE;
    return TRUE;
}

static gboolean video_ctm_gpu_draw(VideoCtm *ctm, int src_fd, uint32_t width, uint32_t height,
                                   uint32_t hor_stride, uint32_t ver_stride) {
    VideoCtmGpuState *gpu = ctm != NULL ? ctm->gpu_state : NULL;
    if (gpu == NULL) {
        return FALSE;
    }
    if (!eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context)) {
        return FALSE;
    }
    if (!video_ctm_gpu_upload_matrix(ctm)) {
        return FALSE;
    }
    uint32_t chroma_width = (width + 1u) / 2u;
    uint32_t chroma_height = (height + 1u) / 2u;
    EGLint y_attrs[] = {
        EGL_WIDTH, (EGLint)width,
        EGL_HEIGHT, (EGLint)height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT, src_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)hor_stride,
        EGL_NONE
    };
    EGLint uv_attrs[] = {
        EGL_WIDTH, (EGLint)chroma_width,
        EGL_HEIGHT, (EGLint)chroma_height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_GR88,
        EGL_DMA_BUF_PLANE0_FD_EXT, src_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)(hor_stride * ver_stride),
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)hor_stride,
        EGL_NONE
    };
    EGLImageKHR y_image = gpu->egl_create_image(gpu->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, y_attrs);
    EGLImageKHR uv_image = gpu->egl_create_image(gpu->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, uv_attrs);
    if (y_image == EGL_NO_IMAGE_KHR || uv_image == EGL_NO_IMAGE_KHR) {
        if (y_image != EGL_NO_IMAGE_KHR) {
            gpu->egl_destroy_image(gpu->display, y_image);
        }
        if (uv_image != EGL_NO_IMAGE_KHR) {
            gpu->egl_destroy_image(gpu->display, uv_image);
        }
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
    gpu->egl_destroy_image(gpu->display, y_image);
    gpu->egl_destroy_image(gpu->display, uv_image);
    glFinish();
    return TRUE;
}

static int video_ctm_gpu_process(VideoCtm *ctm, int src_fd, int dst_fd, uint32_t width, uint32_t height,
                                 uint32_t hor_stride, uint32_t ver_stride, uint32_t fourcc) {
    if (ctm == NULL || ctm->gpu_state == NULL) {
        return -1;
    }
    if (fourcc != DRM_FORMAT_NV12) {
        return -1;
    }
    if (!video_ctm_gpu_ensure_target(ctm, width, height)) {
        return -1;
    }
    if (!video_ctm_gpu_draw(ctm, src_fd, width, height, hor_stride, ver_stride)) {
        return -1;
    }
    VideoCtmGpuState *gpu = ctm->gpu_state;
    int rgba_stride_px = (int)(gpu->dst_stride / 4u);
    if (rgba_stride_px <= 0) {
        rgba_stride_px = (int)width;
    }
    rga_buffer_t src = wrapbuffer_fd(gpu->dst_fd, (int)width, (int)height, RK_FORMAT_RGBA_8888,
                                     rgba_stride_px, (int)height);
    rga_buffer_t dst = wrapbuffer_fd(dst_fd, (int)width, (int)height, RK_FORMAT_YCbCr_420_SP,
                                     (int)hor_stride, (int)ver_stride);
    IM_STATUS ret = imcvtcolor(src, dst, src.format, dst.format, IM_COLOR_SPACE_DEFAULT);
    if (ret != IM_STATUS_SUCCESS) {
        LOGW("Video CTM: RGBA->NV12 conversion after GPU pass failed: %s", imStrError(ret));
        return -1;
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

static gboolean video_ctm_hw_available(const VideoCtm *ctm) {
    return (ctm != NULL && ctm->hw_supported && ctm->hw_fd >= 0 && ctm->hw_prop_id != 0 &&
            ctm->hw_object_id != 0 && ctm->hw_object_type != 0);
}

static void video_ctm_destroy_blob(VideoCtm *ctm) {
    if (ctm == NULL) {
        return;
    }
    if (ctm->hw_blob_id != 0 && ctm->hw_fd >= 0) {
        drmModeDestroyPropertyBlob(ctm->hw_fd, ctm->hw_blob_id);
        ctm->hw_blob_id = 0;
    }
}

static uint64_t video_ctm_to_s3132(double value) {
    double abs_val = fabs(value);
    const double max_val = ((double)((1ULL << 63) - 1)) / (double)(1ULL << 32);
    if (abs_val > max_val) {
        abs_val = max_val;
    }
    uint64_t magnitude = (uint64_t)llround(abs_val * (double)(1ULL << 32));
    if (magnitude > ((1ULL << 63) - 1)) {
        magnitude = (1ULL << 63) - 1;
    }
    if (value < 0.0) {
        magnitude |= (1ULL << 63);
    }
    return magnitude;
}

static gboolean video_ctm_apply_hw(VideoCtm *ctm) {
    if (!video_ctm_hw_available(ctm) || !ctm->enabled) {
        return FALSE;
    }

    struct drm_color_ctm blob;
    for (int i = 0; i < 9; ++i) {
        blob.matrix[i] = video_ctm_to_s3132(ctm->matrix[i]);
    }

    video_ctm_destroy_blob(ctm);

    if (drmModeCreatePropertyBlob(ctm->hw_fd, &blob, sizeof(blob), &ctm->hw_blob_id) != 0) {
        LOGW("Video CTM: failed to create DRM CTM blob: %s", g_strerror(errno));
        ctm->hw_blob_id = 0;
        return FALSE;
    }

    if (drmModeObjectSetProperty(ctm->hw_fd, ctm->hw_object_id, ctm->hw_object_type, ctm->hw_prop_id,
                                 ctm->hw_blob_id) != 0) {
        LOGW("Video CTM: failed to set DRM CTM property: %s", g_strerror(errno));
        drmModeDestroyPropertyBlob(ctm->hw_fd, ctm->hw_blob_id);
        ctm->hw_blob_id = 0;
        return FALSE;
    }

    ctm->hw_applied = TRUE;
    return TRUE;
}

void video_ctm_init(VideoCtm *ctm, const AppCfg *cfg) {
    if (ctm == NULL) {
        return;
    }
    memset(ctm, 0, sizeof(*ctm));
    video_ctm_set_identity(ctm);
    ctm->backend = VIDEO_CTM_BACKEND_AUTO;
    ctm->hw_supported = FALSE;
    ctm->hw_applied = FALSE;
    ctm->hw_fd = -1;
    ctm->hw_object_id = 0;
    ctm->hw_object_type = 0;
    ctm->hw_prop_id = 0;
    ctm->hw_blob_id = 0;
    ctm->render_fd = -1;

    if (cfg != NULL) {
        if (cfg->video_ctm.enable) {
            ctm->enabled = TRUE;
        }
        ctm->backend = cfg->video_ctm.backend;
        for (int i = 0; i < 9; ++i) {
            ctm->matrix[i] = cfg->video_ctm.matrix[i];
        }
    }
    if (ctm->backend < VIDEO_CTM_BACKEND_AUTO || ctm->backend > VIDEO_CTM_BACKEND_GPU) {
        ctm->backend = VIDEO_CTM_BACKEND_AUTO;
    }
#if !defined(HAVE_LIBRGA)
    if (ctm->enabled) {
        LOGW("Video CTM requested but librga support is unavailable; disabling color transform");
        ctm->enabled = FALSE;
    }
#else
    ctm->lut_ready = FALSE;
    ctm->gpu_active = FALSE;
    ctm->gpu_forced_off = FALSE;
    ctm->gpu_state = NULL;
#endif
}

void video_ctm_reset(VideoCtm *ctm) {
    if (ctm == NULL) {
        return;
    }
    video_ctm_disable_drm(ctm);
#if defined(HAVE_LIBRGA)
#if defined(HAVE_GBM_GLES2)
    if (ctm->gpu_state != NULL) {
        video_ctm_gpu_destroy(ctm);
    }
#endif
    if (ctm->rgba_buf) {
        g_free(ctm->rgba_buf);
        ctm->rgba_buf = NULL;
    }
    ctm->rgba_buf_size = 0;
    ctm->rgba_width = 0;
    ctm->rgba_height = 0;
    ctm->rgba_stride = 0;
    ctm->rgba_ver_stride = 0;
    ctm->lut_ready = FALSE;
    ctm->gpu_active = FALSE;
    ctm->gpu_forced_off = FALSE;
#endif
}

void video_ctm_set_render_fd(VideoCtm *ctm, int drm_fd) {
    if (ctm == NULL) {
        return;
    }
    ctm->render_fd = drm_fd;
}

void video_ctm_use_drm_property(VideoCtm *ctm, int drm_fd, uint32_t object_id, uint32_t object_type,
                                uint32_t prop_id) {
    if (ctm == NULL) {
        return;
    }

    video_ctm_disable_drm(ctm);

    ctm->hw_fd = drm_fd;
    ctm->hw_object_id = object_id;
    ctm->hw_object_type = object_type;
    ctm->hw_prop_id = prop_id;
    ctm->hw_supported = (drm_fd >= 0 && object_id != 0 && prop_id != 0);
    if (ctm->render_fd < 0) {
        ctm->render_fd = drm_fd;
    }
}

void video_ctm_disable_drm(VideoCtm *ctm) {
    if (ctm == NULL) {
        return;
    }

    if (ctm->hw_applied && video_ctm_hw_available(ctm)) {
        if (drmModeObjectSetProperty(ctm->hw_fd, ctm->hw_object_id, ctm->hw_object_type, ctm->hw_prop_id, 0) != 0) {
            LOGW("Video CTM: failed to clear DRM CTM property: %s", g_strerror(errno));
        }
    }
    ctm->hw_applied = FALSE;
    video_ctm_destroy_blob(ctm);
}

#if defined(HAVE_LIBRGA)
static gboolean video_ctm_build_lut(VideoCtm *ctm) {
    if (ctm == NULL) {
        return FALSE;
    }

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            double coeff = ctm->matrix[row * 3 + col];
            if (!isfinite(coeff)) {
                LOGW("Video CTM: invalid coefficient (%d,%d); disabling transform", row, col);
                ctm->enabled = FALSE;
                ctm->lut_ready = FALSE;
                return FALSE;
            }
            for (int value = 0; value < 256; ++value) {
                double scaled = coeff * (double)value * (double)CTM_LUT_SCALE;
                if (scaled > (double)INT32_MAX) {
                    scaled = (double)INT32_MAX;
                } else if (scaled < (double)INT32_MIN) {
                    scaled = (double)INT32_MIN;
                }
                ctm->lut[row][col][value] = (int32_t)llround(scaled);
            }
        }
    }

    ctm->lut_ready = TRUE;
    return TRUE;
}

static inline guint8 clamp_byte_from_shifted(int64_t value) {
    int64_t shifted;
    if (value >= 0) {
        shifted = (value + CTM_LUT_ROUND) >> CTM_LUT_SHIFT;
    } else {
        shifted = (value - CTM_LUT_ROUND) >> CTM_LUT_SHIFT;
    }
    if (shifted < 0) {
        return 0;
    }
    if (shifted > 255) {
        return 255;
    }
    return (guint8)shifted;
}

static void apply_rgba_matrix(VideoCtm *ctm, uint32_t width, uint32_t height) {
    if (ctm == NULL || ctm->rgba_buf == NULL || ctm->rgba_stride == 0) {
        return;
    }
    if (!ctm->lut_ready && !video_ctm_build_lut(ctm)) {
        return;
    }

    guint8 *row = ctm->rgba_buf;
    size_t stride_bytes = (size_t)ctm->rgba_stride * 4u;
    for (uint32_t y = 0; y < height; ++y) {
        guint8 *pixel = row + (size_t)y * stride_bytes;
        for (uint32_t x = 0; x < width; ++x) {
            guint8 r = pixel[0];
            guint8 g = pixel[1];
            guint8 b = pixel[2];

            int64_t acc_r = (int64_t)ctm->lut[0][0][r] + (int64_t)ctm->lut[0][1][g] +
                            (int64_t)ctm->lut[0][2][b];
            int64_t acc_g = (int64_t)ctm->lut[1][0][r] + (int64_t)ctm->lut[1][1][g] +
                            (int64_t)ctm->lut[1][2][b];
            int64_t acc_b = (int64_t)ctm->lut[2][0][r] + (int64_t)ctm->lut[2][1][g] +
                            (int64_t)ctm->lut[2][2][b];

            pixel[0] = clamp_byte_from_shifted(acc_r);
            pixel[1] = clamp_byte_from_shifted(acc_g);
            pixel[2] = clamp_byte_from_shifted(acc_b);

            pixel += 4;
        }
    }
}
#endif

int video_ctm_prepare(VideoCtm *ctm, uint32_t width, uint32_t height, uint32_t hor_stride,
                      uint32_t ver_stride, uint32_t fourcc) {
    if (ctm == NULL || !ctm->enabled) {
        video_ctm_disable_drm(ctm);
        return 0;
    }
    if (video_ctm_hw_available(ctm)) {
        if (video_ctm_apply_hw(ctm)) {
            return 0;
        }
        LOGW("Video CTM: falling back to software path after DRM CTM failure");
        video_ctm_disable_drm(ctm);
        ctm->hw_supported = FALSE;
    }

#if !defined(HAVE_LIBRGA)
    (void)width;
    (void)height;
    (void)hor_stride;
    (void)ver_stride;
    (void)fourcc;
    return -1;
#else

    if (fourcc != DRM_FORMAT_NV12) {
        LOGW("Video CTM: unsupported DRM format 0x%08x; disabling transform", fourcc);
        ctm->enabled = FALSE;
        return -1;
    }

#if defined(HAVE_GBM_GLES2)
    if (!ctm->gpu_forced_off && (ctm->backend == VIDEO_CTM_BACKEND_GPU ||
                                 (ctm->backend == VIDEO_CTM_BACKEND_AUTO && ctm->render_fd >= 0))) {
        if (video_ctm_gpu_init(ctm) && video_ctm_gpu_ensure_target(ctm, width, height)) {
            ctm->gpu_active = TRUE;
            return 0;
        }
        if (ctm->gpu_state != NULL) {
            video_ctm_gpu_destroy(ctm);
        }
        if (ctm->backend == VIDEO_CTM_BACKEND_GPU) {
            LOGW("Video CTM: GPU backend requested but initialization failed; disabling transform");
            ctm->enabled = FALSE;
            return -1;
        }
        ctm->gpu_forced_off = TRUE;
    }
#else
    if (ctm->backend == VIDEO_CTM_BACKEND_GPU) {
        LOGW("Video CTM: GPU backend requested but libmali support is unavailable at build time");
        ctm->enabled = FALSE;
        return -1;
    }
#endif

    ctm->gpu_active = FALSE;

    if (width == 0 || height == 0) {
        LOGW("Video CTM: refusing to prepare zero-sized buffer (%ux%u)", width, height);
        ctm->enabled = FALSE;
        return -1;
    }

    uint32_t rgba_stride = hor_stride != 0 ? hor_stride : width;
    uint32_t rgba_ver_stride = ver_stride != 0 ? ver_stride : height;
    if (rgba_stride < width || rgba_ver_stride < height) {
        LOGW("Video CTM: invalid stride %u/%u for %ux%u frame; disabling transform", rgba_stride, rgba_ver_stride,
             width, height);
        ctm->enabled = FALSE;
        return -1;
    }

    if (!ctm->lut_ready && !video_ctm_build_lut(ctm)) {
        return -1;
    }

    size_t needed = (size_t)rgba_stride * rgba_ver_stride * 4u;
    if (ctm->rgba_buf_size < needed) {
        guint8 *new_buf = g_try_malloc0(needed);
        if (new_buf == NULL) {
            LOGE("Video CTM: failed to allocate intermediate RGBA buffer");
            ctm->enabled = FALSE;
            return -1;
        }
        if (ctm->rgba_buf) {
            g_free(ctm->rgba_buf);
        }
        ctm->rgba_buf = new_buf;
        ctm->rgba_buf_size = needed;
    }
    ctm->rgba_width = width;
    ctm->rgba_height = height;
    ctm->rgba_stride = rgba_stride;
    ctm->rgba_ver_stride = rgba_ver_stride;
    return 0;
#endif
}

int video_ctm_process(VideoCtm *ctm, int src_fd, int dst_fd, uint32_t width, uint32_t height,
                      uint32_t hor_stride, uint32_t ver_stride, uint32_t fourcc) {
    if (ctm == NULL || !ctm->enabled) {
        return -1;
    }
    if (video_ctm_hw_available(ctm)) {
        return ctm->hw_applied ? 0 : -1;
    }
#if !defined(HAVE_LIBRGA)
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
#if defined(HAVE_GBM_GLES2)
    if (ctm->gpu_active && ctm->gpu_state != NULL) {
        int gpu_ret = video_ctm_gpu_process(ctm, src_fd, dst_fd, width, height, eff_hor_stride, eff_ver_stride, fourcc);
        if (gpu_ret == 0) {
            return 0;
        }
        LOGW("Video CTM: GPU processing failed; falling back to CPU path");
        if (ctm->gpu_state != NULL) {
            video_ctm_gpu_destroy(ctm);
        }
        ctm->gpu_forced_off = TRUE;
        if (ctm->backend == VIDEO_CTM_BACKEND_GPU) {
            ctm->enabled = FALSE;
            return -1;
        }
    }
#endif
    if (ctm->rgba_buf == NULL || ctm->rgba_width != width || ctm->rgba_height != height) {
        if (video_ctm_prepare(ctm, width, height, eff_hor_stride, eff_ver_stride, fourcc) != 0) {
            return -1;
        }
#if defined(HAVE_GBM_GLES2)
        if (ctm->gpu_active && ctm->gpu_state != NULL) {
            int gpu_ret = video_ctm_gpu_process(ctm, src_fd, dst_fd, width, height, eff_hor_stride, eff_ver_stride, fourcc);
            if (gpu_ret == 0) {
                return 0;
            }
            LOGW("Video CTM: GPU processing failed during prepare fallback; disabling GPU backend");
            if (ctm->gpu_state != NULL) {
                video_ctm_gpu_destroy(ctm);
            }
            ctm->gpu_forced_off = TRUE;
            if (ctm->backend == VIDEO_CTM_BACKEND_GPU) {
                ctm->enabled = FALSE;
                return -1;
            }
        }
#endif
    }

    if (fourcc != DRM_FORMAT_NV12) {
        return -1;
    }

    rga_buffer_t src = wrapbuffer_fd(src_fd, (int)width, (int)height, RK_FORMAT_YCbCr_420_SP,
                                     (int)eff_hor_stride, (int)eff_ver_stride);
    rga_buffer_t dst = wrapbuffer_fd(dst_fd, (int)width, (int)height, RK_FORMAT_YCbCr_420_SP,
                                     (int)eff_hor_stride, (int)eff_ver_stride);
    rga_buffer_t tmp = wrapbuffer_virtualaddr(ctm->rgba_buf, (int)width, (int)height, RK_FORMAT_RGBA_8888,
                                              (int)ctm->rgba_stride, (int)ctm->rgba_ver_stride);

    IM_STATUS ret = imcvtcolor(src, tmp, src.format, tmp.format, IM_COLOR_SPACE_DEFAULT);
    if (ret != IM_STATUS_SUCCESS) {
        LOGW("Video CTM: imcvtcolor NV12->RGBA failed: %s", imStrError(ret));
        ctm->enabled = FALSE;
        return -1;
    }

    apply_rgba_matrix(ctm, width, height);

    ret = imcvtcolor(tmp, dst, tmp.format, dst.format, IM_COLOR_SPACE_DEFAULT);
    if (ret != IM_STATUS_SUCCESS) {
        LOGW("Video CTM: imcvtcolor RGBA->NV12 failed: %s", imStrError(ret));
        ctm->enabled = FALSE;
        return -1;
    }

    return 0;
#endif
}
