/* layout.c — our own flexbox (subset: direction, wrap, grow/shrink, justify, align, gap,
 * margins, padding, %, min/max, position:absolute). With flexWrap: 'wrap' the children that don't
 * fit move to another line (gap also separates the lines; lines go at the start, like
 * align-content: flex-start). Semantics similar to React Native:
 * flexDirection defaults to "column", flexShrink defaults to 0.
 *
 * `direction: 'rtl'` (inherited, as in CSS) flips the main axis of rows and the
 * cross-axis start of columns; left/right, margins and paddings remain
 * physical (as in CSS), so absolute positions —windows, menus— don't change. The logical ones
 * (marginStart/End, paddingStart/End, start/end, borderStart/End…) are mapped to a physical side
 * by the node's own direction (node_resolve_edges) and win over the physical one, as in React Native. */
#include "ui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The node's effective direction (its own, or inherited) and, if it has per-side or logical edges,
 * those edges for that direction. The parent is always prepared first (layout and measure go from
 * the root down). */
static inline void prepare(Node *n) {
    n->rtl = n->s.dir == DIR_RTL ? 1 : n->s.dir == DIR_LTR ? 0 : n->parent ? n->parent->rtl : ui.rtl;
    StyleExt *x = n->s.ext;
    if (x && (!x->r.ok || x->r.rtl != n->rtl)) node_resolve_edges(n);
}

void node_resolve_edges(Node *n) {
    const Style *s = &n->s;
    StyleExt *x = s->ext;
    if (!x) return;
    uint32_t m = x->edge_set;
    memcpy(x->r.mg, s->margin, sizeof x->r.mg);
    memcpy(x->r.pd, s->padding, sizeof x->r.pd);
    for (int i = 0; i < 4; i++) { x->r.bw[i] = s->border_width; x->r.bc[i] = s->border_color; x->r.rad[i] = s->radius; }
    int st = n->rtl ? EDGE_R : EDGE_L, en = n->rtl ? EDGE_L : EDGE_R;
    int side[6] = { EDGE_L, EDGE_T, EDGE_R, EDGE_B, st, en };
    for (int i = 0; i < 6; i++) {
        if (m & ES_BW(i)) x->r.bw[side[i]] = x->border_side[i];
        if (m & ES_BC(i)) x->r.bc[side[i]] = x->border_side_color[i];
    }
    if (m & ES_MARGIN(0)) x->r.mg[st] = x->margin_inline[0];
    if (m & ES_MARGIN(1)) x->r.mg[en] = x->margin_inline[1];
    if (m & ES_PADDING(0)) x->r.pd[st] = x->padding_inline[0];
    if (m & ES_PADDING(1)) x->r.pd[en] = x->padding_inline[1];
    /* corners: 0 TL, 1 TR, 2 BR, 3 BL; logical TS, TE, BS, BE */
    int corner[8] = { 0, 1, 2, 3, n->rtl ? 1 : 0, n->rtl ? 0 : 1, n->rtl ? 2 : 3, n->rtl ? 3 : 2 };
    for (int i = 0; i < 8; i++) if (m & ES_RADIUS(i)) x->r.rad[corner[i]] = x->radius_corner[i];
    x->r.plain = 1;
    for (int i = 1; i < 4; i++)
        if (x->r.bw[i] != x->r.bw[0] || x->r.bc[i] != x->r.bc[0] || x->r.rad[i] != x->r.rad[0]) x->r.plain = 0;
    x->r.rtl = n->rtl;
    x->r.ok = 1;
}

#define MAX_KIDS 1024

static float res(Dim d, float ref) {
    if (d.unit == UNIT_PX) return d.v;
    if (d.unit == UNIT_PCT && !isnan(ref)) return ref * d.v / 100.0f;
    return NAN;
}

static float clamp_dim(float v, Dim mn, Dim mx, float ref) {
    float a = res(mn, ref), b = res(mx, ref);
    if (!isnan(b) && v > b) v = b;
    if (!isnan(a) && v < a) v = a;
    return v < 0 ? 0 : v;
}

static int in_flow(const Node *c) {
    return c->type != NODE_RAW && !c->s.display_none && !c->s.absolute;
}

static float pad_h(const Node *n) { const float *p = node_padding(n); return p[EDGE_L] + p[EDGE_R] + node_border(n, EDGE_L) + node_border(n, EDGE_R); }
static float pad_v(const Node *n) { const float *p = node_padding(n); return p[EDGE_T] + p[EDGE_B] + node_border(n, EDGE_T) + node_border(n, EDGE_B); }

static char text_buf[8192];

/* Intrinsic size (border-box) given the container's available space. */
static void measure(Node *n, float aw, float ah, float *ow, float *oh) {
    const Style *s = &n->s;           /* the caller ran prepare(n) */
    float ph = pad_h(n), pv = pad_v(n);
    float w = res(s->width, aw), h = res(s->height, ah);
    if (isnan(w) || isnan(h)) {
        float cw = 0, ch = 0;
        if (n->type == NODE_TEXT) {
            float maxw = !isnan(w) ? w - ph : !isnan(aw) ? aw - ph : 0;
            float mx = res(s->max_w, aw);
            if (!isnan(mx) && (maxw <= 0 || mx - ph < maxw)) maxw = mx - ph;
            if (maxw < 0) maxw = 0;
            if (!n->measure_valid || fabsf(n->measure_maxw - maxw) > 0.5f) {
                node_gather_text(n, text_buf, sizeof text_buf);
                text_measure_ex(text_buf, TEXT_WEIGHT(s), s->font_size, s->line_height, maxw, s->nowrap,
                                s->line_clamp, &n->measure_w, &n->measure_h);
                n->measure_maxw = maxw;
                n->measure_valid = 1;
            }
            cw = n->measure_w; ch = n->measure_h;
            /* truncated text never asks for more than the space it has (it ends in "…") */
            if ((s->text_overflow || s->line_clamp) && maxw > 0 && cw > maxw) cw = maxw;
        } else if (n->type == NODE_IMAGE && n->img) {
            int iw, ih;
            image_size(n->img, &iw, &ih);
            cw = (float)iw; ch = (float)ih;
            if (!isnan(w) && iw) ch = (w - ph) * ih / iw;
            else if (!isnan(h) && ih) cw = (h - pv) * iw / ih;
        } else {
            float iaw = !isnan(w) ? w - ph : !isnan(aw) ? aw - ph : NAN;
            float iah = !isnan(h) ? h - pv : !isnan(ah) ? ah - pv : NAN;
            int count = 0;
            float lim = s->row ? iaw : iah;                 /* with wrap: length of one line */
            int wrap = s->wrap && !isnan(lim);
            float line_m = 0, line_c = 0, tot_m = 0, tot_c = 0;
            int in_line = 0;
            /* a row's children that grow (flex: 1) end up narrower than the whole row: their height
             * is measured again below at the width layout_line gives them, or wrapping text inside
             * them (a list item's text next to its bullet) is taller than measured and overlaps
             * what follows (0.8.2: Store → Updates, "What's new") */
            enum { GROW_MAX = 16 };
            Node *gk[GROW_MAX];
            float gbase[GROW_MAX], gmv[GROW_MAX], gw[GROW_MAX], fixed_m = 0, grow_sum = 0;
            int ng = 0, grow_over = 0;
            for (Node *c = n->first; c; c = c->next) {
                if (!in_flow(c)) continue;
                prepare(c);
                const float *cm = node_margin(c);
                float mh = cm[EDGE_L] + cm[EDGE_R], mv = cm[EDGE_T] + cm[EDGE_B];
                float c_w, c_h;
                measure(c, isnan(iaw) ? NAN : iaw - mh, isnan(iah) ? NAN : iah - mv, &c_w, &c_h);
                c_w += mh; c_h += mv;
                if (wrap) {
                    /* the main size the lines are broken with, as layout breaks them: the flexBasis
                     * or the width/height when set, else the measured size (a card with a long
                     * summary measures wider than its basis; breaking with that made a grid of cards
                     * measure more lines than it lays out, a blank band under it: 0.8.8.4, #37) */
                    float b = res(c->s.basis, lim), e = res(s->row ? c->s.width : c->s.height, lim);
                    float cm = !isnan(b) ? b + (s->row ? mh : mv) : !isnan(e) ? e + (s->row ? mh : mv) : s->row ? c_w : c_h;
                    float cc = s->row ? c_h : c_w;
                    if (in_line && line_m + s->gap + cm > lim + 0.5f) {
                        if (line_m > tot_m) tot_m = line_m;
                        tot_c += line_c + s->gap;
                        line_m = line_c = 0;
                        in_line = 0;
                    }
                    line_m += (in_line ? s->gap : 0) + cm;
                    if (cc > line_c) line_c = cc;
                    in_line++;
                    continue;
                }
                if (s->row) {
                    cw += c_w;
                    if (c_h > ch) ch = c_h;
                    if (c->s.grow > 0 && !isnan(iaw)) {
                        if (ng < GROW_MAX) {
                            float b = res(c->s.basis, iaw);
                            gk[ng] = c;
                            gbase[ng] = (isnan(b) ? c_w - mh : b) + mh;
                            gmv[ng] = mv;
                            gw[ng] = c_w - mh;
                            ng++;
                            grow_sum += c->s.grow;
                        } else grow_over = 1;
                    } else fixed_m += c_w;
                }
                else        { ch += c_h; if (c_w > cw) cw = c_w; }
                count++;
            }
            if (s->row && ng && !grow_over && !wrap) {
                float free = iaw - fixed_m - (count > 1 ? s->gap * (count - 1) : 0);
                for (int i = 0; i < ng; i++) free -= gbase[i];
                for (int i = 0; i < ng; i++) {
                    const float *cm = node_margin(gk[i]);
                    float mh = cm[EDGE_L] + cm[EDGE_R];
                    float sz = gbase[i] + (free > 0 ? free * gk[i]->s.grow / grow_sum : 0) - mh;
                    sz = clamp_dim(sz, gk[i]->s.min_w, gk[i]->s.max_w, iaw);
                    if (gw[i] <= sz + 0.5f) continue;       /* it fits: same height */
                    float c_w, c_h;
                    measure(gk[i], sz, isnan(iah) ? NAN : iah - gmv[i], &c_w, &c_h);
                    if (c_h + gmv[i] > ch) ch = c_h + gmv[i];
                }
            }
            if (wrap) {
                if (line_m > tot_m) tot_m = line_m;
                tot_c += line_c;
                if (s->row) { cw = tot_m; ch = tot_c; } else { ch = tot_m; cw = tot_c; }
            } else if (count > 1) { if (s->row) cw += s->gap * (count - 1); else ch += s->gap * (count - 1); }
        }
        if (isnan(w)) w = cw + ph;
        if (isnan(h)) h = ch + pv;
    }
    *ow = clamp_dim(w, s->min_w, s->max_w, aw);
    *oh = clamp_dim(h, s->min_h, s->max_h, ah);
}

static void layout_node(Node *n, float w, float h);

static void layout_absolute(Node *n, Node *c, float w, float h) {
    const Style *cs = &c->s;
    prepare(c);
    float bl = node_border(n, EDGE_L), bt = node_border(n, EDGE_T);
    float cbw = w - bl - node_border(n, EDGE_R), cbh = h - bt - node_border(n, EDGE_B);
    const float *pd = node_padding(n), *cm = node_margin(c);
    /* insets straight from the style (grab.c moves a window through them without node_invalidate),
     * with the logical ones on top */
    Dim in[4] = { cs->pos[0], cs->pos[1], cs->pos[2], cs->pos[3] };
    if (cs->ext && cs->ext->inset_inline[0].unit != UNIT_AUTO) in[c->rtl ? EDGE_R : EDGE_L] = cs->ext->inset_inline[0];
    if (cs->ext && cs->ext->inset_inline[1].unit != UNIT_AUTO) in[c->rtl ? EDGE_L : EDGE_R] = cs->ext->inset_inline[1];
    float l = res(in[EDGE_L], cbw), t = res(in[EDGE_T], cbh);
    float r = res(in[EDGE_R], cbw), b = res(in[EDGE_B], cbh);
    float cw = res(cs->width, cbw), ch = res(cs->height, cbh);
    if (isnan(cw) && !isnan(l) && !isnan(r)) cw = cbw - l - r;
    if (isnan(ch) && !isnan(t) && !isnan(b)) ch = cbh - t - b;
    if (isnan(cw) || isnan(ch)) {
        float mw, mh;
        measure(c, isnan(cw) ? cbw : cw, isnan(ch) ? cbh : ch, &mw, &mh);
        if (isnan(cw)) cw = mw;
        if (isnan(ch)) ch = mh;
    }
    cw = clamp_dim(cw, cs->min_w, cs->max_w, cbw);
    ch = clamp_dim(ch, cs->min_h, cs->max_h, cbh);
    float sx = n->rtl ? cbw - pd[EDGE_R] - cw - cm[EDGE_R] - cm[EDGE_L] : pd[EDGE_L];
    c->x = bl + (!isnan(l) ? l : !isnan(r) ? cbw - r - cw : sx) + cm[EDGE_L];
    c->y = bt + (!isnan(t) ? t : !isnan(b) ? cbh - b - ch : pd[EDGE_T]) + cm[EDGE_T];
    layout_node(c, cw, ch);
}

/* Distributes and places a line of children: kids[0..k) on the main axis (main) and in a band
 * of the cross axis [cross_off, cross_off + line_cross) inside the content (cross_total). */
static void layout_line(Node *n, Node **kids, float *size_main, const float *meas_cross, int k, float main,
                        float line_cross, float cross_off, float cross_total, float ix, float iy, float ih) {
    const Style *s = &n->s;
    int row = s->row, rtl = n->rtl;
    float size_cross[MAX_KIDS];
    float used = k > 1 ? s->gap * (k - 1) : 0, grow = 0, shrink = 0;
    for (int i = 0; i < k; i++) {
        const Style *cs = &kids[i]->s;
        const float *mg = node_margin(kids[i]);
        used += size_main[i] + (row ? mg[EDGE_L] + mg[EDGE_R] : mg[EDGE_T] + mg[EDGE_B]);
        grow += cs->grow;
        shrink += cs->shrink * size_main[i];
    }

    /* Distribute free space (grow) or missing space (shrink). */
    float free_space = main - used;
    if (free_space > 0 && grow > 0) {
        for (int i = 0; i < k; i++) size_main[i] += free_space * kids[i]->s.grow / grow;
        free_space = 0;
    } else if (free_space < 0 && shrink > 0) {
        for (int i = 0; i < k; i++) {
            size_main[i] += free_space * kids[i]->s.shrink * size_main[i] / shrink;
            if (size_main[i] < 0) size_main[i] = 0;
        }
        free_space = 0;
    }
    if (free_space != 0 || grow > 0) {
        /* min/max on the main axis and recomputation of free space. */
        float total = k > 1 ? s->gap * (k - 1) : 0;
        for (int i = 0; i < k; i++) {
            const Style *cs = &kids[i]->s;
            const float *mg = node_margin(kids[i]);
            size_main[i] = clamp_dim(size_main[i], row ? cs->min_w : cs->min_h, row ? cs->max_w : cs->max_h, main);
            total += size_main[i] + (row ? mg[EDGE_L] + mg[EDGE_R] : mg[EDGE_T] + mg[EDGE_B]);
        }
        free_space = main - total;
    }

    for (int i = 0; i < k; i++) {
        Node *c = kids[i];
        const Style *cs = &c->s;
        const float *cm = node_margin(c);
        float mc = row ? cm[EDGE_T] + cm[EDGE_B] : cm[EDGE_L] + cm[EDGE_R];
        int align = cs->align_self != ALIGN_AUTO ? cs->align_self : s->align_items;
        float ec = res(row ? cs->height : cs->width, line_cross);
        float sz;
        if (!isnan(ec)) sz = ec;
        else if (align == ALIGN_STRETCH) sz = line_cross - mc;
        else if (row) {
            /* Child height for the width already decided (wrapping text). */
            float mw, mh;
            measure(c, size_main[i], ih - mc, &mw, &mh);
            sz = mh;
        } else sz = meas_cross[i];
        size_cross[i] = clamp_dim(sz, row ? cs->min_h : cs->min_w, row ? cs->max_h : cs->max_w, line_cross);
    }

    float pos = 0, between = s->gap;
    int fill = free_space > 0 || s->justify == JUSTIFY_CENTER || s->justify == JUSTIFY_END;
    if (fill && k > 0) switch (s->justify) {
        case JUSTIFY_CENTER:  pos = free_space / 2; break;
        case JUSTIFY_END:     pos = free_space; break;
        case JUSTIFY_BETWEEN: if (k > 1) between += free_space / (k - 1); break;
        case JUSTIFY_AROUND:  pos = free_space / k / 2; between += free_space / k; break;
        case JUSTIFY_EVENLY:  pos = free_space / (k + 1); between += free_space / (k + 1); break;
        default: break;
    }

    for (int i = 0; i < k; i++) {
        Node *c = kids[i];
        const Style *cs = &c->s;
        const float *cm = node_margin(c);
        float ml = cm[rtl ? EDGE_R : EDGE_L], mr = cm[rtl ? EDGE_L : EDGE_R];
        float m_start = row ? ml : cm[EDGE_T];
        float m_end   = row ? mr : cm[EDGE_B];
        float c_start = row ? cm[EDGE_T] : ml;
        float c_end   = row ? cm[EDGE_B] : mr;
        int align = cs->align_self != ALIGN_AUTO ? cs->align_self : s->align_items;
        float cpos = c_start;
        if (align == ALIGN_CENTER) cpos = (line_cross - size_cross[i] - c_start - c_end) / 2 + c_start;
        else if (align == ALIGN_END) cpos = line_cross - size_cross[i] - c_end;
        cpos += cross_off;
        pos += m_start;
        if (row) { c->x = rtl ? ix + main - pos - size_main[i] : ix + pos; c->y = iy + cpos; layout_node(c, size_main[i], size_cross[i]); }
        else     { c->x = rtl ? ix + cross_total - cpos - size_cross[i] : ix + cpos; c->y = iy + pos; layout_node(c, size_cross[i], size_main[i]); }
        pos += size_main[i] + m_end + between;
    }
}

static void layout_node(Node *n, float w, float h) {
    /* nothing changed inside since the last layout and it has the same box (a window being moved):
     * its children keep their positions, only place() runs */
    if (n->layout_ok && n->w == w && n->h == h && n->layout_rtl == n->rtl) return;
    n->w = w; n->h = h;                 /* prepare(n) ran in the parent's loop (layout_root for the root) */
    n->layout_ok = 1;
    n->layout_rtl = n->rtl;
    if (n->type == NODE_RAW || n->type == NODE_TEXT) return;
    const Style *s = &n->s;
    int row = s->row;
    const float *pd = node_padding(n);
    float ix = pd[EDGE_L] + node_border(n, EDGE_L), iy = pd[EDGE_T] + node_border(n, EDGE_T);
    float iw = w - pad_h(n), ih = h - pad_v(n);
    if (iw < 0) iw = 0;
    if (ih < 0) ih = 0;
    float main = row ? iw : ih, cross = row ? ih : iw;

    Node *kids[MAX_KIDS];
    float size_main[MAX_KIDS], meas_cross[MAX_KIDS], outer_cross[MAX_KIDS];
    int k = 0;
    for (Node *c = n->first; c && k < MAX_KIDS; c = c->next) {
        if (!in_flow(c)) continue;
        const Style *cs = &c->s;
        prepare(c);
        const float *cm = node_margin(c);
        float mc = row ? cm[EDGE_T] + cm[EDGE_B] : cm[EDGE_L] + cm[EDGE_R];
        float basis = res(cs->basis, main);
        float em = res(row ? cs->width : cs->height, main);
        float mw = 0, mh = 0;
        if ((isnan(basis) && isnan(em)) || isnan(res(row ? cs->height : cs->width, cross)) || s->wrap)
            measure(c, row ? iw : iw - mc, row ? ih - mc : ih, &mw, &mh);
        float base = !isnan(basis) ? basis : !isnan(em) ? em : (row ? mw : mh);
        kids[k] = c;
        size_main[k] = base;
        meas_cross[k] = row ? mh : mw;
        float ec = res(row ? cs->height : cs->width, cross);
        outer_cross[k] = (!isnan(ec) ? ec : meas_cross[k]) + mc;
        k++;
    }

    if (!s->wrap) layout_line(n, kids, size_main, meas_cross, k, main, cross, 0, cross, ix, iy, ih);
    else {
        /* lines: break when the next child doesn't fit on the main axis */
        int start = 0;
        float off = 0;
        while (start < k) {
            int end = start;
            float len = 0, lc = 0;
            while (end < k) {
                const float *mg = node_margin(kids[end]);
                float mm = row ? mg[EDGE_L] + mg[EDGE_R] : mg[EDGE_T] + mg[EDGE_B];
                float add = (end > start ? s->gap : 0) + size_main[end] + mm;
                if (end > start && len + add > main + 0.5f) break;
                len += add;
                if (outer_cross[end] > lc) lc = outer_cross[end];
                end++;
            }
            layout_line(n, kids + start, size_main + start, meas_cross + start, end - start, main, lc, off, cross, ix, iy, ih);
            off += lc + s->gap;
            start = end;
        }
    }

    for (Node *c = n->first; c; c = c->next)
        if (c->type != NODE_RAW && !c->s.display_none && c->s.absolute) layout_absolute(n, c, w, h);
}

/* Absolute positions and transforms: world = parent's world · own (scale/rotate around the
 * origin). Without transforms (almost always) only the flags are cleared. */
static void place(Node *n, float px, float py, const Affine *world) {
    n->ax = px + n->x + n->s.translate_x;
    n->ay = py + n->y + n->s.translate_y;
    n->own_tf = n->type != NODE_RAW && style_has_transform(&n->s);
    const Affine *cw = world;
    if (n->own_tf || world) {
        if (!n->tf && !(n->tf = calloc(1, sizeof *n->tf))) { n->own_tf = n->has_tf = 0; cw = NULL; goto kids; }
        NodeTf *t = n->tf;
        if (n->own_tf) {
            t->own = style_transform(&n->s, n->ax, n->ay, n->w, n->h);
            t->world = world ? affine_mul(*world, t->own) : t->own;
        } else t->world = *world;
        if (!affine_invert(t->world, &t->inv)) t->inv = (Affine){ 0, 0, 0, 0, -1e9f, -1e9f };   /* scale 0: never hit */
        cw = &t->world;
    }
    n->has_tf = cw != NULL;
kids:
    for (Node *c = n->first; c; c = c->next) place(c, n->ax, n->ay, cw);
}

static void layout_forget(Node *n) {
    n->layout_ok = 0;
    for (Node *c = n->first; c; c = c->next) layout_forget(c);
}

void layout_root(Node *root, float w, float h) {
    if (!root) return;
    /* the language changed (fonts, direction): every text may measure differently */
    static unsigned gen;
    static int rtl = -1;
    if (gen != text_generation() || rtl != ui.rtl) { layout_forget(root); gen = text_generation(); rtl = ui.rtl; }
    root->x = root->y = 0;
    prepare(root);
    layout_node(root, w, h);
    place(root, 0, 0, NULL);
}
