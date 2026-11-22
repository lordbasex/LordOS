/* wltiming_client.c — fifo-v1, commit-timing-v1, tearing-control-v1 and content-type-v1 as a client sees them
 * (src/wl/timing.c, docs/WAYLAND.md "Frame timing"). One mode per run; tests/wltiming_headless.sh runs them against a
 * headless lord-server.
 *
 *   globals      the four globals, version 1
 *   fifo N [off] Mesa's FIFO present for N s: every commit sets the barrier and waits for the one before it, no frame
 *                callbacks, at most 3 commits waiting for their feedback. Prints the commits a second and how many
 *                were presented in the same refresh as the one before (must be 0; "off" leaves fifo out, as a control
 *                that shows the check can fail)
 *   timing       commits with a target time 100, 250 and 400 ms ahead: each presented at or after it (less 1/16 of
 *                a refresh) and within two refreshes after; a commit with no target behind one is not presented first
 *   errors K     one protocol error, K: fifo-twice, timer-twice, tearing-twice, ctype-twice, timestamp-twice,
 *                bad-nsec, fifo-destroyed, timer-destroyed; must end with that error
 *   destroy      updates held (a barrier, a target 10 s ahead) when the surface is destroyed: their feedback is
 *                discarded, the frame callbacks go, the connection survives
 *   nomap        a surface with no role (never on screen) using the barrier: never stalls
 *   hints        tearing "async" and content type "game", then "video", and both objects destroyed: no error
 *
 *   WAYLAND_DISPLAY=wayland-0 build/wltiming-client MODE [ARG]   (exit 0, or 1 on a failed check) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"
#include "client/presentation-time-client-protocol.h"
#include "client/fifo-v1-client-protocol.h"
#include "client/commit-timing-v1-client-protocol.h"
#include "client/tearing-control-v1-client-protocol.h"
#include "client/content-type-v1-client-protocol.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wmbase;
static struct wp_presentation *pres;
static struct wp_fifo_manager_v1 *fifom;
static struct wp_commit_timing_manager_v1 *timingm;
static struct wp_tearing_control_manager_v1 *tearm;
static struct wp_content_type_manager_v1 *ctm;
static uint32_t v_fifo, v_timing, v_tear, v_ct;
static int fails;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wmbase = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, wp_presentation_interface.name)) pres = wl_registry_bind(r, name, &wp_presentation_interface, 1);
    else if (!strcmp(iface, wp_fifo_manager_v1_interface.name)) { v_fifo = ver; fifom = wl_registry_bind(r, name, &wp_fifo_manager_v1_interface, 1); }
    else if (!strcmp(iface, wp_commit_timing_manager_v1_interface.name)) { v_timing = ver; timingm = wl_registry_bind(r, name, &wp_commit_timing_manager_v1_interface, 1); }
    else if (!strcmp(iface, wp_tearing_control_manager_v1_interface.name)) { v_tear = ver; tearm = wl_registry_bind(r, name, &wp_tearing_control_manager_v1_interface, 1); }
    else if (!strcmp(iface, wp_content_type_manager_v1_interface.name)) { v_ct = ver; ctm = wl_registry_bind(r, name, &wp_content_type_manager_v1_interface, 1); }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };

static int conf;
static uint32_t conf_serial;
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) { (void)d; (void)xs; conf++; conf_serial = serial; }
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)w; (void)h; (void)st; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, NULL, NULL };

typedef struct { struct wl_buffer *b; uint32_t *px; } Buf;
static Buf buffer(int W, int H, uint32_t color) {
    int stride = W * 4, size = stride * H;
    int fd = memfd_create("wltiming-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { perror("memfd"); exit(2); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int i = 0; i < W * H; i++) px[i] = color;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return (Buf){ b, px };
}

static struct wl_surface *surf;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *top;
#define W 640
#define H 480
static Buf bufs[3];

static void window(const char *title) {
    surf = wl_compositor_create_surface(comp);
    xsurf = xdg_wm_base_get_xdg_surface(wmbase, surf);
    xdg_surface_add_listener(xsurf, &xs_l, NULL);
    top = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(top, &top_l, NULL);
    xdg_toplevel_set_app_id(top, "wltiming-client");
    xdg_toplevel_set_title(top, title);
    wl_surface_commit(surf);
    wl_display_roundtrip(dpy);
    if (!conf) { printf("FAIL no configure\n"); exit(1); }
    xdg_surface_ack_configure(xsurf, conf_serial);
    for (int i = 0; i < 3; i++) bufs[i] = buffer(W, H, 0xff203040u + (uint32_t)i * 0x101010u);
    wl_surface_attach(surf, bufs[0].b, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, W, H);
    wl_surface_commit(surf);
    wl_display_roundtrip(dpy);
}

/* ---- presentation feedback, one record per commit ---- */
typedef struct { int state; uint64_t ns, target; uint32_t refresh; unsigned seq; } Fb;   /* state: 0 waiting, 1 presented, 2 discarded */
#define NFB 4096
static Fb fbs[NFB];
static int nfb, waiting;
static void fb_sync(void *d, struct wp_presentation_feedback *f, struct wl_output *o) { (void)d; (void)f; (void)o; }
static void fb_presented(void *d, struct wp_presentation_feedback *f, uint32_t sh, uint32_t sl, uint32_t ns, uint32_t refresh,
                         uint32_t qh, uint32_t ql, uint32_t flags) {
    (void)flags;
    Fb *b = d;
    b->state = 1;
    b->ns = ((uint64_t)sh << 32 | sl) * 1000000000ull + ns;
    b->refresh = refresh;
    b->seq = (unsigned)((uint64_t)qh << 32 | ql);
    waiting--;
    wp_presentation_feedback_destroy(f);
}
static void fb_discarded(void *d, struct wp_presentation_feedback *f) { Fb *b = d; b->state = 2; waiting--; wp_presentation_feedback_destroy(f); }
static const struct wp_presentation_feedback_listener fb_l = { fb_sync, fb_presented, fb_discarded };
static Fb *feedback(void) {
    if (nfb >= NFB) { printf("FAIL too many commits\n"); exit(1); }
    Fb *b = &fbs[nfb++];
    memset(b, 0, sizeof *b);
    wp_presentation_feedback_add_listener(wp_presentation_feedback(pres, surf), &fb_l, b);
    waiting++;
    return b;
}

static void draw(int n) {
    Buf *b = &bufs[n % 3];
    for (int i = 0; i < W * 8; i++) b->px[i] = 0xff000000u | (uint32_t)(n * 2654435761u);
    wl_surface_attach(surf, b->b, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, W, 8);
}

static int dispatch_until(uint64_t end) {
    while (now_ns() < end) {
        wl_display_flush(dpy);
        if (wl_display_roundtrip(dpy) < 0) return -1;
        usleep(1000);
    }
    return 0;
}

/* ---- fifo ---- */
static int run_fifo(int secs, int off) {
    window("fifo");
    struct wp_fifo_v1 *fifo = off ? NULL : wp_fifo_manager_v1_get_fifo(fifom, surf);
    uint64_t t0 = now_ns(), end = t0 + (uint64_t)secs * 1000000000ull;
    int n = 0;
    while (now_ns() < end) {
        while (waiting >= 3) if (wl_display_dispatch(dpy) < 0) { printf("FAIL connection lost\n"); return 1; }
        draw(n++);
        feedback();
        if (fifo) { wp_fifo_v1_set_barrier(fifo); wp_fifo_v1_wait_barrier(fifo); }
        wl_surface_commit(surf);
        wl_display_flush(dpy);
        wl_display_dispatch_pending(dpy);
    }
    double el = (double)(now_ns() - t0) / 1e9;
    uint64_t drain = now_ns() + 2000000000ull;
    while (waiting > 0 && now_ns() < drain) if (dispatch_until(now_ns() + 20000000ull) < 0) break;
    int presented = 0, discarded = 0, same = 0, late = 0;
    uint64_t prev = 0;
    for (int i = 0; i < nfb; i++) {
        if (fbs[i].state == 1) {
            presented++;
            if (prev && fbs[i].ns == prev) same++;          /* shown in the same refresh as the commit before it */
            if (prev && fbs[i].ns > prev + 3ull * (fbs[i].refresh ? fbs[i].refresh : 16666667u)) late++;   /* a stall */
            prev = fbs[i].ns;
        } else if (fbs[i].state == 2) discarded++;
    }
    printf("fifo%s: %d commits in %.2f s = %.1f per second; %d presented, %d discarded, %d still waiting; "
           "%d presented in the same refresh as the one before, %d gaps over 3 refreshes\n",
           off ? " (off)" : "", n, el, n / el, presented, discarded, waiting, same, late);
    if (wl_display_get_error(dpy)) { printf("FAIL protocol error %d\n", wl_display_get_error(dpy)); return 1; }
    if (off) return 0;
    if (same) { printf("FAIL %d updates shared a refresh with the one before\n", same); fails++; }
    if (waiting) { printf("FAIL %d updates never presented\n", waiting); fails++; }
    return fails ? 1 : 0;
}

/* ---- commit timing ---- */
static int run_timing(void) {
    window("timing");
    struct wp_commit_timer_v1 *timer = wp_commit_timing_manager_v1_get_timer(timingm, surf);
    dispatch_until(now_ns() + 300000000ull);
    static const int ahead_ms[] = { 100, 250, 400, 33, 17 };
    Fb *rec[8];
    int nrec = 0;
    for (unsigned i = 0; i < sizeof ahead_ms / sizeof *ahead_ms; i++) {
        uint64_t target = now_ns() + (uint64_t)ahead_ms[i] * 1000000ull, sec = target / 1000000000ull;
        draw((int)i + 1);
        Fb *b = feedback();
        b->target = target;
        rec[nrec++] = b;
        wp_commit_timer_v1_set_timestamp(timer, (uint32_t)(sec >> 32), (uint32_t)sec, (uint32_t)(target % 1000000000ull));
        wl_surface_commit(surf);
        /* behind it at once, with no target: never shown before it */
        if (i == 1) {
            draw(99);
            Fb *after = feedback();
            after->target = target;     /* (the one before it's) */
            rec[nrec++] = after;
            wl_surface_commit(surf);
        }
        wl_display_flush(dpy);
        uint64_t drain = now_ns() + 2000000000ull;
        while (waiting > 0 && now_ns() < drain) dispatch_until(now_ns() + 2000000ull);
    }
    for (int i = 0; i < nrec; i++) {
        Fb *b = rec[i];
        uint64_t r = b->refresh ? b->refresh : 16666667u;
        double d = ((double)b->ns - (double)b->target) / 1e6;
        printf("timing: commit %d %s at %+.2f ms from its target (refresh %.2f ms)\n", i, b->state == 1 ? "presented" : "NOT presented",
               d, r / 1e6);
        if (b->state != 1) { printf("FAIL commit %d not presented\n", i); fails++; continue; }
        if (b->ns + r / 16 < b->target) { printf("FAIL commit %d presented %.2f ms before its target\n", i, -d); fails++; }
        if (b->ns > b->target + 2 * r + 4000000ull) { printf("FAIL commit %d presented %.2f ms after its target\n", i, d); fails++; }
    }
    wp_commit_timer_v1_destroy(timer);
    if (wl_display_get_error(dpy)) { printf("FAIL protocol error\n"); return 1; }
    return fails ? 1 : 0;
}

/* ---- protocol errors ---- */
static int run_errors(const char *k) {
    struct wl_surface *s = wl_compositor_create_surface(comp);
    const struct wl_interface *want = NULL;
    uint32_t code = 0;
    if (!strcmp(k, "fifo-twice")) {
        wp_fifo_manager_v1_get_fifo(fifom, s);
        wp_fifo_manager_v1_get_fifo(fifom, s);
        want = &wp_fifo_manager_v1_interface; code = WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS;
    } else if (!strcmp(k, "timer-twice")) {
        wp_commit_timing_manager_v1_get_timer(timingm, s);
        wp_commit_timing_manager_v1_get_timer(timingm, s);
        want = &wp_commit_timing_manager_v1_interface; code = WP_COMMIT_TIMING_MANAGER_V1_ERROR_COMMIT_TIMER_EXISTS;
    } else if (!strcmp(k, "tearing-twice")) {
        wp_tearing_control_manager_v1_get_tearing_control(tearm, s);
        wp_tearing_control_manager_v1_get_tearing_control(tearm, s);
        want = &wp_tearing_control_manager_v1_interface; code = WP_TEARING_CONTROL_MANAGER_V1_ERROR_TEARING_CONTROL_EXISTS;
    } else if (!strcmp(k, "ctype-twice")) {
        wp_content_type_manager_v1_get_surface_content_type(ctm, s);
        wp_content_type_manager_v1_get_surface_content_type(ctm, s);
        want = &wp_content_type_manager_v1_interface; code = WP_CONTENT_TYPE_MANAGER_V1_ERROR_ALREADY_CONSTRUCTED;
    } else if (!strcmp(k, "timestamp-twice")) {
        struct wp_commit_timer_v1 *t = wp_commit_timing_manager_v1_get_timer(timingm, s);
        wp_commit_timer_v1_set_timestamp(t, 0, 1, 0);
        wp_commit_timer_v1_set_timestamp(t, 0, 2, 0);
        want = &wp_commit_timer_v1_interface; code = WP_COMMIT_TIMER_V1_ERROR_TIMESTAMP_EXISTS;
    } else if (!strcmp(k, "bad-nsec")) {
        struct wp_commit_timer_v1 *t = wp_commit_timing_manager_v1_get_timer(timingm, s);
        wp_commit_timer_v1_set_timestamp(t, 0, 1, 1000000000u);
        want = &wp_commit_timer_v1_interface; code = WP_COMMIT_TIMER_V1_ERROR_INVALID_TIMESTAMP;
    } else if (!strcmp(k, "fifo-destroyed")) {
        struct wp_fifo_v1 *f = wp_fifo_manager_v1_get_fifo(fifom, s);
        wl_surface_destroy(s);
        wp_fifo_v1_set_barrier(f);
        want = &wp_fifo_v1_interface; code = WP_FIFO_V1_ERROR_SURFACE_DESTROYED;
    } else if (!strcmp(k, "timer-destroyed")) {
        struct wp_commit_timer_v1 *t = wp_commit_timing_manager_v1_get_timer(timingm, s);
        wl_surface_destroy(s);
        wp_commit_timer_v1_set_timestamp(t, 0, 1, 0);
        want = &wp_commit_timer_v1_interface; code = WP_COMMIT_TIMER_V1_ERROR_SURFACE_DESTROYED;
    } else { printf("FAIL unknown error case %s\n", k); return 1; }
    wl_display_roundtrip(dpy);
    const struct wl_interface *iface = NULL;
    uint32_t id = 0, got = wl_display_get_protocol_error(dpy, &iface, &id);
    int ok = wl_display_get_error(dpy) && iface == want && got == code;
    printf("errors %s: %s (%s %u)\n", k, ok ? "ok" : "FAIL", iface ? iface->name : "no error", got);
    return ok ? 0 : 1;
}

/* ---- the surface destroyed with updates held ---- */
static int cb_done;
static void cb_fired(void *d, struct wl_callback *cb, uint32_t t) { (void)d; (void)t; cb_done++; wl_callback_destroy(cb); }
static const struct wl_callback_listener cb_l = { cb_fired };
static int run_destroy(void) {
    window("destroy");
    struct wp_fifo_v1 *fifo = wp_fifo_manager_v1_get_fifo(fifom, surf);
    struct wp_commit_timer_v1 *timer = wp_commit_timing_manager_v1_get_timer(timingm, surf);
    /* 1: sets the barrier; 2: waits for it; 3: 10 s ahead. 2 and 3 are held when the surface goes. */
    draw(1); Fb *a = feedback(); wp_fifo_v1_set_barrier(fifo); wl_surface_commit(surf);
    draw(2); Fb *b = feedback(); wp_fifo_v1_wait_barrier(fifo);
    wl_callback_add_listener(wl_surface_frame(surf), &cb_l, NULL);
    wl_surface_commit(surf);
    uint64_t t = now_ns() + 10000000000ull, sec = t / 1000000000ull;
    draw(3); Fb *c = feedback();
    wp_commit_timer_v1_set_timestamp(timer, (uint32_t)(sec >> 32), (uint32_t)sec, (uint32_t)(t % 1000000000ull));
    wl_callback_add_listener(wl_surface_frame(surf), &cb_l, NULL);
    wl_surface_commit(surf);
    xdg_toplevel_destroy(top);
    xdg_surface_destroy(xsurf);
    wl_surface_destroy(surf);
    dispatch_until(now_ns() + 300000000ull);
    printf("destroy: feedback %d %d %d (1 presented or discarded, 2 discarded), %d frame callbacks done\n", a->state, b->state, c->state, cb_done);
    wp_fifo_v1_destroy(fifo);
    wp_commit_timer_v1_destroy(timer);
    wl_display_roundtrip(dpy);
    if (wl_display_get_error(dpy)) { printf("FAIL protocol error\n"); return 1; }
    if (c->state != 2) { printf("FAIL the update held for its time was not discarded\n"); return 1; }
    if (b->state == 0) { printf("FAIL the update held behind the barrier got no feedback\n"); return 1; }
    return 0;
}

/* ---- a surface with no role ---- */
static int run_nomap(void) {
    surf = wl_compositor_create_surface(comp);
    bufs[0] = buffer(16, 16, 0xffff0000u);
    struct wp_fifo_v1 *fifo = wp_fifo_manager_v1_get_fifo(fifom, surf);
    int n = 30;
    uint64_t t0 = now_ns();
    for (int i = 0; i < n; i++) {
        wl_surface_attach(surf, bufs[0].b, 0, 0);
        wp_fifo_v1_set_barrier(fifo);
        wp_fifo_v1_wait_barrier(fifo);
        wl_callback_add_listener(wl_surface_frame(surf), &cb_l, NULL);
        wl_surface_commit(surf);
        wl_display_flush(dpy);
        uint64_t limit = now_ns() + 2000000000ull;
        while (cb_done < i + 1 && now_ns() < limit) dispatch_until(now_ns() + 1000000ull);
        if (cb_done < i + 1) { printf("FAIL nomap: stalled at commit %d\n", i); return 1; }
    }
    printf("nomap: %d commits in %.2f s, none stalled\n", n, (double)(now_ns() - t0) / 1e9);
    return wl_display_get_error(dpy) ? 1 : 0;
}

/* ---- hints ---- */
static int run_hints(void) {
    window("hints");
    struct wp_tearing_control_v1 *tc = wp_tearing_control_manager_v1_get_tearing_control(tearm, surf);
    struct wp_content_type_v1 *ct = wp_content_type_manager_v1_get_surface_content_type(ctm, surf);
    wp_tearing_control_v1_set_presentation_hint(tc, WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
    wp_content_type_v1_set_content_type(ct, WP_CONTENT_TYPE_V1_TYPE_GAME);
    draw(1); Fb *a = feedback(); wl_surface_commit(surf);
    wp_content_type_v1_set_content_type(ct, WP_CONTENT_TYPE_V1_TYPE_VIDEO);
    draw(2); Fb *b = feedback(); wl_surface_commit(surf);
    wp_tearing_control_v1_destroy(tc);
    wp_content_type_v1_destroy(ct);
    draw(3); Fb *c = feedback(); wl_surface_commit(surf);
    /* again after destroying them: a new object per surface is allowed */
    tc = wp_tearing_control_manager_v1_get_tearing_control(tearm, surf);
    ct = wp_content_type_manager_v1_get_surface_content_type(ctm, surf);
    wl_surface_commit(surf);
    uint64_t drain = now_ns() + 2000000000ull;
    while (waiting > 0 && now_ns() < drain) dispatch_until(now_ns() + 2000000ull);
    printf("hints: feedback %d %d %d (1 presented), flags kept vsync on a composed frame\n", a->state, b->state, c->state);
    if (wl_display_get_error(dpy)) { printf("FAIL protocol error\n"); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: wltiming-client globals|fifo N [off]|timing|errors K|destroy|nomap|hints\n"); return 2; }
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "wltiming-client: no Wayland display\n"); return 2; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (wmbase) xdg_wm_base_add_listener(wmbase, &wm_l, NULL);
    const char *m = argv[1];
    if (!strcmp(m, "globals")) {
        printf("globals: wp_fifo_manager_v1 %u, wp_commit_timing_manager_v1 %u, wp_tearing_control_manager_v1 %u, "
               "wp_content_type_manager_v1 %u\n", v_fifo, v_timing, v_tear, v_ct);
        return v_fifo == 1 && v_timing == 1 && v_tear == 1 && v_ct == 1 ? 0 : 1;
    }
    if (!comp || !shm || !wmbase || !pres || !fifom || !timingm || !tearm || !ctm) { printf("FAIL a global is missing\n"); return 1; }
    if (!strcmp(m, "fifo")) return run_fifo(argc > 2 ? atoi(argv[2]) : 3, argc > 3 && !strcmp(argv[3], "off"));
    if (!strcmp(m, "timing")) return run_timing();
    if (!strcmp(m, "errors") && argc > 2) return run_errors(argv[2]);
    if (!strcmp(m, "destroy")) return run_destroy();
    if (!strcmp(m, "nomap")) return run_nomap();
    if (!strcmp(m, "hints")) return run_hints();
    fprintf(stderr, "wltiming-client: unknown mode %s\n", m);
    return 2;
}
