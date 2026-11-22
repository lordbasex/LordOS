/* timing.c — see timing.h. The protocol objects and their double-buffered state; wl.c holds and applies updates. */
#include "timing.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "fifo-v1-protocol.h"
#include "commit-timing-v1-protocol.h"
#include "tearing-control-v1-protocol.h"
#include "content-type-v1-protocol.h"

static WlTiming **(*surface_slot)(struct wl_resource *surface);

/* the surface's state, made on first use; NULL when out of memory (posted) */
static WlTiming *of_surface(struct wl_client *c, struct wl_resource *surface) {
    WlTiming **slot = surface_slot(surface);
    if (!slot) return NULL;
    if (!*slot && !(*slot = calloc(1, sizeof **slot))) { wl_client_post_no_memory(c); return NULL; }
    return *slot;
}

static void res_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }

/* ---- wp_fifo_v1 ---- */

static void fifo_set(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    WlTiming *t = wl_resource_get_user_data(r);
    if (!t) { wl_resource_post_error(r, WP_FIFO_V1_ERROR_SURFACE_DESTROYED, "the surface was destroyed"); return; }
    t->p.set_barrier = 1;
}
static void fifo_wait(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    WlTiming *t = wl_resource_get_user_data(r);
    if (!t) { wl_resource_post_error(r, WP_FIFO_V1_ERROR_SURFACE_DESTROYED, "the surface was destroyed"); return; }
    t->p.wait_barrier = 1;
}
static const struct wp_fifo_v1_interface fifo_impl = { .set_barrier = fifo_set, .wait_barrier = fifo_wait, .destroy = res_destroy };
/* (what it set stays: "surface state changes previously made by this protocol are unaffected") */
static void fifo_gone(struct wl_resource *r) {
    WlTiming *t = wl_resource_get_user_data(r);
    if (t && t->fifo_r == r) t->fifo_r = NULL;
}
static void fifom_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    WlTiming *t = of_surface(c, surface);
    if (!t) return;
    if (t->fifo_r) { wl_resource_post_error(r, WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS, "the surface has a wp_fifo_v1"); return; }
    struct wl_resource *f = wl_resource_create(c, &wp_fifo_v1_interface, wl_resource_get_version(r), id);
    if (!f) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(f, &fifo_impl, t, fifo_gone);
    t->fifo_r = f;
}
static const struct wp_fifo_manager_v1_interface fifom_impl = { .destroy = res_destroy, .get_fifo = fifom_get };
static void bind_fifom(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wp_fifo_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &fifom_impl, NULL, NULL);
}

/* ---- wp_commit_timer_v1 ---- */

static void timer_set(struct wl_client *c, struct wl_resource *r, uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec) {
    (void)c;
    WlTiming *t = wl_resource_get_user_data(r);
    if (!t) { wl_resource_post_error(r, WP_COMMIT_TIMER_V1_ERROR_SURFACE_DESTROYED, "the surface was destroyed"); return; }
    if (nsec >= 1000000000u) { wl_resource_post_error(r, WP_COMMIT_TIMER_V1_ERROR_INVALID_TIMESTAMP, "tv_nsec %u", nsec); return; }
    if (t->p.has_target) { wl_resource_post_error(r, WP_COMMIT_TIMER_V1_ERROR_TIMESTAMP_EXISTS, "a timestamp is already set"); return; }
    uint64_t sec = (uint64_t)sec_hi << 32 | sec_lo;
    /* past what CLOCK_MONOTONIC can reach in nanoseconds (584 years): as far as it goes */
    t->p.target = sec >= UINT64_MAX / 1000000000ull - 1 ? UINT64_MAX : sec * 1000000000ull + nsec;
    t->p.has_target = 1;
}
static const struct wp_commit_timer_v1_interface timer_impl = { .set_timestamp = timer_set, .destroy = res_destroy };
static void timer_gone(struct wl_resource *r) {
    WlTiming *t = wl_resource_get_user_data(r);
    if (t && t->timer_r == r) t->timer_r = NULL;
}
static void timingm_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    WlTiming *t = of_surface(c, surface);
    if (!t) return;
    if (t->timer_r) {
        wl_resource_post_error(r, WP_COMMIT_TIMING_MANAGER_V1_ERROR_COMMIT_TIMER_EXISTS, "the surface has a wp_commit_timer_v1");
        return;
    }
    struct wl_resource *k = wl_resource_create(c, &wp_commit_timer_v1_interface, wl_resource_get_version(r), id);
    if (!k) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(k, &timer_impl, t, timer_gone);
    t->timer_r = k;
}
static const struct wp_commit_timing_manager_v1_interface timingm_impl = { .destroy = res_destroy, .get_timer = timingm_get };
static void bind_timingm(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wp_commit_timing_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &timingm_impl, NULL, NULL);
}

/* ---- wp_tearing_control_v1 ---- */

static void tear_hint(struct wl_client *c, struct wl_resource *r, uint32_t hint) {
    (void)c;
    WlTiming *t = wl_resource_get_user_data(r);
    if (!t) return;                     /* the surface is gone: inert */
    /* (an unknown value is taken as vsync, the default: the enum has no error for it) */
    t->p.hint = hint == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC ? WLT_HINT_ASYNC : WLT_HINT_VSYNC;
}
static const struct wp_tearing_control_v1_interface tear_impl = { .set_presentation_hint = tear_hint, .destroy = res_destroy };
/* destroyed: back to vsync with the next commit */
static void tear_gone(struct wl_resource *r) {
    WlTiming *t = wl_resource_get_user_data(r);
    if (!t || t->tear_r != r) return;
    t->tear_r = NULL;
    t->p.hint = WLT_HINT_VSYNC;
}
static void tearm_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    WlTiming *t = of_surface(c, surface);
    if (!t) return;
    if (t->tear_r) {
        wl_resource_post_error(r, WP_TEARING_CONTROL_MANAGER_V1_ERROR_TEARING_CONTROL_EXISTS, "the surface has a wp_tearing_control_v1");
        return;
    }
    struct wl_resource *k = wl_resource_create(c, &wp_tearing_control_v1_interface, wl_resource_get_version(r), id);
    if (!k) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(k, &tear_impl, t, tear_gone);
    t->tear_r = k;
}
static const struct wp_tearing_control_manager_v1_interface tearm_impl = { .destroy = res_destroy, .get_tearing_control = tearm_get };
static void bind_tearm(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wp_tearing_control_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &tearm_impl, NULL, NULL);
}

/* ---- wp_content_type_v1 ---- */

static void ct_set(struct wl_client *c, struct wl_resource *r, uint32_t type) {
    (void)c;
    WlTiming *t = wl_resource_get_user_data(r);
    if (!t) return;
    t->p.ctype = type <= WP_CONTENT_TYPE_V1_TYPE_GAME ? (int)type : WLT_CT_NONE;
}
static const struct wp_content_type_v1_interface ct_impl = { .destroy = res_destroy, .set_content_type = ct_set };
/* destroyed: "none", double-buffered like set_content_type */
static void ct_gone(struct wl_resource *r) {
    WlTiming *t = wl_resource_get_user_data(r);
    if (!t || t->ct_r != r) return;
    t->ct_r = NULL;
    t->p.ctype = WLT_CT_NONE;
}
static void ctm_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    WlTiming *t = of_surface(c, surface);
    if (!t) return;
    if (t->ct_r) {
        wl_resource_post_error(r, WP_CONTENT_TYPE_MANAGER_V1_ERROR_ALREADY_CONSTRUCTED, "the surface has a wp_content_type_v1");
        return;
    }
    struct wl_resource *k = wl_resource_create(c, &wp_content_type_v1_interface, wl_resource_get_version(r), id);
    if (!k) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(k, &ct_impl, t, ct_gone);
    t->ct_r = k;
}
static const struct wp_content_type_manager_v1_interface ctm_impl = { .destroy = res_destroy, .get_surface_content_type = ctm_get };
static void bind_ctm(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wp_content_type_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &ctm_impl, NULL, NULL);
}

/* ---- the surface's side (wl.c) ---- */

void wlt_globals(struct wl_display *d, WlTiming **(*slot)(struct wl_resource *surface)) {
    surface_slot = slot;
    wl_global_create(d, &wp_fifo_manager_v1_interface, 1, NULL, bind_fifom);
    wl_global_create(d, &wp_commit_timing_manager_v1_interface, 1, NULL, bind_timingm);
    wl_global_create(d, &wp_tearing_control_manager_v1_interface, 1, NULL, bind_tearm);
    wl_global_create(d, &wp_content_type_manager_v1_interface, 1, NULL, bind_ctm);
}

WltUpdate wlt_take(WlTiming *t) {
    WltUpdate u = t->p;
    t->p.set_barrier = t->p.wait_barrier = t->p.has_target = 0;     /* one commit's; hint and ctype stay as set */
    t->p.target = 0;
    return u;
}

void wlt_applied(WlTiming *t, const WltUpdate *u) {
    if (u->set_barrier) t->barrier = 1;
    t->hint = u->hint;
    t->ctype = u->ctype;
}

void wlt_gone(WlTiming *t) {
    if (!t) return;
    if (t->fifo_r) wl_resource_set_user_data(t->fifo_r, NULL);
    if (t->timer_r) wl_resource_set_user_data(t->timer_r, NULL);
    if (t->tear_r) wl_resource_set_user_data(t->tear_r, NULL);
    if (t->ct_r) wl_resource_set_user_data(t->ct_r, NULL);
    free(t);
}

/* ---- the displays' vblank grids ---- */

#define CLOCKS 32
static WltClock clocks[CLOCKS], any_clock;

void wlt_clock_set(int key, uint64_t ns, uint32_t refresh_ns) {
    WltClock c = { ns, refresh_ns };
    if (key < 0) { for (int i = 0; i < CLOCKS; i++) clocks[i] = c; any_clock = c; return; }
    if (key < CLOCKS) clocks[key] = c;
    any_clock = c;
}

const WltClock *wlt_clock(int key) { return key >= 0 && key < CLOCKS ? &clocks[key] : &any_clock; }
