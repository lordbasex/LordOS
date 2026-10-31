/* wl_textinput.c — zwp_text_input_v3: a Linux app tells the compositor that a text field took focus
 * and where its caret is. LordOS uses it only to open the on-screen keyboard and keep the field in
 * view (docs/ON-SCREEN-KEYBOARD.md); the text itself keeps arriving as keys on wl_keyboard, so nothing
 * is ever sent back with commit_string, and surrounding text and content types are ignored. */
#include "wl.h"
#include "wl_input.h"
#include "text-input-unstable-v3-protocol.h"

#include <stdlib.h>
#include <wayland-server-core.h>

typedef struct {
    struct wl_resource *res;
    struct wl_client *client;
    struct wl_list link;
    int entered;                        /* on the surface with the keyboard */
    int want, active;                   /* pending enable, enabled since the last commit */
    int rx, ry, rw, rh, px, py, pw, ph; /* cursor rectangle, committed and pending */
} TextInput;

static struct wl_list inputs;
static struct wl_resource *focus_surface;
static struct wl_client *focus_client;
static void (*report)(int on, int x, int y, int w, int h);

static void notify(const TextInput *ti, int on) { if (report) report(on, ti->rx, ti->ry, ti->rw, ti->rh); }

static void ti_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void ti_enable(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    TextInput *ti = wl_resource_get_user_data(r);
    ti->want = 1;
    ti->px = ti->py = ti->pw = ti->ph = 0;
}
static void ti_disable(struct wl_client *c, struct wl_resource *r) { (void)c; ((TextInput *)wl_resource_get_user_data(r))->want = 0; }
static void ti_surrounding(struct wl_client *c, struct wl_resource *r, const char *t, int32_t a, int32_t b) { (void)c; (void)r; (void)t; (void)a; (void)b; }
static void ti_cause(struct wl_client *c, struct wl_resource *r, uint32_t v) { (void)c; (void)r; (void)v; }
static void ti_content(struct wl_client *c, struct wl_resource *r, uint32_t h, uint32_t p) { (void)c; (void)r; (void)h; (void)p; }
static void ti_cursor(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c;
    TextInput *ti = wl_resource_get_user_data(r);
    ti->px = x; ti->py = y; ti->pw = w; ti->ph = h;
}
static void ti_commit(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    TextInput *ti = wl_resource_get_user_data(r);
    int was = ti->active, moved = ti->rx != ti->px || ti->ry != ti->py || ti->rh != ti->ph;
    ti->active = ti->want;
    ti->rx = ti->px; ti->ry = ti->py; ti->rw = ti->pw; ti->rh = ti->ph;
    if (ti->entered && (ti->active != was || (ti->active && moved))) notify(ti, ti->active);
}

static const struct zwp_text_input_v3_interface ti_impl = {
    .destroy = ti_destroy, .enable = ti_enable, .disable = ti_disable, .set_surrounding_text = ti_surrounding,
    .set_text_change_cause = ti_cause, .set_content_type = ti_content, .set_cursor_rectangle = ti_cursor, .commit = ti_commit,
};

static void ti_gone(struct wl_resource *r) {
    TextInput *ti = wl_resource_get_user_data(r);
    if (ti->entered && ti->active) notify(ti, 0);
    wl_list_remove(&ti->link);
    free(ti);
}

static void mgr_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void mgr_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *seat) {
    (void)seat;
    TextInput *ti = calloc(1, sizeof *ti);
    struct wl_resource *res = ti ? wl_resource_create(c, &zwp_text_input_v3_interface, wl_resource_get_version(r), id) : NULL;
    if (!res) { free(ti); wl_client_post_no_memory(c); return; }
    ti->res = res;
    ti->client = c;
    wl_resource_set_implementation(res, &ti_impl, ti, ti_gone);
    wl_list_insert(&inputs, &ti->link);
    if (focus_client == c) { ti->entered = 1; zwp_text_input_v3_send_enter(res, focus_surface); }
}
static const struct zwp_text_input_manager_v3_interface mgr_impl = { .destroy = mgr_destroy, .get_text_input = mgr_get };
static void mgr_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    (void)d;
    struct wl_resource *r = wl_resource_create(c, &zwp_text_input_manager_v3_interface, (int)ver, id);
    if (r) wl_resource_set_implementation(r, &mgr_impl, NULL, NULL);
}

void wl_textinput_focus(struct wl_client *client, struct wl_resource *surface) {
    TextInput *ti;
    wl_list_for_each(ti, &inputs, link) {
        if (!ti->entered) continue;
        ti->entered = 0;
        if (ti->active) notify(ti, 0);
        ti->active = ti->want = 0;
        if (focus_surface) zwp_text_input_v3_send_leave(ti->res, focus_surface);
    }
    focus_client = client;
    focus_surface = surface;
    if (!client) return;
    wl_list_for_each(ti, &inputs, link) {
        if (ti->client != client) continue;
        ti->entered = 1;
        zwp_text_input_v3_send_enter(ti->res, surface);
    }
}

/* Its resource is going away: no leave event, the client knows. */
void wl_textinput_gone(void) {
    TextInput *ti;
    wl_list_for_each(ti, &inputs, link) ti->entered = ti->active = ti->want = 0;
    focus_client = NULL;
    focus_surface = NULL;
}

void wl_textinput_globals(struct wl_display *dpy, void (*fn)(int on, int x, int y, int w, int h)) {
    wl_list_init(&inputs);
    report = fn;
    wl_global_create(dpy, &zwp_text_input_manager_v3_interface, 1, NULL, mgr_bind);
}
