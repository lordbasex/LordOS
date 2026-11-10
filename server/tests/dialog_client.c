/* dialog_client.c — a dialog over another process's window (docs/WAYLAND.md, "Dialogs"): run inside a
 * LordOS session, it checks xdg-foreign-v2 and xdg-dialog-v1 from two connections, as a portal and the
 * app that asked it would be.
 *
 *   A maps a window and exports it: the handle is 32 hex digits and a second export gets another one;
 *   B imports a made-up handle (told destroyed right away) and A's (not), maps a modal dialog with
 *   set_parent_of over it; once A's window goes away, B's import is told destroyed.
 *
 *   WAYLAND_DISPLAY=wayland-0 build/dialog-client          (prints ok/FAIL per check, exit 1 on a failure)
 *   WAYLAND_DISPLAY=wayland-0 build/dialog-client hold 20  (keeps both open: the grey window can't be
 *                                                           clicked, the blue dialog stays above it and
 *                                                           moves with it) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"
#include "client/xdg-foreign-unstable-v2-client-protocol.h"
#include "client/xdg-dialog-v1-client-protocol.h"

typedef struct {
    struct wl_display *dpy;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct xdg_wm_base *wmbase;
    struct zxdg_exporter_v2 *exporter;
    struct zxdg_importer_v2 *importer;
    struct xdg_wm_dialog_v1 *dialogs;
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
    else if (!strcmp(iface, zxdg_exporter_v2_interface.name)) c->exporter = wl_registry_bind(r, name, &zxdg_exporter_v2_interface, 1);
    else if (!strcmp(iface, zxdg_importer_v2_interface.name)) c->importer = wl_registry_bind(r, name, &zxdg_importer_v2_interface, 1);
    else if (!strcmp(iface, xdg_wm_dialog_v1_interface.name)) c->dialogs = wl_registry_bind(r, name, &xdg_wm_dialog_v1_interface, 1);
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
    int configured;
} Win;

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) { ((Win *)d)->configured = 1; xdg_surface_ack_configure(xs, serial); }
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)w; (void)h; (void)st; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };

static Win *new_window(Conn *c, const char *title) {
    Win *w = calloc(1, sizeof *w);
    w->surf = wl_compositor_create_surface(c->comp);
    w->xs = xdg_wm_base_get_xdg_surface(c->wmbase, w->surf);
    xdg_surface_add_listener(w->xs, &xs_l, w);
    w->top = xdg_surface_get_toplevel(w->xs);
    xdg_toplevel_add_listener(w->top, &top_l, w);
    xdg_toplevel_set_title(w->top, title);
    xdg_toplevel_set_app_id(w->top, "dialog-client");
    wl_surface_commit(w->surf);
    wl_display_roundtrip(c->dpy);
    return w;
}

static void draw(Conn *c, Win *w, int W, int H, uint32_t color) {
    int stride = W * 4, size = stride * H;
    int fd = memfd_create("dialog-client", MFD_CLOEXEC);
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
}

static void got_handle(void *d, struct zxdg_exported_v2 *e, const char *h) { (void)e; snprintf(d, 64, "%s", h); }
static const struct zxdg_exported_v2_listener exported_l = { got_handle };
static void got_destroyed(void *d, struct zxdg_imported_v2 *i) { (void)i; (*(int *)d)++; }
static const struct zxdg_imported_v2_listener imported_l = { got_destroyed };

static int is_hex32(const char *s) {
    if (strlen(s) != 32) return 0;
    for (; *s; s++) if (!strchr("0123456789abcdef", *s)) return 0;
    return 1;
}

static void sleep_ms(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

int main(int argc, char **argv) {
    Conn a, b;
    if (connect_to(&a) || connect_to(&b)) { fprintf(stderr, "dialog-client: no Wayland display\n"); return 2; }
    check("zxdg_exporter_v2 offered", a.exporter != NULL, 1);
    check("zxdg_importer_v2 offered", b.importer != NULL, 1);
    check("xdg_wm_dialog_v1 offered", b.dialogs != NULL, 1);
    if (!a.exporter || !b.importer || !b.dialogs) { printf("%d checks, %d failed\n", checks, fails); return 1; }

    /* A: the app's window, exported twice */
    Win *main_win = new_window(&a, "dialog-client: the window");
    draw(&a, main_win, 520, 360, 0xffd0d0d0u);
    char h1[64] = "", h2[64] = "";
    struct zxdg_exported_v2 *e1 = zxdg_exporter_v2_export_toplevel(a.exporter, main_win->surf);
    zxdg_exported_v2_add_listener(e1, &exported_l, h1);
    struct zxdg_exported_v2 *e2 = zxdg_exporter_v2_export_toplevel(a.exporter, main_win->surf);
    zxdg_exported_v2_add_listener(e2, &exported_l, h2);
    wl_display_roundtrip(a.dpy);
    check("the handle is 32 hex digits", is_hex32(h1), 1);
    check("a second export gets another handle", h2[0] && strcmp(h1, h2) != 0, 1);
    zxdg_exported_v2_destroy(e2);
    wl_display_roundtrip(a.dpy);

    /* B: a made-up handle, then A's */
    int bad_gone = 0, gone = 0, old_gone = 0;
    struct zxdg_imported_v2 *bad = zxdg_importer_v2_import_toplevel(b.importer, "00000000000000000000000000000000");
    zxdg_imported_v2_add_listener(bad, &imported_l, &bad_gone);
    struct zxdg_imported_v2 *old = zxdg_importer_v2_import_toplevel(b.importer, h2);
    zxdg_imported_v2_add_listener(old, &imported_l, &old_gone);
    struct zxdg_imported_v2 *imp = zxdg_importer_v2_import_toplevel(b.importer, h1);
    zxdg_imported_v2_add_listener(imp, &imported_l, &gone);
    wl_display_roundtrip(b.dpy);
    check("an unknown handle is told destroyed", bad_gone, 1);
    check("a handle whose export is gone is told destroyed", old_gone, 1);
    check("a live handle is not", gone, 0);

    /* B: a modal dialog over A's window */
    Win *dlg = new_window(&b, "dialog-client: the dialog");
    struct xdg_dialog_v1 *dd = xdg_wm_dialog_v1_get_xdg_dialog(b.dialogs, dlg->top);
    xdg_dialog_v1_set_modal(dd);
    zxdg_imported_v2_set_parent_of(imp, dlg->surf);
    draw(&b, dlg, 300, 160, 0xff2a6fd6u);
    check("the dialog is configured", dlg->configured, 1);
    check("the connection is still up", wl_display_get_error(b.dpy), 0);

    if (argc >= 3 && !strcmp(argv[1], "hold")) {
        for (int i = 0, n = atoi(argv[2]) * 4; i < n; i++) {
            sleep_ms(250);
            wl_display_roundtrip(a.dpy);
            wl_display_roundtrip(b.dpy);
        }
    }

    /* A's window goes away: B's import stops being valid */
    xdg_toplevel_destroy(main_win->top);
    xdg_surface_destroy(main_win->xs);
    wl_surface_destroy(main_win->surf);
    wl_display_roundtrip(a.dpy);
    wl_display_roundtrip(b.dpy);
    check("the import is told destroyed when the window goes", gone, 1);
    xdg_dialog_v1_destroy(dd);
    zxdg_imported_v2_destroy(imp);
    zxdg_imported_v2_destroy(bad);
    zxdg_imported_v2_destroy(old);
    zxdg_exported_v2_destroy(e1);
    wl_display_roundtrip(b.dpy);
    check("still up after the cleanup", wl_display_get_error(b.dpy), 0);

    /* a wl_surface destroyed before its xdg_toplevel (out of order, but a client may): the toplevel must not keep the
     * freed window, so making it a parent or a dialog afterwards changes nothing and the compositor stays up */
    Conn c;
    if (!connect_to(&c) && c.dialogs) {
        Win *gone_win = new_window(&c, "dialog-client: gone first");
        draw(&c, gone_win, 200, 120, 0xff808080u);
        Win *child = new_window(&c, "dialog-client: its child");
        draw(&c, child, 200, 120, 0xff606060u);
        wl_surface_destroy(gone_win->surf);
        wl_display_roundtrip(c.dpy);
        xdg_toplevel_set_parent(child->top, gone_win->top);
        struct xdg_dialog_v1 *d2 = xdg_wm_dialog_v1_get_xdg_dialog(c.dialogs, gone_win->top);
        xdg_dialog_v1_set_modal(d2);
        wl_display_roundtrip(c.dpy);
        check("a parent whose surface is gone changes nothing", wl_display_get_error(c.dpy), 0);
        /* the compositor still answers another client */
        wl_display_roundtrip(a.dpy);
        check("the compositor is still up", wl_display_get_error(a.dpy), 0);
        xdg_dialog_v1_destroy(d2);
        wl_display_disconnect(c.dpy);
    }

    /* a second xdg_surface, or a second xdg_toplevel, for the same window is a protocol error: the extra object
     * would otherwise keep pointing at the window after it is freed */
    for (int twice_top = 0; twice_top < 2; twice_top++) {
        Conn d;
        if (connect_to(&d)) continue;
        struct wl_surface *ws = wl_compositor_create_surface(d.comp);
        struct xdg_surface *x1 = xdg_wm_base_get_xdg_surface(d.wmbase, ws);
        if (twice_top) {
            xdg_surface_get_toplevel(x1);
            xdg_surface_get_toplevel(x1);
        } else {
            xdg_wm_base_get_xdg_surface(d.wmbase, ws);
        }
        wl_display_roundtrip(d.dpy);
        check(twice_top ? "a second toplevel is refused" : "a second xdg_surface is refused", wl_display_get_error(d.dpy) != 0, 1);
        wl_display_disconnect(d.dpy);
        wl_display_roundtrip(a.dpy);
        check("the compositor is still up after the refusal", wl_display_get_error(a.dpy), 0);
    }
    printf("%d checks, %d failed\n", checks, fails);
    wl_display_disconnect(b.dpy);
    wl_display_disconnect(a.dpy);
    return fails ? 1 : 0;
}
