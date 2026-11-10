/* textinput_client.c — LordOS's input method inside a Linux app (docs/WAYLAND.md, "Input methods"): run
 * inside a LordOS session, it checks zwp_text_input_v3 as GTK and Qt use it.
 *
 *   A window takes the keyboard and its text input gets enter; enable, content type, cursor rectangle
 *   and surrounding text are committed; commits alone never bring a done (an app that commits on every
 *   done would loop); garbage (invalid UTF-8, offsets past the text, a rectangle at INT_MIN) keeps the
 *   connection up; a second text input, destroying it while enabled and the window going first leave
 *   the compositor up.
 *
 *   WAYLAND_DISPLAY=wayland-0 build/textinput-client            (prints ok/FAIL per check, exit 1 on a failure)
 *   WAYLAND_DISPLAY=wayland-0 build/textinput-client type 30    (30 s to type with an input method on:
 *                                                                 every preedit, commit and done is printed
 *                                                                 and checked: serial = commits sent,
 *                                                                 UTF-8 valid, cursor on a boundary)
 *   ... type 30 password                                         (the field is a password: nothing composes) */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"
#include "client/text-input-unstable-v3-client-protocol.h"
#include "../src/wl/tiutf8.h"

typedef struct {
    struct wl_display *dpy;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct xdg_wm_base *wmbase;
    struct wl_seat *seat;
    struct zwp_text_input_manager_v3 *tim;
} Conn;

static int fails, checks;

static void check(const char *what, long got, long want) {
    checks++;
    if (got == want) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    Conn *c = d;
    if (!strcmp(iface, wl_compositor_interface.name)) c->comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) c->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) c->wmbase = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
    else if (!strcmp(iface, wl_seat_interface.name) && !c->seat) c->seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, zwp_text_input_manager_v3_interface.name)) c->tim = wl_registry_bind(r, name, &zwp_text_input_manager_v3_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };

static int connect_to(Conn *c) {
    memset(c, 0, sizeof *c);
    c->dpy = wl_display_connect(NULL);
    if (!c->dpy) return -1;
    struct wl_registry *reg = wl_display_get_registry(c->dpy);
    wl_registry_add_listener(reg, &reg_l, c);
    wl_display_roundtrip(c->dpy);
    if (!c->comp || !c->shm || !c->wmbase) return -1;
    xdg_wm_base_add_listener(c->wmbase, &wm_l, NULL);
    return 0;
}

typedef struct {
    struct wl_surface *surf;
    struct xdg_surface *xs;
    struct xdg_toplevel *top;
} Win;

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) { (void)d; xdg_surface_ack_configure(xs, serial); }
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)w; (void)h; (void)st; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };

static Win *new_window(Conn *c, const char *title, int W, int H, uint32_t color) {
    Win *w = calloc(1, sizeof *w);
    w->surf = wl_compositor_create_surface(c->comp);
    w->xs = xdg_wm_base_get_xdg_surface(c->wmbase, w->surf);
    xdg_surface_add_listener(w->xs, &xs_l, w);
    w->top = xdg_surface_get_toplevel(w->xs);
    xdg_toplevel_add_listener(w->top, &top_l, w);
    xdg_toplevel_set_title(w->top, title);
    xdg_toplevel_set_app_id(w->top, "textinput-client");
    wl_surface_commit(w->surf);
    wl_display_roundtrip(c->dpy);
    int stride = W * 4, size = stride * H;
    int fd = memfd_create("textinput-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { perror("memfd"); exit(2); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int i = 0; i < W * H; i++) px[i] = color;
    struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    munmap(px, (size_t)size);
    wl_surface_attach(w->surf, b, 0, 0);
    wl_surface_damage(w->surf, 0, 0, W, H);
    wl_surface_commit(w->surf);
    wl_display_roundtrip(c->dpy);
    return w;
}

static void close_window(Win *w) {
    xdg_toplevel_destroy(w->top);
    xdg_surface_destroy(w->xs);
    wl_surface_destroy(w->surf);
    free(w);
}

/* What a text input received: pending until done, as an app applies it. */
typedef struct {
    struct zwp_text_input_v3 *ti;
    uint32_t commits;               /* commit requests sent: what done must carry */
    int entered, left, dones, bad_serial, bad_text, preedits, verbose;
    char preedit[4096], committed[4096];
    int pb, pe;
    char pending_pre[4096], pending_commit[4096];
    int has_pre, has_commit;
    uint32_t del_b, del_a;
} Field;

static void ti_enter(void *d, struct zwp_text_input_v3 *t, struct wl_surface *s) { (void)t; (void)s; ((Field *)d)->entered++; }
static void ti_leave(void *d, struct zwp_text_input_v3 *t, struct wl_surface *s) { (void)t; (void)s; ((Field *)d)->left++; }
static void ti_preedit(void *d, struct zwp_text_input_v3 *t, const char *text, int32_t b, int32_t e) {
    (void)t;
    Field *f = d;
    const char *s = text ? text : "";
    size_t n = strlen(s);
    if (tiu_valid_prefix(s, n) != n) f->bad_text++;
    if (!(b == -1 && e == -1) && (b > e || !tiu_boundary(s, n, b) || !tiu_boundary(s, n, e))) f->bad_text++;
    snprintf(f->pending_pre, sizeof f->pending_pre, "%s", s);
    f->pb = b;
    f->pe = e;
    f->has_pre = 1;
    f->preedits++;
}
static void ti_commit_string(void *d, struct zwp_text_input_v3 *t, const char *text) {
    (void)t;
    Field *f = d;
    const char *s = text ? text : "";
    if (tiu_valid_prefix(s, strlen(s)) != strlen(s)) f->bad_text++;
    snprintf(f->pending_commit, sizeof f->pending_commit, "%s", s);
    f->has_commit = 1;
}
static void ti_delete(void *d, struct zwp_text_input_v3 *t, uint32_t b, uint32_t a) { (void)t; ((Field *)d)->del_b = b; ((Field *)d)->del_a = a; }
static void ti_done(void *d, struct zwp_text_input_v3 *t, uint32_t serial) {
    (void)t;
    Field *f = d;
    f->dones++;
    if (serial != f->commits) f->bad_serial++;
    /* applied as the protocol says: a done without preedit_string clears the preedit */
    if (f->has_commit) {
        size_t n = strlen(f->committed), m = strlen(f->pending_commit);
        if (n + m < sizeof f->committed) memcpy(f->committed + n, f->pending_commit, m + 1);
    }
    snprintf(f->preedit, sizeof f->preedit, "%s", f->has_pre ? f->pending_pre : "");
    if (f->verbose)
        printf("done %u: preedit \"%s\" [%d, %d]%s%s%s\n", serial, f->preedit, f->pb, f->pe,
               f->has_commit ? ", commit \"" : "", f->has_commit ? f->pending_commit : "", f->has_commit ? "\"" : "");
    f->has_pre = f->has_commit = 0;
    f->pending_pre[0] = f->pending_commit[0] = 0;
}
static const struct zwp_text_input_v3_listener ti_l = { ti_enter, ti_leave, ti_preedit, ti_commit_string, ti_delete, ti_done };

static void commit(Field *f) { zwp_text_input_v3_commit(f->ti); f->commits++; }

static void enable(Field *f, uint32_t purpose) {
    zwp_text_input_v3_enable(f->ti);
    zwp_text_input_v3_set_content_type(f->ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, purpose);
    zwp_text_input_v3_set_cursor_rectangle(f->ti, 40, 60, 2, 20);
    zwp_text_input_v3_set_surrounding_text(f->ti, "hola ", 5, 5);
    commit(f);
}

static void sleep_ms(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

int main(int argc, char **argv) {
    Conn a;
    if (connect_to(&a)) { fprintf(stderr, "textinput-client: no Wayland display\n"); return 2; }
    check("zwp_text_input_manager_v3 offered", a.tim != NULL, 1);
    check("wl_seat offered", a.seat != NULL, 1);
    if (!a.tim || !a.seat) { printf("%d checks, %d failed\n", checks, fails); return 1; }
    int typing = argc >= 3 && !strcmp(argv[1], "type") ? atoi(argv[2]) : 0;
    int password = argc >= 4 && !strcmp(argv[3], "password");

    Win *w = new_window(&a, "textinput-client: type here", 480, 240, 0xfff4f4f4u);
    Field f = { .verbose = typing > 0 };
    f.ti = zwp_text_input_manager_v3_get_text_input(a.tim, a.seat);
    zwp_text_input_v3_add_listener(f.ti, &ti_l, &f);
    /* a new window takes the keyboard; if not, a click on it does */
    for (int i = 0; i < 40 && !f.entered; i++) { sleep_ms(250); wl_display_roundtrip(a.dpy); if (i == 8) printf("(click the window)\n"); }
    check("the text input gets enter with the keyboard", f.entered > 0, 1);

    enable(&f, password ? ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD : ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    wl_display_roundtrip(a.dpy);
    check("enable is accepted", wl_display_get_error(a.dpy), 0);
    /* commits alone bring nothing back: answering them would loop with an app that commits on each done */
    for (int i = 0; i < 5; i++) { zwp_text_input_v3_set_cursor_rectangle(f.ti, 40 + i, 60, 2, 20); commit(&f); }
    wl_display_roundtrip(a.dpy);
    check("commits without a composition get no done", f.dones, 0);

    /* garbage: invalid UTF-8, offsets past the text and inside a character, rectangles at the limits */
    zwp_text_input_v3_set_surrounding_text(f.ti, "ab\xff\xfe" "cd", 1000000, -7);
    zwp_text_input_v3_set_cursor_rectangle(f.ti, INT_MIN, INT_MAX, -5, INT_MAX);
    commit(&f);
    zwp_text_input_v3_set_surrounding_text(f.ti, "\xed\xa0\x80" "日本", 1, 1);
    zwp_text_input_v3_set_content_type(f.ti, 0xffffffffu, 0xffffffffu);
    commit(&f);
    wl_display_roundtrip(a.dpy);
    check("garbage keeps the connection up", wl_display_get_error(a.dpy), 0);
    enable(&f, password ? ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD : ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    wl_display_roundtrip(a.dpy);

    if (typing) {
        printf("type with an input method on (Korean, Japanese or Chinese) for %d s\n", typing);
        for (int i = 0; i < typing * 4; i++) { sleep_ms(250); wl_display_roundtrip(a.dpy); }
        printf("committed: \"%s\"\n", f.committed);
        if (password) check("a password field gets no composition", f.preedits, 0);
    }
    check("every done carries the commits sent", f.bad_serial, 0);
    check("every preedit and commit is valid UTF-8, its cursor on a boundary", f.bad_text, 0);

    /* disable: nothing more arrives for it */
    zwp_text_input_v3_disable(f.ti);
    commit(&f);
    wl_display_roundtrip(a.dpy);
    int dones = f.dones;
    sleep_ms(300);
    wl_display_roundtrip(a.dpy);
    check("nothing arrives after disable", f.dones - dones, 0);

    /* a second text input on the same seat is allowed and gets enter too */
    Field g = { 0 };
    g.ti = zwp_text_input_manager_v3_get_text_input(a.tim, a.seat);
    zwp_text_input_v3_add_listener(g.ti, &ti_l, &g);
    wl_display_roundtrip(a.dpy);
    check("a second text input is accepted", wl_display_get_error(a.dpy), 0);
    check("the second one gets enter", g.entered > 0, f.entered > 0);

    /* destroyed while enabled, then the window goes first with the other one still enabled */
    enable(&g, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    enable(&f, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    wl_display_roundtrip(a.dpy);
    zwp_text_input_v3_destroy(g.ti);
    close_window(w);
    wl_display_roundtrip(a.dpy);
    check("the compositor is still up after the window goes", wl_display_get_error(a.dpy), 0);
    zwp_text_input_v3_destroy(f.ti);
    wl_display_roundtrip(a.dpy);
    Conn b;
    check("another client still connects", connect_to(&b), 0);
    if (b.dpy) wl_display_disconnect(b.dpy);
    printf("%d checks, %d failed\n", checks, fails);
    wl_display_disconnect(a.dpy);
    return fails ? 1 : 0;
}
