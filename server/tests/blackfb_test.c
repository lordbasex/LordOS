/* blackfb_test.c — the black frame of a hand-off (src/remote/blackfb.c, GitHub #94): its dumb buffer is
 * cleared before it becomes a framebuffer, since radeon's comes from VRAM as it was, and nothing is
 * left behind when a step fails. ioctl, mmap and munmap are wrapped (-Wl,--wrap): a fake card whose
 * new dumb buffers are full of noise.
 *
 *   build/blackfb-test   (make -C server test runs it) */
#include "../src/remote/blackfb.h"

#include <drm/drm.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define FD 42
#define HANDLE 7
#define FB 99

static unsigned char *vram;                    /* the dumb buffer's memory, dirty when created */
static size_t vsize;
static int fail_map, fail_mmap, fail_addfb;
static int addfb, closed, mapped, unmapped, addfb_dirty;
static char order[64];                         /* C(reate) M(ap) m(map) u(nmap) A(ddfb) G(em close) */

static void step(char c) { size_t l = strlen(order); if (l + 1 < sizeof order) { order[l] = c; order[l + 1] = 0; } }
static int dirty(void) { for (size_t i = 0; i < vsize; i++) if (vram[i]) return 1; return 0; }

int __wrap_ioctl(int fd, unsigned long req, ...) {
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (fd != FD) return -1;
    if (req == DRM_IOCTL_MODE_CREATE_DUMB) {
        struct drm_mode_create_dumb *d = arg;
        step('C');
        d->handle = HANDLE;
        d->pitch = d->width * 4 + 64;          /* a pitch wider than the line, as drivers align it */
        d->size = (uint64_t)d->pitch * d->height;
        free(vram);
        vsize = (size_t)d->size;
        vram = malloc(vsize);
        for (size_t i = 0; i < vsize; i++) vram[i] = (unsigned char)(i * 131 + 17) | 1;
        return 0;
    }
    if (req == DRM_IOCTL_MODE_MAP_DUMB) {
        struct drm_mode_map_dumb *m = arg;
        step('M');
        if (fail_map || m->handle != HANDLE) return -1;
        m->offset = 0x10000000;
        return 0;
    }
    if (req == DRM_IOCTL_MODE_ADDFB) {
        struct drm_mode_fb_cmd *f = arg;
        step('A');
        addfb++;
        addfb_dirty = dirty();
        if (fail_addfb || f->handle != HANDLE || f->bpp != 32 || f->depth != 24) return -1;
        f->fb_id = FB;
        return 0;
    }
    if (req == DRM_IOCTL_GEM_CLOSE) {
        struct drm_gem_close *g = arg;
        step('G');
        if (g->handle == HANDLE) closed++;
        return 0;
    }
    return -1;
}

void *__wrap_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    (void)addr;
    step('m');
    if (fail_mmap || fd != FD || off != 0x10000000 || len != vsize || !(prot & PROT_WRITE) || !(flags & MAP_SHARED)) return MAP_FAILED;
    mapped++;
    return vram;
}

int __wrap_munmap(void *addr, size_t len) {
    step('u');
    if (addr == vram && len == vsize) unmapped++;
    return 0;
}

static void reset(void) {
    fail_map = fail_mmap = fail_addfb = 0;
    addfb = closed = mapped = unmapped = addfb_dirty = 0;
    order[0] = 0;
}

static int n, bad;
#define CHECK(name, cond) do { n++; if (!(cond)) { bad++; printf("✗ %s\n", name); } } while (0)

int main(void) {
    reset();
    uint32_t fb = black_fb(FD, 1366, 768);
    CHECK("a framebuffer comes back", fb == FB);
    CHECK("the whole buffer, pitch padding included, is black", vram && !dirty());
    CHECK("it was black before ADDFB", addfb == 1 && !addfb_dirty);
    CHECK("mapped once and unmapped", mapped == 1 && unmapped == 1);
    CHECK("the handle is closed (the framebuffer keeps the buffer)", closed == 1);
    CHECK("order: create, map, mmap, munmap, addfb, close", !strcmp(order, "CMmuAG"));

    reset();
    fail_map = 1;
    CHECK("MAP_DUMB fails: no framebuffer", black_fb(FD, 1920, 1080) == 0);
    CHECK("MAP_DUMB fails: noise never reaches ADDFB, the handle is closed", addfb == 0 && closed == 1);

    reset();
    fail_mmap = 1;
    CHECK("mmap fails: no framebuffer", black_fb(FD, 1920, 1080) == 0);
    CHECK("mmap fails: no ADDFB, the handle is closed", addfb == 0 && closed == 1 && unmapped == 0);

    reset();
    fail_addfb = 1;
    CHECK("ADDFB fails: no framebuffer", black_fb(FD, 800, 600) == 0);
    CHECK("ADDFB fails: unmapped and the handle closed", unmapped == 1 && closed == 1);

    reset();
    CHECK("an empty mode: nothing is created", black_fb(FD, 0, 768) == 0 && !order[0]);
    CHECK("CREATE_DUMB fails: nothing else is tried", black_fb(3, 640, 480) == 0 && !strcmp(order, ""));

    free(vram);
    printf("blackfb-test: %d/%d ok\n", n - bad, n);
    return bad ? 1 : 0;
}
