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
#include <unistd.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

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
        dmaread_free(im);
        CHECK(eglGetCurrentContext() == other, "%s: the other context isn't current after the free", name);
    }
    free(px);
    if (fd >= 0) close(fd);
    gbm_bo_destroy(bo);
    printf("  %s %dx%d: %s\n", name, w, h, fails ? "FAILED" : "ok");
}

int main(void) {
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
