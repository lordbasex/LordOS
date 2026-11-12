/* blackfb.c — a black framebuffer, cleared by hand (blackfb.h). */
#include "blackfb.h"

#include <drm/drm.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

uint32_t black_fb(int fd, uint32_t w, uint32_t h) {
    struct drm_mode_create_dumb d = { .width = w, .height = h, .bpp = 32 };
    if (!w || !h || ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &d)) return 0;
    uint32_t fb = 0;
    /* not every driver zeroes a new dumb buffer (radeon's comes from VRAM as it was): clear it first */
    struct drm_mode_map_dumb m = { .handle = d.handle };
    void *p = MAP_FAILED;
    if (d.size && d.pitch && !ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m))
        p = mmap(NULL, (size_t)d.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
    if (p != MAP_FAILED) {
        memset(p, 0, (size_t)d.size);
        munmap(p, (size_t)d.size);
        struct drm_mode_fb_cmd f = { .width = d.width, .height = d.height, .pitch = d.pitch, .bpp = 32, .depth = 24, .handle = d.handle };
        if (!ioctl(fd, DRM_IOCTL_MODE_ADDFB, &f)) fb = f.fb_id;
    }
    struct drm_gem_close g = { .handle = d.handle };
    ioctl(fd, DRM_IOCTL_GEM_CLOSE, &g);
    return fb;
}
