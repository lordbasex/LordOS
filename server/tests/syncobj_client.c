/* syncobj_client.c — explicit sync (linux-drm-syncobj-v1) against a running lord-server, through the kernel
 * (docs/SYNCOBJ-PLAN.md, "Tests"; src/wl/syncobj.h). No GPU is needed: the timelines are made on any render node with
 * timeline syncobjs and the buffers are udmabufs (linear XRGB8888), so the CPU path is checked the same way as the GPU
 * one (LORD_GPU=1, on a monitor).
 *
 *   syncobj-client [check ...]       checks: wait order freeze limit errors (all of them when none is given)
 *
 *   wait    a frame committed with an acquire point not signalled yet is not applied (no frame callback, no
 *           presentation) until the point is signalled from the CPU; then it is, and the release point of the buffer it
 *           replaced is signalled
 *   order   a commit whose acquire point is already signalled waits behind an earlier one that waits; both are applied,
 *           in order, once the first point is signalled
 *   freeze  a point never signalled freezes only its surface: another window of the same client (implicit sync) keeps
 *           getting frames; destroying the frozen surface signals its waiting frame's release point
 *   limit   a ninth commit waiting on one surface disconnects the client (no_memory)
 *   errors  every protocol error: no_acquire_point, no_release_point, no_buffer, unsupported_buffer, conflicting_points,
 *           surface_exists, no_surface, invalid_timeline
 *
 * Exit 0 all passed, 1 a failure, 77 when something it needs is missing (the global, /dev/udmabuf, a render node with
 * timeline syncobjs). Run by tests/syncobj_headless.sh against a headless lord-server. */
#include <errno.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"
#include "client/linux-dmabuf-v1-client-protocol.h"
#include "client/presentation-time-client-protocol.h"
#include "client/linux-drm-syncobj-v1-client-protocol.h"

#define W 160
#define H 120

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct zwp_linux_dmabuf_v1 *dmabuf;
static struct wp_presentation *pres;
static struct wp_linux_drm_syncobj_manager_v1 *som;
static int rnode = -1, udmabuf = -1, failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed = 1; } } while (0)

/* ---- timelines, on the client's own render node ---- */

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; }

static int open_rnode(void) {
    DIR *d = opendir("/dev/dri");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "renderD", 7)) continue;
        char p[300];
        snprintf(p, sizeof p, "/dev/dri/%s", e->d_name);
        int fd = open(p, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        struct drm_get_cap c = { .capability = DRM_CAP_SYNCOBJ_TIMELINE };
        if (!ioctl(fd, DRM_IOCTL_GET_CAP, &c) && c.value) { closedir(d); return fd; }
        close(fd);
    }
    if (d) closedir(d);
    return -1;
}

typedef struct { uint32_t h; struct wp_linux_drm_syncobj_timeline_v1 *t; } Tl;

static Tl tl_new(void) {
    Tl t = { 0, NULL };
    struct drm_syncobj_create c = { 0 };
    if (ioctl(rnode, DRM_IOCTL_SYNCOBJ_CREATE, &c)) { perror("SYNCOBJ_CREATE"); exit(1); }
    t.h = c.handle;
    struct drm_syncobj_handle a = { .handle = c.handle };
    if (ioctl(rnode, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &a)) { perror("SYNCOBJ_HANDLE_TO_FD"); exit(1); }
    t.t = wp_linux_drm_syncobj_manager_v1_import_timeline(som, a.fd);
    close(a.fd);
    return t;
}

static void tl_signal(Tl t, uint64_t pt) {
    struct drm_syncobj_timeline_array a = { .handles = (uintptr_t)&t.h, .points = (uintptr_t)&pt, .count_handles = 1 };
    if (ioctl(rnode, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &a)) perror("SYNCOBJ_TIMELINE_SIGNAL");
}

/* 1 when the point is signalled within ms (the wait blocks this test client only) */
static int tl_wait(Tl t, uint64_t pt, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t deadline = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec + (int64_t)ms * 1000000LL;
    struct drm_syncobj_timeline_wait w = { .handles = (uintptr_t)&t.h, .points = (uintptr_t)&pt, .timeout_nsec = deadline,
                                           .count_handles = 1, .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT };
    return ioctl(rnode, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &w) == 0;
}

/* ---- the connection ---- */

/* events for ms (or until *until becomes nonzero); -1 once the connection has an error */
static int pump(int ms, const int *until) {
    double end = now() + ms / 1000.0;
    while (!until || !*until) {
        if (wl_display_dispatch_pending(dpy) < 0) return -1;
        if (until && *until) break;
        if (wl_display_flush(dpy) < 0 && errno != EAGAIN) return -1;
        double left = end - now();
        if (left <= 0) break;
        if (wl_display_prepare_read(dpy) != 0) continue;
        struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
        int r = poll(&p, 1, (int)(left * 1000) + 1);
        if (r > 0) { if (wl_display_read_events(dpy) < 0) return -1; }
        else wl_display_cancel_read(dpy);
    }
    return wl_display_get_error(dpy) ? -1 : 0;
}

static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t serial) { (void)d; xdg_wm_base_pong(w, serial); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d; (void)ver;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) { wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(wm, &wm_listener, NULL); }
    else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, 3);
    else if (!strcmp(iface, wp_presentation_interface.name)) pres = wl_registry_bind(r, name, &wp_presentation_interface, 1);
    else if (!strcmp(iface, wp_linux_drm_syncobj_manager_v1_interface.name)) som = wl_registry_bind(r, name, &wp_linux_drm_syncobj_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static int connect_wl(void) {
    comp = NULL; shm = NULL; wm = NULL; dmabuf = NULL; pres = NULL; som = NULL;
    dpy = wl_display_connect(NULL);
    if (!dpy) { printf("no Wayland display\n"); return -1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    return 0;
}

/* ---- buffers: udmabuf (linear XRGB8888) and shared memory ---- */

typedef struct { struct wl_buffer *b; int released; } Buf;
static void buf_release(void *data, struct wl_buffer *b) { (void)b; ((Buf *)data)->released++; }
static const struct wl_buffer_listener buf_listener = { buf_release };

static Buf *dmabuf_new(uint32_t color) {
    size_t stride = W * 4, size = (stride * H + 4095) & ~(size_t)4095;
    int memfd = memfd_create("syncobj-client", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (memfd < 0 || ftruncate(memfd, (off_t)size) || fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK)) { perror("memfd"); exit(1); }
    uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(1); }
    for (int i = 0; i < W * H; i++) px[i] = color;
    munmap(px, size);
    struct udmabuf_create c = { .memfd = (uint32_t)memfd, .flags = UDMABUF_FLAGS_CLOEXEC, .offset = 0, .size = size };
    int fd = ioctl(udmabuf, UDMABUF_CREATE, &c);
    close(memfd);
    if (fd < 0) { perror("UDMABUF_CREATE"); exit(1); }
    Buf *b = calloc(1, sizeof *b);
    struct zwp_linux_buffer_params_v1 *p = zwp_linux_dmabuf_v1_create_params(dmabuf);
    zwp_linux_buffer_params_v1_add(p, fd, 0, 0, (uint32_t)stride, (uint32_t)(DRM_FORMAT_MOD_LINEAR >> 32), (uint32_t)DRM_FORMAT_MOD_LINEAR);
    b->b = zwp_linux_buffer_params_v1_create_immed(p, W, H, DRM_FORMAT_XRGB8888, 0);
    zwp_linux_buffer_params_v1_destroy(p);
    close(fd);
    wl_buffer_add_listener(b->b, &buf_listener, b);
    return b;
}

static Buf *shm_new(void) {
    int fd = memfd_create("syncobj-client-shm", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, W * H * 4)) { perror("memfd"); exit(1); }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, W * H * 4);
    Buf *b = calloc(1, sizeof *b);
    b->b = wl_shm_pool_create_buffer(pool, 0, W, H, W * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b->b, &buf_listener, b);
    return b;
}

/* ---- windows ---- */

typedef struct {
    struct wl_surface *s;
    struct xdg_surface *x;
    struct xdg_toplevel *t;
    struct wp_linux_drm_syncobj_surface_v1 *so;
    int configured;
} Win;

static void xs_configure(void *d, struct xdg_surface *x, uint32_t serial) { xdg_surface_ack_configure(x, serial); ((Win *)d)->configured = 1; }
static const struct xdg_surface_listener xs_listener = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)w; (void)h; (void)st; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener top_listener = { top_configure, top_close, NULL, NULL };

static Win *win_new(const char *title, int sync) {
    Win *w = calloc(1, sizeof *w);
    w->s = wl_compositor_create_surface(comp);
    w->x = xdg_wm_base_get_xdg_surface(wm, w->s);
    xdg_surface_add_listener(w->x, &xs_listener, w);
    w->t = xdg_surface_get_toplevel(w->x);
    xdg_toplevel_add_listener(w->t, &top_listener, w);
    xdg_toplevel_set_title(w->t, title);
    if (sync) w->so = wp_linux_drm_syncobj_manager_v1_get_surface(som, w->s);
    wl_surface_commit(w->s);
    pump(3000, &w->configured);
    return w;
}

/* frame callbacks: each one's own counter, and the order they came in */
typedef struct { int done, seq; } Cb;
static int cb_seq;
static void cb_done(void *data, struct wl_callback *cb, uint32_t t) { (void)t; Cb *c = data; c->done = 1; c->seq = ++cb_seq; wl_callback_destroy(cb); }
static const struct wl_callback_listener cb_listener = { cb_done };
static Cb *frame(Win *w) {
    Cb *c = calloc(1, sizeof *c);
    wl_callback_add_listener(wl_surface_frame(w->s), &cb_listener, c);
    return c;
}

static int presented, discarded;
static void fb_sync(void *d, struct wp_presentation_feedback *f, struct wl_output *o) { (void)d; (void)f; (void)o; }
static void fb_presented(void *d, struct wp_presentation_feedback *f, uint32_t a, uint32_t b, uint32_t c, uint32_t r, uint32_t e, uint32_t g, uint32_t fl) {
    (void)d; (void)a; (void)b; (void)c; (void)r; (void)e; (void)g; (void)fl;
    presented++;
    wp_presentation_feedback_destroy(f);
}
static void fb_discarded(void *d, struct wp_presentation_feedback *f) { (void)d; discarded++; wp_presentation_feedback_destroy(f); }
static const struct wp_presentation_feedback_listener fb_listener = { fb_sync, fb_presented, fb_discarded };
static void feedback(Win *w) {
    if (pres) wp_presentation_feedback_add_listener(wp_presentation_feedback(pres, w->s), &fb_listener, NULL);
}

/* a frame: buffer b with its acquire and release points */
static void commit_sync(Win *w, Buf *b, Tl acq, uint64_t ap, Tl rel, uint64_t rp) {
    wl_surface_attach(w->s, b->b, 0, 0);
    wl_surface_damage_buffer(w->s, 0, 0, W, H);
    wp_linux_drm_syncobj_surface_v1_set_acquire_point(w->so, acq.t, (uint32_t)(ap >> 32), (uint32_t)ap);
    wp_linux_drm_syncobj_surface_v1_set_release_point(w->so, rel.t, (uint32_t)(rp >> 32), (uint32_t)rp);
    wl_surface_commit(w->s);
}

static void win_free(Win *w) {
    if (w->so) wp_linux_drm_syncobj_surface_v1_destroy(w->so);
    xdg_toplevel_destroy(w->t);
    xdg_surface_destroy(w->x);
    wl_surface_destroy(w->s);
    free(w);
}

/* ---- the checks ---- */

static void check_wait(void) {
    Win *w = win_new("syncobj wait", 1);
    Tl a = tl_new(), r1 = tl_new(), r2 = tl_new();
    Buf *b1 = dmabuf_new(0xff3060c0), *b2 = dmabuf_new(0xffc06030);
    /* the first frame, its point signalled before the commit: applied at once */
    tl_signal(a, 1);
    Cb *c0 = frame(w);
    commit_sync(w, b1, a, 1, r1, 1);
    pump(2000, &c0->done);
    CHECK(c0->done, "wait: a frame whose acquire point was already signalled got no frame callback");
    /* copied (the CPU path, or a dmabuf EGL doesn't import): released at once; shown from the buffer itself (the GPU
     * path): held until another one replaces it */
    int copied = tl_wait(r1, 1, 300);
    /* the second, not signalled yet: nothing for 500 ms */
    presented = discarded = 0;
    Cb *c1 = frame(w);
    feedback(w);
    commit_sync(w, b2, a, 2, r2, 1);
    pump(500, &c1->done);
    CHECK(!c1->done, "wait: a frame callback came before the acquire point was signalled");
    CHECK(!presented && !discarded, "wait: presentation feedback before the acquire point was signalled");
    CHECK(copied || !tl_wait(r1, 1, 0), "wait: the shown buffer's release point was signalled while it still showed");
    /* signalled from the CPU: the frame is applied and presented, and the replaced buffer's release point comes */
    double t0 = now();
    tl_signal(a, 2);
    pump(2000, &c1->done);
    CHECK(c1->done, "wait: no frame callback after the acquire point was signalled");
    double dt = (now() - t0) * 1000;
    int ok = 0;
    for (int i = 0; i < 40 && !(ok = tl_wait(r1, 1, 0)); i++) {
        Cb *c = frame(w);
        wl_surface_commit(w->s);
        pump(100, &c->done);
    }
    CHECK(ok, "wait: the replaced buffer's release point was never signalled");
    pump(300, NULL);
    /* presented on a monitor; discarded headless (there is no flip) */
    CHECK(presented + discarded >= 1 || !pres, "wait: no presentation feedback for the frame");
    printf("wait: held until its acquire point, then applied %.0f ms after it was signalled; feedback %d presented, %d discarded; %s\n",
           dt, presented, discarded,
           copied ? "buffers copied (CPU path): each released once copied" : "buffers sampled in place (GPU path): the replaced one released after the next frame");
    win_free(w);
    pump(200, NULL);
    CHECK(tl_wait(r2, 1, 1000), "wait: the last buffer's release point was not signalled when its surface went");
}

static void check_order(void) {
    Win *w = win_new("syncobj order", 1);
    Tl a = tl_new(), b = tl_new(), r1 = tl_new(), r2 = tl_new();
    Buf *b1 = dmabuf_new(0xff20a040), *b2 = dmabuf_new(0xffa02040);
    Cb *c1 = frame(w);
    commit_sync(w, b1, a, 1, r1, 1);           /* waits */
    tl_signal(b, 1);
    Cb *c2 = frame(w);
    commit_sync(w, b2, b, 1, r2, 1);           /* ready, but behind the first */
    pump(500, &c2->done);
    CHECK(!c1->done && !c2->done, "order: a commit was applied while the first one still waited (%d %d)", c1->done, c2->done);
    tl_signal(a, 1);
    pump(2000, &c2->done);
    CHECK(c1->done && c2->done, "order: not both applied once the first point was signalled (%d %d)", c1->done, c2->done);
    CHECK(c1->seq < c2->seq, "order: applied out of order (%d, %d)", c1->seq, c2->seq);
    int ok = 0;
    for (int i = 0; i < 40 && !(ok = tl_wait(r1, 1, 0)); i++) {
        Cb *c = frame(w);
        wl_surface_commit(w->s);
        pump(100, &c->done);
    }
    CHECK(ok, "order: the first buffer (replaced by the second) was never released");
    printf("order: the ready commit waited behind the waiting one; both applied in order\n");
    win_free(w);
    pump(200, NULL);
}

static void check_freeze(void) {
    Win *f = win_new("syncobj frozen", 1), *o = win_new("implicit sync", 0);
    Tl a = tl_new(), r1 = tl_new(), r2 = tl_new();
    Buf *b1 = dmabuf_new(0xff808080), *b2 = dmabuf_new(0xff404040), *ob = dmabuf_new(0xff00ff00);
    tl_signal(a, 1);
    Cb *c0 = frame(f);
    commit_sync(f, b1, a, 1, r1, 1);
    pump(2000, &c0->done);
    Cb *cf = frame(f);
    commit_sync(f, b2, a, 5, r2, 1);           /* never signalled */
    /* the other window draws on its frame callbacks for 1.5 s */
    int frames = 0;
    double end = now() + 1.5;
    while (now() < end) {
        Cb *c = frame(o);
        wl_surface_attach(o->s, ob->b, 0, 0);
        wl_surface_damage_buffer(o->s, 0, 0, W, H);
        wl_surface_commit(o->s);
        if (pump(1000, &c->done) < 0) break;
        if (c->done) frames++;
    }
    CHECK(!cf->done, "freeze: the frozen surface's frame was applied");
    CHECK(frames >= 30, "freeze: the other window got only %d frames in 1.5 s", frames);
    CHECK(!tl_wait(r2, 1, 0), "freeze: a waiting frame's release point was signalled");
    /* the frozen surface goes: its waiting frame's release point and its shown buffer's are signalled */
    win_free(f);
    pump(300, NULL);
    CHECK(tl_wait(r2, 1, 1000), "freeze: the waiting frame's release point was not signalled when its surface went");
    CHECK(tl_wait(r1, 1, 1000), "freeze: the shown buffer's release point was not signalled when its surface went");
    printf("freeze: a point never signalled froze only its surface; the other window got %d frames in 1.5 s; destroying it released both points\n", frames);
    win_free(o);
    pump(200, NULL);
}

static void check_limit(void) {
    connect_wl();
    Win *w = win_new("syncobj limit", 1);
    Tl a = tl_new(), r = tl_new();
    Buf *b = dmabuf_new(0xff000080);
    for (int i = 0; i < 9; i++) commit_sync(w, b, a, (uint64_t)i + 1, r, (uint64_t)i + 100);
    pump(1000, NULL);
    int err = wl_display_get_error(dpy);
    CHECK(err, "limit: nine waiting commits on one surface did not disconnect the client");
    printf("limit: nine waiting commits on one surface: disconnected (%s)\n", strerror(err));
    wl_display_disconnect(dpy);
}

/* each error on a connection of its own: the error's interface and code */
static int expect_error(const char *what, const struct wl_interface *iface, uint32_t code) {
    pump(1000, NULL);
    const struct wl_interface *got = NULL;
    uint32_t id = 0, c = wl_display_get_error(dpy) == EPROTO ? wl_display_get_protocol_error(dpy, &got, &id) : 0xffffffffu;
    int ok = got == iface && c == code;
    CHECK(ok, "errors: %s: got %s %u", what, got ? got->name : "no error", c);
    if (ok) printf("errors: %s: %s error %u\n", what, iface->name, code);
    wl_display_disconnect(dpy);
    return ok;
}

static void check_errors(void) {
    const struct wl_interface *si = &wp_linux_drm_syncobj_surface_v1_interface, *mi = &wp_linux_drm_syncobj_manager_v1_interface;
    Win *w;
    Tl a, r;
    Buf *b;

    connect_wl(); w = win_new("e", 1); a = tl_new(); r = tl_new(); b = dmabuf_new(0);
    wl_surface_attach(w->s, b->b, 0, 0);
    wp_linux_drm_syncobj_surface_v1_set_release_point(w->so, r.t, 0, 1);
    wl_surface_commit(w->s);
    expect_error("a buffer with no acquire point", si, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_ACQUIRE_POINT);

    connect_wl(); w = win_new("e", 1); a = tl_new(); b = dmabuf_new(0);
    wl_surface_attach(w->s, b->b, 0, 0);
    wp_linux_drm_syncobj_surface_v1_set_acquire_point(w->so, a.t, 0, 1);
    wl_surface_commit(w->s);
    expect_error("a buffer with no release point", si, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_RELEASE_POINT);

    connect_wl(); w = win_new("e", 1); a = tl_new(); r = tl_new();
    wp_linux_drm_syncobj_surface_v1_set_acquire_point(w->so, a.t, 0, 1);
    wp_linux_drm_syncobj_surface_v1_set_release_point(w->so, r.t, 0, 1);
    wl_surface_commit(w->s);
    expect_error("points with no buffer", si, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER);

    connect_wl(); w = win_new("e", 1); a = tl_new(); r = tl_new(); b = dmabuf_new(0);
    wl_surface_attach(w->s, NULL, 0, 0);
    wp_linux_drm_syncobj_surface_v1_set_acquire_point(w->so, a.t, 0, 1);
    wp_linux_drm_syncobj_surface_v1_set_release_point(w->so, r.t, 0, 1);
    wl_surface_commit(w->s);
    expect_error("points with a null buffer", si, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER);

    connect_wl(); w = win_new("e", 1); a = tl_new(); r = tl_new(); b = shm_new();
    commit_sync(w, b, a, 1, r, 1);
    expect_error("a shared-memory buffer", si, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_UNSUPPORTED_BUFFER);

    connect_wl(); w = win_new("e", 1); a = tl_new(); b = dmabuf_new(0);
    commit_sync(w, b, a, 5, a, 5);
    expect_error("acquire = release on one timeline", si, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS);

    connect_wl(); w = win_new("e", 1); a = tl_new(); b = dmabuf_new(0);
    commit_sync(w, b, a, 7, a, 3);
    expect_error("acquire above release on one timeline", si, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS);

    connect_wl(); w = win_new("e", 1);
    wp_linux_drm_syncobj_manager_v1_get_surface(som, w->s);
    expect_error("a second syncobj object for a surface", mi, WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS);

    connect_wl(); w = win_new("e", 1); a = tl_new();
    xdg_toplevel_destroy(w->t);
    xdg_surface_destroy(w->x);
    wl_surface_destroy(w->s);
    wp_linux_drm_syncobj_surface_v1_set_acquire_point(w->so, a.t, 0, 1);
    expect_error("a point after the wl_surface is gone", si, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE);

    connect_wl();
    int fd = memfd_create("not-a-syncobj", MFD_CLOEXEC);
    wp_linux_drm_syncobj_manager_v1_import_timeline(som, fd);
    close(fd);
    expect_error("a timeline fd that is not a syncobj", mi, WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE);

    /* a surface whose syncobj object was destroyed goes back to implicit sync: a plain commit is fine */
    connect_wl(); w = win_new("e", 1); b = dmabuf_new(0);
    wp_linux_drm_syncobj_surface_v1_destroy(w->so);
    w->so = NULL;
    Cb *c = frame(w);
    wl_surface_attach(w->s, b->b, 0, 0);
    wl_surface_commit(w->s);
    pump(2000, &c->done);
    CHECK(!wl_display_get_error(dpy) && c->done, "errors: an implicit commit after the syncobj object went failed");
    if (c->done) printf("errors: after its syncobj object is destroyed a surface commits with implicit sync again\n");
    wl_display_disconnect(dpy);
}

int main(int argc, char **argv) {
    rnode = open_rnode();
    if (rnode < 0) { printf("skipped: needs a render node with syncobj timelines\n"); return 77; }
    udmabuf = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (udmabuf < 0) { printf("skipped: needs /dev/udmabuf (%s)\n", strerror(errno)); return 77; }
    if (connect_wl()) return 1;
    if (!som) { printf("skipped: the compositor offers no wp_linux_drm_syncobj_manager_v1\n"); return 77; }
    if (!dmabuf || !wm || !comp || !shm) { printf("FAIL: missing globals\n"); return 1; }
    int all = argc < 2;
    for (int i = 1; i < argc || all; i++) {
        const char *c = all ? NULL : argv[i];
        if (all || !strcmp(c, "wait")) check_wait();
        if (all || !strcmp(c, "order")) check_order();
        if (all || !strcmp(c, "freeze")) check_freeze();
        if (wl_display_get_error(dpy)) { printf("FAIL the connection broke: %s\n", strerror(wl_display_get_error(dpy))); return 1; }
        if (all || !strcmp(c, "limit")) check_limit();
        if (all || !strcmp(c, "errors")) check_errors();
        if (all) break;
    }
    printf(failed ? "syncobj: FAILED\n" : "syncobj: all passed\n");
    return failed;
}
