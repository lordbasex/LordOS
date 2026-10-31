/* gpu_contention.c — what slows the GPU under CPU load (docs/GPU.md, "What slows the GPU under CPU load"). A test
 * tool, never installed.
 *
 * On the Raspberry Pi 5 a frame's GPU time goes from 1.5 ms alone to 14–16 ms under an all-core load with no clock
 * change. This repeats a fixed frame (an upload of 1 Mpx into a 1920 × 1080 texture, then that texture composed
 * into a framebuffer of that size, at 60 frames a second) under each load of gc_plan: worker threads on 1, 2, 4…
 * cores that either spin inside their caches, read or write a large buffer. A load that only spins uses the CPU and
 * hardly the memory; a load that reads or writes uses both. Comparing them is the experiment (gc_verdict).
 *
 *   build/gpu-contention [--node /dev/dri/renderD128] [--frames 240] [--cores N]
 *
 * One JSON object a line on stdout, one per phase and a last one with the verdict. It uses a render node and no
 * display, so it runs beside a session. busMBs is each worker's last-level misses (perf_event_open, its own
 * thread) × 64 bytes: the CPU side of the traffic only, null if the kernel gives no such counter. The GPU's time is
 * the flush to the native fence's timestamp when the driver has one, as LordBench's gpuMs starts at the flip queued, so
 * a render thread waiting for a core under load doesn't count as GPU time; else, the draw and glFinish together. */
#include "gpu/gcontend.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <gbm.h>
#include <linux/perf_event.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define FW 1920
#define FH 1080
#define UPW 1400
#define UPH 714                         /* about 1 Mpx, what a drag frame uploaded on the Pi 5 */
#define WORK_BYTES (64u << 20)          /* each reader's or writer's buffer: past any cache here */
#define MAX_WORKERS 64

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ---- the load ---- */

typedef struct { pthread_t th; int cpu, kind, perf; long long misses; } Worker;
static atomic_int stop_workers;
static volatile uint64_t sink;

static int perf_open(void) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.type = PERF_TYPE_HARDWARE;
    a.config = PERF_COUNT_HW_CACHE_MISSES;
    a.exclude_kernel = a.exclude_hv = 1;
    return (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
}

static void *worker(void *arg) {
    Worker *w = arg;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(w->cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    w->perf = perf_open();
    uint8_t *buf = w->kind == GC_SPIN ? NULL : malloc(WORK_BYTES);
    if (buf) memset(buf, 1, WORK_BYTES);
    uint64_t x = 88172645463325252ull;
    while (!atomic_load_explicit(&stop_workers, memory_order_relaxed)) {
        if (w->kind == GC_SPIN) for (int i = 0; i < 100000; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; }
        else if (w->kind == GC_READ) for (size_t i = 0; i + 64 <= WORK_BYTES; i += 64) x += buf[i];
        else if (buf) memset(buf, (int)(x++ & 255), WORK_BYTES);
    }
    sink += x;
    if (w->perf >= 0) {
        long long v = 0;
        if (read(w->perf, &v, sizeof v) == (ssize_t)sizeof v) w->misses = v;
        close(w->perf);
    } else w->misses = -1;
    free(buf);
    return NULL;
}

/* ---- the GPU ---- */

static struct {
    EGLDisplay dpy;
    GLuint prog, tex, fbo, target;
    GLint u_tex;
    uint32_t *px;
    PFNEGLCREATESYNCKHRPROC create_sync;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence;
    int fence;
} G;

static GLuint shader(GLenum kind, const char *src) {
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

static int gl_setup(const char *node) {
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(node); return -1; }
    struct gbm_device *gbm = gbm_create_device(fd);
    PFNEGLGETPLATFORMDISPLAYEXTPROC get = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!gbm || !get) { fprintf(stderr, "no GBM or EGL display\n"); return -1; }
    G.dpy = get(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    EGLint maj, min, n = 0;
    if (G.dpy == EGL_NO_DISPLAY || !eglInitialize(G.dpy, &maj, &min)) { fprintf(stderr, "EGL did not start\n"); return -1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint cattr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE }, xattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLConfig cfg = NULL;
    eglChooseConfig(G.dpy, cattr, &cfg, 1, &n);
    EGLContext ctx = eglCreateContext(G.dpy, n ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, xattr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(G.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { fprintf(stderr, "no surfaceless GLES2 context\n"); return -1; }
    const char *ex = eglQueryString(G.dpy, EGL_EXTENSIONS);
    if (ex && strstr(ex, "EGL_ANDROID_native_fence_sync")) {
        G.create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
        G.destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
        G.dup_fence = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
        G.fence = G.create_sync && G.destroy_sync && G.dup_fence;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, "attribute vec2 a; varying vec2 v; void main() { v = a * 0.5 + 0.5; gl_Position = vec4(a, 0, 1); }"));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, "precision mediump float; varying vec2 v; uniform sampler2D t; void main() { gl_FragColor = texture2D(t, v); }"));
    glBindAttribLocation(p, 0, "a");
    glLinkProgram(p);
    G.prog = p;
    G.u_tex = glGetUniformLocation(p, "t");
    G.px = malloc(FW * FH * 4);
    if (!G.px) return -1;
    for (int i = 0; i < FW * FH; i++) G.px[i] = 0xff000000u | (uint32_t)(i * 2654435761u >> 8);
    GLuint t[2];
    glGenTextures(2, t);
    G.tex = t[0];
    G.target = t[1];
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, t[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FW, FH, 0, GL_RGBA, GL_UNSIGNED_BYTE, i ? NULL : G.px);
    }
    glGenFramebuffers(1, &G.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.target, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "framebuffer incomplete\n"); return -1; }
    glViewport(0, 0, FW, FH);
    fprintf(stderr, "gpu-contention: %s, GPU time from %s\n", glGetString(GL_RENDERER), G.fence ? "the native fence" : "glFinish");
    return 0;
}

/* a native fence's finish time, 0 if it has none */
static uint64_t fence_done(int fd) {
    struct pollfd p = { .fd = fd, .events = POLLIN };
    if (poll(&p, 1, 2000) <= 0) return 0;
    struct sync_fence_info fi[4];
    struct sync_file_info info;
    memset(&info, 0, sizeof info);
    memset(fi, 0, sizeof fi);
    uint64_t t = 0;
    if (!ioctl(fd, SYNC_IOC_FILE_INFO, &info) && info.num_fences > 0 && info.num_fences <= 4) {
        info.sync_fence_info = (uint64_t)(uintptr_t)fi;
        if (!ioctl(fd, SYNC_IOC_FILE_INFO, &info))
            for (uint32_t i = 0; i < info.num_fences; i++) if (fi[i].timestamp_ns > t) t = fi[i].timestamp_ns;
    }
    return t;
}

static void frame(int f, double *up_ms, double *gpu_ms) {
    uint64_t t0 = now_ns();
    glBindTexture(GL_TEXTURE_2D, G.tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, f & 63, UPW, UPH, GL_RGBA, GL_UNSIGNED_BYTE, G.px);
    uint64_t t1 = now_ns();
    static const GLfloat v[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glUseProgram(G.prog);
    glUniform1i(G.u_tex, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    uint64_t t2 = t1, done = 0, start = t1;
    if (G.fence) {
        EGLSyncKHR s = G.create_sync(G.dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
        glFlush();
        t2 = now_ns();
        int fd = s != EGL_NO_SYNC_KHR ? G.dup_fence(G.dpy, s) : -1;
        if (s != EGL_NO_SYNC_KHR) G.destroy_sync(G.dpy, s);
        if (fd >= 0) { done = fence_done(fd); close(fd); }
    }
    if (done > t2) start = t2;
    else { glFinish(); done = now_ns(); }               /* no fence, or one that was signalled from the start */
    *up_ms = (double)(t1 - t0) / 1e6;
    *gpu_ms = (double)(done - start) / 1e6;
}

static int read_temp(void) {
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    int t = -1;
    if (f) { if (fscanf(f, "%d", &t) != 1) t = -1; fclose(f); }
    return t;
}

static void run_phase(GcPhase ph, int frames, GcResult *r) {
    Worker w[MAX_WORKERS];
    memset(w, 0, sizeof w);
    atomic_store(&stop_workers, 0);
    uint64_t t_start = now_ns();           /* the workers count from when they start, their setup included */
    for (int i = 0; i < ph.workers; i++) {
        w[i].cpu = i;
        w[i].kind = ph.kind;
        pthread_create(&w[i].th, NULL, worker, &w[i]);
    }
    if (ph.workers) usleep(600000);        /* their buffers are touched and they are running */
    double *up = malloc(sizeof *up * (size_t)frames), *gpu = malloc(sizeof *gpu * (size_t)frames);
    uint64_t next = now_ns();
    for (int f = 0; f < frames; f++) {
        frame(f, &up[f], &gpu[f]);
        next += 16666667u;
        uint64_t n = now_ns();
        if (n < next) { struct timespec d = { 0, (long)(next - n) }; nanosleep(&d, NULL); }
        else next = n;
    }
    atomic_store(&stop_workers, 1);
    long long misses = 0;
    int counted = ph.workers > 0;
    for (int i = 0; i < ph.workers; i++) {
        pthread_join(w[i].th, NULL);
        if (w[i].misses < 0) counted = 0;
        else misses += w[i].misses;
    }
    double secs = (double)(now_ns() - t_start) / 1e9;
    *r = (GcResult){ .ph = ph, .frames = frames, .bus_mb_s = counted ? (double)misses * 64 / 1e6 / secs : -1, .temp_mc = read_temp() };
    r->gpu_p50 = gc_quantile(gpu, frames, 0.5);
    r->gpu_p99 = gc_quantile(gpu, frames, 0.99);
    r->up_p50 = gc_quantile(up, frames, 0.5);
    r->up_p99 = gc_quantile(up, frames, 0.99);
    free(up);
    free(gpu);
}

int main(int argc, char **argv) {
    const char *node = "/dev/dri/renderD128";
    int frames = 240, cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) { fprintf(stderr, "usage: gpu-contention [--node path] [--frames n] [--cores n]\n"); return 2; }
        if (!strcmp(argv[i], "--node")) node = argv[i + 1];
        else if (!strcmp(argv[i], "--frames")) frames = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--cores")) cores = atoi(argv[i + 1]);
        else { fprintf(stderr, "usage: gpu-contention [--node path] [--frames n] [--cores n]\n"); return 2; }
    }
    if (frames < 10 || frames > 100000 || cores < 1 || cores > MAX_WORKERS) { fprintf(stderr, "bad --frames or --cores\n"); return 2; }
    if (gl_setup(node)) return 1;
    GcPhase plan[64];
    GcResult res[64];
    int n = gc_plan(plan, 64, cores);
    char line[256];
    for (int i = 0; i < n; i++) {
        run_phase(plan[i], frames, &res[i]);
        if (gc_json(&res[i], line, sizeof line)) puts(line);
        fflush(stdout);
    }
    printf("{\"verdict\":\"%s\",\"renderer\":\"%s\",\"fence\":%s}\n", gc_verdict_name(gc_verdict(res, n)), glGetString(GL_RENDERER), G.fence ? "true" : "false");
    return 0;
}
