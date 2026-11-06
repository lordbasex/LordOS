/* xwm.c — LordOS's X window manager for Xwayland (see xwm.h).
 *
 * It speaks the X11 protocol itself (x11wire.c), on the connection Xwayland opened for it with
 * `-wm`, fully asynchronously: it never waits for a reply, so lord-server can't stall on
 * Xwayland while Xwayland waits on the compositor. Replies are matched by sequence number to a
 * FIFO of callbacks.
 *
 * What it does:
 *   - redirects the root's children (Composite, manual: rootless Xwayland only gives redirected
 *     windows a wl_surface), takes SubstructureRedirect on the root and maps the windows that ask
 *     (MapRequest); reads their
 *     title, class (WM_CLASS → the app's .desktop), protocols, hints, Motif decorations, window type
 *     and transient parent; WL_SURFACE_ID tells which wl_surface shows each one;
 *   - keeps every X window where LordOS shows it (the X position is the content's position on
 *     screen, so Xwayland routes the pointer right), and stacks the focused one on top;
 *   - focus (SetInputFocus / WM_TAKE_FOCUS), close (WM_DELETE_WINDOW or KillClient), fullscreen,
 *     maximize and minimize requests (_NET_WM_STATE, WM_CHANGE_STATE), moves and resizes asked by
 *     apps that draw their own frame (_NET_WM_MOVERESIZE);
 *   - an X app may not move the keyboard focus to another X app's window (FocusIn from another
 *     client is reverted);
 *   - the clipboard: CLIPBOARD and PRIMARY both ways (XFixes tells when an X app copies; the copy is
 *     read once and becomes LordOS's; LordOS's is offered to X only while an X window has the
 *     keyboard, so a background X app can't read what was copied elsewhere, and a secret copied in
 *     Passwords never is);
 *   - the root window's cursor is LordOS's arrow (Render cursor);
 *   - 10 s after the last X window closes, if X-Resource shows no other client, Xwayland stops.
 * X11 apps on one display can always see each other; what this guards is the border with Wayland
 * apps and LordOS (docs/PRIVACY.md, "X11 apps").
 * See docs/WAYLAND.md, "X11 apps (Xwayland)". */
#include "xwm.h"
#include "x11wire.h"
#include "wl.h"
#include "../cursor.h"
#include "../term/term.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

void xwl_wm_lost(void);                 /* xwayland.c: the window manager connection is gone */
void xwl_idle(void);                    /* xwayland.c: no X11 app left: Xwayland may stop */
#define IDLE_MS 10000                   /* grace period after the last X11 app closes */

/* predefined atoms */
enum { XA_PRIMARY = 1, XA_ATOM = 4, XA_CARDINAL = 6, XA_STRING = 31, XA_WINDOW = 33, XA_WM_HINTS = 35, XA_WM_NAME = 39,
       XA_WM_NORMAL_HINTS = 40, XA_WM_CLASS = 67, XA_WM_TRANSIENT_FOR = 68 };

#define ATOM_LIST(X) \
    X(WM_PROTOCOLS, "WM_PROTOCOLS") X(WM_DELETE_WINDOW, "WM_DELETE_WINDOW") X(WM_TAKE_FOCUS, "WM_TAKE_FOCUS") \
    X(WM_STATE, "WM_STATE") X(WM_CHANGE_STATE, "WM_CHANGE_STATE") X(WM_S0, "WM_S0") X(UTF8_STRING, "UTF8_STRING") \
    X(NET_WM_NAME, "_NET_WM_NAME") X(NET_SUPPORTED, "_NET_SUPPORTED") X(NET_SUPPORTING_WM_CHECK, "_NET_SUPPORTING_WM_CHECK") \
    X(NET_ACTIVE_WINDOW, "_NET_ACTIVE_WINDOW") X(NET_WM_STATE, "_NET_WM_STATE") \
    X(NET_WM_STATE_FULLSCREEN, "_NET_WM_STATE_FULLSCREEN") X(NET_WM_STATE_MAXIMIZED_VERT, "_NET_WM_STATE_MAXIMIZED_VERT") \
    X(NET_WM_STATE_MAXIMIZED_HORZ, "_NET_WM_STATE_MAXIMIZED_HORZ") X(NET_WM_STATE_HIDDEN, "_NET_WM_STATE_HIDDEN") \
    X(NET_WM_STATE_FOCUSED, "_NET_WM_STATE_FOCUSED") X(NET_WM_WINDOW_TYPE, "_NET_WM_WINDOW_TYPE") \
    X(NET_WM_WINDOW_TYPE_NORMAL, "_NET_WM_WINDOW_TYPE_NORMAL") X(NET_WM_WINDOW_TYPE_DIALOG, "_NET_WM_WINDOW_TYPE_DIALOG") \
    X(NET_WM_WINDOW_TYPE_SPLASH, "_NET_WM_WINDOW_TYPE_SPLASH") X(NET_WM_WINDOW_TYPE_MENU, "_NET_WM_WINDOW_TYPE_MENU") \
    X(NET_WM_WINDOW_TYPE_DROPDOWN_MENU, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU") \
    X(NET_WM_WINDOW_TYPE_POPUP_MENU, "_NET_WM_WINDOW_TYPE_POPUP_MENU") X(NET_WM_WINDOW_TYPE_TOOLTIP, "_NET_WM_WINDOW_TYPE_TOOLTIP") \
    X(NET_WM_WINDOW_TYPE_NOTIFICATION, "_NET_WM_WINDOW_TYPE_NOTIFICATION") X(NET_WM_WINDOW_TYPE_COMBO, "_NET_WM_WINDOW_TYPE_COMBO") \
    X(NET_WM_WINDOW_TYPE_DND, "_NET_WM_WINDOW_TYPE_DND") X(NET_WM_MOVERESIZE, "_NET_WM_MOVERESIZE") X(NET_WM_PID, "_NET_WM_PID") \
    X(NET_CLIENT_LIST, "_NET_CLIENT_LIST") X(NET_FRAME_EXTENTS, "_NET_FRAME_EXTENTS") X(MOTIF_WM_HINTS, "_MOTIF_WM_HINTS") \
    X(WL_SURFACE_ID, "WL_SURFACE_ID") X(CLIPBOARD, "CLIPBOARD") X(TARGETS, "TARGETS") X(TIMESTAMP, "TIMESTAMP") X(INCR, "INCR") \
    X(TEXT, "TEXT") X(MIME_UTF8, "text/plain;charset=utf-8") X(MIME_TEXT, "text/plain") X(MIME_PNG, "image/png") X(MIME_HTML, "text/html") \
    X(LORD_SEL_CLIPBOARD, "_LORD_SEL_CLIPBOARD") X(LORD_SEL_PRIMARY, "_LORD_SEL_PRIMARY")

#define AE(id, name) A_##id,
enum { ATOM_LIST(AE) A_COUNT };
#define AN(id, name) name,
static const char *const atom_names[A_COUNT] = { ATOM_LIST(AN) };
static uint32_t atom[A_COUNT];
#define AT(id) atom[A_##id]

struct XWin {
    uint32_t id;
    XWin *next;
    int x, y, w, h;                     /* X geometry (root coordinates) */
    int override, managed, viewable;
    uint32_t surface_res;               /* WL_SURFACE_ID */
    int sid;                            /* wl.c surface (0: not attached) */
    char title[256], cls[128], inst[128];
    int net_name;                       /* _NET_WM_NAME seen: WM_NAME no longer counts */
    int decorated, input, take_focus, del_window;
    uint32_t transient, wtype;
    XSizeHints hints;
    int want_fs;                        /* asked for fullscreen before it had a surface */
    int fullscreen, maximized, minimized, focused;
    int px, py, pw, ph, placed;         /* last position/size we configured */
    unsigned state_sig;
};

typedef void (*ReplyFn)(const uint8_t *r, size_t n, uint32_t a, uint32_t b);
typedef struct { uint32_t seq; ReplyFn fn; uint32_t a, b; } Pend;
#define NPEND 4096

/* a selection being sent to an X app in chunks (INCR) */
typedef struct Xfer {
    uint32_t win, prop, type;
    uint8_t *buf;
    size_t len, off;
    time_t t0;
    struct Xfer *next;
} Xfer;

enum { SEL_CLIP, SEL_PRIM, NSEL };
typedef struct {
    int owned;                          /* the window manager owns it in X (LordOS's selection) */
    unsigned owned_gen;
    unsigned x_gen;                     /* LordOS generation that belongs to an X app's copy */
    int stage;                          /* import: 0 idle, 1 TARGETS, 2 data, 3 INCR chunks */
    uint32_t owner, target;
    int image, latin1;
    uint8_t *buf;
    size_t len, cap;
} Sel;

static struct {
    int fd;
    struct wl_event_loop *loop;
    struct wl_event_source *ev;
    XOut out;
    uint8_t *in;
    size_t in_n, in_cap;
    int setup_done, init_left, ready, debug;
    XSetup su;
    uint32_t seq, id_next, wmwin, cursor;
    uint8_t xfixes_op, xfixes_ev, render_op, composite_op, xres_op;
    struct wl_event_source *idle;       /* fires IDLE_MS after the last X window is gone */
    Pend pend[NPEND];
    unsigned ph, pn;
    XWin *wins;
    XWin *focus;
    Sel sel[NSEL];
    Xfer *xfers;
} X = { .fd = -1 };

#define CLIP_MAX (16u << 20)

static void lost(const char *why);
static void idle_arm(void);

/* ---- requests ---- */

static void req(uint8_t op, uint8_t data) { X.seq++; xo_begin(&X.out, op, data); }
static void expect(ReplyFn fn, uint32_t a, uint32_t b) {
    if (X.pn == NPEND) { X.out.oom = 1; return; }
    X.pend[(X.ph + X.pn) % NPEND] = (Pend){ X.seq, fn, a, b };
    X.pn++;
}
static uint32_t new_id(void) {
    uint32_t inc = X.su.id_mask & (~X.su.id_mask + 1);
    X.id_next += inc;
    return X.su.id_base | (X.id_next & X.su.id_mask);
}

static void change_attrs(uint32_t win, uint32_t mask, uint32_t value) {
    req(X_ChangeWindowAttributes, 0);
    xo_u32(&X.out, win); xo_u32(&X.out, mask); xo_u32(&X.out, value);
    xo_end(&X.out);
}

static void change_prop(uint32_t win, uint32_t prop, uint32_t type, int format, const void *data, uint32_t units) {
    req(X_ChangeProperty, 0);                                /* Replace */
    xo_u32(&X.out, win); xo_u32(&X.out, prop); xo_u32(&X.out, type);
    xo_u8(&X.out, (uint8_t)format); xo_pad(&X.out, 3);
    xo_u32(&X.out, units);
    xo_bytes(&X.out, data, (size_t)units * (size_t)(format / 8));
    xo_end(&X.out);
}

static void get_prop(uint32_t win, uint32_t prop, int del, uint32_t longs, ReplyFn fn, uint32_t a, uint32_t b) {
    req(X_GetProperty, (uint8_t)del);
    xo_u32(&X.out, win); xo_u32(&X.out, prop); xo_u32(&X.out, 0); xo_u32(&X.out, 0); xo_u32(&X.out, longs);
    xo_end(&X.out);
    expect(fn, a, b);
}

static void configure(uint32_t win, uint16_t mask, const int32_t *vals, int nvals) {
    req(X_ConfigureWindow, 0);
    xo_u32(&X.out, win); xo_u16(&X.out, mask); xo_u16(&X.out, 0);
    for (int i = 0; i < nvals; i++) xo_u32(&X.out, (uint32_t)vals[i]);
    xo_end(&X.out);
}

static void set_focus(uint32_t win) {
    req(X_SetInputFocus, win ? 1 : 0);                       /* revert to PointerRoot / None */
    xo_u32(&X.out, win); xo_u32(&X.out, 0);
    xo_end(&X.out);
}

static void set_owner(uint32_t owner, uint32_t selection) {
    req(X_SetSelectionOwner, 0);
    xo_u32(&X.out, owner); xo_u32(&X.out, selection); xo_u32(&X.out, 0);
    xo_end(&X.out);
}

static void convert(uint32_t selection, uint32_t target, uint32_t prop, uint32_t time) {
    req(X_ConvertSelection, 0);
    xo_u32(&X.out, X.wmwin); xo_u32(&X.out, selection); xo_u32(&X.out, target); xo_u32(&X.out, prop); xo_u32(&X.out, time);
    xo_end(&X.out);
}

/* SendEvent with a 32-byte event to the window's owner (no propagation, empty mask). */
static void send_event(uint32_t dest, const uint8_t ev[32]) {
    req(X_SendEvent, 0);
    xo_u32(&X.out, dest); xo_u32(&X.out, 0);
    xo_bytes(&X.out, ev, 32);
    xo_end(&X.out);
}

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static void client_message(uint32_t win, uint32_t type, const uint32_t d[5]) {
    uint8_t ev[32] = { X_ClientMessage, 32 };
    put32(ev + 4, win);
    put32(ev + 8, type);
    for (int i = 0; i < 5; i++) put32(ev + 12 + 4 * i, d[i]);
    send_event(win, ev);
}

static void flush(void) {
    if (X.fd < 0) return;
    if (X.out.oom) { lost("out of memory"); return; }
    while (X.out.n) {
        ssize_t w = send(X.fd, X.out.b, X.out.n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w > 0) { xo_drop(&X.out, (size_t)w); continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        lost("write failed");
        return;
    }
    wl_event_source_fd_update(X.ev, WL_EVENT_READABLE | (X.out.n ? WL_EVENT_WRITABLE : 0));
}

/* ---- windows ---- */

static XWin *find(uint32_t id) {
    for (XWin *w = X.wins; w; w = w->next) if (w->id == id) return w;
    return NULL;
}

static XWin *add_win(uint32_t id) {
    XWin *w = find(id);
    if (w) return w;
    w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->id = id;
    w->decorated = -1;
    w->input = 1;
    w->next = X.wins;
    X.wins = w;
    return w;
}

static int same_client(uint32_t a, uint32_t b) { return x11_same_client(a, b, X.su.id_mask); }

static int is_popup_type(uint32_t t) {
    return t && (t == AT(NET_WM_WINDOW_TYPE_MENU) || t == AT(NET_WM_WINDOW_TYPE_DROPDOWN_MENU) ||
                 t == AT(NET_WM_WINDOW_TYPE_POPUP_MENU) || t == AT(NET_WM_WINDOW_TYPE_TOOLTIP) ||
                 t == AT(NET_WM_WINDOW_TYPE_NOTIFICATION) || t == AT(NET_WM_WINDOW_TYPE_COMBO) ||
                 t == AT(NET_WM_WINDOW_TYPE_DND));
}

static void update_client_list(void) {
    uint32_t ids[256];
    uint32_t n = 0;
    for (XWin *w = X.wins; w && n < 256; w = w->next) if (w->managed) ids[n++] = w->id;
    change_prop(X.su.root, AT(NET_CLIENT_LIST), XA_WINDOW, 32, ids, n);
}

static void set_wm_state(XWin *w, uint32_t state) {
    uint32_t v[2] = { state, 0 };
    change_prop(w->id, AT(WM_STATE), AT(WM_STATE), 32, v, 2);
}

static void write_net_state(XWin *w) {
    uint32_t v[6];
    uint32_t n = 0;
    if (w->fullscreen) v[n++] = AT(NET_WM_STATE_FULLSCREEN);
    if (w->maximized) { v[n++] = AT(NET_WM_STATE_MAXIMIZED_VERT); v[n++] = AT(NET_WM_STATE_MAXIMIZED_HORZ); }
    if (w->minimized) v[n++] = AT(NET_WM_STATE_HIDDEN);
    if (w->focused) v[n++] = AT(NET_WM_STATE_FOCUSED);
    unsigned sig = (unsigned)(w->fullscreen | w->maximized << 1 | w->minimized << 2 | w->focused << 3) + 1;
    if (sig == w->state_sig) return;
    w->state_sig = sig;
    change_prop(w->id, AT(NET_WM_STATE), XA_ATOM, 32, v, n);
}

/* Origin for the clipboard history: the class of the X app that copied. */
static const char *origin_of(uint32_t owner) {
    XWin *w = find(owner);
    if (!w || !w->cls[0])
        for (XWin *o = X.wins; o; o = o->next) if (o->managed && o->cls[0] && same_client(o->id, owner)) { w = o; break; }
    return w && w->cls[0] ? w->cls : "x11";
}

static void try_attach(XWin *w) {
    if (!w->surface_res || w->sid) return;
    XWinInfo info = {
        .title = w->title, .app_id = w->cls, .app_alt = w->inst,
        .csd = w->decorated == 0 || w->wtype == AT(NET_WM_WINDOW_TYPE_SPLASH),
        .popup = w->override || is_popup_type(w->wtype),
        .x = w->x, .y = w->y, .width = w->w, .height = w->h,
    };
    int sid = wlx_attach(w->surface_res, w, &info);
    if (!sid) return;
    w->sid = sid;
    if (X.debug) fprintf(stderr, "xwm: 0x%x is surface %d (%s, class %s, %dx%d%s)\n", w->id, sid,
                         info.popup ? "popup" : "window", w->cls, w->w, w->h, info.csd ? ", own frame" : "");
    if (w->want_fs && !info.popup) { w->want_fs = 0; wlx_request(sid, XREQ_FULLSCREEN, 0); }
}

static void detach(XWin *w) {
    if (w->sid) { int sid = w->sid; w->sid = 0; wlx_detach(sid); }
    w->surface_res = 0;
}

static void free_win(XWin *w) {
    detach(w);
    if (X.focus == w) X.focus = NULL;
    for (XWin **p = &X.wins; *p; p = &(*p)->next) if (*p == w) { *p = w->next; break; }
    free(w);
}

/* ---- properties ---- */

static void prop_reply(const uint8_t *r, size_t n, uint32_t id, uint32_t prop) {
    XWin *w = find(id);
    if (!w || !r || n < 32) return;
    int format = r[1];
    uint32_t type = x11_get32(r + 8), units = x11_get32(r + 16);
    size_t bytes = (size_t)units * (size_t)(format / 8);
    if (bytes > n - 32) bytes = n - 32;
    const uint8_t *v = r + 32;
    uint32_t longs[64];
    size_t nl = format == 32 ? bytes / 4 : 0;
    if (nl > 64) nl = 64;
    for (size_t i = 0; i < nl; i++) longs[i] = x11_get32(v + 4 * i);
    if (prop == XA_WM_CLASS) {
        x11_wm_class(v, bytes, w->inst, sizeof w->inst, w->cls, sizeof w->cls);
    } else if (prop == XA_WM_NAME || prop == AT(NET_WM_NAME)) {
        if (prop == XA_WM_NAME && w->net_name) return;
        if (prop == AT(NET_WM_NAME)) w->net_name = type != 0;
        if (!type && prop == AT(NET_WM_NAME)) return;
        x11_title(v, bytes, type == XA_STRING, w->title, sizeof w->title);
        if (w->sid) wlx_set_title(w->sid, w->title);
    } else if (prop == AT(WM_PROTOCOLS)) {
        w->take_focus = w->del_window = 0;
        for (size_t i = 0; i < nl; i++) {
            if (longs[i] == AT(WM_TAKE_FOCUS)) w->take_focus = 1;
            if (longs[i] == AT(WM_DELETE_WINDOW)) w->del_window = 1;
        }
    } else if (prop == XA_WM_HINTS) {
        w->input = x11_wm_hints_input(nl ? longs : NULL, nl);
    } else if (prop == XA_WM_NORMAL_HINTS) {
        x11_size_hints(nl ? longs : NULL, nl, &w->hints);
    } else if (prop == AT(NET_WM_WINDOW_TYPE)) {
        w->wtype = nl ? longs[0] : 0;
    } else if (prop == XA_WM_TRANSIENT_FOR) {
        w->transient = nl ? longs[0] : 0;
    } else if (prop == AT(MOTIF_WM_HINTS)) {
        w->decorated = x11_motif_decorated(nl ? longs : NULL, nl);
    } else if (prop == AT(NET_WM_STATE)) {
        for (size_t i = 0; i < nl; i++) if (longs[i] == AT(NET_WM_STATE_FULLSCREEN) && !w->sid) w->want_fs = 1;
    }
}

static const int watched[] = { XA_WM_CLASS, XA_WM_NAME, -1 /* _NET_WM_NAME */, -2 /* WM_PROTOCOLS */, XA_WM_HINTS,
                               XA_WM_NORMAL_HINTS, -3 /* window type */, XA_WM_TRANSIENT_FOR, -4 /* Motif */, -5 /* state */ };
static uint32_t watched_atom(int i) {
    switch (watched[i]) {
    case -1: return AT(NET_WM_NAME);
    case -2: return AT(WM_PROTOCOLS);
    case -3: return AT(NET_WM_WINDOW_TYPE);
    case -4: return AT(MOTIF_WM_HINTS);
    case -5: return AT(NET_WM_STATE);
    default: return (uint32_t)watched[i];
    }
}
#define NWATCHED (int)(sizeof watched / sizeof watched[0])

static void read_props(XWin *w) {
    for (int i = 0; i < NWATCHED; i++) get_prop(w->id, watched_atom(i), 0, 1024, prop_reply, w->id, watched_atom(i));
}

/* ---- the clipboard bridge ---- */

static uint32_t sel_atom(int k) { return k == SEL_CLIP ? AT(CLIPBOARD) : XA_PRIMARY; }
static uint32_t sel_prop(int k) { return k == SEL_CLIP ? AT(LORD_SEL_CLIPBOARD) : AT(LORD_SEL_PRIMARY); }
static int sel_index(uint32_t a) { return a == AT(CLIPBOARD) ? SEL_CLIP : a == XA_PRIMARY ? SEL_PRIM : -1; }

/* LordOS's current selection, as a generation (any change → another value). */
static unsigned lord_gen(int k) {
    if (k == SEL_PRIM) { size_t n; unsigned g; wlx_primary(&n, &g); return g; }
    return term_clipboard_gen() * 2654435761u + wl_clip_image_gen() * 40503u + 1;
}
static int lord_has(int k) {
    if (k == SEL_PRIM) { size_t n; unsigned g; return wlx_primary(&n, &g) && n > 0; }
    if (wl_clip_is_image()) return 1;
    /* a secret (Passwords, password fields) never goes to X: any X client can read the property the
     * text is written to while it's pasted */
    return term_clipboard()[0] && !term_clipboard_sensitive();
}

/* LordOS's selection is offered to X only while an X window has the keyboard, and only if it is
 * newer than what an X app copied. */
static void clip_sync(void) {
    for (int k = 0; k < NSEL; k++) {
        Sel *s = &X.sel[k];
        unsigned g = lord_gen(k);
        if (X.focus && lord_has(k) && g != s->x_gen) {
            if (!s->owned || s->owned_gen != g) {
                set_owner(X.wmwin, sel_atom(k));
                s->owned = 1;
                s->owned_gen = g;
            }
        } else if (s->owned && (!X.focus || !lord_has(k))) {
            set_owner(0, sel_atom(k));
            s->owned = 0;
        }
    }
}

static void sel_reset(Sel *s) { free(s->buf); s->buf = NULL; s->len = s->cap = 0; s->stage = 0; }

static int sel_append(Sel *s, const uint8_t *p, size_t n) {
    if (s->len + n > CLIP_MAX) return -1;
    if (s->len + n + 1 > s->cap) {
        size_t cap = s->cap ? s->cap : 4096;
        while (cap < s->len + n + 1) cap *= 2;
        uint8_t *nb = realloc(s->buf, cap);
        if (!nb) return -1;
        s->buf = nb;
        s->cap = cap;
    }
    memcpy(s->buf + s->len, p, n);
    s->len += n;
    return 0;
}

static void sel_finish(int k) {
    Sel *s = &X.sel[k];
    if (s->len) {
        const char *origin = origin_of(s->owner);
        if (s->latin1) {                    /* STRING is Latin-1 */
            uint8_t *u = malloc(s->len * 2 + 1);
            size_t o = 0;
            for (size_t i = 0; u && i < s->len; i++) {
                if (s->buf[i] < 0x80) u[o++] = s->buf[i];
                else { u[o++] = (uint8_t)(0xc0 | s->buf[i] >> 6); u[o++] = (uint8_t)(0x80 | (s->buf[i] & 0x3f)); }
            }
            if (u) { free(s->buf); s->buf = u; s->len = o; s->cap = s->len * 2 + 1; }
        }
        if (k == SEL_PRIM) wlx_primary_text((const char *)s->buf, s->len);
        else if (s->image) wlx_clip_png(s->buf, s->len, origin);
        else wlx_clip_text((const char *)s->buf, s->len, origin);
        s->x_gen = lord_gen(k);             /* this one is the X app's: not offered back */
        if (X.debug) fprintf(stderr, "xwm: %s copied %zu bytes (%s) from %s\n", k ? "PRIMARY" : "CLIPBOARD", s->len,
                             s->image ? "image/png" : "text", origin);
    }
    sel_reset(s);
}

static void sel_reply(const uint8_t *r, size_t n, uint32_t k, uint32_t stage) {
    Sel *s = &X.sel[k];
    if (s->stage != (int)stage) return;                     /* a newer copy replaced it */
    if (!r || n < 32) { sel_reset(s); return; }
    int format = r[1];
    uint32_t type = x11_get32(r + 8);
    size_t bytes = (size_t)x11_get32(r + 16) * (size_t)(format / 8);
    if (bytes > n - 32) bytes = n - 32;
    const uint8_t *v = r + 32;
    if (stage == 1) {                                       /* TARGETS: the best text, or a PNG */
        uint32_t best = 0;
        int rank = 0, png = 0;
        for (size_t i = 0; format == 32 && i + 4 <= bytes; i += 4) {
            uint32_t a = x11_get32(v + i);
            int rk = a == AT(UTF8_STRING) ? 5 : a == AT(MIME_UTF8) ? 4 : a == XA_STRING ? 3 : a == AT(MIME_TEXT) ? 2 : a == AT(TEXT) ? 1 : 0;
            if (rk > rank) { rank = rk; best = a; }
            if (a == AT(MIME_PNG)) png = 1;
        }
        if (!best && png && k == SEL_CLIP) { best = AT(MIME_PNG); s->image = 1; }
        if (!best) { sel_reset(s); return; }
        s->target = best;
        s->latin1 = best == XA_STRING;
        s->stage = 2;
        convert(sel_atom((int)k), best, sel_prop((int)k), 0);
        return;
    }
    if (stage == 2 && type == AT(INCR)) { s->stage = 3; return; }  /* chunks follow (PropertyNotify) */
    if (bytes && sel_append(s, v, bytes)) { sel_reset(s); return; }
    if (stage == 2 || !bytes) sel_finish((int)k);
}

static void sel_start(int k, uint32_t owner) {
    Sel *s = &X.sel[k];
    sel_reset(s);
    s->owner = owner;
    s->image = s->latin1 = 0;
    s->stage = 1;
    convert(sel_atom(k), AT(TARGETS), sel_prop(k), 0);
}

static void sel_owner_changed(const uint8_t *p) {
    uint32_t owner = x11_get32(p + 8), selection = x11_get32(p + 12);
    int k = sel_index(selection);
    if (k < 0 || owner == X.wmwin) return;
    Sel *s = &X.sel[k];
    s->owned = 0;
    if (!owner) { s->x_gen = 0; return; }                  /* the X app that had it is gone */
    s->x_gen = lord_gen(k);
    sel_start(k, owner);
}

static void sel_notify_event(const uint8_t *p) {
    uint32_t requestor = x11_get32(p + 8), selection = x11_get32(p + 12), target = x11_get32(p + 16), prop = x11_get32(p + 20);
    int k = sel_index(selection);
    if (k < 0 || requestor != X.wmwin) return;
    Sel *s = &X.sel[k];
    if (!s->stage) return;
    if (!prop) {                                            /* refused */
        if (s->stage == 1) { s->stage = 2; s->target = AT(UTF8_STRING); convert(selection, AT(UTF8_STRING), sel_prop(k), 0); }
        else sel_reset(s);
        return;
    }
    (void)target;
    get_prop(X.wmwin, sel_prop(k), 1, CLIP_MAX / 4, sel_reply, (uint32_t)k, (uint32_t)s->stage);
}

static void notify_requestor(uint32_t time, uint32_t requestor, uint32_t selection, uint32_t target, uint32_t prop) {
    uint8_t ev[32] = { X_SelectionNotify };
    put32(ev + 4, time);
    put32(ev + 8, requestor);
    put32(ev + 12, selection);
    put32(ev + 16, target);
    put32(ev + 20, prop);
    send_event(requestor, ev);
}

static void write_data(uint32_t win, uint32_t prop, uint32_t type, const uint8_t *data, size_t len) {
    if (len + 64 <= X.su.max_req_bytes) { change_prop(win, prop, type, 8, data, (uint32_t)len); return; }
    Xfer *t = calloc(1, sizeof *t);
    if (!t || !(t->buf = malloc(len))) { free(t); return; }
    memcpy(t->buf, data, len);
    t->len = len;
    t->win = win;
    t->prop = prop;
    t->type = type;
    t->t0 = time(NULL);
    t->next = X.xfers;
    X.xfers = t;
    XWin *known = find(win);
    change_attrs(win, XCW_EventMask, XM_PropertyChange | (known ? XM_FocusChange : 0));
    uint32_t total = (uint32_t)len;
    change_prop(win, prop, AT(INCR), 32, &total, 1);
}

static void xfer_free(Xfer *t) {
    for (Xfer **p = &X.xfers; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    explicit_bzero(t->buf, t->len);                         /* may be a password copied in Passwords */
    free(t->buf);
    free(t);
}

static int xfer_step(uint32_t win, uint32_t prop) {
    for (Xfer *t = X.xfers; t; t = t->next) {
        if (t->win != win || t->prop != prop) continue;
        size_t chunk = X.su.max_req_bytes > 1024 ? X.su.max_req_bytes - 64 : 960;
        if (chunk > 65536) chunk = 65536;
        size_t n = t->len - t->off < chunk ? t->len - t->off : chunk;
        change_prop(win, prop, t->type, 8, t->buf + t->off, (uint32_t)n);
        t->off += n;
        if (!n) xfer_free(t);                               /* the empty chunk ends it */
        return 1;
    }
    return 0;
}

static void sel_request(const uint8_t *p) {
    uint32_t time_ = x11_get32(p + 4), owner = x11_get32(p + 8), requestor = x11_get32(p + 12), selection = x11_get32(p + 16),
             target = x11_get32(p + 20), prop = x11_get32(p + 24);
    int k = sel_index(selection);
    if (!prop) prop = target;                               /* obsolete clients */
    /* LordOS's selection is in X only while an X window has the keyboard (clip_sync). Asking from
     * a window of the focused client is a hint, not a boundary: the requestor id isn't
     * authenticated in X11 and X apps on one display can read each other's selections anyway
     * (docs/PRIVACY.md, "X11 apps") */
    if (k < 0 || owner != X.wmwin || !X.sel[k].owned || !X.focus || !same_client(requestor, X.focus->id) || !lord_has(k)) {
        notify_requestor(time_, requestor, selection, target, 0);
        return;
    }
    int image = k == SEL_CLIP && wl_clip_is_image();
    const char *text = "";
    size_t tlen = 0;
    unsigned g;
    if (k == SEL_PRIM) text = wlx_primary(&tlen, &g);
    else if (!image) { text = term_clipboard(); tlen = strlen(text); }
    size_t hlen = 0;
    const char *html = k == SEL_CLIP && !image ? wl_clip_html(&hlen) : NULL;
    if (target == AT(TARGETS)) {
        uint32_t list[12];
        uint32_t nl = 0;
        list[nl++] = AT(TARGETS);
        list[nl++] = AT(TIMESTAMP);
        if (image) list[nl++] = AT(MIME_PNG);
        else { list[nl++] = AT(UTF8_STRING); list[nl++] = AT(MIME_UTF8); list[nl++] = AT(MIME_TEXT); list[nl++] = XA_STRING; list[nl++] = AT(TEXT); }
        if (html) list[nl++] = AT(MIME_HTML);
        change_prop(requestor, prop, XA_ATOM, 32, list, nl);
    } else if (target == AT(TIMESTAMP)) {
        uint32_t t = 0;
        change_prop(requestor, prop, XA_CARDINAL, 32, &t, 1);
    } else if (html && target == AT(MIME_HTML)) {
        write_data(requestor, prop, target, (const uint8_t *)html, hlen);
    } else if (image && target == AT(MIME_PNG)) {
        size_t n = 0;
        const unsigned char *png = wl_clip_png(&n);
        if (!png) { notify_requestor(time_, requestor, selection, target, 0); return; }
        write_data(requestor, prop, target, png, n);
    } else if (!image && target == XA_STRING) {             /* Latin-1 (what can't be said becomes '?') */
        uint8_t *l = malloc(tlen + 1);
        size_t o = 0;
        for (size_t i = 0; l && i < tlen;) {
            unsigned char c = (unsigned char)text[i];
            uint32_t cp;
            int k2 = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
            if (k2 == 2 && i + 1 < tlen) cp = (uint32_t)(c & 0x1f) << 6 | ((unsigned char)text[i + 1] & 0x3f);
            else cp = c < 0x80 ? c : 0x3f;
            l[o++] = cp <= 0xff ? (uint8_t)cp : '?';
            i += (size_t)k2;
        }
        if (l) write_data(requestor, prop, XA_STRING, l, o);
        free(l);
    } else if (!image && (target == AT(UTF8_STRING) || target == AT(MIME_UTF8) || target == AT(MIME_TEXT) || target == AT(TEXT))) {
        write_data(requestor, prop, target == AT(TEXT) ? AT(UTF8_STRING) : target, (const uint8_t *)text, tlen);
    } else {
        notify_requestor(time_, requestor, selection, target, 0);
        return;
    }
    notify_requestor(time_, requestor, selection, target, prop);
}

/* ---- events ---- */

static void on_create(const uint8_t *p) {
    uint32_t parent = x11_get32(p + 4), id = x11_get32(p + 8);
    if (parent != X.su.root || id == X.wmwin) return;
    XWin *w = add_win(id);
    if (!w) return;
    idle_arm();
    w->x = (int16_t)x11_get16(p + 12);
    w->y = (int16_t)x11_get16(p + 14);
    w->w = x11_get16(p + 16);
    w->h = x11_get16(p + 18);
    w->override = p[22] != 0;
    change_attrs(id, XCW_EventMask, XM_PropertyChange | XM_FocusChange);
    read_props(w);
}

static void on_map_request(const uint8_t *p) {
    uint32_t id = x11_get32(p + 8);
    XWin *w = add_win(id);
    if (!w) return;
    if (!w->managed) {
        w->managed = 1;
        w->placed = 0;
        /* Xwayland's surface includes the border, and the surface's size is what comes back as the
         * window's: with a border, each configure would grow the window by twice its width */
        int32_t no_border = 0;
        configure(id, XCF_Border, &no_border, 1);
        uint32_t ext[4] = { 0, 0, 0, 0 };
        change_prop(id, AT(NET_FRAME_EXTENTS), XA_CARDINAL, 32, ext, 4);
        set_wm_state(w, 1);                                 /* NormalState */
        update_client_list();
    }
    req(X_MapWindow, 0);
    xo_u32(&X.out, id);
    xo_end(&X.out);
}

static void on_unmap(const uint8_t *p) {
    XWin *w = find(x11_get32(p + 8));
    if (!w) return;
    w->viewable = 0;
    detach(w);
    if (w->managed) { w->managed = 0; set_wm_state(w, 0); update_client_list(); }   /* WithdrawnState */
    if (X.focus == w) X.focus = NULL;
}

static void on_configure_request(const uint8_t *p) {
    uint32_t id = x11_get32(p + 8);
    uint16_t mask = x11_get16(p + 26);
    int x = (int16_t)x11_get16(p + 16), y = (int16_t)x11_get16(p + 18), w_ = x11_get16(p + 20), h = x11_get16(p + 22),
        border = x11_get16(p + 24);
    XWin *w = find(id);
    if (!w || !w->managed || w->override || is_popup_type(w->wtype)) {
        /* not one of LordOS's windows (yet): as asked */
        int32_t v[5];
        int n = 0;
        uint16_t m = mask & (XCF_X | XCF_Y | XCF_W | XCF_H | XCF_Border);
        if (m & XCF_X) v[n++] = x;
        if (m & XCF_Y) v[n++] = y;
        if (m & XCF_W) v[n++] = w_;
        if (m & XCF_H) v[n++] = h;
        if (m & XCF_Border) v[n++] = border;
        if (m) configure(id, m, v, n);
        return;
    }
    /* a LordOS window: it may change its size (not while maximized or fullscreen), never its place */
    int nw = (mask & XCF_W) && !w->maximized && !w->fullscreen ? w_ : w->w;
    int nh = (mask & XCF_H) && !w->maximized && !w->fullscreen ? h : w->h;
    if (nw != w->w || nh != w->h) {
        int32_t v[2] = { nw, nh };
        configure(id, XCF_W | XCF_H, v, 2);
        w->pw = nw;
        w->ph = nh;
    } else {                                                /* ICCCM: a synthetic ConfigureNotify */
        uint8_t ev[32] = { X_ConfigureNotify };
        put32(ev + 4, id);
        put32(ev + 8, id);
        ev[16] = (uint8_t)w->x; ev[17] = (uint8_t)(w->x >> 8);
        ev[18] = (uint8_t)w->y; ev[19] = (uint8_t)(w->y >> 8);
        ev[20] = (uint8_t)w->w; ev[21] = (uint8_t)(w->w >> 8);
        ev[22] = (uint8_t)w->h; ev[23] = (uint8_t)(w->h >> 8);
        req(X_SendEvent, 0);
        xo_u32(&X.out, id); xo_u32(&X.out, XM_StructureNotify);
        xo_bytes(&X.out, ev, 32);
        xo_end(&X.out);
    }
}

static void on_configure_notify(const uint8_t *p) {
    uint32_t ev = x11_get32(p + 4), id = x11_get32(p + 8);
    XWin *w = find(id);
    if (!w || ev != X.su.root) return;
    w->x = (int16_t)x11_get16(p + 16);
    w->y = (int16_t)x11_get16(p + 18);
    w->w = x11_get16(p + 20);
    w->h = x11_get16(p + 22);
    w->override = p[26] != 0;
    if (w->sid && (w->override || is_popup_type(w->wtype))) wlx_popup_moved(w->sid, w->x, w->y);
}

static void on_property(const uint8_t *p) {
    uint32_t id = x11_get32(p + 4), a = x11_get32(p + 8);
    int deleted = p[16] == 1;
    if (id == X.wmwin) {                                    /* the next INCR chunk of a copy */
        for (int k = 0; k < NSEL; k++)
            if (a == sel_prop(k) && !deleted && X.sel[k].stage == 3)
                get_prop(X.wmwin, a, 1, CLIP_MAX / 4, sel_reply, (uint32_t)k, 3);
        return;
    }
    if (deleted && xfer_step(id, a)) return;                /* a paste in chunks asks for more */
    XWin *w = find(id);
    if (!w) return;
    for (int i = 0; i < NWATCHED; i++)
        if (watched_atom(i) == a) { get_prop(id, a, 0, 1024, prop_reply, id, a); break; }
}

static void on_client_message(const uint8_t *p) {
    uint32_t id = x11_get32(p + 4), type = x11_get32(p + 8);
    uint32_t d[5];
    for (int i = 0; i < 5; i++) d[i] = x11_get32(p + 12 + 4 * i);
    XWin *w = find(id);
    if (!w) return;
    if (type == AT(WL_SURFACE_ID)) {
        w->surface_res = d[0];
        try_attach(w);
    } else if (type == AT(NET_WM_STATE)) {
        int action = (int)d[0];                             /* 0 remove, 1 add, 2 toggle */
        for (int i = 1; i <= 2; i++) {
            if (d[i] == AT(NET_WM_STATE_FULLSCREEN)) {
                int on = action == 1 || (action == 2 && !w->fullscreen);
                if (w->sid) wlx_request(w->sid, on ? XREQ_FULLSCREEN : XREQ_UNFULLSCREEN, 0);
                else w->want_fs = on;
            } else if (d[i] == AT(NET_WM_STATE_MAXIMIZED_VERT) || (d[i] == AT(NET_WM_STATE_MAXIMIZED_HORZ) && d[1] != AT(NET_WM_STATE_MAXIMIZED_VERT))) {
                int on = action == 1 || (action == 2 && !w->maximized);
                if (w->sid) wlx_request(w->sid, on ? XREQ_MAXIMIZE : XREQ_UNMAXIMIZE, 0);
            } else if (d[i] == AT(NET_WM_STATE_HIDDEN) && action == 1 && w->sid) {
                wlx_request(w->sid, XREQ_MINIMIZE, 0);
            }
        }
    } else if (type == AT(NET_WM_MOVERESIZE)) {
        int e = x11_moveresize_edges(d[2]);
        if (w->sid && e) wlx_request(w->sid, e < 0 ? XREQ_MOVE : XREQ_RESIZE, e < 0 ? 0 : e);
    } else if (type == AT(WM_CHANGE_STATE)) {
        if (d[0] == 3 && w->sid) wlx_request(w->sid, XREQ_MINIMIZE, 0);   /* IconicState */
    }
    /* _NET_ACTIVE_WINDOW from an app is ignored: only the user moves the focus (no focus stealing) */
}

static void on_focus_in(const uint8_t *p) {
    uint32_t id = x11_get32(p + 4);
    int mode = p[8];
    if (mode == 1 || mode == 2) return;                     /* grabs */
    XWin *w = find(id);
    if (!w || !w->managed || w == X.focus) return;
    /* an X app moved the keyboard to a window of another X app: back to the one the user chose */
    if (!X.focus || !same_client(w->id, X.focus->id)) set_focus(X.focus && X.focus->input ? X.focus->id : 0);
}

static void handle(const uint8_t *p, size_t n) {
    int type = p[0] & 0x7f;
    if (type == X_Error || type == X_Reply) {
        uint32_t seq = x11_seq_widen(X.seq, x11_get16(p + 2));
        if (type == X_Error && X.debug) fprintf(stderr, "xwm: X error %u (request %u.%u, value 0x%x)\n", p[1], p[10], x11_get16(p + 8), x11_get32(p + 4));
        while (X.pn && (int32_t)(X.pend[X.ph].seq - seq) < 0) {     /* no reply came: its request failed */
            Pend e = X.pend[X.ph];
            X.ph = (X.ph + 1) % NPEND; X.pn--;
            e.fn(NULL, 0, e.a, e.b);
        }
        if (X.pn && X.pend[X.ph].seq == seq) {
            Pend e = X.pend[X.ph];
            X.ph = (X.ph + 1) % NPEND; X.pn--;
            e.fn(type == X_Reply ? p : NULL, type == X_Reply ? n : 0, e.a, e.b);
        }
        return;
    }
    if (X.xfixes_ev && type == X.xfixes_ev) { sel_owner_changed(p); return; }
    switch (type) {
    case X_CreateNotify: on_create(p); break;
    case X_DestroyNotify: { XWin *w = find(x11_get32(p + 8)); if (w) { int m = w->managed; free_win(w); if (m) update_client_list(); idle_arm(); } break; }
    case X_UnmapNotify: on_unmap(p); break;
    case X_MapNotify: { XWin *w = find(x11_get32(p + 8)); if (w) { w->viewable = 1; w->override = p[12] != 0; } break; }
    case X_MapRequest: on_map_request(p); break;
    case X_ConfigureRequest: on_configure_request(p); break;
    case X_ConfigureNotify: on_configure_notify(p); break;
    case X_PropertyNotify: on_property(p); break;
    case X_ClientMessage: on_client_message(p); break;
    case X_FocusIn: on_focus_in(p); break;
    case X_SelectionNotify: sel_notify_event(p); break;
    case X_SelectionRequest: sel_request(p); break;
    case X_SelectionClear: { int k = sel_index(x11_get32(p + 12)); if (k >= 0) X.sel[k].owned = 0; break; }
    default: break;
    }
}

/* ---- stopping when unused ----
 * The window manager is an X client too, so Xwayland's own -terminate never fires. When no X
 * window is left for IDLE_MS, the X-Resource extension tells whether any other client is still
 * connected (one without windows, like a clipboard tool); if none, Xwayland is stopped. */
static void idle_arm(void) { if (X.idle) wl_event_source_timer_update(X.idle, X.wins ? 0 : IDLE_MS); }

static void clients_reply(const uint8_t *r, size_t n, uint32_t a, uint32_t b) {
    (void)a; (void)b;
    if (!r || n < 32 || X.wins) return;
    uint32_t count = x11_get32(r + 8), others = 0;
    for (uint32_t i = 0; i < count && 32 + (size_t)(i + 1) * 8 <= n; i++) {
        uint32_t base = x11_get32(r + 32 + (size_t)i * 8);
        if (base && base != X.su.id_base) others++;             /* not the server, not us */
    }
    if (others) { idle_arm(); return; }
    if (X.debug) fprintf(stderr, "xwm: no X11 app left\n");
    xwl_idle();
}

static int idle_fired(void *data) {
    (void)data;
    if (X.wins || !X.ready) return 0;
    if (!X.xres_op) { xwl_idle(); return 0; }
    req(X.xres_op, 1);                                          /* XResQueryClients */
    xo_end(&X.out);
    expect(clients_reply, 0, 0);
    flush();
    return 0;
}

/* ---- startup ---- */

static void ignore_reply(const uint8_t *r, size_t n, uint32_t a, uint32_t b) { (void)r; (void)n; (void)a; (void)b; }

/* The root window's cursor: LordOS's arrow, through the Render extension (ARGB). */
static void pictfmt_reply(const uint8_t *r, size_t n, uint32_t a, uint32_t b) {
    (void)a; (void)b;
    if (!r || n < 32) return;
    uint32_t nf = x11_get32(r + 8), fmt = 0;
    for (uint32_t i = 0; i < nf && 32 + (size_t)(i + 1) * 28 <= n; i++) {
        const uint8_t *f = r + 32 + (size_t)i * 28;
        if (f[4] == 1 && f[5] == 32 && x11_get16(f + 8) == 16 && x11_get16(f + 12) == 8 && x11_get16(f + 16) == 0 &&
            x11_get16(f + 20) == 24 && x11_get16(f + 22) == 0xff) { fmt = x11_get32(f); break; }
    }
    const CursorImage *ci = cursor_image(CUR_DEFAULT, 1);
    if (!fmt || !ci || ci->w <= 0 || ci->h <= 0 || ci->w > 128 || ci->h > 128) return;
    uint32_t pix = new_id(), gc = new_id(), pic = new_id(), cur = new_id();
    req(X_CreatePixmap, 32);
    xo_u32(&X.out, pix); xo_u32(&X.out, X.su.root); xo_u16(&X.out, (uint16_t)ci->w); xo_u16(&X.out, (uint16_t)ci->h);
    xo_end(&X.out);
    req(X_CreateGC, 0);
    xo_u32(&X.out, gc); xo_u32(&X.out, pix); xo_u32(&X.out, 0);
    xo_end(&X.out);
    req(X_PutImage, 2);                                     /* ZPixmap */
    xo_u32(&X.out, pix); xo_u32(&X.out, gc); xo_u16(&X.out, (uint16_t)ci->w); xo_u16(&X.out, (uint16_t)ci->h);
    xo_u16(&X.out, 0); xo_u16(&X.out, 0); xo_u8(&X.out, 0); xo_u8(&X.out, 32); xo_pad(&X.out, 2);
    for (int i = 0; i < ci->w * ci->h; i++) xo_u32(&X.out, ci->px[i]);
    xo_end(&X.out);
    req(X.render_op, 4);                                    /* RenderCreatePicture */
    xo_u32(&X.out, pic); xo_u32(&X.out, pix); xo_u32(&X.out, fmt); xo_u32(&X.out, 0);
    xo_end(&X.out);
    req(X.render_op, 27);                                   /* RenderCreateCursor */
    xo_u32(&X.out, cur); xo_u32(&X.out, pic); xo_u16(&X.out, (uint16_t)ci->hx); xo_u16(&X.out, (uint16_t)ci->hy);
    xo_end(&X.out);
    change_attrs(X.su.root, XCW_Cursor, cur);
    req(X.render_op, 7); xo_u32(&X.out, pic); xo_end(&X.out);   /* RenderFreePicture */
    req(X_FreeGC, 0); xo_u32(&X.out, gc); xo_end(&X.out);
    req(X_FreePixmap, 0); xo_u32(&X.out, pix); xo_end(&X.out);
    X.cursor = cur;
    flush();
}

static void init_done(void) {
    uint32_t root = X.su.root;
    /* the window manager: every top-level window's map and configure requests come to us */
    change_attrs(root, XCW_EventMask, XM_SubstructureRedirect | XM_SubstructureNotify | XM_PropertyChange | XM_FocusChange);
    X.wmwin = new_id();
    req(X_CreateWindow, 0);                                 /* depth CopyFromParent */
    xo_u32(&X.out, X.wmwin); xo_u32(&X.out, root);
    xo_u16(&X.out, (uint16_t)-100); xo_u16(&X.out, (uint16_t)-100); xo_u16(&X.out, 1); xo_u16(&X.out, 1);
    xo_u16(&X.out, 0); xo_u16(&X.out, 2);                   /* InputOnly */
    xo_u32(&X.out, 0);                                      /* visual CopyFromParent */
    xo_u32(&X.out, XCW_OverrideRedirect | XCW_EventMask);
    xo_u32(&X.out, 1); xo_u32(&X.out, XM_PropertyChange);
    xo_end(&X.out);
    change_prop(X.wmwin, AT(NET_SUPPORTING_WM_CHECK), XA_WINDOW, 32, &X.wmwin, 1);
    change_prop(root, AT(NET_SUPPORTING_WM_CHECK), XA_WINDOW, 32, &X.wmwin, 1);
    change_prop(X.wmwin, AT(NET_WM_NAME), AT(UTF8_STRING), 8, "LordOS", 6);
    uint32_t sup[] = { AT(NET_WM_STATE), AT(NET_WM_STATE_FULLSCREEN), AT(NET_WM_STATE_MAXIMIZED_VERT), AT(NET_WM_STATE_MAXIMIZED_HORZ),
                       AT(NET_WM_STATE_HIDDEN), AT(NET_WM_STATE_FOCUSED), AT(NET_ACTIVE_WINDOW), AT(NET_WM_MOVERESIZE),
                       AT(NET_CLIENT_LIST), AT(NET_SUPPORTING_WM_CHECK), AT(NET_WM_NAME), AT(NET_FRAME_EXTENTS),
                       AT(NET_WM_WINDOW_TYPE), AT(NET_WM_WINDOW_TYPE_NORMAL), AT(NET_WM_WINDOW_TYPE_DIALOG),
                       AT(NET_WM_WINDOW_TYPE_SPLASH), AT(NET_WM_WINDOW_TYPE_MENU), AT(NET_WM_WINDOW_TYPE_POPUP_MENU),
                       AT(NET_WM_WINDOW_TYPE_DROPDOWN_MENU), AT(NET_WM_WINDOW_TYPE_TOOLTIP), AT(NET_WM_WINDOW_TYPE_COMBO) };
    change_prop(root, AT(NET_SUPPORTED), XA_ATOM, 32, sup, (uint32_t)(sizeof sup / sizeof sup[0]));
    uint32_t none = 0;
    change_prop(root, AT(NET_ACTIVE_WINDOW), XA_WINDOW, 32, &none, 1);
    update_client_list();
    if (X.xfixes_op) {
        req(X.xfixes_op, 0); xo_u32(&X.out, 5); xo_u32(&X.out, 0); xo_end(&X.out);   /* XFixesQueryVersion */
        expect(ignore_reply, 0, 0);
        for (int k = 0; k < NSEL; k++) {
            req(X.xfixes_op, 2);                            /* XFixesSelectSelectionInput: owner, window, client */
            xo_u32(&X.out, X.wmwin); xo_u32(&X.out, sel_atom(k)); xo_u32(&X.out, 7);
            xo_end(&X.out);
        }
    } else fprintf(stderr, "xwm: no XFIXES: the clipboard is not shared with X11 apps\n");
    if (X.render_op) {
        req(X.render_op, 0); xo_u32(&X.out, 0); xo_u32(&X.out, 11); xo_end(&X.out);   /* RenderQueryVersion */
        expect(ignore_reply, 0, 0);
        req(X.render_op, 1); xo_end(&X.out);                /* RenderQueryPictFormats */
        expect(pictfmt_reply, 0, 0);
    }
    /* rootless Xwayland gives a wl_surface only to windows the window manager redirected */
    if (X.composite_op) {
        req(X.composite_op, 0); xo_u32(&X.out, 0); xo_u32(&X.out, 4); xo_end(&X.out);   /* CompositeQueryVersion */
        expect(ignore_reply, 0, 0);
        req(X.composite_op, 2);                             /* CompositeRedirectSubwindows, manual */
        xo_u32(&X.out, root); xo_u8(&X.out, 1); xo_pad(&X.out, 3);
        xo_end(&X.out);
    } else fprintf(stderr, "xwm: no Composite extension: X11 windows can't be shown\n");
    /* owning WM_S0 tells Xwayland the window manager is ready: only then it accepts X apps */
    set_owner(X.wmwin, AT(WM_S0));
    X.ready = 1;
    X.idle = wl_event_loop_add_timer(X.loop, idle_fired, NULL);
    idle_arm();
    fprintf(stderr, "xwm: ready (root 0x%x, %ux%u)\n", root, X.su.width, X.su.height);
}

static void atom_reply(const uint8_t *r, size_t n, uint32_t i, uint32_t b) {
    (void)b;
    if (r && n >= 12 && i < A_COUNT) atom[i] = x11_get32(r + 8);
    if (--X.init_left == 0) init_done();
}

static void ext_reply(const uint8_t *r, size_t n, uint32_t which, uint32_t b) {
    (void)b;
    if (r && n >= 12 && r[8]) {
        if (which == 0) { X.xfixes_op = r[9]; X.xfixes_ev = r[10]; }
        else if (which == 1) X.render_op = r[9];
        else if (which == 2) X.composite_op = r[9];
        else X.xres_op = r[9];
    }
    if (--X.init_left == 0) init_done();
}

static void query_ext(const char *name, uint32_t which) {
    size_t n = strlen(name);
    req(X_QueryExtension, 0);
    xo_u16(&X.out, (uint16_t)n); xo_u16(&X.out, 0);
    xo_bytes(&X.out, name, n);
    xo_end(&X.out);
    expect(ext_reply, which, 0);
    X.init_left++;
}

static void start_init(void) {
    for (int i = 0; i < A_COUNT; i++) {
        size_t n = strlen(atom_names[i]);
        req(X_InternAtom, 0);
        xo_u16(&X.out, (uint16_t)n); xo_u16(&X.out, 0);
        xo_bytes(&X.out, atom_names[i], n);
        xo_end(&X.out);
        expect(atom_reply, (uint32_t)i, 0);
        X.init_left++;
    }
    query_ext("XFIXES", 0);
    query_ext("RENDER", 1);
    query_ext("Composite", 2);
    query_ext("X-Resource", 3);
}

/* ---- the connection ---- */

static int on_fd(int fd, uint32_t mask, void *data) {
    (void)data;
    if (mask & WL_EVENT_WRITABLE) flush();
    if (X.fd < 0) return 0;
    if (mask & WL_EVENT_READABLE) {
        for (;;) {
            if (X.in_cap - X.in_n < 65536) {
                size_t cap = X.in_cap ? X.in_cap * 2 : 131072;
                if (cap > (40u << 20)) { lost("reply too large"); return 0; }
                uint8_t *nb = realloc(X.in, cap);
                if (!nb) { lost("out of memory"); return 0; }
                X.in = nb;
                X.in_cap = cap;
            }
            ssize_t r = recv(fd, X.in + X.in_n, X.in_cap - X.in_n, MSG_DONTWAIT);
            if (r > 0) { X.in_n += (size_t)r; continue; }
            if (r == 0) { lost("Xwayland closed the connection"); return 0; }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            lost("read failed");
            return 0;
        }
        size_t off = 0;
        if (!X.setup_done) {
            size_t used = 0;
            int ok = x11_parse_setup(X.in, X.in_n, &X.su, &used);
            if (ok < 0) { lost("connection refused"); return 0; }
            if (ok > 0) {
                off = used;
                X.setup_done = 1;
                if (X.su.max_req_bytes < 4096) X.su.max_req_bytes = 4096;
                start_init();
            }
        }
        while (X.setup_done && X.fd >= 0) {
            size_t len = x11_packet_len(X.in + off, X.in_n - off);
            if (!len) break;
            handle(X.in + off, len);
            off += len;
        }
        if (X.fd < 0) return 0;
        memmove(X.in, X.in + off, X.in_n - off);
        X.in_n -= off;
    } else if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        lost("Xwayland hung up");
        return 0;
    }
    flush();
    return 0;
}

int xwm_start(int fd, struct wl_event_loop *loop) {
    xwm_stop();
    X.debug = getenv("LORD_XWM_DEBUG") != NULL;     /* every window, copy and X error in the session log */
    X.fd = fd;
    X.loop = loop;
    X.ev = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, on_fd, NULL);
    if (!X.ev) { close(fd); X.fd = -1; return -1; }
    uint8_t setup[12];
    x11_setup_request(setup);
    for (int i = 0; i < 12; i++) xo_u8(&X.out, setup[i]);
    flush();
    return 0;
}

void xwm_stop(void) {
    while (X.wins) free_win(X.wins);
    while (X.xfers) xfer_free(X.xfers);
    for (int k = 0; k < NSEL; k++) sel_reset(&X.sel[k]);
    if (X.ev) wl_event_source_remove(X.ev);
    if (X.idle) wl_event_source_remove(X.idle);
    if (X.fd >= 0) close(X.fd);
    xo_free(&X.out);
    free(X.in);
    int debug = X.debug;
    memset(&X, 0, sizeof X);
    X.fd = -1;
    X.debug = debug;
    memset(atom, 0, sizeof atom);
}

static void lost(const char *why) {
    if (X.fd < 0) return;
    fprintf(stderr, "xwm: %s\n", why);
    xwm_stop();
    xwl_wm_lost();
}

int xwm_running(void) { return X.fd >= 0 && X.ready; }

/* ---- from the compositor ---- */

void xwm_surface_created(uint32_t res_id) {
    for (XWin *w = X.wins; w; w = w->next) if (w->surface_res == res_id && !w->sid) { try_attach(w); break; }
}

void xwm_surface_gone(XWin *w) {
    if (!w) return;
    for (XWin *o = X.wins; o; o = o->next) if (o == w) { w->sid = 0; w->surface_res = 0; return; }
}

int xwm_is_dialog(const XWin *w) { return w->transient || w->wtype == AT(NET_WM_WINDOW_TYPE_DIALOG); }
int xwm_is_fixed(const XWin *w) { return w->hints.min_w > 0 && w->hints.min_w == w->hints.max_w && w->hints.min_h > 0 && w->hints.min_h == w->hints.max_h; }

void xwm_place(XWin *w, int x, int y) {
    if (!X.ready || !w->managed || w->minimized) return;
    if (w->placed && x == w->px && y == w->py) return;
    int32_t v[2] = { x, y };
    configure(w->id, XCF_X | XCF_Y, v, 2);
    w->px = x;
    w->py = y;
    w->placed = 1;
    flush();
}

void xwm_resize(XWin *w, int width, int height) {
    if (!X.ready || !w->managed || width < 1 || height < 1 || width > 32767 || height > 32767) return;
    if (width == w->pw && height == w->ph && width == w->w && height == w->h) return;
    int32_t v[2] = { width, height };
    configure(w->id, XCF_W | XCF_H, v, 2);
    w->pw = width;
    w->ph = height;
    flush();
}

void xwm_set_state(XWin *w, int maximized, int fullscreen, int minimized) {
    if (!X.ready || !w->managed) return;
    int was_min = w->minimized;
    w->maximized = maximized;
    w->fullscreen = fullscreen;
    w->minimized = minimized;
    if (minimized != was_min) {
        set_wm_state(w, minimized ? 3 : 1);                 /* IconicState / NormalState */
        if (minimized) {                                    /* at the bottom: it never catches the pointer */
            int32_t v[1] = { X_Below };
            configure(w->id, XCF_StackMode, v, 1);
        } else w->placed = 0;
    }
    write_net_state(w);
    flush();
}

void xwm_focus(XWin *w) {
    if (!X.ready || w == X.focus) { if (X.ready) flush(); return; }
    XWin *old = X.focus;
    X.focus = w;
    if (old) { old->focused = 0; write_net_state(old); }
    uint32_t id = w ? w->id : 0;
    if (w) {
        if (w->input) set_focus(w->id);                    /* ICCCM: passive / locally active */
        else if (!w->take_focus) set_focus(0);             /* no input */
        if (w->take_focus) {
            uint32_t d[5] = { AT(WM_TAKE_FOCUS), 0, 0, 0, 0 };
            client_message(w->id, AT(WM_PROTOCOLS), d);
        }
        int32_t v[1] = { X_Above };
        configure(w->id, XCF_StackMode, v, 1);
        w->focused = 1;
        write_net_state(w);
    } else set_focus(0);
    change_prop(X.su.root, AT(NET_ACTIVE_WINDOW), XA_WINDOW, 32, &id, 1);
    clip_sync();
    flush();
}

void xwm_close(XWin *w) {
    if (!X.ready) return;
    if (w->del_window) {
        uint32_t d[5] = { AT(WM_DELETE_WINDOW), 0, 0, 0, 0 };
        client_message(w->id, AT(WM_PROTOCOLS), d);
    } else {
        req(X_KillClient, 0);
        xo_u32(&X.out, w->id);
        xo_end(&X.out);
    }
    flush();
}

void xwm_tick(void) {
    if (!X.ready) return;
    clip_sync();
    time_t now = time(NULL);
    for (Xfer *t = X.xfers, *nx; t; t = nx) { nx = t->next; if (now - t->t0 > 30) xfer_free(t); }   /* a paste nobody reads */
    flush();
}
