/* wlperf_client.c — a plain Wayland client for the compositor's frame and title work (docs/WAYLAND.md, "Frames,
 * titles and damage"). One mode per run:
 *
 *   titles      a window mapped as "A"; "A" set 1000 times more, then 1000 different titles in one burst ("b0"…"b999"),
 *               then "end": lord-server must tell JS at most once for the repeats and only a few times for the burst
 *               (tests/wlperf_headless.sh counts the events the theme gets)
 *   frames N    redraws on every frame callback for N seconds and prints the callbacks per second (and the
 *               presentation feedback it got)
 *   damage N [S] a 640×480 window that changes a 10×10 square N times a second for S s (3), damaging only that square
 *               (surface or buffer coordinates, alternately); prints the frames done
 *   subsurface [MS] a 640×480 window with menus drawn as desync subsurfaces, as GTK 3 maps Firefox's <select> lists: one
 *               shown, redrawn on its own, destroyed with no commit of the window, a new one elsewhere, one moved with
 *               the window's commit, and one destroyed while a subsurface of its own is still there (Firefox's order);
 *               the window damages one pixel in a corner after each step so a frame is painted (LORD_PAINT_VERIFY); each
 *               step is held MS ms (250)
 *   transform   a 200×100 buffer with buffer_transform 90: the window is 100×200 (geometry checked by the script)
 *   idle        a 300 ms ext-idle-notify notification for 3 s: prints how many "idled" came (none until the "idle"
 *               permission is allowed; tests/wlperf_headless.sh answers the prompt)
 *   icon        a window that sets its own icon (xdg-toplevel-icon: the 64 px buffer kept, a 300 px one refused), and a
 *               non-square buffer that must be invalid_buffer
 *   protocols   the globals and versions of items 4 and 7 (xdg_wm_base 7, linux-dmabuf 5, wp_presentation 2,
 *               cursor-shape 2, single-pixel-buffer, alpha-modifier): the capabilities, the bounds, a single-pixel
 *               buffer at 50 % opacity; no drm-lease device (no VR headset, no card: docs/WAYLAND.md "DRM leases")
 *
 *   WAYLAND_DISPLAY=wayland-0 build/wlperf-client MODE [ARG]   (exit 0, or 1 on a protocol error or a failed check) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"
#include "client/presentation-time-client-protocol.h"
#include "client/cursor-shape-v1-client-protocol.h"
#include "client/linux-dmabuf-v1-client-protocol.h"
#include "client/single-pixel-buffer-v1-client-protocol.h"
#include "client/alpha-modifier-v1-client-protocol.h"
#include "client/ext-idle-notify-v1-client-protocol.h"
#include "client/xdg-toplevel-icon-v1-client-protocol.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wmbase;
static struct wp_presentation *pres;
static struct wp_single_pixel_buffer_manager_v1 *spb;
static struct wp_alpha_modifier_v1 *alpha;
static struct ext_idle_notifier_v1 *idler;
static struct xdg_toplevel_icon_manager_v1 *iconm;
static struct wl_seat *seat;
static struct wl_subcompositor *subcomp;
static uint32_t ver_wm, ver_dmabuf, ver_pres, ver_cursor, ver_spb, ver_alpha, ver_lease;
static int fails;

static void check(const char *what, long got, long want) {
    if (got == want) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        ver_wm = ver;
        wmbase = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < (uint32_t)xdg_wm_base_interface.version ? ver : (uint32_t)xdg_wm_base_interface.version);
    } else if (!strcmp(iface, wp_presentation_interface.name)) {
        ver_pres = ver;
        pres = wl_registry_bind(r, name, &wp_presentation_interface, 1);
    } else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) ver_dmabuf = ver;
    else if (!strcmp(iface, wp_cursor_shape_manager_v1_interface.name)) ver_cursor = ver;
    else if (!strcmp(iface, wp_single_pixel_buffer_manager_v1_interface.name)) {
        ver_spb = ver;
        spb = wl_registry_bind(r, name, &wp_single_pixel_buffer_manager_v1_interface, 1);
    } else if (!strcmp(iface, xdg_toplevel_icon_manager_v1_interface.name)) iconm = wl_registry_bind(r, name, &xdg_toplevel_icon_manager_v1_interface, 1);
    else if (!strcmp(iface, ext_idle_notifier_v1_interface.name)) idler = wl_registry_bind(r, name, &ext_idle_notifier_v1_interface, 1);
    else if (!strcmp(iface, "wp_drm_lease_device_v1")) ver_lease = ver;
    else if (!strcmp(iface, wl_subcompositor_interface.name)) subcomp = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, wp_alpha_modifier_v1_interface.name)) {
        ver_alpha = ver;
        alpha = wl_registry_bind(r, name, &wp_alpha_modifier_v1_interface, 1);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };

typedef struct {
    int conf;
    uint32_t serial;
    int w, h, bounds_w, bounds_h, caps_fullscreen, caps_menu, caps_max, caps_min, nstates;
} Win;
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) { Win *w = d; w->conf++; w->serial = serial; (void)xs; }
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
    (void)t;
    Win *win = d;
    win->w = w;
    win->h = h;
    win->nstates = (int)(st->size / sizeof(uint32_t));
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)t; Win *win = d; win->bounds_w = w; win->bounds_h = h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) {
    (void)t;
    Win *win = d;
    uint32_t *p;
    wl_array_for_each(p, c) {
        if (*p == XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN) win->caps_fullscreen = 1;
        if (*p == XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU) win->caps_menu = 1;
        if (*p == XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE) win->caps_max = 1;
        if (*p == XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE) win->caps_min = 1;
    }
}
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };

typedef struct { struct wl_buffer *b; uint32_t *px; int w, h; } Buf;
static Buf buffer(int W, int H, uint32_t color) {
    int stride = W * 4, size = stride * H;
    int fd = memfd_create("wlperf-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { perror("memfd"); exit(2); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int i = 0; i < W * H; i++) px[i] = color;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return (Buf){ b, px, W, H };
}

static struct wl_surface *surf;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *top;
static Win win;

/* a window: configured, acked, drawn with buf */
static void window(const char *title, Buf *buf, int transform) {
    surf = wl_compositor_create_surface(comp);
    xsurf = xdg_wm_base_get_xdg_surface(wmbase, surf);
    xdg_surface_add_listener(xsurf, &xs_l, &win);
    top = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(top, &top_l, &win);
    xdg_toplevel_set_app_id(top, "wlperf-client");
    xdg_toplevel_set_title(top, title);
    wl_surface_commit(surf);
    wl_display_roundtrip(dpy);
    if (!win.conf) { printf("FAIL no configure\n"); exit(1); }
    xdg_surface_ack_configure(xsurf, win.serial);
    if (transform) wl_surface_set_buffer_transform(surf, transform);
    wl_surface_attach(surf, buf->b, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, buf->w, buf->h);
    wl_surface_commit(surf);
    wl_display_roundtrip(dpy);
}

static void pause_ms(int ms) {
    double end = now_s() + ms / 1000.0;
    while (now_s() < end) {
        wl_display_flush(dpy);
        wl_display_dispatch_pending(dpy);
        usleep(5000);
        wl_display_roundtrip(dpy);
    }
}

/* ---- frames ---- */
static int frames, presented, discarded, want_more;
static void frame_done(void *d, struct wl_callback *cb, uint32_t t);
static const struct wl_callback_listener frame_l = { frame_done };
static void fb_sync(void *d, struct wp_presentation_feedback *f, struct wl_output *o) { (void)d; (void)f; (void)o; }
static void fb_presented(void *d, struct wp_presentation_feedback *f, uint32_t sh, uint32_t sl, uint32_t ns, uint32_t refresh,
                         uint32_t sh2, uint32_t sl2, uint32_t flags) {
    (void)d; (void)sh; (void)sl; (void)ns; (void)refresh; (void)sh2; (void)sl2; (void)flags;
    presented++;
    wp_presentation_feedback_destroy(f);
}
static void fb_discarded(void *d, struct wp_presentation_feedback *f) { (void)d; discarded++; wp_presentation_feedback_destroy(f); }
static const struct wp_presentation_feedback_listener fb_l = { fb_sync, fb_presented, fb_discarded };
static Buf fbuf;
static void redraw(void) {
    for (int i = 0; i < 64 * 64; i++) fbuf.px[i] = 0xff000000u | (uint32_t)(frames * 2654435761u);
    struct wl_callback *cb = wl_surface_frame(surf);
    wl_callback_add_listener(cb, &frame_l, NULL);
    if (pres) wp_presentation_feedback_add_listener(wp_presentation_feedback(pres, surf), &fb_l, NULL);
    wl_surface_attach(surf, fbuf.b, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, 64, 64);
    wl_surface_commit(surf);
}
static void frame_done(void *d, struct wl_callback *cb, uint32_t t) {
    (void)d; (void)t;
    wl_callback_destroy(cb);
    frames++;
    if (want_more) redraw();
}

static int run_frames(int secs) {
    fbuf = buffer(320, 240, 0xff336699u);
    window("frames", &fbuf, 0);
    want_more = 1;
    redraw();
    double t0 = now_s();
    int last = 0;
    for (int s = 1; s <= secs; s++) {
        while (now_s() < t0 + s) if (wl_display_dispatch(dpy) < 0) { printf("FAIL connection lost\n"); return 1; }
        printf("second %d: %d frame callbacks, %d presented, %d discarded\n", s, frames - last, presented, discarded);
        last = frames;
    }
    want_more = 0;
    double el = now_s() - t0;
    printf("frames: %d in %.2f s = %.1f per second; presentation feedback: %d presented, %d discarded\n", frames, el, frames / el,
           presented, discarded);
    return wl_display_get_error(dpy) ? 1 : 0;
}

/* ---- damage ---- */
static int run_damage(int per_sec, int secs) {
    Buf b = buffer(640, 480, 0xff202020u);
    window("damage", &b, 0);
    double t0 = now_s(), step = 1.0 / per_sec;
    int n = 0;
    while (now_s() < t0 + secs) {
        int x = 20 + (n * 13) % 600, y = 20 + (n * 7) % 440;
        for (int yy = 0; yy < 10; yy++)
            for (int xx = 0; xx < 10; xx++) b.px[(y + yy) * 640 + x + xx] = 0xff000000u | (uint32_t)(n * 40503u);
        wl_surface_attach(surf, b.b, 0, 0);
        if (n & 1) wl_surface_damage(surf, x, y, 10, 10);
        else wl_surface_damage_buffer(surf, x, y, 10, 10);
        wl_surface_commit(surf);
        n++;
        double next = t0 + n * step;
        while (now_s() < next) { wl_display_flush(dpy); usleep(1000); wl_display_dispatch_pending(dpy); }
        wl_display_roundtrip(dpy);
    }
    /* the same buffer again with no damage at all (a whole copy), then with damage past its edges (clamped) */
    wl_surface_attach(surf, b.b, 0, 0);
    wl_surface_commit(surf);
    wl_display_roundtrip(dpy);
    wl_surface_attach(surf, b.b, 0, 0);
    wl_surface_damage_buffer(surf, 600, 400, 1 << 20, 1 << 20);
    wl_surface_damage(surf, -100000, -100000, 100010, 100010);
    wl_surface_commit(surf);
    wl_display_roundtrip(dpy);
    printf("damage: %d commits of a 10x10 square, one with no damage, one past the edges\n", n);
    return wl_display_get_error(dpy) ? 1 : 0;
}

/* ---- subsurfaces that come and go on their own (desync), as GTK 3's menus in Firefox ---- */
typedef struct { struct wl_surface *s; struct wl_subsurface *sub; Buf b; } Sub;
static Sub sub_new(struct wl_surface *parent, int x, int y, int w, int h, uint32_t color) {
    Sub m = { wl_compositor_create_surface(comp), NULL, buffer(w, h, color) };
    m.sub = wl_subcompositor_get_subsurface(subcomp, m.s, parent);
    wl_subsurface_set_desync(m.sub);
    wl_subsurface_set_position(m.sub, x, y);
    wl_surface_attach(m.s, m.b.b, 0, 0);
    wl_surface_damage_buffer(m.s, 0, 0, w, h);
    wl_surface_commit(m.s);
    return m;
}
static void sub_gone(Sub *m) {
    wl_subsurface_destroy(m->sub);
    wl_surface_destroy(m->s);
    wl_buffer_destroy(m->b.b);
    munmap(m->b.px, (size_t)m->b.w * (size_t)m->b.h * 4);
}
static Buf win_buf;
static int corner, hold_ms = 250;
static void win_poke(void) {                    /* one pixel of the window: a frame is painted and checked */
    win_buf.px[0] = 0xff000000u | (uint32_t)(++corner * 2654435761u);
    wl_surface_attach(surf, win_buf.b, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, 1, 1);
    wl_surface_commit(surf);
    wl_display_roundtrip(dpy);
    pause_ms(hold_ms);
}
static int run_subsurface(void) {
    if (!subcomp) { printf("FAIL no wl_subcompositor\n"); return 1; }
    win_buf = buffer(640, 480, 0xff303030u);
    window("subsurface", &win_buf, 0);
    xdg_surface_set_window_geometry(xsurf, 20, 16, 600, 448);  /* a frame drawn by the app, as GTK's */
    wl_surface_commit(surf);
    pause_ms(1500);                             /* past the window's opening animation, which repaints all of it */
    Sub a = sub_new(surf, 100, 100, 120, 130, 0xffc03030u);
    wl_surface_commit(surf);                    /* the position comes with the window's commit */
    win_poke();
    for (int i = 0; i < 120 * 40; i++) a.b.px[i] = 0xff30c030u;    /* redrawn on its own: only its commit */
    wl_surface_attach(a.s, a.b.b, 0, 0);
    wl_surface_damage_buffer(a.s, 0, 0, 120, 40);
    wl_surface_commit(a.s);
    wl_display_roundtrip(dpy);
    pause_ms(hold_ms);
    win_poke();
    sub_gone(&a);                               /* closed: no commit of the window */
    wl_display_roundtrip(dpy);
    pause_ms(hold_ms);
    win_poke();
    Sub b = sub_new(surf, 380, 220, 150, 100, 0xff3030c0u);       /* a new one elsewhere, its commit first */
    wl_display_roundtrip(dpy);
    pause_ms(hold_ms);
    wl_surface_commit(surf);
    win_poke();
    wl_subsurface_set_position(b.sub, 60, 300);  /* moved with the window's commit */
    wl_surface_commit(surf);
    win_poke();
    Sub c = sub_new(b.s, 10, 10, 60, 40, 0xffc0c030u);            /* one of its own, then the outer one goes first */
    wl_surface_commit(b.s);
    win_poke();
    sub_gone(&b);
    wl_display_roundtrip(dpy);
    pause_ms(hold_ms);
    win_poke();
    wl_surface_attach(c.s, NULL, 0, 0);
    wl_surface_commit(c.s);
    sub_gone(&c);
    win_poke();
    printf("subsurface: shown, redrawn, destroyed, made elsewhere, moved, destroyed under its own subsurface\n");
    return wl_display_get_error(dpy) ? 1 : 0;
}

/* ---- titles ---- */
static int run_titles(void) {
    Buf b = buffer(200, 120, 0xff505050u);
    window("A", &b, 0);
    pause_ms(500);
    for (int i = 0; i < 1000; i++) xdg_toplevel_set_title(top, "A");
    wl_display_roundtrip(dpy);
    pause_ms(500);
    char t[16];
    for (int i = 0; i < 1000; i++) { snprintf(t, sizeof t, "b%d", i); xdg_toplevel_set_title(top, t); }
    wl_display_roundtrip(dpy);
    pause_ms(500);
    xdg_toplevel_set_title(top, "end");
    wl_display_roundtrip(dpy);
    pause_ms(500);
    printf("titles: sent\n");
    return wl_display_get_error(dpy) ? 1 : 0;
}

/* ---- buffer transform ---- */
static int run_transform(void) {
    Buf b = buffer(200, 100, 0xff4080c0u);
    window("transform", &b, WL_OUTPUT_TRANSFORM_90);
    pause_ms(600);
    printf("transform: a 200x100 buffer at 90 degrees\n");
    return wl_display_get_error(dpy) ? 1 : 0;
}

/* ---- protocols ---- */
static int run_protocols(void) {
    check("xdg_wm_base version 7", ver_wm, 7);
    check("zwp_linux_dmabuf_v1 version 5", ver_dmabuf, 5);
    check("wp_presentation version 2", ver_pres, 2);
    check("wp_cursor_shape_manager_v1 version 2", ver_cursor, 2);
    check("wp_single_pixel_buffer_manager_v1", ver_spb, 1);
    check("wp_alpha_modifier_v1", ver_alpha, 1);
    check("no wp_drm_lease_device_v1 without a VR headset on lord-server's card", ver_lease, 0);
    Buf b = buffer(300, 200, 0xff808080u);
    window("protocols", &b, 0);
    check("wm_capabilities: maximize", win.caps_max, 1);
    check("wm_capabilities: minimize", win.caps_min, 1);
    check("wm_capabilities: fullscreen", win.caps_fullscreen, 1);
    check("wm_capabilities: window_menu", win.caps_menu, 1);
    check("configure_bounds sent", win.bounds_w > 0 && win.bounds_h > 0, 1);
    printf("     bounds %dx%d\n", win.bounds_w, win.bounds_h);
    if (spb && alpha) {
        /* a subsurface-free check: a second window whose content is one pixel, half transparent */
        struct wl_surface *s2 = wl_compositor_create_surface(comp);
        struct xdg_surface *x2 = xdg_wm_base_get_xdg_surface(wmbase, s2);
        Win w2 = { 0 };
        xdg_surface_add_listener(x2, &xs_l, &w2);
        struct xdg_toplevel *t2 = xdg_surface_get_toplevel(x2);
        xdg_toplevel_add_listener(t2, &top_l, &w2);
        xdg_toplevel_set_title(t2, "single pixel");
        struct wp_alpha_modifier_surface_v1 *am = wp_alpha_modifier_v1_get_surface(alpha, s2);
        wl_surface_commit(s2);
        wl_display_roundtrip(dpy);
        xdg_surface_ack_configure(x2, w2.serial);
        struct wl_buffer *px = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(spb, 0, 0xffffffffu, 0, 0xffffffffu);
        wp_alpha_modifier_surface_v1_set_multiplier(am, 0x7fffffffu);
        wl_surface_attach(s2, px, 0, 0);
        wl_surface_damage_buffer(s2, 0, 0, 1, 1);
        wl_surface_commit(s2);
        wl_display_roundtrip(dpy);
        pause_ms(300);
        check("a single-pixel buffer at 50 % opacity: no protocol error", wl_display_get_error(dpy), 0);
    }
    int err = wl_display_get_error(dpy);
    if (err) {
        const struct wl_interface *iface = NULL;
        uint32_t id = 0, code = wl_display_get_protocol_error(dpy, &iface, &id);
        printf("     protocol error %u on %s@%u\n", code, iface ? iface->name : "?", id);
    }
    return fails ? 1 : 0;
}

/* ---- idle: a 300 ms notification; how many "idled" came in 3 s (the script answers the prompt first, or not) ---- */
static int idled, resumed;
static void on_idled(void *d, struct ext_idle_notification_v1 *n) { (void)d; (void)n; idled++; }
static void on_resumed(void *d, struct ext_idle_notification_v1 *n) { (void)d; (void)n; resumed++; }
static const struct ext_idle_notification_v1_listener idle_l = { on_idled, on_resumed };
static int run_idle(void) {
    if (!idler || !seat) { printf("FAIL no ext_idle_notifier_v1 or wl_seat\n"); return 1; }
    struct ext_idle_notification_v1 *n = ext_idle_notifier_v1_get_idle_notification(idler, 300, seat);
    ext_idle_notification_v1_add_listener(n, &idle_l, NULL);
    wl_display_roundtrip(dpy);
    pause_ms(3000);
    printf("idle: %d idled, %d resumed\n", idled, resumed);
    ext_idle_notification_v1_destroy(n);
    wl_display_roundtrip(dpy);
    return wl_display_get_error(dpy) ? 1 : 0;
}

/* ---- icon: a window that sets its own icon (a 64 px buffer, a 300 px one that is too large, and a 48 px one), then a
 * second connection whose non-square buffer must be refused with invalid_buffer ---- */
static int icon_sizes[8], nicon_sizes;
static void im_size(void *d, struct xdg_toplevel_icon_manager_v1 *m, int32_t size) { (void)d; (void)m; if (nicon_sizes < 8) icon_sizes[nicon_sizes++] = size; }
static void im_done(void *d, struct xdg_toplevel_icon_manager_v1 *m) { (void)d; (void)m; }
static const struct xdg_toplevel_icon_manager_v1_listener im_l = { im_size, im_done };
static int run_icon(void) {
    if (!iconm) { printf("FAIL no xdg_toplevel_icon_manager_v1\n"); return 1; }
    xdg_toplevel_icon_manager_v1_add_listener(iconm, &im_l, NULL);
    wl_display_roundtrip(dpy);
    printf("icon: sizes asked:");
    for (int i = 0; i < nicon_sizes; i++) printf(" %d", icon_sizes[i]);
    printf("\n");
    Buf b = buffer(200, 120, 0xff505050u);
    window("icon", &b, 0);
    pause_ms(300);
    struct xdg_toplevel_icon_v1 *ic = xdg_toplevel_icon_manager_v1_create_icon(iconm);
    xdg_toplevel_icon_v1_set_name(ic, "firefox");          /* ignored: a name could be another app's icon */
    Buf i64 = buffer(64, 64, 0x80ff0000u), i300 = buffer(300, 300, 0xff00ff00u), i48 = buffer(48, 48, 0xff0000ffu);
    xdg_toplevel_icon_v1_add_buffer(ic, i64.b, 1);
    xdg_toplevel_icon_v1_add_buffer(ic, i300.b, 1);
    xdg_toplevel_icon_v1_add_buffer(ic, i48.b, 1);
    xdg_toplevel_icon_manager_v1_set_icon(iconm, top, ic);
    xdg_toplevel_icon_v1_destroy(ic);                       /* the window keeps it */
    wl_surface_commit(surf);
    wl_display_roundtrip(dpy);
    pause_ms(500);
    check("icon: set, no protocol error", wl_display_get_error(dpy), 0);
    /* a second connection: a buffer that isn't square is invalid_buffer */
    struct wl_display *d2 = wl_display_connect(NULL);
    struct wl_registry *r2 = d2 ? wl_display_get_registry(d2) : NULL;
    if (!r2) { printf("FAIL second connection\n"); return 1; }
    struct wl_display *keep = dpy;
    struct wl_shm *keep_shm = shm;
    struct xdg_toplevel_icon_manager_v1 *keep_im = iconm;
    dpy = d2;
    iconm = NULL;
    wl_registry_add_listener(r2, &reg_l, NULL);
    wl_display_roundtrip(d2);
    Buf bad = buffer(64, 32, 0xffffffffu);
    struct xdg_toplevel_icon_v1 *ic2 = xdg_toplevel_icon_manager_v1_create_icon(iconm);
    xdg_toplevel_icon_v1_add_buffer(ic2, bad.b, 1);
    wl_display_roundtrip(d2);
    const struct wl_interface *iface = NULL;
    uint32_t oid = 0, code = wl_display_get_error(d2) ? wl_display_get_protocol_error(d2, &iface, &oid) : 99;
    check("icon: a 64x32 buffer is invalid_buffer", code == XDG_TOPLEVEL_ICON_V1_ERROR_INVALID_BUFFER && iface == &xdg_toplevel_icon_v1_interface, 1);
    wl_display_disconnect(d2);
    dpy = keep;
    shm = keep_shm;
    iconm = keep_im;
    return fails ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: wlperf-client titles|frames N|damage N [S]|subsurface|transform|protocols|idle|icon\n"); return 2; }
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "wlperf-client: no Wayland display\n"); return 2; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wmbase) { fprintf(stderr, "wlperf-client: no compositor, shm or xdg_wm_base\n"); return 2; }
    xdg_wm_base_add_listener(wmbase, &wm_l, NULL);
    int r;
    if (!strcmp(argv[1], "titles")) r = run_titles();
    else if (!strcmp(argv[1], "frames")) r = run_frames(argc > 2 ? atoi(argv[2]) : 3);
    else if (!strcmp(argv[1], "damage")) r = run_damage(argc > 2 ? atoi(argv[2]) : 2, argc > 3 ? atoi(argv[3]) : 3);
    else if (!strcmp(argv[1], "transform")) r = run_transform();
    else if (!strcmp(argv[1], "subsurface")) { if (argc > 2) hold_ms = atoi(argv[2]); r = run_subsurface(); }
    else if (!strcmp(argv[1], "protocols")) r = run_protocols();
    else if (!strcmp(argv[1], "idle")) r = run_idle();
    else if (!strcmp(argv[1], "icon")) r = run_icon();
    else { fprintf(stderr, "wlperf-client: unknown mode %s\n", argv[1]); return 2; }
    wl_display_disconnect(dpy);
    return r || fails ? 1 : 0;
}
