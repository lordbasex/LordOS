/* gband.h — which rows each of a vGPU picture's two linear copies lacks (ui/js_vm.c, gout; docs/MACHINES-GPU.md, "A
 * vGPU's picture without a readback"). Each frame is copied into the copy not shown, and only the rows that copy lacks
 * are drawn: the frame's changed band plus whatever the copy missed while the other one was shown. Pure; tests in
 * tests/gband_test.c.
 *
 * A copy's stale band [y0, y1) is the rows it doesn't hold the picture's current pixels in; y1 <= y0: none. A copy just
 * made, just imported by the desktop's GPU, or whose picture's source changed (another dmabuf, a new attach, another
 * size) lacks every row: gband_all. */
#ifndef LORD_GPU_GBAND_H
#define LORD_GPU_GBAND_H

typedef struct { int y0, y1; } GBand;

static inline void gband_all(GBand *b, int h) { b->y0 = 0; b->y1 = h; }
static inline void gband_none(GBand *b) { b->y0 = b->y1 = 0; }
static inline int gband_empty(const GBand *b) { return b->y1 <= b->y0; }

/* rows [y0, y1) added to what b lacks (one band: the hull of both) */
static inline void gband_add(GBand *b, int y0, int y1) {
    if (y1 <= y0) return;
    if (gband_empty(b)) { b->y0 = y0; b->y1 = y1; return; }
    if (y0 < b->y0) b->y0 = y0;
    if (y1 > b->y1) b->y1 = y1;
}

/* the rows to copy into a copy that lacks b for a frame whose rows [y0, y1) changed, kept within [0, h) */
static inline GBand gband_copy(const GBand *b, int y0, int y1, int h) {
    GBand r = { y0, y1 };
    if (!gband_empty(b)) { if (b->y0 < r.y0) r.y0 = b->y0; if (b->y1 > r.y1) r.y1 = b->y1; }
    if (r.y0 < 0) r.y0 = 0;
    if (r.y1 > h) r.y1 = h;
    return r;
}

/* a frame's rows [y0, y1) copied into copy k (gband_copy's rows): k holds the whole picture now, and the other copy
 * lacks those rows, whether it is shown or not. Before 2026-11-22 js_vm.c added them only to the copy shown: with none
 * shown (the shown one had just been imported again) the other copy kept an old frame's rows and showed them next */
static inline void gband_copied(GBand b[2], int k, int y0, int y1) {
    gband_none(&b[k]);
    gband_add(&b[1 - k], y0, y1);
}

#endif
