/* drmcard.h — which /dev/dri/cardN is the display (lord-server and lord-sessiond).
 *
 * On a PC it is usually card0, but not always: with simpledrm at boot, amdgpu/i915 show up
 * as card1. On ARM boards (Raspberry Pi 4/5 and many others) the display and the 3D GPU are
 * two devices: on the Pi 5, v3d (render only, no connectors) may end up as card0 and vc4
 * (the one with HDMI) as card1. That is why the first card with connectors and
 * CRTCs is chosen, and among those, the one with a monitor plugged in. LORD_CARD=/dev/dri/cardN forces it.
 *
 * Only kernel ioctls (no libdrm); a dependency-free header so that lord-sessiond can use it
 * too, since it does not link kms.c. See docs/RASPBERRY.md. */
#ifndef LORD_DRMCARD_H
#define LORD_DRMCARD_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#ifdef LORD_LIBSEAT
#include "platform/seat.h"
#endif

/* 2 = has a connected connector, 1 = has connectors (none connected), 0 = not KMS. */
static inline int drm_card_score(int fd) {
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof res);
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_connectors || !res.count_crtcs) return 0;
    uint32_t n = res.count_connectors > 64 ? 64 : res.count_connectors;
    uint32_t ids[64];
    struct drm_mode_card_res r2;
    memset(&r2, 0, sizeof r2);
    r2.connector_id_ptr = (uintptr_t)ids;
    r2.count_connectors = n;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2)) return 1;
    if (r2.count_connectors < n) n = r2.count_connectors;
    for (uint32_t i = 0; i < n; i++) {
        struct drm_mode_get_connector c;
        memset(&c, 0, sizeof c);
        c.connector_id = ids[i];
        if (!ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) && c.connection == 1) return 2;
    }
    return 1;
}

/* Writes the display card's path to out. 0 if it found one with KMS; -1 otherwise
 * (out stays /dev/dri/card0, as before). */
static inline int drm_pick_card(char *out, size_t size) {
    const char *env = getenv("LORD_CARD");
    if (env && !strncmp(env, "/dev/dri/card", 13) && !strstr(env, "..")) {
        snprintf(out, size, "%s", env);
        return 0;
    }
    int best = 0, best_i = -1;
    for (int i = 0; i < 16 && best < 2; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/dri/card%d", i);
        if (access(p, F_OK)) continue;
#ifdef LORD_LIBSEAT
        int fd = seat_device_open(p, O_RDWR | O_CLOEXEC);
#else
        int fd = open(p, O_RDWR | O_CLOEXEC);
#endif
        if (fd < 0) continue;
        int s = drm_card_score(fd);
#ifdef LORD_LIBSEAT
        seat_device_close(fd);
#else
        close(fd);
#endif
        if (s > best) { best = s; best_i = i; }
    }
    snprintf(out, size, "/dev/dri/card%d", best_i < 0 ? 0 : best_i);
    return best_i < 0 ? -1 : 0;
}

#endif
