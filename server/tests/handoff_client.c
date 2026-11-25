/* handoff_client.c — a Wayland app that runs once, as a browser does (tests/handoff_headless.sh; docs/APPS.md, "An app
 * that is already running"). Started with an app id (the .desktop id that launches it, also its xdg_toplevel app_id):
 *
 *   - the first copy listens on $XDG_RUNTIME_DIR/handoff-<id>.sock and maps one window, using the launch's
 *     XDG_ACTIVATION_TOKEN on it before it shows (so that token is used up, as Google Chrome's first window does);
 *   - a later copy finds that socket, hands its launch over ("open") and exits at once, without using the token it was
 *     given and without ever connecting to Wayland: Google Chrome 158's "Opening in existing browser session";
 *   - the first copy, told "open": with a window, asks for a token of its own (xdg_activation_token_v1 with no serial,
 *     no surface, no app id) and activates its window with it, exactly what Chrome's process does for the window it
 *     already shows (its profile chooser); with no window, maps a new one.
 *
 * "--windowless": the first copy closes its window a second after mapping it and keeps running with none (Chrome with
 * "Continue running background apps"). "--quit": tells the copy running to end, nothing else. The first copy ends when
 * lord-server goes away, on "quit", or after 120 s. Exit 0. */
#include <errno.h>
#include <poll.h>
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
#include "client/xdg-activation-v1-client-protocol.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wmbase;
static struct xdg_activation_v1 *act;
static const char *app_id;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---- the window ---- */
static struct wl_surface *surf;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *top;
static int mapped;
static double mapped_at;

static struct wl_buffer *buffer(int W, int H) {
    int stride = W * 4, size = stride * H;
    int fd = memfd_create("handoff-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { perror("memfd"); exit(2); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int i = 0; i < W * H; i++) px[i] = 0xff40a060u;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    munmap(px, (size_t)size);
    return b;
}
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
    (void)d;
    xdg_surface_ack_configure(xs, serial);
    if (mapped) { wl_surface_commit(surf); return; }
    wl_surface_attach(surf, buffer(320, 200), 0, 0);
    wl_surface_damage(surf, 0, 0, 320, 200);
    wl_surface_commit(surf);
    mapped = 1;
    mapped_at = now_ms();
}
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)w; (void)h; (void)st; }
static void close_window(void);
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; close_window(); }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };
static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };

static void open_window(void) {
    surf = wl_compositor_create_surface(comp);
    xsurf = xdg_wm_base_get_xdg_surface(wmbase, surf);
    xdg_surface_add_listener(xsurf, &xs_l, NULL);
    top = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(top, &top_l, NULL);
    xdg_toplevel_set_app_id(top, app_id);
    xdg_toplevel_set_title(top, "Handoff test");
    wl_surface_commit(surf);
    mapped = 0;
}
static void close_window(void) {
    if (!surf) return;
    xdg_toplevel_destroy(top);
    xdg_surface_destroy(xsurf);
    wl_surface_destroy(surf);
    surf = NULL;
    mapped = 0;
}

/* ---- xdg-activation: a token of its own, as Chrome asks for one (no serial, surface or app id) ---- */
static char own_token[128];
static void tok_done(void *d, struct xdg_activation_token_v1 *t, const char *token) {
    (void)d;
    snprintf(own_token, sizeof own_token, "%s", token);
    xdg_activation_token_v1_destroy(t);
}
static const struct xdg_activation_token_v1_listener tok_l = { tok_done };
static void activate_with_own_token(void) {
    if (!act || !surf) return;
    own_token[0] = 0;
    struct xdg_activation_token_v1 *t = xdg_activation_v1_get_activation_token(act);
    xdg_activation_token_v1_add_listener(t, &tok_l, NULL);
    xdg_activation_token_v1_commit(t);
    while (!own_token[0] && wl_display_roundtrip(dpy) >= 0) {}
    if (own_token[0]) xdg_activation_v1_activate(act, own_token, surf);
    wl_display_flush(dpy);
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) { wmbase = wl_registry_bind(r, name, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(wmbase, &wm_l, NULL); }
    else if (!strcmp(iface, xdg_activation_v1_interface.name)) act = wl_registry_bind(r, name, &xdg_activation_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

int main(int argc, char **argv) {
    int windowless = 0, quit = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--windowless")) windowless = 1;
        else if (!strcmp(argv[i], "--quit")) quit = 1;
        else app_id = argv[i];
    }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!app_id || !app_id[0] || strchr(app_id, '/') || !rt) { fprintf(stderr, "usage: handoff-client <app id> [--windowless]\n"); return 2; }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if ((size_t)snprintf(a.sun_path, sizeof a.sun_path, "%s/handoff-%s.sock", rt, app_id) >= sizeof a.sun_path) return 2;

    /* a copy already runs: the launch goes to it, and this one ends (its token never used) */
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof a)) {
        const char *msg = quit ? "quit\n" : "open\n";
        if (write(s, msg, strlen(msg)) < 0) {}
        close(s);
        printf("handoff-client %s: handed over to the copy already running\n", app_id);
        return 0;
    }
    if (s >= 0) close(s);
    if (quit) return 0;
    unlink(a.sun_path);
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 4)) { perror("handoff-client: listen"); return 2; }

    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "handoff-client: no Wayland display\n"); unlink(a.sun_path); return 2; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wmbase || !act) { fprintf(stderr, "handoff-client: missing globals\n"); unlink(a.sun_path); return 2; }

    open_window();
    /* the launch's token, used at once on the new surface before it shows: what Google Chrome 158 does (the token is
     * used up; the new window comes forward anyway, as every new window does) */
    const char *tk = getenv("XDG_ACTIVATION_TOKEN");
    if (tk && tk[0]) xdg_activation_v1_activate(act, tk, surf);
    unsetenv("XDG_ACTIVATION_TOKEN");
    double end = now_ms() + 120000;
    int closed_once = 0;
    while (now_ms() < end) {
        if (windowless && !closed_once && mapped && now_ms() - mapped_at > 1000) {
            close_window();
            closed_once = 1;
            printf("handoff-client %s: closed its last window, still running\n", app_id);
            fflush(stdout);
        }
        wl_display_flush(dpy);
        struct pollfd p[2] = { { wl_display_get_fd(dpy), POLLIN, 0 }, { ls, POLLIN, 0 } };
        if (poll(p, 2, 100) < 0 && errno != EINTR) break;
        if (p[0].revents & (POLLERR | POLLHUP)) break;
        if ((p[0].revents & POLLIN) && wl_display_dispatch(dpy) < 0) break;
        if (p[1].revents & POLLIN) {
            int c = accept(ls, NULL, NULL);
            char buf[16] = "";
            ssize_t n = c >= 0 ? read(c, buf, sizeof buf - 1) : -1;
            if (c >= 0) close(c);
            if (n > 0 && !strncmp(buf, "quit", 4)) break;
            if (n > 0 && !strncmp(buf, "open", 4)) {
                if (surf) activate_with_own_token();
                else open_window();
                printf("handoff-client %s: took a launch over (%s)\n", app_id, surf && mapped ? "its window, forward" : "a new window");
                fflush(stdout);
            }
        }
        if (wl_display_dispatch_pending(dpy) < 0) break;
    }
    unlink(a.sun_path);
    close(ls);
    wl_display_disconnect(dpy);
    return 0;
}
