/* paint.c — pinta el árbol de nodos en el canvas y resuelve qué nodo está bajo el cursor. */
#include "ui.h"
#include "../term/term.h"
#include "../wl/wl.h"
#include "../media/media.h"
#include "grab.h"
#include "../gpu/gpu.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

/* Perfil por primitiva (solo con --debug). */
int paint_profile;
static double prof[6];
static const char *const prof_name[6] = {"sombra", "blur", "fondo", "imagen", "texto", "borde"};
static double clk(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
#define PROF(i, call) do { double _t = paint_profile ? clk() : 0; call; if (paint_profile) prof[i] += clk() - _t; } while (0)

#define MAX_KIDS 1024

static char text_buf[8192];

/* The display being painted (docs/DISPLAYS.md): layout is in logical px, the canvas is that display's
 * physical frame. Every coordinate, radius, border, shadow, blur and font size goes through it, so
 * text and vectors are rasterized at the display's resolution (150 % is sharp, not upscaled). */
PaintView paint_view = { 0, 0, 1, 0, 0, 0 };
static inline float VX(float x) { return (x - paint_view.x) * paint_view.k + paint_view.dx; }
static inline float VY(float y) { return (y - paint_view.y) * paint_view.k + paint_view.dy; }

static uint32_t with_opacity(uint32_t argb, float op) {
    uint32_t a = (uint32_t)((argb >> 24) * op + 0.5f);
    return a << 24 | (argb & 0xffffff);
}

/* Cuadro de video que tapa todo lo que cambió (media_cover_node): lo anterior a ese nodo en el
 * orden de pintado no se ve, así que no se pinta (solo se recorre, por los recortes). */
static int skip_until, skip_reached;
static int paint_cull;                  /* paint_node may skip clipped subtrees outside the clip (CPU path) */

static int layer_paint(Canvas *c, Node *n, float opacity);
static int tf_paint(Canvas *c, Node *n, float opacity);
static int tf_self, tf_unit;    /* the transformed node being rasterized; the one painted at opacity 1 */
int paint_tf_nocache;           /* tests: transformed subtrees rasterized again, the cache untouched */

static float max4(const float r[4]) { return fmaxf(fmaxf(r[0], r[1]), fmaxf(r[2], r[3])); }

/* Lines that fit in the box (textOverflow: 'ellipsis' on wrapped text without numberOfLines). */
static int text_max_lines(const Node *n, float box_h) {
    const Style *s = &n->s;
    if (s->line_clamp) return s->line_clamp;
    if (!s->text_overflow || s->nowrap) return 0;
    float lh = text_line_height(s->font_size, s->line_height);
    int fit = lh > 0 ? (int)floorf((box_h + 0.5f) / lh) : 0;
    return fit < 1 ? 1 : fit;
}

static void paint_node(Canvas *c, Node *n, float opacity) {
    if (n->type == NODE_RAW || n->s.display_none) return;
    if (layer_paint(c, n, opacity)) return;
    if (n->own_tf && n->id != tf_self && tf_paint(c, n, opacity)) return;
    const Style *s = &n->s;
    float op = n->id == tf_unit ? 1 : opacity * s->opacity;
    if (op < 0.004f) return;
    const float k = paint_view.k;
    float x = VX(n->ax), y = VY(n->ay), w = n->w * k, h = n->h * k, rad = s->radius * k;
    /* A box that clips its children and lies outside the region being painted: nothing of it or of
     * its subtree can land there (0.8.2; each of several damaged rectangles is a walk of the tree,
     * and this keeps that walk to the branches that reach it). Not through transforms, not on the
     * GPU path (its list of app windows is built while painting) and not before the node a
     * partial repaint starts from (its ancestors are inside the region by construction). */
    if (paint_cull && s->overflow_hidden && !n->has_tf) {
        float e = (s->shadow_color >> 24) ? (s->shadow_blur + fmaxf(fabsf(s->shadow_x), fabsf(s->shadow_y))) * k + 1 : 1;
        if (x - e >= c->clip.x1 || y - e >= c->clip.y1 || x + w + e <= c->clip.x0 || y + h + e <= c->clip.y0) return;
    }
    /* per-corner radii and per-side borders (not plain): the *4 canvas functions */
    int plain = node_edges_plain(n);
    const StyleExt *ex = s->ext;
    float r4[4] = { rad, rad, rad, rad };
    if (!plain) for (int i = 0; i < 4; i++) r4[i] = ex->r.rad[i] * k;
    if (!plain) rad = max4(r4);
    int corners_differ = !plain && (r4[1] != r4[0] || r4[2] != r4[0] || r4[3] != r4[0]);
    if (paint_capture_mode && n->protect) {     /* ventana protegida: negra en cualquier captura */
        if (corners_differ) canvas_rrect4(c, x, y, w, h, r4, 0xff000000u);
        else canvas_rrect(c, x, y, w, h, rad, 0xff000000u);
        return;
    }
    if (skip_until && n->id == skip_until) skip_reached = 1;
    int own = !skip_until || skip_reached;     /* ¿se pinta este nodo (o solo sus hijos)? */

    if (own && (s->shadow_color >> 24) && (s->shadow_blur > 0 || s->shadow_x || s->shadow_y))
        PROF(0, canvas_shadow(c, x, y, w, h, rad, s->shadow_x * k, s->shadow_y * k, s->shadow_blur * k,
                              with_opacity(s->shadow_color, op)));

    Corners corners = {0};
    int round_clip = s->overflow_hidden && rad > 0;
    if (round_clip) {
        if (corners_differ) canvas_corners_save4(c, &corners, x, y, w, h, r4);
        else canvas_corners_save(c, &corners, x, y, w, h, rad);
    }

    if (own && s->backdrop_blur >= 1) PROF(1, canvas_backdrop_blur(c, x, y, w, h, rad, (int)(s->backdrop_blur * k + 0.5f)));
    if (own && s->bg >> 24) {
        if (corners_differ) PROF(2, canvas_rrect4(c, x, y, w, h, r4, with_opacity(s->bg, op)));
        else PROF(2, canvas_rrect(c, x, y, w, h, rad, with_opacity(s->bg, op)));
    }
    if (own && s->has_gradient) {
        if (corners_differ) PROF(2, canvas_rrect4_gradient(c, x, y, w, h, r4, s->grad_from, s->grad_to, s->grad_angle, op));
        else PROF(2, canvas_rrect_gradient(c, x, y, w, h, rad, s->grad_from, s->grad_to, s->grad_angle, op));
    }

    float bw = node_border(n, EDGE_L);
    /* content box: logical (Wayland surfaces take logical coordinates and scale themselves) and physical */
    const float *pd = node_padding(n);
    float bl = bw, bt = node_border(n, EDGE_T), br = node_border(n, EDGE_R), bb = node_border(n, EDGE_B);
    float lix = n->ax + pd[EDGE_L] + bl, liy = n->ay + pd[EDGE_T] + bt;
    float liw = n->w - pd[EDGE_L] - pd[EDGE_R] - bl - br;
    float lih = n->h - pd[EDGE_T] - pd[EDGE_B] - bt - bb;
    float ix = VX(lix), iy = VY(liy), iw = liw * k, ih = lih * k;

    if (!own) goto children;
    if (n->type == NODE_IMAGE && n->img) {
        if (corners_differ) {               /* the image clipped by the per-corner shape */
            Corners ic = {0};
            canvas_corners_save4(c, &ic, x, y, w, h, r4);
            PROF(3, image_draw(c, n->img, ix, iy, iw, ih, 0, s->object_fit, op));
            canvas_corners_restore4(c, &ic, x, y, w, h, r4);
        } else PROF(3, image_draw(c, n->img, ix, iy, iw, ih, (s->radius - bw) * k, s->object_fit, op));
    }
    if (n->type == NODE_TERM) term_paint(c, n->term, ix, iy, iw, ih, op);
    if (n->type == NODE_SURFACE) wl_paint(c, n->term, n->id, lix, liy, liw, lih, op);
    if (n->type == NODE_MEDIA) media_paint(c, n->term, n->id, ix, iy, iw, ih, (s->radius - bw) * k, op);
    /* text is clipped to its box: a label outside the clip (a list row scrolled away) isn't laid out */
    if (n->type == NODE_TEXT && (s->color >> 24) && x < c->clip.x1 && x + w > c->clip.x0 && y < c->clip.y1 && y + h > c->clip.y0) {
        node_gather_text(n, text_buf, sizeof text_buf);
        Rect old = canvas_push_clip(c, x, y, w, h);
        int al = s->text_align;
        if (al == TEXT_START) al = n->rtl ? TEXT_RIGHT : TEXT_LEFT;
        else if (al == TEXT_END) al = n->rtl ? TEXT_LEFT : TEXT_RIGHT;
        /* line breaks come from the logical metrics (the ones layout measured), glyphs at the display's size */
        text_paint_scale = k;
        int max_lines = text_max_lines(n, lih);
        int ellipsis = s->text_overflow || s->line_clamp;
        PROF(4, text_draw_ex(c, text_buf, TEXT_WEIGHT(s), s->font_size * k, s->line_height > 4 ? s->line_height * k : s->line_height,
                             ix, iy, iw, al, s->nowrap, max_lines, ellipsis, with_opacity(s->color, op)));
        text_paint_scale = 1;
        canvas_set_clip(c, old);
    }

children:;
    Rect old_clip = c->clip;
    if (s->overflow_hidden) canvas_push_clip(c, x, y, w, h);
    if (n->type != NODE_TEXT) {
        Node *kids[MAX_KIDS];
        int k = node_children_ordered(n, kids, MAX_KIDS);
        for (int i = 0; i < k; i++) paint_node(c, kids[i], op);
    }
    canvas_set_clip(c, old_clip);
    /* The border goes over the children. A partial repaint that starts from a node inside this
     * subtree (the opaque cover, a video) reached it among the children: the border is above it,
     * so it is painted (before 0.8.6 it was skipped, and a child that overflows its parent lost
     * the parent's border line where a small damaged rectangle fell; engine-test
     * test_damage_fuzz). */
    if (!own && skip_until && skip_reached) own = 1;

    if (own && plain && bw > 0 && (s->border_color >> 24))
        PROF(5, canvas_rrect_stroke(c, x, y, w, h, rad, bw * k, with_opacity(s->border_color, op)));
    if (own && !plain) {
        float b4[4];
        uint32_t c4[4];
        int any = 0;
        for (int i = 0; i < 4; i++) {
            b4[i] = ex->r.bw[i] * k;
            c4[i] = with_opacity(ex->r.bc[i], op);
            any |= b4[i] > 0 && (c4[i] >> 24);
        }
        if (any) PROF(5, canvas_border4(c, x, y, w, h, r4, b4, c4));
    }
    if (round_clip) {
        if (corners_differ) canvas_corners_restore4(c, &corners, x, y, w, h, r4);
        else canvas_corners_restore(c, &corners, x, y, w, h, rad);
    }
}

/* Área que ocupa un nodo en pantalla, incluida su sombra. */
static Box visual_box(const Node *n) {
    float e = 1;
    const Style *s = &n->s;
    if ((s->shadow_color >> 24) && (s->shadow_blur > 0 || s->shadow_x || s->shadow_y))
        e += s->shadow_blur + fmaxf(fabsf(s->shadow_x), fabsf(s->shadow_y));
    return (Box){ floorf(n->ax - e), floorf(n->ay - e), ceilf(n->ax + n->w + e), ceilf(n->ay + n->h + e) };
}

/* The same on screen: through the transforms of the node and its ancestors (bounding box). */
static inline Box screen_box(const Node *n) {
    Box b = visual_box(n);
    if (!n->has_tf) return b;
    Box t = affine_box(n->tf->world, b);
    return (Box){ floorf(t.x0) - 1, floorf(t.y0) - 1, ceilf(t.x1) + 1, ceilf(t.y1) + 1 };
}

static int box_eq(Box a, Box b) {
    return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}

/* ---- transformed subtrees (transform: scale/rotate; docs/DESIGN.md "Engine notes") ----
 * A node with its own scale or rotation is painted, with its subtree, once into an offscreen raster
 * in its untransformed layout space, and that raster is composited with the transform (bilinear).
 * The raster scale is the display's (paint_view.k) times the transform scale rounded up to a step
 * (1/8 … 4), so it stays sharp at 150 % or when scaled up, and an animation of the scale (the Dock's
 * minimize, a button's press) only re-rasterizes when it crosses a step: in between it is the same
 * raster composited at another scale. Anything that changes inside (a style, a text, a child's box)
 * invalidates it (damage_walk). The GPU path composites the same CPU canvas, so it gets the same
 * result. Subtrees with an app window or a video on the GPU are painted untransformed; terminals,
 * backdrop blurs (blurring only the subtree's own pixels) and app windows on the CPU path are
 * rasterized at the display's scale and never cached. */
#define TF_CACHE 16
#define TF_MAX_PX (2048 * 2048)
typedef struct {
    int node;
    float kr;                   /* raster px per logical px */
    uint32_t *px;
    int w, h;
    size_t cap;
    float ox, oy;               /* raster origin relative to the node (logical px, padding included) */
    int valid;
    unsigned used;
    uint64_t sig;               /* the subtree's geometry when it was rasterized (damage_walk) */
    Style st;                   /* the node's style without transform and opacity (tf_norm) */
    StyleExt xst;
} TfLayer;
static TfLayer tfl[TF_CACHE];
static unsigned tf_tick;
static long tf_renders, tf_composites;

/* The style that decides what the raster looks like: without the transform, the opacity and the
 * position (those are applied when compositing). */
static void tf_norm(const Style *s, Style *o, StyleExt *ox) {
    *o = *s;
    o->opacity = 1;
    o->translate_x = o->translate_y = 0;
    o->z_index = 0;
    o->ext = NULL;
    memset(ox, 0, sizeof *ox);
    if (s->ext) memcpy(ox, s->ext, STYLE_EXT_VALUES);
    ox->scale_x = ox->scale_y = 1;
    ox->rotate = 0;
    ox->origin_x = ox->origin_y = (Dim){ 0, 0 };
}

/* Bounding box of the subtree in the raster's space; *ok = 0 if it can't be rasterized, *live = 1
 * if something in it changes on its own (terminal, app window: never cached). */
static void tf_bounds(const Node *n, Affine rel, Box *b, int *ok, int *live) {
    if (n->type == NODE_RAW || n->s.display_none) return;
    /* a backdrop blur inside only sees what the subtree painted under it in the raster (a window's
     * own content, not the desktop): good enough for a scaled preview (the Classic task view, the
     * Dock's minimize), and much better than painting the subtree untransformed. Never cached. */
    if (n->s.backdrop_blur >= 1) *live = 1;
    if (n->type == NODE_MEDIA) *ok = 0;
    if (n->type == NODE_SURFACE) { if (gpu_active()) *ok = 0; *live = 1; }
    if (n->type == NODE_TERM) *live = 1;
    Affine r = n->own_tf ? affine_mul(rel, n->tf->own) : rel;
    Box v = affine_box(r, visual_box(n));
    b->x0 = fminf(b->x0, v.x0); b->y0 = fminf(b->y0, v.y0);
    b->x1 = fmaxf(b->x1, v.x1); b->y1 = fmaxf(b->y1, v.y1);
    if (n->type != NODE_TEXT) for (const Node *c = n->first; c; c = c->next) tf_bounds(c, r, b, ok, live);
}

static float tf_step(float sc) {
    static const float steps[] = { 0.125f, 0.25f, 0.5f, 0.75f, 1, 1.5f, 2, 3, 4 };
    for (size_t i = 0; i < sizeof steps / sizeof *steps; i++) if (sc <= steps[i] + 1e-4f) return steps[i];
    return 4;
}

static TfLayer *tf_slot(int node, float kr) {
    TfLayer *lru = &tfl[0];
    for (int i = 0; i < TF_CACHE; i++) {
        if (tfl[i].node == node && tfl[i].kr == kr) return &tfl[i];
        if (tfl[i].used < lru->used) lru = &tfl[i];
    }
    lru->node = node;
    lru->kr = kr;
    lru->valid = 0;
    lru->sig = 0;
    return lru;
}

static int tf_paint(Canvas *c, Node *n, float opacity) {
    if (paint_capture_mode && n->protect) return 0;
    if (skip_until && !skip_reached) return 0;      /* the normal path knows how to skip */
    float sc = fmaxf(fabsf(n->s.ext->scale_x), fabsf(n->s.ext->scale_y));
    if (sc < 1e-3f) return 1;                       /* scale(0): nothing to paint */
    Box b = visual_box(n);
    int ok = 1, live = 0;
    Box sub = b;
    if (n->type != NODE_TEXT)
        for (const Node *k = n->first; k; k = k->next) tf_bounds(k, affine_identity(), &sub, &ok, &live);
    if (!ok) return 0;
    if (!n->s.overflow_hidden) b = sub;
    float kr = live ? paint_view.k : paint_view.k * tf_step(sc);
    const int pad = 1;                              /* a transparent edge: antialiased borders */
    /* The raster's pixel grid is fixed to the node (whole raster pixels from its corner), not to the
     * bounds: a child that moves changes the bounds, and a grid that moved with them resampled every
     * edge of the subtree a fraction of a pixel away, outside the damage of the child that moved
     * (0.8.6, engine-test test_damage_fuzz). */
    int w, h;
    float gx0, gy0;
    for (;;) {
        gx0 = floorf((b.x0 - n->ax) * kr);
        gy0 = floorf((b.y0 - n->ay) * kr);
        w = (int)(ceilf((b.x1 - n->ax) * kr) - gx0) + 2 * pad;
        h = (int)(ceilf((b.y1 - n->ay) * kr) - gy0) + 2 * pad;
        if (w <= 0 || h <= 0) return 1;
        if ((size_t)w * h <= TF_MAX_PX || kr < 0.05f) break;
        kr *= 0.7f;
    }
    if ((size_t)w * h > TF_MAX_PX) return 0;
    /* paint_tf_nocache (tests): a raster of its own per nesting level, the cache untouched */
    static TfLayer scratch[8];
    static int depth;
    if (paint_tf_nocache && depth >= 8) return 0;
    TfLayer *e = paint_tf_nocache ? &scratch[depth] : tf_slot(n->id, kr);
    if (paint_tf_nocache) e->valid = 0;
    e->used = ++tf_tick;
    float ox = (gx0 - pad) / kr, oy = (gy0 - pad) / kr;
    if (!e->valid || live || e->w != w || e->h != h || fabsf(e->ox - ox) > 0.01f || fabsf(e->oy - oy) > 0.01f) {
        if ((size_t)w * h > e->cap) {
            uint32_t *px = realloc(e->px, (size_t)w * h * 4);
            if (!px) return 0;
            e->px = px;
            e->cap = (size_t)w * h;
        }
        e->w = w; e->h = h; e->ox = ox; e->oy = oy;
        memset(e->px, 0, (size_t)w * h * 4);
        Canvas rc;
        canvas_init(&rc, e->px, w, h, w);
        PaintView keep = paint_view;
        paint_view = (PaintView){ n->ax + ox, n->ay + oy, kr, 0, 0, keep.index };
        int ks = skip_until, kr_self = tf_self, ku = tf_unit;
        skip_until = 0;
        tf_self = tf_unit = n->id;
        depth++;
        paint_node(&rc, n, 1);
        depth--;
        skip_until = ks; tf_self = kr_self; tf_unit = ku;
        paint_view = keep;
        tf_norm(&n->s, &e->st, &e->xst);
        e->sig = n->tf->sig;
        e->valid = !live && !paint_capture_mode;
        tf_renders++;
    }
    /* raster px → logical (Ri) → transformed (own) → this canvas (V) */
    const float k = paint_view.k;
    Affine ri = { 1 / kr, 0, 0, 1 / kr, n->ax + ox, n->ay + oy };
    Affine v = { k, 0, 0, k, paint_view.dx - paint_view.x * k, paint_view.dy - paint_view.y * k };
    Affine fwd = affine_mul(v, affine_mul(n->tf->own, ri)), inv;
    if (!affine_invert(fwd, &inv)) return 1;
    float f6[6] = { fwd.a, fwd.b, fwd.c, fwd.d, fwd.e, fwd.f }, i6[6] = { inv.a, inv.b, inv.c, inv.d, inv.e, inv.f };
    canvas_blit_affine(c, e->px, e->w, e->h, i6, f6, opacity * n->s.opacity);
    tf_composites++;
    return 1;
}

void paint_tf_stats(long *renders, long *composites) {
    *renders = tf_renders;
    *composites = tf_composites;
    tf_renders = tf_composites = 0;
}

/* damage_walk: the geometry of a transformed subtree (each node's box relative to its parent) and
 * whether a style or text changed in it; the node's own transform and opacity don't count.
 * A change is seen by the walk only once (paint_dirty is cleared), and the walk only checks a
 * subtree while it is visible and transformed: a change made while it was hidden, or while its
 * scale was 1, left a raster cached for that node valid, and it came back with the old content
 * when the subtree was shown or scaled again (0.8.6, engine-test test_damage_fuzz). So the
 * signature also holds every descendant's generation (Node.gen, bumped by node_invalidate and
 * never cleared), and the node's own style is compared every time. */
typedef struct {
    uint64_t h;
    int dirty;
    int live;                   /* what tf_bounds decides the raster by: 1 live (blur, terminal, app window), 2 not rasterized (video) */
    Box area;                   /* the subtree on screen (the nodes' visible boxes) */
} TfSig;
static TfSig *walk_sig;

static void sig_mix(TfSig *g, const void *p, size_t len) {
    const unsigned char *b = p;
    for (size_t i = 0; i < len; i++) { g->h ^= b[i]; g->h *= 1099511628211ull; }
}

static void sig_node(TfSig *g, const Node *n, int self) {
    float f[6] = { self ? 0 : n->x, self ? 0 : n->y, n->w, n->h, self ? 0 : n->s.translate_x, self ? 0 : n->s.translate_y };
    sig_mix(g, f, sizeof f);
    sig_mix(g, &n->id, sizeof n->id);
    if (!self) sig_mix(g, &n->gen, sizeof n->gen);   /* its own: compared as a style (tf_check) */
    if (!self && n->own_tf) {
        float t[3] = { n->s.ext->scale_x, n->s.ext->scale_y, n->s.ext->rotate };
        sig_mix(g, t, sizeof t);
    }
}

static void tf_check(Node *n, const TfSig *g) {
    n->tf->sig = g->h;
    for (int i = 0; i < TF_CACHE; i++) {
        TfLayer *e = &tfl[i];
        if (e->node != n->id) continue;
        int changed = g->dirty || e->sig != g->h;
        if (!changed) {
            Style st;
            StyleExt xst;
            tf_norm(&n->s, &st, &xst);
            changed = memcmp(&st, &e->st, sizeof st) != 0 || memcmp(&xst, &e->xst, STYLE_EXT_VALUES) != 0;
        }
        if (changed) e->valid = 0;
    }
}

/* Rasters not used for a while give their memory back. */
static void tf_gc(void) {
    for (int i = 0; i < TF_CACHE; i++)
        if (tfl[i].px && tf_tick - tfl[i].used > 600) {
            free(tfl[i].px);
            memset(&tfl[i], 0, sizeof tfl[i]);
        }
}

/* ---- capa de la ventana que se mueve (grab.h) ----
 * Al empezar a mover una ventana su árbol se pinta una vez en un búfer aparte (con su sombra) y
 * en cada cuadro solo se copia en la posición nueva: no se vuelve a pintar texto, terminal ni
 * sombra. If something inside changes (a style, a text, the terminal, a Linux app), the layer is
 * painted again: only the part that changed when that gives the same pixels (layer_tree_part,
 * layer_part_exact), otherwise all of it. Sin capa (se pinta como siempre): con desenfoque de fondo adentro (necesita
 * lo de abajo) y, en el camino GPU, si adentro hay una ventana de app (va como textura aparte).
 *
 * Lo de abajo (todo lo que se pinta antes que la ventana: fondo, escritorio, las otras ventanas)
 * también se guarda una vez («below»): por cuadro se copia la región que cambió, encima va la capa
 * y después se pinta normal solo lo que está por encima (barra de menú, Dock). Si algo de abajo
 * cambia, se vuelve a armar. */
static struct {
    int node, want, valid, rendering;
    uint32_t *px;
    int w, h, cap;
    float ox, oy;               /* esquina de la capa respecto del nodo (logical px) */
    float pox, poy;             /* the same in physical px of the view it was painted for */
    int view;                   /* paint_view.index of the display that uses the layer (paint_layer_view) */
    float k;                    /* that display's scale when the layer was painted */
    float op;                   /* opacidad con la que se pintó */
    float gdx, gdy;             /* corrimiento del último cuadro (damage_walk) */
    Box at;                     /* dónde estaba en pantalla al pintarla */
    /* lo de abajo */
    uint32_t *below;
    int bw, bh, below_valid, stop, stopped;
    int since, no_below;        /* cuadros desde que se armó; si cambia seguido (un video), no se usa */
    Box part;                   /* parte de la capa a repintar (coordenadas de la capa), p. ej. el cursor de la terminal */
    int resize;                 /* cambiando el tamaño: sin capa, solo lo de abajo */
    uint8_t *hole;              /* camino GPU: dónde la capa tiene el hueco de una app (alfa 0 de verdad) */
    int has_holes;              /* 0: every byte of hole is 0 (no app surface in the window) */
    int painted;                /* la capa se pintó en este cuadro (las apps ya quedaron anotadas) */
    /* a node inside the window changed: layer.part also holds its boxes (layer_tree_part) */
    int tree_part;
    int inexact;                /* something in the window may not paint the same when moved (layer_note_exact) */
    float rx, ry;               /* where the window was (physical px) when the layer was last painted whole */
    int in_range;               /* and the layer lay within [0, LAYER_EXACT_MAX) px (rounding is not
                                 * shift-exact across 0: roundf, (int) truncation, canvas_corners_save) */
    int ext_part;               /* outside damage went into layer.part through layer.at since then */
    int odd_text;               /* text in it took a path that may not move exactly (text_drew_inexact) */
} layer;

void paint_layer_begin(int node, int resize) {
    layer.node = node;
    layer.want = 1;
    layer.resize = resize;
    layer.valid = 0;
    layer.below_valid = 0;
}

void paint_layer_end(void) {
    if (!layer.node) return;
    Node *n = node_get(layer.node);
    if (n) damage_add(n->prev_box);
    free(layer.px);
    free(layer.below);
    free(layer.hole);
    memset(&layer, 0, sizeof layer);
}

void paint_layer_external_damage(void) { layer.valid = 0; }

/* Several displays: only the one with most of the window uses the layer and the copy of what is
 * below; the others paint the window as usual. Moving to another display rebuilds both. */
int paint_layer_node(void) { return layer.want ? layer.node : 0; }

void paint_layer_view(int index) {
    if (layer.view == index) return;
    layer.view = index;
    layer.valid = 0;
    layer.below_valid = 0;
}

static long layer_renders, layer_blits, below_renders, layer_parts;
void paint_layer_stats(long *renders, long *blits) {
    *renders = layer_renders + 1000 * below_renders;
    *blits = layer_blits;
    layer_renders = layer_blits = below_renders = 0;
}

/* How many of those renders were only layer.part (layer_render_part), since the last call. */
long paint_layer_parts(void) {
    long n = layer_parts;
    layer_parts = 0;
    return n;
}

static void subtree_box(Node *n, Box *b, int *ok) {
    if (n->type == NODE_RAW || n->s.display_none) return;
    if (n->s.backdrop_blur >= 1) *ok = 0;
    if (gpu_active() && n->type == NODE_MEDIA) *ok = 0;
    Box v = screen_box(n);
    if (v.x0 < b->x0) b->x0 = v.x0;
    if (v.y0 < b->y0) b->y0 = v.y0;
    if (v.x1 > b->x1) b->x1 = v.x1;
    if (v.y1 > b->y1) b->y1 = v.y1;
    if (n->type != NODE_TEXT) for (Node *c = n->first; c; c = c->next) subtree_box(c, b, ok);
}

/* Camino GPU: la capa empieza con un valor centinela (alfa 1) y no con 0, así después se distingue el
 * hueco de una app (canvas_clear deja 0) de lo que quedó vacío o las esquinas redondeadas. */
#define SENTINEL 0x01000000u
static void layer_fill(int x0, int y0, int x1, int y1) {
    if (x0 >= x1 || y0 >= y1) return;
    size_t bytes = (size_t)(x1 - x0) * 4;
    uint32_t *first = layer.px + (size_t)y0 * layer.w + x0;
    if (!gpu_active()) {
        for (int y = y0; y < y1; y++) memset(layer.px + (size_t)y * layer.w + x0, 0, bytes);
        return;
    }
    for (int x = 0; x < x1 - x0; x++) first[x] = SENTINEL;      /* one row, then copied */
    for (int y = y0 + 1; y < y1; y++) memcpy(layer.px + (size_t)y * layer.w + x0, first, bytes);
}
/* Where the window's app surfaces are, in layer pixels: only there can a 0 pixel be an app's hole.
 * Elsewhere a 0 is what a rounded clip left outside its corner, and taking it for a hole made the
 * corners of a dragged window black on the GPU path (nothing shows through where there is no app). */
#define LAYER_SURF_MAX 32
typedef struct { int x0, y0, x1, y1; } LSurf;
static LSurf lsurf[LAYER_SURF_MAX];
static int nlsurf, lsurf_all;
static void layer_surfaces(Node *n, int ox, int oy) {
    if (n->type == NODE_RAW || n->s.display_none) return;
    if (n->type == NODE_SURFACE) {
        if (nlsurf == LAYER_SURF_MAX) lsurf_all = 1;        /* too many: every 0 counts, as before */
        else lsurf[nlsurf++] = (LSurf){ (int)floorf(VX(n->ax)) - ox, (int)floorf(VY(n->ay)) - oy,
                                                   (int)ceilf(VX(n->ax + n->w)) - ox, (int)ceilf(VY(n->ay + n->h)) - oy };
    }
    if (n->type != NODE_TEXT) for (Node *k = n->first; k; k = k->next) layer_surfaces(k, ox, oy);
}
static int in_surface(int x, int y) {
    if (lsurf_all) return 1;
    for (int i = 0; i < nlsurf; i++) if (x >= lsurf[i].x0 && x < lsurf[i].x1 && y >= lsurf[i].y0 && y < lsurf[i].y1) return 1;
    return 0;
}
/* layer.has_holes == 0 stands for a map of zeros, which is what the loop below writes for a window
 * with no app surfaces: then it neither writes the map nor does layer_paint walk it. The bytes it
 * left are made zeros only when a part brings a hole into a map that had none. */
static void layer_holes(int x0, int y0, int x1, int y1) {
    if (!gpu_active() || !layer.hole) return;
    int any = nlsurf || lsurf_all;
    if (!any && !layer.has_holes) {
        for (int y = y0; y < y1; y++) {
            uint32_t *row = layer.px + (size_t)y * layer.w;
            for (int x = x0; x < x1; x++) if (row[x] == SENTINEL) row[x] = 0;
        }
        return;
    }
    int whole = x0 == 0 && y0 == 0 && x1 == layer.w && y1 == layer.h;
    if (!layer.has_holes && !whole) memset(layer.hole, 0, (size_t)layer.w * layer.h);   /* what it stood for */
    uint8_t set = 0;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = layer.px + (size_t)y * layer.w;
        uint8_t *m = layer.hole + (size_t)y * layer.w;
        for (int x = x0; x < x1; x++) {
            m[x] = row[x] == 0 && any && in_surface(x, y);
            set |= m[x];
            if (row[x] == SENTINEL) row[x] = 0;
        }
    }
    if (whole) layer.has_holes = set;
    else layer.has_holes |= set;
}

/* Vuelve a anotar las ventanas de apps de la capa (camino GPU) en su lugar de ahora, sin tocar el canvas. */
static void layer_register(Canvas *c, Node *n, float op) {
    if (n->type == NODE_RAW || n->s.display_none) return;
    op *= n->s.opacity;
    if (n->type == NODE_SURFACE) {
        const float *pd = node_padding(n);
        float bl = node_border(n, EDGE_L), bt = node_border(n, EDGE_T), br = node_border(n, EDGE_R), bb = node_border(n, EDGE_B);
        wl_paint(c, n->term, n->id, n->ax + pd[EDGE_L] + bl, n->ay + pd[EDGE_T] + bt,
                 n->w - pd[EDGE_L] - pd[EDGE_R] - bl - br, n->h - pd[EDGE_T] - pd[EDGE_B] - bt - bb, op);
    }
    if (n->type != NODE_TEXT) for (Node *k = n->first; k; k = k->next) layer_register(c, k, op);
}

/* The layer's box: the window's and its subtree's (logical px) in *b, its pixels on the view's
 * physical ones. 0: no layer (a blur, a transformed window, too big). */
static int layer_box(Node *n, Box *b, int *x0, int *y0, int *w, int *h) {
    *b = visual_box(n);
    int ok = !n->has_tf;                /* a transformed window is painted as usual */
    subtree_box(n, b, &ok);
    *x0 = (int)floorf(VX(b->x0));
    *y0 = (int)floorf(VY(b->y0));
    *w = (int)ceilf(VX(b->x1)) - *x0;
    *h = (int)ceilf(VY(b->y1)) - *y0;
    return ok && *w > 0 && *h > 0 && (size_t)*w * *h <= 4096u * 4096u;
}

/* Painting part of the layer again gives what painting it all at the window's new place would only
 * if every pixel outside that part comes out the same when moved there. That holds when the window
 * moved by whole physical pixels and every coordinate the paint computes from a position is a
 * binary fraction small enough for a float to hold exactly at both places: positions and sizes in
 * sixteenths of a logical pixel within ±LAYER_EXACT_MAX, a whole display scale, whole font sizes
 * and text drawn by text.c's own path in a font with a power-of-two em (text_drew_inexact). A transformed node composites through its
 * matrix, which is not exact when moved, so it must be inside the part (changed this frame). App
 * windows and videos are left out (painting them registers them). */
#define LAYER_EXACT_MAX 4096
static int fine(float v) { float s = v * 16; return s == floorf(s) && fabsf(v) < LAYER_EXACT_MAX; }

static void layer_note_exact(const Node *n, int dirty) {
    const Style *s = &n->s;
    const float *pd = node_padding(n);
    int ok = fine(n->ax) && fine(n->ay) && fine(n->w) && fine(n->h);
    for (int e = EDGE_L; e <= EDGE_B && ok; e++) ok = fine(pd[e]) && fine(node_border(n, e));
    if (n->own_tf && !dirty) ok = 0;
    if (n->type == NODE_SURFACE || n->type == NODE_MEDIA) ok = 0;
    if (n->type == NODE_TEXT && (s->font_size != floorf(s->font_size) || !fine(s->line_height))) ok = 0;
    if (!ok) layer.inexact = 1;
}

/* Whether the layer, kept and painted again only in layer.part, gives the pixels and the layer
 * state of layer_render at the window's place now: the same box, offsets and scale. *bp: that box. */
static int layer_part_exact(Node *n, Box *bp) {
    const float k = paint_view.k;
    if (layer.inexact || layer.odd_text || !layer.in_range || k != layer.k || k < 1 || k != floorf(k)) return 0;
    if (!fine(paint_view.x) || !fine(paint_view.y) || !fine(paint_view.dx) || !fine(paint_view.dy)) return 0;
    float sx = VX(n->ax) - layer.rx, sy = VY(n->ay) - layer.ry;
    if (sx != floorf(sx) || sy != floorf(sy)) return 0;
    Box b;
    int x0, y0, w, h;
    if (!layer_box(n, &b, &x0, &y0, &w, &h) || w != layer.w || h != layer.h) return 0;
    if (x0 != (int)lroundf(VX(n->ax) + layer.pox) || y0 != (int)lroundf(VY(n->ay) + layer.poy)) return 0;
    if (b.x0 - n->ax != layer.ox || b.y0 - n->ay != layer.oy || x0 - VX(n->ax) != layer.pox || y0 - VY(n->ay) != layer.poy) return 0;
    if (x0 < 0 || y0 < 0 || x0 + w >= LAYER_EXACT_MAX || y0 + h >= LAYER_EXACT_MAX) return 0;
    *bp = b;
    return 1;
}

static int layer_render(Node *n, float opacity) {
    Box b;
    int x0, y0, w, h;
    if (!layer_box(n, &b, &x0, &y0, &w, &h)) { layer.want = 0; return 0; }
    if ((size_t)w * h > (size_t)layer.cap) {
        uint32_t *px = realloc(layer.px, (size_t)w * h * 4);
        uint8_t *hole = realloc(layer.hole, (size_t)w * h);
        if (px) layer.px = px;
        if (hole) layer.hole = hole;
        if (!px || !hole) { layer.want = 0; return 0; }
        layer.cap = w * h;
    }
    layer.w = w;
    layer.h = h;
    layer_fill(0, 0, w, h);
    layer.ox = b.x0 - n->ax;
    layer.oy = b.y0 - n->ay;
    layer.pox = x0 - VX(n->ax);
    layer.poy = y0 - VY(n->ay);
    layer.k = paint_view.k;
    layer.op = opacity;
    layer.at = b;
    layer.part = (Box){ 0, 0, 0, 0 };
    layer.tree_part = layer.inexact = layer.ext_part = 0;
    layer.rx = VX(n->ax);
    layer.ry = VY(n->ay);
    layer.in_range = x0 >= 0 && y0 >= 0 && x0 + w < LAYER_EXACT_MAX && y0 + h < LAYER_EXACT_MAX;
    /* canvas con el origen corrido: el árbol se pinta en sus coordenadas de pantalla (así la terminal
     * y las apps Linux siguen anotando dónde están), pero cae en el búfer de la capa */
    Canvas lc;
    canvas_init(&lc, layer.px - ((ptrdiff_t)y0 * w + x0), x0 + w, y0 + h, w);
    canvas_set_clip(&lc, (Rect){ x0, y0, x0 + w, y0 + h });
    int skip = skip_until;
    skip_until = 0;
    layer.rendering = 1;
    text_drew_inexact();
    paint_node(&lc, n, opacity);
    layer.odd_text = text_drew_inexact();
    layer.rendering = 0;
    skip_until = skip;
    nlsurf = lsurf_all = 0;
    layer_surfaces(n, x0, y0);
    layer_holes(0, 0, w, h);
    layer.valid = 1;
    layer.painted = 1;
    layer_renders++;
    return 1;
}

/* Repinta solo layer.part (lo que cambió adentro: una línea de la terminal, el cursor que parpadea).
 * 0: a part with a node's change (layer.tree_part) cut a transformed node's image on its left edge,
 * which a full paint would not have cut (canvas_affine_watch): the caller paints the layer whole. */
static int layer_render_part(Node *n, float opacity) {
    int x0 = (int)lroundf(VX(n->ax) + layer.pox), y0 = (int)lroundf(VY(n->ay) + layer.poy);
    Box p = layer.part;
    int tree = layer.tree_part;
    layer.part = (Box){ 0, 0, 0, 0 };
    layer.tree_part = 0;
    int px0 = (int)fmaxf(0, floorf(p.x0)), py0 = (int)fmaxf(0, floorf(p.y0));
    int px1 = (int)fminf((float)layer.w, ceilf(p.x1)), py1 = (int)fminf((float)layer.h, ceilf(p.y1));
    if (px0 >= px1 || py0 >= py1) return 1;
    layer_fill(px0, py0, px1, py1);
    Canvas lc;
    canvas_init(&lc, layer.px - ((ptrdiff_t)y0 * layer.w + x0), x0 + layer.w, y0 + layer.h, layer.w);
    canvas_set_clip(&lc, (Rect){ x0 + px0, y0 + py0, x0 + px1, y0 + py1 });
    int skip = skip_until;
    skip_until = 0;
    layer.rendering = 1;
    if (tree) canvas_affine_watch(&lc, x0 + px0);
    text_drew_inexact();
    paint_node(&lc, n, opacity);
    layer.odd_text |= text_drew_inexact();
    int cut = tree && canvas_affine_cut();
    layer.rendering = 0;
    skip_until = skip;
    if (cut) return 0;
    nlsurf = lsurf_all = 0;
    layer_surfaces(n, x0, y0);
    layer_holes(px0, py0, px1, py1);
    layer.painted = 1;
    float k = paint_view.k, lx = (x0 - paint_view.dx) / k + paint_view.x, ly = (y0 - paint_view.dy) / k + paint_view.y;
    layer.at = (Box){ lx, ly, lx + layer.w / k, ly + layer.h / k };
    layer_renders++;
    layer_parts++;
    return 1;
}

static int layer_paint(Canvas *c, Node *n, float opacity) {
    if (layer.stop && n->id == layer.stop) layer.stopped = 1;     /* armando lo de abajo: hasta acá */
    if (layer.stopped) return 1;
    if (!layer.want || layer.rendering || n->id != layer.node || paint_capture_mode || layer.resize || paint_view.index != layer.view) return 0;
    if (skip_until && !skip_reached && skip_until != layer.node) return 0;   /* lo tapa un video */
    /* a node inside changed: only its part, when that paints what a full render would */
    Box b = layer.at;
    if (layer.valid && layer.tree_part && layer.op == opacity && !layer_part_exact(n, &b)) layer.valid = 0;
    if ((!layer.valid || layer.op != opacity) && !layer_render(n, opacity)) return 0;
    if (layer.part.x1 > layer.part.x0) {
        int tree = layer.tree_part;
        if (layer_render_part(n, opacity)) { if (tree) layer.at = b; }     /* layer_render's box */
        else if (!layer_render(n, opacity)) return 0;
    }
    if (skip_until == layer.node) skip_reached = 1;
    int bx = (int)lroundf(VX(n->ax) + layer.pox), by = (int)lroundf(VY(n->ay) + layer.poy);
    canvas_blit(c, layer.px, layer.w, layer.h, bx, by, 0, 1, 0);
    if (gpu_active()) {
        /* los huecos de las apps: alfa 0 de verdad (la mezcla no los abre) y la app anotada donde está ahora */
        Rect r = c->clip;
        int x0 = bx > r.x0 ? bx : r.x0, x1 = bx + layer.w < r.x1 ? bx + layer.w : r.x1;
        if (layer.has_holes)                                    /* else the map is all 0 */
        for (int y = by > r.y0 ? by : r.y0; y < by + layer.h && y < r.y1; y++) {
            const uint8_t *m = layer.hole + (size_t)(y - by) * layer.w - bx;
            uint32_t *row = c->px + (size_t)y * c->stride;
            for (int x = x0; x < x1; x++) if (m[x]) row[x] = 0;
        }
        if (!layer.painted) {
            wl_paint_holes(0);
            layer_register(c, n, opacity);
            wl_paint_holes(1);
        }
    }
    layer.painted = 0;
    layer_blits++;
    return 1;
}

/* Lo de abajo de la ventana, en pantalla completa: se pinta el árbol hasta llegar a ella. En el camino
 * GPU sus ventanas de apps (huecos en la UI) se guardan y se vuelven a anotar en cada cuadro. */
static int below_ready(const Canvas *c, Node *root) {
    if (!layer.want || layer.rendering || paint_capture_mode || paint_view.index != layer.view) return 0;
    layer.since++;
    if (layer.no_below) return 0;
    if (layer.below_valid && layer.bw == c->w && layer.bh == c->h) { wl_paint_kept(); return 1; }
    if (layer.below && layer.since < 8) { layer.no_below = 1; return 0; }   /* lo de abajo cambia a cada rato */
    layer.since = 0;
    if (!layer.below || layer.bw != c->w || layer.bh != c->h) {
        free(layer.below);
        layer.below = malloc((size_t)c->w * c->h * 4);
        if (!layer.below) return 0;
        layer.bw = c->w;
        layer.bh = c->h;
    }
    Canvas bc;
    canvas_init(&bc, layer.below, c->w, c->h, c->w);
    canvas_clear(&bc, 0x000000);
    layer.stop = layer.node;
    layer.stopped = 0;
    paint_node(&bc, root, 1);
    layer.stop = layer.stopped = 0;
    wl_paint_keep();                /* las ventanas de apps de abajo (camino GPU) */
    layer.below_valid = 1;
    below_renders++;
    return 1;
}

static void below_copy(Canvas *c) {
    Rect r = c->clip;
    for (int y = r.y0; y < r.y1; y++)
        memcpy(c->px + (size_t)y * c->stride + r.x0, layer.below + (size_t)y * layer.bw + r.x0, (size_t)(r.x1 - r.x0) * 4);
}

/* in_layer: el nodo está dentro de la ventana que se mueve; ahí un nodo que solo se corrió lo
 * mismo que la ventana no invalida la capa (sí hay que repintar la pantalla donde estaba y donde
 * está). Afuera, cualquier cambio invalida lo de abajo. */
/* clip: lo visible según los ancestros con overflow: hidden. Un hijo que se corre adentro de una
 * caja recortada (el mapa del Monitor al moverlo) solo daña lo que se ve, no lo de alrededor. */
static Box box_and(Box a, Box b) {
    Box r = { fmaxf(a.x0, b.x0), fmaxf(a.y0, b.y0), fminf(a.x1, b.x1), fminf(a.y1, b.y1) };
    return r.x1 > r.x0 && r.y1 > r.y0 ? r : (Box){0, 0, 0, 0};
}

static Box box_or(Box a, Box b) {
    if (a.x1 <= a.x0 || a.y1 <= a.y0) return b;
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return a;
    return (Box){ fminf(a.x0, b.x0), fminf(a.y0, b.y0), fmaxf(a.x1, b.x1), fmaxf(a.y1, b.y1) };
}

/* A node inside the dragged window changed or moved inside it: its old box (moved with the window)
 * and its new one go into layer.part, in layer pixels from the window's place now, with a margin
 * for the rounding of both (4 px: a transformed node's image reaches up to 3 px past its box).
 * 0: the layer is painted whole (outside the layer, more than half of it, already invalid). */
static int layer_tree_part(Box cur, Box moved) {
    Node *w = node_get(layer.node);
    int hc = cur.x1 > cur.x0 && cur.y1 > cur.y0, hm = moved.x1 > moved.x0 && moved.y1 > moved.y0;
    /* outside damage mapped through layer.at, which may be stale (a node removed after copies only):
     * until a full render, a node's change repaints the layer whole, as it always did */
    if (!layer.valid || layer.ext_part || !w || (!hc && !hm)) return 0;
    Box u = !hm ? cur : !hc ? moved : (Box){ fminf(cur.x0, moved.x0), fminf(cur.y0, moved.y0), fmaxf(cur.x1, moved.x1), fmaxf(cur.y1, moved.y1) };
    float k = layer.k > 0 ? layer.k : 1, ox = roundf(w->ax + layer.ox), oy = roundf(w->ay + layer.oy);
    Box q = { floorf((u.x0 - ox) * k), floorf((u.y0 - oy) * k), ceilf((u.x1 - ox) * k), ceilf((u.y1 - oy) * k) };
    if (q.x0 < 0 || q.y0 < 0 || q.x1 > layer.w || q.y1 > layer.h) return 0;
    q = (Box){ q.x0 - 4, q.y0 - 4, q.x1 + 4, q.y1 + 4 };
    Box *p = &layer.part;
    if (p->x1 <= p->x0) *p = q;
    else { p->x0 = fminf(p->x0, q.x0); p->y0 = fminf(p->y0, q.y0); p->x1 = fmaxf(p->x1, q.x1); p->y1 = fmaxf(p->y1, q.y1); }
    float aw = fminf(p->x1, (float)layer.w) - fmaxf(p->x0, 0), ah = fminf(p->y1, (float)layer.h) - fmaxf(p->y0, 0);
    if (aw * ah * 2 > (float)layer.w * layer.h) return 0;
    layer.tree_part = 1;
    return 1;
}

static void damage_walk(Node *n, int parent_dirty, int visible, int in_layer, Box clip) {
    if (n->type == NODE_RAW) {
        if (walk_sig && visible) sig_mix(walk_sig, &n->gen, sizeof n->gen);   /* the text, for a cached raster */
        n->paint_dirty = 0;
        return;
    }
    int vis = visible && !n->s.display_none;
    int dirty = parent_dirty || n->paint_dirty;
    Box cur = vis ? box_and(screen_box(n), clip) : (Box){0, 0, 0, 0};
    TfSig *outer_sig = walk_sig, sig;
    if (walk_sig && vis) {
        sig_node(walk_sig, n, 0);
        if (n->paint_dirty) walk_sig->dirty = 1;
        /* as tf_bounds */
        if (n->s.backdrop_blur >= 1 || n->type == NODE_TERM || (n->type == NODE_SURFACE && !gpu_active())) walk_sig->live |= 1;
        if (n->type == NODE_MEDIA || (n->type == NODE_SURFACE && gpu_active())) walk_sig->live |= 2;
        walk_sig->area = box_or(walk_sig->area, cur);
    }
    if (vis && n->own_tf) {
        sig = (TfSig){ 1469598103934665603ull, 0, 0, cur };
        sig_node(&sig, n, 1);
        walk_sig = &sig;
    }
    if (layer.want && n->id == layer.node) {
        in_layer = 1;
        layer.gdx = cur.x0 - n->prev_box.x0;
        layer.gdy = cur.y0 - n->prev_box.y0;
    }
    if (in_layer && vis) layer_note_exact(n, dirty);
    if (dirty || !box_eq(cur, n->prev_box)) {
        damage_add(n->prev_box);
        damage_add(cur);
        Box moved = { n->prev_box.x0 + layer.gdx, n->prev_box.y0 + layer.gdy, n->prev_box.x1 + layer.gdx, n->prev_box.y1 + layer.gdy };
        /* the window itself (its style, its size): the whole layer; a node inside: its part */
        if (in_layer && (dirty || !box_eq(cur, moved)) && (n->id == layer.node || !layer_tree_part(cur, moved))) layer.valid = 0;
        if (!in_layer) layer.below_valid = 0;
    }
    n->prev_box = cur;
    n->paint_dirty = 0;
    Box inner = clip;
    if (n->s.overflow_hidden) {
        Box own_box = { floorf(n->ax), floorf(n->ay), ceilf(n->ax + n->w), ceilf(n->ay + n->h) };
        if (n->has_tf) {
            Box t = affine_box(n->tf->world, own_box);
            own_box = (Box){ floorf(t.x0) - 1, floorf(t.y0) - 1, ceilf(t.x1) + 1, ceilf(t.y1) + 1 };
        }
        inner = box_and(clip, own_box);
    }
    for (Node *c = n->first; c; c = c->next) damage_walk(c, dirty, vis, in_layer, inner);
    if (walk_sig == &sig) {
        tf_check(n, &sig);
        /* The raster's kind changed (a blur, a terminal or an app window came into the subtree or
         * left it): it is now drawn at another resolution, or untransformed, so every pixel of the
         * subtree changes, not only the node that came or went (0.8.6, engine-test
         * test_damage_fuzz: a blurred node moved into a scaled, unclipped subtree changed the
         * text next to it, outside the damage). */
        if (n->tf->live_known && n->tf->live != sig.live) damage_add(sig.area);
        n->tf->live = (uint8_t)sig.live;
        n->tf->live_known = 1;
        walk_sig = outer_sig;
        /* a transformed subtree inside another one: the outer raster holds the inner one's pixels,
         * so whatever changes the inner raster changes the outer one (before 0.8.6 only the inner
         * node's own box reached the outer signature, and the outer raster stayed cached with old
         * content of the inner subtree; engine-test test_damage_fuzz with nested transforms) */
        if (walk_sig) {
            sig_mix(walk_sig, &sig.h, sizeof sig.h);
            walk_sig->dirty |= sig.dirty;
            walk_sig->live |= sig.live;
            walk_sig->area = box_or(walk_sig->area, sig.area);
        }
    }
}

static int box_inside(Box a, Box b) { return a.x0 >= b.x0 && a.y0 >= b.y0 && a.x1 <= b.x1 && a.y1 <= b.y1; }
static int box_meets(Box a, Box b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }

/* A backdrop blur reads the canvas around its box (canvas_backdrop_blur samples a margin of twice
 * the radius), and outside the region being repainted the canvas still holds the last frame, with
 * this node's own tint already on it. Blurring that in made the repainted part lighter: the band
 * left in the menu bar where a menu or the Control Center had just closed (0.8.1, docs/LAYERS.md).
 * So a damage that touches a blurred node, or the margin it samples, repaints the whole node: what
 * it blurs is then this frame's, as in a full repaint. Same on the GPU path (same CPU canvas).
 * (0.8.6) The margin is repainted too: outside the region the canvas holds the last frame's final
 * pixels, with whatever is painted above the blurred node (a sibling's shadow, a menu) already in
 * it, while a full repaint blurs only what lies below it. The node's edges took those in, and the
 * difference stayed until the next full repaint (engine-test test_damage_fuzz). */
static Box blur_near(const Node *n) {
    Box b = n->prev_box;
    float m = ceilf(n->s.backdrop_blur * 2) + 1;
    Box near = { floorf(b.x0 - m), floorf(b.y0 - m), ceilf(b.x1 + m), ceilf(b.y1 + m) };
    if (ui.width > 0 && ui.height > 0) {        /* the canvas ends there: so does what it samples */
        near.x0 = fmaxf(near.x0, 0); near.y0 = fmaxf(near.y0, 0);
        near.x1 = fminf(near.x1, ui.width); near.y1 = fminf(near.y1, ui.height);
    }
    return near;
}

static void blur_walk(Node *n, int *grew) {
    if (n->type == NODE_RAW || n->s.display_none) return;
    Box b = n->prev_box;
    if (n->s.backdrop_blur >= 1 && b.x1 > b.x0 && b.y1 > b.y0) {
        Box near = blur_near(n);
        if (box_meets(ui.damage, near) && !box_inside(near, ui.damage)) { damage_add(near); *grew = 1; }
    }
    for (Node *c = n->first; c; c = c->next) blur_walk(c, grew);
}

/* ---- Several damaged rectangles (0.8.2) ----
 * damage_add keeps, besides the box around everything (ui.damage), the changes as a short list of
 * rectangles (ui.dlist). When the changes are small and far apart, the Monitor's moving dots on a
 * world map, each one is repainted on its own: the box around them was most of the map (0.3 % of a
 * core per frame per second, docs/MONITOR.md). Every rectangle is a complete repaint of its pixels
 * (clear, paint the tree clipped to it), so the result is the same as with the one box.
 *   - Two rectangles whose union wastes less than DAMAGE_PASS_PX are joined (each one is a walk of
 *     the tree), and when they cover most of their box it is one box again.
 *   - Blurred surfaces (backdropFilter) read the canvas around them. The 0.8.1 rule stays: a
 *     rectangle that touches a blurred node or the margin its blur samples grows to the whole node,
 *     and every rectangle that touches that margin joins it, so the margin is either repainted in
 *     the same pass or not repainted this frame at all (then it still holds what it showed).
 *   - Only on the CPU path and without a window being dragged (its layer and the GPU's window list
 *     are built for one paint_root per frame); otherwise one box, as before. */
/* Off by default since the 0.8.2 release: once, out of five tries during the 0.8.2 integration
 * test, a window minimized over Monitor's animated map left a grey rectangle until it came back,
 * and the cause was not found (engine-test test_minimize_damage, a full-repaint comparison of that
 * scene, passes). Correctness first: one box, exactly as in 0.8.1, unless lord-server runs with
 * LORD_DAMAGE=multi (main.c). The clipped-subtree skip (paint_cull) and the copy of only the
 * painted rectangles (present_cpu) follow the same switch.
 * (0.8.6) engine-test test_damage_pipeline (that scene through a copy of main.c's whole
 * bookkeeping: pending flips, both KMS buffers, the cursor) and test_damage_fuzz (random trees)
 * found and fixed here: a parent's border lost where an opaque child started a partial repaint,
 * cached rasters kept after changes made while hidden or unscaled or inside a nested transform, a
 * raster grid that moved with the subtree's extent, a raster changing kind outside the damage,
 * blurs taking in what is painted above them from outside the repainted area, and several frames'
 * rectangles cutting through a blurred node (paint_damage_settle). Each one could leave a stale
 * area until something repainted it, with one box or several; which one the 0.8.2 report was is
 * not proven. The 0.8.2 scene was tried again on VM 116 (docs/MONITOR.md) with both modes and no
 * stale area appeared, before or after these fixes: the report is still not reproduced, so the
 * switch stays off. */
int paint_multi_damage = 0;
static Box drects[DAMAGE_MAX];
static int ndrects;

#ifndef DAMAGE_PASS_PX
#define DAMAGE_PASS_PX 1024.0f          /* about what one more pass costs, in pixels (measured, docs/MONITOR.md) */
#endif
#define DAMAGE_COVER   0.7f             /* rectangles covering this much of their box: one box */
#define BLUR_MAX 48

static Box dl_bbox(const DamageList *l) {
    Box b = l->n ? l->r[0] : (Box){ 0, 0, 0, 0 };
    for (int i = 1; i < l->n; i++)
        b = (Box){ fminf(b.x0, l->r[i].x0), fminf(b.y0, l->r[i].y0), fmaxf(b.x1, l->r[i].x1), fmaxf(b.y1, l->r[i].y1) };
    return b;
}

static void blur_collect(Node *n, Box *bb, int *k) {
    if (n->type == NODE_RAW || n->s.display_none) return;
    Box b = n->prev_box;
    if (n->s.backdrop_blur >= 1 && b.x1 > b.x0 && b.y1 > b.y0) {
        if (*k < BLUR_MAX) bb[*k] = blur_near(n);   /* the node and the margin it samples, as blur_walk */
        (*k)++;
    }
    for (Node *c = n->first; c; c = c->next) blur_collect(c, bb, k);
}

/* The blur rule on a list of rectangles: every rectangle that touches a blurred node's margin joins
 * the rectangle holding that node and its whole margin. 1 if it did not settle in 8 passes. */
static int blur_rule(DamageList *l, const Box *bb, int nb) {
    int changed = 1;
    for (int pass = 0; changed && pass < 8; pass++) {
        changed = 0;
        for (int k = 0; k < nb; k++) {
            Box near = bb[k];
            int hits = 0, inside = 0;
            Box u = near;
            for (int i = 0; i < l->n; i++) {
                if (!box_meets(l->r[i], near)) continue;
                hits++;
                if (box_inside(near, l->r[i])) inside = 1;
                u = (Box){ fminf(u.x0, l->r[i].x0), fminf(u.y0, l->r[i].y0), fmaxf(u.x1, l->r[i].x1), fmaxf(u.y1, l->r[i].y1) };
            }
            if (!hits || (hits == 1 && inside)) continue;
            for (int i = 0; i < l->n; i++) if (box_meets(l->r[i], near)) l->r[i--] = l->r[--l->n];
            dlist_add(l, (Box){ floorf(u.x0), floorf(u.y0), ceilf(u.x1), ceilf(u.y1) });
            changed = 1;
        }
    }
    return changed;
}

/* Damage gathered over several frames (main.c: a display whose page flip was pending keeps adding
 * each frame's rectangles to its list, and dlist_add joins the ones that overlap): each frame's
 * rectangles kept the blur rule, but a join of two of them, or a rectangle of an older frame after
 * a blurred node moved, can cut through a blurred node, whose blur then depends on where the cut
 * falls (0.8.6, engine-test test_damage_pipeline/test_damage_fuzz). The rule again, on the tree as
 * it is painted now; if it doesn't settle, one box grown to every margin it touches. */
void paint_damage_settle(DamageList *l) {
    if (!ui.root || l->n < 1) return;
    Box bb[BLUR_MAX];
    int nb = 0;
    blur_collect(ui.root, bb, &nb);
    if (!nb) return;
    if (nb <= BLUR_MAX && !blur_rule(l, bb, nb)) return;
    Box all = dl_bbox(l);
    if (nb > BLUR_MAX) nb = BLUR_MAX;          /* (more than 48 blurred nodes: the first ones) */
    for (int pass = 0, grew = 1; grew && pass < 8; pass++) {
        grew = 0;
        for (int k = 0; k < nb; k++)
            if (box_meets(all, bb[k]) && !box_inside(bb[k], all)) { all = box_or(all, bb[k]); grew = 1; }
    }
    l->n = 0;
    dlist_add(l, (Box){ floorf(all.x0), floorf(all.y0), ceilf(all.x1), ceilf(all.y1) });
}

/* The list for this frame, or 0 if it has to be one box. */
static int multi_rects(Node *root) {
    if (!paint_multi_damage || layer.want || gpu_active() || !root) return 0;
    Box *d = &ui.damage;
    if (d->x0 != ui.dlast.x0 || d->y0 != ui.dlast.y0 || d->x1 != ui.dlast.x1 || d->y1 != ui.dlast.y1) return 0;   /* set directly */
    if (ui.dlist.n < 2) return 0;
    DamageList l = { .n = 0 };
    for (int i = 0; i < ui.dlist.n; i++) {
        Box b = ui.dlist.r[i];
        dlist_add(&l, (Box){ floorf(b.x0), floorf(b.y0), ceilf(b.x1), ceilf(b.y1) });
    }
    /* join what is cheaper to paint together */
    while (l.n > 1) {
        int bi = -1, bj = -1;
        float best = DAMAGE_PASS_PX;
        for (int i = 0; i < l.n; i++)
            for (int j = i + 1; j < l.n; j++) {
                Box u = { fminf(l.r[i].x0, l.r[j].x0), fminf(l.r[i].y0, l.r[j].y0), fmaxf(l.r[i].x1, l.r[j].x1), fmaxf(l.r[i].y1, l.r[j].y1) };
                float w = box_area(u) - box_area(l.r[i]) - box_area(l.r[j]);
                if (w < best) { best = w; bi = i; bj = j; }
            }
        if (bi < 0) break;
        Box u = { fminf(l.r[bi].x0, l.r[bj].x0), fminf(l.r[bi].y0, l.r[bj].y0), fmaxf(l.r[bi].x1, l.r[bj].x1), fmaxf(l.r[bi].y1, l.r[bj].y1) };
        l.r[bj] = l.r[--l.n];
        l.r[bi] = l.r[--l.n];
        dlist_add(&l, u);
    }
    /* blurred surfaces: see above */
    Box bb[BLUR_MAX];
    int nb = 0;
    blur_collect(root, bb, &nb);
    if (nb > BLUR_MAX) return 0;
    int changed = blur_rule(&l, bb, nb);
    if (changed || l.n < 2) return 0;
    Box all = dl_bbox(&l);
    float sum = 0;
    for (int i = 0; i < l.n; i++) sum += box_area(l.r[i]);
    if (sum >= DAMAGE_COVER * box_area(all)) return 0;
    for (int i = 0; i < l.n; i++) drects[i] = l.r[i];
    ndrects = l.n;
    *d = all;
    ui.dlast = all;
    return 1;
}

int paint_damage_rects(Box *out, int max) {
    int n = ndrects < max ? ndrects : max;
    for (int i = 0; i < n; i++) out[i] = drects[i];
    return n;
}

int paint_damage(Node *root) {
    /* lo que llegó de afuera desde el cuadro anterior (terminal, apps Linux, video): si toca la
     * ventana, se vuelve a pintar su capa; si cae afuera, lo de abajo */
    Box ext = ui.damage;
    if (!layer.tree_part) layer.inexact = 0;        /* layer_note_exact, for the part still to paint */
    if (layer.want && ext.x1 > ext.x0 && ext.y1 > ext.y0) {
        Node *n = node_get(layer.node);
        Box now = n ? n->prev_box : layer.at;
        if (box_inside(ext, layer.at)) {        /* adentro, donde se pintó: solo esa parte */
            float k = layer.k > 0 ? layer.k : 1, e = k != 1 ? 1 : 0;      /* layer coordinates are physical */
            Box q = { floorf((ext.x0 - layer.at.x0) * k) - e, floorf((ext.y0 - layer.at.y0) * k) - e,
                      ceilf((ext.x1 - layer.at.x0) * k) + e, ceilf((ext.y1 - layer.at.y0) * k) + e };
            Box *p = &layer.part;
            if (p->x1 <= p->x0) *p = q;
            else { p->x0 = fminf(p->x0, q.x0); p->y0 = fminf(p->y0, q.y0); p->x1 = fmaxf(p->x1, q.x1); p->y1 = fmaxf(p->y1, q.y1); }
            layer.ext_part = 1;
        } else if (box_meets(ext, layer.at) || box_meets(ext, now)) layer.valid = 0;
        if (!box_inside(ext, layer.at) && !box_inside(ext, now)) layer.below_valid = 0;
    }
    if (root) damage_walk(root, 0, 1, 0, (Box){ -1e9f, -1e9f, 1e9f, 1e9f });
    ndrects = 0;
    if (multi_rects(root)) return 1;
    /* the damage is one box: growing it for a blurred node can reach another one (the bar and a menu) */
    for (int pass = 0, grew = 1; root && grew && pass < 4 && ui.damage.x1 > ui.damage.x0; pass++) { grew = 0; blur_walk(root, &grew); }
    /* esa parte, donde está ahora la ventana; not with a node's change, where the whole layer used to be
     * painted again and nodes' own boxes are the damage */
    if (layer.want && layer.valid && !layer.tree_part && layer.part.x1 > layer.part.x0) {
        Node *n = node_get(layer.node);
        if (n) {
            float x0 = roundf(n->ax + layer.ox), y0 = roundf(n->ay + layer.oy), k = layer.k > 0 ? layer.k : 1;
            damage_add((Box){ floorf(x0 + layer.part.x0 / k), floorf(y0 + layer.part.y0 / k), ceilf(x0 + layer.part.x1 / k), ceilf(y0 + layer.part.y1 / k) });
        }
    }
    Box *d = &ui.damage;
    if (d->x1 > d->x0 && d->y1 > d->y0) { drects[0] = *d; ndrects = 1; }
    return d->x1 > d->x0 && d->y1 > d->y0;
}

/* El nodo opaco de más arriba que tapa toda la región dañada (fondo opaco, opacidad 1, sin
 * desenfoque de fondo; la caja sin las esquinas redondeadas y recortada por sus ancestros): lo
 * anterior a él (fondo de pantalla, sombras, ventanas de atrás) no se ve y no se pinta. Con un
 * mapa o una lista que se mueve adentro de una tarjeta, se pinta desde la tarjeta. */
static void cover_walk(Node *n, Box d, Box clip, int *found) {
    const Style *s = &n->s;
    if (n->type == NODE_RAW || s->display_none || s->opacity < 0.999f || n->own_tf) return;   /* transformed: not where layout says */
    Box box = box_and((Box){ n->ax, n->ay, n->ax + n->w, n->ay + n->h }, clip);
    if ((s->bg >> 24) == 0xff && !s->has_gradient && s->backdrop_blur < 1) {
        float k = s->radius * 0.3f;             /* adentro de las esquinas redondeadas */
        Box in = { box.x0 + k, box.y0 + k, box.x1 - k, box.y1 - k };
        if (d.x0 >= in.x0 && d.y0 >= in.y0 && d.x1 <= in.x1 && d.y1 <= in.y1) *found = n->id;
    }
    if (n->type == NODE_TEXT) return;
    /* its children are clipped to its box: if that doesn't hold the region, none of them can */
    if (s->overflow_hidden && !(d.x0 >= box.x0 && d.y0 >= box.y0 && d.x1 <= box.x1 && d.y1 <= box.y1)) return;
    Box inner = s->overflow_hidden ? box : clip;
    /* rounded corners clip the children too: what lies in them shows this node's background, so
     * a child only covers away from the corners (0.8.2) */
    float rr = node_edges_plain(n) ? s->radius : max4(s->ext->r.rad);
    if (s->overflow_hidden && rr > 0) inner = (Box){ inner.x0 + rr, inner.y0 + rr, inner.x1 - rr, inner.y1 - rr };
    Node *kids[MAX_KIDS];
    int k = node_children_ordered(n, kids, MAX_KIDS);
    for (int i = 0; i < k; i++) cover_walk(kids[i], d, inner, found);
}

static int opaque_cover_node(Node *root) {
    int found = 0;
    if (ui.damage.x1 > ui.damage.x0) cover_walk(root, ui.damage, (Box){ -1e9f, -1e9f, 1e9f, 1e9f }, &found);
    return found == root->id ? 0 : found;
}

/* clear: fill the clip with rgb first, unless the copy of what is below the dragged window is
 * about to write every pixel of that same clip (below_copy), which makes the fill wasted work */
static void paint_root_body(Canvas *c, Node *root, int clear, uint32_t rgb) {
    if (!root) { if (clear) canvas_clear(c, rgb); return; }
    wl_paint_begin();
    /* solo cambió un cuadro de video que tapa esa región: se pinta desde ese nodo (no en el camino
     * GPU, que arma acá la lista de ventanas de las apps) */
    skip_until = gpu_active() ? 0 : media_cover_node(ui.damage.x0, ui.damage.y0, ui.damage.x1, ui.damage.y1);
    if (!skip_until && !gpu_active() && !layer.want) skip_until = opaque_cover_node(root);
    skip_reached = 0;
    paint_cull = !gpu_active() && paint_multi_damage;
    /* una ventana que se mueve: lo de abajo se copia y se pinta desde ella (capa del arrastre) */
    int below = !skip_until && below_ready(c, root);      /* once per call: it counts layer.since */
    if (below) {
        below_copy(c);
        skip_until = layer.node;
    } else if (clear) {
        canvas_clear(c, rgb);
    }
    paint_node(c, root, 1);
    skip_until = 0;
    paint_cull = 0;
    tf_gc();
    wl_paint_popups(c);                     /* menús de las apps Wayland, encima de todo */
    if (paint_profile) {
        fprintf(stderr, "  paint:");
        for (int i = 0; i < 6; i++) { fprintf(stderr, " %s %.2f", prof_name[i], prof[i]); prof[i] = 0; }
        fprintf(stderr, " ms\n");
    }
}

void paint_root(Canvas *c, Node *root) {
    paint_root_body(c, root, 0, 0);
}

/* canvas_clear(c, rgb) followed by paint_root(c, root), with the same pixels */
void paint_root_clear(Canvas *c, Node *root, uint32_t rgb) {
    paint_root_body(c, root, 1, rgb);
}

/* The whole tree painted again with nothing kept from earlier frames: no drag layer, no copy of what
 * is below it, no node skipped (LORD_PAINT_VERIFY in main.c and engine-test compare it with the
 * frame). Like paint_root it starts a new list of app windows on the GPU path: the caller keeps the
 * frame's (wl_paint_save/wl_paint_restore). The canvas must be cleared first. */
void paint_root_full(Canvas *c, Node *root) {
    if (!root) return;
    wl_paint_begin();
    int want = layer.want;
    layer.want = 0;
    skip_until = skip_reached = 0;
    paint_cull = 0;
    paint_node(c, root, 1);
    layer.want = want;
    wl_paint_popups(c);
}

int paint_capture_mode;

void paint_capture(Canvas *c, Node *root) {
    if (!root) return;
    paint_capture_mode = 1;
    paint_node(c, root, 1);
    wl_paint_popups(c);
    paint_capture_mode = 0;
}

static Node *hit(Node *n, float x, float y) {
    if (n->type == NODE_RAW || n->s.display_none || n->s.pointer_none == POINTER_NONE) return NULL;
    float lx = x, ly = y;              /* through the transforms: the box as it is painted */
    if (n->has_tf) affine_apply(n->tf->inv, x, y, &lx, &ly);
    int inside = lx >= n->ax && ly >= n->ay && lx < n->ax + n->w && ly < n->ay + n->h;
    if (n->s.overflow_hidden && !inside) return NULL;
    if (n->type != NODE_TEXT) {
        Node *kids[MAX_KIDS];
        int k = node_children_ordered(n, kids, MAX_KIDS);
        for (int i = k - 1; i >= 0; i--) {
            Node *h = hit(kids[i], x, y);
            if (h) return h;
        }
    }
    return inside && n->s.pointer_none != POINTER_BOX_NONE ? n : NULL;
}

Node *hit_test(Node *root, float x, float y) {
    return root ? hit(root, x, y) : NULL;
}
