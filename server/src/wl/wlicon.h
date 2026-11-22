/* wlicon.h — an app's own window icon (xdg-toplevel-icon-v1, wl.c; docs/WAYLAND.md "Window icons"). Header only:
 * tests/wlicon_test.c checks it.
 *
 * The client's pixels are never kept or handed on as they came: each buffer is copied at once into lord-server's own
 * memory, re-encoded from premultiplied ARGB words to straight RGBA bytes (what <Image pixels> takes), within fixed
 * limits: square, 1…WLICON_MAX px a side, and at most WLICON_BUDGET bytes of icons alive in the whole session (past it
 * a buffer is just not taken). Of an icon object's buffers one is kept, the closest to WLICON_WANT px. Refcounted:
 * an icon object and every toplevel it was set on hold it. */
#ifndef LORD_WL_WLICON_H
#define LORD_WL_WLICON_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WLICON_MAX 256                  /* px a side: larger buffers are not taken */
#define WLICON_WANT 128                 /* the size the Dock wants most (86 px magnified, on a 1.5x display) */
#define WLICON_BUDGET (16u << 20)       /* bytes of icons alive at once, every client together */

typedef struct { int refs, size, scale; uint8_t *rgba; } WlIcon;

static size_t wlicon_bytes;             /* alive now (one copy per program; tests reset it) */

static inline WlIcon *wlicon_ref(WlIcon *ic) { if (ic) ic->refs++; return ic; }
static inline void wlicon_unref(WlIcon *ic) {
    if (!ic || --ic->refs > 0) return;
    wlicon_bytes -= (size_t)ic->size * (size_t)ic->size * 4;
    free(ic->rgba);
    free(ic);
}

/* Is a new buffer (size px at scale) better than the kept one (have, have_scale; 0: none)? Within the limits, the one
 * whose device pixels (size; a scaled buffer's are already its pixels) are closest to WLICON_WANT, the larger on a tie. */
static inline int wlicon_better(int have, int have_scale, int size, int scale) {
    (void)have_scale; (void)scale;
    if (size <= 0 || size > WLICON_MAX) return 0;
    if (have <= 0) return 1;
    int dn = size > WLICON_WANT ? size - WLICON_WANT : WLICON_WANT - size;
    int dh = have > WLICON_WANT ? have - WLICON_WANT : WLICON_WANT - have;
    return dn < dh || (dn == dh && size > have);
}

/* A copy of a side × side shared-memory buffer (ARGB8888 premultiplied, or XRGB8888: opaque) as straight RGBA bytes,
 * stride in bytes; NULL past the limits or the budget, or without memory. */
static inline WlIcon *wlicon_from_argb(const void *data, int side, int stride, int scale, int opaque) {
    if (!data || side <= 0 || side > WLICON_MAX || stride < side * 4) return NULL;
    size_t n = (size_t)side * (size_t)side * 4;
    if (wlicon_bytes + n > WLICON_BUDGET) return NULL;
    WlIcon *ic = calloc(1, sizeof *ic);
    uint8_t *px = ic ? malloc(n) : NULL;
    if (!px) { free(ic); return NULL; }
    for (int y = 0; y < side; y++) {
        const uint8_t *row = (const uint8_t *)data + (size_t)y * (size_t)stride;
        for (int x = 0; x < side; x++) {
            uint32_t v;
            memcpy(&v, row + (size_t)x * 4, 4);
            uint32_t a = opaque ? 255 : v >> 24, r = v >> 16 & 255, g = v >> 8 & 255, b = v & 255;
            if (a && a < 255) {                     /* premultiplied → straight (a colour above its alpha is cut) */
                r = r >= a ? 255 : (r * 255 + a / 2) / a;
                g = g >= a ? 255 : (g * 255 + a / 2) / a;
                b = b >= a ? 255 : (b * 255 + a / 2) / a;
            } else if (!a) r = g = b = 0;
            uint8_t *o = px + ((size_t)y * (size_t)side + (size_t)x) * 4;
            o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)b; o[3] = (uint8_t)a;
        }
    }
    ic->refs = 1;
    ic->size = side;
    ic->scale = scale > 0 ? scale : 1;
    ic->rgba = px;
    wlicon_bytes += n;
    return ic;
}

/* An X app's _NET_WM_ICON image (xwm.c): w × h ARGB words with straight alpha, little endian as they came over the
 * X connection, copied into a square side = max(w, h) canvas (centred, transparent around it) as straight RGBA
 * bytes. NULL past the limits or the budget, or without memory. */
static inline WlIcon *wlicon_from_net(const uint8_t *le, int w, int h) {
    int side = w > h ? w : h;
    if (!le || w <= 0 || h <= 0 || side > WLICON_MAX) return NULL;
    size_t n = (size_t)side * (size_t)side * 4;
    if (wlicon_bytes + n > WLICON_BUDGET) return NULL;
    WlIcon *ic = calloc(1, sizeof *ic);
    uint8_t *px = ic ? calloc(1, n) : NULL;
    if (!px) { free(ic); return NULL; }
    int ox = (side - w) / 2, oy = (side - h) / 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *p = le + ((size_t)y * (size_t)w + (size_t)x) * 4;   /* B, G, R, A */
            uint8_t *o = px + ((size_t)(y + oy) * (size_t)side + (size_t)(x + ox)) * 4;
            o[3] = p[3];
            if (p[3]) { o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; }
        }
    ic->refs = 1;
    ic->size = side;
    ic->scale = 1;
    ic->rgba = px;
    wlicon_bytes += n;
    return ic;
}

#endif
