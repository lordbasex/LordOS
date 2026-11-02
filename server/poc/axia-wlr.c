/* axia-wlr.c — proof of concept: Axia DE's own UI engine on top of wlroots (docs/WLROOTS.md).
 *
 * wlroots runs everything below the desktop: the backend (DRM/KMS and libinput through the session, or
 * a window inside another compositor), the Wayland protocols, xdg-shell, the seat and the cursor. The
 * engine (src/ui, src/canvas.c) paints the whole screen as axia-server does: a wallpaper, a menu bar and
 * a Dock with backdrop blur, and each app window as a framed View whose NODE_SURFACE child holds the
 * app's surface. wl_paint and wl_paint_popups below are the engine's hooks for those surfaces; the
 * frame goes to the output as one texture.
 *
 *   axia-wlr [-s command]        command: started inside, with WAYLAND_DISPLAY set (e.g. firefox)
 *
 * Not the desktop: no QuickJS, no React shell, CPU painting only, one output at scale 1. The point is
 * to check that the engine's look survives and that apps (Firefox first) get their input. */
#include "ui/ui.h"
#include "ui/fonts.h"
#include "canvas.h"

#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

enum { TITLE_H = 32, WIN_MAX = 32 };

typedef struct Win {
    int id;                             /* NODE_SURFACE's id (n->term) */
    struct wlr_xdg_toplevel *top;
    Node *frame, *body;                 /* the framed View and its NODE_SURFACE */
    int x, y, mapped;
    struct wl_listener map, unmap, commit, destroy, request_move, new_deco_mode;
} Win;

static struct {
    struct wl_display *display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_output_layout *layout;
    struct wlr_output *output;          /* the first one: the PoC paints a single display */
    struct wlr_xdg_shell *xdg;
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *xcursor;
    struct wlr_seat *seat;
    struct wl_list keyboards;
    Win *win[WIN_MAX];                  /* bottom to top */
    int nwin, next_id;
    Node *root, *windows;
    uint32_t *px;                       /* the frame, premultiplied ARGB */
    int W, H, dirty;
    Win *grab;                          /* moving a window by its title bar */
    double grab_dx, grab_dy;
    uint32_t *scratch;                  /* one surface's pixels */
    size_t scratch_n;
    struct wl_listener new_output, new_input, new_toplevel, cursor_motion, cursor_motion_abs,
                       cursor_button, cursor_axis, cursor_frame, request_cursor, request_selection,
                       new_deco, activate, output_frame, new_surface;
} S;

static void damage(void) {
    S.dirty = 1;
    if (S.output) wlr_output_schedule_frame(S.output);
}

/* Every surface's commit repaints: the toplevel's, and also its subsurfaces' and popups', which
 * commit on their own (Firefox draws its content in a subsurface and waits for that frame's
 * frame_done before drawing the next one: without this its window would stop updating). */
typedef struct { struct wl_listener commit, destroy; } SurfWatch;
static void on_surface_commit(struct wl_listener *l, void *data) { (void)l; (void)data; damage(); }
static void on_surface_destroy(struct wl_listener *l, void *data) {
    (void)data;
    SurfWatch *w = wl_container_of(l, w, destroy);
    wl_list_remove(&w->commit.link);
    wl_list_remove(&w->destroy.link);
    free(w);
}
static void on_new_surface(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_surface *s = data;
    SurfWatch *w = calloc(1, sizeof *w);
    if (!w) return;
    w->commit.notify = on_surface_commit;   wl_signal_add(&s->events.commit, &w->commit);
    w->destroy.notify = on_surface_destroy; wl_signal_add(&s->events.destroy, &w->destroy);
}

/* ---- the UI tree (as engine-test builds it: what lord.setStyle would set) ---- */

static void sn(Node *n, const char *k, double v) { style_set_number(&n->s, k, v); node_invalidate(n); }
static void ss(Node *n, const char *k, const char *v) { style_set_string(&n->s, k, v); node_invalidate(n); }
static Node *view(Node *parent) { Node *n = node_new(NODE_VIEW); if (parent) node_append(parent, n); return n; }
static Node *text(Node *parent, const char *s, float size, uint32_t color, int bold) {
    Node *t = node_new(NODE_TEXT), *r = node_new(NODE_RAW);
    node_set_text(r, s);
    node_append(t, r);
    node_append(parent, t);
    sn(t, "fontSize", size);
    sn(t, "color", color);
    if (bold) sn(t, "fontWeight", 600);
    return t;
}
static void absolute(Node *n, float x, float y, float w, float h) {
    ss(n, "position", "absolute");
    sn(n, "left", x); sn(n, "top", y);
    if (w > 0) sn(n, "width", w);
    if (h > 0) sn(n, "height", h);
}

static void build_ui(const char *wallpaper) {
    ui_reset();
    S.root = view(NULL);
    ui.root = S.root;
    sn(S.root, "backgroundColor", 0xff14102a);
    if (wallpaper) {
        Node *img = node_new(NODE_IMAGE);
        img->img = image_get(wallpaper);
        node_append(S.root, img);
        absolute(img, 0, 0, 0, 0);
        sn(img, "right", 0); sn(img, "bottom", 0);
        ss(img, "objectFit", "cover");
    }
    S.windows = view(S.root);
    absolute(S.windows, 0, 0, 0, 0);
    sn(S.windows, "right", 0); sn(S.windows, "bottom", 0);
    ss(S.windows, "pointerEvents", "box-none");
    /* the menu bar and the Dock: translucent, with the same backdrop blur as the desktop's */
    Node *bar = view(S.root);
    absolute(bar, 0, 0, 0, 28);
    sn(bar, "right", 0);
    sn(bar, "backgroundColor", 0x66101018);
    sn(bar, "backdropBlur", 12);
    ss(bar, "flexDirection", "row");
    ss(bar, "alignItems", "center");
    sn(bar, "paddingLeft", 14);
    sn(bar, "gap", 18);
    text(bar, "Axia DE", 13, 0xffffffff, 1);
    text(bar, "wlroots proof of concept", 13, 0xccffffff, 0);
    Node *dock = view(S.root);
    ss(dock, "position", "absolute");
    sn(dock, "bottom", 10); sn(dock, "left", 0); sn(dock, "right", 0);
    sn(dock, "height", 72);
    ss(dock, "alignItems", "center");
    Node *shelf = view(dock);
    sn(shelf, "height", 72);
    sn(shelf, "borderRadius", 20);
    sn(shelf, "backgroundColor", 0x40ffffff);
    sn(shelf, "backdropBlur", 24);
    ss(shelf, "flexDirection", "row");
    ss(shelf, "alignItems", "center");
    sn(shelf, "paddingLeft", 12); sn(shelf, "paddingRight", 12);
    sn(shelf, "gap", 10);
    static const uint32_t icons[] = { 0xff4f8cff, 0xff7c5cff, 0xff2bc48a, 0xffffb020, 0xffff5a6a, 0xff30b0ff };
    for (size_t i = 0; i < sizeof icons / sizeof icons[0]; i++) {
        Node *ic = view(shelf);
        sn(ic, "width", 52); sn(ic, "height", 52);
        sn(ic, "borderRadius", 13);
        sn(ic, "backgroundColor", icons[i]);
    }
}

static Win *win_at(double x, double y, int *in_title) {
    for (int i = S.nwin - 1; i >= 0; i--) {
        Win *w = S.win[i];
        if (!w->mapped) continue;
        struct wlr_box g = w->top->base->geometry;
        if (x >= w->x && x < w->x + g.width && y >= w->y && y < w->y + TITLE_H + g.height) {
            if (in_title) *in_title = y < w->y + TITLE_H;
            return w;
        }
    }
    return NULL;
}

static void win_place(Win *w) {
    struct wlr_box g = w->top->base->geometry;
    absolute(w->frame, (float)w->x, (float)w->y, (float)g.width, (float)(g.height + TITLE_H));
    sn(w->body, "width", g.width);
    sn(w->body, "height", g.height);
    damage();
}

static void win_raise(Win *w) {
    int i = 0;
    while (i < S.nwin && S.win[i] != w) i++;
    if (i == S.nwin) return;
    memmove(&S.win[i], &S.win[i + 1], sizeof S.win[0] * (size_t)(S.nwin - i - 1));
    S.win[S.nwin - 1] = w;
    node_detach(w->frame);
    node_append(S.windows, w->frame);
    damage();
}

static void win_focus(Win *w) {
    struct wlr_surface *prev = S.seat->keyboard_state.focused_surface;
    if (w && prev == w->top->base->surface) return;
    if (prev) {
        struct wlr_xdg_toplevel *pt = wlr_xdg_toplevel_try_from_wlr_surface(prev);
        if (pt) wlr_xdg_toplevel_set_activated(pt, false);
    }
    if (!w) { wlr_seat_keyboard_notify_clear_focus(S.seat); return; }
    win_raise(w);
    wlr_xdg_toplevel_set_activated(w->top, true);
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(S.seat);
    if (kb) wlr_seat_keyboard_notify_enter(S.seat, w->top->base->surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
}

/* ---- the engine's hooks for app surfaces ---- */

static Win *win_by_id(int id) {
    for (int i = 0; i < S.nwin; i++) if (S.win[i]->id == id) return S.win[i];
    return NULL;
}

/* One surface's current buffer into the canvas at (x, y), canvas pixels. wlroots reads it back as
 * premultiplied ARGB, whatever the buffer is (shm, or a dmabuf the renderer imported). */
static void blit_surface(Canvas *c, struct wlr_surface *s, int x, int y, float op) {
    struct wlr_texture *tex = wlr_surface_get_texture(s);
    if (!tex) return;
    size_t n = (size_t)tex->width * tex->height;
    if (n > S.scratch_n) {
        free(S.scratch);
        S.scratch = malloc(n * 4);
        S.scratch_n = S.scratch ? n : 0;
        if (!S.scratch) return;
    }
    if (!wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
            .data = S.scratch, .format = DRM_FORMAT_ARGB8888, .stride = tex->width * 4 })) return;
    canvas_blit(c, S.scratch, (int)tex->width, (int)tex->height, x, y, 0, op, 0);
}

typedef struct { Canvas *c; int x, y; float op; } BlitAt;
static void blit_each(struct wlr_surface *s, int sx, int sy, void *data) {
    BlitAt *b = data;
    blit_surface(b->c, s, b->x + sx, b->y + sy, b->op);
}

/* The window's surface tree (the toplevel and its subsurfaces), clipped to its node: the geometry's
 * origin is the node's corner, so client-side shadows outside it are left out. */
void wl_paint(Canvas *c, int id, int node, float x, float y, float w, float h, float op) {
    (void)node;
    Win *win = win_by_id(id);
    if (!win || !win->mapped) return;
    const float k = paint_view.k;
    float cx = (x - paint_view.x) * k + paint_view.dx, cy = (y - paint_view.y) * k + paint_view.dy;
    Rect old = canvas_push_clip(c, cx, cy, w * k, h * k);
    struct wlr_box g = win->top->base->geometry;
    BlitAt b = { c, (int)cx - g.x, (int)cy - g.y, op };
    wlr_surface_for_each_surface(win->top->base->surface, blit_each, &b);
    canvas_set_clip(c, old);
}

/* Menus and other popups above everything, bottom window first. */
void wl_paint_popups(Canvas *c) {
    for (int i = 0; i < S.nwin; i++) {
        Win *w = S.win[i];
        if (!w->mapped) continue;
        struct wlr_box g = w->top->base->geometry;
        BlitAt b = { c, w->x - g.x, w->y + TITLE_H - g.y, 1 };
        wlr_xdg_surface_for_each_popup_surface(w->top->base, blit_each, &b);
    }
}
/* the GPU path's window list (paint.c): unused on the CPU path */
void wl_paint_holes(int on) { (void)on; }
void wl_paint_kept(void) {}
void wl_paint_keep(void) {}
void wl_paint_begin(void) {}
void wl_paint_save(void) {}
void wl_paint_restore(void) {}
void wl_tf_begin(int node, float sx, float sy, float tx, float ty, float op, int skip) { (void)node; (void)sx; (void)sy; (void)tx; (void)ty; (void)op; (void)skip; }
void wl_tf_end(void) {}
void wl_tf_nested(int d) { (void)d; }
void wl_tf_open_holes(Canvas *c) { (void)c; }
int wl_draws_meet(Rect r) { (void)r; return 0; }
/* the rest of axia-server the engine calls: terminals, media, GPU, LordBench input timing */
void term_paint(Canvas *c, int id, float x, float y, float w, float h, float op) { (void)c; (void)id; (void)x; (void)y; (void)w; (void)h; (void)op; }
void media_paint(Canvas *c, int id, int node, float x, float y, float w, float h, float r, float op) { (void)c; (void)id; (void)node; (void)x; (void)y; (void)w; (void)h; (void)r; (void)op; }
int media_cover_node(float x0, float y0, float x1, float y1) { (void)x0; (void)y0; (void)x1; (void)y1; return 0; }
int gpu_active(void) { return 0; }
void lbf_input(uint64_t ev_ns) { (void)ev_ns; }

/* ---- output: the engine paints, wlroots presents ---- */

static void frame_done(struct wlr_surface *s, int sx, int sy, void *data) {
    (void)sx; (void)sy;
    wlr_surface_send_frame_done(s, data);
}

static void on_output_frame(struct wl_listener *l, void *data) {
    (void)l; (void)data;
    struct wlr_output *o = S.output;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (S.W != o->width || S.H != o->height) {
        free(S.px);
        S.W = o->width; S.H = o->height;
        S.px = calloc((size_t)S.W * S.H, 4);
        S.dirty = 1;
    }
    if (S.dirty && S.px) {
        Canvas c;
        canvas_init(&c, S.px, S.W, S.H, S.W);
        layout_root(S.root, (float)S.W, (float)S.H);
        paint_damage(S.root);
        ui.damage = (Box){ 0, 0, (float)S.W, (float)S.H };
        paint_root_clear(&c, S.root, 0);
        ui.damage = (Box){ 0, 0, 0, 0 };
        S.dirty = 0;
    }
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    struct wlr_render_pass *pass = wlr_output_begin_render_pass(o, &st, NULL);
    if (pass) {
        struct wlr_texture *tex = S.px ? wlr_texture_from_pixels(S.renderer, DRM_FORMAT_ARGB8888, (uint32_t)S.W * 4, (uint32_t)S.W, (uint32_t)S.H, S.px) : NULL;
        if (tex) wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){ .texture = tex, .blend_mode = WLR_RENDER_BLEND_MODE_NONE });
        wlr_output_add_software_cursors_to_render_pass(o, pass, NULL);
        wlr_render_pass_submit(pass);
        wlr_output_commit_state(o, &st);
        if (tex) wlr_texture_destroy(tex);
    }
    wlr_output_state_finish(&st);
    for (int i = 0; i < S.nwin; i++)
        if (S.win[i]->mapped) wlr_xdg_surface_for_each_surface(S.win[i]->top->base, frame_done, &now);
}

static void on_new_output(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_output *o = data;
    wlr_output_init_render(o, S.allocator, S.renderer);
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(o);
    if (mode) wlr_output_state_set_mode(&st, mode);
    wlr_output_commit_state(o, &st);
    wlr_output_state_finish(&st);
    wlr_output_layout_add_auto(S.layout, o);
    if (S.output) return;
    S.output = o;
    S.output_frame.notify = on_output_frame;
    wl_signal_add(&o->events.frame, &S.output_frame);
    damage();
}

/* ---- windows ---- */

static void on_map(struct wl_listener *l, void *data) {
    (void)data;
    Win *w = wl_container_of(l, w, map);
    w->mapped = 1;
    static int cascade;
    w->x = 120 + cascade * 40;
    w->y = 70 + cascade * 34;
    cascade = (cascade + 1) % 6;
    node_append(S.windows, w->frame);
    win_place(w);
    win_focus(w);
}
static void on_unmap(struct wl_listener *l, void *data) {
    (void)data;
    Win *w = wl_container_of(l, w, unmap);
    w->mapped = 0;
    if (S.grab == w) S.grab = NULL;
    node_detach(w->frame);
    damage();
}
static void on_commit(struct wl_listener *l, void *data) {
    (void)data;
    Win *w = wl_container_of(l, w, commit);
    if (w->top->base->initial_commit) {
        wlr_xdg_toplevel_set_size(w->top, 1000, 620);
        return;
    }
    if (w->mapped) win_place(w);
}
static void on_destroy(struct wl_listener *l, void *data) {
    (void)data;
    Win *w = wl_container_of(l, w, destroy);
    int i = 0;
    while (i < S.nwin && S.win[i] != w) i++;
    if (i < S.nwin) { memmove(&S.win[i], &S.win[i + 1], sizeof S.win[0] * (size_t)(S.nwin - i - 1)); S.nwin--; }
    wl_list_remove(&w->map.link); wl_list_remove(&w->unmap.link); wl_list_remove(&w->commit.link);
    wl_list_remove(&w->destroy.link); wl_list_remove(&w->request_move.link);
    if (w->frame->parent) node_detach(w->frame);
    free(w);
    damage();
}
static void on_request_move(struct wl_listener *l, void *data) {
    (void)data;
    Win *w = wl_container_of(l, w, request_move);
    S.grab = w;
    S.grab_dx = S.cursor->x - w->x;
    S.grab_dy = S.cursor->y - w->y;
}

static void on_new_toplevel(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_xdg_toplevel *t = data;
    if (S.nwin == WIN_MAX) return;
    Win *w = calloc(1, sizeof *w);
    if (!w) return;
    w->top = t;
    w->id = ++S.next_id;
    /* the frame the desktop draws: rounded, shadowed, with a title bar above the app */
    w->frame = node_new(NODE_VIEW);
    sn(w->frame, "borderRadius", 12);
    ss(w->frame, "overflow", "hidden");
    sn(w->frame, "backgroundColor", 0xff26262c);
    sn(w->frame, "shadowColor", 0x70000000);
    sn(w->frame, "shadowBlur", 30);
    sn(w->frame, "shadowOffsetY", 10);
    Node *title = view(w->frame);
    sn(title, "height", TITLE_H);
    ss(title, "flexDirection", "row");
    ss(title, "alignItems", "center");
    sn(title, "paddingLeft", 12);
    sn(title, "gap", 8);
    static const uint32_t lights[] = { 0xffff5f57, 0xfffebc2e, 0xff28c840 };
    for (int i = 0; i < 3; i++) {
        Node *d = view(title);
        sn(d, "width", 12); sn(d, "height", 12); sn(d, "borderRadius", 6);
        sn(d, "backgroundColor", lights[i]);
    }
    text(title, "app", 13, 0xffd0d0d8, 1);
    w->body = node_new(NODE_SURFACE);
    w->body->term = w->id;
    node_append(w->frame, w->body);
    struct wlr_surface *s = t->base->surface;
    w->map.notify = on_map;         wl_signal_add(&s->events.map, &w->map);
    w->unmap.notify = on_unmap;     wl_signal_add(&s->events.unmap, &w->unmap);
    w->commit.notify = on_commit;   wl_signal_add(&s->events.commit, &w->commit);
    w->destroy.notify = on_destroy; wl_signal_add(&t->events.destroy, &w->destroy);
    w->request_move.notify = on_request_move; wl_signal_add(&t->events.request_move, &w->request_move);
    S.win[S.nwin++] = w;
}

/* ---- input ---- */

static void pointer_update(uint32_t time) {
    double x = S.cursor->x, y = S.cursor->y;
    if (S.grab) {
        S.grab->x = (int)(x - S.grab_dx);
        S.grab->y = (int)(y - S.grab_dy);
        win_place(S.grab);
        return;
    }
    /* popups first: they can reach outside their window */
    for (int i = S.nwin - 1; i >= 0; i--) {
        Win *w = S.win[i];
        if (!w->mapped) continue;
        struct wlr_box g = w->top->base->geometry;
        double ox = w->x - g.x, oy = w->y + TITLE_H - g.y, sx, sy;
        struct wlr_surface *s = wlr_xdg_surface_surface_at(w->top->base, x - ox, y - oy, &sx, &sy);
        int title = 0;
        if (s && (s != w->top->base->surface || win_at(x, y, &title) == w)) {
            if (title) break;
            wlr_seat_pointer_notify_enter(S.seat, s, sx, sy);
            wlr_seat_pointer_notify_motion(S.seat, time, sx, sy);
            return;
        }
    }
    wlr_cursor_set_xcursor(S.cursor, S.xcursor, "default");
    wlr_seat_pointer_clear_focus(S.seat);
}

static void on_motion(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_pointer_motion_event *e = data;
    wlr_cursor_move(S.cursor, &e->pointer->base, e->delta_x, e->delta_y);
    pointer_update(e->time_msec);
}
static void on_motion_abs(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_pointer_motion_absolute_event *e = data;
    wlr_cursor_warp_absolute(S.cursor, &e->pointer->base, e->x, e->y);
    pointer_update(e->time_msec);
}
static void on_button(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_pointer_button_event *e = data;
    if (e->state == WL_POINTER_BUTTON_STATE_RELEASED && S.grab) { S.grab = NULL; return; }
    if (e->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        int title = 0;
        Win *w = win_at(S.cursor->x, S.cursor->y, &title);
        if (w) win_focus(w);
        if (w && title && e->button == BTN_LEFT) {
            /* the close light ends the app; the rest of the bar moves the window */
            if (S.cursor->x < w->x + 30) { wlr_xdg_toplevel_send_close(w->top); return; }
            S.grab = w;
            S.grab_dx = S.cursor->x - w->x;
            S.grab_dy = S.cursor->y - w->y;
            return;
        }
    }
    wlr_seat_pointer_notify_button(S.seat, e->time_msec, e->button, e->state);
}
static void on_axis(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_pointer_axis_event *e = data;
    wlr_seat_pointer_notify_axis(S.seat, e->time_msec, e->orientation, e->delta, e->delta_discrete, e->source, e->relative_direction);
}
static void on_cursor_frame(struct wl_listener *l, void *data) {
    (void)l; (void)data;
    wlr_seat_pointer_notify_frame(S.seat);
}
static void on_request_cursor(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_seat_pointer_request_set_cursor_event *e = data;
    if (S.seat->pointer_state.focused_client == e->seat_client)
        wlr_cursor_set_surface(S.cursor, e->surface, e->hotspot_x, e->hotspot_y);
}
static void on_request_selection(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_seat_request_set_selection_event *e = data;
    wlr_seat_set_selection(S.seat, e->source, e->serial);
}

typedef struct { struct wlr_keyboard *kb; struct wl_listener key, modifiers, destroy; struct wl_list link; } Kb;
static void on_key(struct wl_listener *l, void *data) {
    Kb *k = wl_container_of(l, k, key);
    struct wlr_keyboard_key_event *e = data;
    /* Ctrl+Alt+Backspace leaves the proof of concept */
    uint32_t mods = wlr_keyboard_get_modifiers(k->kb);
    if (e->state == WL_KEYBOARD_KEY_STATE_PRESSED && e->keycode == KEY_BACKSPACE &&
        (mods & WLR_MODIFIER_CTRL) && (mods & WLR_MODIFIER_ALT)) { wl_display_terminate(S.display); return; }
    wlr_seat_set_keyboard(S.seat, k->kb);
    wlr_seat_keyboard_notify_key(S.seat, e->time_msec, e->keycode, e->state);
}
static void on_modifiers(struct wl_listener *l, void *data) {
    (void)data;
    Kb *k = wl_container_of(l, k, modifiers);
    wlr_seat_set_keyboard(S.seat, k->kb);
    wlr_seat_keyboard_notify_modifiers(S.seat, &k->kb->modifiers);
}
static void on_kb_destroy(struct wl_listener *l, void *data) {
    (void)data;
    Kb *k = wl_container_of(l, k, destroy);
    wl_list_remove(&k->key.link); wl_list_remove(&k->modifiers.link);
    wl_list_remove(&k->destroy.link); wl_list_remove(&k->link);
    free(k);
}

static void on_new_input(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_input_device *d = data;
    if (d->type == WLR_INPUT_DEVICE_POINTER) wlr_cursor_attach_input_device(S.cursor, d);
    else if (d->type == WLR_INPUT_DEVICE_KEYBOARD) {
        Kb *k = calloc(1, sizeof *k);
        if (!k) return;
        k->kb = wlr_keyboard_from_input_device(d);
        struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
        wlr_keyboard_set_keymap(k->kb, km);
        xkb_keymap_unref(km);
        xkb_context_unref(ctx);
        wlr_keyboard_set_repeat_info(k->kb, 25, 600);
        k->key.notify = on_key;             wl_signal_add(&k->kb->events.key, &k->key);
        k->modifiers.notify = on_modifiers; wl_signal_add(&k->kb->events.modifiers, &k->modifiers);
        k->destroy.notify = on_kb_destroy;  wl_signal_add(&d->events.destroy, &k->destroy);
        wl_list_insert(&S.keyboards, &k->link);
        wlr_seat_set_keyboard(S.seat, k->kb);
    }
    wlr_seat_set_capabilities(S.seat, WL_SEAT_CAPABILITY_POINTER | (wl_list_empty(&S.keyboards) ? 0 : WL_SEAT_CAPABILITY_KEYBOARD));
}

/* Apps draw their own title bar unless asked not to: the desktop draws the frame (server side). */
static void on_new_deco(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_xdg_toplevel_decoration_v1 *d = data;
    if (d->toplevel->base->initialized) wlr_xdg_toplevel_decoration_v1_set_mode(d, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static void on_activate(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_xdg_activation_v1_request_activate_event *e = data;
    struct wlr_xdg_toplevel *t = wlr_xdg_toplevel_try_from_wlr_surface(e->surface);
    for (int i = 0; t && i < S.nwin; i++) if (S.win[i]->top == t && S.win[i]->mapped) win_focus(S.win[i]);
}

int main(int argc, char **argv) {
    const char *cmd = NULL;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-s") && i + 1 < argc) cmd = argv[++i];
    wlr_log_init(WLR_ERROR, NULL);
    const char *root = getenv("AXIA_ROOT") ? getenv("AXIA_ROOT") : "/usr/libexec/axia";
    char dir[1024], wall[1024];
    snprintf(dir, sizeof dir, "%s/assets/fonts", root);
    fonts_set_builtin_dir(dir);
    if (text_init(dir)) { fprintf(stderr, "axia-wlr: no fonts in %s\n", dir); return 1; }
    snprintf(wall, sizeof wall, "%s/assets/wallpapers/cosmos/lordos-infinite-galaxy.jpg", root);
    build_ui(access(wall, R_OK) == 0 ? wall : NULL);

    S.display = wl_display_create();
    S.backend = wlr_backend_autocreate(wl_display_get_event_loop(S.display), NULL);
    if (!S.backend) { fprintf(stderr, "axia-wlr: no backend\n"); return 1; }
    S.renderer = wlr_renderer_autocreate(S.backend);
    if (!S.renderer) { fprintf(stderr, "axia-wlr: no renderer\n"); return 1; }
    wlr_renderer_init_wl_display(S.renderer, S.display);
    S.allocator = wlr_allocator_autocreate(S.backend, S.renderer);
    struct wlr_compositor *comp = wlr_compositor_create(S.display, 6, S.renderer);
    S.new_surface.notify = on_new_surface;
    wl_signal_add(&comp->events.new_surface, &S.new_surface);
    wlr_subcompositor_create(S.display);
    wlr_data_device_manager_create(S.display);
    S.layout = wlr_output_layout_create(S.display);
    S.new_output.notify = on_new_output;
    wl_signal_add(&S.backend->events.new_output, &S.new_output);
    S.xdg = wlr_xdg_shell_create(S.display, 6);
    S.new_toplevel.notify = on_new_toplevel;
    wl_signal_add(&S.xdg->events.new_toplevel, &S.new_toplevel);
    struct wlr_xdg_decoration_manager_v1 *deco = wlr_xdg_decoration_manager_v1_create(S.display);
    S.new_deco.notify = on_new_deco;
    wl_signal_add(&deco->events.new_toplevel_decoration, &S.new_deco);
    struct wlr_xdg_activation_v1 *act = wlr_xdg_activation_v1_create(S.display);
    S.activate.notify = on_activate;
    wl_signal_add(&act->events.request_activate, &S.activate);

    S.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(S.cursor, S.layout);
    S.xcursor = wlr_xcursor_manager_create(NULL, 24);
    S.cursor_motion.notify = on_motion;          wl_signal_add(&S.cursor->events.motion, &S.cursor_motion);
    S.cursor_motion_abs.notify = on_motion_abs;  wl_signal_add(&S.cursor->events.motion_absolute, &S.cursor_motion_abs);
    S.cursor_button.notify = on_button;          wl_signal_add(&S.cursor->events.button, &S.cursor_button);
    S.cursor_axis.notify = on_axis;              wl_signal_add(&S.cursor->events.axis, &S.cursor_axis);
    S.cursor_frame.notify = on_cursor_frame;     wl_signal_add(&S.cursor->events.frame, &S.cursor_frame);
    wl_list_init(&S.keyboards);
    S.new_input.notify = on_new_input;
    wl_signal_add(&S.backend->events.new_input, &S.new_input);
    S.seat = wlr_seat_create(S.display, "seat0");
    S.request_cursor.notify = on_request_cursor;
    wl_signal_add(&S.seat->events.request_set_cursor, &S.request_cursor);
    S.request_selection.notify = on_request_selection;
    wl_signal_add(&S.seat->events.request_set_selection, &S.request_selection);

    const char *socket = wl_display_add_socket_auto(S.display);
    if (!socket || !wlr_backend_start(S.backend)) { fprintf(stderr, "axia-wlr: could not start\n"); return 1; }
    setenv("WAYLAND_DISPLAY", socket, 1);
    wlr_cursor_set_xcursor(S.cursor, S.xcursor, "default");
    fprintf(stderr, "axia-wlr: WAYLAND_DISPLAY=%s\n", socket);
    if (cmd && fork() == 0) {
        setenv("GDK_BACKEND", "wayland", 1);
        setenv("MOZ_ENABLE_WAYLAND", "1", 1);
        unsetenv("DISPLAY");
        execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    wl_display_run(S.display);
    wl_display_destroy_clients(S.display);
    wl_display_destroy(S.display);
    return 0;
}
