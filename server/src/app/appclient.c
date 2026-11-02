/* appclient.c — «modo app»: `lord-server --app <id>` corre una app .lpk instalada como su propio
 * proceso, aislado (server/src/sec/sandbox.c), y se dibuja como un CLIENTE Wayland del compositor
 * de LordOS. El tema le pone su marco (pide decoración del servidor, como las apps Qt), aparece
 * en el Dock y se ve por el escritorio remoto, igual que cualquier app.
 *
 * Reusa el motor de interfaz propio (ui/: nodos, layout, pintado, texto) y QuickJS (js.c), igual
 * que el compositor, pero sin KMS ni evdev: dibuja en un búfer de memoria compartida (wl_shm) y
 * recibe la entrada del compositor. Las llamadas lord.* que no son de dibujo van por el broker
 * (ui/js_broker.c), y lord-server verifica los permisos de cada una (server/src/sec).
 *
 * Lo lanza lord-server (tercera excepción acotada a «el servidor no ejecuta comandos», ver
 * AGENTS.md): el único argumento es un id validado de un paquete instalado. */
#include "app.h"
#include "../sec/codesig.h"
#include "../ui/ui.h"
#include "../ui/js.h"
#include "../ui/js_sys.h"
#include "../ui/js_broker.h"
#include "../canvas.h"
#include "../keyboard.h"
#include "../lpk.h"
#include "../sec/sandbox.h"
#include "probe.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "client/xdg-shell-client-protocol.h"
#include "client/xdg-decoration-unstable-v1-client-protocol.h"
#include "client/pointer-gestures-unstable-v1-client-protocol.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_keyboard *keyboard;
static struct zxdg_decoration_manager_v1 *decman;

typedef struct { struct wl_buffer *b; uint32_t *px; size_t size; int busy, w, h; } Buf;

/* One toplevel window of the app. The app host (runtime/app-host.js) renders every window as a
 * child of the React root; `node` is that window's root node, laid out and painted on its own
 * surface. Key 0 is the main window, created before the sandbox. */
#define MAX_WINS 16
typedef struct {
    int used, key;
    struct wl_surface *surf;
    struct xdg_surface *xsurf;
    struct xdg_toplevel *top;
    struct zxdg_toplevel_decoration_v1 *deco;
    int cw, ch, configured, need_render, frame_pending;
    int node;                       /* root node of this window (0 until the app host attaches it) */
    char title[256];
    Buf bufs[2];
} AppWin;
static AppWin wins[MAX_WINS];
static int next_key = 1, closed;
static double t_start;              /* app_main entry (CLOCK_MONOTONIC ms), for the first-frame log */
static int first_frame_logged;
static const char *app_id_str = "";
static AppWin *ptr_win, *kbd_win;   /* window under the pointer / with keyboard focus */

static double px_hx, px_hy;     /* last pointer position (surface-local) */
static int ptr_down[8], ptr_btn[8], nptr;

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static AppWin *win_by_key(int key) { for (int i = 0; i < MAX_WINS; i++) if (wins[i].used && wins[i].key == key) return &wins[i]; return NULL; }
static AppWin *win_by_surface(struct wl_surface *s) { for (int i = 0; i < MAX_WINS; i++) if (wins[i].used && wins[i].surf == s) return &wins[i]; return NULL; }
/* The window's root node; the main window falls back to the whole tree (an app host that does not
 * attach windows, or before its first commit). */
static Node *win_root(const AppWin *w) {
    if (!w) return NULL;
    Node *n = w->node ? node_get(w->node) : NULL;
    return n ? n : w->key == 0 ? ui.root : NULL;
}

/* tell the app host (JS): "close" (the compositor asked), "focus" */
static void win_event(const AppWin *w, const char *type) {
    char j[96];
    int n = snprintf(j, sizeof j, "{\"key\":%d,\"type\":\"%s\"}", w->key, type);
    js_broker_app_event("appwindow", j, (size_t)n);
}

/* ---- wl_shm double buffer ---- */

static void buf_release(void *d, struct wl_buffer *b) { (void)b; ((Buf *)d)->busy = 0; }
static const struct wl_buffer_listener buf_listener = { buf_release };

static void buf_free(Buf *b) {
    if (b->b) { wl_buffer_destroy(b->b); b->b = NULL; }
    if (b->px) { munmap(b->px, b->size); b->px = NULL; }
}

static int make_buffer(Buf *b, int w, int h) {
    buf_free(b);
    size_t stride = (size_t)w * 4, size = stride * h;
    int fd = memfd_create("lordos-app", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size)) { if (fd >= 0) close(fd); return -1; }
    b->px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (b->px == MAP_FAILED) { b->px = NULL; close(fd); return -1; }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    b->b = wl_shm_pool_create_buffer(pool, 0, w, h, (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b->b, &buf_listener, b);
    b->size = size;
    b->w = w;
    b->h = h;
    b->busy = 0;
    return 0;
}

static const struct wl_callback_listener fl;    /* below */

/* Layout and paint one window into its buffer. */
static void render(AppWin *w) {
    Node *root = win_root(w);
    if (!root) return;                              /* the app host has not attached it yet */
    Buf *b = !w->bufs[0].busy ? &w->bufs[0] : !w->bufs[1].busy ? &w->bufs[1] : NULL;
    if (!b || w->cw <= 0 || w->ch <= 0) return;
    if (b->w != w->cw || b->h != w->ch) { if (make_buffer(b, w->cw, w->ch)) return; }
    Canvas c;
    canvas_init(&c, b->px, w->cw, w->ch, w->cw);
    ui.width = (float)w->cw;
    ui.height = (float)w->ch;
    layout_root(root, (float)w->cw, (float)w->ch);
    /* every window shares node coordinates starting at 0,0: no "paint from the video" shortcut
     * (an empty damage box turns off media_cover_node/opaque_cover_node in paint_root) */
    ui.damage = (Box){ 0, 0, 0, 0 };
    paint_root_clear(&c, root, 0x00000000);
    b->busy = 1;
    wl_surface_attach(w->surf, b->b, 0, 0);
    wl_surface_damage_buffer(w->surf, 0, 0, w->cw, w->ch);
    struct wl_callback *cb = wl_surface_frame(w->surf);
    wl_callback_add_listener(cb, &fl, w);
    w->frame_pending = 1;
    wl_surface_commit(w->surf);
    w->need_render = 0;
    if (!first_frame_logged && w->key == 0) {      /* one line: how long the app took to show */
        first_frame_logged = 1;
        double t = now_ms();
        fprintf(stderr, "app %s: first frame %.1f ms after the process started (at %.1f ms, monotonic)\n", app_id_str, t - t_start, t);
    }
}

static void frame_done(void *d, struct wl_callback *cb, uint32_t t) {
    (void)t;
    wl_callback_destroy(cb);
    AppWin *w = d;
    if (w->used) w->frame_pending = 0;
}
static const struct wl_callback_listener fl = { frame_done };

/* ---- xdg ---- */

static void xs_configure(void *d, struct xdg_surface *x, uint32_t serial) {
    AppWin *w = d;
    xdg_surface_ack_configure(x, serial);
    if (!w->cw || !w->ch) { w->cw = 640; w->ch = 480; }
    w->configured = 1;
    w->need_render = 1;
}
static const struct xdg_surface_listener xs_listener = { xs_configure };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t wd, int32_t ht, struct wl_array *st) {
    (void)t; (void)st;
    AppWin *w = d;
    if (wd > 0 && ht > 0 && (wd != w->cw || ht != w->ch)) { w->cw = wd; w->ch = ht; w->need_render = 1; }
}
/* the compositor asks to close: the app host decides (wm.close, which honours close guards) */
static void top_close(void *d, struct xdg_toplevel *t) { (void)t; win_event((AppWin *)d, "close"); }
static void top_cfg_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_listener = { top_configure, top_close, top_cfg_bounds, top_caps };

static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t serial) { (void)d; xdg_wm_base_pong(w, serial); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

/* New toplevel with server-side decoration (the theme draws the frame). Returns it or NULL. */
static AppWin *win_create(int key, const char *title, int w, int h) {
    AppWin *a = NULL;
    for (int i = 0; i < MAX_WINS && !a; i++) if (!wins[i].used) a = &wins[i];
    if (!a) return NULL;
    memset(a, 0, sizeof *a);
    a->used = 1;
    a->key = key;
    a->cw = w > 0 ? w : 640;
    a->ch = h > 0 ? h : 480;
    a->surf = wl_compositor_create_surface(comp);
    a->xsurf = xdg_wm_base_get_xdg_surface(wm, a->surf);
    xdg_surface_add_listener(a->xsurf, &xs_listener, a);
    a->top = xdg_surface_get_toplevel(a->xsurf);
    xdg_toplevel_add_listener(a->top, &top_listener, a);
    xdg_toplevel_set_app_id(a->top, app_id_str);    /* the Dock groups every window by this id */
    snprintf(a->title, sizeof a->title, "%s", title && title[0] ? title : app_id_str);
    xdg_toplevel_set_title(a->top, a->title);
    if (decman) {
        a->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(decman, a->top);
        zxdg_toplevel_decoration_v1_set_mode(a->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(a->surf);
    return a;
}

static void win_destroy(AppWin *a) {
    if (!a || !a->used) return;
    if (ptr_win == a) ptr_win = NULL;
    if (kbd_win == a) kbd_win = NULL;
    if (a->deco) zxdg_toplevel_decoration_v1_destroy(a->deco);
    xdg_toplevel_destroy(a->top);
    xdg_surface_destroy(a->xsurf);
    wl_surface_destroy(a->surf);
    buf_free(&a->bufs[0]);
    buf_free(&a->bufs[1]);
    a->used = 0;
    int left = 0;
    for (int i = 0; i < MAX_WINS; i++) left += wins[i].used;
    if (!left) closed = 1;                          /* last window closed: the app ends */
}

/* ---- lord.appWindows (app host only): the app's own windows ---- */

static JSValue f_win_open(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *title = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(c, argv[0]) : NULL;
    int32_t w = 640, h = 480;
    if (argc > 1) JS_ToInt32(c, &w, argv[1]);
    if (argc > 2) JS_ToInt32(c, &h, argv[2]);
    if (w < 160 || w > 8192) w = 640;
    if (h < 100 || h > 8192) h = 480;
    AppWin *a = next_key < 1 << 20 ? win_create(next_key, title, w, h) : NULL;
    if (title) JS_FreeCString(c, title);
    if (!a) return JS_NewInt32(c, -1);
    next_key++;
    return JS_NewInt32(c, a->key);
}
static JSValue f_win_attach(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t key = -1, node = 0;
    if (argc < 2 || JS_ToInt32(c, &key, argv[0]) || JS_ToInt32(c, &node, argv[1])) return JS_FALSE;
    AppWin *a = win_by_key(key);
    if (!a || !node_get(node)) return JS_FALSE;
    if (a->node != node) { a->node = node; a->need_render = 1; }
    return JS_TRUE;
}
static JSValue f_win_close(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t key = -1;
    if (argc > 0) JS_ToInt32(c, &key, argv[0]);
    /* the app's code was refused (verified code): React tears its windows down, but the main one
     * stays to show the recovery screen */
    if (js_broker_code_refused() && key == 0) return JS_UNDEFINED;
    win_destroy(win_by_key(key));
    return JS_UNDEFINED;
}
static JSValue f_win_title(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t key = -1;
    if (argc < 2) return JS_UNDEFINED;
    JS_ToInt32(c, &key, argv[0]);
    AppWin *a = win_by_key(key);
    const char *title = JS_ToCString(c, argv[1]);
    if (a && title) {
        const char *want = title[0] ? title : app_id_str;
        if (strcmp(a->title, want)) { snprintf(a->title, sizeof a->title, "%s", want); xdg_toplevel_set_title(a->top, a->title); }
    }
    if (title) JS_FreeCString(c, title);
    return JS_UNDEFINED;
}
static const JSCFunctionListEntry win_funcs[] = {
    JS_CFUNC_DEF("open", 3, f_win_open), JS_CFUNC_DEF("attach", 2, f_win_attach),
    JS_CFUNC_DEF("close", 1, f_win_close), JS_CFUNC_DEF("setTitle", 2, f_win_title),
};

void app_windows_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, win_funcs, sizeof win_funcs / sizeof win_funcs[0]);
    JS_SetPropertyStr(c, lord, "appWindows", o);
}

/* ---- entrada ---- */

/* node under the pointer, in the window the pointer is over */
static Node *hit_at(void) { Node *r = win_root(ptr_win); return r ? hit_test(r, (float)px_hx, (float)px_hy) : NULL; }

static void dispatch_pointer(void) {
    Node *target = hit_at();
    int id = target ? target->id : 0;
    js_pointer("move", id, (float)px_hx, (float)px_hy, 0);
    for (int i = 0; i < nptr; i++) js_pointer(ptr_down[i] ? "down" : "up", id, (float)px_hx, (float)px_hy, ptr_btn[i]);
    nptr = 0;
}

static void p_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)s; ptr_win = win_by_surface(sf); px_hx = wl_fixed_to_double(x); px_hy = wl_fixed_to_double(y);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) {
    (void)d; (void)p; (void)s;
    if (ptr_win && ptr_win->surf == sf) ptr_win = NULL;
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)t; px_hx = wl_fixed_to_double(x); px_hy = wl_fixed_to_double(y); dispatch_pointer();
}
static void p_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st) {
    (void)d; (void)p; (void)s; (void)t;
    int button = b == BTN_LEFT ? 0 : b == BTN_MIDDLE ? 1 : b == BTN_RIGHT ? 2 : -1;
    if (button >= 0 && nptr < 8) { ptr_down[nptr] = st == WL_POINTER_BUTTON_STATE_PRESSED; ptr_btn[nptr] = button; nptr++; }
    dispatch_pointer();
}
/* touchpad (axis_source finger): scroll fino con fases, como en lord-server (docs/TRACKPAD.md) */
static int axis_src = -1, fine_on;
static void p_source(void *d, struct wl_pointer *p, uint32_t src) { (void)d; (void)p; axis_src = (int)src; }
static void p_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {
    (void)d; (void)p; (void)t; (void)a;
    if (!fine_on) return;
    fine_on = 0;
    Node *n = hit_at();
    js_gesture("scroll", "end", n ? n->id : 0, (float)px_hx, (float)px_hy, 0, 0, 0, 0);
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {
    (void)d; (void)p; (void)t;
    if (axis_src == WL_POINTER_AXIS_SOURCE_FINGER) {
        Node *n = hit_at();
        if (!fine_on) { fine_on = 1; js_gesture("scroll", "begin", n ? n->id : 0, (float)px_hx, (float)px_hy, 0, 0, 0, 0); }
        double val = wl_fixed_to_double(v);
        int h = a == WL_POINTER_AXIS_HORIZONTAL_SCROLL;
        js_gesture("scroll", "update", n ? n->id : 0, (float)px_hx, (float)px_hy, h ? val : 0, h ? 0 : val, 0, 0);
        return;
    }
    if (a == WL_POINTER_AXIS_VERTICAL_SCROLL) { Node *n = hit_at(); js_pointer("wheel", n ? n->id : 0, (float)px_hx, (float)px_hy, wl_fixed_to_double(v) > 0 ? -1 : 1); }
}
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; axis_src = -1; }
static void p_noop2(void *d, struct wl_pointer *p, uint32_t a, int32_t b) { (void)d; (void)p; (void)a; (void)b; }
static const struct wl_pointer_listener p_listener = { p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_source, p_stop, p_noop2, p_noop2, NULL };

/* pellizco y deslizar del touchpad (zwp_pointer_gestures_v1) → onPinch / onSwipe de la app */
static struct zwp_pointer_gestures_v1 *gestures;
static double g_rot;
static int g_node(void) { Node *n = hit_at(); return n ? n->id : 0; }
static void pi_begin(void *d, struct zwp_pointer_gesture_pinch_v1 *g, uint32_t s, uint32_t t, struct wl_surface *sf, uint32_t f) {
    (void)d; (void)g; (void)s; (void)t; (void)sf; (void)f;
    g_rot = 0;
    js_gesture("pinch", "begin", g_node(), (float)px_hx, (float)px_hy, 1, 0, 0, 0);
}
static void pi_update(void *d, struct zwp_pointer_gesture_pinch_v1 *g, uint32_t t, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t sc, wl_fixed_t rot) {
    (void)d; (void)g; (void)t;
    g_rot += wl_fixed_to_double(rot);
    js_gesture("pinch", "update", g_node(), (float)px_hx, (float)px_hy, wl_fixed_to_double(sc), g_rot, wl_fixed_to_double(dx), wl_fixed_to_double(dy));
}
static void pi_end(void *d, struct zwp_pointer_gesture_pinch_v1 *g, uint32_t s, uint32_t t, int32_t cancelled) {
    (void)d; (void)g; (void)s; (void)t;
    js_gesture("pinch", cancelled ? "cancel" : "end", g_node(), (float)px_hx, (float)px_hy, 1, g_rot, 0, 0);
}
static const struct zwp_pointer_gesture_pinch_v1_listener pinch_listener = { pi_begin, pi_update, pi_end };
static int sw_fingers;
static void sw_begin(void *d, struct zwp_pointer_gesture_swipe_v1 *g, uint32_t s, uint32_t t, struct wl_surface *sf, uint32_t f) {
    (void)d; (void)g; (void)s; (void)t; (void)sf;
    sw_fingers = (int)f;
    js_gesture("swipe", "begin", g_node(), (float)px_hx, (float)px_hy, 0, 0, f, 0);
}
static void sw_update(void *d, struct zwp_pointer_gesture_swipe_v1 *g, uint32_t t, wl_fixed_t dx, wl_fixed_t dy) {
    (void)d; (void)g; (void)t;
    js_gesture("swipe", "update", g_node(), (float)px_hx, (float)px_hy, wl_fixed_to_double(dx), wl_fixed_to_double(dy), sw_fingers, 0);
}
static void sw_end(void *d, struct zwp_pointer_gesture_swipe_v1 *g, uint32_t s, uint32_t t, int32_t cancelled) {
    (void)d; (void)g; (void)s; (void)t;
    js_gesture("swipe", cancelled ? "cancel" : "end", g_node(), (float)px_hx, (float)px_hy, 0, 0, sw_fingers, 0);
}
static const struct zwp_pointer_gesture_swipe_v1_listener swipe_listener = { sw_begin, sw_update, sw_end };
static void gestures_setup(void) {
    static int done;
    if (done || !gestures || !pointer) return;
    done = 1;
    zwp_pointer_gesture_pinch_v1_add_listener(zwp_pointer_gestures_v1_get_pinch_gesture(gestures, pointer), &pinch_listener, NULL);
    zwp_pointer_gesture_swipe_v1_add_listener(zwp_pointer_gestures_v1_get_swipe_gesture(gestures, pointer), &swipe_listener, NULL);
}

/* The compositor's keymap (the user's layouts, already expanded): compiling it reads no files,
 * so it also works inside the sandbox (keyboard.c kb_set_keymap_string). */
static void k_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t sz) {
    (void)d; (void)k;
    if (fd < 0) return;
    if (fmt == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && sz > 1 && sz < (16u << 20)) {
        char *m = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m != MAP_FAILED) {
            if (!memchr(m, 0, sz) || m[sz - 1] == 0) {
                char *txt = strndup(m, sz);
                if (txt) { kb_set_keymap_string(txt); free(txt); }
            }
            munmap(m, sz);
        }
    }
    close(fd);
}
/* keyboard focus moved to one of our windows: the app host raises it (wm.focus) */
static void k_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *keys) {
    (void)d; (void)k; (void)s; (void)keys;
    AppWin *w = win_by_surface(sf);
    kb_reset_state();                       /* keys released while another window had the keyboard */
    if (w && w != kbd_win) { kbd_win = w; win_event(w, "focus"); }
}
static void k_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) { (void)d; (void)k; (void)s; (void)sf; kb_reset_state(); }
static void k_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t st) {
    (void)d; (void)k; (void)s; (void)t;
    js_key((int)key, st == WL_KEYBOARD_KEY_STATE_PRESSED ? 1 : 0);
}
static void k_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp) {
    (void)d; (void)k; (void)s; (void)dep; (void)lat; (void)lock;   /* modifiers come from the keys themselves */
    kb_set_group((int)grp);                                         /* the active layout follows the compositor */
}
static void k_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) { (void)d; (void)k; (void)rate; (void)delay; }
static const struct wl_keyboard_listener k_listener = { k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat };

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) { pointer = wl_seat_get_pointer(s); wl_pointer_add_listener(pointer, &p_listener, NULL); gestures_setup(); }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) { keyboard = wl_seat_get_keyboard(s); wl_keyboard_add_listener(keyboard, &k_listener, NULL); }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

/* ---- registry ---- */

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, "wl_compositor")) comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base")) { wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 6 ? ver : 6); xdg_wm_base_add_listener(wm, &wm_listener, NULL); }
    else if (!strcmp(iface, "wl_seat")) { seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 7 ? ver : 7); wl_seat_add_listener(seat, &seat_listener, NULL); }
    else if (!strcmp(iface, "zxdg_decoration_manager_v1")) decman = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
    else if (!strcmp(iface, "zwp_pointer_gestures_v1")) { gestures = wl_registry_bind(r, name, &zwp_pointer_gestures_v1_interface, 1); gestures_setup(); }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* "key": [w, h] from the package's manifest.json (size, minSize); 0 if missing or out of range. */
static int manifest_pair(const char *dir, const char *key, int *w, int *h) {
    char p[PATH_MAX + 32], buf[8192], pat[40];
    if ((size_t)snprintf(p, sizeof p, "%s/manifest.json", dir) >= sizeof p) return 0;
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *k = strstr(buf, pat);
    if (!k || !(k = strchr(k + strlen(pat), '['))) return 0;
    int a = 0, b = 0;
    if (sscanf(k + 1, " %d , %d", &a, &b) != 2 || a < 160 || b < 100 || a > 8192 || b > 8192) return 0;
    *w = a;
    *h = b;
    return 1;
}

int app_main(const char *id, const char *theme_bundle, const char *fonts) {
    t_start = now_ms();
    if (!lpk_valid_id(id)) { fprintf(stderr, "lord-server --app: invalid id\n"); return 2; }
    /* the process name is the app's id ("mail"), not "exe" (it was started through /proc/self/exe):
     * ps, top and the Monitor's process list show which app it is */
    prctl(PR_SET_NAME, id, 0, 0, 0);
    char dir[PATH_MAX];
    int sys;
    {
        /* <assets> = the parent of the fonts folder; a system app there wins over a user .lpk */
        char assets[PATH_MAX];
        snprintf(assets, sizeof assets, "%s", fonts);
        char *sl = strrchr(assets, '/');
        if (sl) *sl = 0;
        /* verified system code (sec/codesig.c): the manifest is read and checked before the sandbox */
        codesig_init(assets);
        if ((sys = app_package_dir(id, assets, dir, sizeof dir)) < 0) {
            fprintf(stderr, "lord-server --app: app '%s' is not installed\n", id);
            return 2;
        }
    }
    /* carpeta de datos de la app (la única que puede escribir) */
    const char *home = getenv("HOME");
    char data[PATH_MAX], parent[PATH_MAX - 48];
    snprintf(parent, sizeof parent, "%s/.local/share/lordos/appdata", home ? home : "/nonexistent");
    mkdir(parent, 0700);
    snprintf(data, sizeof data, "%s/%.40s", parent, id);
    mkdir(data, 0700);

    /* System Information's sandbox probe (probe.c): the system package `sysprobe` only. Same sandbox
     * as any isolated app (the read-only folders below are the ones the main path gives), then it tries
     * what must fail, reports over its broker and exits: no window, no app code. */
    if (sys == 1 && !strcmp(id, PROBE_APP_ID)) {
        char hd[PATH_MAX], lg[PATH_MAX];
        snprintf(hd, sizeof hd, "%s", theme_bundle);
        char *sl = strrchr(hd, '/');
        if (sl) *sl = 0; else snprintf(hd, sizeof hd, ".");
        snprintf(lg, sizeof lg, "%s/.local/share/lordos/locales", home ? home : "/nonexistent");
        const char *pro[] = { fonts, hd, "/usr/share/fonts", lg, NULL };
        SandboxConfig psb = { .id = id, .pkg_dir = dir, .data_dir = data, .runtime_dir = getenv("XDG_RUNTIME_DIR"), .ro_extra = pro, .network = 0 };
        int hpid = -1, hfd = probe_helper_start(data, &hpid);   /* single-threaded, nothing loaded yet */
        int mask = sandbox_apply(&psb);
        return probe_main(home ? home : "/nonexistent", data, mask, hfd, hpid);
    }

    if (text_init(fonts)) { fprintf(stderr, "lord-server --app: no fonts\n"); return 1; }
    { char a[PATH_MAX * 2]; snprintf(a, sizeof a, "%.*s/..", (int)(sizeof a - 4), fonts); js_sys_set_assets(a); }
    kb_set("us", "");

    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "lord-server --app: could not connect to the compositor\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wm) { fprintf(stderr, "lord-server --app: the compositor lacks a required interface\n"); return 1; }

    app_id_str = id;
    int mw = 640, mh = 480, nw = 0, nh = 0;
    manifest_pair(dir, "size", &mw, &mh);    /* the size the app asks for in its manifest */
    AppWin *main_win = win_create(0, id, mw, mh);
    if (!main_win) return 1;                 /* the main window (key 0) */
    if (manifest_pair(dir, "minSize", &nw, &nh)) xdg_toplevel_set_min_size(main_win->top, nw, nh);
    wl_display_roundtrip(dpy);               /* first configure */

    /* Caja de arena: después de conectar Wayland y abrir el broker, antes de cargar la app. */
    js_broker_set_pkgdir(dir, sys);          /* the package folder, before the sandbox */
    int net = js_broker_wants_network();     /* del manifest + el permiso */
    /* el «app host» (React + SDK) es código del SO; su carpeta es de solo lectura */
    char hostdir[PATH_MAX];
    snprintf(hostdir, sizeof hostdir, "%s", theme_bundle);
    char *slash = strrchr(hostdir, '/');
    if (slash) *slash = 0; else snprintf(hostdir, sizeof hostdir, ".");
    char langs[PATH_MAX];                    /* paquetes de idioma del usuario (árabe, francés…): solo lectura */
    snprintf(langs, sizeof langs, "%s/.local/share/lordos/locales", home ? home : "/nonexistent");
    const char *ro[] = { fonts, hostdir, "/usr/share/fonts", langs, NULL };   /* + fuentes del sistema (emoji, respaldo por escritura) */
    SandboxConfig sb = { .id = id, .pkg_dir = dir, .data_dir = data, .runtime_dir = getenv("XDG_RUNTIME_DIR"), .ro_extra = ro, .network = net };
    /* the local time zone is read now (libc keeps it): /etc/localtime and the zone files are
     * outside the sandbox, and without this dates in Mail and Calendar would be in UTC */
    tzset();
    sandbox_apply(&sb);

    int jr = js_init_app(theme_bundle, id);
    if (jr == JS_REFUSED) fprintf(stderr, "lord-server --app: the app host was modified and was not loaded (recovery screen)\n");
    else if (jr) { fprintf(stderr, "lord-server --app: could not load the app\n"); return 1; }
    int recovered = jr == JS_REFUSED;

    int wfd = wl_display_get_fd(dpy);
    double start = now_ms();
    (void)start;
    while (!closed && !ui.quit_requested) {
        double delay = -1;
        for (int i = 0; i < 64; i++) { delay = js_tick(now_ms()); if (delay != 0) break; }
        if (js_wants_frame()) { js_frame(now_ms()); }
        /* the system app's own code was refused while the host loaded it: show why (outside JS) */
        if (!recovered && js_broker_code_refused()) {
            recovered = 1;
            js_app_recovery();
            for (int i = 0; i < MAX_WINS; i++) if (wins[i].used) { if (wins[i].key == 0) wins[i].node = 0; wins[i].need_render = 1; }
            continue;
        }
        /* one node tree for all windows: any change repaints every window (cheap: apps are small) */
        if (ui.dirty) { for (int i = 0; i < MAX_WINS; i++) if (wins[i].used) wins[i].need_render = 1; ui.dirty = 0; }
        /* a window waiting for its frame callback or a free buffer renders when those arrive */
        for (int i = 0; i < MAX_WINS; i++) {
            AppWin *w = &wins[i];
            if (w->used && w->configured && w->need_render && !w->frame_pending) render(w);
        }
        wl_display_flush(dpy);
        struct pollfd pfd[2];
        int n = 0;
        pfd[n].fd = wfd; pfd[n].events = POLLIN; n++;
        int bfd = js_broker_fd();
        if (bfd >= 0) { pfd[n].fd = bfd; pfd[n].events = POLLIN; n++; }
        int wait = delay < 0 ? 100 : (int)ceil(delay);
        if (js_broker_pending()) wait = 0;       /* events that arrived during an RPC */
        if (wl_display_prepare_read(dpy) != 0) { wl_display_dispatch_pending(dpy); continue; }
        if (poll(pfd, (nfds_t)n, wait) < 0 && errno != EINTR) { wl_display_cancel_read(dpy); break; }
        if (pfd[0].revents & POLLIN) wl_display_read_events(dpy); else wl_display_cancel_read(dpy);
        wl_display_dispatch_pending(dpy);
        if ((n > 1 && (pfd[1].revents & POLLIN)) || js_broker_pending()) js_broker_poll();
        if (pfd[0].revents & (POLLHUP | POLLERR)) break;
    }
    js_shutdown();
    wl_display_disconnect(dpy);
    return 0;
}
