/* bgregion.h — a blur region (ext-background-effect-v1, bgeffect.c) as a few plain rectangles the shell can draw.
 * Pure, for tests/bgregion_test.c. The region is the app's add/subtract list (wlregion.h), in surface coordinates;
 * the result is clipped to the window geometry and relative to its corner. Regions are cut into bands on every edge
 * the rectangles have and merged back (a rounded panel's few steps stay a few rectangles). A region with more than
 * BGR_OPS rectangles, or one that needs more than the caller's max, becomes its bounding box: the blur still covers
 * what the app asked for, and a little more. */
#ifndef LORD_BGREGION_H
#define LORD_BGREGION_H

#include "wlregion.h"

#define BGR_OPS 64
#define BGR_EDGES (2 * BGR_OPS + 2)

static inline int bgr_cmp(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }

/* sorted unique values of v[0..n) */
static inline int bgr_uniq(int *v, int n) {
    qsort(v, (size_t)n, sizeof *v, bgr_cmp);
    int k = 0;
    for (int i = 0; i < n; i++) if (!k || v[k - 1] != v[i]) v[k++] = v[i];
    return k;
}

/* r clipped to the geometry (gx, gy, gw × gh): at most max rectangles (x, y, w, h, relative to gx, gy) in out. */
static inline int bgr_rects(const WlRgn *r, int gx, int gy, int gw, int gh, int (*out)[4], int max) {
    if (!r || !r->n || gw <= 0 || gh <= 0 || max <= 0) return 0;
    int x0 = gx, y0 = gy, x1 = gx + gw, y1 = gy + gh;
    if (r->n > BGR_OPS) goto bbox;
    {
        int xs[BGR_EDGES], ys[BGR_EDGES], nx = 0, ny = 0;
        xs[nx++] = x0; xs[nx++] = x1; ys[ny++] = y0; ys[ny++] = y1;
        for (int i = 0; i < r->n; i++) {
            const WlRgnOp *o = &r->op[i];
            int a = o->x < x0 ? x0 : o->x > x1 ? x1 : o->x, b = o->x + o->w < x0 ? x0 : o->x + o->w > x1 ? x1 : o->x + o->w;
            int c = o->y < y0 ? y0 : o->y > y1 ? y1 : o->y, d = o->y + o->h < y0 ? y0 : o->y + o->h > y1 ? y1 : o->y + o->h;
            xs[nx++] = a; xs[nx++] = b; ys[ny++] = c; ys[ny++] = d;
        }
        nx = bgr_uniq(xs, nx);
        ny = bgr_uniq(ys, ny);
        int n = 0;
        int open[BGR_EDGES], nopen = 0;     /* rects (indexes into out) that end at the current band's top */
        for (int j = 0; j + 1 < ny; j++) {
            double cy = (ys[j] + ys[j + 1]) / 2.0;
            int next[BGR_EDGES], nnext = 0;
            for (int i = 0; i + 1 < nx;) {
                if (!wlrgn_has(r, (xs[i] + xs[i + 1]) / 2.0, cy)) { i++; continue; }
                int s = i;
                while (i + 1 < nx && wlrgn_has(r, (xs[i] + xs[i + 1]) / 2.0, cy)) i++;
                int sx = xs[s], ex = xs[i];
                int k = -1;                 /* the same span right above: grows down */
                for (int q = 0; q < nopen; q++) {
                    int *o = out[open[q]];
                    if (o[0] == sx - gx && o[2] == ex - sx && o[1] + o[3] == ys[j] - gy) { k = open[q]; break; }
                }
                if (k < 0) {
                    if (n == max) goto bbox;
                    k = n++;
                    out[k][0] = sx - gx; out[k][1] = ys[j] - gy; out[k][2] = ex - sx; out[k][3] = 0;
                }
                out[k][3] += ys[j + 1] - ys[j];
                next[nnext++] = k;
            }
            memcpy(open, next, sizeof(int) * (size_t)nnext);
            nopen = nnext;
        }
        return n;
    }
bbox:;
    int bx0 = x1, by0 = y1, bx1 = x0, by1 = y0;
    for (int i = 0; i < r->n; i++) {
        const WlRgnOp *o = &r->op[i];
        if (o->sub) continue;
        if (o->x < bx0) bx0 = o->x;
        if (o->y < by0) by0 = o->y;
        if (o->x + o->w > bx1) bx1 = o->x + o->w;
        if (o->y + o->h > by1) by1 = o->y + o->h;
    }
    if (bx0 < x0) bx0 = x0;
    if (by0 < y0) by0 = y0;
    if (bx1 > x1) bx1 = x1;
    if (by1 > y1) by1 = y1;
    if (bx1 <= bx0 || by1 <= by0) return 0;
    out[0][0] = bx0 - gx; out[0][1] = by0 - gy; out[0][2] = bx1 - bx0; out[0][3] = by1 - by0;
    return 1;
}

#endif
