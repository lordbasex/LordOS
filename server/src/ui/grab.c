/* grab.c — window dragging in C: move and resize without JS per frame (grab.h). */
#include "grab.h"
#include "../cursor.h"
#include "../bench/lbinput.h"

#include <math.h>
#include <string.h>

#define KEEP 48             /* px of the frame that always stay visible */

static struct {
    int active, node, edges;
    float px0, py0;                 /* pointer at start */
    float x0, y0, w0, h0;           /* node's left/top/width/height at start */
    float x, y, w, h;               /* now */
    float ox, oy;                   /* parent's origin on screen */
    GrabLimits lim;
} g;
static float ptr_x, ptr_y;
static int ptr_buttons;

void grab_note_pointer(float x, float y, int buttons) {
    ptr_x = x;
    ptr_y = y;
    ptr_buttons = buttons;
}

int grab_active(void) { return g.active; }

int grab_state(float *x, float *y, int *edges) {
    *x = ptr_x;
    *y = ptr_y;
    *edges = g.active ? g.edges : 0;
    return g.active;
}

int grab_cursor(void) { return g.edges ? cursor_for_edges(g.edges) : CUR_MOVE; }

static float dim_or(Dim d, float fallback) { return d.unit == UNIT_PX ? d.v : fallback; }

/* LordBench's scripted move (grab_script_begin): the node follows the synthetic pointer (bench/lbinput.h), or
 * without it the same path by the clock, one step per frame */
static struct { int on; double t0; float rx, ry, period; } sc;

static void script_off(void) {
    sc.on = 0;
    lbi_stop();
}

static int setup(int node, int edges, const GrabLimits *lim) {
    Node *n = node_get(node);
    if (!n || !n->s.absolute) return -1;
    if (g.active) grab_cancel();
    memset(&g, 0, sizeof g);
    g.active = 1;
    g.node = node;
    g.edges = edges & 15;
    g.lim = *lim;
    g.px0 = ptr_x;
    g.py0 = ptr_y;
    g.x = g.x0 = dim_or(n->s.pos[EDGE_L], n->x);
    g.y = g.y0 = dim_or(n->s.pos[EDGE_T], n->y);
    g.w = g.w0 = dim_or(n->s.width, n->w);
    g.h = g.h0 = dim_or(n->s.height, n->h);
    g.ox = n->ax - n->x;
    g.oy = n->ay - n->y;
    paint_layer_begin(node, g.edges != 0);
    return 0;
}

int grab_begin(int node, int edges, const GrabLimits *lim) {
    if (!ptr_buttons) return -1;
    script_off();
    return setup(node, edges, lim);
}

int grab_script_begin(int node, const GrabLimits *lim, float rx, float ry, float period_ms) {
    if (rx < 0 || ry < 0 || period_ms < 500) return -1;
    if (setup(node, 0, lim)) return -1;
    sc.on = 1;
    sc.t0 = -1;
    sc.rx = rx;
    sc.ry = ry;
    sc.period = period_ms;
    lbi_start(rx, ry, period_ms);           /* if it fails, the clock drives it (no input latency) */
    ui.dirty = 1;
    return 0;
}

int grab_scripted(void) { return sc.on && g.active; }

/* a figure of eight that starts and ends where the window was (lbi_path) */
void grab_script_tick(double now_ms) {
    if (!sc.on) return;
    if (!g.active) { script_off(); return; }
    float dx, dy;
    if (lbi_fd() >= 0) {                    /* the synthetic pointer's newest position, if one came */
        if (lbi_take(&dx, &dy)) grab_motion(g.px0 + dx, g.py0 + dy);
        return;
    }
    if (sc.t0 < 0) sc.t0 = now_ms;
    lbi_path(sc.rx, sc.ry, sc.period, now_ms - sc.t0, &dx, &dy);
    grab_motion(g.px0 + dx, g.py0 + dy);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void grab_motion(float px, float py) {
    if (!g.active) return;
    Node *n = node_get(g.node);
    if (!n) { grab_cancel(); return; }
    float dx = roundf(px - g.px0), dy = roundf(py - g.py0), in = g.lim.inset;
    float x = g.x0, y = g.y0, w = g.w0, h = g.h0;
    /* frame bounds in node coordinates (the parent may not be at 0,0) */
    float top = g.lim.min_top - g.oy - in, sw = ui.width - g.ox, sh = ui.height - g.oy;
    if (!g.edges) {
        x = clampf(g.x0 + dx, KEEP - (g.w0 - in), sw - KEEP - in);
        /* the work area of the display under the frame's center (its own menu bar, or none) */
        float cx = g.ox + x + g.w0 / 2;
        for (int i = 0; i < g.lim.nareas; i++)
            if (cx >= g.lim.ax[i] && cx < g.lim.ax[i] + g.lim.aw[i]) { top = g.lim.ay[i] - g.oy - in; break; }
        y = clampf(g.y0 + dy, top, sh - KEEP - in);
    } else {
        float minw = g.lim.min_w + 2 * in, minh = g.lim.min_h + 2 * in;
        float maxw = g.lim.max_w > 0 ? g.lim.max_w + 2 * in : 1e9f, maxh = g.lim.max_h > 0 ? g.lim.max_h + 2 * in : 1e9f;
        if (g.edges & 8) w = clampf(g.w0 + dx, minw, fminf(maxw, sw + in - g.x0));
        if (g.edges & 4) {
            w = clampf(g.w0 - dx, minw, fminf(maxw, g.x0 + g.w0 + in));
            x = g.x0 + g.w0 - w;
        }
        if (g.edges & 2) h = clampf(g.h0 + dy, minh, fminf(maxh, sh + in - g.y0));
        if (g.edges & 1) {
            h = clampf(g.h0 - dy, minh, fminf(maxh, g.y0 + g.h0 - top));
            y = g.y0 + g.h0 - h;
        }
    }
    if (x == g.x && y == g.y && w == g.w && h == g.h) return;
    int sized = w != g.w || h != g.h;
    g.x = x; g.y = y; g.w = w; g.h = h;
    n->s.pos[EDGE_L] = (Dim){ x, UNIT_PX };
    n->s.pos[EDGE_T] = (Dim){ y, UNIT_PX };
    node_moved(n);
    if (sized) {
        n->s.width = (Dim){ w, UNIT_PX };
        n->s.height = (Dim){ h, UNIT_PX };
        node_invalidate(n);         /* the content gets laid out again */
    }
    ui.dirty = 1;                   /* move: only the position layout, without repainting the content */
}

int grab_finish(int *node, int *edges, float *dx, float *dy, float *dw, float *dh) {
    if (!g.active || ptr_buttons) return 0;
    if (sc.on) { grab_cancel(); return 0; }         /* the person moved the pointer: the scene stops where it is */
    *node = g.node;
    *edges = g.edges;
    *dx = g.x - g.x0;
    *dy = g.y - g.y0;
    *dw = g.w - g.w0;
    *dh = g.h - g.h0;
    g.active = 0;
    paint_layer_end();
    return 1;
}

void grab_cancel(void) {
    script_off();
    if (!g.active) return;
    g.active = 0;
    paint_layer_end();
}
