/* x11_conformance.c — the X11 half of tools/wl-conformance.mjs (docs/WAYLAND.md, "Conformance"): an xcb client
 * that checks our X window manager (src/wl/xwm.c) on Xwayland, as an X11 app sees it. One line per check:
 *
 *   CHECK <item> <check-id> PASS|FAIL|SKIP <hint> <message>
 *
 *   build/x11-conformance-client wm                       ICCCM and EWMH: root properties, a window's life
 *   build/x11-conformance-client clip-own <bytes> <incr> [primary]   owns CLIPBOARD (or PRIMARY), serves it 15 s
 *   build/x11-conformance-client clip-read <bytes>        reads CLIPBOARD once it has the keyboard
 *
 * Window titles start with "wlconf-": the driver's theme hook logs what the theme gets for them, and closes
 * the one titled "wlconf-close" (so WM_DELETE_WINDOW can be checked). Run it only against the headless
 * lord-server the driver starts, with its DISPLAY and XAUTHORITY. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <xcb/xcb.h>

static xcb_connection_t *X;
static xcb_screen_t *scr;
static int nfail;

static void check(const char *item, const char *id, int ok, const char *hint, const char *fmt, ...) {
    va_list ap;
    printf("CHECK %s %s %s %s ", item, id, ok ? "PASS" : "FAIL", hint);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    if (!ok) nfail++;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static xcb_atom_t atom(const char *name) {
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(X, xcb_intern_atom(X, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : 0;
    free(r);
    return a;
}
static const char *aname(xcb_atom_t a) {
    static char buf[128];
    xcb_get_atom_name_reply_t *r = xcb_get_atom_name_reply(X, xcb_get_atom_name(X, a), NULL);
    snprintf(buf, sizeof buf, "%.*s", r ? xcb_get_atom_name_name_length(r) : 0, r ? xcb_get_atom_name_name(r) : "");
    free(r);
    return buf;
}

/* a property as 32-bit values (up to max); count, or -1 if absent */
static int prop32(xcb_window_t w, xcb_atom_t p, uint32_t *out, int max) {
    xcb_get_property_reply_t *r = xcb_get_property_reply(X, xcb_get_property(X, 0, w, p, XCB_GET_PROPERTY_TYPE_ANY, 0, 4096), NULL);
    if (!r || r->type == XCB_NONE || r->format != 32) { free(r); return -1; }
    int n = xcb_get_property_value_length(r) / 4;
    if (n > max) n = max;
    memcpy(out, xcb_get_property_value(r), (size_t)n * 4);
    free(r);
    return n;
}
static int prop_str(xcb_window_t w, xcb_atom_t p, char *out, size_t cap) {
    xcb_get_property_reply_t *r = xcb_get_property_reply(X, xcb_get_property(X, 0, w, p, XCB_GET_PROPERTY_TYPE_ANY, 0, 65536), NULL);
    if (!r || r->type == XCB_NONE) { free(r); out[0] = 0; return -1; }
    int n = xcb_get_property_value_length(r);
    if ((size_t)n >= cap) n = (int)cap - 1;
    memcpy(out, xcb_get_property_value(r), (size_t)n);
    out[n] = 0;
    free(r);
    return n;
}
static int has(const uint32_t *v, int n, uint32_t a) { for (int i = 0; i < n; i++) if (v[i] == a) return 1; return 0; }

static void set32(xcb_window_t w, xcb_atom_t p, xcb_atom_t type, const uint32_t *v, int n) { xcb_change_property(X, XCB_PROP_MODE_REPLACE, w, p, type, 32, (uint32_t)n, v); }
static void set8(xcb_window_t w, xcb_atom_t p, xcb_atom_t type, const char *s, int len) { xcb_change_property(X, XCB_PROP_MODE_REPLACE, w, p, type, 8, (uint32_t)len, s); }

/* atoms */
static xcb_atom_t A_WM_PROTOCOLS, A_WM_DELETE, A_WM_TAKE_FOCUS, A_WM_STATE, A_WM_CHANGE_STATE, A_UTF8, A_NET_WM_NAME, A_NET_SUPPORTED,
    A_NET_CHECK, A_NET_ACTIVE, A_NET_STATE, A_FULLSCREEN, A_MAXV, A_MAXH, A_HIDDEN, A_ABOVE, A_ATTENTION, A_MODAL, A_FOCUSED, A_TYPE, A_TYPE_NORMAL,
    A_TYPE_DIALOG, A_PID, A_CLIENT_LIST, A_FRAME, A_WORKAREA, A_MOTIF, A_ICON, A_MOVERESIZE, A_CLIPBOARD, A_TARGETS, A_INCR, A_MIME_UTF8,
    A_PROP, A_RESOURCE_MANAGER, A_REQ_FRAME, A_STACKING, A_SKIP_TASKBAR, A_TYPE_UTILITY, A_CLOSE, A_BELOW, A_SKIP_PAGER;
static void atoms(void) {
    A_WM_PROTOCOLS = atom("WM_PROTOCOLS"); A_WM_DELETE = atom("WM_DELETE_WINDOW"); A_WM_TAKE_FOCUS = atom("WM_TAKE_FOCUS");
    A_WM_STATE = atom("WM_STATE"); A_WM_CHANGE_STATE = atom("WM_CHANGE_STATE"); A_UTF8 = atom("UTF8_STRING");
    A_NET_WM_NAME = atom("_NET_WM_NAME"); A_NET_SUPPORTED = atom("_NET_SUPPORTED"); A_NET_CHECK = atom("_NET_SUPPORTING_WM_CHECK");
    A_NET_ACTIVE = atom("_NET_ACTIVE_WINDOW"); A_NET_STATE = atom("_NET_WM_STATE"); A_FULLSCREEN = atom("_NET_WM_STATE_FULLSCREEN");
    A_MAXV = atom("_NET_WM_STATE_MAXIMIZED_VERT"); A_MAXH = atom("_NET_WM_STATE_MAXIMIZED_HORZ"); A_HIDDEN = atom("_NET_WM_STATE_HIDDEN");
    A_ABOVE = atom("_NET_WM_STATE_ABOVE"); A_ATTENTION = atom("_NET_WM_STATE_DEMANDS_ATTENTION"); A_MODAL = atom("_NET_WM_STATE_MODAL");
    A_FOCUSED = atom("_NET_WM_STATE_FOCUSED"); A_TYPE = atom("_NET_WM_WINDOW_TYPE"); A_TYPE_NORMAL = atom("_NET_WM_WINDOW_TYPE_NORMAL");
    A_TYPE_DIALOG = atom("_NET_WM_WINDOW_TYPE_DIALOG"); A_PID = atom("_NET_WM_PID"); A_CLIENT_LIST = atom("_NET_CLIENT_LIST");
    A_FRAME = atom("_NET_FRAME_EXTENTS"); A_WORKAREA = atom("_NET_WORKAREA"); A_MOTIF = atom("_MOTIF_WM_HINTS"); A_ICON = atom("_NET_WM_ICON");
    A_MOVERESIZE = atom("_NET_WM_MOVERESIZE"); A_CLIPBOARD = atom("CLIPBOARD"); A_TARGETS = atom("TARGETS"); A_INCR = atom("INCR");
    A_MIME_UTF8 = atom("text/plain;charset=utf-8"); A_PROP = atom("WLCONF_SEL"); A_RESOURCE_MANAGER = XCB_ATOM_RESOURCE_MANAGER;
    A_REQ_FRAME = atom("_NET_REQUEST_FRAME_EXTENTS"); A_STACKING = atom("_NET_CLIENT_LIST_STACKING");
    A_SKIP_TASKBAR = atom("_NET_WM_STATE_SKIP_TASKBAR"); A_TYPE_UTILITY = atom("_NET_WM_WINDOW_TYPE_UTILITY");
    A_CLOSE = atom("_NET_CLOSE_WINDOW"); A_BELOW = atom("_NET_WM_STATE_BELOW"); A_SKIP_PAGER = atom("_NET_WM_STATE_SKIP_PAGER");
}

/* ---------------------------------------------------------------- events */
typedef struct { int map, focus_in, take_focus, delete_win, configure, cw, ch, prop_state; } WinEv;
#define MAXW 12
static struct { xcb_window_t id; WinEv ev; } wins[MAXW];
static int nwins;
static WinEv *ev_of(xcb_window_t w) { for (int i = 0; i < nwins; i++) if (wins[i].id == w) return &wins[i].ev; return NULL; }

static void handle(xcb_generic_event_t *e) {
    switch (e->response_type & 0x7f) {
    case XCB_MAP_NOTIFY: { xcb_map_notify_event_t *m = (void *)e; WinEv *w = ev_of(m->window); if (w) w->map++; break; }
    case XCB_FOCUS_IN: { xcb_focus_in_event_t *f = (void *)e; WinEv *w = ev_of(f->event); if (w && f->detail != XCB_NOTIFY_DETAIL_POINTER) w->focus_in++; break; }
    case XCB_CONFIGURE_NOTIFY: { xcb_configure_notify_event_t *c = (void *)e; WinEv *w = ev_of(c->window); if (w) { w->configure++; w->cw = c->width; w->ch = c->height; } break; }
    case XCB_PROPERTY_NOTIFY: { xcb_property_notify_event_t *p = (void *)e; WinEv *w = ev_of(p->window); if (w && p->atom == A_NET_STATE) w->prop_state++; break; }
    case XCB_CLIENT_MESSAGE: {
        xcb_client_message_event_t *m = (void *)e;
        WinEv *w = ev_of(m->window);
        if (w && m->type == A_WM_PROTOCOLS) {
            if (m->data.data32[0] == A_WM_TAKE_FOCUS) w->take_focus++;
            if (m->data.data32[0] == A_WM_DELETE) w->delete_win++;
        }
        break;
    }
    }
}
/* events until cond() or ms passed */
static int wait_for(int (*cond)(void *), void *arg, int ms) {
    double end = now_ms() + ms;
    for (;;) {
        xcb_flush(X);
        xcb_generic_event_t *e;
        while ((e = xcb_poll_for_event(X))) { handle(e); free(e); }
        if (cond && cond(arg)) return 1;
        double left = end - now_ms();
        if (left <= 0) return 0;
        struct pollfd p = { xcb_get_file_descriptor(X), POLLIN, 0 };
        poll(&p, 1, left > 50 ? 50 : (int)left + 1);
        if (xcb_connection_has_error(X)) return 0;
    }
}
static int c_map(void *a) { return ((WinEv *)a)->map > 0; }
static int c_focus(void *a) { return ((WinEv *)a)->focus_in > 0 || ((WinEv *)a)->take_focus > 0; }
static int c_take(void *a) { return ((WinEv *)a)->take_focus > 0; }
static int c_delete(void *a) { return ((WinEv *)a)->delete_win > 0; }
static int c_never(void *a) { (void)a; return 0; }

/* a top-level window with the usual ICCCM/EWMH properties; not mapped yet */
static xcb_window_t make_window(const char *title, int input, int take_focus, xcb_window_t transient, xcb_atom_t type, int min_w, int min_h) {
    xcb_window_t w = xcb_generate_id(X);
    uint32_t vals[] = { scr->white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_FOCUS_CHANGE };
    xcb_create_window(X, XCB_COPY_FROM_PARENT, w, scr->root, 50, 50, 320, 240, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);
    set8(w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, title, (int)strlen(title));
    set8(w, A_NET_WM_NAME, A_UTF8, title, (int)strlen(title));
    static const char cls[] = "wlconf-x11\0Wlconf-X11";
    set8(w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, cls, sizeof cls);
    uint32_t pid = (uint32_t)getpid();
    set32(w, A_PID, XCB_ATOM_CARDINAL, &pid, 1);
    uint32_t protos[2] = { A_WM_DELETE, A_WM_TAKE_FOCUS };
    set32(w, A_WM_PROTOCOLS, XCB_ATOM_ATOM, protos, take_focus ? 2 : 1);
    uint32_t hints[9] = { 1 /* InputHint */, (uint32_t)input, 1 /* NormalState */ };
    set32(w, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, hints, 9);
    if (min_w) {
        uint32_t nh[18] = { 1u << 4 /* PMinSize */ };
        nh[5] = (uint32_t)min_w;
        nh[6] = (uint32_t)min_h;
        set32(w, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, nh, 18);
    }
    if (transient) set32(w, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, &transient, 1);
    if (type) set32(w, A_TYPE, XCB_ATOM_ATOM, &type, 1);
    if (nwins < MAXW) { wins[nwins].id = w; memset(&wins[nwins].ev, 0, sizeof(WinEv)); nwins++; }
    return w;
}
static void root_message(xcb_window_t w, xcb_atom_t type, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    xcb_client_message_event_t m = { .response_type = XCB_CLIENT_MESSAGE, .format = 32, .window = w, .type = type };
    m.data.data32[0] = d0; m.data.data32[1] = d1; m.data.data32[2] = d2; m.data.data32[3] = d3;
    xcb_send_event(X, 0, scr->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char *)&m);
    xcb_flush(X);
}
static int state_has(xcb_window_t w, xcb_atom_t a) { uint32_t v[32]; int n = prop32(w, A_NET_STATE, v, 32); return n > 0 && has(v, n, a); }
typedef struct { xcb_window_t w; xcb_atom_t a; int want; } StateWait;
static int c_state(void *arg) { StateWait *s = arg; return state_has(s->w, s->a) == s->want; }
static int wait_state(xcb_window_t w, xcb_atom_t a, int want, int ms) { StateWait s = { w, a, want }; return wait_for(c_state, &s, ms); }
/* _NET_CLIENT_LIST_STACKING: where w is (0 = bottom), -1 if absent; and waiting until it is the top one */
static int stack_index(xcb_window_t w, int *count) {
    uint32_t v[256];
    int n = prop32(scr->root, A_STACKING, v, 256);
    if (count) *count = n;
    for (int i = 0; i < n; i++) if (v[i] == w) return i;
    return -1;
}
static int c_top(void *arg) { int n = 0, i = stack_index(*(xcb_window_t *)arg, &n); return n > 0 && i == n - 1; }
static int wait_top(xcb_window_t w, int ms) { return wait_for(c_top, &w, ms); }
static xcb_window_t active_window(void) { uint32_t a = 0; prop32(scr->root, A_NET_ACTIVE, &a, 1); return a; }

/* ---------------------------------------------------------------- the window manager */
static void wm(void) {
    uint32_t v[256];
    char s[4096];
    /* the root */
    uint32_t check_w = 0;
    int ok = prop32(scr->root, A_NET_CHECK, &check_w, 1) == 1 && check_w;
    uint32_t again = 0;
    int ok2 = ok && prop32(check_w, A_NET_CHECK, &again, 1) == 1 && again == check_w;
    int named = ok && prop_str(check_w, A_NET_WM_NAME, s, sizeof s) > 0;
    check("ewmh.supporting_wm_check", "root_property", ok, "_NET_SUPPORTING_WM_CHECK", "root has _NET_SUPPORTING_WM_CHECK: 0x%x", check_w);
    check("ewmh.supporting_wm_check", "child_property", ok2, "_NET_SUPPORTING_WM_CHECK", "the check window points at itself: %s", ok2 ? "yes" : "no");
    check("ewmh.supporting_wm_check", "wm_name", named, "_NET_WM_NAME", "the check window has _NET_WM_NAME: \"%s\"", named ? s : "");
    int ns = prop32(scr->root, A_NET_SUPPORTED, v, 256);
    check("ewmh.net_supported", "present", ns > 0, "_NET_SUPPORTED", "_NET_SUPPORTED on the root: %d atoms", ns);
    struct { const char *item, *name; } sup[] = {
        { "ewmh.wm_name", "_NET_WM_NAME" }, { "ewmh.wm_state", "_NET_WM_STATE" }, { "ewmh.wm_state", "_NET_WM_STATE_FULLSCREEN" },
        { "ewmh.wm_state", "_NET_WM_STATE_MAXIMIZED_VERT" }, { "ewmh.wm_state", "_NET_WM_STATE_MAXIMIZED_HORZ" }, { "ewmh.wm_state", "_NET_WM_STATE_HIDDEN" },
        { "ewmh.wm_state", "_NET_WM_STATE_ABOVE" }, { "ewmh.wm_state", "_NET_WM_STATE_DEMANDS_ATTENTION" }, { "ewmh.wm_state", "_NET_WM_STATE_MODAL" },
        { "ewmh.wm_state", "_NET_WM_STATE_BELOW" }, { "ewmh.wm_state", "_NET_WM_STATE_SKIP_PAGER" }, { "ewmh.wm_state", "_NET_WM_STATE_SKIP_TASKBAR" },
        { "ewmh.window_type", "_NET_WM_WINDOW_TYPE" }, { "ewmh.window_type", "_NET_WM_WINDOW_TYPE_NORMAL" }, { "ewmh.window_type", "_NET_WM_WINDOW_TYPE_DIALOG" },
        { "ewmh.window_type", "_NET_WM_WINDOW_TYPE_UTILITY" }, { "ewmh.window_type", "_NET_WM_WINDOW_TYPE_POPUP_MENU" },
        { "ewmh.active_window", "_NET_ACTIVE_WINDOW" }, { "ewmh.client_list", "_NET_CLIENT_LIST" }, { "ewmh.client_list", "_NET_CLIENT_LIST_STACKING" },
        { "ewmh.wm_pid", "_NET_WM_PID" }, { "ewmh.wm_icon", "_NET_WM_ICON" }, { "ewmh.moveresize", "_NET_WM_MOVERESIZE" },
        { "ewmh.frame_extents", "_NET_FRAME_EXTENTS" }, { "ewmh.frame_extents", "_NET_REQUEST_FRAME_EXTENTS" }, { "ewmh.workarea", "_NET_WORKAREA" },
        { "ewmh.close_window", "_NET_CLOSE_WINDOW" },
    };
    for (size_t i = 0; i < sizeof sup / sizeof sup[0]; i++) {
        char id[96];
        snprintf(id, sizeof id, "supported.%s", sup[i].name);
        check(sup[i].item, id, ns > 0 && has(v, ns, atom(sup[i].name)), sup[i].name, "%s listed in _NET_SUPPORTED", sup[i].name);
    }
    int wa = prop32(scr->root, A_WORKAREA, v, 16);
    check("ewmh.workarea", "root_property", wa >= 4, "_NET_WORKAREA", "_NET_WORKAREA on the root: %d values", wa);
    /* Xft.dpi is written only when X pixels aren't logical ones (scale ≠ 100 %, xwscale.c); at 100 % apps use 96 */
    int rm = prop_str(scr->root, A_RESOURCE_MANAGER, s, sizeof s);
    if (rm > 0 && strstr(s, "Xft.dpi")) check("xwayland.hidpi", "xft_dpi", 1, "RESOURCE_MANAGER", "Xft.dpi in the root's RESOURCE_MANAGER: %s", strstr(s, "Xft.dpi"));
    else printf("CHECK xwayland.hidpi xft_dpi SKIP RESOURCE_MANAGER no Xft.dpi at this scale (correct at 100 %%; the scaled path is covered by xwscale-test)\n");

    /* a normal window: mapped by the WM, given WM_STATE, listed, focused, framed */
    xcb_window_t w1 = make_window("wlconf-x11", 1, 1, 0, A_TYPE_NORMAL, 300, 200);
    /* before it maps, it asks what its frame will be (EWMH _NET_REQUEST_FRAME_EXTENTS): the WM sets the property */
    root_message(w1, A_REQ_FRAME, 0, 0, 0, 0);
    wait_for(c_never, NULL, 400);
    int rfe = prop32(w1, A_FRAME, v, 4);
    check("ewmh.frame_extents", "request_answered", rfe == 4, "_NET_REQUEST_FRAME_EXTENTS", "_NET_FRAME_EXTENTS set before the map, on request: %d values", rfe);
    xcb_map_window(X, w1);
    WinEv *e1 = ev_of(w1);
    int mapped = wait_for(c_map, e1, 4000);
    check("xwayland.startup", "map", mapped, "MapRequest", "the WM maps a window that asks (MapNotify): %s", mapped ? "yes" : "no in 4 s");
    if (!mapped) return;
    int st = prop32(w1, A_WM_STATE, v, 2);
    check("icccm.wm_state", "normal", st >= 1 && v[0] == 1, "WM_STATE", "WM_STATE NormalState after map: %s", st >= 1 ? (v[0] == 1 ? "yes" : "other state") : "absent");
    int cl = prop32(scr->root, A_CLIENT_LIST, v, 256);
    check("ewmh.client_list", "contains", cl > 0 && has(v, cl, w1), "_NET_CLIENT_LIST", "_NET_CLIENT_LIST has the mapped window: %s", cl > 0 && has(v, cl, w1) ? "yes" : "no");
    int fe = prop32(w1, A_FRAME, v, 4);
    check("ewmh.frame_extents", "set", fe == 4, "_NET_FRAME_EXTENTS", "_NET_FRAME_EXTENTS set on the window: %d values", fe);
    int focused = wait_for(c_focus, e1, 3000);
    check("icccm.focus", "focus_in", focused, "SetInputFocus", "a mapped window with input=True gets FocusIn (or WM_TAKE_FOCUS): %s", focused ? "yes" : "no in 3 s");
    uint32_t act = 0;
    wait_for(c_never, NULL, 300);
    int ha = prop32(scr->root, A_NET_ACTIVE, &act, 1) == 1;
    check("ewmh.active_window", "root_property", ha && act == w1, "_NET_ACTIVE_WINDOW", "_NET_ACTIVE_WINDOW is the focused window: 0x%x (ours 0x%x)", act, w1);
    check("ewmh.wm_state", "focused", state_has(w1, A_FOCUSED), "_NET_WM_STATE_FOCUSED", "_NET_WM_STATE_FOCUSED on the focused window: %s", state_has(w1, A_FOCUSED) ? "yes" : "no");
    int sk = prop32(scr->root, A_STACKING, v, 256);
    check("ewmh.client_list", "stacking_top", sk > 0 && v[sk - 1] == w1, "_NET_CLIENT_LIST_STACKING", "_NET_CLIENT_LIST_STACKING has the focused window on top: %s", sk > 0 && v[sk - 1] == w1 ? "yes" : "no");

    /* a title change after map (the driver checks the theme got it) */
    set8(w1, A_NET_WM_NAME, A_UTF8, "wlconf-x11-renamed", 18);
    xcb_flush(X);
    wait_for(c_never, NULL, 400);

    /* _NET_WM_STATE requests (source 1: a normal application) */
    root_message(w1, A_NET_STATE, 1, A_FULLSCREEN, 0, 1);
    int fs = wait_state(w1, A_FULLSCREEN, 1, 2000);
    wait_for(c_never, NULL, 300);
    check("ewmh.wm_state", "fullscreen_add", fs, "_NET_WM_STATE_FULLSCREEN", "add fullscreen: property %s, size %dx%d (screen %dx%d)", fs ? "set" : "not set", e1->cw, e1->ch, scr->width_in_pixels, scr->height_in_pixels);
    check("ewmh.wm_state", "fullscreen_size", e1->cw == scr->width_in_pixels && e1->ch == scr->height_in_pixels, "_NET_WM_STATE_FULLSCREEN", "fullscreen covers the screen: %dx%d", e1->cw, e1->ch);
    root_message(w1, A_NET_STATE, 0, A_FULLSCREEN, 0, 1);
    int fs0 = wait_state(w1, A_FULLSCREEN, 0, 2000);
    check("ewmh.wm_state", "fullscreen_remove", fs0, "_NET_WM_STATE_FULLSCREEN", "remove fullscreen: %s", fs0 ? "removed" : "still set");
    root_message(w1, A_NET_STATE, 1, A_MAXV, A_MAXH, 1);
    int mx = wait_state(w1, A_MAXV, 1, 2000) && state_has(w1, A_MAXH);
    check("ewmh.wm_state", "maximize_add", mx, "_NET_WM_STATE_MAXIMIZED_VERT", "add maximized vert+horz: %s", mx ? "set" : "not set");
    root_message(w1, A_NET_STATE, 0, A_MAXV, A_MAXH, 1);
    int mx0 = wait_state(w1, A_MAXV, 0, 2000);
    check("ewmh.wm_state", "maximize_remove", mx0, "_NET_WM_STATE_MAXIMIZED_VERT", "remove maximized: %s", mx0 ? "removed" : "still set");
    struct { const char *id; xcb_atom_t a; } more[] = { { "above", A_ABOVE }, { "modal", A_MODAL }, { "skip_taskbar", A_SKIP_TASKBAR },
                                                        { "skip_pager", A_SKIP_PAGER }, { "below", A_BELOW } };
    for (int i = 0; i < (int)(sizeof more / sizeof more[0]); i++) {
        root_message(w1, A_NET_STATE, 1, more[i].a, 0, 1);
        int on = wait_state(w1, more[i].a, 1, 1500);
        char id[64];
        snprintf(id, sizeof id, "%s_add", more[i].id);
        check("ewmh.wm_state", id, on, aname(more[i].a), "add %s: %s", aname(more[i].a), on ? "set" : "not set");
        root_message(w1, A_NET_STATE, 0, more[i].a, 0, 1);
        int off = wait_state(w1, more[i].a, 0, 1500);
        snprintf(id, sizeof id, "%s_remove", more[i].id);
        check("ewmh.wm_state", id, off, aname(more[i].a), "remove %s: %s", aname(more[i].a), off ? "removed" : "still set");
    }
    /* the window in front never demands attention (KWin and Mutter clear it on the active window) */
    root_message(w1, A_NET_STATE, 1, A_ATTENTION, 0, 1);
    wait_for(c_never, NULL, 500);
    check("ewmh.wm_state", "demands_attention_focused", !state_has(w1, A_ATTENTION), "_NET_WM_STATE_DEMANDS_ATTENTION", "the focused window asking for attention: %s", state_has(w1, A_ATTENTION) ? "set (it already has it)" : "not set, as KWin and Mutter do");
    /* below its minimum size (WM_NORMAL_HINTS 300×200): a ConfigureRequest for 100×80 */
    uint32_t sz[2] = { 100, 80 };
    xcb_configure_window(X, w1, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, sz);
    wait_for(c_never, NULL, 600);
    xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(X, xcb_get_geometry(X, w1), NULL);
    int gw = g ? g->width : 0, gh = g ? g->height : 0;
    free(g);
    check("icccm.wm_normal_hints", "min_size", gw >= 300 && gh >= 200, "WM_NORMAL_HINTS", "a ConfigureRequest below the minimum (300×200) gives %dx%d", gw, gh);
    uint32_t nh[18] = { (1u << 4) | (1u << 5) };            /* PMinSize | PMaxSize: 300×200 to 500×400 */
    nh[5] = 300; nh[6] = 200; nh[7] = 500; nh[8] = 400;
    set32(w1, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, nh, 18);
    uint32_t big[2] = { 900, 900 };
    xcb_configure_window(X, w1, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, big);
    wait_for(c_never, NULL, 600);
    g = xcb_get_geometry_reply(X, xcb_get_geometry(X, w1), NULL);
    gw = g ? g->width : 0; gh = g ? g->height : 0;
    free(g);
    check("icccm.wm_normal_hints", "max_size", gw <= 500 && gh <= 400 && gw >= 300, "WM_NORMAL_HINTS", "a ConfigureRequest above the maximum (500×400) gives %dx%d", gw, gh);

    /* a transient dialog */
    xcb_window_t w2 = make_window("wlconf-x11-dialog", 1, 0, w1, A_TYPE_DIALOG, 0, 0);
    xcb_map_window(X, w2);
    int m2 = wait_for(c_map, ev_of(w2), 3000);
    check("icccm.transient_for", "mapped", m2, "WM_TRANSIENT_FOR", "a transient dialog maps: %s", m2 ? "yes" : "no");
    /* input=False with WM_TAKE_FOCUS: the WM sends WM_TAKE_FOCUS instead of setting the focus */
    xcb_window_t w3 = make_window("wlconf-x11-takefocus", 0, 1, 0, A_TYPE_NORMAL, 0, 0);
    xcb_map_window(X, w3);
    wait_for(c_map, ev_of(w3), 3000);
    int tf = wait_for(c_take, ev_of(w3), 3000);
    check("icccm.wm_protocols", "take_focus", tf, "WM_TAKE_FOCUS", "a window with input=False and WM_TAKE_FOCUS is sent WM_TAKE_FOCUS when focused: %s", tf ? "yes" : "no in 3 s");
    /* the theme closes this one (the driver's hook): WM_DELETE_WINDOW */
    xcb_window_t w4 = make_window("wlconf-close", 1, 0, 0, A_TYPE_NORMAL, 0, 0);
    xcb_map_window(X, w4);
    wait_for(c_map, ev_of(w4), 3000);
    int del = wait_for(c_delete, ev_of(w4), 4000);
    check("icccm.wm_protocols", "delete_window", del, "WM_DELETE_WINDOW", "closing the window from LordOS sends WM_DELETE_WINDOW: %s", del ? "yes" : "no in 4 s");
    xcb_destroy_window(X, w4);
    /* Motif hints: no decorations (the driver checks the theme draws no frame) */
    xcb_window_t w5 = make_window("wlconf-x11-motif", 1, 0, 0, A_TYPE_NORMAL, 0, 0);
    uint32_t motif[5] = { 2 /* MWM_HINTS_DECORATIONS */, 0, 0, 0, 0 };
    set32(w5, A_MOTIF, A_MOTIF, motif, 5);
    xcb_map_window(X, w5);
    wait_for(c_map, ev_of(w5), 3000);
    /* an icon (the driver checks the theme got it) */
    uint32_t icon[2 + 16 * 16];
    icon[0] = icon[1] = 16;
    for (int i = 0; i < 256; i++) icon[2 + i] = 0xffff0000;
    set32(w5, A_ICON, XCB_ATOM_CARDINAL, icon, 2 + 256);
    xcb_flush(X);
    wait_for(c_never, NULL, 500);

    /* w1 is no longer in front: it may ask for attention, with _NET_WM_STATE or the ICCCM urgency hint */
    uint32_t act2 = 0;
    prop32(scr->root, A_NET_ACTIVE, &act2, 1);
    if (act2 != w1) {
        root_message(w1, A_NET_STATE, 1, A_ATTENTION, 0, 1);
        int at = wait_state(w1, A_ATTENTION, 1, 1500);
        check("ewmh.wm_state", "demands_attention_add", at, "_NET_WM_STATE_DEMANDS_ATTENTION", "add _NET_WM_STATE_DEMANDS_ATTENTION on a window not in front: %s", at ? "set" : "not set");
        root_message(w1, A_NET_STATE, 0, A_ATTENTION, 0, 1);
        int at0 = wait_state(w1, A_ATTENTION, 0, 1500);
        check("ewmh.wm_state", "demands_attention_remove", at0, "_NET_WM_STATE_DEMANDS_ATTENTION", "remove it: %s", at0 ? "removed" : "still set");
        uint32_t urg[9] = { 1 | (1u << 8), 1, 1 };        /* InputHint | XUrgencyHint */
        set32(w1, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, urg, 9);
        int ur = wait_state(w1, A_ATTENTION, 1, 1500);
        check("icccm.wm_hints", "urgency", ur, "WM_HINTS", "the urgency hint gives _NET_WM_STATE_DEMANDS_ATTENTION: %s", ur ? "yes" : "no");
        uint32_t calm[9] = { 1, 1, 1 };
        set32(w1, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, calm, 9);
        int ur0 = wait_state(w1, A_ATTENTION, 0, 1500);
        check("icccm.wm_hints", "urgency_cleared", ur0, "WM_HINTS", "the urgency hint cleared clears it: %s", ur0 ? "yes" : "no");
    } else {
        printf("CHECK ewmh.wm_state demands_attention_add SKIP _NET_WM_STATE_DEMANDS_ATTENTION the first window is still in front\n");
        printf("CHECK icccm.wm_hints urgency SKIP WM_HINTS the first window is still in front\n");
    }
    /* a utility window (a palette) maps as a window */
    xcb_window_t w6 = make_window("wlconf-x11-utility", 1, 0, w1, A_TYPE_UTILITY, 0, 0);
    xcb_map_window(X, w6);
    int m6 = wait_for(c_map, ev_of(w6), 3000);
    check("ewmh.window_type", "utility_maps", m6, "_NET_WM_WINDOW_TYPE_UTILITY", "a utility window maps: %s", m6 ? "yes" : "no");
    /* a pager closes a window: _NET_CLOSE_WINDOW becomes WM_DELETE_WINDOW */
    xcb_window_t w7 = make_window("wlconf-x11-netclose", 1, 0, 0, A_TYPE_NORMAL, 0, 0);
    xcb_map_window(X, w7);
    wait_for(c_map, ev_of(w7), 3000);
    root_message(w7, A_CLOSE, XCB_CURRENT_TIME, 2, 0, 0);
    int nc = wait_for(c_delete, ev_of(w7), 3000);
    check("ewmh.close_window", "close_request", nc, "_NET_CLOSE_WINDOW", "_NET_CLOSE_WINDOW from a pager sends WM_DELETE_WINDOW: %s", nc ? "yes" : "no in 3 s");
    xcb_destroy_window(X, w7);
    xcb_destroy_window(X, w6);

    /* Always on top: a window that asks for _NET_WM_STATE_ABOVE before it maps stays over a normal window that maps
     * after it, and that window has the keyboard (stacking and focus apart, sdk/desktopmodel.js stackOrder) */
    xcb_window_t w_above = make_window("wlconf-x11-above", 1, 0, 0, A_TYPE_NORMAL, 0, 0);
    set32(w_above, A_NET_STATE, XCB_ATOM_ATOM, &A_ABOVE, 1);
    xcb_map_window(X, w_above);
    int ma = wait_for(c_map, ev_of(w_above), 3000);
    check("ewmh.wm_state", "above_initial", ma && state_has(w_above, A_ABOVE), "_NET_WM_STATE_ABOVE", "asked for before the map, kept: %s", ma && state_has(w_above, A_ABOVE) ? "yes" : "no");
    xcb_window_t wunder = make_window("wlconf-x11-under", 1, 0, 0, A_TYPE_NORMAL, 0, 0);
    xcb_map_window(X, wunder);
    wait_for(c_map, ev_of(wunder), 3000);
    wait_for(c_focus, ev_of(wunder), 3000);
    int ontop = wait_top(w_above, 2000);
    xcb_window_t act_now = active_window();
    check("ewmh.wm_state", "above_stays_on_top", ontop && act_now == wunder, "_NET_WM_STATE_ABOVE",
          "a normal window mapped after it has the keyboard (%s) and the always-on-top one stays at the top of _NET_CLIENT_LIST_STACKING (%s)",
          act_now == wunder ? "yes" : "no", ontop ? "yes" : "no");
    root_message(w_above, A_NET_STATE, 2, A_ABOVE, 0, 1);              /* toggle: off */
    int off_now = wait_state(w_above, A_ABOVE, 0, 1500) && wait_top(wunder, 1500);
    check("ewmh.wm_state", "above_toggle_off", off_now, "_NET_WM_STATE_ABOVE", "toggled off: the window with the keyboard is on top again: %s", off_now ? "yes" : "no");
    root_message(w_above, A_NET_STATE, 2, A_ABOVE, 0, 1);              /* toggle: on */
    int on_again = wait_state(w_above, A_ABOVE, 1, 1500) && wait_top(w_above, 1500) && active_window() == wunder;
    check("ewmh.wm_state", "above_toggle_on", on_again, "_NET_WM_STATE_ABOVE", "toggled on: over it again, the keyboard not taken: %s", on_again ? "yes" : "no");
    root_message(w_above, A_NET_STATE, 1, A_BELOW, 0, 1);              /* kept below: it excludes above, and goes under */
    int bl = wait_state(w_above, A_BELOW, 1, 1500) && wait_state(w_above, A_ABOVE, 0, 1500) && wait_top(wunder, 1500);
    int ia = stack_index(w_above, NULL), ib = stack_index(wunder, NULL);
    check("ewmh.wm_state", "below_under", bl && ia >= 0 && ia < ib, "_NET_WM_STATE_BELOW",
          "_NET_WM_STATE_BELOW drops ABOVE and stacks it under the normal window (%d < %d): %s", ia, ib, bl && ia >= 0 && ia < ib ? "yes" : "no");
    xcb_destroy_window(X, wunder);
    xcb_destroy_window(X, w_above);
    xcb_flush(X);
    wait_for(c_never, NULL, 400);

    /* iconify (WM_CHANGE_STATE): WM_STATE Iconic and _NET_WM_STATE_HIDDEN */
    xcb_client_message_event_t m = { .response_type = XCB_CLIENT_MESSAGE, .format = 32, .window = w1, .type = A_WM_CHANGE_STATE };
    m.data.data32[0] = 3;
    xcb_send_event(X, 0, scr->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char *)&m);
    int hid = wait_state(w1, A_HIDDEN, 1, 2000);
    check("ewmh.wm_state", "hidden", hid, "_NET_WM_STATE_HIDDEN", "WM_CHANGE_STATE Iconic gives _NET_WM_STATE_HIDDEN: %s", hid ? "yes" : "no");
    st = prop32(w1, A_WM_STATE, v, 2);
    check("icccm.wm_state", "iconic", st >= 1 && v[0] == 3, "WM_CHANGE_STATE", "WM_STATE IconicState after WM_CHANGE_STATE: %s", st >= 1 ? (v[0] == 3 ? "yes" : "other state") : "absent");
    /* _NET_ACTIVE_WINDOW request from a pager-like source brings it back */
    root_message(w1, A_NET_ACTIVE, 2, XCB_CURRENT_TIME, 0, 0);
    int back = wait_state(w1, A_HIDDEN, 0, 2000);
    check("ewmh.active_window", "request", back, "_NET_ACTIVE_WINDOW", "a _NET_ACTIVE_WINDOW request restores a minimized window: %s", back ? "yes" : "no");
    /* destroyed: gone from the client list */
    xcb_destroy_window(X, w1);
    wait_for(c_never, NULL, 600);
    cl = prop32(scr->root, A_CLIENT_LIST, v, 256);
    check("ewmh.client_list", "removed", cl >= 0 && !has(v, cl, w1), "_NET_CLIENT_LIST", "a destroyed window leaves _NET_CLIENT_LIST: %s", cl >= 0 && !has(v, cl, w1) ? "yes" : "no");
    xcb_destroy_window(X, w2);
    xcb_destroy_window(X, w3);
    xcb_destroy_window(X, w5);
    xcb_flush(X);
}

/* ---------------------------------------------------------------- selections */
static uint32_t fnv(const unsigned char *p, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; } return h; }

static xcb_window_t focused_window(const char *title) {
    xcb_window_t w = make_window(title, 1, 0, 0, A_TYPE_NORMAL, 0, 0);
    xcb_map_window(X, w);
    wait_for(c_map, ev_of(w), 4000);
    wait_for(c_focus, ev_of(w), 3000);
    return w;
}
static xcb_timestamp_t server_time(xcb_window_t w) {
    /* a zero-length property append: its PropertyNotify carries the server time */
    xcb_change_property(X, XCB_PROP_MODE_APPEND, w, A_PROP, XCB_ATOM_STRING, 8, 0, "");
    xcb_flush(X);
    double end = now_ms() + 2000;
    while (now_ms() < end) {
        xcb_generic_event_t *e = xcb_wait_for_event(X);
        if (!e) break;
        if ((e->response_type & 0x7f) == XCB_PROPERTY_NOTIFY && ((xcb_property_notify_event_t *)e)->atom == A_PROP) { xcb_timestamp_t t = ((xcb_property_notify_event_t *)e)->time; free(e); return t; }
        handle(e);
        free(e);
    }
    return XCB_CURRENT_TIME;
}

static int clip_own(long bytes, int incr, int primary) {
    xcb_window_t w = focused_window("wlconf-clip-own");
    xcb_atom_t sel = primary ? XCB_ATOM_PRIMARY : A_CLIPBOARD;
    char *data = malloc((size_t)bytes);
    for (long i = 0; i < bytes; i++) data[i] = (char)('A' + i % 26);
    xcb_timestamp_t t = server_time(w);
    xcb_set_selection_owner(X, w, sel, t);
    xcb_get_selection_owner_reply_t *r = xcb_get_selection_owner_reply(X, xcb_get_selection_owner(X, sel), NULL);
    int own = r && r->owner == w;
    free(r);
    printf("READY %s %08x\n", own ? "owner" : "not-owner", fnv((unsigned char *)data, (size_t)bytes));
    fflush(stdout);
    /* serve requests; an INCR transfer per requestor */
    struct { xcb_window_t req; xcb_atom_t prop; size_t off; int active; } tr = { 0, 0, 0, 0 };
    double end = now_ms() + 15000;
    int served = 0;
    while (now_ms() < end) {
        xcb_flush(X);
        xcb_generic_event_t *e = xcb_poll_for_event(X);
        if (!e) { struct pollfd p = { xcb_get_file_descriptor(X), POLLIN, 0 }; poll(&p, 1, 100); continue; }
        uint8_t type = e->response_type & 0x7f;
        if (type == XCB_SELECTION_REQUEST) {
            xcb_selection_request_event_t *q = (void *)e;
            xcb_selection_notify_event_t n = { .response_type = XCB_SELECTION_NOTIFY, .time = q->time, .requestor = q->requestor, .selection = q->selection, .target = q->target, .property = q->property ? q->property : q->target };
            if (q->target == A_TARGETS) {
                uint32_t tg[3] = { A_TARGETS, A_UTF8, A_MIME_UTF8 };
                xcb_change_property(X, XCB_PROP_MODE_REPLACE, q->requestor, n.property, XCB_ATOM_ATOM, 32, 3, tg);
            } else if (q->target == A_UTF8 || q->target == A_MIME_UTF8 || q->target == XCB_ATOM_STRING) {
                if (incr) {
                    uint32_t len = (uint32_t)bytes;
                    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
                    xcb_change_window_attributes(X, q->requestor, XCB_CW_EVENT_MASK, &mask);
                    xcb_change_property(X, XCB_PROP_MODE_REPLACE, q->requestor, n.property, A_INCR, 32, 1, &len);
                    tr.req = q->requestor; tr.prop = n.property; tr.off = 0; tr.active = 1;
                } else xcb_change_property(X, XCB_PROP_MODE_REPLACE, q->requestor, n.property, q->target, 8, (uint32_t)bytes, data);
                served++;
            } else n.property = XCB_NONE;
            xcb_send_event(X, 0, q->requestor, 0, (const char *)&n);
        } else if (type == XCB_PROPERTY_NOTIFY && tr.active) {
            xcb_property_notify_event_t *p = (void *)e;
            if (p->window == tr.req && p->atom == tr.prop && p->state == XCB_PROPERTY_DELETE) {
                size_t chunk = (size_t)bytes - tr.off > 65536 ? 65536 : (size_t)bytes - tr.off;
                xcb_change_property(X, XCB_PROP_MODE_REPLACE, tr.req, tr.prop, A_UTF8, 8, (uint32_t)chunk, data + tr.off);
                tr.off += chunk;
                if (!chunk) tr.active = 0;
            }
        } else if (type == XCB_SELECTION_CLEAR) {
            printf("CLEARED\n");
            fflush(stdout);
        } else handle(e);
        free(e);
    }
    printf("SERVED %d\n", served);
    return 0;
}

static int clip_read(long want) {
    xcb_window_t w = focused_window("wlconf-clip-read");
    xcb_timestamp_t t = server_time(w);
    long got = -1;
    char *buf = malloc((size_t)want + 65536);
    size_t n = 0;
    int incr = 0;
    for (int attempt = 0; attempt < 5 && got < 0; attempt++) {
        xcb_convert_selection(X, w, A_CLIPBOARD, A_UTF8, A_PROP, t);
        xcb_flush(X);
        double end = now_ms() + 4000;
        int done = 0;
        while (!done && now_ms() < end) {
            xcb_generic_event_t *e = xcb_poll_for_event(X);
            if (!e) { struct pollfd p = { xcb_get_file_descriptor(X), POLLIN, 0 }; poll(&p, 1, 100); xcb_flush(X); continue; }
            uint8_t type = e->response_type & 0x7f;
            if (type == XCB_SELECTION_NOTIFY && !incr) {
                xcb_selection_notify_event_t *s = (void *)e;
                if (s->property == XCB_NONE) { done = 1; }
                else {
                    xcb_get_property_reply_t *r = xcb_get_property_reply(X, xcb_get_property(X, 1, w, A_PROP, XCB_GET_PROPERTY_TYPE_ANY, 0, 0x7fffffff), NULL);
                    if (r && r->type == A_INCR) incr = 1;
                    else if (r) { int l = xcb_get_property_value_length(r); memcpy(buf, xcb_get_property_value(r), (size_t)l); n = (size_t)l; got = (long)n; done = 1; }
                    free(r);
                    xcb_flush(X);
                }
            } else if (type == XCB_PROPERTY_NOTIFY && incr) {
                xcb_property_notify_event_t *p = (void *)e;
                if (p->atom == A_PROP && p->state == XCB_PROPERTY_NEW_VALUE) {
                    xcb_get_property_reply_t *r = xcb_get_property_reply(X, xcb_get_property(X, 1, w, A_PROP, XCB_GET_PROPERTY_TYPE_ANY, 0, 0x7fffffff), NULL);
                    int l = r ? xcb_get_property_value_length(r) : 0;
                    if (l == 0) { got = (long)n; done = 1; }
                    else if (n + (size_t)l <= (size_t)want + 65536) { memcpy(buf + n, xcb_get_property_value(r), (size_t)l); n += (size_t)l; }
                    free(r);
                    xcb_flush(X);
                }
            } else handle(e);
            free(e);
        }
        if (got < 0 && !incr) wait_for(c_never, NULL, 500);
    }
    printf("CLIP %ld %08x incr=%d\n", got, got > 0 ? fnv((unsigned char *)buf, (size_t)got) : 0, incr);
    fflush(stdout);
    return got == want ? 0 : 1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    double t0 = now_ms();
    int sn = 0;
    X = xcb_connect(NULL, &sn);
    int bad = xcb_connection_has_error(X);
    if (argc >= 2 && !strcmp(argv[1], "wm"))
        check("xwayland.startup", "connect", !bad, "xwl_init", "an X11 client connects (Xwayland started on demand) in %.0f ms%s", now_ms() - t0, bad ? ": failed" : "");
    if (bad) return 2;
    scr = xcb_setup_roots_iterator(xcb_get_setup(X)).data;
    atoms();
    if (argc >= 2 && !strcmp(argv[1], "wm")) { wm(); xcb_disconnect(X); return 0; }
    if (argc >= 4 && !strcmp(argv[1], "clip-own")) return clip_own(atol(argv[2]), atoi(argv[3]), argc > 4 && !strcmp(argv[4], "primary"));
    if (argc >= 3 && !strcmp(argv[1], "clip-read")) return clip_read(atol(argv[2]));
    /* the EWMH request a pager or `wmctrl -b` sends (xprop only sets properties, and the WM owns _NET_WM_STATE once a
     * window is mapped): netstate <window> add|remove|toggle <_NET_WM_STATE_...>; tools/wl-conformance/x11-proofs.sh */
    if (argc >= 5 && !strcmp(argv[1], "netstate")) {
        uint32_t w = (uint32_t)strtoul(argv[2], NULL, 0);
        uint32_t how = !strcmp(argv[3], "add") ? 1 : !strcmp(argv[3], "toggle") ? 2 : 0;
        root_message(w, A_NET_STATE, how, atom(argv[4]), 0, 1);
        free(xcb_get_input_focus_reply(X, xcb_get_input_focus(X), NULL));   /* a round trip: sent before we hang up */
        xcb_disconnect(X);
        return 0;
    }
    fprintf(stderr, "usage: x11-conformance-client wm | clip-own <bytes> <incr 0|1> [primary] | clip-read <bytes> | netstate <window> add|remove|toggle <atom>\n");
    return 2;
}
