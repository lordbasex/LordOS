/* gpucap.c — see gpucap.h. Measured on a Raspberry Pi 5 at 1920×1080: the compositor pays 0.2 ms here
 * against 26 ms for glReadPixels. A pixel pack buffer doesn't help there: the driver detiles on the CPU
 * inside glReadPixels all the same. */
#include "gpucap.h"

#ifdef LORD_GPU
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

enum { C_IDLE, C_QUEUED, C_READY };

static struct {
    int on, stop, efd, w, h, bgra;
    EGLDisplay dpy;
    EGLContext ctx;
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    atomic_int state;
    GLuint tex, fbo;                    /* the compositor's context */
    GLsync fence;
    uint32_t *px;
} C = { .efd = -1, .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static void read_frame(void) {
    pthread_mutex_lock(&C.mu);
    GLsync f = C.fence;
    C.fence = NULL;
    int w = C.w, h = C.h;
    uint32_t *px = C.px;
    GLuint tex = C.tex;
    pthread_mutex_unlock(&C.mu);
    if (f) { glClientWaitSync(f, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000); glDeleteSync(f); }
    /* attached again for every frame: V3D's driver keeps showing this context the first frames otherwise */
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glFinish();
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, C.bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (!C.bgra)
        for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
            const uint8_t *p = (const uint8_t *)&px[i];
            px[i] = 0xff000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }
    atomic_store(&C.state, C_READY);
    uint64_t one = 1;
    if (write(C.efd, &one, sizeof one) < 0) { /* the counter is full: it is readable anyway */ }
}

static void *worker(void *u) {
    (void)u;
    if (!eglMakeCurrent(C.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, C.ctx)) return NULL;
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    for (;;) {
        pthread_mutex_lock(&C.mu);
        while (!C.stop && atomic_load(&C.state) != C_QUEUED) pthread_cond_wait(&C.cv, &C.mu);
        int stop = C.stop;
        pthread_mutex_unlock(&C.mu);
        if (stop) break;
        read_frame();
    }
    glDeleteFramebuffers(1, &fbo);
    eglMakeCurrent(C.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return NULL;
}

int gpucap_start(void *dpy, void *cfg, void *ctx) {
    const char *e = getenv("LORD_CAPTURE_ASYNC");
    if (C.on || (e && !strcmp(e, "0"))) return C.on ? 0 : -1;
    GLint major = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    const char *exts = eglQueryString(dpy, EGL_EXTENSIONS), *gl = (const char *)glGetString(GL_EXTENSIONS);
    glGetError();                       /* an ES 2 context refuses GL_MAJOR_VERSION */
    if (major < 3 || !exts || !strstr(exts, "EGL_KHR_surfaceless_context")) return -1;
    static const EGLint attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    C.dpy = dpy;
    C.ctx = eglCreateContext(dpy, cfg, ctx, attr);
    if (C.ctx == EGL_NO_CONTEXT) return -1;
    C.efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    C.bgra = gl && strstr(gl, "GL_EXT_read_format_bgra");
    C.stop = 0;
    atomic_store(&C.state, C_IDLE);
    if (C.efd < 0 || pthread_create(&C.th, NULL, worker, NULL)) {
        if (C.efd >= 0) close(C.efd);
        C.efd = -1;
        eglDestroyContext(dpy, C.ctx);
        return -1;
    }
    C.on = 1;
    fprintf(stderr, "gpu: frames for sharing are read on their own thread\n");
    return 0;
}

void gpucap_stop(void) {
    if (!C.on) return;
    pthread_mutex_lock(&C.mu);
    C.stop = 1;
    pthread_cond_broadcast(&C.cv);
    pthread_mutex_unlock(&C.mu);
    pthread_join(C.th, NULL);
    eglDestroyContext(C.dpy, C.ctx);
    close(C.efd);
    free(C.px);
    C.px = NULL;
    C.efd = -1;
    C.on = 0;
    C.w = C.h = 0;
    C.tex = C.fbo = 0;                  /* they go with the compositor's context */
    C.fence = NULL;
}

int gpucap_on(void) { return C.on; }
int gpucap_idle(void) { return C.on && atomic_load(&C.state) == C_IDLE; }
int gpucap_fd(void) { return C.on ? C.efd : -1; }

static int size_for(int w, int h) {
    if (w == C.w && h == C.h) return 0;
    uint32_t *px = realloc(C.px, (size_t)w * h * 4);
    if (!px) return -1;
    if (C.tex) glDeleteTextures(1, &C.tex);
    if (!C.fbo) glGenFramebuffers(1, &C.fbo);
    glGenTextures(1, &C.tex);
    GLint was = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &was);
    glBindTexture(GL_TEXTURE_2D, C.tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glBindTexture(GL_TEXTURE_2D, (GLuint)was);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, C.fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, C.tex, 0);
    pthread_mutex_lock(&C.mu);
    C.px = px;
    C.w = w;
    C.h = h;
    pthread_mutex_unlock(&C.mu);
    return 0;
}

int gpucap_kick(int w, int h) {
    if (!gpucap_idle()) return -1;
    GLint rd = 0, dr = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rd);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dr);
    int sc = glIsEnabled(GL_SCISSOR_TEST);
    int bad = size_for(w, h);
    if (!bad) {
        if (sc) glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)dr);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, C.fbo);
        glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);     /* GL's rows go bottom first */
        bad = glGetError() != GL_NO_ERROR;
        if (sc) glEnable(GL_SCISSOR_TEST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rd);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dr);
    if (bad) return -1;
    GLsync f = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    pthread_mutex_lock(&C.mu);
    C.fence = f;
    atomic_store(&C.state, C_QUEUED);
    pthread_cond_broadcast(&C.cv);
    pthread_mutex_unlock(&C.mu);
    return 0;
}

const uint32_t *gpucap_take(int *w, int *h) {
    uint64_t n;
    if (read(C.efd, &n, sizeof n) < 0) { /* nothing to clear */ }
    if (atomic_load(&C.state) != C_READY) return NULL;
    *w = C.w;
    *h = C.h;
    return C.px;
}

void gpucap_release(void) { atomic_store(&C.state, C_IDLE); }

#else
int  gpucap_start(void *d, void *c, void *x) { (void)d; (void)c; (void)x; return -1; }
void gpucap_stop(void) {}
int  gpucap_on(void) { return 0; }
int  gpucap_idle(void) { return 0; }
int  gpucap_kick(int w, int h) { (void)w; (void)h; return -1; }
int  gpucap_fd(void) { return -1; }
const uint32_t *gpucap_take(int *w, int *h) { (void)w; (void)h; return 0; }
void gpucap_release(void) {}
#endif
