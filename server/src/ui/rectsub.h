/* rectsub.h — is a rectangle wholly covered by the union of others? (paint.c paint_node_hidden: a moving wallpaper's
 * view under several opaque windows; build/mloop-test checks it). The part not covered yet is kept as at most
 * RECTSUB_MAX rectangles; past that the answer is "not covered" (the safe one: the video keeps playing). */
#ifndef LORD_UI_RECTSUB_H
#define LORD_UI_RECTSUB_H

#define RECTSUB_MAX 128

typedef struct { float x0, y0, x1, y1; } RsBox;

typedef struct { RsBox r[RECTSUB_MAX]; int n, over; } RsLeft;

static inline void rs_start(RsLeft *l, RsBox t) {
    l->n = 0;
    l->over = 0;
    if (t.x1 > t.x0 && t.y1 > t.y0) l->r[l->n++] = t;
}

/* takes c away from what is left */
static inline void rs_sub(RsLeft *l, RsBox c) {
    if (l->over || c.x1 <= c.x0 || c.y1 <= c.y0) return;
    RsBox out[RECTSUB_MAX];
    int m = 0;
    for (int i = 0; i < l->n; i++) {
        RsBox a = l->r[i];
        if (c.x0 >= a.x1 || c.x1 <= a.x0 || c.y0 >= a.y1 || c.y1 <= a.y0) {      /* apart: kept whole */
            if (m >= RECTSUB_MAX) { l->over = 1; return; }
            out[m++] = a;
            continue;
        }
        float iy0 = c.y0 > a.y0 ? c.y0 : a.y0, iy1 = c.y1 < a.y1 ? c.y1 : a.y1;
        RsBox parts[4] = {
            { a.x0, a.y0, a.x1, iy0 },                                          /* above */
            { a.x0, iy1, a.x1, a.y1 },                                          /* below */
            { a.x0, iy0, c.x0 > a.x0 ? c.x0 : a.x0, iy1 },                      /* left */
            { c.x1 < a.x1 ? c.x1 : a.x1, iy0, a.x1, iy1 },                      /* right */
        };
        for (int k = 0; k < 4; k++) {
            if (parts[k].x1 <= parts[k].x0 || parts[k].y1 <= parts[k].y0) continue;
            if (m >= RECTSUB_MAX) { l->over = 1; return; }
            out[m++] = parts[k];
        }
    }
    for (int i = 0; i < m; i++) l->r[i] = out[i];
    l->n = m;
}

/* 1 when nothing is left (and the list never overflowed) */
static inline int rs_empty(const RsLeft *l) { return !l->over && l->n == 0; }

#endif
