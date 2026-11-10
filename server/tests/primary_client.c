/* primary_client.c — a Wayland client that checks the primary selection (zwp_primary_selection_v1, wl.c;
 * docs/WAYLAND.md, "Clipboard and primary selection"): run inside a LordOS session, it maps a window,
 * waits for the keyboard and then
 *
 *   - sets a selection with a made-up serial: the compositor ignores it (its source is never asked for data);
 *   - sets it with the serial of its keyboard enter: the source is asked for text/plain;charset=utf-8, and
 *     the offer that comes back gives that text, for the three text types it lists;
 *   - asks that offer for a type it didn't list: nothing comes;
 *   - sets a second source: the first one is told it was cancelled;
 *   - sets a source with no text type: it is never asked for data, and the text stays.
 *
 *   make -C server build/primary-client
 *   WAYLAND_DISPLAY=wayland-0 build/primary-client      (prints ok/FAIL per check, exit 1 on a failure)
 *
 * With Settings → Mouse & Trackpad → "Middle click pastes" off, the offers come empty: the checks that
 * read one fail, as they should. */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"
#include "client/primary-selection-unstable-v1-client-protocol.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_seat *seat;
static struct zwp_primary_selection_device_manager_v1 *pdm;
static int focused, configured;
static uint32_t enter_serial;
static struct zwp_primary_selection_offer_v1 *cur;
static int changes, cur_mimes;

static int fails, checks;
static void check(const char *what, long got, long want) {
    checks++;
    if (got == want) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d; (void)ver;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, zwp_primary_selection_device_manager_v1_interface.name))
        pdm = wl_registry_bind(r, name, &zwp_primary_selection_device_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg = { reg_global, reg_remove };

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };
static void xs_configure(void *d, struct xdg_surface *s, uint32_t serial) { (void)d; xdg_surface_ack_configure(s, serial); configured = 1; }
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)w; (void)h; (void)st; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener top_l = { .configure = top_configure, .close = top_close };

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t n) { (void)d; (void)k; (void)f; (void)n; close(fd); }
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *keys) {
    (void)d; (void)k; (void)sf; (void)keys;
    focused = 1;
    enter_serial = s;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) { (void)d; (void)k; (void)s; (void)sf; focused = 0; }
static void kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t st) { (void)d; (void)k; (void)s; (void)t; (void)key; (void)st; }
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t g) { (void)d; (void)k; (void)s; (void)a; (void)b; (void)c; (void)g; }
static const struct wl_keyboard_listener kb_l = { kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, NULL };

/* the offers: the newest one, and how many text types it listed */
static void poff_offer(void *d, struct zwp_primary_selection_offer_v1 *o, const char *m) {
    (void)d;
    if (o == cur && (!strcmp(m, "text/plain;charset=utf-8") || !strcmp(m, "text/plain") || !strcmp(m, "UTF8_STRING"))) cur_mimes++;
}
static const struct zwp_primary_selection_offer_v1_listener poff_l = { poff_offer };
static void pdev_offer(void *d, struct zwp_primary_selection_device_v1 *dv, struct zwp_primary_selection_offer_v1 *o) {
    (void)d; (void)dv;
    if (cur) zwp_primary_selection_offer_v1_destroy(cur);
    cur = o;
    cur_mimes = 0;
    zwp_primary_selection_offer_v1_add_listener(o, &poff_l, NULL);
}
static void pdev_selection(void *d, struct zwp_primary_selection_device_v1 *dv, struct zwp_primary_selection_offer_v1 *o) {
    (void)d; (void)dv;
    if (o) changes++;
}
static const struct zwp_primary_selection_device_v1_listener pdev_l = { pdev_offer, pdev_selection };

/* a source: what it gives when asked, how many times it was asked and whether it was cancelled */
typedef struct { struct zwp_primary_selection_source_v1 *src; const char *text; int sends, cancelled; char mime[64]; } Src;
static void src_send(void *d, struct zwp_primary_selection_source_v1 *s, const char *mime, int32_t fd) {
    (void)s;
    Src *x = d;
    x->sends++;
    snprintf(x->mime, sizeof x->mime, "%s", mime);
    size_t n = strlen(x->text), off = 0;
    while (off < n) { ssize_t w = write(fd, x->text + off, n - off); if (w <= 0) break; off += (size_t)w; }
    close(fd);
}
static void src_cancelled(void *d, struct zwp_primary_selection_source_v1 *s) { (void)s; ((Src *)d)->cancelled = 1; }
static const struct zwp_primary_selection_source_v1_listener src_l = { src_send, src_cancelled };

static void new_source(Src *x, const char *text, const char *const *mimes) {
    memset(x, 0, sizeof *x);
    x->text = text;
    x->src = zwp_primary_selection_device_manager_v1_create_source(pdm);
    zwp_primary_selection_source_v1_add_listener(x->src, &src_l, x);
    for (int i = 0; mimes[i]; i++) zwp_primary_selection_source_v1_offer(x->src, mimes[i]);
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; }

/* Dispatches until cond() or the timeout. */
static int wait_for(int (*cond)(void), double secs) {
    double end = now() + secs;
    while ((!cond || !cond()) && now() < end) {
        wl_display_flush(dpy);
        struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
        if (poll(&p, 1, 50) > 0) { if (wl_display_dispatch(dpy) < 0) return 0; }
        else wl_display_dispatch_pending(dpy);
    }
    return cond ? cond() : 1;
}

/* Reads what the newest offer gives for mime (at most size-1 bytes; EOF or 2 s). */
static size_t receive(const char *mime, char *buf, size_t size) {
    int p[2];
    buf[0] = 0;
    if (!cur || pipe(p)) return 0;
    zwp_primary_selection_offer_v1_receive(cur, mime, p[1]);
    close(p[1]);
    size_t n = 0;
    double end = now() + 2;
    while (n + 1 < size && now() < end) {
        wl_display_flush(dpy);
        wl_display_dispatch_pending(dpy);
        struct pollfd q = { p[0], POLLIN, 0 };
        if (poll(&q, 1, 50) <= 0) continue;
        ssize_t r = read(p[0], buf + n, size - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    buf[n] = 0;
    close(p[0]);
    return n;
}

static int focus_cond(void) { return focused; }
static int base;
static int changed_cond(void) { return changes > base; }

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IOLBF, 0);
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "no Wayland\n"); return 2; }
    struct wl_registry *r = wl_display_get_registry(dpy);
    wl_registry_add_listener(r, &reg, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wm || !seat || !pdm) { fprintf(stderr, "a global is missing\n"); return 2; }
    xdg_wm_base_add_listener(wm, &wm_l, NULL);
    struct wl_keyboard *kb = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(kb, &kb_l, NULL);
    struct zwp_primary_selection_device_v1 *pdev = zwp_primary_selection_device_manager_v1_get_device(pdm, seat);
    zwp_primary_selection_device_v1_add_listener(pdev, &pdev_l, NULL);
    /* a small window, so the compositor gives it the keyboard */
    struct wl_surface *s = wl_compositor_create_surface(comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, s);
    xdg_surface_add_listener(xs, &xs_l, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top, &top_l, NULL);
    xdg_toplevel_set_title(top, "Primary selection test");
    xdg_toplevel_set_app_id(top, "primary-client");
    wl_surface_commit(s);
    while (!configured && wl_display_dispatch(dpy) >= 0) {}
    int w = 240, h = 120, stride = w * 4;
    int fd = memfd_create("primary-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, stride * h)) return 2;
    uint32_t *px = mmap(NULL, (size_t)stride * h, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) return 2;
    for (int i = 0; i < w * h; i++) px[i] = 0xff60a030;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, stride * h);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_surface_attach(s, buf, 0, 0);
    wl_surface_commit(s);
    if (!wait_for(focus_cond, 10)) { printf("FAIL: no keyboard focus\n"); return 1; }
    wait_for(NULL, 0.3);                /* the offer of what was selected before, if anything */

    static const char *const text_types[] = { "text/plain", "text/plain;charset=utf-8", "UTF8_STRING", NULL };
    static const char *const image_only[] = { "image/png", NULL };
    const char *want = "primary-client \xc3\xb1 \xe2\x9c\x93";
    char got[4096];

    Src a;
    new_source(&a, want, text_types);
    zwp_primary_selection_device_v1_set_selection(pdev, a.src, enter_serial + 100000);
    wait_for(NULL, 0.8);
    check("a made-up serial: the source is never asked", a.sends, 0);

    base = changes;
    zwp_primary_selection_device_v1_set_selection(pdev, a.src, enter_serial);
    int changed = wait_for(changed_cond, 3);
    check("the enter serial: the source is asked once", a.sends, 1);
    check("for text/plain;charset=utf-8", !strcmp(a.mime, "text/plain;charset=utf-8"), 1);
    check("a new offer comes back", changed, 1);
    wait_for(NULL, 0.2);
    check("it lists the three text types", cur_mimes, 3);
    receive("text/plain;charset=utf-8", got, sizeof got);
    check("it gives the text (utf-8)", !strcmp(got, want), 1);
    receive("UTF8_STRING", got, sizeof got);
    check("it gives the text (UTF8_STRING)", !strcmp(got, want), 1);
    check("a type it didn't list gives nothing", (long)receive("application/octet-stream", got, sizeof got), 0);

    Src b;
    new_source(&b, "second", text_types);
    base = changes;
    zwp_primary_selection_device_v1_set_selection(pdev, b.src, enter_serial);
    wait_for(changed_cond, 3);
    check("a second source: the first is cancelled", a.cancelled, 1);
    receive("text/plain;charset=utf-8", got, sizeof got);
    check("and the offer gives the second text", !strcmp(got, "second"), 1);

    Src c;
    new_source(&c, "an image", image_only);
    zwp_primary_selection_device_v1_set_selection(pdev, c.src, enter_serial);
    wait_for(NULL, 0.8);
    check("a source with no text type is never asked", c.sends, 0);
    check("and the second source is cancelled", b.cancelled, 1);
    receive("text/plain;charset=utf-8", got, sizeof got);
    check("the text stays", !strcmp(got, "second"), 1);

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
