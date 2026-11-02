/* outputs.h — layout math for several displays (docs/DISPLAYS.md): the global logical space,
 * per-display scale, logical ↔ physical conversion, arranging and snapping displays, the pointer
 * across displays. Pure functions, no KMS or UI state: tested by tests/outputs_test.c.
 *
 * Terms:
 *   - logical px: the unit of the UI engine (styles, layout, pointer, windows, Wayland surface
 *     coordinates). The desktop is one logical space; every display shows a rectangle of it.
 *   - physical px: the display's own pixels (its mode, after rotation).
 *   - scale: physical px per logical px, in percent (100, 125, 150, 175, 200).
 */
#ifndef LORD_OUTPUTS_H
#define LORD_OUTPUTS_H

#include "canvas.h"

#define OUT_MAX 4                       /* displays driven at once (CRTCs) */
#define OUT_SCALE_MIN 100
#define OUT_SCALE_MAX 200
#define OUT_SCALE_STEP 25

/* Rectangle of the global logical space. */
typedef struct { int x, y, w, h; } ORect;

/* How a display shows the global space: logical rect (x, y, w, h), its upright physical frame
 * (pw × ph) and the transform: physical = (logical - (x, y)) * k + (dx, dy). dx/dy center a mirrored
 * display whose shape differs from the main one (letterbox). */
typedef struct {
    int x, y, w, h;
    int pw, ph;
    double k;
    int dx, dy;
} OutView;

/* ---- scale ---- */
int  out_scale_valid(int pct);                          /* 100, 125, 150, 175 or 200 */
int  out_scale_round(double factor);                    /* 1.37 → 125: nearest valid step, clamped */
/* Recommended scale from the physical density (EDID size in mm). 100 when the size is unknown. */
int  out_scale_recommended(int pw, int ph, int mm_w, int mm_h);
/* Logical size of a physical frame at a scale (rounded to the nearest logical pixel). */
void out_logical_size(int pw, int ph, int pct, int *lw, int *lh);

/* ---- views ---- */
OutView out_view(int x, int y, int pw, int ph, int pct);
/* A mirrored display: shows the logical rect (0, 0, lw, lh) fitted into pw × ph, centered. */
OutView out_view_mirror(int lw, int lh, int pw, int ph);
void out_to_phys(const OutView *v, double lx, double ly, double *px, double *py);
void out_to_logical(const OutView *v, double px, double py, double *lx, double *ly);
/* Physical rectangle (clamped to the frame) that covers a logical box; empty if it misses. */
Rect out_phys_rect(const OutView *v, float x0, float y0, float x1, float y1);
/* Logical box covered by a physical rectangle of the view. */
void out_logical_box(const OutView *v, Rect r, float *x0, float *y0, float *x1, float *y1);

/* ---- arrangement (extend) ---- */
/* Where display `moving` (size r[moving].w × h) lands if dropped at (want_x, want_y): touching
 * another display along an edge, never overlapping, edges aligned when within `snap` px. Others
 * stay where they are. Returns the position in *x, *y. */
void out_snap(const ORect *r, int n, int moving, int want_x, int want_y, int snap, int *x, int *y);
/* 1 if display `moving` at (x, y) overlaps no other and shares an edge (at least one pixel, not a
 * corner) with one of them: a position Settings computed (sdk/displaylayout.js) that is kept as is. */
int  out_fits(const ORect *r, int n, int moving, int x, int y);
/* 1 if no two displays overlap and every display is joined to the others through shared edges. */
int  out_valid(const ORect *r, int n);
/* Displays that overlap or float apart (a scale or resolution changed, a display unplugged):
 * keep r[0] (the main one) and re-snap the others in order, each at its current position. A valid
 * arrangement (out_valid) is left alone, so a chain like main ← 2 ← 1 is kept. */
void out_fix(ORect *r, int n);
/* Shift every display so the arrangement's top-left corner is (0, 0). Returns the bounding size. */
void out_normalize(ORect *r, int n, int *bw, int *bh);
/* Default arrangement for displays without a saved position: each one to the right of the last. */
void out_auto(ORect *r, int n);

/* ---- pointer and windows ---- */
int  out_at(const ORect *r, int n, int x, int y);       /* display containing the point, or -1 */
/* The nearest point of the union of displays (the pointer never lands in a gap). */
void out_clamp(const ORect *r, int n, int *x, int *y);
/* Display with the largest overlap with a box (ties: the lowest index); -1 if it touches none. */
int  out_best(const ORect *r, int n, float x0, float y0, float x1, float y1);

#endif
