/* dmabuf_client.c — app Wayland de prueba que muestra búferes dmabuf (zwp_linux_dmabuf_v1).
 *
 * Los dmabuf salen de /dev/udmabuf (memoria común con forma de dmabuf, lineal XRGB8888): así se
 * prueba el camino de los búferes de GPU de axia-server sin una GPU real (importación EGL,
 * scanout directo a pantalla completa y, en el camino por CPU, el mapeo lineal).
 *
 *   dmabuf-client [--fullscreen] [--frames N] [--size AxB]
 *
 * Dibuja franjas que se mueven, espera el frame callback y al final cuenta cuadros, FPS y
 * cuántos se presentaron (wp_presentation). */

#include <errno.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <drm/drm_fourcc.h>
#include "client/xdg-shell-client-protocol.h"
#include "client/linux-dmabuf-v1-client-protocol.h"
#include "client/presentation-time-client-protocol.h"
#include "client/relative-pointer-unstable-v1-client-protocol.h"
#include "client/pointer-constraints-unstable-v1-client-protocol.h"

typedef struct { struct wl_buffer *b; int fd, memfd, busy; uint32_t *px; size_t size; } Buf;

static struct wl_compositor *comp;
static struct xdg_wm_base *wm;
static struct zwp_linux_dmabuf_v1 *dmabuf;
static struct wp_presentation *pres;
static struct wl_surface *surf;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *top;
static Buf bufs[2];
static int W = 640, H = 400, cw, ch, configured, closed, want_fs, frames, max_frames = 300, presented, discarded, pending_cb;
static int udmabuf_fd = -1;
/* --lock: puntero bloqueado, cursor oculto y movimiento relativo (como un juego) */
static int want_lock, locked, rel_events, abs_events;
static double rel_x, rel_y;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct zwp_relative_pointer_manager_v1 *relm;
static struct zwp_pointer_constraints_v1 *consm;

static void lk_locked(void *d, struct zwp_locked_pointer_v1 *l) { (void)d; (void)l; locked++; }
static void lk_unlocked(void *d, struct zwp_locked_pointer_v1 *l) { (void)d; (void)l; }
static const struct zwp_locked_pointer_v1_listener lk_listener = { lk_locked, lk_unlocked };
static void rel_motion(void *d, struct zwp_relative_pointer_v1 *r, uint32_t hi, uint32_t lo, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t ux, wl_fixed_t uy) {
    (void)d; (void)r; (void)hi; (void)lo; (void)ux; (void)uy;
    rel_events++;
    rel_x += wl_fixed_to_double(dx);
    rel_y += wl_fixed_to_double(dy);
}
static const struct zwp_relative_pointer_v1_listener rel_listener = { rel_motion };
static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)s; (void)x; (void)y;
    if (want_lock) wl_pointer_set_cursor(p, serial, NULL, 0, 0);      /* oculto, como un juego */
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) { (void)d; (void)p; (void)serial; (void)s; }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)t; (void)x; (void)y; abs_events++; }
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t st) { (void)d; (void)p; (void)serial; (void)t; (void)b; (void)st; }
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)p; (void)t; (void)a; (void)v; }
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_src(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_disc(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void p_v120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener p_listener = { p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_src, p_stop, p_disc, p_v120, NULL };

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void buf_release(void *data, struct wl_buffer *b) { (void)b; ((Buf *)data)->busy = 0; }
static const struct wl_buffer_listener buf_listener = { buf_release };

static void free_buf(Buf *b) {
    if (b->b) wl_buffer_destroy(b->b);
    if (b->px) munmap(b->px, b->size);
    if (b->fd >= 0) close(b->fd);
    if (b->memfd >= 0) close(b->memfd);
    memset(b, 0, sizeof *b);
    b->fd = b->memfd = -1;
}

static int make_buf(Buf *b, int w, int h) {
    size_t stride = (size_t)w * 4, size = (stride * h + 4095) & ~(size_t)4095;
    b->memfd = memfd_create("dmabuf-client", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (b->memfd < 0 || ftruncate(b->memfd, (off_t)size) || fcntl(b->memfd, F_ADD_SEALS, F_SEAL_SHRINK)) { perror("memfd"); return -1; }
    struct udmabuf_create c = { .memfd = (uint32_t)b->memfd, .flags = UDMABUF_FLAGS_CLOEXEC, .offset = 0, .size = size };
    b->fd = ioctl(udmabuf_fd, UDMABUF_CREATE, &c);
    if (b->fd < 0) { perror("UDMABUF_CREATE"); return -1; }
    b->px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, b->memfd, 0);
    if (b->px == MAP_FAILED) { b->px = NULL; return -1; }
    b->size = size;
    struct zwp_linux_buffer_params_v1 *p = zwp_linux_dmabuf_v1_create_params(dmabuf);
    zwp_linux_buffer_params_v1_add(p, b->fd, 0, 0, (uint32_t)stride, (uint32_t)(DRM_FORMAT_MOD_LINEAR >> 32), (uint32_t)DRM_FORMAT_MOD_LINEAR);
    b->b = zwp_linux_buffer_params_v1_create_immed(p, w, h, DRM_FORMAT_XRGB8888, 0);
    zwp_linux_buffer_params_v1_destroy(p);
    wl_buffer_add_listener(b->b, &buf_listener, b);
    return 0;
}

static void draw(Buf *b, int w, int h, int t) {
    for (int y = 0; y < h; y++) {
        uint32_t *row = b->px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int band = ((x + t * 4) / 40) & 1;
            uint32_t r = (uint32_t)(x * 255 / (w ? w : 1)), g = (uint32_t)(y * 255 / (h ? h : 1));
            row[x] = 0xff000000u | (band ? r << 16 | g << 8 | 0x40 : 0x202840);
        }
    }
    /* un cuadrado que rebota */
    int sx = (t * 7) % (w > 80 ? w - 80 : 1), sy = (t * 5) % (h > 80 ? h - 80 : 1);
    for (int y = sy; y < sy + 80 && y < h; y++)
        for (int x = sx; x < sx + 80 && x < w; x++) b->px[(size_t)y * w + x] = 0xffffd60a;
}

/* wp_presentation */
static void fb_sync(void *d, struct wp_presentation_feedback *f, struct wl_output *o) { (void)d; (void)f; (void)o; }
static void fb_presented(void *d, struct wp_presentation_feedback *f, uint32_t a, uint32_t b, uint32_t c, uint32_t r, uint32_t e, uint32_t g, uint32_t fl) {
    (void)d; (void)a; (void)b; (void)c; (void)r; (void)e; (void)g; (void)fl;
    presented++;
    wp_presentation_feedback_destroy(f);
}
static void fb_discarded(void *d, struct wp_presentation_feedback *f) { (void)d; discarded++; wp_presentation_feedback_destroy(f); }
static const struct wp_presentation_feedback_listener fb_listener = { fb_sync, fb_presented, fb_discarded };

static void frame_done(void *data, struct wl_callback *cb, uint32_t t);
static const struct wl_callback_listener frame_listener = { frame_done };

static void submit(void) {
    Buf *b = !bufs[0].busy ? &bufs[0] : !bufs[1].busy ? &bufs[1] : NULL;
    if (!b) return;
    draw(b, cw, ch, frames);
    b->busy = 1;
    wl_surface_attach(surf, b->b, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, cw, ch);
    struct wl_callback *cb = wl_surface_frame(surf);
    wl_callback_add_listener(cb, &frame_listener, NULL);
    if (pres) {
        struct wp_presentation_feedback *f = wp_presentation_feedback(pres, surf);
        wp_presentation_feedback_add_listener(f, &fb_listener, NULL);
    }
    pending_cb = 1;
    wl_surface_commit(surf);
    frames++;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t t) {
    (void)data; (void)t;
    wl_callback_destroy(cb);
    pending_cb = 0;
    if (frames < max_frames) submit();
}

static void resize(int w, int h) {
    for (int i = 0; i < 2; i++) free_buf(&bufs[i]);
    cw = w;
    ch = h;
    for (int i = 0; i < 2; i++) if (make_buf(&bufs[i], w, h)) exit(1);
}

static void xs_configure(void *d, struct xdg_surface *x, uint32_t serial) {
    (void)d;
    xdg_surface_ack_configure(x, serial);
    if (!configured || cw != W || ch != H) resize(W, H);
    if (!configured) { configured = 1; submit(); }
}
static const struct xdg_surface_listener xs_listener = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
    (void)d; (void)t; (void)st;
    if (w > 0 && h > 0) { W = w; H = h; }
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; closed = 1; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_listener = { top_configure, top_close, top_bounds, top_caps };
static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t serial) { (void)d; xdg_wm_base_pong(w, serial); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) { wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(wm, &wm_listener, NULL); }
    else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, ver < 3 ? ver : 3);
    else if (!strcmp(iface, wp_presentation_interface.name)) pres = wl_registry_bind(r, name, &wp_presentation_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
    else if (!strcmp(iface, zwp_relative_pointer_manager_v1_interface.name)) relm = wl_registry_bind(r, name, &zwp_relative_pointer_manager_v1_interface, 1);
    else if (!strcmp(iface, zwp_pointer_constraints_v1_interface.name)) consm = wl_registry_bind(r, name, &zwp_pointer_constraints_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fullscreen")) want_fs = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lock")) want_lock = 1;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
    }
    bufs[0].fd = bufs[0].memfd = bufs[1].fd = bufs[1].memfd = -1;
    udmabuf_fd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (udmabuf_fd < 0) { perror("/dev/udmabuf"); return 1; }
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "sin Wayland\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !wm || !dmabuf) { fprintf(stderr, "faltan globales (dmabuf %s)\n", dmabuf ? "sí" : "no"); return 1; }
    surf = wl_compositor_create_surface(comp);
    xsurf = xdg_wm_base_get_xdg_surface(wm, surf);
    xdg_surface_add_listener(xsurf, &xs_listener, NULL);
    top = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(top, &top_listener, NULL);
    xdg_toplevel_set_title(top, "dmabuf de prueba");
    xdg_toplevel_set_app_id(top, "lordos.dmabuf-client");
    if (want_fs) xdg_toplevel_set_fullscreen(top, NULL);
    if (want_lock && seat && relm && consm) {
        pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pointer, &p_listener, NULL);
        struct zwp_relative_pointer_v1 *rp = zwp_relative_pointer_manager_v1_get_relative_pointer(relm, pointer);
        zwp_relative_pointer_v1_add_listener(rp, &rel_listener, NULL);
        struct zwp_locked_pointer_v1 *lk = zwp_pointer_constraints_v1_lock_pointer(consm, surf, pointer, NULL,
                                                                                   ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        zwp_locked_pointer_v1_add_listener(lk, &lk_listener, NULL);
    } else if (want_lock) fprintf(stderr, "faltan relative-pointer o pointer-constraints\n");
    wl_surface_commit(surf);
    double t0 = now();
    while (!closed && (frames < max_frames || pending_cb) && wl_display_dispatch(dpy) >= 0) {}
    double dt = now() - t0;
    wl_display_roundtrip(dpy);
    printf("dmabuf-client: %d cuadros en %.2f s = %.1f FPS, %dx%d, presentados %d, descartados %d\n",
           frames, dt, frames / dt, cw, ch, presented, discarded);
    if (want_lock) printf("puntero: bloqueado %d vez/veces, %d movimientos relativos (%.0f, %.0f), %d absolutos\n",
                          locked, rel_events, rel_x, rel_y, abs_events);
    return 0;
}
