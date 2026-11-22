/* wlfuzz_client.c — misbehaving clients against the protocol errors lord-server raises (docs/WAYLAND.md,
 * "Conformance"): hundreds of short connections, each sending a random mix of the requests whose misuse is an error
 * (subsurface order against strangers and itself, acks of unknown and repeated serials, destroys in the wrong order,
 * incomplete positioners, popups over popups, viewports outside their buffers, odd scales, size limits, decoration
 * modes, pointer constraint regions, data offers and sources, activation tokens), with random arguments, and objects
 * destroyed at random points. Meanwhile a well-behaved witness keeps a window mapped on its own connection.
 *
 * Passes when lord-server answers every connection (a protocol error is expected, a crash or a hang is not), the
 * witness is never disconnected and still gets frame callbacks at the end, and a new client can still map a window.
 *
 *   WAYLAND_DISPLAY=wayland-0 build/wlfuzz-client [rounds] [seed]   (tests/popup_headless.sh wlfuzz-client; or
 *   WLFUZZ_ROUNDS and WLFUZZ_SEED in the environment) */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"
#include "client/viewporter-client-protocol.h"
#include "client/xdg-decoration-unstable-v1-client-protocol.h"
#include "client/pointer-constraints-unstable-v1-client-protocol.h"
#include "client/xdg-activation-v1-client-protocol.h"

typedef struct {
    struct wl_display *dpy;
    struct wl_compositor *comp;
    struct wl_subcompositor *subc;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct xdg_wm_base *wm;
    struct wp_viewporter *vp;
    struct zxdg_decoration_manager_v1 *deco;
    struct zwp_pointer_constraints_v1 *cons;
    struct wl_data_device_manager *ddm;
    struct xdg_activation_v1 *act;
    uint32_t serial;
    int configured;
} Conn;

static unsigned rnd_state;
static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }
static int pick(int n) { return n > 0 ? (int)(rnd() % (unsigned)n) : 0; }
static int32_t any_int(void) {
    static const int32_t v[] = { 0, 1, -1, 2, 7, 33, 64, 4096, -4096, 0x7fffffff, (int32_t)0x80000000 };
    return pick(3) ? v[pick(sizeof v / sizeof v[0])] : (int32_t)rnd();
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    Conn *c = d;
#define B(i, v) wl_registry_bind(r, name, &i, ver < (v) ? ver : (v))
    if (!strcmp(iface, wl_compositor_interface.name)) c->comp = B(wl_compositor_interface, 6);
    else if (!strcmp(iface, wl_subcompositor_interface.name)) c->subc = B(wl_subcompositor_interface, 1);
    else if (!strcmp(iface, wl_shm_interface.name)) c->shm = B(wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) c->seat = B(wl_seat_interface, 7);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) c->wm = B(xdg_wm_base_interface, 6);
    else if (!strcmp(iface, wp_viewporter_interface.name)) c->vp = B(wp_viewporter_interface, 1);
    else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) c->deco = B(zxdg_decoration_manager_v1_interface, 1);
    else if (!strcmp(iface, zwp_pointer_constraints_v1_interface.name)) c->cons = B(zwp_pointer_constraints_v1_interface, 1);
    else if (!strcmp(iface, wl_data_device_manager_interface.name)) c->ddm = B(wl_data_device_manager_interface, 3);
    else if (!strcmp(iface, xdg_activation_v1_interface.name)) c->act = B(xdg_activation_v1_interface, 1);
#undef B
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };
static void xs_conf(void *d, struct xdg_surface *xs, uint32_t serial) { (void)xs; Conn *c = d; c->serial = serial; c->configured = 1; }
static const struct xdg_surface_listener xs_l = { xs_conf };

static int connect_to(Conn *c) {
    memset(c, 0, sizeof *c);
    if (!(c->dpy = wl_display_connect(NULL))) return -1;
    struct wl_registry *reg = wl_display_get_registry(c->dpy);
    wl_registry_add_listener(reg, &reg_l, c);
    if (wl_display_roundtrip(c->dpy) < 0 || !c->comp || !c->subc || !c->shm || !c->wm) return -1;
    xdg_wm_base_add_listener(c->wm, &wm_l, NULL);
    return 0;
}

static struct wl_buffer *buffer(Conn *c, int W, int H, uint32_t color) {
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    if (W > 512) W = 512;
    if (H > 512) H = 512;
    int stride = W * 4, size = stride * H;
    int fd = memfd_create("wlfuzz", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { perror("memfd"); exit(2); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int i = 0; i < W * H; i++) px[i] = color;
    struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    munmap(px, (size_t)size);
    return b;
}

#define NS 8
/* one round: a connection, a random script, the server must answer (with or without an error) */
static int round_once(void) {
    Conn c;
    if (connect_to(&c)) return -1;
    struct wl_surface *s[NS];
    struct xdg_surface *xs[NS] = { 0 };
    struct xdg_toplevel *top[NS] = { 0 };
    struct xdg_popup *pop[NS] = { 0 };
    struct wl_subsurface *sub[NS] = { 0 };
    for (int i = 0; i < NS; i++) s[i] = wl_compositor_create_surface(c.comp);
    int steps = 4 + pick(40);
    for (int k = 0; k < steps; k++) {
        int a = pick(NS), b = pick(NS);
        switch (pick(24)) {
        case 0: if (!sub[a] && !xs[a]) sub[a] = wl_subcompositor_get_subsurface(c.subc, s[a], s[b]); break;
        case 1: if (sub[a]) wl_subsurface_place_above(sub[a], s[b]); break;
        case 2: if (sub[a]) wl_subsurface_place_below(sub[a], s[b]); break;
        case 3: if (sub[a]) wl_subsurface_set_position(sub[a], any_int(), any_int()); break;
        case 4: if (!xs[a]) { xs[a] = xdg_wm_base_get_xdg_surface(c.wm, s[a]); xdg_surface_add_listener(xs[a], &xs_l, &c); } break;
        case 5: if (xs[a] && !top[a] && !pop[a]) top[a] = xdg_surface_get_toplevel(xs[a]); break;
        case 6: if (xs[a] && !top[a] && !pop[a] && xs[b]) {
                    struct xdg_positioner *p = xdg_wm_base_create_positioner(c.wm);
                    if (pick(4)) xdg_positioner_set_size(p, 1 + pick(300), 1 + pick(300));
                    if (pick(4)) xdg_positioner_set_anchor_rect(p, any_int() % 500, any_int() % 500, pick(50), pick(50));
                    if (pick(2)) xdg_positioner_set_anchor(p, (uint32_t)pick(10));
                    if (pick(2)) xdg_positioner_set_gravity(p, (uint32_t)pick(10));
                    if (pick(2)) xdg_positioner_set_reactive(p);
                    pop[a] = xdg_surface_get_popup(xs[a], xs[b], p);
                    xdg_positioner_destroy(p);
                } break;
        case 7: if (xs[a]) xdg_surface_ack_configure(xs[a], pick(2) ? c.serial : (uint32_t)rnd()); break;
        case 8: if (xs[a]) xdg_surface_set_window_geometry(xs[a], any_int(), any_int(), any_int(), any_int()); break;
        case 9: if (top[a]) { pick(2) ? xdg_toplevel_set_min_size(top[a], any_int(), any_int()) : xdg_toplevel_set_max_size(top[a], any_int(), any_int()); } break;
        case 10: if (top[a]) xdg_toplevel_set_parent(top[a], top[b]); break;
        case 11: if (top[a] && c.seat) xdg_toplevel_resize(top[a], c.seat, c.serial, (uint32_t)pick(16)); break;
        case 12: if (pop[a] && c.seat) xdg_popup_grab(pop[a], c.seat, c.serial); break;
        case 13: wl_surface_attach(s[a], pick(5) ? buffer(&c, 1 + pick(200), 1 + pick(200), 0xff336699u) : NULL, 0, 0); break;
        case 14: wl_surface_set_buffer_scale(s[a], pick(4)); break;
        case 15: wl_surface_commit(s[a]); break;
        case 16: if (c.vp) {
                     struct wp_viewport *v = wp_viewporter_get_viewport(c.vp, s[a]);
                     wp_viewport_set_source(v, wl_fixed_from_double(pick(100) / 3.0), wl_fixed_from_double(pick(100) / 3.0),
                                            wl_fixed_from_double(1 + pick(400) / 3.0), wl_fixed_from_double(1 + pick(400) / 3.0));
                     if (pick(2)) wp_viewport_set_destination(v, 1 + pick(300), 1 + pick(300));
                     wl_surface_commit(s[a]);
                 } break;
        case 17: if (c.deco && top[a]) {
                     struct zxdg_toplevel_decoration_v1 *d = zxdg_decoration_manager_v1_get_toplevel_decoration(c.deco, top[a]);
                     zxdg_toplevel_decoration_v1_set_mode(d, (uint32_t)pick(4));
                 } break;
        case 18: if (c.cons && c.seat) {
                     struct wl_pointer *ptr = wl_seat_get_pointer(c.seat);
                     struct wl_region *r = wl_compositor_create_region(c.comp);
                     wl_region_add(r, any_int() % 300, any_int() % 300, pick(300), pick(300));
                     if (pick(2)) {
                         struct zwp_confined_pointer_v1 *cp = zwp_pointer_constraints_v1_confine_pointer(c.cons, s[a], ptr, pick(2) ? r : NULL, 1 + pick(2));
                         zwp_confined_pointer_v1_set_region(cp, pick(2) ? r : NULL);
                     } else {
                         struct zwp_locked_pointer_v1 *lp = zwp_pointer_constraints_v1_lock_pointer(c.cons, s[a], ptr, pick(2) ? r : NULL, 1 + pick(2));
                         zwp_locked_pointer_v1_set_region(lp, pick(2) ? r : NULL);
                     }
                     wl_region_destroy(r);
                     wl_surface_commit(s[a]);
                 } break;
        case 19: if (c.ddm && c.seat) {
                     struct wl_data_source *src = wl_data_device_manager_create_data_source(c.ddm);
                     wl_data_source_offer(src, "text/plain");
                     if (pick(2)) wl_data_source_set_actions(src, (uint32_t)pick(8));
                     wl_data_device_set_selection(wl_data_device_manager_get_data_device(c.ddm, c.seat), src, c.serial);
                 } break;
        case 20: if (c.act) {
                     struct xdg_activation_token_v1 *t = xdg_activation_v1_get_activation_token(c.act);
                     xdg_activation_token_v1_set_surface(t, s[a]);
                     xdg_activation_token_v1_commit(t);
                     if (pick(2)) xdg_activation_token_v1_set_app_id(t, "again");
                 } break;
        /* destroys in a random order */
        case 21: if (pop[a]) { xdg_popup_destroy(pop[a]); pop[a] = NULL; } else if (top[a]) { xdg_toplevel_destroy(top[a]); top[a] = NULL; } break;
        case 22: if (xs[a]) { xdg_surface_destroy(xs[a]); xs[a] = NULL; } else if (sub[a]) { wl_subsurface_destroy(sub[a]); sub[a] = NULL; } break;
        case 23: if (pick(4) == 0) { xdg_wm_base_destroy(c.wm); c.wm = NULL; goto done; } break;
        }
    }
done:;
    int r = wl_display_roundtrip(c.dpy);
    int err = wl_display_get_error(c.dpy);
    wl_display_disconnect(c.dpy);
    /* answered: fine, or a protocol error (EPROTO); anything else (a reset, no answer) is lord-server's failure */
    return r >= 0 || err == EPROTO ? 0 : -1;
}

static int frames;
static void frame_done(void *d, struct wl_callback *cb, uint32_t t) { (void)d; (void)t; frames++; wl_callback_destroy(cb); }
static const struct wl_callback_listener frame_l = { frame_done };

/* a well-behaved window: mapped, then a frame callback at a time */
static int witness_map(Conn *w, struct wl_surface **ws) {
    if (connect_to(w)) return -1;
    *ws = wl_compositor_create_surface(w->comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(w->wm, *ws);
    xdg_surface_add_listener(xs, &xs_l, w);
    struct xdg_toplevel *t = xdg_surface_get_toplevel(xs);
    xdg_toplevel_set_title(t, "wlfuzz witness");
    wl_surface_commit(*ws);
    while (!w->configured) if (wl_display_dispatch(w->dpy) < 0) return -1;
    xdg_surface_ack_configure(xs, w->serial);
    wl_surface_attach(*ws, buffer(w, 200, 150, 0xff208040u), 0, 0);
    wl_surface_damage(*ws, 0, 0, 200, 150);
    wl_surface_commit(*ws);
    return wl_display_roundtrip(w->dpy) < 0 ? -1 : 0;
}
static int witness_frame(Conn *w, struct wl_surface *ws) {
    int before = frames;
    wl_callback_add_listener(wl_surface_frame(ws), &frame_l, NULL);
    wl_surface_damage(ws, 0, 0, 10, 10);
    wl_surface_commit(ws);
    for (int i = 0; i < 40 && frames == before; i++) {
        if (wl_display_roundtrip(w->dpy) < 0) return -1;
        usleep(50000);
    }
    return frames > before ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *e = getenv("WLFUZZ_ROUNDS"), *es = getenv("WLFUZZ_SEED");
    int rounds = argc > 1 ? atoi(argv[1]) : e ? atoi(e) : 400;
    unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : es ? (unsigned)strtoul(es, NULL, 0) : 0x5eed1234u;
    rnd_state = seed;
    Conn w;
    struct wl_surface *ws;
    if (witness_map(&w, &ws)) { printf("FAIL the witness window didn't map\n"); return 1; }
    int bad = 0;
    for (int i = 0; i < rounds; i++) {
        if (round_once()) { printf("FAIL round %d: lord-server didn't answer\n", i); bad++; break; }
        if (i % 50 == 49 && wl_display_roundtrip(w.dpy) < 0) { printf("FAIL the witness was disconnected at round %d\n", i); return 1; }
    }
    int fr = witness_frame(&w, ws);
    printf("%s the witness is still connected and gets its frame callbacks after %d hostile connections\n", fr ? "FAIL" : "ok  ", rounds);
    Conn n;
    struct wl_surface *ns;
    int again = witness_map(&n, &ns);
    printf("%s a new client maps a window afterwards\n", again ? "FAIL" : "ok  ");
    printf("wlfuzz: %d rounds, seed %#x\n", rounds, seed);
    return bad || fr || again ? 1 : 0;
}
