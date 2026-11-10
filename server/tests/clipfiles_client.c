/* clipfiles_client.c — files on the clipboard (docs/WAYLAND.md, "Clipboard"): run inside a LordOS session, it
 * copies files the way a Linux file manager does and checks what the compositor kept and offers back.
 *
 *   1. text and a text/uri-list with this machine's files beside others' (another host, a NUL, https):
 *      once the app lets go of its selection, LordOS offers text/uri-list and gnome-copied-files with only
 *      this machine's files, re-encoded
 *   2. only a gnome-copied-files "cut": the text becomes the paths and the cut comes back as a copy (another
 *      app's cut never moves anything in Files)
 *   3. a list with no file of this machine: no file formats offered at all
 *   4. a list whose text names other things (another text, one path more, another order): text only, so a
 *      paste in Files never copies files the person didn't see on the clipboard
 *   5. a text of file:// URIs that name the same files: the list is kept
 * (A Flatpak app's or an isolated LordOS app's list is never kept either; this client can't be one, so that
 * decision is tested in tests/urilist_test.c, uril_accept.)
 *
 *   WAYLAND_DISPLAY=wayland-0 build/clipfiles-client      (prints ok/FAIL per check, exit 1 on a failure)
 * It replaces the session clipboard. */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wmbase;
static struct wl_seat *seat;
static struct wl_data_device_manager *ddm;
static uint32_t enter_serial;
static int fails, checks;

static void check(const char *what, long got, long want) {
    checks++;
    if (got == want) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}
static void check_str(const char *what, const char *got, const char *want) {
    checks++;
    if (!strcmp(got, want)) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want); }
}
static void sleep_ms(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wmbase = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
    else if (!strcmp(iface, wl_seat_interface.name) && !seat) seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
    else if (!strcmp(iface, wl_data_device_manager_interface.name)) ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, ver < 3 ? ver : 3);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };

/* the keyboard: only its enter serial matters (set_selection needs one) */
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t n) { (void)d; (void)k; (void)f; (void)n; close(fd); }
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) { (void)d; (void)k; (void)s; (void)keys; enter_serial = serial; }
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) { (void)d; (void)k; (void)serial; (void)s; }
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t st) { (void)d; (void)k; (void)serial; (void)t; (void)key; (void)st; }
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t a, uint32_t b, uint32_t c, uint32_t g) { (void)d; (void)k; (void)serial; (void)a; (void)b; (void)c; (void)g; }
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t dl) { (void)d; (void)k; (void)r; (void)dl; }
static const struct wl_keyboard_listener kb_l = { kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat };

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) { (void)d; xdg_surface_ack_configure(xs, serial); }
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)w; (void)h; (void)st; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };

static void map_window(void) {
    struct wl_surface *surf = wl_compositor_create_surface(comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wmbase, surf);
    xdg_surface_add_listener(xs, &xs_l, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top, &top_l, NULL);
    xdg_toplevel_set_title(top, "clipfiles-client");
    xdg_toplevel_set_app_id(top, "clipfiles-client");
    wl_surface_commit(surf);
    wl_display_roundtrip(dpy);
    int W = 320, H = 200, stride = W * 4, size = stride * H;
    int fd = memfd_create("clipfiles-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { perror("memfd"); exit(2); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int i = 0; i < W * H; i++) px[i] = 0xff3a7a4au;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    munmap(px, (size_t)size);
    wl_surface_attach(surf, b, 0, 0);
    wl_surface_damage(surf, 0, 0, W, H);
    wl_surface_commit(surf);
    wl_display_roundtrip(dpy);
}

/* our selection: each format's bytes */
typedef struct { const char *mime[4], *data[4]; int n; } Copy;
static void src_target(void *d, struct wl_data_source *s, const char *m) { (void)d; (void)s; (void)m; }
static void src_send(void *d, struct wl_data_source *s, const char *mime, int32_t fd) {
    Copy *c = d;
    (void)s;
    for (int i = 0; i < c->n; i++)
        if (!strcmp(mime, c->mime[i])) { size_t n = strlen(c->data[i]), off = 0; while (off < n) { ssize_t w = write(fd, c->data[i] + off, n - off); if (w <= 0) break; off += (size_t)w; } }
    close(fd);
}
static void src_cancelled(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void src_dnd(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void src_action(void *d, struct wl_data_source *s, uint32_t a) { (void)d; (void)s; (void)a; }
static const struct wl_data_source_listener src_l = { src_target, src_send, src_cancelled, src_dnd, src_dnd, src_action };

/* what LordOS offers back */
typedef struct { struct wl_data_offer *offer; char mimes[16][128]; int n; } Seen;
static void ofr_offer(void *d, struct wl_data_offer *o, const char *mime) {
    Seen *s = d;
    (void)o;
    if (s->n < 16) snprintf(s->mimes[s->n++], sizeof s->mimes[0], "%s", mime);
}
static void ofr_actions(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static const struct wl_data_offer_listener ofr_l = { ofr_offer, ofr_actions, ofr_actions };
static void dev_offer(void *d, struct wl_data_device *dev, struct wl_data_offer *o) {
    Seen *s = d;
    (void)dev;
    s->n = 0;
    wl_data_offer_add_listener(o, &ofr_l, s);
}
static void dev_enter(void *d, struct wl_data_device *dev, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o) { (void)d; (void)dev; (void)serial; (void)s; (void)x; (void)y; (void)o; }
static void dev_leave(void *d, struct wl_data_device *dev) { (void)d; (void)dev; }
static void dev_motion(void *d, struct wl_data_device *dev, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)dev; (void)t; (void)x; (void)y; }
static void dev_selection(void *d, struct wl_data_device *dev, struct wl_data_offer *o) { Seen *s = d; (void)dev; s->offer = o; }
static const struct wl_data_device_listener dev_l = { dev_offer, dev_enter, dev_leave, dev_motion, dev_leave, dev_selection };

static int offers(Seen *s, const char *mime) {
    for (int i = 0; i < s->n; i++) if (!strcmp(s->mimes[i], mime)) return 1;
    return 0;
}

/* reads one format of the offer ("" if nothing comes) */
static const char *receive(Seen *s, const char *mime) {
    static char buf[4096];
    int p[2];
    buf[0] = 0;
    if (!s->offer || pipe(p)) return buf;
    wl_data_offer_receive(s->offer, mime, p[1]);
    close(p[1]);
    wl_display_flush(dpy);
    size_t n = 0;
    struct pollfd pf = { p[0], POLLIN, 0 };
    while (n + 1 < sizeof buf && poll(&pf, 1, 2000) > 0) {
        ssize_t r = read(p[0], buf + n, sizeof buf - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    buf[n] = 0;
    close(p[0]);
    return buf;
}

/* copies as an app, lets go of it, and looks at what LordOS offers in its place */
static void copy_and_look(Copy *c, Seen *seen) {
    struct wl_data_device *dev = wl_data_device_manager_get_data_device(ddm, seat);
    Seen own = { 0 };
    wl_data_device_add_listener(dev, &dev_l, &own);
    struct wl_data_source *src = wl_data_device_manager_create_data_source(ddm);
    wl_data_source_add_listener(src, &src_l, c);
    for (int i = 0; i < c->n; i++) wl_data_source_offer(src, c->mime[i]);
    wl_data_device_set_selection(dev, src, enter_serial);
    for (int i = 0; i < 8; i++) { wl_display_roundtrip(dpy); sleep_ms(50); }    /* LordOS reads each format */
    wl_data_source_destroy(src);
    wl_data_device_release(dev);
    wl_display_roundtrip(dpy);
    memset(seen, 0, sizeof *seen);
    struct wl_data_device *dev2 = wl_data_device_manager_get_data_device(ddm, seat);
    wl_data_device_add_listener(dev2, &dev_l, seen);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
}

int main(void) {
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "clipfiles-client: no Wayland display\n"); return 2; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wmbase || !seat || !ddm) { fprintf(stderr, "clipfiles-client: missing globals\n"); return 2; }
    xdg_wm_base_add_listener(wmbase, &wm_l, NULL);
    struct wl_keyboard *kb = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(kb, &kb_l, NULL);
    map_window();
    for (int i = 0; i < 40 && !enter_serial; i++) { wl_display_roundtrip(dpy); sleep_ms(50); }
    check("the window has the keyboard", enter_serial != 0, 1);

    Seen seen;
    /* 1. a file manager's copy, with files that aren't this machine's mixed in */
    Copy c1 = { { "text/plain;charset=utf-8", "text/uri-list" },
                { "/tmp/lord clip/a\n/tmp/b",
                  "# a comment\r\nfile:///tmp/lord%20clip/a\r\nfile://otherhost/etc/passwd\r\nfile:///tmp/bad%00nul\r\n"
                  "https://example.org/x\r\nfile://localhost/tmp/b\r\n" }, 2 };
    copy_and_look(&c1, &seen);
    check("1: offered again", seen.offer != NULL, 1);
    check("1: text/uri-list offered", offers(&seen, "text/uri-list"), 1);
    check("1: gnome-copied-files offered", offers(&seen, "x-special/gnome-copied-files"), 1);
    check_str("1: only this machine's files, re-encoded", receive(&seen, "text/uri-list"), "file:///tmp/lord%20clip/a\r\nfile:///tmp/b\r\n");
    check_str("1: as a gnome list", receive(&seen, "x-special/gnome-copied-files"), "copy\nfile:///tmp/lord%20clip/a\nfile:///tmp/b");
    check_str("1: the text is the app's", receive(&seen, "text/plain;charset=utf-8"), "/tmp/lord clip/a\n/tmp/b");

    /* 2. only a cut, as a gnome list */
    Copy c2 = { { "x-special/gnome-copied-files" }, { "cut\nfile:///tmp/c%C3%B1\nfile://evil/x" }, 1 };
    copy_and_look(&c2, &seen);
    check_str("2: the text is the paths", receive(&seen, "text/plain;charset=utf-8"), "/tmp/c\xc3\xb1");
    check_str("2: another app's cut comes back as a copy", receive(&seen, "x-special/gnome-copied-files"), "copy\nfile:///tmp/c%C3%B1");

    /* 3. nothing of this machine: plain text only */
    Copy c3 = { { "text/plain", "text/uri-list" }, { "x", "file://otherhost/a\r\nsmb://h/s\r\n" }, 2 };
    copy_and_look(&c3, &seen);
    check_str("3: the text", receive(&seen, "text/plain"), "x");
    check("3: no text/uri-list", offers(&seen, "text/uri-list"), 0);
    check("3: no gnome-copied-files", offers(&seen, "x-special/gnome-copied-files"), 0);
    check_str("3: asking anyway gets nothing", receive(&seen, "text/uri-list"), "");

    /* 4. the text doesn't name the files: the list is dropped, the text stays */
    Copy c4 = { { "text/plain;charset=utf-8", "text/uri-list" }, { "holiday photos", "file:///tmp/x\r\n" }, 2 };
    copy_and_look(&c4, &seen);
    check_str("4: the text", receive(&seen, "text/plain;charset=utf-8"), "holiday photos");
    check("4: another text: no text/uri-list", offers(&seen, "text/uri-list"), 0);
    check("4: another text: no gnome-copied-files", offers(&seen, "x-special/gnome-copied-files"), 0);
    Copy c4b = { { "text/plain;charset=utf-8", "x-special/gnome-copied-files" }, { "/tmp/x", "copy\nfile:///tmp/x\nfile:///tmp/.ssh/id" }, 2 };
    copy_and_look(&c4b, &seen);
    check("4: a file the text doesn't name: no text/uri-list", offers(&seen, "text/uri-list"), 0);
    Copy c4c = { { "text/plain;charset=utf-8", "text/uri-list" }, { "/tmp/y\n/tmp/x", "file:///tmp/x\r\nfile:///tmp/y\r\n" }, 2 };
    copy_and_look(&c4c, &seen);
    check("4: another order: no text/uri-list", offers(&seen, "text/uri-list"), 0);

    /* 5. the text as URIs of the same files */
    Copy c5 = { { "text/plain;charset=utf-8", "text/uri-list" }, { "file:///tmp/x\nfile:///tmp/y\n", "file:///tmp/x\r\nfile:///tmp/y\r\n" }, 2 };
    copy_and_look(&c5, &seen);
    check("5: the same files as URIs: text/uri-list", offers(&seen, "text/uri-list"), 1);
    check_str("5: the list", receive(&seen, "text/uri-list"), "file:///tmp/x\r\nfile:///tmp/y\r\n");

    check("the connection is still up", wl_display_get_error(dpy), 0);
    printf("%d checks, %d failed\n", checks, fails);
    wl_display_disconnect(dpy);
    return fails ? 1 : 0;
}
