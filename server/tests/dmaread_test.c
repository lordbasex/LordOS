/* dmaread_test.c — the second EGL reader (src/gpu/dmaread.c): an Intel Y-tiled dmabuf (what a GVT-g machine's
 * Windows desktop hands QEMU) and a linear one (its UEFI screen), made with GBM on the Intel render node and filled
 * with a known pattern, read back whole, in parts and flipped, while another EGL context is current (it must be
 * current again afterwards). Skipped without an Intel render node. */
#include "gpu/dmaread.h"

#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint32_t pat(int x, int y) { return (uint32_t)((x * 7 + y) & 255) << 16 | (uint32_t)(y & 255) << 8 | (uint32_t)(x & 255); }

/* a w × h XRGB8888 buffer with that modifier, its X bytes 0; NULL if GBM can't make or map it */
static struct gbm_bo *make_bo(struct gbm_device *g, int w, int h, uint64_t mod) {
    struct gbm_bo *bo = mod == DRM_FORMAT_MOD_LINEAR
        ? gbm_bo_create(g, (uint32_t)w, (uint32_t)h, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR)
        : gbm_bo_create_with_modifiers(g, (uint32_t)w, (uint32_t)h, GBM_FORMAT_XRGB8888, &mod, 1);
    if (!bo) return NULL;
    uint32_t stride = 0;
    void *data = NULL;
    uint8_t *m = gbm_bo_map(bo, 0, 0, (uint32_t)w, (uint32_t)h, GBM_BO_TRANSFER_WRITE, &stride, &data);
    if (!m) { gbm_bo_destroy(bo); return NULL; }
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(m + (size_t)y * stride);
        for (int x = 0; x < w; x++) row[x] = pat(x, y);
    }
    gbm_bo_unmap(bo, data);
    return bo;
}

/* rows [y, y + n) of px against the pattern (flip: the buffer's rows bottom up) */
static int rows_ok(const uint8_t *px, int w, int h, int y, int n, int flip) {
    for (int r = y; r < y + n; r++) {
        int sy = flip ? h - 1 - r : r;
        for (int x = 0; x < w; x++) {
            const uint8_t *p = px + ((size_t)r * (size_t)w + (size_t)x) * 4;
            uint32_t want = pat(x, sy);
            if (p[0] != (want & 255) || p[1] != (want >> 8 & 255) || p[2] != (want >> 16 & 255) || p[3] != 255) {
                fprintf(stderr, "  row %d x %d: %02x %02x %02x %02x, want %02x %02x %02x ff\n", r, x, p[0], p[1], p[2], p[3],
                        want & 255, want >> 8 & 255, want >> 16 & 255);
                return 0;
            }
        }
    }
    return 1;
}

/* the desktop's GPU (another vendor's render node, the AMD card beside the iGPU), for the linear copies' import */
static EGLDisplay amd_dpy = EGL_NO_DISPLAY;
static EGLContext amd_ctx = EGL_NO_CONTEXT;
static PFNEGLCREATEIMAGEKHRPROC amd_create;
static PFNEGLDESTROYIMAGEKHRPROC amd_destroy;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC amd_target;
static void amd_open(void) {
    for (int n = 128; n < 136; n++) {
        char node[32], p[64], v[16] = "";
        snprintf(node, sizeof node, "/dev/dri/renderD%d", n);
        snprintf(p, sizeof p, "/sys/class/drm/renderD%d/device/vendor", n);
        FILE *f = fopen(p, "re");
        if (!f) continue;
        if (!fgets(v, sizeof v, f)) v[0] = 0;
        fclose(f);
        if (!strncmp(v, "0x8086", 6)) continue;
        int fd = open(node, O_RDWR | O_CLOEXEC);
        struct gbm_device *g = fd >= 0 ? gbm_create_device(fd) : NULL;
        PFNEGLGETPLATFORMDISPLAYEXTPROC gd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (!g || !gd) { if (fd >= 0) close(fd); continue; }
        amd_dpy = gd(EGL_PLATFORM_GBM_KHR, g, NULL);
        static const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE }, xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        EGLConfig cfg;
        EGLint k = 0;
        if (amd_dpy != EGL_NO_DISPLAY && eglInitialize(amd_dpy, NULL, NULL) && eglBindAPI(EGL_OPENGL_ES_API) && eglChooseConfig(amd_dpy, ca, &cfg, 1, &k) && k == 1)
            amd_ctx = eglCreateContext(amd_dpy, cfg, EGL_NO_CONTEXT, xa);
        amd_create = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        amd_destroy = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        amd_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        if (amd_ctx != EGL_NO_CONTEXT) { printf("dmaread-test: the other GPU: %s\n", node); return; }
    }
}

/* the linear copy of the picture read on the other GPU (as gpu.c imports it): 0 equal, -1 not possible, 1 differs */
static int amd_check(int fd, int stride, int w, int h, int flip) {
    if (amd_ctx == EGL_NO_CONTEXT || !amd_create) return -1;
    EGLDisplay od = eglGetCurrentDisplay();
    EGLContext oc = eglGetCurrentContext();
    EGLSurface dr = eglGetCurrentSurface(EGL_DRAW), rd = eglGetCurrentSurface(EGL_READ);
    if (!eglMakeCurrent(amd_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, amd_ctx)) return -1;
    EGLint at[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888, EGL_DMA_BUF_PLANE0_FD_EXT, fd,
                    EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
                    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0, EGL_NONE };
    EGLImageKHR img = amd_create(amd_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, at);
    int r = -1;
    if (img != EGL_NO_IMAGE_KHR) {
        GLuint t, fb;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        amd_target(GL_TEXTURE_2D, (GLeglImageOES)img);
        glGenFramebuffers(1, &fb);
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
        uint8_t *px = malloc((size_t)w * h * 4);
        if (px && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
            r = 0;
            /* the copy's memory row r is the picture's row r (top first): GL's row 0 is the buffer's first */
            for (int y = 0; y < h && !r; y++) for (int x = 0; x < w; x++) {
                const uint8_t *p = px + ((size_t)y * w + x) * 4;
                uint32_t want = pat(x, flip ? h - 1 - y : y);
                if (p[0] != (want >> 16 & 255) || p[1] != (want >> 8 & 255) || p[2] != (want & 255)) {
                    fprintf(stderr, "  copy row %d x %d: %02x %02x %02x\n", y, x, p[0], p[1], p[2]);
                    r = 1;
                    break;
                }
            }
        }
        free(px);
        glDeleteFramebuffers(1, &fb);
        glDeleteTextures(1, &t);
        amd_destroy(amd_dpy, img);
    }
    if (oc != EGL_NO_CONTEXT) eglMakeCurrent(od, dr, rd, oc);
    else eglMakeCurrent(amd_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return r;
}

static int wait_fd(int fd) {
    struct pollfd p = { fd, POLLIN, 0 };
    return poll(&p, 1, 2000) == 1 ? 0 : -1;
}

/* linear copies: made, filled on the reader's GPU, read on the other GPU; the change test against the copy shown */
static void copies(DmaImg *im, struct gbm_bo *bo, int w, int h, const char *name, EGLContext other) {
    (void)bo;
    DmaOut *a = dmaread_out_new(im), *b = dmaread_out_new(im);
    CHECK(a && b, "%s: linear copies not made", name);
    if (!a || !b) { dmaread_out_free(a); dmaread_out_free(b); return; }
    int sa = 0, fa = dmaread_out_fd(a, &sa), sb = 0, fb = dmaread_out_fd(b, &sb);
    CHECK(fa >= 0 && fb >= 0 && sa % 256 == 0 && sa >= w * 4, "%s: copy's dmabuf %d, stride %d", name, fa, sa);
    CHECK(!dmaread_copy(im, a, 0, h, 0, NULL), "%s: copy failed", name);
    CHECK(eglGetCurrentContext() == other, "%s: the other context isn't current after the copy", name);
    CHECK(!wait_fd(fa), "%s: the copy's fence never signalled", name);
    CHECK(dmaread_changed(a) == -1, "%s: a copy without prev has a change answer", name);
    int r = amd_check(fa, sa, w, h, 0);
    if (r < 0) printf("  %s: copy not checked on another GPU (none, or it refused the buffer)\n", name);
    else CHECK(r == 0, "%s: the copy differs on the other GPU", name);
    /* b from the same picture, tested against a: the same */
    CHECK(!dmaread_copy(im, b, 0, h, 0, a), "%s: second copy failed", name);
    CHECK(!wait_fd(fb), "%s: the second copy's fence never signalled", name);
    int c = dmaread_changed(b);
    CHECK(c == 0 || c == -1, "%s: the same picture counted as changed (%d)", name, c);
    if (c == -1) printf("  %s: no change answer on this GPU\n", name);
    /* flipped against a: changed (the pattern isn't symmetric) */
    CHECK(!dmaread_copy(im, b, 0, h, 1, a), "%s: flipped copy failed", name);
    CHECK(!wait_fd(fb), "%s: the flipped copy's fence never signalled", name);
    c = dmaread_changed(b);
    CHECK(c == 1 || c == -1, "%s: a different picture counted as the same", name);
    r = amd_check(fb, sb, w, h, 1);
    if (r >= 0) CHECK(r == 0, "%s: the flipped copy differs on the other GPU", name);
    /* rows past the end, another size: refused */
    CHECK(dmaread_copy(im, a, h - 1, 2, 0, NULL) == -1, "%s: copy past the end accepted", name);
    dmaread_out_free(a);
    dmaread_out_free(b);
    CHECK(eglGetCurrentContext() == other, "%s: the other context isn't current after the copies", name);
}

static void one(struct gbm_device *g, const char *node, uint64_t mod, const char *name, EGLContext other) {
    const int w = 1000, h = 300;        /* a width no tile divides */
    struct gbm_bo *bo = make_bo(g, w, h, mod);
    if (!bo) { printf("  %s: skipped: GBM can't make or map it\n", name); return; }
    CHECK(gbm_bo_get_modifier(bo) == mod, "%s: GBM gave modifier %llx", name, (unsigned long long)gbm_bo_get_modifier(bo));
    int fd = gbm_bo_get_fd(bo);
    CHECK(fd >= 0, "%s: no dmabuf", name);
    uint8_t *px = calloc((size_t)w * h, 4);
    DmaImg *im = fd >= 0 && px ? dmaread_import(node, fd, w, h, (int)gbm_bo_get_stride(bo), DRM_FORMAT_XRGB8888, mod) : NULL;
    CHECK(im, "%s: not imported", name);
    if (im) {
        CHECK(!dmaread_rows(im, px, 0, h, 0), "%s: whole read failed", name);
        CHECK(rows_ok(px, w, h, 0, h, 0), "%s: whole picture differs", name);
        CHECK(eglGetCurrentContext() == other, "%s: the other context isn't current after the read", name);
        memset(px, 0, (size_t)w * h * 4);
        CHECK(!dmaread_rows(im, px, 17, 101, 1), "%s: flipped rows failed", name);
        CHECK(rows_ok(px, w, h, 17, 101, 1), "%s: flipped rows differ", name);
        CHECK(px[16 * w * 4 + 3] == 0 && px[118 * w * 4 + 3] == 0, "%s: rows outside [17, 118) written", name);
        CHECK(!dmaread_rows(im, px, h - 1, 1, 0) && rows_ok(px, w, h, h - 1, 1, 0), "%s: last row", name);
        CHECK(dmaread_rows(im, px, h - 1, 2, 0) == -1, "%s: rows past the end accepted", name);
        /* the change test: the same picture read again with a key is "the same" and leaves px alone; a changed
         * buffer is read */
        static int key;
        memset(px, 0, (size_t)w * h * 4);
        CHECK(dmaread_rows_new(im, px, 0, h, 0, &key) == 1 && rows_ok(px, w, h, 0, h, 0), "%s: keyed read", name);
        memset(px, 0, (size_t)w * h * 4);
        int r = dmaread_rows_new(im, px, 0, h, 0, &key);
        CHECK(r == 0 || r == 1, "%s: keyed read again failed", name);
        if (r == 0) CHECK(px[3] == 0, "%s: an unchanged read wrote px", name);
        else printf("  %s: no change test on this GPU\n", name);
        CHECK(dmaread_rows_new(im, px, 0, h, 0, &fails) == 1, "%s: another key's picture counted as the same", name);
        CHECK(eglGetCurrentContext() == other, "%s: the other context isn't current after the keyed reads", name);
        copies(im, bo, w, h, name, other);
        dmaread_free(im);
        CHECK(eglGetCurrentContext() == other, "%s: the other context isn't current after the free", name);
    }
    free(px);
    if (fd >= 0) close(fd);
    gbm_bo_destroy(bo);
    printf("  %s %dx%d: %s\n", name, w, h, fails ? "FAILED" : "ok");
}

/* ---- `dmaread-test bench [seconds]`: what lord-server's main thread spends per frame on a vGPU's 1920x1080 Y-tiled
 * picture at QEMU's vfio refresh (33 frames a second), before and after (docs/MACHINES-GPU.md); not run by make test ---- */
#include <sys/resource.h>
#include <time.h>
static double cpu_ms(void) {
    struct rusage ru;
    getrusage(RUSAGE_THREAD, &ru);
    return ru.ru_utime.tv_sec * 1e3 + ru.ru_utime.tv_usec / 1e3 + ru.ru_stime.tv_sec * 1e3 + ru.ru_stime.tv_usec / 1e3;
}
static double wall_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static void sleep_until(double t) {
    double now = wall_ms();
    if (t <= now) return;
    long long ns = (long long)((t - now) * 1e6);
    struct timespec ts = { (time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL) };
    nanosleep(&ts, NULL);
}
/* the old CPU scaling of mview.c (a bilinear float loop per pixel) for a 1920x1080 picture in a 1000x562 box */
static uint32_t *scaled;
static void old_scale(const uint32_t *px, int w, int h) {
    const int vw = 1000, vh = 562;
    if (!scaled) scaled = malloc((size_t)vw * vh * 4);
    float inv = (float)w / vw;
    for (int j = 0; j < vh; j++) for (int i = 0; i < vw; i++) {
        float u = (i + 0.5f) * inv - 0.5f, v = (j + 0.5f) * inv - 0.5f;
        if (u < 0) u = 0;
        if (v < 0) v = 0;
        int x0 = (int)u, y0 = (int)v, x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
        uint32_t fx = (uint32_t)((u - x0) * 256), fy = (uint32_t)((v - y0) * 256), o = 0;
        uint32_t a = px[(size_t)y0 * w + x0], b = px[(size_t)y0 * w + x1], c = px[(size_t)y1 * w + x0], d = px[(size_t)y1 * w + x1];
        for (int k = 0; k < 32; k += 8) {
            uint32_t top = ((a >> k) & 255) * (256 - fx) + ((b >> k) & 255) * fx, bot = ((c >> k) & 255) * (256 - fx) + ((d >> k) & 255) * fx;
            o |= ((top * (256 - fy) + bot * fy + 32768) >> 16) << k;
        }
        scaled[(size_t)j * vw + i] = o;
    }
}
static void bench(int secs) {
    const char *node = dmaread_intel_node();
    if (!node) { printf("bench: needs an Intel render node\n"); return; }
    int fd = open(node, O_RDWR | O_CLOEXEC);
    struct gbm_device *g = fd >= 0 ? gbm_create_device(fd) : NULL;
    if (!g) { printf("bench: no GBM\n"); return; }
    amd_open();
    const int w = 1920, h = 1080, n = secs * 33;
    struct gbm_bo *bo[2] = { make_bo(g, w, h, I915_FORMAT_MOD_Y_TILED), NULL };
    /* a second picture, different in one corner (a clock that changed) */
    bo[1] = make_bo(g, w, h, I915_FORMAT_MOD_Y_TILED);
    if (!bo[0] || !bo[1]) { printf("bench: no Y-tiled buffers\n"); return; }
    {
        uint32_t stride = 0;
        void *data = NULL;
        uint8_t *m = gbm_bo_map(bo[1], 1800, 1040, 100, 30, GBM_BO_TRANSFER_WRITE, &stride, &data);
        if (m) { for (int y = 0; y < 30; y++) memset(m + (size_t)y * stride, 0x55, 400); gbm_bo_unmap(bo[1], data); }
    }
    int bfd[2] = { gbm_bo_get_fd(bo[0]), gbm_bo_get_fd(bo[1]) };
    DmaImg *im[2];
    for (int i = 0; i < 2; i++) im[i] = dmaread_import(node, bfd[i], w, h, (int)gbm_bo_get_stride(bo[i]), DRM_FORMAT_XRGB8888, I915_FORMAT_MOD_Y_TILED);
    if (!im[0] || !im[1]) { printf("bench: not imported\n"); return; }
    uint8_t *px = malloc((size_t)w * h * 4);
    static int key;
    /* the desktop's GPU: a texture the picture is uploaded into each frame, as the CPU picture was (gpu.c's UI texture) */
    GLuint up = 0;
    EGLDisplay od = eglGetCurrentDisplay();
    (void)od;
    printf("bench: %s, %dx%d Y-tiled, %d frames at 33 a second per case; main-thread CPU (user+system) per frame and %% of a core\n", node, w, h, n);
    for (int cs = 0; cs < 5; cs++) {
        const char *name[] = { "before: readback + CPU scale + upload, every frame",
                               "after, CPU path (LORD_GPU=0): readback with the change test, picture unchanged",
                               "after, CPU path: picture changing every frame",
                               "after, GPU path: linear copy + change test, picture unchanged",
                               "after, GPU path: picture changing every frame" };
        DmaOut *o[2] = { NULL, NULL };
        if (cs >= 3) { o[0] = dmaread_out_new(im[0]); o[1] = dmaread_out_new(im[0]); if (!o[0] || !o[1]) { printf("  %s: no copies\n", name[cs]); continue; } }
        int shown = -1, same = 0;
        double c0 = cpu_ms(), t0 = wall_ms(), worst = 0;
        for (int f = 0; f < n; f++) {
            sleep_until(t0 + f * (1000.0 / 33));
            double f0 = cpu_ms();
            DmaImg *src = im[(cs == 2 || cs == 4) ? f & 1 : 0];
            if (cs == 0) {
                dmaread_rows(src, px, 0, h, 0);
                old_scale((const uint32_t *)px, w, h);
                if (amd_ctx != EGL_NO_CONTEXT) {
                    EGLContext oc = eglGetCurrentContext();
                    EGLDisplay odp = eglGetCurrentDisplay();
                    eglMakeCurrent(amd_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, amd_ctx);
                    if (!up) { glGenTextures(1, &up); glBindTexture(GL_TEXTURE_2D, up); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1000, 562, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL); }
                    glBindTexture(GL_TEXTURE_2D, up);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1000, 562, GL_RGBA, GL_UNSIGNED_BYTE, scaled);
                    glFlush();
                    if (oc != EGL_NO_CONTEXT) eglMakeCurrent(odp, EGL_NO_SURFACE, EGL_NO_SURFACE, oc);
                    else eglMakeCurrent(amd_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                }
            } else if (cs <= 2) {
                if (dmaread_rows_new(src, px, 0, h, 0, &key) == 0) same++;
            } else {
                int k = shown == 0 ? 1 : 0;
                dmaread_copy(src, o[k], 0, h, 0, shown >= 0 ? o[shown] : NULL);
                /* lord-server polls the copy's fence in its loop; here: wait for it outside the CPU count */
                double p0 = cpu_ms();
                wait_fd(dmaread_out_fd(o[k], NULL));
                double p1 = cpu_ms();
                f0 += p1 - p0;
                if (dmaread_changed(o[k]) == 0) same++;
                shown = k;
            }
            double d = cpu_ms() - f0;
            if (d > worst) worst = d;
        }
        double c = cpu_ms() - c0, t = wall_ms() - t0;
        printf("  %-80s %6.2f ms/frame (max %5.2f), %5.1f %% of a core; %d of %d frames found unchanged\n", name[cs], c / n, worst, 100 * c / t, same, n);
        dmaread_out_free(o[0]);
        dmaread_out_free(o[1]);
    }
    free(px);
    free(scaled);
    for (int i = 0; i < 2; i++) { dmaread_free(im[i]); close(bfd[i]); gbm_bo_destroy(bo[i]); }
    dmaread_close_all();
    gbm_device_destroy(g);
    close(fd);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "bench")) { bench(argc > 2 ? atoi(argv[2]) : 10); return 0; }
    /* the parts that need no GPU */
    CHECK(dmaread_mod_intel(I915_FORMAT_MOD_Y_TILED), "Y-tiled is Intel's");
    CHECK(dmaread_mod_intel(I915_FORMAT_MOD_X_TILED), "X-tiled is Intel's");
    CHECK(!dmaread_mod_intel(DRM_FORMAT_MOD_LINEAR), "linear isn't");
    CHECK(!dmaread_mod_intel(DRM_FORMAT_MOD_INVALID), "invalid isn't");
    CHECK(!dmaread_mod_intel(AMD_FMT_MOD | AMD_FMT_MOD_SET(TILE, AMD_FMT_MOD_TILE_GFX9_64K_S)), "AMD's isn't");
    CHECK(dmaread_node_valid("/dev/dri/renderD128") && dmaread_node_valid("/dev/dri/renderD1023"), "render nodes");
    CHECK(!dmaread_node_valid("/dev/dri/card0") && !dmaread_node_valid("/dev/dri/renderD12") && !dmaread_node_valid("/dev/dri/renderD128/..")
          && !dmaread_node_valid("/dev/dri/renderD1024") && !dmaread_node_valid("/dev/dri/renderD12x") && !dmaread_node_valid(NULL)
          && !dmaread_node_valid("/dev/dri/../renderD128"), "bad nodes refused");
    CHECK(!dmaread_import("/dev/dri/card0", 0, 16, 16, 64, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR), "a card node imports");
    if (fails) return 1;

    const char *node = dmaread_intel_node();
    if (!node) { printf("dmaread-test: skipped: needs an Intel render node\n"); return 0; }
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) { printf("dmaread-test: skipped: needs access to %s\n", node); return 0; }
    struct gbm_device *g = gbm_create_device(fd);
    if (!g) { printf("dmaread-test: skipped: needs GBM on %s\n", node); close(fd); return 0; }
    printf("dmaread-test: %s\n", node);
    amd_open();

    /* another context current, as gpu.c's is in lord-server: its own GBM device and EGL display on the same node */
    EGLDisplay od = EGL_NO_DISPLAY;
    EGLContext oc = EGL_NO_CONTEXT;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    int ofd = open(node, O_RDWR | O_CLOEXEC);
    struct gbm_device *og = ofd >= 0 ? gbm_create_device(ofd) : NULL;
    if (og && get_display) od = get_display(EGL_PLATFORM_GBM_KHR, og, NULL);
    if (od != EGL_NO_DISPLAY && eglInitialize(od, NULL, NULL) && eglBindAPI(EGL_OPENGL_ES_API)) {
        static const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE }, xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        EGLConfig cfg;
        EGLint n = 0;
        if (eglChooseConfig(od, ca, &cfg, 1, &n) && n == 1) oc = eglCreateContext(od, cfg, EGL_NO_CONTEXT, xa);
        if (oc != EGL_NO_CONTEXT && !eglMakeCurrent(od, EGL_NO_SURFACE, EGL_NO_SURFACE, oc)) oc = EGL_NO_CONTEXT;
    }
    CHECK(oc != EGL_NO_CONTEXT, "the other context couldn't be made");

    one(g, node, I915_FORMAT_MOD_Y_TILED, "Y-tiled", oc);
    one(g, node, DRM_FORMAT_MOD_LINEAR, "linear", oc);
    /* and with nothing current: nothing is left current */
    eglMakeCurrent(od, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    one(g, node, I915_FORMAT_MOD_Y_TILED, "Y-tiled, nothing current", EGL_NO_CONTEXT);

    dmaread_close_all();
    if (oc != EGL_NO_CONTEXT) eglDestroyContext(od, oc);
    if (od != EGL_NO_DISPLAY) eglTerminate(od);
    if (og) gbm_device_destroy(og);
    if (ofd >= 0) close(ofd);
    gbm_device_destroy(g);
    close(fd);
    printf("dmaread-test: %s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
