/* handscur.c — the agent's cursor on the screen (see handscur.h) */
#include "handscur.h"
#include "hands.h"
#include "../ui/ui.h"

#include <math.h>
#include <string.h>

static struct {
    int root, ring;
    uint64_t root_serial, ring_serial;
    double x, y;                    /* the tip, logical */
    int shown;                      /* display: flex */
    double opacity;
    double last;                    /* its last event */
    int keep;
    /* the ring's effect: kind, start, length, sizes, opacity */
    int fx;
    double fx_t0, fx_ms, fx_s0, fx_s1, fx_o0;
} C;

/* the root: a child of the desktop's root at the cursor's zIndex that takes no clicks; the ring inside it */
static Node *root_node(void) {
    Node *n = node_ref(C.root, C.root_serial);
    if (!n || !ui.root || n->parent != ui.root || fabsf(n->s.z_index - HANDSCUR_Z) > 0.01f || n->s.pointer_none != POINTER_NONE) return NULL;
    return n;
}
static Node *ring_node(Node *root) {
    Node *n = node_ref(C.ring, C.ring_serial);
    if (!n || !root) return NULL;
    for (const Node *a = n->parent; a; a = a->parent) if (a == root) return n;
    return NULL;
}

int handscur_set(int root, int ring) {
    Node *r = root > 0 ? node_get(root) : NULL, *g = ring > 0 ? node_get(ring) : NULL;
    if (!r || !g) return -1;
    C.root = root;
    C.root_serial = r->serial;
    C.ring = ring;
    C.ring_serial = g->serial;
    if (!root_node() || !ring_node(r)) { C.root = C.ring = 0; return -1; }
    C.shown = 0;
    return 0;
}

int handscur_root(void) { Node *n = root_node(); return n ? n->id : 0; }

/* a style of the node through the engine's own path (node_restyled: the damage is its box, before and after) */
static void set_num(Node *n, const char *key, double v) {
    Style before = n->s;
    StyleExt eb;
    if (n->s.ext) memcpy(&eb, n->s.ext, STYLE_EXT_VALUES);
    style_set_number(&n->s, key, v);
    node_restyled(n, &before, before.ext ? &eb : NULL);
}
static void set_str(Node *n, const char *key, const char *v) {
    Style before = n->s;
    StyleExt eb;
    if (n->s.ext) memcpy(&eb, n->s.ext, STYLE_EXT_VALUES);
    style_set_string(&n->s, key, v);
    node_restyled(n, &before, before.ext ? &eb : NULL);
}

static void place(Node *r) {
    set_num(r, "left", round(C.x) - HANDSCUR_TIP);
    set_num(r, "top", round(C.y) - HANDSCUR_TIP);
}

static void show(Node *r, double now) {
    if (!C.shown) { set_str(r, "display", "flex"); C.shown = 1; }
    if (C.opacity != 1) { C.opacity = 1; set_num(r, "opacity", 1); }
    C.last = now;
}

static void ring_at(Node *g, double size, double op) {
    if (size < 1) size = 1;
    set_num(g, "width", size);
    set_num(g, "height", size);
    set_num(g, "left", HANDSCUR_TIP - size / 2);
    set_num(g, "top", HANDSCUR_TIP - size / 2);
    set_num(g, "borderRadius", size / 2);
    set_num(g, "opacity", op);
}

static void hide(Node *r) {
    if (r && C.shown) set_str(r, "display", "none");
    C.shown = 0;
    C.fx = 0;
}

void handscur_event(int what, double x, double y, double w, double h, double now) {
    (void)w; (void)h;
    Node *r = root_node();
    if (!r) return;
    if (what == HANDS_CUR_HIDE) { hide(r); return; }
    if (what != HANDS_CUR_PULSE && isfinite(x) && isfinite(y) && x > -1e5 && x < 1e5 && y > -1e5 && y < 1e5) {
        C.x = x;
        C.y = y;
        place(r);
    }
    show(r, now);
    if (what == HANDS_CUR_MOVE) return;
    /* the ring: a ripple (click), a quick ring (a machine-mode landing), a small pulse (keys) */
    C.fx = what;
    C.fx_t0 = now;
    if (what == HANDS_CUR_CLICK) { C.fx_ms = 420; C.fx_s0 = 8; C.fx_s1 = 44; C.fx_o0 = 0.9; }
    else if (what == HANDS_CUR_FLASH) { C.fx_ms = 340; C.fx_s0 = 6; C.fx_s1 = 36; C.fx_o0 = 1; }
    else { C.fx_ms = 260; C.fx_s0 = 14; C.fx_s1 = 30; C.fx_o0 = 0.6; }
    Node *g = ring_node(r);
    if (g) ring_at(g, C.fx_s0, C.fx_o0);
}

void handscur_keep(int on) { C.keep = !!on; }

void handscur_tick(double now) {
    if (!C.shown) return;
    Node *r = root_node();
    if (!r) { C.shown = 0; return; }
    if (C.keep) C.last = now;
    if (C.fx) {
        Node *g = ring_node(r);
        double t = (now - C.fx_t0) / C.fx_ms;
        if (t >= 1 || !g) { if (g) ring_at(g, C.fx_s0, 0); C.fx = 0; }
        else {
            double e = 1 - (1 - t) * (1 - t);           /* ease out */
            ring_at(g, C.fx_s0 + (C.fx_s1 - C.fx_s0) * e, C.fx_o0 * (1 - t));
        }
    }
    double idle = now - C.last;
    if (idle >= HANDSCUR_IDLE_MS + HANDSCUR_FADE_MS) { hide(r); return; }
    if (idle >= HANDSCUR_IDLE_MS) {
        double o = 1 - (idle - HANDSCUR_IDLE_MS) / HANDSCUR_FADE_MS;
        o = round(o * 20) / 20;                         /* a few steps: no repaint for every millisecond */
        if (o != C.opacity) { C.opacity = o; set_num(r, "opacity", o); }
    }
}

double handscur_timeout(double now) {
    if (!C.shown) return -1;
    if (C.fx || now - C.last >= HANDSCUR_IDLE_MS) return 16;
    return HANDSCUR_IDLE_MS - (now - C.last);
}
