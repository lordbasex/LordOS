/* gscissor.c — see gscissor.h */
#include "gscissor.h"

#include <string.h>

void gsc_reset(GpuScissor *g) { g->n = 0; }

long gsc_area(Rect r) { return r.x1 > r.x0 && r.y1 > r.y0 ? (long)(r.x1 - r.x0) * (r.y1 - r.y0) : 0; }

Rect gsc_join(Rect a, Rect b) {
    if (!gsc_area(a)) return gsc_area(b) ? b : (Rect){ 0, 0, 0, 0 };
    if (!gsc_area(b)) return a;
    return (Rect){ a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
}

Rect gsc_clip(Rect r, int W, int H) {
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > W) r.x1 = W;
    if (r.y1 > H) r.y1 = H;
    return gsc_area(r) ? r : (Rect){ 0, 0, 0, 0 };
}

Rect gsc_region(const GpuScissor *g, int age, Rect cur, int W, int H) {
    Rect all = { 0, 0, W, H };
    if (age <= 0 || age - 1 > g->n) return all;
    Rect u = gsc_clip(cur, W, H);
    for (int i = 0; i < age - 1; i++) u = gsc_join(u, gsc_clip(g->r[i], W, H));
    return u;
}

void gsc_push(GpuScissor *g, Rect cur) {
    memmove(g->r + 1, g->r, sizeof g->r[0] * (GSC_HIST - 1));
    g->r[0] = cur;
    if (g->n < GSC_HIST) g->n++;
}
