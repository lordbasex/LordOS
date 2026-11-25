/* fsub_client.c — a fullscreen window whose content is a desync subsurface, as Firefox draws a page (GitHub #232): each
 * commit of the subsurface repaints its own box, not all of the window's place, as in a window. Before, every frame of
 * a fullscreen video in Firefox repainted the whole window under it (and on the GPU path uploaded it again): GPU use
 * went from 7 % windowed to 17 % fullscreen. Against a headless build/lord-server-test with --test-hooks
 * (tests/fsub_headless.sh): the hooks' "painted" counts the UI pixels repainted. Prints ok/FAIL; exit 1 on a failure,
 * 77 without the hooks. */
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_subcompositor *subcomp;
static struct wl_shm *shm;
static struct xdg_wm_base *wmbase;
static int fails, checks, conf_w, conf_h, configured, fullscreen;
static uint32_t conf_serial;

static void check(const char *what, int ok, const char *fmt, ...) {
    checks++;
    char detail[200] = "";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    if (ok) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: %s\n", what, detail); }
}

/* a test hooks command; its answer in out (NULL: none wanted); 0 when "ok" */
static int th(const char *cmd, char *out, size_t outn) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (!rt || (size_t)snprintf(a.sun_path, sizeof a.sun_path, "%s/lordos-test-hooks.sock", rt) >= sizeof a.sun_path) return -1;
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    char buf[64];
    struct pollfd p = { s, POLLIN, 0 };
    ssize_t n = -1;
    if (!connect(s, (struct sockaddr *)&a, sizeof a) && send(s, cmd, strlen(cmd), MSG_NOSIGNAL) >= 0 && poll(&p, 1, 3000) == 1)
        n = recv(s, buf, sizeof buf - 1, 0);
    close(s);
    if (n <= 0) return -1;
    buf[n] = 0;
    if (out) snprintf(out, outn, "%s", buf);
    return strncmp(buf, "ok", 2) ? -1 : 0;
}
static long long painted(void) {
    char r[64];
    if (th("painted", r, sizeof r)) return -1;
    return atoll(r + 2);
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_subcompositor_interface.name)) subcomp = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wmbase = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) { (void)d; (void)xs; conf_serial = serial; configured = 1; }
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
    (void)d; (void)t;
    conf_w = w;
    conf_h = h;
    fullscreen = 0;
    uint32_t *s;
    wl_array_for_each(s, st) if (*s == XDG_TOPLEVEL_STATE_FULLSCREEN) fullscreen = 1;
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };

static struct wl_buffer *buffer(int W, int H, uint32_t color) {
    int stride = W * 4, size = stride * H;
    int fd = memfd_create("fsub-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { perror("memfd"); exit(2); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int i = 0; i < W * H; i++) px[i] = color;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    munmap(px, (size_t)size);
    return b;
}
static void pump(int ms) {
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        wl_display_flush(dpy);
        clock_gettime(CLOCK_MONOTONIC, &t);
        long left = ms - ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0) break;
        struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
        if (wl_display_prepare_read(dpy) == 0) {
            if (poll(&p, 1, (int)left) > 0) wl_display_read_events(dpy);
            else wl_display_cancel_read(dpy);
        }
        wl_display_dispatch_pending(dpy);
    }
}

#define SW 160
#define SH 90
#define COMMITS 30

/* COMMITS commits of the subsurface, each a new frame of "video": the UI pixels repainted per commit */
static long long per_commit(struct wl_surface *sub_s) {
    pump(600);
    { long long q = painted(); if (getenv("FSUB_DEBUG")) fprintf(stderr, "idle before: %lld\n", q); }   /* what came before */
    for (int i = 0; i < COMMITS; i++) {
        wl_surface_attach(sub_s, buffer(SW, SH, i & 1 ? 0xff3060c0u : 0xffc06030u), 0, 0);
        wl_surface_damage(sub_s, 0, 0, SW, SH);
        wl_surface_commit(sub_s);
        pump(40);
        if (getenv("FSUB_DEBUG")) fprintf(stderr, "commit %d: %lld\n", i, painted());
    }
    pump(300);
    long long p = painted();
    return p < 0 ? -1 : p / COMMITS;
}

int main(void) {
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "fsub-client: no Wayland display\n"); return 2; }
    if (painted() < 0) { printf("skipped: needs lord-server-test --test-hooks\n"); return 77; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !subcomp || !shm || !wmbase) { fprintf(stderr, "fsub-client: no compositor, subcompositor, shm or xdg_wm_base\n"); return 2; }
    xdg_wm_base_add_listener(wmbase, &wm_l, NULL);

    struct wl_surface *surf = wl_compositor_create_surface(comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wmbase, surf);
    xdg_surface_add_listener(xs, &xs_l, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top, &top_l, NULL);
    xdg_toplevel_set_app_id(top, "fsub-client");
    xdg_toplevel_set_title(top, "fsub-client");
    wl_surface_commit(surf);
    wl_display_roundtrip(dpy);
    check("the window is configured", configured, "no configure");
    xdg_surface_ack_configure(xs, conf_serial);
    int w = 640, h = 400;
    wl_surface_attach(surf, buffer(w, h, 0xff101010u), 0, 0);
    wl_surface_damage(surf, 0, 0, w, h);
    wl_surface_commit(surf);
    pump(800);

    /* the page: a desync subsurface, as Firefox's */
    struct wl_surface *sub_s = wl_compositor_create_surface(comp);
    struct wl_subsurface *sub = wl_subcompositor_get_subsurface(subcomp, sub_s, surf);
    wl_subsurface_set_desync(sub);
    wl_subsurface_set_position(sub, 100, 80);
    wl_surface_attach(sub_s, buffer(SW, SH, 0xffc06030u), 0, 0);
    wl_surface_damage(sub_s, 0, 0, SW, SH);
    wl_surface_commit(sub_s);
    wl_surface_commit(surf);                            /* the position applies with the window's commit */
    pump(500);
    long long win = per_commit(sub_s);
    long long box = (long long)(SW + 6) * (SH + 6);
    check("in a window: a subsurface commit repaints about its box", win >= 0 && win <= 3 * box, "%lld pixels a commit, its box is %lld", win, box);

    configured = 0;
    xdg_toplevel_set_fullscreen(top, NULL);
    for (int i = 0; i < 50 && !(configured && fullscreen); i++) pump(40);
    check("the window went fullscreen", configured && fullscreen, "no fullscreen configure");
    xdg_surface_ack_configure(xs, conf_serial);
    if (conf_w > 0 && conf_h > 0) { w = conf_w; h = conf_h; }
    wl_surface_attach(surf, buffer(w, h, 0xff101010u), 0, 0);
    wl_surface_damage(surf, 0, 0, w, h);
    wl_subsurface_set_position(sub, w / 2 - SW / 2, h / 2 - SH / 2);   /* the video in the middle, away from the theme's bars */
    wl_surface_commit(surf);
    pump(1500);                                         /* the fullscreen animation, if any, is over */
    long long fs = per_commit(sub_s);
    check("fullscreen: a subsurface commit repaints about its box, not the window's place (#232)", fs >= 0 && fs <= 3 * box,
          "%lld pixels a commit, its box is %lld, the screen %d×%d", fs, box, w, h);

    check("still connected at the end", wl_display_get_error(dpy) == 0, "protocol error");
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
