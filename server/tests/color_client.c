/* color_client.c — color-management-v1 and color-representation-v1 against lord-server (src/wl/wlcolor.c,
 * docs/WAYLAND.md "Colour management"). Run by tests/color_headless.sh against a headless lord-server.
 *
 *   protocols   what both globals advertise (exactly what colorconv.c converts), the output's image description and its
 *               information, the surface feedback's preferred description (the output's), parametric descriptions
 *               (ready, shared records, failed for primaries that can't be used), a description and an alpha mode set
 *               on a mapped window; then every protocol error, each on a connection of its own
 *   show        a 200×100 window "color:linear" whose pixels (0x80, 0x40, 0x20) are linear light (ext_linear, sRGB
 *               primaries) for 3 s, then the same pixels in "color:plain" with no description for 3 s: the theme of
 *               tests/color_headless.sh reads the screen and counts both colours
 *   held        "color:held": a commit of those pixels as linear light held behind the fifo barrier, the description
 *               unset (pending, never committed) right after: it must still show as linear light
 *
 *   WAYLAND_DISPLAY=… build/color-client MODE   (exit 0, or 1 on a failed check) */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"
#include "client/color-management-v1-client-protocol.h"
#include "client/color-representation-v1-client-protocol.h"
#include "client/fifo-v1-client-protocol.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wmbase;
static struct wp_color_manager_v1 *cm;
static struct wp_color_representation_manager_v1 *crm;
static struct wp_fifo_manager_v1 *fifom;
static struct wl_output *output;
static unsigned intents, features, tfs, prims, alphas, cm_done, cr_done;
static int ncoef, coef[16][2];
static int fails;

static void check(const char *what, long got, long want) {
    if (got == want) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}

static void cm_intent(void *d, struct wp_color_manager_v1 *m, uint32_t v) { (void)d; (void)m; intents |= 1u << v; }
static void cm_feature(void *d, struct wp_color_manager_v1 *m, uint32_t v) { (void)d; (void)m; features |= 1u << v; }
static void cm_tf(void *d, struct wp_color_manager_v1 *m, uint32_t v) { (void)d; (void)m; tfs |= 1u << v; }
static void cm_prim(void *d, struct wp_color_manager_v1 *m, uint32_t v) { (void)d; (void)m; prims |= 1u << v; }
static void cm_done_ev(void *d, struct wp_color_manager_v1 *m) { (void)d; (void)m; cm_done++; }
static const struct wp_color_manager_v1_listener cm_l = { cm_intent, cm_feature, cm_tf, cm_prim, cm_done_ev };
static void cr_alpha(void *d, struct wp_color_representation_manager_v1 *m, uint32_t v) { (void)d; (void)m; alphas |= 1u << v; }
static void cr_coef(void *d, struct wp_color_representation_manager_v1 *m, uint32_t c, uint32_t r) {
    (void)d; (void)m;
    if (ncoef < 16) { coef[ncoef][0] = (int)c; coef[ncoef][1] = (int)r; ncoef++; }
}
static void cr_done_ev(void *d, struct wp_color_representation_manager_v1 *m) { (void)d; (void)m; cr_done++; }
static const struct wp_color_representation_manager_v1_listener cr_l = { cr_alpha, cr_coef, cr_done_ev };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d; (void)ver;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wmbase = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, wl_output_interface.name) && !output) output = wl_registry_bind(r, name, &wl_output_interface, 2);
    else if (!strcmp(iface, wp_color_manager_v1_interface.name)) {
        cm = wl_registry_bind(r, name, &wp_color_manager_v1_interface, 1);
        wp_color_manager_v1_add_listener(cm, &cm_l, NULL);
    } else if (!strcmp(iface, wp_fifo_manager_v1_interface.name)) {
        fifom = wl_registry_bind(r, name, &wp_fifo_manager_v1_interface, 1);
    } else if (!strcmp(iface, wp_color_representation_manager_v1_interface.name)) {
        crm = wl_registry_bind(r, name, &wp_color_representation_manager_v1_interface, 1);
        wp_color_representation_manager_v1_add_listener(crm, &cr_l, NULL);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };

static void connect_all(void) {
    if (dpy) wl_display_disconnect(dpy);
    comp = NULL; shm = NULL; wmbase = NULL; cm = NULL; crm = NULL; output = NULL;
    intents = features = tfs = prims = alphas = cm_done = cr_done = 0;
    ncoef = 0;
    dpy = wl_display_connect(NULL);
    if (!dpy) { printf("FAIL no Wayland display\n"); exit(1); }
    wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, NULL);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wmbase || !cm || !crm || !output) { printf("FAIL a global is missing (color manager %p, representation %p)\n", (void *)cm, (void *)crm); exit(1); }
    xdg_wm_base_add_listener(wmbase, &wm_l, NULL);
}

/* ---- image descriptions ---- */

typedef struct { int ready, failed; uint32_t id, cause; } Img;
static void img_failed(void *d, struct wp_image_description_v1 *i, uint32_t cause, const char *msg) { (void)i; (void)msg; Img *m = d; m->failed = 1; m->cause = cause; }
static void img_ready(void *d, struct wp_image_description_v1 *i, uint32_t id) { (void)i; Img *m = d; m->ready = 1; m->id = id; }
static const struct wp_image_description_v1_listener img_l = { img_failed, img_ready };

typedef struct {
    int done, prim[8], named, tf, power, lum[3], target[8], tlum[2], cll, fall, nprim, ntarget, nlum, ntlum;
} Info;
static void in_done(void *d, struct wp_image_description_info_v1 *i) { ((Info *)d)->done = 1; wp_image_description_info_v1_destroy(i); }
static void in_icc(void *d, struct wp_image_description_info_v1 *i, int32_t fd, uint32_t n) { (void)d; (void)i; (void)n; close(fd); }
static void in_prim(void *d, struct wp_image_description_info_v1 *i, int32_t a, int32_t b, int32_t c, int32_t e, int32_t f, int32_t g, int32_t h, int32_t k) {
    (void)i;
    Info *n = d;
    int v[8] = { a, b, c, e, f, g, h, k };
    memcpy(n->prim, v, sizeof v);
    n->nprim++;
}
static void in_named(void *d, struct wp_image_description_info_v1 *i, uint32_t p) { (void)i; ((Info *)d)->named = (int)p; }
static void in_power(void *d, struct wp_image_description_info_v1 *i, uint32_t p) { (void)i; ((Info *)d)->power = (int)p; }
static void in_tf(void *d, struct wp_image_description_info_v1 *i, uint32_t t) { (void)i; ((Info *)d)->tf = (int)t; }
static void in_lum(void *d, struct wp_image_description_info_v1 *i, uint32_t a, uint32_t b, uint32_t c) {
    (void)i;
    Info *n = d;
    n->lum[0] = (int)a; n->lum[1] = (int)b; n->lum[2] = (int)c;
    n->nlum++;
}
static void in_target(void *d, struct wp_image_description_info_v1 *i, int32_t a, int32_t b, int32_t c, int32_t e, int32_t f, int32_t g, int32_t h, int32_t k) {
    (void)i;
    Info *n = d;
    int v[8] = { a, b, c, e, f, g, h, k };
    memcpy(n->target, v, sizeof v);
    n->ntarget++;
}
static void in_tlum(void *d, struct wp_image_description_info_v1 *i, uint32_t a, uint32_t b) { (void)i; Info *n = d; n->tlum[0] = (int)a; n->tlum[1] = (int)b; n->ntlum++; }
static void in_cll(void *d, struct wp_image_description_info_v1 *i, uint32_t v) { (void)i; ((Info *)d)->cll = (int)v; }
static void in_fall(void *d, struct wp_image_description_info_v1 *i, uint32_t v) { (void)i; ((Info *)d)->fall = (int)v; }
static const struct wp_image_description_info_v1_listener info_l = { in_done, in_icc, in_prim, in_named, in_power, in_tf, in_lum, in_target,
                                                                     in_tlum, in_cll, in_fall };

static struct wp_image_description_v1 *watch(struct wp_image_description_v1 *d, Img *m) {
    memset(m, 0, sizeof *m);
    wp_image_description_v1_add_listener(d, &img_l, m);
    wl_display_roundtrip(dpy);
    return d;
}

static struct wp_image_description_v1 *params(uint32_t tf, uint32_t prim, Img *m) {
    struct wp_image_description_creator_params_v1 *p = wp_color_manager_v1_create_parametric_creator(cm);
    wp_image_description_creator_params_v1_set_tf_named(p, tf);
    wp_image_description_creator_params_v1_set_primaries_named(p, prim);
    return watch(wp_image_description_creator_params_v1_create(p), m);
}

/* ---- a window ---- */

typedef struct { int conf; uint32_t serial; } Win;
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) { (void)xs; Win *w = d; w->conf++; w->serial = serial; }
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) { (void)d; (void)t; (void)w; (void)h; (void)s; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, NULL, NULL };   /* (xdg_wm_base 1: no bounds or capabilities) */

static struct wl_buffer *buffer(int W, int H, uint32_t color, uint32_t format) {
    int stride = W * 4, size = stride * H;
    int fd = memfd_create("color-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { perror("memfd"); exit(2); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int i = 0; i < W * H; i++) px[i] = color;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, stride, format);
    wl_shm_pool_destroy(pool);
    munmap(px, (size_t)size);
    close(fd);
    return b;
}

typedef struct { struct wl_surface *s; struct xdg_surface *x; struct xdg_toplevel *t; Win w; } Window;
/* configured and acked; the caller attaches and commits */
static void window(Window *w, const char *title) {
    memset(w, 0, sizeof *w);
    w->s = wl_compositor_create_surface(comp);
    w->x = xdg_wm_base_get_xdg_surface(wmbase, w->s);
    xdg_surface_add_listener(w->x, &xs_l, &w->w);
    w->t = xdg_surface_get_toplevel(w->x);
    xdg_toplevel_add_listener(w->t, &top_l, NULL);
    xdg_toplevel_set_app_id(w->t, "color-client");
    xdg_toplevel_set_title(w->t, title);
    wl_surface_commit(w->s);
    wl_display_roundtrip(dpy);
    if (!w->w.conf) { printf("FAIL no configure\n"); exit(1); }
    xdg_surface_ack_configure(w->x, w->w.serial);
}
static void show(Window *w, struct wl_buffer *b) {
    wl_surface_attach(w->s, b, 0, 0);
    wl_surface_damage_buffer(w->s, 0, 0, 1 << 20, 1 << 20);
    wl_surface_commit(w->s);
}
static void close_window(Window *w) {
    xdg_toplevel_destroy(w->t);
    xdg_surface_destroy(w->x);
    wl_surface_destroy(w->s);
}

static void pause_ms(int ms) {
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        wl_display_roundtrip(dpy);
        usleep(20000);
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 < ms);
}

static int no_error(const char *what) {
    wl_display_roundtrip(dpy);
    int e = wl_display_get_error(dpy);
    check(what, e, 0);
    return e == 0;
}

/* ---- protocols ---- */

static const int srgb1m[8] = { 640000, 330000, 300000, 600000, 150000, 60000, 312700, 329000 };

static int run_protocols(void) {
    connect_all();
    check("wp_color_manager_v1: done once", cm_done, 1);
    check("rendering intents: perceptual and relative", intents, 1u << WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL | 1u << WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE);
    check("features: parametric, set_primaries, set_tf_power (no ICC, luminances, mastering, scRGB)", features,
          1u << WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC | 1u << WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES | 1u << WP_COLOR_MANAGER_V1_FEATURE_SET_TF_POWER);
    check("transfer functions: gamma22, srgb, ext_linear, bt1886, gamma28 (no PQ, HLG)", tfs,
          1u << WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22 | 1u << WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB |
          1u << WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR | 1u << WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886 |
          1u << WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28);
    check("primaries: every named set (1..10)", prims, 0x7feu);
    check("wp_color_representation_manager_v1: done once", cr_done, 1);
    check("alpha modes: premultiplied electrical, premultiplied optical, straight", alphas, 7);
    check("coefficients and ranges: one pair", ncoef, 1);
    check("  identity", coef[0][0], WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY);
    check("  full range", coef[0][1], WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL);

    /* the output's description and its information */
    struct wp_color_management_output_v1 *co = wp_color_manager_v1_get_output(cm, output);
    Img out;
    struct wp_image_description_v1 *od = watch(wp_color_management_output_v1_get_image_description(co), &out);
    check("output: image description ready", out.ready, 1);
    Info in;
    memset(&in, 0, sizeof in);
    wp_image_description_info_v1_add_listener(wp_image_description_v1_get_information(od), &info_l, &in);
    wl_display_roundtrip(dpy);
    check("output info: done", in.done, 1);
    check("output info: primaries once", in.nprim, 1);
    check("output info: sRGB primaries", !memcmp(in.prim, srgb1m, sizeof srgb1m), 1);
    check("output info: named sRGB", in.named, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
    check("output info: gamma 2.2", in.tf, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
    check("output info: luminances 0.2, 80, 80", in.lum[0] == 2000 && in.lum[1] == 80 && in.lum[2] == 80 && in.nlum == 1, 1);
    check("output info: target primaries (headless: sRGB's)", in.ntarget == 1 && !memcmp(in.target, srgb1m, sizeof srgb1m), 1);
    check("output info: target luminance 0.2, 80", in.ntlum == 1 && in.tlum[0] == 2000 && in.tlum[1] == 80, 1);

    /* a window: its preferred description is its display's */
    Window w;
    window(&w, "color:protocols");
    show(&w, buffer(64, 64, 0xff336699u, WL_SHM_FORMAT_XRGB8888));
    pause_ms(300);
    struct wp_color_management_surface_feedback_v1 *fb = wp_color_manager_v1_get_surface_feedback(cm, w.s);
    Img pref, prefp;
    watch(wp_color_management_surface_feedback_v1_get_preferred(fb), &pref);
    watch(wp_color_management_surface_feedback_v1_get_preferred_parametric(fb), &prefp);
    check("feedback: preferred ready, the output's record", pref.ready && pref.id == out.id, 1);
    check("feedback: preferred parametric, the same", prefp.ready && prefp.id == out.id, 1);

    /* parametric descriptions */
    Img lin, lin2, p3, pw, bad;
    struct wp_image_description_v1 *ld = params(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, &lin);
    params(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, &lin2);
    params(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB, WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3, &p3);
    check("ext_linear + sRGB: ready", lin.ready, 1);
    check("the same parameters: the same record", lin2.ready && lin2.id == lin.id, 1);
    check("a different one: another identity", lin.id != out.id && p3.ready && p3.id != lin.id, 1);
    struct wp_image_description_creator_params_v1 *pp = wp_color_manager_v1_create_parametric_creator(cm);
    wp_image_description_creator_params_v1_set_tf_power(pp, 24000);
    wp_image_description_creator_params_v1_set_primaries_named(pp, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    wp_image_description_creator_params_v1_set_max_cll(pp, 50);
    wp_image_description_creator_params_v1_set_max_fall(pp, 40);
    watch(wp_image_description_creator_params_v1_create(pp), &pw);
    check("power 2.4 + BT.2020 (+ max_cll, max_fall): ready", pw.ready, 1);
    pp = wp_color_manager_v1_create_parametric_creator(cm);
    wp_image_description_creator_params_v1_set_tf_named(pp, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
    wp_image_description_creator_params_v1_set_primaries(pp, 200000, 200000, 400000, 400000, 600000, 600000, 312700, 329000);
    watch(wp_image_description_creator_params_v1_create(pp), &bad);
    check("primaries on one line: failed, unsupported", bad.failed && bad.cause == WP_IMAGE_DESCRIPTION_V1_CAUSE_UNSUPPORTED, 1);

    /* set on the window, with an alpha mode */
    struct wp_color_management_surface_v1 *cs = wp_color_manager_v1_get_surface(cm, w.s);
    wp_color_management_surface_v1_set_image_description(cs, ld, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    wp_image_description_v1_destroy(ld);    /* copy semantics: destroying it changes nothing */
    struct wp_color_representation_surface_v1 *rs = wp_color_representation_manager_v1_get_surface(crm, w.s);
    wp_color_representation_surface_v1_set_alpha_mode(rs, WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_STRAIGHT);
    wp_color_representation_surface_v1_set_coefficients_and_range(rs, WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY,
                                                                   WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL);
    show(&w, buffer(64, 64, 0x80804020u, WL_SHM_FORMAT_ARGB8888));
    no_error("a description, straight alpha and identity/full on a window: no error");
    wp_color_management_surface_v1_unset_image_description(cs);
    wp_color_management_surface_v1_set_image_description(cs, od, WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE);
    wl_surface_commit(w.s);
    no_error("the output's own description, relative intent: no error");
    wp_color_management_surface_v1_destroy(cs);
    wp_color_representation_surface_v1_destroy(rs);
    wl_surface_commit(w.s);
    no_error("both destroyed (unset), committed: no error");
    close_window(&w);
    no_error("window closed: no error");
    return fails ? 1 : 0;
}

/* ---- protocol errors, one connection each ---- */

/* gone: the error is on an object the request itself destroyed (create is a destructor), which the client library
 * then reports with no interface */
static void expect_error_on(const char *what, const struct wl_interface *iface, uint32_t code, int gone) {
    wl_display_roundtrip(dpy);
    const struct wl_interface *got = NULL;
    uint32_t id = 0, c = wl_display_get_error(dpy) == EPROTO ? wl_display_get_protocol_error(dpy, &got, &id) : 0xffffffffu;
    if ((got == iface || (gone && !got)) && c == code) printf("ok   error: %s (%s.%u)\n", what, iface->name, code);
    else { fails++; printf("FAIL error: %s: got %s.%u, want %s.%u\n", what, got ? got->name : "none", c, iface->name, code); }
}
static void expect_error(const char *what, const struct wl_interface *iface, uint32_t code) { expect_error_on(what, iface, code, 0); }

static struct wp_image_description_creator_params_v1 *creator(void) { return wp_color_manager_v1_create_parametric_creator(cm); }

static void run_errors(void) {
    connect_all();
    wp_color_manager_v1_create_icc_creator(cm);
    expect_error("create_icc_creator (not advertised)", &wp_color_manager_v1_interface, WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE);

    connect_all();
    wp_color_manager_v1_create_windows_scrgb(cm);
    expect_error("create_windows_scrgb (not advertised)", &wp_color_manager_v1_interface, WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE);

    connect_all();
    struct wl_surface *s = wl_compositor_create_surface(comp);
    wp_color_manager_v1_get_surface(cm, s);
    wp_color_manager_v1_get_surface(cm, s);
    expect_error("get_surface twice", &wp_color_manager_v1_interface, WP_COLOR_MANAGER_V1_ERROR_SURFACE_EXISTS);

    connect_all();
    wp_image_description_creator_params_v1_set_tf_named(creator(), WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
    expect_error("PQ (HDR, not advertised)", &wp_image_description_creator_params_v1_interface, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF);

    connect_all();
    wp_image_description_creator_params_v1_set_tf_power(creator(), 5000);
    expect_error("power 0.5", &wp_image_description_creator_params_v1_interface, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF);

    connect_all();
    wp_image_description_creator_params_v1_set_primaries_named(creator(), 99);
    expect_error("primaries 99", &wp_image_description_creator_params_v1_interface, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_PRIMARIES_NAMED);

    connect_all();
    struct wp_image_description_creator_params_v1 *p = creator();
    wp_image_description_creator_params_v1_set_tf_named(p, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
    wp_image_description_creator_params_v1_set_tf_named(p, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB);
    expect_error("transfer function twice", &wp_image_description_creator_params_v1_interface, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET);

    connect_all();
    p = creator();
    wp_image_description_creator_params_v1_set_tf_named(p, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
    wp_image_description_creator_params_v1_create(p);
    expect_error_on("create without primaries", &wp_image_description_creator_params_v1_interface, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET, 1);

    connect_all();
    wp_image_description_creator_params_v1_set_luminances(creator(), 2000, 80, 80);
    expect_error("set_luminances (not advertised)", &wp_image_description_creator_params_v1_interface, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE);

    connect_all();
    wp_image_description_creator_params_v1_set_mastering_display_primaries(creator(), 1, 1, 1, 1, 1, 1, 1, 1);
    expect_error("set_mastering_display_primaries (not advertised)", &wp_image_description_creator_params_v1_interface, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE);

    connect_all();
    p = creator();
    wp_image_description_creator_params_v1_set_tf_named(p, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
    wp_image_description_creator_params_v1_set_primaries_named(p, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
    wp_image_description_creator_params_v1_set_max_cll(p, 1000);
    wp_image_description_creator_params_v1_create(p);
    expect_error_on("max_cll above the 80 cd/m² of gamma 2.2", &wp_image_description_creator_params_v1_interface, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE, 1);

    connect_all();
    Img m;
    struct wp_image_description_v1 *d = params(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020, &m);
    wp_image_description_v1_get_information(d);
    expect_error("get_information on a client's description", &wp_image_description_v1_interface, WP_IMAGE_DESCRIPTION_V1_ERROR_NO_INFORMATION);

    connect_all();
    p = creator();
    wp_image_description_creator_params_v1_set_tf_named(p, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
    wp_image_description_creator_params_v1_set_primaries(p, 200000, 200000, 400000, 400000, 600000, 600000, 312700, 329000);
    d = watch(wp_image_description_creator_params_v1_create(p), &m);
    wp_image_description_v1_get_information(d);
    expect_error("get_information on a failed description", &wp_image_description_v1_interface, WP_IMAGE_DESCRIPTION_V1_ERROR_NOT_READY);

    connect_all();
    p = creator();
    wp_image_description_creator_params_v1_set_tf_named(p, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
    wp_image_description_creator_params_v1_set_primaries(p, 200000, 200000, 400000, 400000, 600000, 600000, 312700, 329000);
    d = watch(wp_image_description_creator_params_v1_create(p), &m);
    s = wl_compositor_create_surface(comp);
    wp_color_management_surface_v1_set_image_description(wp_color_manager_v1_get_surface(cm, s), d, 0);
    expect_error("set_image_description with a failed description", &wp_color_management_surface_v1_interface, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_IMAGE_DESCRIPTION);

    connect_all();
    d = params(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, &m);
    s = wl_compositor_create_surface(comp);
    wp_color_management_surface_v1_set_image_description(wp_color_manager_v1_get_surface(cm, s), d, WP_COLOR_MANAGER_V1_RENDER_INTENT_ABSOLUTE);
    expect_error("absolute intent (not advertised)", &wp_color_management_surface_v1_interface, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_RENDER_INTENT);

    connect_all();
    d = params(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, &m);
    s = wl_compositor_create_surface(comp);
    struct wp_color_management_surface_v1 *cs = wp_color_manager_v1_get_surface(cm, s);
    wl_surface_destroy(s);
    wp_color_management_surface_v1_set_image_description(cs, d, 0);
    expect_error("set_image_description after the wl_surface is gone", &wp_color_management_surface_v1_interface, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT);

    connect_all();
    s = wl_compositor_create_surface(comp);
    struct wp_color_management_surface_feedback_v1 *fb = wp_color_manager_v1_get_surface_feedback(cm, s);
    wl_surface_destroy(s);
    wp_color_management_surface_feedback_v1_get_preferred(fb);
    expect_error("get_preferred after the wl_surface is gone", &wp_color_management_surface_feedback_v1_interface, WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_ERROR_INERT);

    connect_all();
    s = wl_compositor_create_surface(comp);
    wp_color_representation_manager_v1_get_surface(crm, s);
    wp_color_representation_manager_v1_get_surface(crm, s);
    expect_error("representation get_surface twice", &wp_color_representation_manager_v1_interface, WP_COLOR_REPRESENTATION_MANAGER_V1_ERROR_SURFACE_EXISTS);

    connect_all();
    s = wl_compositor_create_surface(comp);
    wp_color_representation_surface_v1_set_alpha_mode(wp_color_representation_manager_v1_get_surface(crm, s), 3);
    expect_error("alpha mode 3", &wp_color_representation_surface_v1_interface, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_ALPHA_MODE);

    connect_all();
    s = wl_compositor_create_surface(comp);
    wp_color_representation_surface_v1_set_coefficients_and_range(wp_color_representation_manager_v1_get_surface(crm, s),
                                                                  WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709,
                                                                  WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED);
    expect_error("BT.709 limited (no YUV import)", &wp_color_representation_surface_v1_interface, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_COEFFICIENTS);

    connect_all();
    s = wl_compositor_create_surface(comp);
    struct wp_color_representation_surface_v1 *rs = wp_color_representation_manager_v1_get_surface(crm, s);
    wp_color_representation_surface_v1_set_chroma_location(rs, WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_0);
    wl_surface_attach(s, buffer(8, 8, 0xff000000u, WL_SHM_FORMAT_XRGB8888), 0, 0);
    wl_surface_commit(s);
    expect_error("a chroma location with an RGB buffer", &wp_color_representation_surface_v1_interface, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_PIXEL_FORMAT);

    connect_all();
    s = wl_compositor_create_surface(comp);
    rs = wp_color_representation_manager_v1_get_surface(crm, s);
    wl_surface_destroy(s);
    wp_color_representation_surface_v1_set_alpha_mode(rs, 0);
    expect_error("set_alpha_mode after the wl_surface is gone", &wp_color_representation_surface_v1_interface, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_INERT);
}

/* ---- show: the same pixels, linear light then undescribed ---- */

static int run_show(void) {
    connect_all();
    Img m;
    struct wp_image_description_v1 *ld = params(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, &m);
    Window w;
    window(&w, "color:first");              /* the session's first window takes longer to come up: not measured */
    show(&w, buffer(200, 100, 0xff102030u, WL_SHM_FORMAT_XRGB8888));
    pause_ms(2000);
    close_window(&w);
    window(&w, "color:linear");
    struct wp_color_management_surface_v1 *cs = wp_color_manager_v1_get_surface(cm, w.s);
    wp_color_management_surface_v1_set_image_description(cs, ld, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    show(&w, buffer(200, 100, 0xff804020u, WL_SHM_FORMAT_XRGB8888));
    pause_ms(3000);
    wp_color_management_surface_v1_destroy(cs);
    close_window(&w);
    window(&w, "color:plain");
    show(&w, buffer(200, 100, 0xff804020u, WL_SHM_FORMAT_XRGB8888));
    pause_ms(3000);
    close_window(&w);
    return no_error("show: no error") ? 0 : 1;
}

/* ---- held: a commit held behind the fifo barrier applies with the description it was committed with ---- */

static int run_held(void) {
    connect_all();
    if (!fifom) { printf("FAIL no wp_fifo_manager_v1\n"); return 1; }
    Img m;
    struct wp_image_description_v1 *ld = params(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, &m);
    Window w;
    window(&w, "color:first");              /* (the session's first window takes longer to come up) */
    show(&w, buffer(200, 100, 0xff102030u, WL_SHM_FORMAT_XRGB8888));
    pause_ms(2000);
    close_window(&w);
    window(&w, "color:held");
    struct wp_color_management_surface_v1 *cs = wp_color_manager_v1_get_surface(cm, w.s);
    struct wp_fifo_v1 *f = wp_fifo_manager_v1_get_fifo(fifom, w.s);
    struct wl_buffer *first = buffer(200, 100, 0xff102030u, WL_SHM_FORMAT_XRGB8888);
    struct wl_buffer *lin = buffer(200, 100, 0xff804020u, WL_SHM_FORMAT_XRGB8888);
    /* in one batch: 1 maps the window and sets the barrier; 2 (linear light) waits for it, so it is held; then the
     * description is unset, pending and never committed. 2 must still apply as linear light. */
    wp_fifo_v1_set_barrier(f);
    show(&w, first);
    wp_color_management_surface_v1_set_image_description(cs, ld, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    wp_fifo_v1_wait_barrier(f);
    show(&w, lin);
    wp_color_management_surface_v1_unset_image_description(cs);
    wl_display_flush(dpy);
    pause_ms(3000);
    close_window(&w);
    return no_error("held: no error") ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "protocols";
    int r;
    if (!strcmp(mode, "protocols")) { r = run_protocols(); run_errors(); r |= fails != 0; }
    else if (!strcmp(mode, "show")) r = run_show();
    else if (!strcmp(mode, "held")) r = run_held();
    else { fprintf(stderr, "usage: color-client protocols|show|held\n"); return 2; }
    if (dpy) wl_display_disconnect(dpy);
    return r;
}
