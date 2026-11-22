/* wlcolor.c — color-management-v1 and color-representation-v1 (see wlcolor.h). */
#include "wlcolor.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server.h>
#include "color-management-v1-protocol.h"
#include "color-representation-v1-protocol.h"

/* ---- image description records: immutable, shared by every object that names the same description ---- */

typedef struct Desc {
    struct Desc *next;
    int refs;
    uint32_t id;                    /* identity sent in ready and preferred_changed */
    struct DescKey {
        uint32_t tf_named;          /* protocol enum, or 0 for a power curve */
        uint32_t power;             /* exponent ×10000 (tf_power) */
        uint32_t prim_named;        /* protocol enum, or 0 for set_primaries */
        int32_t prim[8];            /* ×1,000,000 */
        int32_t target[8];          /* target primaries ×1,000,000 (outputs: the EDID's; else the primaries) */
        uint32_t min_lum, max_lum, ref_lum;     /* min ×10000 */
        uint32_t max_cll, max_fall; /* 0 = not set */
    } k;
    CcDesc cc;
} Desc;

static Desc *descs;
static uint32_t next_id = 1;

static Desc *desc_ref(Desc *d) { if (d) d->refs++; return d; }
static void desc_unref(Desc *d) {
    if (!d || --d->refs > 0) return;
    for (Desc **p = &descs; *p; p = &(*p)->next)
        if (*p == d) { *p = d->next; break; }
    free(d);
}

static const struct { uint32_t name; int32_t p[8]; } named_prim[] = {
    { WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, { 640000, 330000, 300000, 600000, 150000, 60000, 312700, 329000 } },
    { WP_COLOR_MANAGER_V1_PRIMARIES_PAL_M, { 670000, 330000, 210000, 710000, 140000, 80000, 310000, 316000 } },
    { WP_COLOR_MANAGER_V1_PRIMARIES_PAL, { 640000, 330000, 290000, 600000, 150000, 60000, 312700, 329000 } },
    { WP_COLOR_MANAGER_V1_PRIMARIES_NTSC, { 630000, 340000, 310000, 595000, 155000, 70000, 312700, 329000 } },
    { WP_COLOR_MANAGER_V1_PRIMARIES_GENERIC_FILM, { 681000, 319000, 243000, 692000, 145000, 49000, 310000, 316000 } },
    { WP_COLOR_MANAGER_V1_PRIMARIES_BT2020, { 708000, 292000, 170000, 797000, 131000, 46000, 312700, 329000 } },
    { WP_COLOR_MANAGER_V1_PRIMARIES_CIE1931_XYZ, { 1000000, 0, 0, 1000000, 0, 0, 333333, 333333 } },
    { WP_COLOR_MANAGER_V1_PRIMARIES_DCI_P3, { 680000, 320000, 265000, 690000, 150000, 60000, 314000, 351000 } },
    { WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3, { 680000, 320000, 265000, 690000, 150000, 60000, 312700, 329000 } },
    { WP_COLOR_MANAGER_V1_PRIMARIES_ADOBE_RGB, { 640000, 330000, 210000, 710000, 150000, 60000, 312700, 329000 } },
};
#define N_PRIM (sizeof named_prim / sizeof *named_prim)

static const struct { uint32_t name; int cc; } named_tf[] = {
    { WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22, CC_TF_GAMMA22 },
    { WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB, CC_TF_SRGB },
    { WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR, CC_TF_LINEAR },
    { WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886, CC_TF_BT1886 },
    { WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28, CC_TF_GAMMA28 },
};
#define N_TF (sizeof named_tf / sizeof *named_tf)

static const int32_t *prim_of(uint32_t name) {
    for (size_t i = 0; i < N_PRIM; i++) if (named_prim[i].name == name) return named_prim[i].p;
    return NULL;
}
static int tf_of(uint32_t name) {
    for (size_t i = 0; i < N_TF; i++) if (named_tf[i].name == name) return named_tf[i].cc;
    return -1;
}
static CcPrim prim_from(const int32_t p[8]) {
    return (CcPrim){ p[0] / 1e6, p[1] / 1e6, p[2] / 1e6, p[3] / 1e6, p[4] / 1e6, p[5] / 1e6, p[6] / 1e6, p[7] / 1e6 };
}

/* The record for these parameters: an existing one, or a new one; NULL when lord-server can't convert from it (or no
 * memory). The caller holds the reference it returns. */
static Desc *desc_get(const struct DescKey *k) {
    for (Desc *d = descs; d; d = d->next)
        if (!memcmp(&d->k, k, sizeof *k)) return desc_ref(d);
    CcDesc cc = { k->tf_named ? tf_of(k->tf_named) : CC_TF_POWER, k->power / 10000.0, prim_from(k->prim) };
    double m[9];
    if (cc.tf < 0 || cc_matrix(&cc.prim, m)) return NULL;
    Desc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->k = *k;
    d->cc = cc;
    d->refs = 1;
    d->id = next_id++;
    if (!next_id) next_id = 1;              /* (0 is never an identity) */
    d->next = descs;
    descs = d;
    return d;
}

/* ---- outputs ---- */

static struct { int used; Desc *d; struct wl_list res; } outs[WLC_OUT_MAX];
static int outs_init;

static void outs_ready(void) {
    if (outs_init) return;
    for (int i = 0; i < WLC_OUT_MAX; i++) wl_list_init(&outs[i].res);
    outs_init = 1;
}

/* sRGB primaries, gamma 2.2, the default SDR luminances (0.2 to 80 cd/m², reference white 80) */
static Desc *output_desc(const int chroma[8]) {
    struct DescKey k;
    memset(&k, 0, sizeof k);
    k.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22;
    k.prim_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
    memcpy(k.prim, prim_of(k.prim_named), sizeof k.prim);
    memcpy(k.target, k.prim, sizeof k.target);
    if (chroma) {
        CcPrim p = prim_from(chroma);
        double m[9];
        if (!cc_matrix(&p, m)) memcpy(k.target, chroma, sizeof k.target);  /* only a gamut that makes sense */
    }
    k.min_lum = 2000;
    k.max_lum = 80;
    k.ref_lum = 80;
    return desc_get(&k);
}

/* The description a surface on slot should use: that display's, else the first display's, else plain sRGB. */
static Desc *slot_desc(int slot) {
    if (slot >= 0 && slot < WLC_OUT_MAX && outs[slot].used && outs[slot].d) return outs[slot].d;
    for (int i = 0; i < WLC_OUT_MAX; i++) if (outs[i].used && outs[i].d) return outs[i].d;
    static Desc *plain;
    if (!plain) plain = output_desc(NULL);  /* kept for good */
    return plain;
}

/* ---- wp_image_description_v1 ---- */

typedef struct { Desc *d; int info; } Img;

static void img_gone(struct wl_resource *r) {
    Img *im = wl_resource_get_user_data(r);
    if (im) { desc_unref(im->d); free(im); }
}
static void img_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }

static void info_prim(struct wl_resource *i, const int32_t *p,
                      void (*send)(struct wl_resource *, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t)) {
    send(i, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
}

static void img_info(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    Img *im = wl_resource_get_user_data(r);
    if (!im || !im->d) { wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_V1_ERROR_NOT_READY, "the image description is not ready"); return; }
    if (!im->info) { wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_V1_ERROR_NO_INFORMATION, "get_information is not allowed for it"); return; }
    struct wl_resource *i = wl_resource_create(c, &wp_image_description_info_v1_interface, wl_resource_get_version(r), id);
    if (!i) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(i, NULL, NULL, NULL);
    const struct DescKey *k = &im->d->k;
    info_prim(i, k->prim, wp_image_description_info_v1_send_primaries);
    if (k->prim_named) wp_image_description_info_v1_send_primaries_named(i, k->prim_named);
    if (k->tf_named) wp_image_description_info_v1_send_tf_named(i, k->tf_named);
    else wp_image_description_info_v1_send_tf_power(i, k->power);
    wp_image_description_info_v1_send_luminances(i, k->min_lum, k->max_lum, k->ref_lum);
    info_prim(i, k->target, wp_image_description_info_v1_send_target_primaries);
    wp_image_description_info_v1_send_target_luminance(i, k->min_lum, k->max_lum);
    if (k->max_cll) wp_image_description_info_v1_send_target_max_cll(i, k->max_cll);
    if (k->max_fall) wp_image_description_info_v1_send_target_max_fall(i, k->max_fall);
    wp_image_description_info_v1_send_done(i);      /* (a destructor event) */
    wl_resource_destroy(i);
}
static const struct wp_image_description_v1_interface img_impl = { .destroy = img_destroy, .get_information = img_info };

/* A new image description object: ready with d (a reference the caller gives it), or failed with why when d is
 * NULL. info: get_information is allowed (outputs' and preferred ones). */
static void img_new(struct wl_client *c, int ver, uint32_t id, Desc *d, int info, uint32_t cause, const char *why) {
    struct wl_resource *r = wl_resource_create(c, &wp_image_description_v1_interface, ver, id);
    Img *im = r ? calloc(1, sizeof *im) : NULL;
    if (!im) {
        if (r) wl_resource_destroy(r);
        desc_unref(d);
        wl_client_post_no_memory(c);
        return;
    }
    im->d = d;
    im->info = info;
    wl_resource_set_implementation(r, &img_impl, im, img_gone);
    if (d) wp_image_description_v1_send_ready(r, d->id);
    else wp_image_description_v1_send_failed(r, cause, why);
}

/* ---- wp_color_management_output_v1 ---- */

static void cmo_gone(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }
static void cmo_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void cmo_get(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    intptr_t slot = (intptr_t)wl_resource_get_user_data(r) - 1;
    if (slot < 0 || slot >= WLC_OUT_MAX || !outs[slot].used || !outs[slot].d)
        img_new(c, wl_resource_get_version(r), id, NULL, 0, WP_IMAGE_DESCRIPTION_V1_CAUSE_NO_OUTPUT, "the display is gone");
    else img_new(c, wl_resource_get_version(r), id, desc_ref(outs[slot].d), 1, 0, NULL);
}
static const struct wp_color_management_output_v1_interface cmo_impl = { .destroy = cmo_destroy, .get_image_description = cmo_get };

/* ---- per surface: both protocols' state ---- */

typedef struct CmSurf {
    struct wl_list link;
    struct wl_listener gone;
    struct wl_resource *surface, *cm, *cr;
    struct wl_list fbs;                 /* wp_color_management_surface_feedback_v1 resources */
    Desc *p_desc, *desc;                /* the image description pending and current (NULL: none, as sRGB) */
    int p_desc_set;
    int p_alpha, alpha, p_chroma, chroma, p_cr_set;
    CcConv *conv;
    int slot;
    uint32_t pref_id;                   /* the preferred description's identity last told (0: none yet) */
} CmSurf;

static struct wl_list cmsurfs;

static void cs_surface_gone(struct wl_listener *l, void *data);

static CmSurf *cs_find(struct wl_resource *surface) {
    struct wl_listener *l = surface ? wl_resource_get_destroy_listener(surface, cs_surface_gone) : NULL;
    return l ? wl_container_of(l, (CmSurf *)NULL, gone) : NULL;
}

static CmSurf *cs_get(struct wl_resource *surface) {
    CmSurf *s = cs_find(surface);
    if (s) return s;
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->surface = surface;
    s->slot = -1;
    wl_list_init(&s->fbs);
    s->gone.notify = cs_surface_gone;
    wl_resource_add_destroy_listener(surface, &s->gone);
    wl_list_insert(&cmsurfs, &s->link);
    return s;
}

static void cs_surface_gone(struct wl_listener *l, void *data) {
    (void)data;
    CmSurf *s = wl_container_of(l, s, gone);
    wl_list_remove(&s->gone.link);
    wl_list_remove(&s->link);
    if (s->cm) wl_resource_set_user_data(s->cm, NULL);     /* inert */
    if (s->cr) wl_resource_set_user_data(s->cr, NULL);
    struct wl_resource *r, *t;
    wl_resource_for_each_safe(r, t, &s->fbs) {
        wl_resource_set_user_data(r, NULL);
        wl_list_remove(wl_resource_get_link(r));
        wl_list_init(wl_resource_get_link(r));
    }
    desc_unref(s->p_desc);
    desc_unref(s->desc);
    cc_conv_free(s->conv);
    free(s);
}

static void cs_preferred(CmSurf *s) {
    Desc *d = slot_desc(s->slot);
    uint32_t id = d ? d->id : 0;
    if (!id || id == s->pref_id) return;
    int first = !s->pref_id;
    s->pref_id = id;
    if (first) return;                      /* (the first one a client asks for itself: get_preferred) */
    struct wl_resource *r;
    wl_resource_for_each(r, &s->fbs) wp_color_management_surface_feedback_v1_send_preferred_changed(r, id);
}

/* wp_color_management_surface_v1 */
static void cms_gone(struct wl_resource *r) {
    CmSurf *s = wl_resource_get_user_data(r);
    if (!s) return;
    s->cm = NULL;
    desc_unref(s->p_desc);                  /* destroy = unset_image_description, from the next commit */
    s->p_desc = NULL;
    s->p_desc_set = 1;
}
static void cms_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void cms_set(struct wl_client *c, struct wl_resource *r, struct wl_resource *desc, uint32_t intent) {
    (void)c;
    CmSurf *s = wl_resource_get_user_data(r);
    if (!s) { wl_resource_post_error(r, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT, "the wl_surface is gone"); return; }
    Img *im = wl_resource_get_user_data(desc);
    if (!im || !im->d) { wl_resource_post_error(r, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_IMAGE_DESCRIPTION, "the image description is not ready"); return; }
    if (intent != WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL && intent != WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE) {
        wl_resource_post_error(r, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_RENDER_INTENT, "rendering intent %u is not supported", intent);
        return;
    }
    desc_unref(s->p_desc);
    s->p_desc = desc_ref(im->d);
    s->p_desc_set = 1;
}
static void cms_unset(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    CmSurf *s = wl_resource_get_user_data(r);
    if (!s) { wl_resource_post_error(r, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT, "the wl_surface is gone"); return; }
    desc_unref(s->p_desc);
    s->p_desc = NULL;
    s->p_desc_set = 1;
}
static const struct wp_color_management_surface_v1_interface cms_impl = { .destroy = cms_destroy, .set_image_description = cms_set,
                                                                          .unset_image_description = cms_unset };

/* wp_color_management_surface_feedback_v1 */
static void fb_gone(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }
static void fb_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void fb_get(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    CmSurf *s = wl_resource_get_user_data(r);
    if (!s) { wl_resource_post_error(r, WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_ERROR_INERT, "the wl_surface is gone"); return; }
    Desc *d = slot_desc(s->slot);
    if (d && !s->pref_id) s->pref_id = d->id;
    img_new(c, wl_resource_get_version(r), id, desc_ref(d), 1, WP_IMAGE_DESCRIPTION_V1_CAUSE_OPERATING_SYSTEM, "no memory");
}
static const struct wp_color_management_surface_feedback_v1_interface fb_impl = { .destroy = fb_destroy, .get_preferred = fb_get,
                                                                                  .get_preferred_parametric = fb_get };

/* ---- wp_image_description_creator_params_v1 ---- */

typedef struct { struct DescKey k; int tf_set, prim_set; } Params;

static void par_gone(struct wl_resource *r) { free(wl_resource_get_user_data(r)); }
static void par_create(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    Params *p = wl_resource_get_user_data(r);
    if (!p->tf_set || !p->prim_set) {
        wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET, "the transfer function and the primaries are both needed");
        return;
    }
    /* the mastering luminances are the primary volume's: the default ones of the transfer function */
    double minl = p->k.tf_named == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886 ? 0.01 : 0.2;
    uint32_t maxl = p->k.tf_named == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886 ? 100 : 80;
    if ((p->k.max_cll && (p->k.max_cll <= minl || p->k.max_cll > maxl)) || (p->k.max_fall && (p->k.max_fall <= minl || p->k.max_fall > maxl)) ||
        (p->k.max_cll && p->k.max_fall && p->k.max_fall > p->k.max_cll)) {
        wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE, "max_cll or max_fall out of the luminance range");
        return;
    }
    memcpy(p->k.target, p->k.prim, sizeof p->k.target);
    p->k.min_lum = (uint32_t)(minl * 10000 + 0.5);
    p->k.max_lum = maxl;
    p->k.ref_lum = maxl;
    Desc *d = desc_get(&p->k);
    img_new(c, wl_resource_get_version(r), id, d, 0, WP_IMAGE_DESCRIPTION_V1_CAUSE_UNSUPPORTED, "lord-server can't convert from these primaries");
    wl_resource_destroy(r);
}
static void par_tf_named(struct wl_client *c, struct wl_resource *r, uint32_t tf) {
    (void)c;
    Params *p = wl_resource_get_user_data(r);
    if (p->tf_set) { wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET, "the transfer function is set"); return; }
    if (tf_of(tf) < 0) { wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF, "transfer function %u is not supported", tf); return; }
    p->k.tf_named = tf;
    p->tf_set = 1;
}
static void par_tf_power(struct wl_client *c, struct wl_resource *r, uint32_t eexp) {
    (void)c;
    Params *p = wl_resource_get_user_data(r);
    if (p->tf_set) { wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET, "the transfer function is set"); return; }
    if (eexp < 10000 || eexp > 100000) { wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF, "exponent %u/10000 is out of 1..10", eexp); return; }
    p->k.power = eexp;
    p->tf_set = 1;
}
static void par_prim_named(struct wl_client *c, struct wl_resource *r, uint32_t prim) {
    (void)c;
    Params *p = wl_resource_get_user_data(r);
    if (p->prim_set) { wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET, "the primaries are set"); return; }
    const int32_t *v = prim_of(prim);
    if (!v) { wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_PRIMARIES_NAMED, "primaries %u are not supported", prim); return; }
    p->k.prim_named = prim;
    memcpy(p->k.prim, v, sizeof p->k.prim);
    p->prim_set = 1;
}
static void par_prim(struct wl_client *c, struct wl_resource *r, int32_t rx, int32_t ry, int32_t gx, int32_t gy, int32_t bx, int32_t by,
                     int32_t wx, int32_t wy) {
    (void)c;
    Params *p = wl_resource_get_user_data(r);
    if (p->prim_set) { wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET, "the primaries are set"); return; }
    int32_t v[8] = { rx, ry, gx, gy, bx, by, wx, wy };
    p->k.prim_named = 0;
    memcpy(p->k.prim, v, sizeof v);
    p->prim_set = 1;
}
static void par_unsupported(struct wl_resource *r) {
    wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE, "LordOS has no HDR output: luminances and mastering metadata are not supported");
}
static void par_lum(struct wl_client *c, struct wl_resource *r, uint32_t a, uint32_t b, uint32_t d) { (void)c; (void)a; (void)b; (void)d; par_unsupported(r); }
static void par_mprim(struct wl_client *c, struct wl_resource *r, int32_t a, int32_t b, int32_t d, int32_t e, int32_t f, int32_t g, int32_t h, int32_t i) {
    (void)c; (void)a; (void)b; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i;
    par_unsupported(r);
}
static void par_mlum(struct wl_client *c, struct wl_resource *r, uint32_t a, uint32_t b) { (void)c; (void)a; (void)b; par_unsupported(r); }
static void par_cll(struct wl_client *c, struct wl_resource *r, uint32_t v) { (void)c; ((Params *)wl_resource_get_user_data(r))->k.max_cll = v; }
static void par_fall(struct wl_client *c, struct wl_resource *r, uint32_t v) { (void)c; ((Params *)wl_resource_get_user_data(r))->k.max_fall = v; }
static const struct wp_image_description_creator_params_v1_interface par_impl = {
    .create = par_create, .set_tf_named = par_tf_named, .set_tf_power = par_tf_power, .set_primaries_named = par_prim_named,
    .set_primaries = par_prim, .set_luminances = par_lum, .set_mastering_display_primaries = par_mprim,
    .set_mastering_luminance = par_mlum, .set_max_cll = par_cll, .set_max_fall = par_fall,
};

/* ---- wp_color_manager_v1 ---- */

static void cm_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void cm_get_output(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *output) {
    struct wl_resource *o = wl_resource_create(c, &wp_color_management_output_v1_interface, wl_resource_get_version(r), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    intptr_t slot = (intptr_t)wl_resource_get_user_data(output) - 1;   /* wl.c's slot + 1, 0 once gone */
    if (slot < 0 || slot >= WLC_OUT_MAX || !outs[slot].used) {
        wl_resource_set_implementation(o, &cmo_impl, NULL, NULL);    /* inert: get_image_description fails */
        return;
    }
    wl_resource_set_implementation(o, &cmo_impl, (void *)(slot + 1), cmo_gone);
    wl_list_insert(&outs[slot].res, wl_resource_get_link(o));
}
static void cm_get_surface(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    CmSurf *s = cs_get(surface);
    if (!s) { wl_client_post_no_memory(c); return; }
    if (s->cm) { wl_resource_post_error(r, WP_COLOR_MANAGER_V1_ERROR_SURFACE_EXISTS, "the wl_surface has one"); return; }
    struct wl_resource *m = wl_resource_create(c, &wp_color_management_surface_v1_interface, wl_resource_get_version(r), id);
    if (!m) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(m, &cms_impl, s, cms_gone);
    s->cm = m;
}
static void cm_get_feedback(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    CmSurf *s = cs_get(surface);
    struct wl_resource *f = s ? wl_resource_create(c, &wp_color_management_surface_feedback_v1_interface, wl_resource_get_version(r), id) : NULL;
    if (!f) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(f, &fb_impl, s, fb_gone);
    wl_list_insert(&s->fbs, wl_resource_get_link(f));
}
static void cm_icc(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    (void)c; (void)id;
    wl_resource_post_error(r, WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE, "ICC profiles are not supported");
}
static void cm_params(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *p = wl_resource_create(c, &wp_image_description_creator_params_v1_interface, wl_resource_get_version(r), id);
    Params *pp = p ? calloc(1, sizeof *pp) : NULL;
    if (!pp) { if (p) wl_resource_destroy(p); wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(p, &par_impl, pp, par_gone);
}
static void cm_scrgb(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    (void)c; (void)id;
    wl_resource_post_error(r, WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE, "Windows-scRGB is not supported (no HDR output)");
}
static const struct wp_color_manager_v1_interface cm_impl = {
    .destroy = cm_destroy, .get_output = cm_get_output, .get_surface = cm_get_surface, .get_surface_feedback = cm_get_feedback,
    .create_icc_creator = cm_icc, .create_parametric_creator = cm_params, .create_windows_scrgb = cm_scrgb,
};

static void bind_cm(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wp_color_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &cm_impl, NULL, NULL);
    wp_color_manager_v1_send_supported_intent(r, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    wp_color_manager_v1_send_supported_intent(r, WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE);
    wp_color_manager_v1_send_supported_feature(r, WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC);
    wp_color_manager_v1_send_supported_feature(r, WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES);
    wp_color_manager_v1_send_supported_feature(r, WP_COLOR_MANAGER_V1_FEATURE_SET_TF_POWER);
    for (size_t i = 0; i < N_TF; i++) wp_color_manager_v1_send_supported_tf_named(r, named_tf[i].name);
    for (size_t i = 0; i < N_PRIM; i++) wp_color_manager_v1_send_supported_primaries_named(r, named_prim[i].name);
    wp_color_manager_v1_send_done(r);
}

/* ---- wp_color_representation_surface_v1 ---- */

static void crs_gone(struct wl_resource *r) {
    CmSurf *s = wl_resource_get_user_data(r);
    if (!s) return;
    s->cr = NULL;                           /* destroy unsets everything, from the next commit */
    s->p_alpha = CC_ALPHA_ELECTRICAL;
    s->p_chroma = 0;
    s->p_cr_set = 1;
}
static void crs_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void crs_alpha(struct wl_client *c, struct wl_resource *r, uint32_t mode) {
    (void)c;
    CmSurf *s = wl_resource_get_user_data(r);
    if (!s) { wl_resource_post_error(r, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_INERT, "the wl_surface is gone"); return; }
    if (mode > WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_STRAIGHT) {
        wl_resource_post_error(r, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_ALPHA_MODE, "alpha mode %u is not supported", mode);
        return;
    }
    s->p_alpha = (int)mode;                 /* the protocol's values are colorconv's CC_ALPHA_* */
    s->p_cr_set = 1;
}
static void crs_coef(struct wl_client *c, struct wl_resource *r, uint32_t coef, uint32_t range) {
    (void)c;
    CmSurf *s = wl_resource_get_user_data(r);
    if (!s) { wl_resource_post_error(r, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_INERT, "the wl_surface is gone"); return; }
    if (coef != WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY || range != WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL) {
        wl_resource_post_error(r, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_COEFFICIENTS,
                               "only identity with full range: lord-server imports no YUV buffers");
        return;
    }
    /* identity and full range is how every RGB buffer is read already: nothing changes */
}
static void crs_chroma(struct wl_client *c, struct wl_resource *r, uint32_t loc) {
    (void)c;
    CmSurf *s = wl_resource_get_user_data(r);
    if (!s) { wl_resource_post_error(r, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_INERT, "the wl_surface is gone"); return; }
    s->p_chroma = (int)loc;
    s->p_cr_set = 1;
}
static const struct wp_color_representation_surface_v1_interface crs_impl = { .destroy = crs_destroy, .set_alpha_mode = crs_alpha,
                                                                              .set_coefficients_and_range = crs_coef,
                                                                              .set_chroma_location = crs_chroma };

static void crm_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void crm_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    CmSurf *s = cs_get(surface);
    if (!s) { wl_client_post_no_memory(c); return; }
    if (s->cr) { wl_resource_post_error(r, WP_COLOR_REPRESENTATION_MANAGER_V1_ERROR_SURFACE_EXISTS, "the wl_surface has one"); return; }
    struct wl_resource *m = wl_resource_create(c, &wp_color_representation_surface_v1_interface, wl_resource_get_version(r), id);
    if (!m) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(m, &crs_impl, s, crs_gone);
    s->cr = m;
}
static const struct wp_color_representation_manager_v1_interface crm_impl = { .destroy = crm_destroy, .get_surface = crm_get };

static void bind_crm(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wp_color_representation_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &crm_impl, NULL, NULL);
    for (uint32_t m = WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_PREMULTIPLIED_ELECTRICAL; m <= WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_STRAIGHT; m++)
        wp_color_representation_manager_v1_send_supported_alpha_mode(r, m);
    wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(r, WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY,
                                                                              WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL);
    wp_color_representation_manager_v1_send_done(r);
}

/* ---- the hooks wl.c calls ---- */

void wlcolor_init(struct wl_display *dpy) {
    outs_ready();
    wl_list_init(&cmsurfs);
    wl_global_create(dpy, &wp_color_manager_v1_interface, 1, NULL, bind_cm);
    wl_global_create(dpy, &wp_color_representation_manager_v1_interface, 1, NULL, bind_crm);
}

int wlcolor_commit(struct wl_resource *surface, int has_buffer) {
    CmSurf *s = cs_find(surface);
    if (!s) return 0;
    int changed = 0;
    if (s->p_desc_set) {
        if (s->p_desc != s->desc) changed = 1;
        desc_unref(s->desc);
        s->desc = s->p_desc;                /* (the pending reference moves) */
        s->p_desc = desc_ref(s->desc);      /* pending stays what was set until it is set again */
        s->p_desc_set = 0;
    }
    if (s->p_cr_set) {
        if (s->p_alpha != s->alpha) changed = 1;
        s->alpha = s->p_alpha;
        s->chroma = s->p_chroma;
        s->p_cr_set = 0;
    }
    /* every buffer lord-server takes is RGB: a chroma location (4:2:0 YCbCr only) can't go with it */
    if (s->chroma && has_buffer && s->cr) {
        wl_resource_post_error(s->cr, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_PIXEL_FORMAT, "a chroma location with an RGB buffer");
        return -1;
    }
    if (!changed) return 0;
    cc_conv_free(s->conv);
    CcDesc srgb = { CC_TF_GAMMA22, 0, cc_srgb };
    s->conv = cc_conv_new(s->desc ? &s->desc->cc : &srgb, s->alpha);
    return 1;
}

void wlcolor_pend_take(struct wl_resource *surface, WlcPend *p) {
    CmSurf *s = cs_find(surface);
    memset(p, 0, sizeof *p);
    if (!s) return;
    p->has = 1;
    p->desc = desc_ref(s->p_desc);          /* (a reference of its own: the pending one stays the surface's) */
    p->desc_set = s->p_desc_set;
    p->alpha = s->p_alpha;
    p->chroma = s->p_chroma;
    p->cr_set = s->p_cr_set;
    s->p_desc_set = s->p_cr_set = 0;
}

void wlcolor_pend_put(struct wl_resource *surface, WlcPend *p) {
    CmSurf *s = p->has ? cs_find(surface) : NULL;
    if (s) {
        desc_unref(s->p_desc);
        s->p_desc = p->desc;                /* (its reference moves) */
        p->desc = NULL;
        s->p_desc_set = p->desc_set;
        s->p_alpha = p->alpha;
        s->p_chroma = p->chroma;
        s->p_cr_set = p->cr_set;
    }
    wlcolor_pend_free(p);
}

void wlcolor_pend_free(WlcPend *p) {
    desc_unref(p->desc);
    p->desc = NULL;
    p->has = 0;
}

const CcConv *wlcolor_conv(struct wl_resource *surface) {
    CmSurf *s = cs_find(surface);
    return s ? s->conv : NULL;
}

void wlcolor_set_output(int slot, const int chroma[8]) {
    outs_ready();
    if (slot < 0 || slot >= WLC_OUT_MAX) return;
    Desc *d = output_desc(chroma), *old = outs[slot].d;
    outs[slot].used = 1;
    outs[slot].d = d;
    if (old && d != old) {
        struct wl_resource *r;
        wl_resource_for_each(r, &outs[slot].res) wp_color_management_output_v1_send_image_description_changed(r);
    }
    desc_unref(old);
    if (cmsurfs.next) {
        CmSurf *s;
        wl_list_for_each(s, &cmsurfs, link) cs_preferred(s);
    }
}

void wlcolor_output_gone(int slot) {
    outs_ready();
    if (slot < 0 || slot >= WLC_OUT_MAX || !outs[slot].used) return;
    struct wl_resource *r, *t;
    wl_resource_for_each_safe(r, t, &outs[slot].res) {     /* inert */
        wl_resource_set_user_data(r, NULL);
        wl_list_remove(wl_resource_get_link(r));
        wl_list_init(wl_resource_get_link(r));
    }
    desc_unref(outs[slot].d);
    outs[slot].d = NULL;
    outs[slot].used = 0;
    if (cmsurfs.next) {
        CmSurf *s;
        wl_list_for_each(s, &cmsurfs, link) cs_preferred(s);
    }
}

void wlcolor_surface_slot(struct wl_resource *surface, int slot) {
    CmSurf *s = cs_find(surface);
    if (!s || s->slot == slot) return;
    s->slot = slot;
    cs_preferred(s);
}
