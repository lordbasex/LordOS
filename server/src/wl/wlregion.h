/* wlregion.h — a wl_region as the list of add/subtract requests that made it. A point is inside if
 * the last rectangle holding it was an add. Used for wl_surface.set_input_region: an app can give
 * a surface no input at all, and the pointer then goes to what is below it. */
#ifndef LORD_WLREGION_H
#define LORD_WLREGION_H

#include <stdlib.h>
#include <string.h>

#define WLRGN_MAX_OPS 16384     /* a shaped window has hundreds; a client can't make it grow forever */

typedef struct { int sub, x, y, w, h; } WlRgnOp;
typedef struct { WlRgnOp *op; int n, cap; } WlRgn;

static inline void wlrgn_free(WlRgn *r) {
    free(r->op);
    r->op = NULL;
    r->n = r->cap = 0;
}

/* -1: too many rectangles or no memory */
static inline int wlrgn_push(WlRgn *r, int sub, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    if (r->n >= WLRGN_MAX_OPS) return -1;
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 8;
        if (cap > WLRGN_MAX_OPS) cap = WLRGN_MAX_OPS;
        WlRgnOp *op = realloc(r->op, (size_t)cap * sizeof *op);
        if (!op) return -1;
        r->op = op;
        r->cap = cap;
    }
    r->op[r->n++] = (WlRgnOp){ sub, x, y, w, h };
    return 0;
}

/* dst becomes a copy of src. -1: no memory, dst is left as it was. */
static inline int wlrgn_copy(WlRgn *dst, const WlRgn *src) {
    WlRgnOp *op = NULL;
    if (src->n) {
        op = malloc((size_t)src->n * sizeof *op);
        if (!op) return -1;
        memcpy(op, src->op, (size_t)src->n * sizeof *op);
    }
    free(dst->op);
    dst->op = op;
    dst->n = dst->cap = src->n;
    return 0;
}

static inline int wlrgn_has(const WlRgn *r, double x, double y) {
    for (int i = r->n - 1; i >= 0; i--) {
        const WlRgnOp *o = &r->op[i];
        if (x >= o->x && y >= o->y && x < (double)o->x + o->w && y < (double)o->y + o->h) return !o->sub;
    }
    return 0;
}

#endif
