/* checks.c — lordos-testbench's protocol checklist: every global the compositor advertises that a
 * desktop app relies on, bound at the advertised version and exercised (create, use, see the event
 * that must come back, destroy). A protocol error ends the connection; the check that was running is
 * then the one named as failed, with the compositor's error. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "tb.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "client/xdg-shell-client-protocol.h"
#include "client/linux-dmabuf-v1-client-protocol.h"
#include "client/viewporter-client-protocol.h"
#include "client/fractional-scale-v1-client-protocol.h"
#include "client/xdg-output-unstable-v1-client-protocol.h"
#include "client/cursor-shape-v1-client-protocol.h"
#include "client/relative-pointer-unstable-v1-client-protocol.h"
#include "client/pointer-constraints-unstable-v1-client-protocol.h"
#include "client/pointer-gestures-unstable-v1-client-protocol.h"
#include "client/primary-selection-unstable-v1-client-protocol.h"
#include "client/idle-inhibit-unstable-v1-client-protocol.h"

Check checks[48];
int nchecks, checks_done;
uint32_t out_refresh_mhz;

static Check *cur;                       /* the check running now */
static Check *begin(const char *id) {
    if (nchecks >= (int)(sizeof checks / sizeof checks[0])) nchecks--;
    cur = &checks[nchecks++];
    memset(cur, 0, sizeof *cur);
    snprintf(cur->id, sizeof cur->id, "%s", id);
    return cur;
}
static void set(Check *c, int st, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set(Check *c, int st, const char *fmt, ...) {
    c->state = st;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->detail, sizeof c->detail, fmt, ap);
    va_end(ap);
}

static double ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int tb_wait(volatile int *flag, int ms) {
    double end = ms_now() + ms;
    while (!(flag && *flag)) {
        if (wl_display_dispatch_pending(dpy) < 0) return -1;
        if (flag && *flag) break;
        double left = end - ms_now();
        if (left <= 0) break;
        if (wl_display_flush(dpy) < 0 && errno != EAGAIN) return -1;
        struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
        if (wl_display_prepare_read(dpy) != 0) continue;
        int r = poll(&p, 1, (int)left);
        if (r > 0) { if (wl_display_read_events(dpy) < 0) return -1; }
        else wl_display_cancel_read(dpy);
    }
    return wl_display_get_error(dpy) ? -1 : 0;
}

static Global *find(const char *iface) {
    for (int i = 0; i < nglobals; i++) if (!strcmp(globals[i].iface, iface)) return &globals[i];
    return NULL;
}
static void *bind(const char *iface, const struct wl_interface *in, uint32_t want) {
    Global *g = find(iface);
    if (!g) return NULL;
    return wl_registry_bind(registry, g->name, in, g->version < want ? g->version : want);
}

struct wl_buffer *tb_solid_buffer(int w, int h, uint32_t argb) {
    size_t size = (size_t)w * h * 4;
    int fd = memfd_create("tb-solid", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size)) { if (fd >= 0) close(fd); return NULL; }
    uint32_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return NULL; }
    for (size_t i = 0; i < size / 4; i++) p[i] = argb;
    munmap(p, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

/* ---- wl_shm ---- */
static int shm_argb, shm_xrgb, shm_n;
static void shm_format(void *d, struct wl_shm *s, uint32_t f) { (void)d; (void)s; shm_n++; if (f == WL_SHM_FORMAT_ARGB8888) shm_argb = 1; if (f == WL_SHM_FORMAT_XRGB8888) shm_xrgb = 1; }
static const struct wl_shm_listener shm_l = { shm_format };

/* ---- outputs ---- */
typedef struct { char make[48], model[48], name[32]; int w, h, scale, lw, lh, lx, ly, done; uint32_t refresh; } Out;
static Out outs[8];
static int nouts;
static void o_geo(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp, const char *make, const char *model, int32_t tf) {
    (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sp; (void)tf;
    Out *u = d;
    snprintf(u->make, sizeof u->make, "%s", make);
    snprintf(u->model, sizeof u->model, "%s", model);
}
static void o_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
    (void)o;
    Out *u = d;
    if (flags & WL_OUTPUT_MODE_CURRENT) { u->w = w; u->h = h; u->refresh = (uint32_t)refresh; }
}
static void o_done(void *d, struct wl_output *o) { (void)o; ((Out *)d)->done = 1; }
static void o_scale(void *d, struct wl_output *o, int32_t f) { (void)o; ((Out *)d)->scale = f; }
static void o_name(void *d, struct wl_output *o, const char *n) { (void)o; snprintf(((Out *)d)->name, sizeof ((Out *)d)->name, "%s", n); }
static void o_desc(void *d, struct wl_output *o, const char *n) { (void)d; (void)o; (void)n; }
static const struct wl_output_listener out_l = { o_geo, o_mode, o_done, o_scale, o_name, o_desc };
static void xo_pos(void *d, struct zxdg_output_v1 *o, int32_t x, int32_t y) { (void)o; ((Out *)d)->lx = x; ((Out *)d)->ly = y; }
static void xo_size(void *d, struct zxdg_output_v1 *o, int32_t w, int32_t h) { (void)o; ((Out *)d)->lw = w; ((Out *)d)->lh = h; }
static void xo_done(void *d, struct zxdg_output_v1 *o) { (void)d; (void)o; }
static void xo_name(void *d, struct zxdg_output_v1 *o, const char *n) { (void)d; (void)o; (void)n; }
static void xo_desc(void *d, struct zxdg_output_v1 *o, const char *n) { (void)d; (void)o; (void)n; }
static const struct zxdg_output_v1_listener xo_l = { xo_pos, xo_size, xo_done, xo_name, xo_desc };

/* ---- fractional scale ---- */
static volatile int frac_got;
static uint32_t frac_val;
static void frac_pref(void *d, struct wp_fractional_scale_v1 *f, uint32_t s) { (void)d; (void)f; frac_val = s; frac_got = 1; }
static const struct wp_fractional_scale_v1_listener frac_l = { frac_pref };

/* ---- frame callbacks of test surfaces ---- */
static void cb_done(void *d, struct wl_callback *cb, uint32_t t) { (void)t; *(volatile int *)d = 1; wl_callback_destroy(cb); }
static const struct wl_callback_listener cb_l = { cb_done };

/* ---- popup ---- */
static volatile int pop_conf, pop_xconf;
static int pop_w, pop_h;
static void pop_configure(void *d, struct xdg_popup *p, int32_t x, int32_t y, int32_t w, int32_t h) { (void)d; (void)p; (void)x; (void)y; pop_w = w; pop_h = h; pop_conf = 1; }
static void pop_done(void *d, struct xdg_popup *p) { (void)d; (void)p; }
static void pop_repos(void *d, struct xdg_popup *p, uint32_t t) { (void)d; (void)p; (void)t; }
static const struct xdg_popup_listener pop_l = { pop_configure, pop_done, pop_repos };
static void pxs_configure(void *d, struct xdg_surface *x, uint32_t serial) { (void)d; xdg_surface_ack_configure(x, serial); pop_xconf = 1; }
static const struct xdg_surface_listener pxs_l = { pxs_configure };

/* ---- dmabuf feedback ---- */
static volatile int fb_done_flag;
static int fb_tranches, fb_formats_total;
static size_t fb_table_size;
static char fb_device[64];
static int fb_scanout;
static void fb_done(void *d, struct zwp_linux_dmabuf_feedback_v1 *f) { (void)d; (void)f; fb_done_flag = 1; }
static void fb_table(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, int32_t fd, uint32_t size) { (void)d; (void)f; fb_table_size = size; close(fd); }
static void dev_name(struct wl_array *a, char *out, size_t n) {
    dev_t dev;
    if (a->size != sizeof dev) { snprintf(out, n, "?"); return; }
    memcpy(&dev, a->data, sizeof dev);
    if (!dev) { snprintf(out, n, "0:0 (NONE)"); return; }
    char link[96], path[128] = "";
    snprintf(link, sizeof link, "/sys/dev/char/%u:%u", major(dev), minor(dev));
    ssize_t k = readlink(link, path, sizeof path - 1);
    path[k > 0 ? k : 0] = 0;
    const char *b = strrchr(path, '/');
    if (b) snprintf(out, n, "%.40s", b + 1);
    else snprintf(out, n, "%u:%u", major(dev), minor(dev));
}
static void fb_main(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, struct wl_array *dev) { (void)d; (void)f; dev_name(dev, fb_device, sizeof fb_device); }
static void fb_tdone(void *d, struct zwp_linux_dmabuf_feedback_v1 *f) { (void)d; (void)f; fb_tranches++; }
static void fb_tdev(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, struct wl_array *dev) { (void)d; (void)f; (void)dev; }
static void fb_tfmt(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, struct wl_array *idx) { (void)d; (void)f; fb_formats_total += (int)(idx->size / sizeof(uint16_t)); }
static void fb_tflags(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, uint32_t fl) { (void)d; (void)f; if (fl & ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT) fb_scanout = 1; }
static const struct zwp_linux_dmabuf_feedback_v1_listener fb_l = { fb_done, fb_table, fb_main, fb_tdone, fb_tdev, fb_tfmt, fb_tflags };
static int dm_formats;
static void dm_format(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t f) { (void)d; (void)z; (void)f; dm_formats++; }
static void dm_mod(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t f, uint32_t hi, uint32_t lo) { (void)d; (void)z; (void)f; (void)hi; (void)lo; dm_formats++; }
static const struct zwp_linux_dmabuf_v1_listener dm_l = { dm_format, dm_mod };

/* ---- clipboard (data device) and primary selection ---- */
#define CLIP_MIME "text/plain;charset=utf-8"
static char clip_token[48];
static volatile int clip_sent, clip_offer_ready;
static struct wl_data_offer *clip_offer;
static void ds_target(void *d, struct wl_data_source *s, const char *m) { (void)d; (void)s; (void)m; }
static void ds_send(void *d, struct wl_data_source *s, const char *mime, int32_t fd) {
    (void)d; (void)s; (void)mime;
    if (write(fd, clip_token, strlen(clip_token)) < 0) { /* the reader sees a short read */ }
    close(fd);
    clip_sent = 1;
}
static void ds_cancel(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void ds_dnd1(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void ds_dnd2(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void ds_action(void *d, struct wl_data_source *s, uint32_t a) { (void)d; (void)s; (void)a; }
static const struct wl_data_source_listener ds_l = { ds_target, ds_send, ds_cancel, ds_dnd1, ds_dnd2, ds_action };
static void do_offer(void *d, struct wl_data_offer *o, const char *m) { (void)d; (void)o; (void)m; }
static void do_actions(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static void do_action(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static const struct wl_data_offer_listener do_l = { do_offer, do_actions, do_action };
static void dd_offer(void *d, struct wl_data_device *dd, struct wl_data_offer *o) { (void)d; (void)dd; wl_data_offer_add_listener(o, &do_l, NULL); }
static void dd_enter(void *d, struct wl_data_device *dd, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o) { (void)d; (void)dd; (void)s; (void)sf; (void)x; (void)y; (void)o; }
static void dd_leave(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)dd; (void)t; (void)x; (void)y; }
static void dd_drop(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static void dd_sel(void *d, struct wl_data_device *dd, struct wl_data_offer *o) { (void)d; (void)dd; clip_offer = o; if (o) clip_offer_ready = 1; }
static const struct wl_data_device_listener dd_l = { dd_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_sel };

static struct zwp_primary_selection_offer_v1 *prim_offer;
static volatile int prim_sent, prim_ready;
static void ps_send(void *d, struct zwp_primary_selection_source_v1 *s, const char *m, int32_t fd) {
    (void)d; (void)s; (void)m;
    if (write(fd, clip_token, strlen(clip_token)) < 0) { /* short read */ }
    close(fd);
    prim_sent = 1;
}
static void ps_cancel(void *d, struct zwp_primary_selection_source_v1 *s) { (void)d; (void)s; }
static const struct zwp_primary_selection_source_v1_listener ps_l = { ps_send, ps_cancel };
static void po_offer(void *d, struct zwp_primary_selection_offer_v1 *o, const char *m) { (void)d; (void)o; (void)m; }
static const struct zwp_primary_selection_offer_v1_listener po_l = { po_offer };
static void pd_offer(void *d, struct zwp_primary_selection_device_v1 *dv, struct zwp_primary_selection_offer_v1 *o) { (void)d; (void)dv; zwp_primary_selection_offer_v1_add_listener(o, &po_l, NULL); }
static void pd_sel(void *d, struct zwp_primary_selection_device_v1 *dv, struct zwp_primary_selection_offer_v1 *o) { (void)d; (void)dv; prim_offer = o; if (o) prim_ready = 1; }
static const struct zwp_primary_selection_device_v1_listener pd_l = { pd_offer, pd_sel };

/* reads what our own source sends back through the compositor (both ends are ours) */
static int read_back(int rfd, volatile int *sent, char *out, size_t n) {
    if (tb_wait(sent, 1500) < 0) return -1;
    size_t got = 0;
    struct pollfd p = { rfd, POLLIN, 0 };
    while (got + 1 < n && poll(&p, 1, 500) > 0) {
        ssize_t k = read(rfd, out + got, n - 1 - got);
        if (k <= 0) break;
        got += (size_t)k;
    }
    out[got] = 0;
    return 0;
}

/* ---- relative pointer and gestures: counted while the bench runs ---- */
static Check *rel_check, *gest_check;
static unsigned long rel_events, swipes, pinches, holds;
static void rel_motion(void *d, struct zwp_relative_pointer_v1 *r, uint32_t hi, uint32_t lo, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t ux, wl_fixed_t uy) {
    (void)d; (void)r; (void)hi; (void)lo; (void)dx; (void)dy; (void)ux; (void)uy;
    rel_events++;
    if (rel_check) set(rel_check, CK_PASS, "%lu RELATIVE MOTION EVENTS", rel_events);
}
static const struct zwp_relative_pointer_v1_listener rel_l = { rel_motion };
static void gest_note(void) { if (gest_check) set(gest_check, CK_PASS, "SWIPES %lu  PINCHES %lu  HOLDS %lu", swipes, pinches, holds); }
static void sw_begin(void *d, struct zwp_pointer_gesture_swipe_v1 *g, uint32_t s, uint32_t t, struct wl_surface *sf, uint32_t f) { (void)d; (void)g; (void)s; (void)t; (void)sf; (void)f; swipes++; gest_note(); }
static void sw_update(void *d, struct zwp_pointer_gesture_swipe_v1 *g, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)g; (void)t; (void)x; (void)y; }
static void sw_end(void *d, struct zwp_pointer_gesture_swipe_v1 *g, uint32_t s, uint32_t t, int32_t c) { (void)d; (void)g; (void)s; (void)t; (void)c; }
static const struct zwp_pointer_gesture_swipe_v1_listener sw_l = { sw_begin, sw_update, sw_end };
static void pi_begin(void *d, struct zwp_pointer_gesture_pinch_v1 *g, uint32_t s, uint32_t t, struct wl_surface *sf, uint32_t f) { (void)d; (void)g; (void)s; (void)t; (void)sf; (void)f; pinches++; gest_note(); }
static void pi_update(void *d, struct zwp_pointer_gesture_pinch_v1 *g, uint32_t t, wl_fixed_t x, wl_fixed_t y, wl_fixed_t sc, wl_fixed_t r) { (void)d; (void)g; (void)t; (void)x; (void)y; (void)sc; (void)r; }
static void pi_end(void *d, struct zwp_pointer_gesture_pinch_v1 *g, uint32_t s, uint32_t t, int32_t c) { (void)d; (void)g; (void)s; (void)t; (void)c; }
static const struct zwp_pointer_gesture_pinch_v1_listener pi_l = { pi_begin, pi_update, pi_end };
static void ho_begin(void *d, struct zwp_pointer_gesture_hold_v1 *g, uint32_t s, uint32_t t, struct wl_surface *sf, uint32_t f) { (void)d; (void)g; (void)s; (void)t; (void)sf; (void)f; holds++; gest_note(); }
static void ho_end(void *d, struct zwp_pointer_gesture_hold_v1 *g, uint32_t s, uint32_t t, int32_t c) { (void)d; (void)g; (void)s; (void)t; (void)c; }
static const struct zwp_pointer_gesture_hold_v1_listener ho_l = { ho_begin, ho_end };

/* a dead connection: name the error on the running check */
static int died(void) {
    const struct wl_interface *in = NULL;
    uint32_t id = 0;
    uint32_t code = wl_display_get_protocol_error(dpy, &in, &id);
    if (cur) set(cur, CK_FAIL, "PROTOCOL ERROR %u ON %s@%u (CONNECTION ENDED)", code, in ? in->name : "?", id);
    checks_done = 1;
    return -1;
}

static void sync_or(void) { wl_display_roundtrip(dpy); }

int checks_run(void) {
    nchecks = 0;
    Check *c;

    /* wl_shm: the formats every client needs */
    c = begin("wl_shm formats");
    struct wl_shm *s2 = bind("wl_shm", &wl_shm_interface, 2);
    if (!s2) set(c, CK_FAIL, "%s", "NOT OFFERED");
    else {
        wl_shm_add_listener(s2, &shm_l, NULL);
        sync_or();
        set(c, shm_argb && shm_xrgb ? CK_PASS : CK_FAIL, "%d FORMATS, ARGB8888 %s, XRGB8888 %s", shm_n, shm_argb ? "YES" : "NO", shm_xrgb ? "YES" : "NO");
        if (wl_shm_get_version(s2) >= 2) wl_shm_release(s2); else wl_shm_destroy(s2);
    }
    if (wl_display_get_error(dpy)) return died();

    /* outputs, their current mode and the logical layout */
    c = begin("wl_output modes");
    struct zxdg_output_manager_v1 *xom = bind("zxdg_output_manager_v1", &zxdg_output_manager_v1_interface, 3);
    for (int i = 0; i < nglobals && nouts < 8; i++) {
        if (strcmp(globals[i].iface, "wl_output")) continue;
        Out *u = &outs[nouts++];
        struct wl_output *o = wl_registry_bind(registry, globals[i].name, &wl_output_interface, globals[i].version < 4 ? globals[i].version : 4);
        wl_output_add_listener(o, &out_l, u);
        if (xom) zxdg_output_v1_add_listener(zxdg_output_manager_v1_get_xdg_output(xom, o), &xo_l, u);
    }
    sync_or();
    sync_or();
    if (!nouts) set(c, CK_FAIL, "%s", "NO OUTPUT");
    else {
        Out *u = &outs[0];
        out_refresh_mhz = u->refresh;
        set(c, u->done && u->w ? CK_PASS : CK_FAIL, "%d OUTPUT%s: %.20s %dX%d @ %.2f HZ, SCALE %d", nouts, nouts > 1 ? "S" : "",
            u->name[0] ? u->name : u->model, u->w, u->h, u->refresh / 1000.0, u->scale ? u->scale : 1);
    }
    if (xom) {
        c = begin("zxdg_output layout");
        set(c, outs[0].lw ? CK_PASS : CK_FAIL, "LOGICAL %dX%d AT %d,%d", outs[0].lw, outs[0].lh, outs[0].lx, outs[0].ly);
    }
    if (wl_display_get_error(dpy)) return died();

    /* fractional scale: the preferred scale of our window */
    c = begin("fractional scale");
    struct wp_fractional_scale_manager_v1 *fsm = bind("wp_fractional_scale_manager_v1", &wp_fractional_scale_manager_v1_interface, 1);
    if (!fsm) set(c, CK_INFO, "%s", "NOT OFFERED");
    else {
        struct wp_fractional_scale_v1 *fs = wp_fractional_scale_manager_v1_get_fractional_scale(fsm, surf);
        wp_fractional_scale_v1_add_listener(fs, &frac_l, NULL);
        wl_surface_commit(surf);
        if (tb_wait(&frac_got, 1500) < 0) return died();
        if (frac_got) set(c, CK_PASS, "PREFERRED %.3f", frac_val / 120.0);
        else set(c, CK_FAIL, "%s", "NO PREFERRED_SCALE EVENT IN 1.5 S");
    }

    /* a subsurface with a viewport, shown (its frame callback comes back) */
    c = begin("subsurface + viewport");
    struct wl_subcompositor *sc = bind("wl_subcompositor", &wl_subcompositor_interface, 1);
    struct wp_viewporter *vp = bind("wp_viewporter", &wp_viewporter_interface, 1);
    if (!sc) set(c, CK_FAIL, "%s", "WL_SUBCOMPOSITOR NOT OFFERED");
    else {
        struct wl_surface *child = wl_compositor_create_surface(comp);
        struct wl_subsurface *sub = wl_subcompositor_get_subsurface(sc, child, surf);
        wl_subsurface_set_position(sub, 24, 24);
        wl_subsurface_set_desync(sub);
        struct wp_viewport *v = vp ? wp_viewporter_get_viewport(vp, child) : NULL;
        if (v) wp_viewport_set_destination(v, 96, 96);
        struct wl_buffer *b = tb_solid_buffer(48, 48, 0xffff00ffu);
        wl_surface_attach(child, b, 0, 0);
        wl_surface_damage_buffer(child, 0, 0, 48, 48);
        static volatile int got;
        got = 0;
        wl_callback_add_listener(wl_surface_frame(child), &cb_l, (void *)&got);
        wl_surface_commit(child);
        wl_surface_commit(surf);
        if (tb_wait(&got, 2000) < 0) return died();
        set(c, got ? CK_PASS : CK_FAIL, "%s, VIEWPORT %s", got ? "SHOWN (FRAME CALLBACK)" : "NO FRAME CALLBACK FOR THE SUBSURFACE IN 2 S", v ? "48 -> 96" : "NOT OFFERED");
        if (v) wp_viewport_destroy(v);
        wl_subsurface_destroy(sub);
        wl_surface_destroy(child);
        if (b) wl_buffer_destroy(b);
        wl_surface_commit(surf);
    }
    if (wl_display_get_error(dpy)) return died();

    /* a popup positioned on our window */
    c = begin("xdg popup");
    {
        struct xdg_positioner *pos = xdg_wm_base_create_positioner(wm);
        xdg_positioner_set_size(pos, 200, 100);
        xdg_positioner_set_anchor_rect(pos, 40, 40, 1, 1);
        xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
        xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        struct wl_surface *ps = wl_compositor_create_surface(comp);
        struct xdg_surface *pxs = xdg_wm_base_get_xdg_surface(wm, ps);
        xdg_surface_add_listener(pxs, &pxs_l, NULL);
        struct xdg_popup *pop = xdg_surface_get_popup(pxs, xsurf, pos);
        xdg_popup_add_listener(pop, &pop_l, NULL);
        pop_conf = pop_xconf = 0;
        wl_surface_commit(ps);
        if (tb_wait(&pop_xconf, 2000) < 0) return died();
        struct wl_buffer *b = NULL;
        if (pop_xconf) {
            b = tb_solid_buffer(200, 100, 0xff2060ffu);
            wl_surface_attach(ps, b, 0, 0);
            wl_surface_commit(ps);
            sync_or();
        }
        if (pop_conf && pop_xconf) set(c, CK_PASS, "CONFIGURED %dX%d, MAPPED", pop_w, pop_h);
        else set(c, CK_FAIL, "NO CONFIGURE IN 2 S");
        xdg_popup_destroy(pop);
        xdg_surface_destroy(pxs);
        wl_surface_destroy(ps);
        if (b) wl_buffer_destroy(b);
        xdg_positioner_destroy(pos);
        sync_or();
    }
    if (wl_display_get_error(dpy)) return died();

    /* maximize and fullscreen: the configure must carry the state, and lose it after */
    static const struct { const char *id; int bit; } wstates[2] = { { "maximize", XDG_TOPLEVEL_STATE_MAXIMIZED }, { "fullscreen", XDG_TOPLEVEL_STATE_FULLSCREEN } };
    for (int k = 0; k < 2; k++) {
        c = begin(wstates[k].id);
        if (k == 0) xdg_toplevel_set_maximized(top); else xdg_toplevel_set_fullscreen(top, NULL);
        wl_surface_commit(surf);
        double end = ms_now() + 2000;
        while (!(toplevel_states & (1u << wstates[k].bit)) && ms_now() < end) if (tb_wait(NULL, 50) < 0) return died();
        int on = !!(toplevel_states & (1u << wstates[k].bit));
        int w = win_w, h = win_h;
        if (k == 0) xdg_toplevel_unset_maximized(top); else xdg_toplevel_unset_fullscreen(top);
        wl_surface_commit(surf);
        end = ms_now() + 2000;
        while ((toplevel_states & (1u << wstates[k].bit)) && ms_now() < end) if (tb_wait(NULL, 50) < 0) return died();
        int off = !(toplevel_states & (1u << wstates[k].bit));
        if (!on) set(c, CK_FAIL, "NO CONFIGURE WITH THE STATE IN 2 S");
        else set(c, off ? CK_PASS : CK_FAIL, "STATE SET (%dX%d)%s", w, h, off ? " AND CLEARED" : " BUT NOT CLEARED");
    }

    /* dmabuf: what the compositor imports, and from which device */
    c = begin("linux-dmabuf");
    Global *dg = find("zwp_linux_dmabuf_v1");
    if (!dg) set(c, CK_INFO, "%s", "NOT OFFERED (SHM ONLY)");
    else if (dg->version >= 4) {
        struct zwp_linux_dmabuf_v1 *dm = bind("zwp_linux_dmabuf_v1", &zwp_linux_dmabuf_v1_interface, 4);
        struct zwp_linux_dmabuf_feedback_v1 *f = zwp_linux_dmabuf_v1_get_surface_feedback(dm, surf);
        zwp_linux_dmabuf_feedback_v1_add_listener(f, &fb_l, NULL);
        if (tb_wait(&fb_done_flag, 2000) < 0) return died();
        if (fb_done_flag) set(c, CK_PASS, "V%u: DEVICE %s, %zu FORMAT ENTRIES, %d TRANCHE%s%s", dg->version, fb_device, fb_table_size / 16,
                              fb_tranches, fb_tranches == 1 ? "" : "S", fb_scanout ? ", SCANOUT TRANCHE" : "");
        else set(c, CK_FAIL, "%s", "NO FEEDBACK DONE IN 2 S");
        zwp_linux_dmabuf_feedback_v1_destroy(f);
    } else {
        struct zwp_linux_dmabuf_v1 *dm = bind("zwp_linux_dmabuf_v1", &zwp_linux_dmabuf_v1_interface, dg->version);
        zwp_linux_dmabuf_v1_add_listener(dm, &dm_l, NULL);
        sync_or();
        set(c, dm_formats ? CK_PASS : CK_FAIL, "V%u: %d FORMAT/MODIFIER PAIRS", dg->version, dm_formats);
    }
    if (wl_display_get_error(dpy)) return died();

    /* the clipboard, through the compositor and back */
    snprintf(clip_token, sizeof clip_token, "lordos-testbench-%ld", (long)getpid());
    c = begin("clipboard round trip");
    struct wl_data_device_manager *ddm = bind("wl_data_device_manager", &wl_data_device_manager_interface, 3);
    if (!ddm) set(c, CK_FAIL, "%s", "WL_DATA_DEVICE_MANAGER NOT OFFERED");
    else if (!kb_serial) set(c, CK_UNTESTED, "%s", "NEEDS KEYBOARD FOCUS (CLICK THE WINDOW, PRESS F5)");
    else {
        struct wl_data_device *dd = wl_data_device_manager_get_data_device(ddm, seat);
        wl_data_device_add_listener(dd, &dd_l, NULL);
        struct wl_data_source *src = wl_data_device_manager_create_data_source(ddm);
        wl_data_source_add_listener(src, &ds_l, NULL);
        wl_data_source_offer(src, CLIP_MIME);
        clip_offer_ready = clip_sent = 0;
        wl_data_device_set_selection(dd, src, kb_serial);
        if (tb_wait(&clip_offer_ready, 1500) < 0) return died();
        char got[64] = "";
        if (clip_offer_ready && clip_offer) {
            int pp[2];
            if (!pipe2(pp, O_CLOEXEC)) {
                clip_sent = 0;              /* the compositor's own read of the source may have set it already */
                wl_data_offer_receive(clip_offer, CLIP_MIME, pp[1]);
                close(pp[1]);
                wl_display_flush(dpy);
                if (read_back(pp[0], &clip_sent, got, sizeof got) < 0) return died();
                close(pp[0]);
            }
        }
        int ok = !strcmp(got, clip_token);
        set(c, ok ? CK_PASS : CK_FAIL, "%s", !clip_offer_ready ? "THE NEW SELECTION WASN'T SENT BACK TO THE FOCUSED CLIENT THAT SET IT" : ok ? "TEXT CAME BACK INTACT" : got[0] ? "TEXT CHANGED" : "OFFER CAME, NO TEXT");
    }
    c = begin("primary selection");
    struct zwp_primary_selection_device_manager_v1 *pdm = bind("zwp_primary_selection_device_manager_v1", &zwp_primary_selection_device_manager_v1_interface, 1);
    if (!pdm) set(c, CK_INFO, "%s", "NOT OFFERED");
    else if (!kb_serial) set(c, CK_UNTESTED, "%s", "NEEDS KEYBOARD FOCUS");
    else {
        struct zwp_primary_selection_device_v1 *pd = zwp_primary_selection_device_manager_v1_get_device(pdm, seat);
        zwp_primary_selection_device_v1_add_listener(pd, &pd_l, NULL);
        struct zwp_primary_selection_source_v1 *ps = zwp_primary_selection_device_manager_v1_create_source(pdm);
        zwp_primary_selection_source_v1_add_listener(ps, &ps_l, NULL);
        zwp_primary_selection_source_v1_offer(ps, CLIP_MIME);
        prim_ready = prim_sent = 0;
        zwp_primary_selection_device_v1_set_selection(pd, ps, kb_serial);
        if (tb_wait(&prim_ready, 1500) < 0) return died();
        char got[64] = "";
        if (prim_ready && prim_offer) {
            int pp[2];
            if (!pipe2(pp, O_CLOEXEC)) {
                prim_sent = 0;
                zwp_primary_selection_offer_v1_receive(prim_offer, CLIP_MIME, pp[1]);
                close(pp[1]);
                wl_display_flush(dpy);
                if (read_back(pp[0], &prim_sent, got, sizeof got) < 0) return died();
                close(pp[0]);
            }
        }
        int ok = !strcmp(got, clip_token);
        set(c, ok ? CK_PASS : CK_FAIL, "%s", !prim_ready ? "THE NEW SELECTION WASN'T SENT BACK TO THE FOCUSED CLIENT THAT SET IT" : ok ? "TEXT CAME BACK INTACT" : got[0] ? "TEXT CHANGED" : "OFFER CAME, NO TEXT");
    }
    if (wl_display_get_error(dpy)) return died();

    /* the pointer's extras */
    c = begin("cursor shape");
    struct wp_cursor_shape_manager_v1 *csm = bind("wp_cursor_shape_manager_v1", &wp_cursor_shape_manager_v1_interface, 1);
    if (!csm) set(c, CK_INFO, "%s", "NOT OFFERED");
    else if (!pointer || !ptr_serial) set(c, CK_UNTESTED, "%s", "NEEDS THE POINTER OVER THE WINDOW");
    else {
        struct wp_cursor_shape_device_v1 *cd = wp_cursor_shape_manager_v1_get_pointer(csm, pointer);
        wp_cursor_shape_device_v1_set_shape(cd, ptr_serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR);
        sync_or();
        if (wl_display_get_error(dpy)) return died();
        set(c, CK_PASS, "%s", "CROSSHAIR ACCEPTED");
    }
    c = begin("relative pointer");
    struct zwp_relative_pointer_manager_v1 *rpm = bind("zwp_relative_pointer_manager_v1", &zwp_relative_pointer_manager_v1_interface, 1);
    if (!rpm) set(c, CK_INFO, "%s", "NOT OFFERED");
    else if (!pointer) set(c, CK_UNTESTED, "%s", "NO POINTER");
    else {
        zwp_relative_pointer_v1_add_listener(zwp_relative_pointer_manager_v1_get_relative_pointer(rpm, pointer), &rel_l, NULL);
        rel_check = c;
        set(c, CK_UNTESTED, "%s", "BOUND: MOVE THE MOUSE");
    }
    c = begin("pointer constraints");
    struct zwp_pointer_constraints_v1 *pcs = bind("zwp_pointer_constraints_v1", &zwp_pointer_constraints_v1_interface, 1);
    set(c, CK_INFO, "%s", pcs ? "BOUND (LOCK AND CONFINE: NOT TRIED YET)" : "NOT OFFERED");
    c = begin("pointer gestures");
    Global *gg = find("zwp_pointer_gestures_v1");
    if (!gg) set(c, CK_INFO, "%s", "NOT OFFERED");
    else if (!pointer) set(c, CK_UNTESTED, "%s", "NO POINTER");
    else {
        struct zwp_pointer_gestures_v1 *pg = bind("zwp_pointer_gestures_v1", &zwp_pointer_gestures_v1_interface, 3);
        zwp_pointer_gesture_swipe_v1_add_listener(zwp_pointer_gestures_v1_get_swipe_gesture(pg, pointer), &sw_l, NULL);
        zwp_pointer_gesture_pinch_v1_add_listener(zwp_pointer_gestures_v1_get_pinch_gesture(pg, pointer), &pi_l, NULL);
        if (gg->version >= 3) zwp_pointer_gesture_hold_v1_add_listener(zwp_pointer_gestures_v1_get_hold_gesture(pg, pointer), &ho_l, NULL);
        gest_check = c;
        set(c, CK_UNTESTED, "BOUND V%u: SWIPE OR PINCH ON A TOUCHPAD", gg->version);
    }
    /* idle inhibit: what a video player does to keep the screen on */
    c = begin("idle inhibit");
    struct zwp_idle_inhibit_manager_v1 *iim = bind("zwp_idle_inhibit_manager_v1", &zwp_idle_inhibit_manager_v1_interface, 1);
    if (!iim) set(c, CK_FAIL, "%s", "NOT OFFERED: A VIDEO PLAYER CAN'T KEEP THE SCREEN ON");
    else {
        struct zwp_idle_inhibitor_v1 *in = zwp_idle_inhibit_manager_v1_create_inhibitor(iim, surf);
        sync_or();
        if (wl_display_get_error(dpy)) return died();
        zwp_idle_inhibitor_v1_destroy(in);
        sync_or();
        set(c, CK_PASS, "%s", "INHIBITOR CREATED AND RELEASED");
    }
    /* what a desktop expects to find, offered or not */
    static const struct { const char *iface, *what; int needed; } expect[] = {
        { "wl_seat", "seat", 1 }, { "xdg_wm_base", "xdg-shell", 1 }, { "zxdg_decoration_manager_v1", "server decorations", 0 },
        { "wp_presentation", "presentation time", 0 }, { "zwp_text_input_manager_v3", "input methods (text-input v3)", 0 },
        { "xdg_activation_v1", "xdg-activation", 0 },
        { "wp_single_pixel_buffer_manager_v1", "single-pixel buffers", 0 }, { "wp_tearing_control_manager_v1", "tearing control (games)", 0 },
        { "wp_content_type_manager_v1", "content type", 0 }, { "zwp_tablet_manager_v2", "drawing tablets", 0 },
        { "ext_idle_notifier_v1", "idle notify", 0 }, { "wp_drm_lease_device_v1", "VR headsets (drm-lease)", 0 },
    };
    for (size_t i = 0; i < sizeof expect / sizeof expect[0]; i++) {
        Global *g = find(expect[i].iface);
        c = begin(expect[i].what);
        if (g) set(c, CK_PASS, "%s V%u OFFERED", expect[i].iface, g->version);
        else set(c, expect[i].needed ? CK_FAIL : CK_INFO, "%s NOT OFFERED", expect[i].iface);
    }
    cur = NULL;
    checks_done = 1;
    return wl_display_get_error(dpy) ? died() : 0;
}
