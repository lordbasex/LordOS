/* lbgpu.c — LordBench's GPU workloads (docs/BENCHMARK.md, "Workloads", GPU): OpenGL ES 2 offscreen on
 * the render node lord-server opened and passed in (GBM + EGL, a surfaceless context and a framebuffer
 * object of 1920×1080), nothing on screen. Every frame ends with glFinish, so the times are the GPU's.
 * Runs inside lord-bench after its sandbox: Mesa's driver loads from /usr/lib (read-only) and talks to
 * the device only through that fd (DRM ioctls). */
#include "lbgpu.h"
#include "bench.h"
#include "glscene.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define W 1920
#define H 1080

static struct gbm_device *gbm;
static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLContext ctx = EGL_NO_CONTEXT;
static GLuint fbo, color, depth, quad_prog, layer_tex[8];
static GLint q_pos, q_tex, q_alpha;

static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(s); return 0; }
    return s;
}

int lb_gpu_open(int render_fd, BenchGlInfo *info) {
    gbm = gbm_create_device(render_fd);
    if (!gbm) return -1;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    dpy = get_display ? get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL) : eglGetDisplay((EGLNativeDisplayType)gbm);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) return -1;
    const char *ext = eglQueryString(dpy, EGL_EXTENSIONS);
    if (!ext || !strstr(ext, "EGL_KHR_surfaceless_context")) return -1;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return -1;
    static const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) return -1;
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return -1;
    if (bench_scene_init(info)) return -1;
    /* the offscreen target: colour texture + depth */
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return -1;
    /* a textured full-screen quad with an alpha, for fill rate and layers */
    static const char *vs = "attribute vec2 pos; varying vec2 uv; void main(){ uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }";
    static const char *fs = "precision mediump float; varying vec2 uv; uniform sampler2D tex; uniform float alpha;"
                            "void main(){ vec4 c = texture2D(tex, uv); gl_FragColor = vec4(c.rgb, c.a * alpha); }";
    GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return -1;
    quad_prog = glCreateProgram();
    glAttachShader(quad_prog, v);
    glAttachShader(quad_prog, f);
    glLinkProgram(quad_prog);
    glDeleteShader(v);
    glDeleteShader(f);
    q_pos = glGetAttribLocation(quad_prog, "pos");
    q_tex = glGetUniformLocation(quad_prog, "tex");
    q_alpha = glGetUniformLocation(quad_prog, "alpha");
    /* eight screen-size layers with different content */
    uint32_t *px = malloc((size_t)W * H * 4);
    if (!px) return -1;
    glGenTextures(8, layer_tex);
    for (int l = 0; l < 8; l++) {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) px[y * W + x] = 0xc0000000u | (uint32_t)((x * (l + 1)) & 255) << 16 | (uint32_t)((y + l * 31) & 255) << 8 | (uint32_t)(l * 32);
        glBindTexture(GL_TEXTURE_2D, layer_tex[l]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    free(px);
    glFinish();
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}

static void quad(GLuint tex, float alpha) {
    static const float P[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glUseProgram(quad_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(q_tex, 0);
    glUniform1f(q_alpha, alpha);
    glVertexAttribPointer((GLuint)q_pos, 2, GL_FLOAT, GL_FALSE, 0, P);
    glEnableVertexAttribArray((GLuint)q_pos);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

double lb_gpu_scene(double secs) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glEnable(GL_DEPTH_TEST);
    double t0 = bench_now_ms(), end = t0 + secs * 1000;
    long frames = 0;
    do { bench_scene_draw(W, H, (double)frames * 16.667); frames++; } while (bench_now_ms() < end);
    glDisable(GL_DEPTH_TEST);
    return frames * 1000.0 / (bench_now_ms() - t0);
}

/* frames of `layers` blended full-screen quads: frames per second */
static double blend_frames(int layers, double secs) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, W, H);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    double t0 = bench_now_ms(), end = t0 + secs * 1000;
    long frames = 0;
    do {
        glClear(GL_COLOR_BUFFER_BIT);
        for (int l = 0; l < layers; l++) quad(layer_tex[l % 8], 0.9f);
        glFinish();
        frames++;
    } while (bench_now_ms() < end);
    glDisable(GL_BLEND);
    return frames * 1000.0 / (bench_now_ms() - t0);
}

double lb_gpu_fill(double secs) { return blend_frames(16, secs) * 16 * W * H / 1e6; }
double lb_gpu_layers(double secs) { return blend_frames(8, secs); }

double lb_gpu_upload(double secs) {
    uint32_t *px = malloc((size_t)W * H * 4);
    if (!px) return -1;
    for (size_t i = 0; i < (size_t)W * H; i++) px[i] = (uint32_t)(i * 2654435761u);
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    double t0 = bench_now_ms(), end = t0 + secs * 1000;
    long n = 0;
    do {
        px[n % ((size_t)W * H)] ^= 1;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glFinish();
        n++;
    } while (bench_now_ms() < end);
    double ms = bench_now_ms() - t0;
    glDeleteTextures(1, &t);
    free(px);
    return n * (double)W * H * 4 / 1e6 / (ms / 1000);
}

void lb_gpu_close(void) {
    if (dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
        eglTerminate(dpy);
    }
    if (gbm) gbm_device_destroy(gbm);
    dpy = EGL_NO_DISPLAY;
    ctx = EGL_NO_CONTEXT;
    gbm = NULL;
}
