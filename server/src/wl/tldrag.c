/* tldrag.c — xdg-toplevel-drag-v1: a window that moves with a drag (docs/WAYLAND.md, "Tab tear-off").
 *
 * A browser tears a tab off: it starts an ordinary wl_data_device drag with a wl_data_source, makes an
 * xdg_toplevel_drag_v1 for that source and attaches a toplevel to it (a new window for the tab, or the whole
 * window when its last tab is dragged), with the point of the window under the pointer (the offset, in window
 * geometry coordinates). From the drag's start until it ends that window follows the pointer: lord-server tells
 * the shell ("tldrag" on the window) and sdk/wayland.js moves it so its geometry's (offset) point stays under the
 * pointer (lord.wayland.toplevelDrag, tldrag_follow). The window doesn't take part in choosing the drag's target:
 * the hit test skips its frame (tldrag_hit), so dropping it over another window of the browser reattaches the
 * tab there. Dropped anywhere else it stays where it was let go, as after a move.
 *
 * Rules: the source must not have been used (set_selection with it, before or after, is invalid_source, as is a
 * second toplevel drag for it); only a toplevel of the client that owns the source follows (no app can move another
 * app's window); attach while an attached window still has its role is toplevel_attached; destroy while the drag
 * runs is ongoing_drag. A window unmapped while attached is detached. Xwayland's X windows never follow (its client
 * has no xdg_toplevel). */
#include "wlshell.h"
#include "../ui/ui.h"
#include "xdg-toplevel-drag-v1-protocol.h"
#include "xdg-shell-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <wayland-server-core.h>

typedef struct TDrag {
    struct wl_resource *res;            /* xdg_toplevel_drag_v1 */
    struct wl_resource *mgr;            /* the manager it came from, NULL once gone (invalid_source goes there) */
    struct wl_listener mgr_gone;
    struct wl_resource *source;         /* its wl_data_source, NULL once gone */
    struct wl_listener source_gone;
    struct wl_resource *top;            /* the attached xdg_toplevel, NULL when none or gone */
    struct wl_listener top_gone;
    int win, ox, oy;                    /* its surface id and offset */
    int seen_mapped;                    /* it was mapped while attached: an unmap detaches it */
    int dragging;                       /* start_drag took the source and the drag hasn't ended */
    int told;                           /* the shell was told it follows ("tldrag" on) */
    struct wl_list link;
} TDrag;

static struct wl_list drags;
static TDrag *active;                   /* the running drag's, if any */
static int skip_node;                   /* the following window's frame node, as the shell named it */

static void tell(TDrag *t, int on) {
    if (!t->win || t->told == on) return;
    int m = 0, c = 0, top = 0;
    if (on && (!wl__surface_state(t->win, &m, &c, &top) || !m)) return;   /* told once it is mapped */
    char j[128];
    if (on) {
        float x, y;
        wl__pointer(&x, &y);
        snprintf(j, sizeof j, "{\"on\":true,\"ox\":%d,\"oy\":%d,\"x\":%.0f,\"y\":%.0f}", t->ox, t->oy, x, y);
    } else {
        snprintf(j, sizeof j, "{\"on\":false}");
    }
    t->told = on;
    wl__emit(t->win, "tldrag", j);
}

static void detach(TDrag *t) {
    if (t->told) tell(t, 0);
    if (t->top) { wl_list_remove(&t->top_gone.link); t->top = NULL; }
    t->win = t->seen_mapped = 0;
    if (t == active) skip_node = 0;
}

/* Still attached: its toplevel lives and wasn't unmapped since it was seen mapped. */
static int attached(TDrag *t) {
    if (!t->top || !t->win) return 0;
    int m = 0, c = 0, top = 0;
    if (!wl__surface_state(t->win, &m, &c, &top) || !top) { detach(t); return 0; }
    if (m) t->seen_mapped = 1;
    else if (t->seen_mapped) { detach(t); return 0; }
    return 1;
}

static void on_top_gone(struct wl_listener *l, void *data) {
    (void)data;
    TDrag *t = wl_container_of(l, t, top_gone);
    wl_list_init(&t->top_gone.link);    /* the signal is going away with it */
    t->top = NULL;
    detach(t);
}
static void on_mgr_gone(struct wl_listener *l, void *data) {
    (void)data;
    TDrag *t = wl_container_of(l, t, mgr_gone);
    wl_list_init(&t->mgr_gone.link);
    t->mgr = NULL;
}
static void on_source_gone(struct wl_listener *l, void *data) {
    (void)data;
    TDrag *t = wl_container_of(l, t, source_gone);
    wl_list_remove(&t->source_gone.link);
    wl_list_init(&t->source_gone.link);
    t->source = NULL;
    if (t == active) { detach(t); active = NULL; }
    t->dragging = 0;
}

static void td_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    TDrag *t = wl_resource_get_user_data(r);
    if (t && t->dragging) {
        wl_resource_post_error(r, XDG_TOPLEVEL_DRAG_V1_ERROR_ONGOING_DRAG, "the drag has not ended");
        return;
    }
    wl_resource_destroy(r);
}
static void td_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *toplevel, int32_t x, int32_t y) {
    TDrag *t = wl_resource_get_user_data(r);
    if (!t) return;
    if (attached(t)) {
        wl_resource_post_error(r, XDG_TOPLEVEL_DRAG_V1_ERROR_TOPLEVEL_ATTACHED, "a toplevel is already attached");
        return;
    }
    int id = wl__toplevel_id(toplevel);
    /* only a window of the app whose drag this is; an inert toplevel (its surface gone) follows nothing */
    if (!id || wl_resource_get_client(toplevel) != c) return;
    if (x < -65536 || x > 65536 || y < -65536 || y > 65536) { x = 0; y = 0; }
    t->top = toplevel;
    t->top_gone.notify = on_top_gone;
    wl_resource_add_destroy_listener(toplevel, &t->top_gone);
    t->win = id;
    t->ox = x;
    t->oy = y;
    t->seen_mapped = 0;
    t->told = 0;
    if (t->dragging) tell(t, 1);
}
static const struct xdg_toplevel_drag_v1_interface td_impl = { .destroy = td_destroy, .attach = td_attach };

static void td_gone(struct wl_resource *r) {
    TDrag *t = wl_resource_get_user_data(r);
    if (!t) return;
    detach(t);
    if (t == active) active = NULL;
    if (t->source) wl_list_remove(&t->source_gone.link);
    if (t->mgr) wl_list_remove(&t->mgr_gone.link);
    wl_list_remove(&t->link);
    free(t);
}

static TDrag *of_source(struct wl_resource *source) {
    TDrag *t;
    wl_list_for_each(t, &drags, link) if (t->source == source) return t;
    return NULL;
}

static void mgr_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void mgr_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *source) {
    if (of_source(source) || wl__source_used(source)) {
        wl_resource_post_error(r, XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE, "the data source is already used");
        return;
    }
    TDrag *t = calloc(1, sizeof *t);
    struct wl_resource *res = t ? wl_resource_create(c, &xdg_toplevel_drag_v1_interface, wl_resource_get_version(r), id) : NULL;
    if (!res) { free(t); wl_client_post_no_memory(c); return; }
    t->res = res;
    t->source = source;
    t->source_gone.notify = on_source_gone;
    wl_resource_add_destroy_listener(source, &t->source_gone);
    t->mgr = r;
    t->mgr_gone.notify = on_mgr_gone;
    wl_resource_add_destroy_listener(r, &t->mgr_gone);
    wl_list_init(&t->top_gone.link);
    wl_list_insert(&drags, &t->link);
    wl_resource_set_implementation(res, &td_impl, t, td_gone);
}
static const struct xdg_toplevel_drag_manager_v1_interface mgr_impl = { .destroy = mgr_destroy, .get_xdg_toplevel_drag = mgr_get };
static void bind_mgr(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &xdg_toplevel_drag_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &mgr_impl, NULL, NULL);
}

void tldrag_globals(struct wl_display *dpy) {
    wl_list_init(&drags);
    wl_global_create(dpy, &xdg_toplevel_drag_manager_v1_interface, 1, NULL, bind_mgr);
}

void tldrag_started(struct wl_resource *source) {
    if (active) tldrag_ended();
    TDrag *t = source ? of_source(source) : NULL;
    if (!t) return;
    active = t;
    t->dragging = 1;
    if (attached(t)) tell(t, 1);
}
void tldrag_ended(void) {
    TDrag *t = active;
    if (!t) return;
    active = NULL;
    skip_node = 0;
    t->dragging = 0;
    if (t->told) tell(t, 0);            /* it stays where it was let go; the shell stops moving it */
}
int tldrag_claims(struct wl_resource *source) {
    TDrag *t = source ? of_source(source) : NULL;
    if (!t) return 0;
    /* the manager's error; with the manager destroyed already, the client is still ended through the drag object */
    if (t->mgr) wl_resource_post_error(t->mgr, XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE, "a toplevel drag's source used as a selection");
    else wl_client_post_implementation_error(wl_resource_get_client(t->res), "a toplevel drag's source used as a selection");
    return 1;
}

/* A new window attached before it was mapped: it follows from its first frame. */
void tldrag_mapped(int id) {
    if (active && active->win == id && attached(active)) tell(active, 1);
}

/* Is node an ancestor of (or) the window's surface node? */
static int frame_of(int frame, int win) {
    int sn = wl__surface_node(win);
    if (!frame || !sn) return 0;
    for (Node *n = node_get(sn); n; n = n->parent) if (n->id == frame) return 1;
    return 0;
}

int tldrag_following(void) { return active && active->told && attached(active); }

int tldrag_follow(int id, int frame_node, int *ox, int *oy, int *gx, int *gy, int *placed) {
    if (!active || active->win != id || !attached(active) || !active->told) return 0;
    *ox = active->ox;
    *oy = active->oy;
    int r[4], mx = 0, fs = 0;
    *placed = wl__window_rect(id, r, &mx, &fs);
    *gx = *placed ? r[0] : 0;
    *gy = *placed ? r[1] : 0;
    if (frame_node && frame_of(frame_node, id)) skip_node = frame_node;
    return 1;
}

Node *tldrag_hit(float x, float y, Node *hit) {
    if (!active || !active->win || !hit) return hit;
    if (skip_node && !frame_of(skip_node, active->win)) skip_node = 0;
    /* its frame, once the shell named it; until then (its first moments) at least its surface */
    int skip = skip_node ? skip_node : wl__surface_node(active->win);
    Node *f = skip ? node_get(skip) : NULL;
    if (!f) return hit;
    int inside = 0;
    for (Node *n = hit; n; n = n->parent) if (n == f) { inside = 1; break; }
    if (!inside) return hit;
    uint8_t was = f->s.pointer_none;
    f->s.pointer_none = POINTER_NONE;   /* only for this hit test: what is under the window */
    Node *under = hit_test(ui.root, x, y);
    f->s.pointer_none = was;
    return under;
}
