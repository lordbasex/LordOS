/* buftf.h — a Wayland surface's buffer transform and damage (wl_surface.set_buffer_transform, damage, damage_buffer;
 * wl.c, docs/WAYLAND.md "Frames, titles and damage"). Pure, header only: tests/buftf_test.c checks it.
 *
 * "Upright" is the buffer turned the way it is shown: uw × uh pixels (the buffer's w × h, swapped for a quarter turn).
 * wl.c keeps shared-memory content upright (copied turned), so everything after the copy ignores the transform; a
 * dmabuf is drawn from its own texture with its corners' texture coordinates turned instead. */
#ifndef LORD_WL_BUFTF_H
#define LORD_WL_BUFTF_H

#include <stdint.h>

/* wl_output.transform: 0 normal, 1 90, 2 180, 3 270, 4 flipped, 5 flipped 90, 6 flipped 180, 7 flipped 270 */
static inline int buftf_swaps(int tf) { return tf & 1; }

/* An upright point (ux, uy) of an upright size uw × uh → the same point in the buffer (continuous coordinates, as
 * weston's weston_transformed_coord and wlroots' wlr_fbox_transform with the inverse transform). */
static inline void buftf_point(int tf, float uw, float uh, float ux, float uy, float *bx, float *by) {
    switch (tf & 7) {
    default: *bx = ux; *by = uy; break;
    case 1: *bx = uy; *by = uw - ux; break;
    case 2: *bx = uw - ux; *by = uh - uy; break;
    case 3: *bx = uh - uy; *by = ux; break;
    case 4: *bx = uw - ux; *by = uy; break;
    case 5: *bx = uy; *by = ux; break;
    case 6: *bx = ux; *by = uh - uy; break;
    case 7: *bx = uh - uy; *by = uw - ux; break;
    }
}

/* The upright pixel (x, y) is the buffer's pixel (*bx, *by) (pixel centres through buftf_point). */
static inline void buftf_pixel(int tf, int uw, int uh, int x, int y, int *bx, int *by) {
    float fx, fy;
    buftf_point(tf, (float)uw, (float)uh, (float)x + 0.5f, (float)y + 0.5f, &fx, &fy);
    *bx = (int)fx;
    *by = (int)fy;
}

/* An upright copy of a 32-bit buffer (bw × bh, stride in pixels): dst is uw × uh (bh × bw for a quarter turn). */
static inline void buftf_copy(int tf, const uint32_t *src, int bw, int bh, int stride, uint32_t *dst, uint32_t or_bits) {
    int uw = buftf_swaps(tf) ? bh : bw, uh = buftf_swaps(tf) ? bw : bh;
    for (int y = 0; y < uh; y++)
        for (int x = 0; x < uw; x++) {
            int bx, by;
            buftf_pixel(tf, uw, uh, x, y, &bx, &by);
            dst[(size_t)y * uw + x] = src[(size_t)by * stride + bx] | or_bits;
        }
}

/* The four corners of an upright rect (x0, y0)–(x1, y1) of an upright size uw × uh, in the buffer: top-left, top-right,
 * bottom-left, bottom-right, as (x, y) pairs — a dmabuf texture's coordinates for that rect drawn upright. */
static inline void buftf_corners(int tf, float uw, float uh, float x0, float y0, float x1, float y1, float out[8]) {
    buftf_point(tf, uw, uh, x0, y0, &out[0], &out[1]);
    buftf_point(tf, uw, uh, x1, y0, &out[2], &out[3]);
    buftf_point(tf, uw, uh, x0, y1, &out[4], &out[5]);
    buftf_point(tf, uw, uh, x1, y1, &out[6], &out[7]);
}

/* A damage box of the surface (surface coordinates, x0 y0 x1 y1) → buffer pixels of an upright buffer of uw × uh:
 * through the viewport's crop (cx, cy, cw, ch, in buffer-scale units; cw <= 0 none) and destination (dw × dh; <= 0
 * none) and the buffer scale. One pixel more on each side when the viewport scales (the filter reads neighbours).
 * Clamped to the buffer; empty when it misses it. */
typedef struct { int x0, y0, x1, y1; } BufBox;
static inline BufBox buftf_surface_damage(float x0, float y0, float x1, float y1, int scale, int uw, int uh,
                                          float cx, float cy, float cw, float ch, int dw, int dh) {
    float b = (float)(scale > 0 ? scale : 1);
    if (cw <= 0 || ch <= 0) { cx = cy = 0; cw = (float)uw / b; ch = (float)uh / b; }
    float lw = dw > 0 ? (float)dw : cw, lh = dh > 0 ? (float)dh : ch;
    float kx = cw / lw, ky = ch / lh;
    int pad = kx != 1 || ky != 1 || cx != (float)(int)cx || cy != (float)(int)cy;
    float fx0 = (cx + x0 * kx) * b, fy0 = (cy + y0 * ky) * b, fx1 = (cx + x1 * kx) * b, fy1 = (cy + y1 * ky) * b;
    BufBox r = { (int)fx0 - pad, (int)fy0 - pad, (int)(fx1 + 0.999f) + pad, (int)(fy1 + 0.999f) + pad };
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > uw) r.x1 = uw;
    if (r.y1 > uh) r.y1 = uh;
    if (r.x1 <= r.x0 || r.y1 <= r.y0) r = (BufBox){ 0, 0, 0, 0 };
    return r;
}

/* The union of two boxes (an empty one adds nothing). */
static inline BufBox buftf_union(BufBox a, BufBox b) {
    if (a.x1 <= a.x0 || a.y1 <= a.y0) return b;
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return a;
    return (BufBox){ a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
}

#endif
