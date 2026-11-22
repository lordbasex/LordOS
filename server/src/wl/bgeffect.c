/* bgeffect.c — ext-background-effect-v1: blur behind part of a window (docs/WAYLAND.md, "Blur behind windows").
 *
 * A translucent app (a terminal with a see-through background, a panel) asks for what is behind a region of its
 * window to be blurred. LordOS draws that with the same backdrop blur its own panels use (the UI's backdropBlur, with
 * its damage rules and cache, ui/paint.c): the shell puts blurred boxes right under the window's surface
 * (sdk/wayland.js), one per rectangle of the region (bgregion.h), clipped to the window geometry.
 *
 * The capability is told honestly: blur is offered only while it can really be drawn, which is on the CPU path
 * (there the windows are painted into the same canvas as the UI, so the blur sees what is under the window) and
 * with Settings → transparency with blur on. With the GPU compositing (windows are textures under the UI's layer, and
 * the GPU path has no blur yet) or the effects off, the capabilities are none and the regions are not drawn; they are
 * kept, and drawn again when the capability comes back. Every bound manager is told when it changes.
 *
 * Only a window's main surface (its xdg_toplevel) is blurred behind; a region on a popup or a subsurface is accepted
 * and kept, never drawn. */
#include "wlshell.h"
#include "bgregion.h"
#include "../gpu/gpu.h"
#include "ext-background-effect-v1-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>

#define FX_RECTS 16

typedef struct {
    int id;                             /* its surface */
    struct wl_listener surf_gone;
    struct wl_resource *res;            /* its ext_background_effect_surface_v1, NULL when destroyed */
    WlRgn pending, cur;
    int pending_set, has;
    int geom[4];                        /* the geometry the rects were cut for */
    int rects[FX_RECTS][4], n;
    char sent[600];                     /* the last event told for it ("" none) */
    struct wl_list link;
} SurfFx;

static struct wl_list fxs, mgrs;
static int effects = 1;                 /* Settings → transparency with blur (bgeffect_set_effects) */
static uint32_t caps_sent = UINT32_MAX;

static uint32_t caps(void) { return effects && !gpu_active() ? EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR : 0; }

static SurfFx *of(int id) {
    SurfFx *f;
    wl_list_for_each(f, &fxs, link) if (f->id == id) return f;
    return NULL;
}

/* Tells the shell the window's rects (none while blur can't be drawn), when they changed. */
static void tell(SurfFx *f) {
    int m = 0, c = 0, top = 0;
    if (!wl__surface_state(f->id, &m, &c, &top) || !m || !top) return;
    char j[600];
    size_t o = (size_t)snprintf(j, sizeof j, "{\"rects\":[");
    int n = caps() ? f->n : 0;
    for (int i = 0; i < n && o + 64 < sizeof j; i++)
        o += (size_t)snprintf(j + o, sizeof j - o, "%s[%d,%d,%d,%d]", i ? "," : "", f->rects[i][0], f->rects[i][1], f->rects[i][2], f->rects[i][3]);
    snprintf(j + o, sizeof j - o, "]}");
    if (!strcmp(j, f->sent)) return;
    memcpy(f->sent, j, sizeof j);
    wl__emit(f->id, "blur", j);
}

static void recut(SurfFx *f) {
    int g[4], lw = 0, lh = 0;
    if (!wl__surface_geom(f->id, g, &lw, &lh)) return;
    /* the region is clipped to the surface, then to the window geometry (a CSD shadow is never blurred behind) */
    int x0 = g[0] < 0 ? 0 : g[0], y0 = g[1] < 0 ? 0 : g[1];
    int x1 = g[0] + g[2] > lw ? lw : g[0] + g[2], y1 = g[1] + g[3] > lh ? lh : g[1] + g[3];
    int n = f->has && x1 > x0 && y1 > y0 ? bgr_rects(&f->cur, x0, y0, x1 - x0, y1 - y0, f->rects, FX_RECTS) : 0;
    /* relative to the geometry's corner, as the shell places the surface */
    for (int i = 0; i < n; i++) { f->rects[i][0] += x0 - g[0]; f->rects[i][1] += y0 - g[1]; }
    f->n = n;
    memcpy(f->geom, g, sizeof g);
}

static void free_fx(SurfFx *f) {
    if (f->res) wl_resource_set_user_data(f->res, NULL);
    wl_list_remove(&f->surf_gone.link);
    wl_list_remove(&f->link);
    wlrgn_free(&f->pending);
    wlrgn_free(&f->cur);
    free(f);
}
static void on_surf_gone(struct wl_listener *l, void *data) {
    (void)data;
    SurfFx *f = wl_container_of(l, f, surf_gone);
    wl_list_init(&f->surf_gone.link);
    free_fx(f);                         /* its object, if alive, is inert now */
}

static void fx_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void fx_set(struct wl_client *c, struct wl_resource *r, struct wl_resource *region) {
    SurfFx *f = wl_resource_get_user_data(r);
    if (!f) { wl_resource_post_error(r, EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED, "the surface is gone"); return; }
    static const WlRgn none;
    const WlRgn *from = region ? wl_resource_get_user_data(region) : &none;
    if (wlrgn_copy(&f->pending, from ? from : &none)) { wl_client_post_no_memory(c); return; }
    f->pending_set = 1;
}
static const struct ext_background_effect_surface_v1_interface fx_impl = { .destroy = fx_destroy, .set_blur_region = fx_set };
static void fx_gone(struct wl_resource *r) {
    SurfFx *f = wl_resource_get_user_data(r);
    if (!f) return;
    f->res = NULL;
    wlrgn_free(&f->pending);            /* "the effect regions will be removed on the next commit" */
    f->pending_set = 1;
}

static void mgr_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void mgr_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    int sid = wl__surface_id(surface);
    SurfFx *f = sid ? of(sid) : NULL;
    if (f && f->res) {
        wl_resource_post_error(r, EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS, "the surface already has a background effect object");
        return;
    }
    struct wl_resource *res = wl_resource_create(c, &ext_background_effect_surface_v1_interface, wl_resource_get_version(r), id);
    if (!res) { wl_client_post_no_memory(c); return; }
    if (!f && sid) {
        f = calloc(1, sizeof *f);
        if (!f) { wl_resource_destroy(res); wl_client_post_no_memory(c); return; }
        f->id = sid;
        f->surf_gone.notify = on_surf_gone;
        wl_resource_add_destroy_listener(surface, &f->surf_gone);
        wl_list_insert(&fxs, &f->link);
    }
    if (f) f->res = res;
    wl_resource_set_implementation(res, &fx_impl, f, fx_gone);     /* f NULL: an inert object */
}
static const struct ext_background_effect_manager_v1_interface mgr_impl = { .destroy = mgr_destroy, .get_background_effect = mgr_get };
static void mgr_unlink(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }
static void bind_mgr(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &ext_background_effect_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &mgr_impl, NULL, mgr_unlink);
    wl_list_insert(&mgrs, wl_resource_get_link(r));
    ext_background_effect_manager_v1_send_capabilities(r, caps());
}

void bgeffect_globals(struct wl_display *dpy) {
    wl_list_init(&fxs);
    wl_list_init(&mgrs);
    caps_sent = caps();
    wl_global_create(dpy, &ext_background_effect_manager_v1_interface, 1, NULL, bind_mgr);
}

void bgeffect_commit(int id) {
    SurfFx *f = of(id);
    if (!f) return;
    int g[4], lw, lh;
    int moved = wl__surface_geom(id, g, &lw, &lh) && memcmp(g, f->geom, sizeof g);
    if (!f->pending_set && !moved) return;
    if (f->pending_set) {
        WlRgn t = f->cur;               /* the pending region becomes the current one */
        f->cur = f->pending;
        f->pending = t;
        wlrgn_free(&f->pending);
        f->has = f->cur.n > 0;
        f->pending_set = 0;
    }
    recut(f);
    tell(f);
}

void bgeffect_mapped(int id) {
    SurfFx *f = of(id);
    if (f) { recut(f); f->sent[0] = 0; tell(f); }
}

void bgeffect_caps_changed(void) {
    uint32_t c = caps();
    if (c == caps_sent) return;
    caps_sent = c;
    struct wl_resource *r;
    wl_resource_for_each(r, &mgrs) ext_background_effect_manager_v1_send_capabilities(r, c);
    SurfFx *f;
    wl_list_for_each(f, &fxs, link) tell(f);    /* drawn again, or taken away */
    fprintf(stderr, "wayland: background blur %s\n", c ? "offered" : "not offered (GPU path or effects off)");
}

void bgeffect_set_effects(int on) {
    effects = !!on;
    bgeffect_caps_changed();
}
