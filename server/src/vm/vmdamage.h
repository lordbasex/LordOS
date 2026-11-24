/* vmdamage.h — what changed in a machine's picture since the last frame, as a few rectangles (docs/MACHINES-GPU.md,
 * Part 1 step 4). lord-vmdisplay gathers QEMU's updates here and sends each rectangle as its own VD_FRAME (vmproto.h),
 * so a clock in one corner and a moving square in another are two small boxes, not one box over both.
 *
 * Rules: a rectangle that overlaps or touches one already kept (within VMDMG_NEAR pixels) is merged into it, and the
 * merge repeats while the grown box reaches another; when all VMDMG_MAX places are taken, the two whose union adds the
 * least area are merged to make room. Merging only ever grows a box, so the result always covers every rectangle that
 * went in. Coordinates are checked by the caller (rect_ok in vmdisplay.c); the arithmetic is in 64 bits anyway, so a
 * picture of up to VD_MAX_DIM on a side never overflows. Pure: no I/O, tested by vmdamage-test. */
#ifndef LORDOS_VMDAMAGE_H
#define LORDOS_VMDAMAGE_H

#include <stdint.h>

#define VMDMG_MAX  8        /* rectangles per frame at most (VD_FRAME f counts down from VMDMG_MAX - 1) */
#define VMDMG_NEAR 8        /* boxes closer than this are one box (fewer copies and messages for nearly no area) */

typedef struct { int x0, y0, x1, y1; } VmRect;
typedef struct { int n; VmRect r[VMDMG_MAX]; } VmDamage;

static inline int64_t vmdmg_area(VmRect a) { return (int64_t)(a.x1 - a.x0) * (int64_t)(a.y1 - a.y0); }
static inline VmRect vmdmg_union(VmRect a, VmRect b) {
    return (VmRect){ a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
}
/* overlapping or within VMDMG_NEAR of each other */
static inline int vmdmg_near(VmRect a, VmRect b) {
    return (int64_t)a.x0 <= (int64_t)b.x1 + VMDMG_NEAR && (int64_t)b.x0 <= (int64_t)a.x1 + VMDMG_NEAR &&
           (int64_t)a.y0 <= (int64_t)b.y1 + VMDMG_NEAR && (int64_t)b.y0 <= (int64_t)a.y1 + VMDMG_NEAR;
}

static inline void vmdmg_clear(VmDamage *d) { d->n = 0; }

/* r[i] grew: every other box it now reaches is merged into it, until none is */
static inline void vmdmg_settle(VmDamage *d, int i) {
    for (int again = 1; again;) {
        again = 0;
        for (int j = 0; j < d->n; j++) {
            if (j == i || !vmdmg_near(d->r[i], d->r[j])) continue;
            d->r[i] = vmdmg_union(d->r[i], d->r[j]);
            d->r[j] = d->r[--d->n];
            if (i == d->n) i = j;           /* r[i] was the last one: it moved into j's place */
            again = 1;
            break;
        }
    }
}

/* (x, y, w, h) changed; w, h > 0 */
static inline void vmdmg_add(VmDamage *d, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    VmRect a = { x, y, x + w, y + h };
    for (int i = 0; i < d->n; i++)
        if (vmdmg_near(d->r[i], a)) { d->r[i] = vmdmg_union(d->r[i], a); vmdmg_settle(d, i); return; }
    if (d->n < VMDMG_MAX) { d->r[d->n++] = a; return; }
    /* full: the new one joins the box it costs least to grow, or two kept boxes join, whichever adds less */
    int bi = -1, bj = -1;
    int64_t best = INT64_MAX;
    for (int i = 0; i < d->n; i++) {
        int64_t c = vmdmg_area(vmdmg_union(d->r[i], a)) - vmdmg_area(d->r[i]) - vmdmg_area(a);
        if (c < best) { best = c; bi = i; bj = -1; }
        for (int j = i + 1; j < d->n; j++) {
            int64_t k = vmdmg_area(vmdmg_union(d->r[i], d->r[j])) - vmdmg_area(d->r[i]) - vmdmg_area(d->r[j]);
            if (k < best) { best = k; bi = i; bj = j; }
        }
    }
    if (bj < 0) { d->r[bi] = vmdmg_union(d->r[bi], a); vmdmg_settle(d, bi); return; }
    d->r[bi] = vmdmg_union(d->r[bi], d->r[bj]);
    d->r[bj] = d->r[--d->n];
    if (bi == d->n) bi = bj;
    vmdmg_settle(d, bi);
    d->r[d->n++] = a;
    vmdmg_settle(d, d->n - 1);
}

/* the boxes kept inside a w × h picture (an update before a resize may reach past the new size); empty ones dropped */
static inline void vmdmg_clip(VmDamage *d, int w, int h) {
    int k = 0;
    for (int i = 0; i < d->n; i++) {
        VmRect r = d->r[i];
        if (r.x0 < 0) r.x0 = 0;
        if (r.y0 < 0) r.y0 = 0;
        if (r.x1 > w) r.x1 = w;
        if (r.y1 > h) r.y1 = h;
        if (r.x1 > r.x0 && r.y1 > r.y0) d->r[k++] = r;
    }
    d->n = k;
}

#endif
