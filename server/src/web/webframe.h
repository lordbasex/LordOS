/* webframe.h — the bounds of a shared-memory frame from WebKit's web process (webshm.c, webback_platform.c, webwin.c).
 * The web process is untrusted: its width, height and stride come from a wl_shm buffer it made (legacy) or from
 * WPEPlatform's buffer (platform), so they are checked here before lord-web maps or copies anything, in 64-bit sizes
 * so that `w * 4` can't wrap. Pure: no Wayland or WebKit, tested by tests/webapp_test.c. */
#ifndef LORD_WEB_WEBFRAME_H
#define LORD_WEB_WEBFRAME_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* the largest frame side lord-web shows: far above any monitor's, far below where 4 * w * h nears INT32_MAX */
#define WEBFRAME_MAX_SIDE 16384

/* 1 when w x h pixels of 4 bytes with this stride is a frame lord-web may take: both sides in 1..WEBFRAME_MAX_SIDE,
 * stride at least w * 4 and stride * h no more than avail bytes (pass SIZE_MAX when the source's size was checked by
 * its owner, as libwayland-server does for a wl_shm buffer's pool) */
static inline int webframe_shm_ok(int w, int h, int stride, size_t avail) {
    if (w <= 0 || h <= 0 || w > WEBFRAME_MAX_SIDE || h > WEBFRAME_MAX_SIDE || stride <= 0) return 0;
    if ((uint64_t)stride < (uint64_t)w * 4u) return 0;
    if ((uint64_t)stride > (uint64_t)INT32_MAX) return 0;
    uint64_t need = (uint64_t)stride * (uint64_t)h;
    return need <= (uint64_t)avail;
}

/* copies a src_w x src_h frame (src_stride bytes a row) into a dst_w x dst_h buffer of dst_size bytes (dst_stride a
 * row): never more than either side's width, rows, stride or the destination's size, whatever the numbers say.
 * Returns the rows copied (0 when the numbers make no sense). */
static inline int webframe_copy(unsigned char *dst, size_t dst_size, int dst_w, int dst_h, int dst_stride,
                                const unsigned char *src, int src_w, int src_h, int src_stride) {
    if (!dst || !src || dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0 || dst_stride <= 0 || src_stride <= 0) return 0;
    size_t line = (size_t)(src_w < dst_w ? src_w : dst_w) * 4u;
    if (line > (size_t)dst_stride) line = (size_t)dst_stride;
    if (line > (size_t)src_stride) line = (size_t)src_stride;
    int rows = src_h < dst_h ? src_h : dst_h;
    if ((size_t)rows > dst_size / (size_t)dst_stride) rows = (int)(dst_size / (size_t)dst_stride);
    if (rows <= 0) return 0;
    if (src_stride == dst_stride && line == (size_t)dst_stride) {
        memcpy(dst, src, (size_t)dst_stride * (size_t)rows);
        return rows;
    }
    for (int y = 0; y < rows; y++) memcpy(dst + (size_t)y * (size_t)dst_stride, src + (size_t)y * (size_t)src_stride, line);
    return rows;
}

#endif
