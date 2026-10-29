/* engine_test.c — headless tests of the UI engine's newer features (docs/DESIGN.md "Engine notes"):
 * text truncation with an ellipsis (LTR, RTL, Arabic, Thai, emoji ZWJ sequences and flags),
 * per-side borders and per-corner radii, logical margins/insets, transform math, transformed
 * painting and hit testing. It links the real layout, text and paint code; the terminal, Wayland,
 * media and GPU hooks of paint.c are stubbed out.
 *   build/engine-test            runs the tests
 *   build/engine-test bench      times layout and paint of a 2000-row list and a window drag
 *   make drag-golden             writes test_drag_exact's golden hashes (only on the reference code)
 * Fallback fonts are looked up as in LordOS (DejaVu Sans covers Arabic and Hebrew in the build
 * container); the checks don't depend on which font draws a script. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bench/lbinput.h"
#include "gpu/gpu.h"
#include "outputs.h"
#include "ui/fonts.h"
#include "ui/ui.h"
#include "ui/unicode.h"

/* ---- stubs for what paint.c reaches outside the UI engine ---- */
/* the terminal (test_drag_exact): a dark box with a cursor block; where it was painted is noted in
 * logical px, as term_paint.c notes s->gx… for term_damage */
static int term_cursor;
static Box term_at;
void term_paint(Canvas *c, int id, float x, float y, float w, float h, float op) {
    (void)id;
    const float k = paint_view.k;
    uint32_t a = (uint32_t)(op * 255 + 0.5f) << 24;
    canvas_rrect(c, x, y, w, h, 0, a | 0x101014);
    if (term_cursor) canvas_rrect(c, x + 8 * k, y + 8 * k, 9 * k, 16 * k, 0, a | 0xd0d0d0);
    float lx = (x - paint_view.dx) / k + paint_view.x, ly = (y - paint_view.dy) / k + paint_view.y;
    term_at = (Box){ lx, ly, lx + w / k, ly + h / k };
}
static int test_gpu;                    /* test_drag_exact: paint.c's GPU path on the same canvas */
/* An app window (test_drag_exact's runs with one below the dragged window), as wl.c: on the GPU path
 * it is noted in the frame's list of windows (what is below a dragged window keeps its own,
 * wl_paint_keep/wl_paint_kept) and opens a hole (alpha 0) in the UI unless wl_paint_holes(0); on the
 * CPU path its content is painted. The list is hashed with the frame's pixels. */
typedef struct { int id, node; float x, y, w, h, op; } WDraw;
enum { WDRAW_MAX = 128 };
static WDraw wdraw[WDRAW_MAX], wkept[WDRAW_MAX], wsaved[WDRAW_MAX];
static int nwdraw, nwkept, nwsaved, wholes_off;
void wl_paint(Canvas *c, int id, int node, float x, float y, float w, float h, float op) {
    const float k = paint_view.k;
    Rect old = canvas_push_clip(c, (x - paint_view.x) * k + paint_view.dx, (y - paint_view.y) * k + paint_view.dy, w * k, h * k);
    if (!test_gpu) canvas_clear(c, 0xff406080u + (uint32_t)id);
    else {
        if (nwdraw < WDRAW_MAX) wdraw[nwdraw++] = (WDraw){ id, node, x, y, w, h, op };
        if (!wholes_off) canvas_clear(c, 0);
    }
    canvas_set_clip(c, old);
}
void wl_paint_holes(int on) { wholes_off = !on; }
void wl_paint_kept(void) { for (int i = 0; i < nwkept && nwdraw < WDRAW_MAX; i++) wdraw[nwdraw++] = wkept[i]; }
void wl_paint_keep(void) { nwkept = nwdraw; memcpy(wkept, wdraw, sizeof *wdraw * (size_t)nwdraw); }
void wl_paint_begin(void) { nwdraw = 0; }
void wl_paint_save(void) { nwsaved = nwdraw; memcpy(wsaved, wdraw, sizeof *wdraw * (size_t)nwdraw); }
void wl_paint_restore(void) { nwdraw = nwsaved; memcpy(wdraw, wsaved, sizeof *wdraw * (size_t)nwsaved); }
void wl_paint_popups(Canvas *c) { (void)c; }
/* GPU drag (paint_root_gdrag): an app window in the list is drawn below the UI where its box is, as wl.c's */
int wl_draws_meet(Rect r) {
    const float k = paint_view.k;
    for (int i = 0; i < nwdraw; i++) {
        const WDraw *d = &wdraw[i];
        Rect b = { (int)floorf((d->x - paint_view.x) * k + paint_view.dx) - 1, (int)floorf((d->y - paint_view.y) * k + paint_view.dy) - 1,
                   (int)ceilf((d->x + d->w - paint_view.x) * k + paint_view.dx) + 1, (int)ceilf((d->y + d->h - paint_view.y) * k + paint_view.dy) + 1 };
        if (b.x0 < r.x1 && r.x0 < b.x1 && b.y0 < r.y1 && r.y0 < b.y1) return 1;
    }
    return 0;
}
void media_paint(Canvas *c, int id, int node, float x, float y, float w, float h, float r, float op) { (void)c; (void)id; (void)node; (void)x; (void)y; (void)w; (void)h; (void)r; (void)op; }
int media_cover_node(float x0, float y0, float x1, float y1) { (void)x0; (void)y0; (void)x1; (void)y1; return 0; }
int gpu_active(void) { return test_gpu; }
void lbf_input(uint64_t ev_ns) { (void)ev_ns; }   /* bench/lbinput.c: only lbi_path is used */

void paint_layer_begin(int node, int resize);
void paint_layer_end(void);
void paint_layer_stats(long *renders, long *blits);
long paint_layer_parts(void);
void paint_tf_stats(long *renders, long *composites);
int text_layout_visual(const char *s, int font, float size, int *cps, int max);

static int fails, checks;
static int quiet;                       /* bench: styles an older engine doesn't know are skipped */
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#ifndef ENGINE_BEFORE
/* ---- UTF-8 helpers ---- */
static int u8_decode(const char *s, uint32_t *out, int max) {
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < max) {
        uint32_t c = *p;
        int k = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : 3;
        c = k == 0 ? c : k == 1 ? c & 0x1f : k == 2 ? c & 0x0f : c & 0x07;
        p++;
        for (int i = 0; i < k && *p; i++, p++) c = c << 6 | (*p & 0x3f);
        out[n++] = c;
    }
    return n;
}

/* what must stay with the code point before it (a grapheme cluster continues) */
static int is_mark_like(uint32_t c) {
    return uc_mark(c) || c == 0x0E33 || c == 0x200D || c == 0xFE0F || (c >= 0x1F3FB && c <= 0x1F3FF);
}

/* Checks a truncation of `s` in a box of width w: out = prefix + "…", the prefix ends on a cluster
 * boundary and it fits. Returns the prefix length in code points (-1 = not truncated). */
static int check_cut(const char *name, const char *s, float w, int force) {
    char out[1024];
    float tw = text_ellipsize(s, 400, 13, w, force, out, sizeof out);
    if (tw < 0) { CHECK(!strcmp(out, s), "%s: fits but changed", name); return -1; }
    size_t lo = strlen(out);
    CHECK(lo >= 3 && !strcmp(out + lo - 3, "\xe2\x80\xa6"), "%s w=%.0f: no ellipsis at the end: %s", name, w, out);
    size_t lp = lo - 3;
    CHECK(!strncmp(out, s, lp), "%s w=%.0f: not a prefix: %s", name, w, out);
    uint32_t a[512], b[512];
    int na = u8_decode(s, a, 512);
    char pre[1024];
    memcpy(pre, out, lp);
    pre[lp] = 0;
    int nb = u8_decode(pre, b, 512);
    if (nb > 0 && nb < na) {
        /* trailing spaces are dropped: find where the prefix really ended in s */
        int k = nb;
        while (k < na && a[k] == ' ') k++;
        uint32_t next = a[nb], prev = a[nb - 1];
        CHECK(!is_mark_like(next), "%s w=%.0f: cut before U+%04X (prefix %d cps)", name, w, next, nb);
        CHECK(prev != 0x200D, "%s w=%.0f: cut after a ZWJ", name, w);
        if (next >= 0x1F1E6 && next <= 0x1F1FF) {
            int r = 0;
            for (int i = nb - 1; i >= 0 && a[i] >= 0x1F1E6 && a[i] <= 0x1F1FF; i--) r++;
            CHECK(!(r & 1), "%s w=%.0f: cut inside a flag", name, w);
        }
        (void)k;
    }
    if (nb > 0) CHECK(tw <= w + 0.5f, "%s w=%.0f: too wide (%.1f)", name, w, tw);
    return nb;
}

static void sweep(const char *name, const char *s) {
    int last = -1;
    for (float w = 8; w <= 400; w += 3) {
        int n = check_cut(name, s, w, 0);
        if (n < 0) break;
        CHECK(n >= last, "%s: a wider box (%.0f) kept less text (%d < %d)", name, w, n, last);
        last = n;
    }
}

/* Visual position of the "…" in the cut form: 0 = leftmost. */
static void check_side(const char *name, const char *s, float w, int want_left) {
    char out[1024];
    text_ellipsize(s, 400, 13, w, 1, out, sizeof out);
    int vis[512];
    int n = text_layout_visual(out, 0, 13, vis, 512);
    uint32_t cps[512];
    int ncp = u8_decode(out, cps, 512);
    int pos = -1;
    for (int i = 0; i < n; i++) if (vis[i] == ncp - 1) pos = i;
    CHECK(n > 0 && pos >= 0, "%s: no glyph for the ellipsis", name);
    if (pos >= 0) {
        if (want_left) CHECK(pos == 0, "%s: ellipsis at visual %d of %d, want the left edge", name, pos, n);
        else CHECK(pos == n - 1, "%s: ellipsis at visual %d of %d, want the right edge", name, pos, n);
    }
}

static void test_ellipsis(void) {
    const char *ltr = "Quarterly report for the board, final version (2).pdf";
    const char *arabic = "\xd8\xaa\xd9\x82\xd8\xb1\xd9\x8a\xd8\xb1 \xd8\xa7\xd9\x84\xd9\x85\xd8\xa8\xd9\x8a\xd8\xb9\xd8\xa7\xd8\xaa "
                         "\xd8\xa7\xd9\x84\xd8\xb3\xd9\x86\xd9\x88\xd9\x8a \xd9\x84\xd9\x84\xd8\xb9\xd8\xa7\xd9\x85 \xd8\xa7\xd9\x84\xd9\x85\xd8\xa7\xd8\xb6\xd9\x8a "
                         "\xd8\xa8\xd8\xa7\xd9\x84\xd8\xaa\xd9\x81\xd8\xb5\xd9\x8a\xd9\x84 \xd9\x84\xd8\xa7 \xd8\xb9\xd9\x8e\xd9\x84\xd9\x91\xd9\x8e\xd9\x85\xd9\x8e";
    const char *hebrew = "\xd7\x93\xd7\x95\xd7\x97 \xd7\xa8\xd7\x91\xd7\xa2\xd7\x95\xd7\xa0\xd7\x99 \xd7\xa1\xd7\x95\xd7\xa4\xd7\x99 \xd7\x9c\xd7\x93\xd7\x99\xd7\xa8\xd7\xa7\xd7\x98\xd7\x95\xd7\xa8\xd7\x99\xd7\x95\xd7\x9f";
    const char *thai = "\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa\xe0\xb8\x94\xe0\xb8\xb5\xe0\xb8\x84\xe0\xb8\xa3\xe0\xb8\xb1\xe0\xb8\x9a "
                       "\xe0\xb8\xa2\xe0\xb8\xb4\xe0\xb8\x99\xe0\xb8\x94\xe0\xb8\xb5\xe0\xb8\x95\xe0\xb9\x89\xe0\xb8\xad\xe0\xb8\x99\xe0\xb8\xa3\xe0\xb8\xb1\xe0\xb8\x9a "
                       "\xe0\xb8\x99\xe0\xb9\x89\xe0\xb8\xb3\xe0\xb8\x97\xe0\xb8\xb3\xe0\xb8\x99\xe0\xb9\x89\xe0\xb8\xb3\xe0\xb9\x83\xe0\xb8\x88";
    const char *emoji = "Family \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7\xe2\x80\x8d\xf0\x9f\x91\xa6 trip "
                        "\xf0\x9f\x87\xa6\xf0\x9f\x87\xb7\xf0\x9f\x87\xa7\xf0\x9f\x87\xb7\xf0\x9f\x87\xa8\xf0\x9f\x87\xb1 \xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd "
                        "\xe2\x9d\xa4\xef\xb8\x8f done";
    const char *marks = "Cafe\xcc\x81 de\xcc\x81ja\xcc\x80 vu na\xc3\xafve re\xcc\x81sume\xcc\x81 tre\xcc\x80s longue";

    /* fits: unchanged */
    char out[256];
    CHECK(text_ellipsize("Short", 400, 13, 200, 0, out, sizeof out) < 0 && !strcmp(out, "Short"), "short text was cut");
    /* forced (more lines follow) adds the ellipsis even if it fits */
    text_ellipsize("Short", 400, 13, 200, 1, out, sizeof out);
    CHECK(!strcmp(out, "Short\xe2\x80\xa6"), "forced: %s", out);
    /* a box smaller than the ellipsis: only the ellipsis */
    text_ellipsize(ltr, 400, 13, 2, 0, out, sizeof out);
    CHECK(!strcmp(out, "\xe2\x80\xa6"), "tiny box: %s", out);

    sweep("LTR", ltr);
    sweep("Arabic", arabic);
    sweep("Hebrew", hebrew);
    sweep("Thai", thai);
    sweep("emoji", emoji);
    sweep("combining", marks);

    /* the ellipsis goes on the visual left of RTL text, on the right of LTR */
    check_side("Arabic", arabic, 120, 1);
    check_side("Hebrew", hebrew, 90, 1);
    check_side("LTR in complex path", emoji, 90, 0);

    /* Arabic: the cut letter takes its final form (the cut form is shaped again), so the last
     * glyph before the ellipsis differs from the same letter in the middle of the word */
    {
        char a[256];
        text_ellipsize(arabic, 400, 13, 60, 0, a, sizeof a);
        float w1, h1;
        text_measure(a, 400, 13, 0, 0, 1, &w1, &h1);
        CHECK(w1 <= 60.5f, "Arabic cut form measures %.1f > 60", w1);
    }

    /* measure with a line clamp: at most N lines, and the width of the box when cut */
    const char *para = "The quick brown fox jumps over the lazy dog and keeps running across the whole field until night";
    float w, h, w2, h2;
    text_measure_ex(para, 400, 13, 0, 120, 0, 0, &w, &h);
    text_measure_ex(para, 400, 13, 0, 120, 0, 2, &w2, &h2);
    float lh = text_line_height(13, 0);
    CHECK(h > 2 * lh && fabsf(h2 - 2 * lh) < 0.01f, "clamp: %f lines -> %f", h / lh, h2 / lh);
    CHECK(fabsf(w2 - 120) < 0.6f, "clamp width %.1f", w2);
    text_measure_ex(arabic, 400, 13, 0, 90, 0, 2, &w2, &h2);
    CHECK(fabsf(h2 - 2 * lh) < 0.01f, "Arabic clamp: %f lines", h2 / lh);

    /* drawing: nowrap + ellipsis stays inside the box; the last clamped line ends in an ellipsis */
    static uint32_t px[300 * 80];
    Canvas c;
    canvas_init(&c, px, 300, 80, 300);
    canvas_clear(&c, 0);
    text_draw_ex(&c, ltr, 400, 13, 0, 10, 10, 100, TEXT_LEFT, 1, 0, 1, 0xffffffffu);
    int ink_out = 0, ink_in = 0;
    for (int y = 0; y < 80; y++)
        for (int x = 0; x < 300; x++)
            if (px[y * 300 + x] & 0xff) { if (x >= 111) ink_out++; else ink_in++; }
    CHECK(ink_in > 50 && ink_out == 0, "nowrap ellipsis drew outside the box (%d in, %d out)", ink_in, ink_out);
    canvas_clear(&c, 0);
    text_draw_ex(&c, para, 400, 13, 0, 10, 0, 120, TEXT_LEFT, 0, 2, 1, 0xffffffffu);
    int below = 0;
    for (int y = (int)(2 * lh) + 1; y < 80; y++) for (int x = 0; x < 300; x++) if (px[y * 300 + x] & 0xff) below++;
    CHECK(below == 0, "clamped text drew a third line (%d px)", below);
}

#endif

/* ---- nodes ---- */
static Node *text(Node *parent, const char *s, float size);
static Node *view(Node *parent) {
    Node *n = node_new(NODE_VIEW);
    if (parent) node_append(parent, n);
    return n;
}
/* as lord.setStyle (js.c): set, then invalidate */
static void sn(Node *n, const char *k, double v) { if (style_set_number(&n->s, k, v) && !quiet) { printf("unknown style %s\n", k); fails++; } node_invalidate(n); }
static void ss(Node *n, const char *k, const char *v) { if (style_set_string(&n->s, k, v) && !quiet) { printf("unknown style %s=%s\n", k, v); fails++; } node_invalidate(n); }

#ifndef ENGINE_BEFORE
static uint32_t *fb;
static int FW, FH;
static Canvas frame;

static void render(Node *root, int w, int h) {
    if (FW != w || FH != h) { free(fb); fb = calloc((size_t)w * h, 4); FW = w; FH = h; }
    canvas_init(&frame, fb, w, h, w);
    layout_root(root, (float)w, (float)h);
    paint_damage(root);
    ui.damage = (Box){ 0, 0, (float)w, (float)h };
    paint_root_clear(&frame, root, 0);
    ui.damage = (Box){ 0, 0, 0, 0 };
}
static uint32_t pix(int x, int y) { return fb[y * FW + x] & 0xffffff; }

static void test_borders(void) {
    ui_reset();
    Node *root = view(NULL);
    ui.root = root;
    sn(root, "backgroundColor", 0xffffffff);
    /* bottom hairline 2 px red and a 1 px blue left edge, no top or right */
    Node *a = view(root);
    ss(a, "position", "absolute");
    sn(a, "left", 10); sn(a, "top", 10); sn(a, "width", 50); sn(a, "height", 30);
    sn(a, "borderBottomWidth", 2); sn(a, "borderBottomColor", 0xffff0000);
    sn(a, "borderLeftWidth", 1); sn(a, "borderLeftColor", 0xff0000ff);
    /* RTL: borderStart is the right side */
    Node *b = view(root);
    ss(b, "position", "absolute"); ss(b, "direction", "rtl");
    sn(b, "left", 70); sn(b, "top", 10); sn(b, "width", 20); sn(b, "height", 20);
    sn(b, "borderStartWidth", 3); sn(b, "borderStartColor", 0xff00ff00);
    /* only the top-left corner rounded */
    Node *r = view(root);
    ss(r, "position", "absolute");
    sn(r, "left", 10); sn(r, "top", 50); sn(r, "width", 40); sn(r, "height", 40);
    sn(r, "backgroundColor", 0xff000000); sn(r, "borderTopLeftRadius", 12);
    /* a rounded box with a per-side border: the sides meet on the corner's diagonal */
    Node *q = view(root);
    ss(q, "position", "absolute");
    sn(q, "left", 60); sn(q, "top", 50); sn(q, "width", 40); sn(q, "height", 40);
    sn(q, "borderRadius", 8); sn(q, "borderWidth", 4); sn(q, "borderColor", 0xff808080);
    sn(q, "borderTopColor", 0xffff0000);
    /* uniform border: the old path */
    Node *u = view(root);
    ss(u, "position", "absolute");
    sn(u, "left", 110); sn(u, "top", 10); sn(u, "width", 20); sn(u, "height", 20);
    sn(u, "borderWidth", 1); sn(u, "borderColor", 0xff000000);
    render(root, 140, 100);

    CHECK(!node_edges_plain(a) && node_edges_plain(u), "edges_plain flags");
    CHECK(pix(35, 39) == 0xff0000 && pix(35, 38) == 0xff0000, "bottom border: %06x", pix(35, 39));
    CHECK(pix(35, 37) == 0xffffff, "bottom border too thick: %06x", pix(35, 37));
    CHECK(pix(10, 25) == 0x0000ff, "left border: %06x", pix(10, 25));
    CHECK(pix(35, 10) == 0xffffff, "a top border appeared: %06x", pix(35, 10));
    CHECK(pix(59, 25) == 0xffffff, "a right border appeared: %06x", pix(59, 25));
    CHECK(node_border(b, EDGE_R) == 3 && node_border(b, EDGE_L) == 0, "RTL start = right (%g %g)", node_border(b, EDGE_L), node_border(b, EDGE_R));
    CHECK(pix(88, 20) == 0x00ff00 && pix(70, 20) == 0xffffff, "RTL border start: %06x %06x", pix(88, 20), pix(70, 20));
    CHECK(pix(11, 51) == 0xffffff, "top-left corner not rounded: %06x", pix(11, 51));
    CHECK(pix(48, 51) == 0x000000 && pix(11, 88) == 0x000000 && pix(48, 88) == 0x000000, "other corners should be square");
    CHECK(pix(80, 51) == 0xff0000 && pix(61, 70) == 0x808080 && pix(80, 88) == 0x808080, "rounded per-side border: %06x %06x %06x",
          pix(80, 51), pix(61, 70), pix(80, 88));
    CHECK(pix(80, 70) == 0xffffff, "rounded border filled the inside: %06x", pix(80, 70));
    CHECK(pix(110, 20) == 0x000000 && pix(120, 10) == 0x000000 && pix(120, 20) == 0xffffff, "uniform border");
    /* the content box shrinks by the per-side widths */
    Node *in = view(a);
    sn(in, "flexGrow", 1);
    render(root, 140, 100);
    CHECK(in->x == 1 && in->y == 0 && in->w == 49 && in->h == 28, "content box %g %g %g %g", in->x, in->y, in->w, in->h);
}

static void test_logical(void) {
    ui_reset();
    Node *root = view(NULL);
    ui.root = root;
    Node *col = view(root);
    sn(col, "width", 200); sn(col, "height", 100);
    Node *c1 = view(col);
    sn(c1, "height", 10); sn(c1, "marginStart", 10); sn(c1, "marginEnd", 30);
    Node *c2 = view(col);
    sn(c2, "height", 10); sn(c2, "paddingStart", 7);
    Node *t = view(c2);
    sn(t, "height", 4);
    Node *abs = view(col);
    ss(abs, "position", "absolute"); sn(abs, "width", 20); sn(abs, "height", 20); sn(abs, "start", 5); sn(abs, "top", 0);
    render(root, 300, 120);
    CHECK(c1->x == 10 && c1->w == 160, "LTR marginStart: x %g w %g", c1->x, c1->w);
    CHECK(t->x == 7, "LTR paddingStart: %g", t->x);
    CHECK(abs->x == 5, "LTR start: %g", abs->x);
    ss(col, "direction", "rtl");
    node_invalidate(col);
    render(root, 300, 120);
    CHECK(c1->x == 30 && c1->w == 160, "RTL marginStart: x %g w %g", c1->x, c1->w);
    CHECK(t->x == 0 && t->w == 193, "RTL paddingStart: x %g w %g", t->x, t->w);
    CHECK(abs->x == 175, "RTL start: %g", abs->x);
    /* a physical value loses against the logical one, as in React Native */
    sn(c1, "marginLeft", 50);
    render(root, 300, 120);
    CHECK(c1->x == 30 && node_margin(c1)[EDGE_R] == 10, "RTL marginLeft loses against marginEnd: x %g", c1->x);
    /* removing the logical values gives the physical ones back */
    style_reset(&c1->s, "marginStart");
    style_reset(&c1->s, "marginEnd");
    node_invalidate(c1);
    render(root, 300, 120);
    CHECK(c1->x == 50 && node_margin(c1)[EDGE_R] == 0, "reset marginStart/End: x %g right %g", c1->x, node_margin(c1)[EDGE_R]);
}

static void test_transform_math(void) {
    Affine s = { 2, 0, 0, 3, 5, 7 }, inv;
    CHECK(affine_invert(s, &inv), "invert");
    Affine id = affine_mul(s, inv);
    CHECK(fabsf(id.a - 1) < 1e-5f && fabsf(id.d - 1) < 1e-5f && fabsf(id.e) < 1e-4f && fabsf(id.f) < 1e-4f && fabsf(id.b) < 1e-6f,
          "m * inv(m) = identity");
    Style st;
    style_defaults(&st);
    CHECK(!style_has_transform(&st), "default style has no transform");
    style_set_number(&st, "scaleX", 2);
    style_set_number(&st, "scaleY", 2);
    Affine m = style_transform(&st, 10, 10, 20, 20);     /* scale 2 around the center (20, 20) */
    float x, y;
    affine_apply(m, 20, 20, &x, &y);
    CHECK(fabsf(x - 20) < 1e-4f && fabsf(y - 20) < 1e-4f, "origin fixed: %g %g", x, y);
    affine_apply(m, 10, 10, &x, &y);
    CHECK(fabsf(x) < 1e-4f && fabsf(y) < 1e-4f, "corner scaled: %g %g", x, y);
    free(st.ext);
    style_defaults(&st);
    style_set_number(&st, "rotate", 90);                /* clockwise on screen */
    m = style_transform(&st, 0, 0, 20, 20);
    affine_apply(m, 20, 10, &x, &y);                    /* right middle → bottom middle */
    CHECK(fabsf(x - 10) < 1e-4f && fabsf(y - 20) < 1e-4f, "rotate 90: %g %g", x, y);
    style_set_number(&st, "transformOriginX", 0);
    style_set_number(&st, "transformOriginY", 0);
    m = style_transform(&st, 0, 0, 20, 20);
    free(st.ext);
    affine_apply(m, 10, 0, &x, &y);
    CHECK(fabsf(x) < 1e-4f && fabsf(y - 10) < 1e-4f, "rotate around the top-left: %g %g", x, y);
    style_defaults(&st);
    style_set_number(&st, "rotate", 45);
    Box b = affine_box(style_transform(&st, 0, 0, 10, 10), (Box){ 0, 0, 10, 10 });
    CHECK(fabsf((b.x1 - b.x0) - 14.142f) < 0.01f, "rotated bounding box %g", b.x1 - b.x0);
    style_set_number(&st, "rotate", 360);
    CHECK(style_has_transform(&st) == 0, "rotate 360 is no transform");
    free(st.ext);
}

/* Shell layers (sdk/layers.js, docs/LAYERS.md): full-screen root children with zIndex and
 * pointerEvents box-none. Paint and hit order follow the zIndex whatever the tree order, a layer
 * never takes a click itself, and pointerEvents none still hides a whole subtree. */
static Node *layer_node(Node *root, float z, float w, float h) {
    Node *l = view(root);
    ss(l, "position", "absolute");
    sn(l, "left", 0); sn(l, "top", 0); sn(l, "width", w); sn(l, "height", h);
    ss(l, "pointerEvents", "box-none");
    sn(l, "zIndex", z);
    return l;
}
static Node *box_in(Node *parent, float x, float y, float w, float h, uint32_t color) {
    Node *b = view(parent);
    ss(b, "position", "absolute");
    sn(b, "left", x); sn(b, "top", y); sn(b, "width", w); sn(b, "height", h);
    sn(b, "backgroundColor", color);
    return b;
}
static void test_layers(void) {
    ui_reset();
    Node *root = view(NULL);
    ui.root = root;
    sn(root, "backgroundColor", 0xffffffff);
    /* created in the "wrong" tree order on purpose: popover before dock, both before the windows */
    Node *pop = layer_node(root, 50, 160, 100);
    Node *dockl = layer_node(root, 10, 160, 100);
    Node *base = view(root);                            /* the theme's tree (zIndex 0) */
    sn(base, "width", 160); sn(base, "height", 100);
    Node *win = box_in(base, 0, 0, 160, 100, 0xff0000ff);         /* a window: blue */
    Node *dock = box_in(dockl, 0, 10, 30, 80, 0xff00ff00);        /* the Dock on the left: green */
    Node *menu = box_in(pop, 10, 20, 60, 30, 0xffff0000);         /* the crown menu: red */
    Node *ghost = box_in(pop, 100, 60, 20, 20, 0xff000000);       /* pointerEvents none: never hit */
    ss(ghost, "pointerEvents", "none");
    render(root, 160, 100);

    CHECK(pix(20, 30) == 0xff0000, "menu painted over the Dock: %06x", pix(20, 30));
    CHECK(pix(5, 70) == 0x00ff00, "Dock painted over the window: %06x", pix(5, 70));
    CHECK(pix(100, 30) == 0x0000ff, "window painted under empty layers: %06x", pix(100, 30));
    CHECK(hit_test(root, 20, 30) == menu, "a click on the menu over the Dock goes to the menu");
    CHECK(hit_test(root, 5, 70) == dock, "a click on the Dock goes to the Dock");
    CHECK(hit_test(root, 100, 30) == win, "an empty layer lets the click through to the window");
    CHECK(hit_test(root, 110, 70) == win, "pointerEvents none: the click goes through");
    /* the base can't climb above a layer, the order holds after a re-render */
    sn(win, "zIndex", 5);
    render(root, 160, 100);
    CHECK(hit_test(root, 20, 30) == menu && pix(20, 30) == 0xff0000, "a zIndex inside the base stays under the layers");
}

/* Closing a menu repaints only where it was; the translucent, blurred menu bar above it must come
 * out as in a full repaint (before 0.8.1 the blur mixed in the bar's last frame: a lighter band). */
static void test_backdrop_band(void) {
    ui_reset();
    Node *root = view(NULL);
    ui.root = root;
    sn(root, "backgroundColor", 0xff102040);
    for (int i = 0; i < 8; i++) box_in(root, (float)(i * 20), 0, 10, 100, 0xff4060a0);   /* stripes to blur */
    Node *barl = layer_node(root, 20, 160, 100);
    Node *bar = box_in(barl, 0, 0, 160, 28, 0x80ffffff);
    sn(bar, "backdropBlur", 12);
    Node *popl = layer_node(root, 50, 160, 100);
    Node *menu = box_in(popl, 30, 20, 60, 60, 0xffff0000);       /* reaches into the bar, like its shadow */
    render(root, 160, 100);
    node_destroy(menu);
    layout_root(root, 160, 100);
    Box d = { 0, 0, 0, 0 };
    if (paint_damage(root)) d = ui.damage;
    CHECK(d.x0 <= 0 && d.y0 <= 0 && d.x1 >= 160 && d.y1 >= 28, "the blurred bar is repainted whole: %.0f,%.0f %.0f,%.0f", d.x0, d.y0, d.x1, d.y1);
    ui.damage = d;
    Rect r = { (int)fmaxf(0, floorf(d.x0)), (int)fmaxf(0, floorf(d.y0)), (int)fminf(160, ceilf(d.x1)), (int)fminf(100, ceilf(d.y1)) };
    canvas_set_clip(&frame, r);
    paint_root_clear(&frame, root, 0);
    canvas_set_clip(&frame, (Rect){ 0, 0, 160, 100 });
    ui.damage = (Box){ 0, 0, 0, 0 };
    uint32_t *partial = malloc(160 * 28 * 4);
    if (!partial) return;
    memcpy(partial, fb, 160 * 28 * 4);
    render(root, 160, 100);
    int worst = 0;
    for (int i = 0; i < 160 * 28; i++)
        for (int sh = 0; sh < 24; sh += 8) {
            int dv = abs((int)((partial[i] >> sh) & 255) - (int)((fb[i] >> sh) & 255));
            if (dv > worst) worst = dv;
        }
    CHECK(worst <= 1, "menu bar after closing a menu differs from a full repaint by %d", worst);
    free(partial);
}

/* Several damaged rectangles (0.8.2): dots moving far apart on a "map" are repainted one rectangle
 * at a time, and every frame must be pixel-identical to a full repaint, also with a translucent,
 * blurred bar whose margin a dot crosses (the 0.8.1 band rule, test_backdrop_band). */
static uint32_t rng_state = 12345;
static uint32_t rnd(void) { rng_state = rng_state * 1103515245u + 12345u; return rng_state >> 8; }

static int paint_partial(Node *root, int w, int h, int *nrects, float *area) {
    Box d = { 0, 0, 0, 0 };
    if (!paint_damage(root)) { *nrects = 0; *area = 0; return 0; }
    d = ui.damage;
    Box rs[DAMAGE_MAX];
    int n = paint_damage_rects(rs, DAMAGE_MAX);
    *nrects = n;
    *area = 0;
    for (int i = 0; i < n; i++) {
        Box b = rs[i];
        if (b.x0 < d.x0 || b.y0 < d.y0 || b.x1 > d.x1 || b.y1 > d.y1) return -1;     /* outside the box */
        for (int j = i + 1; j < n; j++)
            if (b.x0 < rs[j].x1 && rs[j].x0 < b.x1 && b.y0 < rs[j].y1 && rs[j].y0 < b.y1) return -2;   /* overlap */
        Rect r = { (int)fmaxf(0, floorf(b.x0)), (int)fmaxf(0, floorf(b.y0)), (int)fminf((float)w, ceilf(b.x1)), (int)fminf((float)h, ceilf(b.y1)) };
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        *area += (float)(r.x1 - r.x0) * (r.y1 - r.y0);
        ui.damage = (Box){ (float)r.x0, (float)r.y0, (float)r.x1, (float)r.y1 };
        canvas_set_clip(&frame, r);
        paint_root_clear(&frame, root, 0);
    }
    canvas_set_clip(&frame, (Rect){ 0, 0, w, h });
    ui.damage = (Box){ 0, 0, 0, 0 };
    return 1;
}

static int full_differs(Node *root, int w, int h, int *where) {
    static uint32_t *ref;
    static int rw, rh;
    if (rw != w || rh != h) { free(ref); ref = calloc((size_t)w * h, 4); rw = w; rh = h; }
    if (!ref) return 999;
    Canvas rc;
    canvas_init(&rc, ref, w, h, w);
    ui.damage = (Box){ 0, 0, (float)w, (float)h };
    paint_root_clear(&rc, root, 0);
    ui.damage = (Box){ 0, 0, 0, 0 };
    int worst = 0;
    for (int i = 0; i < w * h; i++)
        for (int sh = 0; sh < 24; sh += 8) {
            int dv = abs((int)((ref[i] >> sh) & 255) - (int)((fb[i] >> sh) & 255));
            if (dv > worst) { worst = dv; *where = i; }
        }
    return worst;
}

static void test_multi_damage(void) {
    enum { W = 640, H = 400, DOTS = 24 };
    paint_multi_damage = 1;                 /* off by default (paint.c); LORD_DAMAGE=multi */
    ui_reset();
    Node *root = view(NULL);
    ui.root = root;
    sn(root, "backgroundColor", 0xff1c1c1e);
    for (int i = 0; i < 16; i++) box_in(root, (float)(i * 40), 0, 14, H, 0xff2c4a7a);      /* something to blur */
    Node *map = box_in(root, 40, 34, 560, 340, 0xff20304a);
    ss(map, "overflow", "hidden");
    sn(map, "borderRadius", 12);
    for (int i = 0; i < 6; i++) box_in(map, (float)(30 + i * 80), (float)(40 + (i % 3) * 70), 50, 30, 0xff3a5a3a);   /* "land" */
    Node *dots[DOTS];
    float dx[DOTS], dy[DOTS];
    for (int i = 0; i < DOTS; i++) {
        dx[i] = (float)(rnd() % 540);
        dy[i] = (float)(rnd() % 300);
        dots[i] = box_in(map, dx[i], dy[i], 6, 6, i % 2 ? 0xd8ff9f0a : 0xd830d158);   /* translucent, round */
        sn(dots[i], "borderRadius", 3);
    }
    Node *barl = layer_node(root, 20, W, H);
    Node *bar = box_in(barl, 0, 0, W, 28, 0x80ffffff);
    sn(bar, "backdropBlur", 10);
    render(root, W, H);
    int multi = 0, worst_all = 0, bad = 0;
    float saved = 0;
    for (int f = 0; f < 40; f++) {
        for (int i = 0; i < DOTS; i++) {
            if (i % 3 == f % 3) continue;            /* not all of them every frame */
            dx[i] += (float)((int)(rnd() % 9) - 4) + 0.5f;
            dy[i] += (float)((int)(rnd() % 7) - 3);
            if (f % 10 == 7 && i == 0) dy[i] = -20;  /* up near the bar: inside its blur margin (the map clips it) */
            if (dx[i] < -6) dx[i] = 540;
            if (dx[i] > 560) dx[i] = 0;
            if (dy[i] > 320 || dy[i] < -20) dy[i] = 100;
            sn(dots[i], "left", dx[i]);
            sn(dots[i], "top", dy[i]);
        }
        if (f % 13 == 5) sn(bar, "backgroundColor", f % 2 ? 0x80ffffff : 0x90f0f0ff);    /* the bar itself */
        if (f % 17 == 9) { Node *m = box_in(barl, 200, 20, 80, 60, 0xffff3030); render(root, W, H); node_destroy(m); }   /* a menu opens, then closes */
        layout_root(root, W, H);
        int n = 0;
        float area = 0;
        int ok = paint_partial(root, W, H, &n, &area);
        if (ok < 0) { bad++; continue; }
        if (n > 1) { multi++; saved += area; }
        int where = 0, worst = full_differs(root, W, H, &where);
        if (worst > worst_all) worst_all = worst;
        CHECK(worst <= 1, "frame %d (%d rects) differs from a full repaint by %d at %d,%d", f, n, worst, where % W, where / W);
    }
    CHECK(!bad, "%d frames had a rectangle outside the box or two that overlap", bad);
    CHECK(multi >= 20, "only %d of 40 frames used several rectangles", multi);
    /* the same with one rectangle forced, for the numbers */
    paint_multi_damage = 0;
    float one = 0;
    int ones = 0;
    for (int f = 0; f < 10; f++) {
        for (int i = 0; i < DOTS; i++) { dx[i] = (float)(rnd() % 540); sn(dots[i], "left", dx[i]); }
        layout_root(root, W, H);
        int n = 0;
        float area = 0;
        paint_partial(root, W, H, &n, &area);
        one += area;
        ones++;
        int where = 0;
        CHECK(n <= 1 && full_differs(root, W, H, &where) <= 1, "one rectangle (LORD_DAMAGE=1) differs from a full repaint");
    }
    paint_multi_damage = 1;
    if (multi && ones) printf("multi-damage: %d frames, %.0f px per frame in rectangles; one box: %.0f px\n", multi, saved / multi, one / ones);
    /* ui.damage set directly (a full repaint): one rectangle, the whole of it */
    damage_add((Box){ 10, 10, 20, 20 });
    ui.damage = (Box){ 0, 0, W, H };
    damage_add((Box){ 100, 100, 110, 110 });
    layout_root(root, W, H);
    paint_damage(root);
    Box rs[DAMAGE_MAX];
    int n = paint_damage_rects(rs, DAMAGE_MAX);
    CHECK(n == 1 && rs[0].x0 <= 0 && rs[0].x1 >= W && rs[0].y1 >= H, "a full repaint set directly stays one rectangle (%d)", n);
    ui.damage = (Box){ 0, 0, 0, 0 };
    /* the list itself: overlaps merge, the cap holds */
    DamageList l = { .n = 0 };
    dlist_add(&l, (Box){ 0, 0, 10, 10 });
    dlist_add(&l, (Box){ 5, 5, 15, 15 });
    dlist_add(&l, (Box){ 100, 100, 110, 110 });
    CHECK(l.n == 2 && l.r[0].x1 == 15 && l.r[1].x0 == 100, "dlist merges overlaps (%d)", l.n);
    for (int i = 0; i < 100; i++) dlist_add(&l, (Box){ (float)(i * 20), 300, (float)(i * 20 + 5), 305 });
    CHECK(l.n <= DAMAGE_MAX, "dlist stays at %d rectangles (%d)", DAMAGE_MAX, l.n);
    int overl = 0;
    for (int i = 0; i < l.n; i++) for (int j = i + 1; j < l.n; j++)
        if (l.r[i].x0 < l.r[j].x1 && l.r[j].x0 < l.r[i].x1 && l.r[i].y0 < l.r[j].y1 && l.r[j].y0 < l.r[i].y1) overl++;
    CHECK(!overl, "dlist never holds two overlapping rectangles (%d)", overl);
    (void)worst_all;
    paint_multi_damage = 0;
}


/* Minimizing a window over the Monitor's moving dots (0.8.2 release): the window is remounted with a
 * shrinking transform towards the Dock, then removed, while the dots keep moving. Every frame,
 * painted as several rectangles, must match a full repaint: once a few small rectangles are the
 * only damage, anything left behind stays on screen (the grey rectangle seen during integration). */
static Node *mk_window(Node *root, float x, float y, float w, float h) {
    Node *win = box_in(root, x, y, w, h, 0xff3a3a3c);
    ss(win, "overflow", "hidden");
    sn(win, "borderRadius", 10);
    sn(win, "shadowColor", 0x80000000);
    sn(win, "shadowBlur", 28);
    sn(win, "shadowOffsetY", 10);
    sn(win, "borderWidth", 1);
    sn(win, "borderColor", 0x40ffffff);
    box_in(win, 0, 0, w, 28, 0xff505052);                /* title bar */
    box_in(win, 8, 36, w - 16, h - 44, 0xff1e1e1e);      /* content */
    return win;
}

static void test_minimize_damage(void) {
    enum { W = 640, H = 400, DOTS = 24 };
    for (int variant = 0; variant < 8; variant++) {
        paint_multi_damage = variant < 4;     /* both: several rectangles (LORD_DAMAGE=multi) and one box */
        ui_reset();
        Node *root = view(NULL);
        ui.root = root;
        sn(root, "backgroundColor", 0xff1c1c1e);
        Node *map = box_in(root, 20, 30, 600, 350, 0xff20304a);
        ss(map, "overflow", "hidden");
        sn(map, "borderRadius", 12);
        Node *dots[DOTS];
        float dx[DOTS], dy[DOTS];
        for (int i = 0; i < DOTS; i++) {
            dx[i] = (float)(rnd() % 580);
            dy[i] = (float)(rnd() % 330);
            dots[i] = box_in(map, dx[i], dy[i], 6, 6, i % 2 ? 0xd8ff9f0a : 0xd830d158);
            sn(dots[i], "borderRadius", 3);
        }
        Node *barl = layer_node(root, 20, W, H);
        Node *bar = box_in(barl, 0, 0, W, 24, 0x80ffffff);
        sn(bar, "backdropBlur", 10);
        Node *dockl = layer_node(root, 10, W, H);
        Node *dock = box_in(dockl, 220, 350, 200, 46, 0x90303034);
        sn(dock, "backdropBlur", 20);
        sn(dock, "borderRadius", 14);
        float wx = 140 + (variant % 4) * 20, wy = 90, ww = 320, wh = 200;
        Node *win = mk_window(root, wx, wy, ww, wh);
        render(root, W, H);
        int steps = 6 + (variant % 4) * 3, bad = 0;
        for (int f = 0; f < 40; f++) {
            for (int i = 0; i < DOTS; i++) {
                if ((i + f) % 4 == 0) continue;
                dx[i] += (float)((int)(rnd() % 9) - 4) + 0.5f;
                dy[i] += (float)((int)(rnd() % 7) - 3);
                if (dx[i] < -6) dx[i] = 580;
                if (dx[i] > 600) dx[i] = 0;
                if (dy[i] > 350 || dy[i] < -6) dy[i] = 100;
                sn(dots[i], "left", dx[i]);
                sn(dots[i], "top", dy[i]);
            }
            if (f == 4) {                                   /* minimize: the same window, remounted */
                node_destroy(win);
                win = mk_window(root, wx, wy, ww, wh);
            }
            if (f >= 4 && f < 4 + steps) {
                float p = (float)(f - 4 + 1) / (float)steps, e = p * p;
                sn(win, "scaleX", 1 - 0.92f * e);
                sn(win, "scaleY", 1 - 0.92f * e);
                sn(win, "translateX", (320 - (wx + ww / 2)) * e);
                sn(win, "translateY", (364 - (wy + wh / 2)) * e);
                sn(win, "opacity", 1 - 0.7f * e);
            }
            if (f == 4 + steps && win) { node_destroy(win); win = NULL; }
            if (f == 30) win = mk_window(root, wx, wy, ww, wh);   /* restored */
            layout_root(root, W, H);
            int n = 0;
            float area = 0;
            if (paint_partial(root, W, H, &n, &area) < 0) { bad++; continue; }
            int where = 0, worst = full_differs(root, W, H, &where);
            CHECK(worst <= 1, "minimize variant %d frame %d (%d rects) differs from a full repaint by %d at %d,%d",
                  variant, f, n, worst, where % W, where / W);
        }
        CHECK(!bad, "minimize variant %d: %d frames with bad rectangles", variant, bad);
    }
    paint_multi_damage = 0;
}

/* A blurred window hidden behind an opaque one (0.8.6, VM 116: a translucent Terminal behind
 * Monitor): the moving dots over it no longer repaint it and its blur margin every frame, in either
 * damage mode, and every frame still matches a full repaint while the window on top moves away,
 * turns translucent, is removed and comes back, and while the hidden one moves. */
static void test_hidden_blur_damage(void) {
    enum { W = 640, H = 400, DOTS = 16 };
    for (int mode = 0; mode < 2; mode++) {
        paint_multi_damage = mode;
        ui_reset();
        Node *root = view(NULL);
        ui.root = root;
        sn(root, "backgroundColor", 0xff1c1c1e);
        for (int i = 0; i < 16; i++) box_in(root, (float)(i * 40), 0, 14, H, 0xff2c4a7a);      /* something to blur */
        Node *term = box_in(root, 120, 90, 380, 220, 0x50202020);                               /* the hidden window */
        ss(term, "overflow", "hidden");
        sn(term, "borderRadius", 10);
        Node *tbody = box_in(term, 0, 24, 380, 196, 0x00000000);
        sn(tbody, "backdropBlur", 14);
        box_in(tbody, 10, 10, 120, 12, 0xffd0d0d0);                                            /* a prompt */
        Node *mon = NULL, *map = NULL, *dots[DOTS];
        float dx[DOTS], dy[DOTS];
        Node *barl = layer_node(root, 20, W, H);
        Node *bar = box_in(barl, 0, 0, W, 24, 0x80ffffff);
        sn(bar, "backdropBlur", 10);
        Node *dock = box_in(barl, 60, H - 50, W - 120, 44, 0x80ffffff);
        sn(dock, "backdropBlur", 10);
        sn(dock, "borderRadius", 12);
        int built = 0, multi = 0, bad = 0, small = 0, quiet_frames = 0;
        for (int f = 0; f < 48; f++) {
            if (!built) {                                /* Monitor over it, its map with moving dots */
                mon = box_in(root, 40, 40, 560, 310, 0xff2a2a2c);
                ss(mon, "overflow", "hidden");
                sn(mon, "borderRadius", 10);
                map = box_in(mon, 100, 50, 360, 220, 0xff20304a);
                ss(map, "overflow", "hidden");
                for (int i = 0; i < DOTS; i++) {
                    dx[i] = (float)(rnd() % 340);
                    dy[i] = (float)(rnd() % 200);
                    dots[i] = box_in(map, dx[i], dy[i], 9, 9, i % 2 ? 0xd8ff9f0a : 0xd830d158);
                    sn(dots[i], "borderRadius", 4);
                }
                render(root, W, H);
                built = 1;
            }
            for (int i = 0; i < DOTS && mon; i++) {
                if (i % 4 != f % 4) continue;           /* a few dots per frame */
                dx[i] += (float)((int)(rnd() % 9) - 4);
                dy[i] += (float)((int)(rnd() % 7) - 3);
                if (dx[i] < 0 || dx[i] > 350) dx[i] = 170;
                if (dy[i] < 0 || dy[i] > 210) dy[i] = 100;
                sn(dots[i], "left", dx[i]);
                sn(dots[i], "top", dy[i]);
            }
            if (f == 14) { sn(term, "left", 130); sn(term, "top", 96); }       /* the hidden one moves */
            if (f == 18) sn(mon, "left", 300);                                  /* Monitor moves: the Terminal shows */
            if (f == 22) sn(mon, "left", 40);                                   /* and back over it */
            if (f == 26) sn(mon, "opacity", 0.9);                               /* translucent: it shows through */
            if (f == 28) sn(mon, "opacity", 1);
            if (f == 32) { node_destroy(mon); mon = NULL; }                     /* closed */
            if (f == 36) built = 0;                                             /* reopened next frame */
            layout_root(root, W, H);
            int n = 0;
            float area = 0;
            if (paint_partial(root, W, H, &n, &area) < 0) { bad++; continue; }
            int where = 0, worst = full_differs(root, W, H, &where);
            CHECK(worst <= 1, "hidden blur mode %d frame %d (%d rects) differs from a full repaint by %d at %d,%d",
                  mode, f, n, worst, where % W, where / W);
            /* frames where only dots moved under a covered Terminal */
            int still = (f >= 2 && f < 14) || (f >= 40);
            if (still && mon) {
                quiet_frames++;
                if (area <= (mode ? 0.05f * W * H : 360.0f * 220)) small++;   /* several: a few dots; one box: within the map */
                if (n > 1) multi++;
            }
        }
        CHECK(!bad, "hidden blur mode %d: %d frames with bad rectangles", mode, bad);
        CHECK(small == quiet_frames, "hidden blur mode %d: %d of %d frames repainted the hidden window", mode, quiet_frames - small, quiet_frames);
        if (mode) CHECK(multi >= quiet_frames / 2, "hidden blur: only %d of %d frames used several rectangles", multi, quiet_frames);
        /* the rule still holds for a visible one: Monitor away, a dot next to the Terminal repaints it whole */
        if (mon) node_destroy(mon);
        render(root, W, H);
        Node *dot = box_in(root, 112, 150, 9, 9, 0xd8ff9f0a);      /* inside the body's blur margin */
        layout_root(root, W, H);
        int n = 0;
        float area = 0;
        paint_partial(root, W, H, &n, &area);
        int where = 0;
        CHECK(area >= 380.0f * 196 && full_differs(root, W, H, &where) <= 1, "hidden blur mode %d: a visible blurred window is still repainted whole (%.0f px)", mode, area);
        (void)dot;
    }
    paint_multi_damage = 0;
}

/* ---- lord-server's whole CPU damage pipeline (main.c), modelled ----
 * The tests above paint each frame's rectangles straight into one canvas. lord-server does more:
 * paint_ui adds each frame's rectangles to the display's pending list (o->pl, o->pend) and a display
 * whose page flip is still pending keeps gathering them; paint_out paints the gathered list into
 * the scene; present_cpu copies into the KMS back buffer what that buffer is missing (what the
 * other buffer got since, as a box and as rectangles), draws the cursor and flips. This copy of
 * that bookkeeping (kept in step with main.c: paint_ui, paint_out, present_cpu) checks every
 * painted scene against a full repaint and every presented buffer against the scene. */
enum { SW = 1280, SH = 800 };
typedef struct {
    Box pend;
    DamageList pl;
    int full, pframes;
    uint32_t *scene;
    Canvas sc;
    uint32_t *buf[2];
    int front, flip_pending;
    Rect stale[2], curs[2];
    DamageList pstale[2];
    Box painted[DAMAGE_MAX];
    int npainted, full_copy;
} SimOut;

static Rect sim_rjoin(Rect a, Rect b) {
    if (a.x1 <= a.x0 || a.y1 <= a.y0) return b;
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return a;
    return (Rect){ a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
}
static Box sim_bjoin(Box a, Box b) {
    if (a.x1 <= a.x0 || a.y1 <= a.y0) return b;
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return a;
    return (Box){ fminf(a.x0, b.x0), fminf(a.y0, b.y0), fmaxf(a.x1, b.x1), fmaxf(a.y1, b.y1) };
}
static int sim_bempty(Box b) { return b.x1 <= b.x0 || b.y1 <= b.y0; }

static void sim_init(SimOut *o) {
    memset(o, 0, sizeof *o);
    o->scene = calloc((size_t)SW * SH, 4);
    o->buf[0] = calloc((size_t)SW * SH, 4);
    o->buf[1] = calloc((size_t)SW * SH, 4);
    canvas_init(&o->sc, o->scene, SW, SH, SW);
    o->full = 1;
    o->full_copy = 2;
}
static void sim_free(SimOut *o) { free(o->scene); free(o->buf[0]); free(o->buf[1]); }

/* main.c paint_ui: layout, damage, and the damage goes to the display's pending list */
static void sim_paint_ui(SimOut *o, Node *root) {
    layout_root(root, SW, SH);
    Box d = { 0, 0, 0, 0 };
    if (paint_damage(root)) d = ui.damage;
    ui.damage = (Box){ 0, 0, 0, 0 };
    if (sim_bempty(d)) return;
    Box rs[DAMAGE_MAX];
    int nr = paint_damage_rects(rs, DAMAGE_MAX);
    if (sim_bempty(o->pend)) { o->pl.n = 0; o->pframes = 0; }
    for (int k = 0; k < nr; k++) dlist_add(&o->pl, rs[k]);
    o->pend = sim_bjoin(o->pend, d);
    o->pframes++;
}

/* main.c paint_out at scale 1 */
static Rect sim_paint_out(SimOut *o, Node *root, int *nparts_out) {
    Rect r = { 0, 0, 0, 0 };
    Box b = o->pend;
    o->npainted = 0;
    *nparts_out = 0;
    if (o->full) b = (Box){ 0, 0, SW, SH };
    int whole = o->full;
    o->full = 0;
    o->pend = (Box){ 0, 0, 0, 0 };
    if (sim_bempty(b)) return r;
    Box lb = o->pl.n ? o->pl.r[0] : (Box){ 0, 0, 0, 0 };
    for (int k = 1; k < o->pl.n; k++) lb = sim_bjoin(lb, o->pl.r[k]);
    int multi = !whole && o->pl.n > 1 && lb.x0 == b.x0 && lb.y0 == b.y0 && lb.x1 == b.x1 && lb.y1 == b.y1;
    DamageList l = { .n = 0 };
    if (multi) l = o->pl;
    else if (!whole) dlist_add(&l, b);
    if (!whole && o->pframes > 1 && !getenv("SIM_NO_SETTLE")) paint_damage_settle(&l);
    if (!whole && l.n) {
        b = l.r[0];
        for (int k = 1; k < l.n; k++) b = sim_bjoin(b, l.r[k]);
    }
    multi = !whole && l.n > 1;
    int nparts = multi ? l.n : 1;
    Box parts[DAMAGE_MAX];
    if (multi) memcpy(parts, l.r, sizeof(Box) * (size_t)nparts);
    else parts[0] = b;
    o->pl.n = 0;
    o->pframes = 0;
    for (int k = 0; k < nparts; k++) {
        Box q = parts[k];
        Rect pr = { (int)floorf(fmaxf(q.x0, 0)), (int)floorf(fmaxf(q.y0, 0)), (int)ceilf(fminf(q.x1, SW)), (int)ceilf(fminf(q.y1, SH)) };
        if (whole) pr = (Rect){ 0, 0, SW, SH };
        if (pr.x1 <= pr.x0 || pr.y1 <= pr.y0) continue;
        ui.damage = (Box){ (float)pr.x0, (float)pr.y0, (float)pr.x1, (float)pr.y1 };
        canvas_set_clip(&o->sc, pr);
        canvas_clear(&o->sc, 0x000000);
        paint_root(&o->sc, root);
        r = sim_rjoin(r, pr);
        o->painted[o->npainted++] = (Box){ (float)pr.x0, (float)pr.y0, (float)pr.x1, (float)pr.y1 };
    }
    *nparts_out = nparts;
    canvas_set_clip(&o->sc, (Rect){ 0, 0, SW, SH });
    ui.damage = (Box){ 0, 0, 0, 0 };
    return r;
}

/* main.c present_cpu (no rotation, no Night Light): returns the back buffer it drew */
static int sim_present(SimOut *o, Rect r, Rect cur) {
    int bi = o->front ^ 1;
    uint32_t *out = o->buf[bi];
    if (o->full_copy > 0) {
        memcpy(out, o->scene, (size_t)SW * SH * 4);
        o->full_copy--;
    } else {
        Rect need = sim_rjoin(sim_rjoin(o->stale[bi], r), o->curs[bi]);
        if (need.x0 < 0) need.x0 = 0;
        if (need.y0 < 0) need.y0 = 0;
        if (need.x1 > SW) need.x1 = SW;
        if (need.y1 > SH) need.y1 = SH;
        DamageList cp = o->pstale[bi];
        for (int i = 0; i < o->npainted; i++) dlist_add(&cp, o->painted[i]);
        Rect cu = o->curs[bi];
        if (cu.x1 > cu.x0 && cu.y1 > cu.y0) dlist_add(&cp, (Box){ (float)cu.x0, (float)cu.y0, (float)cu.x1, (float)cu.y1 });
        Box cb = cp.n ? cp.r[0] : (Box){ 0, 0, 0, 0 };
        for (int i = 1; i < cp.n; i++) cb = sim_bjoin(cb, cp.r[i]);
        Rect cbr = { (int)cb.x0, (int)cb.y0, (int)cb.x1, (int)cb.y1 };
        Rect nj = sim_rjoin(sim_rjoin(o->stale[bi], r), o->curs[bi]);
        if (paint_multi_damage && cp.n > 1 && cbr.x0 == nj.x0 && cbr.y0 == nj.y0 && cbr.x1 == nj.x1 && cbr.y1 == nj.y1) {
            for (int i = 0; i < cp.n; i++) {
                Rect q = { (int)cp.r[i].x0, (int)cp.r[i].y0, (int)cp.r[i].x1, (int)cp.r[i].y1 };
                if (q.x0 < 0) q.x0 = 0;
                if (q.y0 < 0) q.y0 = 0;
                if (q.x1 > SW) q.x1 = SW;
                if (q.y1 > SH) q.y1 = SH;
                for (int y = q.y0; y < q.y1; y++) memcpy(out + (size_t)y * SW + q.x0, o->scene + (size_t)y * SW + q.x0, (size_t)(q.x1 - q.x0) * 4);
            }
        } else
            for (int y = need.y0; y < need.y1; y++) memcpy(out + (size_t)y * SW + need.x0, o->scene + (size_t)y * SW + need.x0, (size_t)(need.x1 - need.x0) * 4);
    }
    o->stale[bi] = (Rect){ 0, 0, 0, 0 };
    o->stale[bi ^ 1] = sim_rjoin(o->stale[bi ^ 1], r);
    o->pstale[bi].n = 0;
    for (int i = 0; i < o->npainted; i++) dlist_add(&o->pstale[bi ^ 1], o->painted[i]);
    o->curs[bi] = (Rect){ 0, 0, 0, 0 };
    if (cur.x1 > cur.x0) {                      /* the arrow: an opaque square */
        for (int y = cur.y0; y < cur.y1; y++) for (int x = cur.x0; x < cur.x1; x++) out[(size_t)y * SW + x] = 0xffff00ff;
        o->curs[bi] = cur;
    }
    return bi;
}

static uint32_t *sim_ref;
static int sim_bad_px;                  /* pixels off by more than 24 levels in the last comparison */
/* a full repaint of the tree as it is now; worst channel difference from px, and where */
static int sim_full_differs(Node *root, const uint32_t *px, int *where) {
    if (!sim_ref) sim_ref = calloc((size_t)SW * SH, 4);
    if (!sim_ref) return 999;
    Canvas rc;
    canvas_init(&rc, sim_ref, SW, SH, SW);
    ui.damage = (Box){ 0, 0, SW, SH };
    canvas_clear(&rc, 0);
    paint_tf_nocache = 1;               /* the truth: no raster from the cache, and the cache untouched */
    paint_root(&rc, root);
    paint_tf_nocache = 0;
    ui.damage = (Box){ 0, 0, 0, 0 };
    int worst = 0;
    sim_bad_px = 0;
    for (int i = 0; i < SW * SH; i++) {
        uint32_t a = sim_ref[i], b = px[i];
        if (a == b) continue;
        int big = 0;
        for (int sh = 0; sh < 24; sh += 8) {
            int dv = abs((int)((a >> sh) & 255) - (int)((b >> sh) & 255));
            if (dv > worst) { worst = dv; *where = i; }
            if (dv > 24) big = 1;
        }
        sim_bad_px += big;
    }
    return worst;
}

/* The desktop of the 0.8.2 report: Monitor's moving map, a translucent, blurred Terminal in front of
 * it, minimized into a blurred Dock (the default theme: WindowShell's frame, fxStyle's transform),
 * under a blurred menu bar. */
typedef struct { Node *shell, *inner, *title; } SimWin;
static SimWin sim_window(Node *parent, float x, float y, float w, float h, int terminal, int with_title) {
    SimWin sw;
    const float m = 6;                           /* WINDOW_EDGE: the invisible resize borders */
    sw.shell = box_in(parent, x - m, y - m, w + 2 * m, h + 2 * m, 0);
    sw.inner = box_in(sw.shell, m, m, w, h, terminal ? 0 : 0xff1e1e20);
    ss(sw.inner, "overflow", "hidden");
    sn(sw.inner, "borderRadius", 12);
    sn(sw.inner, "borderWidth", 1);
    sn(sw.inner, "borderColor", terminal ? 0xff3a3a3d : 0x33ffffff);
    sn(sw.inner, "shadowColor", 0x8c000000);
    sn(sw.inner, "shadowBlur", 56);
    sn(sw.inner, "shadowOffsetY", 24);
    Node *bar = box_in(sw.inner, 0, 0, w, 40, 0xff2c2c2f);
    sw.title = with_title ? text(bar, terminal ? "lord@lordos: ~" : "Monitor", 13) : NULL;
    if (sw.title) { sn(sw.title, "color", 0xffe0e0e0); ss(sw.title, "position", "absolute"); sn(sw.title, "left", w / 2 - 50); sn(sw.title, "top", 12); }
    static const uint32_t lights[3] = { 0xffff5f57, 0xfffebc2e, 0xff28c840 };
    for (int i = 0; i < 3; i++) { Node *l = box_in(bar, 14 + i * 21, 13, 13, 13, lights[i]); sn(l, "borderRadius", 7); }
    box_in(sw.inner, 0, 40, w, 1, terminal ? 0xff3a3a3d : 0x22ffffff);
    if (terminal) {
        Node *pane = box_in(sw.inner, 0, 41, w, h - 41, 0xc81e1e1e);   /* translucent profile, with blur */
        ss(pane, "overflow", "hidden");
        sn(pane, "backdropBlur", 14);
        for (int i = 0; i < 4; i++) { Node *t = text(pane, "lord@lordos:~$ ls -la /usr/share", 13); sn(t, "color", 0xffd0d0d0); ss(t, "position", "absolute"); sn(t, "left", 8); sn(t, "top", 6 + i * 18); }
    }
    return sw;
}

typedef struct { int pend_pct, seed, frames; } SimPlan;

/* Returns the number of frames that differed. */
static int sim_run(const SimPlan *pl, int *painted_frames, int *multi_frames) {
    enum { DOTS = 30 };
    rng_state = (uint32_t)pl->seed * 2654435761u + 7;
    ui_reset();
    Node *root = view(NULL);
    ui.root = root;
    sn(root, "backgroundColor", 0xff35507a);
    box_in(root, 0, 0, SW, SH, 0xff35507a);       /* the wallpaper image */
    for (int i = 0; i < 12; i++) box_in(root, (float)(i * 110), 0, 40, SH, 0xff2a4466);
    SimWin mon = sim_window(root, 70, 60, 960, 620, 0, 1);
    Node *map = box_in(mon.inner, 16, 56, 928, 548, 0xff26282c);
    ss(map, "overflow", "hidden");
    sn(map, "borderRadius", 8);
    for (int i = 0; i < 9; i++) box_in(map, (float)(40 + (i % 5) * 170), (float)(60 + (i / 5) * 220), 120, 140, 0xff3b4f3b);
    Node *dots[DOTS];
    float dx[DOTS], dy[DOTS];
    for (int i = 0; i < DOTS; i++) {
        dx[i] = (float)(rnd() % 900);
        dy[i] = (float)(rnd() % 520);
        dots[i] = box_in(map, dx[i], dy[i], 9, 9, i % 2 ? 0xd8ff9f0a : 0xd830d158);
        sn(dots[i], "borderRadius", 4.5);
    }
    Node *pill = box_in(map, 300, 200, 90, 18, 0xe01c1d21);
    sn(pill, "borderRadius", 9);
    float tx = 260 + (float)(rnd() % 300), ty = 140 + (float)(rnd() % 200), tw = 640, th = 400;
    SimWin term = sim_window(root, tx, ty, tw, th, 1, 1);
    Node *bar = box_in(layer_node(root, 20, SW, SH), 0, 0, SW, 28, 0x80202024);
    sn(bar, "backdropBlur", 20);
    text(bar, "Terminal   File   Edit   View", 13);
    Node *dockl = layer_node(root, 10, SW, SH);
    Node *dock = box_in(dockl, SW / 2 - 300, SH - 70, 600, 62, 0x90303034);
    sn(dock, "backdropBlur", 20);
    sn(dock, "borderRadius", 16);
    for (int i = 0; i < 10; i++) { Node *ic = box_in(dock, 8 + i * 58, 7, 48, 48, 0xff5080c0 + (uint32_t)i * 0x101010); sn(ic, "borderRadius", 11); }
    Node *popl = layer_node(root, 50, SW, SH), *menu = NULL;
    SimOut o;
    sim_init(&o);
    int bad = 0, cx = 600, cy = 400;
    int state = 0, t0 = 0, steps = 0;           /* 0 shown, 1 gone one frame, 2 shrinking, 3 minimized */
    *painted_frames = *multi_frames = 0;
    for (int f = 0; f < pl->frames; f++) {
        /* the moving dots (12 fps in Monitor, here every frame) */
        for (int i = 0; i < DOTS; i++) {
            if (rnd() % 4 == 0) continue;
            dx[i] += (float)((int)(rnd() % 9) - 4);
            dy[i] += (float)((int)(rnd() % 7) - 3);
            if (dx[i] < -9 || dx[i] > 930) dx[i] = (float)(rnd() % 900);
            if (dy[i] < -9 || dy[i] > 550) dy[i] = (float)(rnd() % 520);
            sn(dots[i], "left", dx[i]);
            sn(dots[i], "top", dy[i]);
        }
        if (rnd() % 6 == 0) { sn(pill, "left", (float)(rnd() % 800)); sn(pill, "top", (float)(rnd() % 500)); }
        /* the Terminal: minimize (wm.minimize, then useMinimizing's remount with fxStyle), restore */
        if (state == 0 && rnd() % 25 == 0) {
            node_destroy(term.shell);
            state = 1;
        } else if (state == 1) {
            term = sim_window(root, tx, ty, tw, th, 1, 1);
            state = 2;
            t0 = f;
            steps = 6 + (int)(rnd() % 14);
        }
        if (state == 2) {
            float p = fminf(1, (float)(f - t0 + 1) / (float)steps), e = p * p;
            float fx = SW / 2.0f, fy = SH - 36.0f;
            sn(term.inner, "translateX", (fx - (tx + tw / 2)) * e);
            sn(term.inner, "translateY", (fy - (ty + th / 2)) * e);
            sn(term.inner, "scaleX", 1 - 0.9f * e);
            sn(term.inner, "scaleY", 1 - 0.9f * e);
            sn(term.inner, "opacity", 1 - 0.7f * e);
            if (f - t0 >= steps) { node_destroy(term.shell); state = 3; t0 = f; }
        } else if (state == 3 && f - t0 > 8 && rnd() % 10 == 0) {
            tx = 200 + (float)(rnd() % 400);
            ty = 100 + (float)(rnd() % 250);
            term = sim_window(root, tx, ty, tw, th, 1, 1);
            state = 0;
        }
        if (state == 0 && term.title && rnd() % 9 == 0) node_set_text(term.title->first, rnd() % 2 ? "lord@lordos: ~/src" : "lord@lordos: ~");
        if (rnd() % 20 == 0) {                   /* a menu opens or closes */
            if (menu) { node_destroy(menu); menu = NULL; }
            else { menu = box_in(popl, (float)(rnd() % 1000), 24, 220, 260, 0xf0303034); sn(menu, "borderRadius", 8); sn(menu, "shadowColor", 0x60000000); sn(menu, "shadowBlur", 20); }
        }
        if (rnd() % 3 == 0) { cx = (int)(rnd() % (SW - 16)); cy = (int)(rnd() % (SH - 16)); }
        /* the frame: damage always; painted and presented only when the flip is not pending */
        sim_paint_ui(&o, root);
        if (o.flip_pending) {
            if ((int)(rnd() % 100) < 50) { o.flip_pending = 0; o.front ^= 1; }
            else continue;
        }
        int nparts = 0;
        Rect r = sim_paint_out(&o, root, &nparts);
        if (r.x1 > r.x0) { (*painted_frames)++; if (nparts > 1) (*multi_frames)++; }
        int where = 0, worst = sim_full_differs(root, o.scene, &where);
        if (worst > 2) {
            if (bad < 4) printf("  pipeline seed %d frame %d (state %d, %d parts): scene differs from a full repaint by %d at %d,%d\n",
                                pl->seed, f, state, nparts, worst, where % SW, where / SW);
            bad++;
        }
        Rect cur = { cx, cy, cx + 12, cy + 18 };
        int bi = sim_present(&o, r, cur);
        int off = 0, at = -1;
        for (int y = 0; y < SH && at < 0; y++)
            for (int x = 0; x < SW; x++) {
                int in = x >= cur.x0 && x < cur.x1 && y >= cur.y0 && y < cur.y1;
                uint32_t want = in ? 0xffff00ff : o.scene[(size_t)y * SW + x];
                if (o.buf[bi][(size_t)y * SW + x] != want) { off++; at = y * SW + x; break; }
            }
        if (off) {
            if (bad < 4) printf("  pipeline seed %d frame %d: the presented buffer %d differs from the scene at %d,%d\n", pl->seed, f, bi, at % SW, at / SW);
            bad++;
        }
        if ((int)(rnd() % 100) < pl->pend_pct) o.flip_pending = 1;
        else o.front = bi;
    }
    sim_free(&o);
    return bad;
}

static void test_damage_pipeline(void) {
    int total_bad = 0, painted = 0, multi = 0;
    for (int mode = 1; mode >= 0; mode--)
        for (int seed = 0; seed < 6; seed++) {
            paint_multi_damage = mode;
            SimPlan pl = { (int[]){ 0, 30, 60 }[seed % 3], seed + 100 * mode, 120 };
            int p = 0, m = 0, bad = sim_run(&pl, &p, &m);
            CHECK(!bad, "pipeline (multi %d, seed %d, %d%% of flips pending): %d frames differ", mode, seed, pl.pend_pct, bad);
            total_bad += bad;
            if (mode) { painted += p; multi += m; }
        }
    printf("pipeline: %d frames painted with several rectangles on, %d of them in several parts\n", painted, multi);
    paint_multi_damage = 0;
    (void)total_bad;
}

/* Random trees and random changes through the same pipeline (0.8.6): boxes that clip or not,
 * rounded, with shadows, translucent, blurred, scaled or rotated, faded, raised with zIndex, hidden,
 * moved, resized, re-parented, destroyed; small changes far apart, so most frames are several
 * rectangles, and 40 % of the flips pending in half the seeds. Every painted frame is compared with
 * a full repaint that doesn't use the transformed subtrees' raster cache (paint_tf_nocache), and
 * every presented buffer with the scene. It found, in both damage modes: a parent's border lost
 * where its opaque child was the start of a partial repaint; cached rasters kept after a change
 * made while hidden or unscaled; a raster grid that moved with the subtree's extent; blurs that
 * took in what is painted above them from outside the repainted area; a raster that changed kind
 * (a blur coming in) outside the damage; several frames' rectangles that cut through a blurred
 * node; a transform inside another one whose changes never reached the outer raster's cache.
 * What is left (FZ_STRICT=1, 24 seeds: one seed in each mode, the same with one box or several):
 * a 1-pixel row of a rotated subtree moved into another transformed subtree (up to 63 levels,
 * 17 px), and the antialiased edge of a blurred node inside a scaled subtree without
 * overflow: hidden, which moves by up to ~13 levels when the subtree's extent changes; both stay
 * until something repaints them. The 12 seeds run by default hit neither (a frame fails here with
 * a pixel off by more than 24 levels).
 * Environment: FUZZ_SEEDS, FZ_ONLY=seed, FZ_MULTI=0|1, FZ_NO, FZ_INIT=nodes, FZ_STRICT, FZ_AREA,
 * FZ_LOG (the changes), FZ_DEBUG (fz_explain). */
#define FZ_MAX 160
static Node *fz_nodes[FZ_MAX];
static int fz_n;

static float frand(float a, float b) { return a + (b - a) * (float)(rnd() % 10001) / 10000.0f; }
/* FZ_NO=blur,shadow,scale,rotate,nest leaves those out (nest: a transform inside another one) */
static int fz_no(const char *what) { const char *e = getenv("FZ_NO"); return e && strstr(e, what) != NULL; }

static int fz_has_tf(const Node *n) { return n->s.ext && (n->s.ext->scale_x != 1 || n->s.ext->scale_y != 1 || n->s.ext->rotate != 0); }
static int fz_sub_tf(const Node *n) { if (fz_has_tf(n)) return 1; for (const Node *c = n->first; c; c = c->next) if (fz_sub_tf(c)) return 1; return 0; }
/* FZ_NO=nest: a transform only where no ancestor or descendant has one */
static int fz_nest_ok(const Node *n) {
    if (!fz_no("nest")) return 1;
    for (const Node *p = n->parent; p; p = p->parent) if (fz_has_tf(p)) return 0;
    for (const Node *c = n->first; c; c = c->next) if (fz_sub_tf(c)) return 0;
    return 1;
}

static void fz_style(Node *b) {
    int r = (int)(rnd() % 100);
    if (r < 35) ss(b, "overflow", "hidden");
    if (rnd() % 3 == 0) sn(b, "borderRadius", frand(0, 20));
    if (rnd() % 5 == 0 && !fz_no("shadow")) { sn(b, "shadowColor", 0x70000000); sn(b, "shadowBlur", frand(2, 40)); sn(b, "shadowOffsetY", frand(-6, 16)); }
    if (rnd() % 7 == 0 && !fz_no("blur")) sn(b, "backdropBlur", frand(2, 24));
    if (rnd() % 6 == 0) sn(b, "opacity", frand(0.2f, 1));
    if (rnd() % 6 == 0 && !fz_no("scale") && fz_nest_ok(b)) { float s = frand(0.1f, 1.3f); sn(b, "scaleX", s); sn(b, "scaleY", s); }
    if (rnd() % 14 == 0 && !fz_no("rotate") && fz_nest_ok(b)) sn(b, "rotate", frand(-30, 30));
    if (rnd() % 8 == 0) sn(b, "zIndex", (float)(rnd() % 5));
    if (rnd() % 6 == 0) { sn(b, "borderWidth", 1); sn(b, "borderColor", 0x40ffffff); }
}

static void fz_forget(Node *n) {
    for (int i = 0; i < fz_n; i++) if (fz_nodes[i] == n) { fz_nodes[i] = fz_nodes[--fz_n]; i--; }
    for (Node *c = n->first; c; c = c->next) fz_forget(c);
}

static Node *fz_add(Node *root) {
    if (fz_n >= FZ_MAX) return NULL;
    Node *p = fz_n && rnd() % 3 ? fz_nodes[rnd() % (unsigned)fz_n] : root;
    if (p->type == NODE_TEXT) p = root;
    uint32_t a = rnd() % 3 == 0 ? 0xff000000u : (uint32_t)(0x60 + rnd() % 0x9f) << 24;
    Node *b = box_in(p, frand(-20, p == root ? SW - 40 : p->w), frand(-20, p == root ? SH - 40 : p->h), frand(4, 300), frand(4, 220),
                     a | (rnd() & 0xffffff));
    fz_style(b);
    if (rnd() % 6 == 0) { Node *t = text(b, rnd() % 2 ? "Quarterly report" : "\xd8\xaa\xd9\x82\xd8\xb1\xd9\x8a\xd8\xb1 abc", 13); sn(t, "color", 0xffffffff); }
    else fz_nodes[fz_n++] = b;
    return b;
}

static void fz_step(Node *root) {
    int ops = 1 + (int)(rnd() % 4);
    for (int k = 0; k < ops; k++) {
        int op = (int)(rnd() % 100);
        if (!fz_n || op < 12) { fz_add(root); continue; }
        Node *n = fz_nodes[rnd() % (unsigned)fz_n];
        if (getenv("FZ_LOG")) printf("      op %d on node %d (parent %d)\n", op, n->id, n->parent ? n->parent->id : 0);
        if (op < 22) { fz_forget(n); node_destroy(n); }
        else if (op < 50) { sn(n, "left", n->x + frand(-12, 12)); sn(n, "top", n->y + frand(-12, 12)); }
        else if (op < 58) { sn(n, "width", frand(4, 300)); sn(n, "height", frand(4, 220)); }
        else if (op < 66) { float s = frand(0.1f, 1.3f); if (!fz_no("scale") && fz_nest_ok(n)) { sn(n, "scaleX", s); sn(n, "scaleY", s); } sn(n, "translateX", frand(-40, 40)); }
        else if (op < 72) sn(n, "opacity", frand(0, 1));
        else if (op < 78) ss(n, "display", n->s.display_none ? "flex" : "none");
        else if (op < 84) sn(n, "backgroundColor", (uint32_t)(0x60 + rnd() % 0xa0) << 24 | (rnd() & 0xffffff));
        else if (op < 88) sn(n, "zIndex", (float)(rnd() % 5));
        else if (op < 92 && fz_n > 1) {           /* re-parented (React moving a node) */
            Node *p = fz_nodes[rnd() % (unsigned)fz_n];
            int cycle = 0;
            for (Node *q = p; q; q = q->parent) if (q == n) cycle = 1;
            if (fz_no("nest") && fz_sub_tf(n)) for (Node *q = p; q; q = q->parent) if (fz_has_tf(q)) cycle = 1;
            if (!cycle && p->type != NODE_TEXT) node_append(p, n);
        } else if (op < 96) { float v = frand(0, 40); if (!fz_no("shadow")) sn(n, "shadowBlur", v); }
        else { float v = rnd() % 2 ? 0 : frand(2, 20); if (!fz_no("blur")) sn(n, "backdropBlur", v); }
    }
}

/* FZ_DEBUG: what the first differing pixel of a seed is made of (the damage, the nodes over it,
 * which ones the reference depends on, and the partial paint replayed three ways). */
static void fz_explain(Node *root, SimOut *op, const uint32_t *before, DamageList keep, Box keepb, int where) {
    SimOut o = *op;
    int px = where % SW, py = where / SW;
    printf("    pend %.0f,%.0f %.0f,%.0f; parts:", keepb.x0, keepb.y0, keepb.x1, keepb.y1);
    for (int i = 0; i < keep.n; i++) printf(" [%.0f,%.0f %.0f,%.0f]", keep.r[i].x0, keep.r[i].y0, keep.r[i].x1, keep.r[i].y1);
    printf("\n    scene %08x ref %08x\n", o.scene[where], sim_ref[where]);
    uint32_t want = sim_ref[where];
    { int outside = 0, first = -1; for (int i = 0; i < SW * SH; i++) { int x = i % SW, y = i / SW, in = 0; for (int k = 0; k < o.npainted; k++) if (x >= o.painted[k].x0 && x < o.painted[k].x1 && y >= o.painted[k].y0 && y < o.painted[k].y1) in = 1; if (!in && before[i] != sim_ref[i]) { outside++; if (first < 0) first = i; } }
      printf("    outside the parts, the scene before this frame differs from the reference in %d px (first %d,%d)\n", outside, first % SW, first / SW); }
    { static uint32_t *t; if (!t) t = malloc((size_t)SW * SH * 4); Canvas tc; canvas_init(&tc, t, SW, SH, SW); Rect pr = { (int)o.painted[0].x0, (int)o.painted[0].y0, (int)o.painted[0].x1, (int)o.painted[0].y1 };
      memcpy(t, before, (size_t)SW * SH * 4); ui.damage = o.painted[0]; canvas_set_clip(&tc, pr); canvas_clear(&tc, 0); paint_tf_nocache = 1; paint_root(&tc, root); printf("    replay part (damage part, clip part): %08x\n", t[where]);
      memcpy(t, before, (size_t)SW * SH * 4); ui.damage = (Box){ 0, 0, SW, SH }; canvas_set_clip(&tc, pr); canvas_clear(&tc, 0); paint_root(&tc, root); printf("    replay part (damage full, clip part): %08x\n", t[where]);
      memcpy(t, before, (size_t)SW * SH * 4); ui.damage = o.painted[0]; canvas_set_clip(&tc, (Rect){0,0,SW,SH}); canvas_clear(&tc, 0); paint_root(&tc, root); printf("    replay (damage part, clip full): %08x\n", t[where]);
      paint_tf_nocache = 0; ui.damage = (Box){0,0,0,0}; }
    { int keepm = paint_multi_damage, w3 = 0; paint_multi_damage = 0; sim_full_differs(root, o.scene, &w3); printf("    reference without cull: %08x\n", sim_ref[where]); paint_multi_damage = keepm; sim_full_differs(root, o.scene, &w3); }
    for (int dy2 = -3; dy2 <= 3; dy2++) { printf("    row %d:", dy2); for (int dx2 = -3; dx2 <= 3; dx2++) printf(" %06x/%06x", sim_ref[where + dy2 * SW + dx2] & 0xffffff, o.scene[where + dy2 * SW + dx2] & 0xffffff); printf("\n"); }
    for (int i = 2; i < ui.cap; i++) {
        Node *n = ui.nodes[i];
        if (!n || n->type == NODE_RAW || n->s.display_none) continue;
        n->s.display_none = 1;
        int w2 = 0;
        sim_full_differs(root, o.scene, &w2);
        if (sim_ref[where] != want) printf("    hiding node %d changes the reference pixel to %08x\n", i, sim_ref[where]);
        n->s.display_none = 0;
    }
    sim_full_differs(root, o.scene, &where);
    for (int i = 0; i < ui.cap; i++) {
        Node *n = ui.nodes[i];
        if (!n || n->type == NODE_RAW) continue;
        Box b = n->prev_box;
        int inl = px >= n->ax && px < n->ax + n->w && py >= n->ay && py < n->ay + n->h;
        if ((px < b.x0 || px >= b.x1 || py < b.y0 || py >= b.y1) && !inl) continue;
        printf("    chain:");
        for (Node *q = n->parent; q; q = q->parent) printf(" %d%s%s[op %.2f bg %08x at %.1f,%.1f %.1fx%.1f r %.1f]", q->id, q->s.overflow_hidden ? "(ovf)" : "", q->s.display_none ? "(none)" : "",
                                                            q->s.opacity, q->s.bg, q->ax, q->ay, q->w, q->h, q->s.radius);
        printf("  kids:");
        for (Node *q = n->first; q; q = q->next) printf(" %d", q->id);
        printf("\n");
        printf("    node %d parent %d at %.1f,%.1f %.1fx%.1f prev %.0f,%.0f %.0f,%.0f ovf %d rad %.1f bg %08x op %.2f z %d tf %d/%d blur %.1f\n", n->id, n->parent ? n->parent->id : 0,
               n->ax, n->ay, n->w, n->h, b.x0, b.y0, b.x1, b.y1, n->s.overflow_hidden, n->s.radius, n->s.bg, n->s.opacity, (int)n->s.z_index, n->own_tf, n->has_tf, n->s.backdrop_blur);
    }
}

static int fz_run(int seed, int frames, int pend_pct, int *multi_frames) {
    rng_state = (uint32_t)seed * 2246822519u + 3;
    ui_reset();
    fz_n = 0;
    Node *root = view(NULL);
    ui.root = root;
    sn(root, "backgroundColor", 0xff203040);
    int init = getenv("FZ_INIT") ? atoi(getenv("FZ_INIT")) : 40;
    for (int i = 0; i < init; i++) fz_add(root);
    SimOut o;
    sim_init(&o);
    int bad = 0;
    *multi_frames = 0;
    for (int f = 0; f < frames; f++) {
        if (getenv("FZ_LOG")) printf("    frame %d\n", f);
        fz_step(root);
        sim_paint_ui(&o, root);
        if (o.flip_pending) {
            if ((int)(rnd() % 100) < 50) { o.flip_pending = 0; o.front ^= 1; }
            else continue;
        }
        int nparts = 0;
        static uint32_t *before;
        if (getenv("FZ_DEBUG")) { if (!before) before = malloc((size_t)SW * SH * 4); memcpy(before, o.scene, (size_t)SW * SH * 4); }
        DamageList keep = o.pl;
        Box keepb = o.pend;
        Rect r = sim_paint_out(&o, root, &nparts);
        if (nparts > 1) (*multi_frames)++;
        int where = 0, worst = sim_full_differs(root, o.scene, &where);
        /* a frame fails with a pixel off by more than 24 levels (FZ_STRICT: by more than 2; FZ_AREA=n:
         * with n such pixels); the antialiased edges of a blurred node inside a scaled, unclipped
         * subtree move a few levels with the subtree's extent (its raster's edge), test_damage_fuzz */
        if (getenv("FZ_AREA") ? sim_bad_px < atoi(getenv("FZ_AREA")) : !getenv("FZ_STRICT") && !sim_bad_px) worst = 0;
        if (worst > 2 && getenv("FZ_DEBUG") && !bad) fz_explain(root, &o, before, keep, keepb, where);
        if (worst > 2) {
            if (bad < 3) printf("  fuzz seed %d frame %d (%d parts): differs from a full repaint by %d at %d,%d (%d px off by more than 24)\n", seed, f, nparts, worst, where % SW, where / SW, sim_bad_px);
            bad++;
        }
        int bi = sim_present(&o, r, (Rect){ 0, 0, 0, 0 });
        if (memcmp(o.buf[bi], o.scene, (size_t)SW * SH * 4)) {
            if (bad < 3) printf("  fuzz seed %d frame %d: the presented buffer differs from the scene\n", seed, f);
            bad++;
        }
        if ((int)(rnd() % 100) < pend_pct) o.flip_pending = 1;
        else o.front = bi;
    }
    sim_free(&o);
    return bad;
}

static void test_damage_fuzz(void) {
    int seeds = getenv("FUZZ_SEEDS") ? atoi(getenv("FUZZ_SEEDS")) : 12, multi = 0, failed = 0;
    for (int seed = 0; seed < seeds; seed++) {
        if (getenv("FZ_ONLY") && atoi(getenv("FZ_ONLY")) != seed) continue;
        paint_multi_damage = getenv("FZ_MULTI") ? atoi(getenv("FZ_MULTI")) : seed % 4 != 3;
        int m = 0, bad = fz_run(seed, 60, (int[]){ 0, 40 }[seed % 2], &m);
        multi += m;
        failed += bad != 0;
        CHECK(!bad, "damage fuzz seed %d (multi %d): %d frames differ", seed, paint_multi_damage, bad);
    }
    printf("damage fuzz: %d seeds, %d frames in several parts, %d seeds failed\n", seeds, multi, failed);
    paint_multi_damage = 0;
}


/* A row with a fixed child and one that grows (a list item: bullet + text): the growing child's
 * wrapped height is measured at the width it gets, not the whole row's (0.8.2: the last line of a
 * long "What's new" bullet in Store → Updates drew over the next bullet). */
static void test_grow_wrap_height(void) {
    ui_reset();
    Node *root = view(NULL);
    ui.root = root;
    sn(root, "width", 200);
    Node *row = view(root);
    ss(row, "flexDirection", "row");
    Node *bullet = view(row);
    sn(bullet, "width", 18); sn(bullet, "height", 14);
    Node *text = view(row);
    sn(text, "flexGrow", 1); sn(text, "flexBasis", 0);
    Node *words = view(text);
    ss(words, "flexDirection", "row");
    ss(words, "flexWrap", "wrap");
    for (int i = 0; i < 10; i++) { Node *w = view(words); sn(w, "width", 40); sn(w, "height", 14); }
    Node *next = view(root);
    sn(next, "height", 10);
    layout_root(root, 200, 300);
    /* 182 px for the words: 4 per line, 3 lines (at 200 it would be 5 per line, 2 lines) */
    CHECK(fabsf(words->h - 42) < 0.5f, "wrapped words: %g high, want 42", words->h);
    CHECK(row->h >= words->h - 0.5f, "the row (%g) is shorter than its wrapped text (%g)", row->h, words->h);
    CHECK(next->ay >= row->ay + words->h - 0.5f, "the next item (y %g) overlaps the wrapped text (ends at %g)", next->ay, row->ay + words->h);
}

static void test_transform_paint_hit(void) {
    ui_reset();
    Node *root = view(NULL);
    ui.root = root;
    sn(root, "backgroundColor", 0xffffffff);
    Node *a = view(root);                               /* 40×40 red, scaled 0.5 around its center */
    ss(a, "position", "absolute");
    sn(a, "left", 10); sn(a, "top", 10); sn(a, "width", 40); sn(a, "height", 40);
    sn(a, "backgroundColor", 0xffff0000);
    sn(a, "scaleX", 0.5); sn(a, "scaleY", 0.5);
    Node *ac = view(a);                                 /* a child: transformed with it */
    ss(ac, "position", "absolute");
    sn(ac, "left", 0); sn(ac, "top", 0); sn(ac, "width", 20); sn(ac, "height", 20);
    sn(ac, "backgroundColor", 0xff0000ff);
    Node *b = view(root);                               /* 60×10 green bar rotated 90° */
    ss(b, "position", "absolute");
    sn(b, "left", 70); sn(b, "top", 45); sn(b, "width", 60); sn(b, "height", 10);
    sn(b, "backgroundColor", 0xff00ff00);
    sn(b, "rotate", 90);
    render(root, 160, 100);

    /* layout is unaffected */
    CHECK(a->x == 10 && a->w == 40 && b->w == 60, "layout changed by the transform");
    /* painting: a covers 20..40, its child (top-left quarter) 20..30 */
    CHECK(pix(15, 15) == 0xffffff, "scaled box painted at its layout size: %06x", pix(15, 15));
    CHECK(pix(25, 25) == 0x0000ff && pix(35, 35) == 0xff0000, "scaled box and child: %06x %06x", pix(25, 25), pix(35, 35));
    CHECK(pix(45, 30) == 0xffffff, "scaled box too wide: %06x", pix(45, 30));
    /* the bar: vertical, x 95..105, y 20..80 */
    CHECK(pix(100, 25) == 0x00ff00 && pix(100, 75) == 0x00ff00, "rotated bar: %06x %06x", pix(100, 25), pix(100, 75));
    CHECK(pix(75, 50) == 0xffffff && pix(125, 50) == 0xffffff, "rotated bar still horizontal: %06x", pix(75, 50));

    /* hit testing uses the transformed boxes */
    CHECK(hit_test(root, 15, 15) == root, "hit inside the untransformed box only");
    CHECK(hit_test(root, 25, 25) == ac, "hit the child of a scaled box");
    CHECK(hit_test(root, 35, 35) == a, "hit the scaled box");
    CHECK(hit_test(root, 100, 22) == b, "hit the rotated bar's end");
    CHECK(hit_test(root, 75, 50) == root, "hit where the bar was before rotating");

    /* the raster is reused while only the scale changes within a step, and redone when a child changes */
    long r0, c0;
    paint_tf_stats(&r0, &c0);
    sn(a, "scaleX", 0.48); sn(a, "scaleY", 0.48);
    node_invalidate(a);
    render(root, 160, 100);
    long r1, c1;
    paint_tf_stats(&r1, &c1);
    CHECK(r1 == 0 && c1 == 2, "scale change re-rasterized (%ld renders, %ld composites)", r1, c1);
    sn(ac, "backgroundColor", 0xff000000);
    node_invalidate(ac);
    render(root, 160, 100);
    paint_tf_stats(&r1, &c1);
    CHECK(r1 == 1, "child change did not re-rasterize (%ld)", r1);
    CHECK(pix(26, 26) == 0x000000, "child color not updated: %06x", pix(26, 26));

    /* at 150 % the raster is at the display's resolution (sharp edge, no blur band) */
    PaintView keep = paint_view;
    paint_view = (PaintView){ 0, 0, 1.5f, 0, 0, 0 };
    sn(a, "scaleX", 1); sn(a, "scaleY", 1); sn(a, "rotate", 0);
    sn(b, "scaleX", 2); sn(b, "scaleY", 2); sn(b, "rotate", 0);
    node_invalidate(a); node_invalidate(b);
    if (FW != 240) { free(fb); fb = calloc(240 * 150, 4); FW = 240; FH = 150; }
    canvas_init(&frame, fb, 240, 150, 240);
    layout_root(root, 160, 100);
    paint_damage(root);
    ui.damage = (Box){ 0, 0, 160, 100 };
    paint_root_clear(&frame, root, 0);
    paint_view = keep;
    /* b: 60×10 at (70,45) scaled 2 around (100,50) → x 40..160, y 40..60 logical → 60..240, 60..90 physical */
    int soft = 0;
    for (int x = 100; x < 230; x++) { uint32_t p = pix(x, 61); if (p != 0x00ff00 && p != 0xffffff && p != 0xff0000 && p != 0x000000) soft++; }
    CHECK(pix(150, 75) == 0x00ff00, "scaled bar at 150%%: %06x", pix(150, 75));
    CHECK(soft == 0, "blurry row inside the scaled bar at 150%% (%d px)", soft);

    /* a scaled subtree with a backdrop blur inside is still painted scaled (the Classic task view:
     * a translucent Terminal); before, it was painted at full size */
    if (FW != 160) { free(fb); fb = calloc(160 * 100, 4); FW = 160; FH = 100; }
    canvas_init(&frame, fb, 160, 100, 160);
    sn(a, "scaleX", 0.5); sn(a, "scaleY", 0.5);
    sn(b, "scaleX", 1); sn(b, "scaleY", 1); sn(b, "rotate", 90);
    sn(ac, "backgroundColor", 0x800000ff);
    sn(ac, "backdropBlur", 6);
    node_invalidate(a); node_invalidate(ac); node_invalidate(b);
    render(root, 160, 100);
    CHECK(pix(15, 15) == 0xffffff && pix(45, 30) == 0xffffff, "a blurred child kept its box unscaled: %06x %06x", pix(15, 15), pix(45, 30));
    CHECK(pix(35, 35) == 0xff0000, "the scaled box with a blurred child: %06x", pix(35, 35));
}

/* A node with its own transform whose own style changes (here its background) is rasterized again:
 * damage_walk keeps the node's paint_dirty for tf_check, which runs after the children (before,
 * it had been cleared and the cached raster kept the old colour). */
static void test_transform_self_restyle(void) {
    ui_reset();
    Node *root = view(NULL);
    ui.root = root;
    sn(root, "backgroundColor", 0xffffffff);
    Node *a = view(root);                               /* 40×40 red, rotated 15° */
    ss(a, "position", "absolute");
    sn(a, "left", 30); sn(a, "top", 30); sn(a, "width", 40); sn(a, "height", 40);
    sn(a, "backgroundColor", 0xffff0000);
    sn(a, "rotate", 15);
    render(root, 100, 100);
    CHECK(pix(50, 50) == 0xff0000, "rotated box: %06x", pix(50, 50));
    sn(a, "backgroundColor", 0xff0000ff);
    node_invalidate(a);
    render(root, 100, 100);
    CHECK(pix(50, 50) == 0x0000ff, "rotated box kept its old background: %06x", pix(50, 50));
}

/* The overlay scroll bar as ScrollView (sdk/components.jsx, sdk/scrollbar.js) builds it: an
 * overflow-hidden view, the content translated inside it with its normal end padding, and over it a
 * 14 px hit strip at the end edge holding the 6 px thumb 2 px from the edge. The bar is painted
 * over the content (no strip reserved), never over the text, takes the pointer only while shown,
 * fades by opacity and goes to the left in RTL. */
static void test_overlay_scrollbar(void) {
    ui_reset();
    Node *root = view(NULL);
    ui.root = root;
    sn(root, "backgroundColor", 0xffffffff);
    Node *sv = view(root);                               /* the ScrollView: 200 × 100 */
    ss(sv, "position", "absolute");
    sn(sv, "left", 20); sn(sv, "top", 10); sn(sv, "width", 200); sn(sv, "height", 100);
    ss(sv, "overflow", "hidden");
    Node *in = view(sv);                                 /* content: 400 high, 12 px end padding */
    sn(in, "backgroundColor", 0xffddeeff);
    sn(in, "paddingEnd", 12);
    sn(in, "translateY", -40);
    Node *rows[8], *label = NULL;
    for (int i = 0; i < 8; i++) {
        rows[i] = view(in);
        sn(rows[i], "height", 50);
        ss(rows[i], "flexDirection", "row");
        Node *tx = text(rows[i], "Value 1,234 MB", 12);
        sn(tx, "flexGrow", 1); ss(tx, "textAlign", "end"); ss(tx, "whiteSpace", "nowrap");
        if (i == 1) label = tx;
    }
    /* thumb geometry for view 100 / content 400 / offset 40: track 96, size 24, top 2 + 72 * 40/300 */
    float top = 2 + (96 - 24) * (40.0f / 300.0f);
    Node *strip = view(sv);
    ss(strip, "position", "absolute");
    sn(strip, "end", 0); sn(strip, "top", top); sn(strip, "width", 14); sn(strip, "height", 24);
    Node *thumb = view(strip);
    ss(thumb, "position", "absolute");
    sn(thumb, "end", 2); sn(thumb, "top", 0); sn(thumb, "width", 6); sn(thumb, "height", 24);
    sn(thumb, "borderRadius", 3); sn(thumb, "backgroundColor", 0xff808080); sn(thumb, "opacity", 1);
    render(root, 240, 120);

    int tx = (int)(sv->ax + 200 - 2 - 3), ty = (int)(sv->ay + top + 12);   /* the thumb's center */
    CHECK(in->w == 200, "the content keeps the full width (no strip reserved): %g", in->w);
    CHECK(pix(tx, ty) == 0x808080, "thumb painted over the content: %06x", pix(tx, ty));
    CHECK(pix((int)(sv->ax + 199), ty) == 0xddeeff, "2 px inset: the edge shows the content: %06x", pix((int)(sv->ax + 199), ty));
    CHECK(pix((int)(sv->ax + 200 - 8 - 2), ty) == 0xddeeff, "thumb wider than 6 px: %06x", pix((int)(sv->ax + 190), ty));
    CHECK(label && label->ax + label->w <= thumb->ax, "the text (ends at %g) runs under the thumb (starts at %g)",
          label ? label->ax + label->w : -1, thumb->ax);
    CHECK(fabsf(rows[1]->ay - (sv->ay + 50 - 40)) < 0.5f, "content scrolled by translateY: row at %g", rows[1]->ay);
    /* hit testing: the strip takes the pointer (drag), just outside it the content does */
    CHECK(hit_test(root, (float)tx, (float)ty) == thumb || hit_test(root, (float)tx, (float)ty) == strip, "thumb not hit");
    CHECK(hit_test(root, sv->ax + 200 - 13, (float)ty) == strip, "the 14 px strip is not hit at its start");
    CHECK(hit_test(root, sv->ax + 200 - 15, (float)ty) != strip && hit_test(root, sv->ax + 200 - 15, (float)ty) != thumb, "hit outside the strip");
    CHECK(hit_test(root, (float)tx, sv->ay + top + 30) != strip, "hit below the thumb went to the strip");

    /* fading: half opacity blends the thumb with the content */
    sn(thumb, "opacity", 0.5);
    render(root, 240, 120);
    uint32_t p = pix(tx, ty);
    int r = (int)(p >> 16 & 0xff), b = (int)(p & 0xff);
    CHECK(r > 0x90 && r < 0xd0 && b > 0x90 && b < 0xf0, "half-faded thumb: %06x", p);
    /* hidden: opacity 0 and pointerEvents none: nothing painted, the click goes to the content */
    sn(thumb, "opacity", 0);
    ss(strip, "pointerEvents", "none");
    render(root, 240, 120);
    CHECK(pix(tx, ty) == 0xddeeff, "hidden thumb painted: %06x", pix(tx, ty));
    Node *h = hit_test(root, (float)tx, (float)ty);
    CHECK(h != strip && h != thumb, "a hidden bar takes the click");

    /* RTL: the bar is on the left edge */
    sn(thumb, "opacity", 1);
    ss(strip, "pointerEvents", "auto");
    ss(sv, "direction", "rtl");
    node_invalidate(sv);
    render(root, 240, 120);
    CHECK(fabsf(strip->ax - sv->ax) < 0.5f, "RTL strip at the left edge: %g (view at %g)", strip->ax, sv->ax);
    CHECK(pix((int)(sv->ax + 2 + 3), ty) == 0x808080, "RTL thumb painted on the left: %06x", pix((int)(sv->ax + 5), ty));
    CHECK(pix((int)(sv->ax + 200 - 5), ty) != 0x808080, "RTL thumb still on the right");
}

/* ---- The dragged window, pixel for pixel (test_drag_exact) ----
 * LordBench's desktop scene (apps/sysinfo/scene.jsx: a 760×480 window, a 72 px square turning inside
 * it, six cards with shadows and text; a rounded window below; a menu bar and a Dock with backdrop
 * blur) moved along LordBench's figure of eight (lbi_path, one step a frame as grab_script_tick does
 * by the clock) for 480 frames, the square turning 6° a frame through lord.setStyle's C path
 * (node_restyled with 'rotate', as scene.jsx sends it), and painted as main.c paints a frame
 * (paint_ui, then paint_out on one display).
 * Every frame's pixels are hashed (FNV-1a over the 32-bit pixels) and compared with
 * tests/drag_golden_cpu.txt and tests/drag_golden_gpu.txt: those must give exactly the same pixels.
 * They were written from the code before the drag-frame optimizations (001633b) and written again at
 * step 9, the one approved pixel change (2026-10-28), from 001633b plus that step: a node the drag
 * frame skips (before the dragged window, not one of its ancestors) no longer has its corners saved
 * and restored, a restore that blended each pixel over itself and left the corners of rounded
 * windows below one level darker than a full repaint. Before the rewrite every frame was compared
 * pixel by pixel with 001633b's: 178,634 pixels in 1,686 of 1,816 frames differed, by 1 level at
 * most, all in those corner squares (n = ceil(r) + 1) or in the Dock's and the menu bar's backdrop
 * blur within its reach of one, none further from a full repaint. They were written a third time
 * (0.8.6, in development) for the restore of rounded corners on the drag layer (canvas.c, mix_px): it
 * made the pixels outside a dragged window's rounded corners opaque, black squares where a full repaint
 * shows what is below. Every frame was compared pixel by pixel with the code before: on the CPU path
 * 498,743 pixels in the dragged window's corner squares and 445,199 in the Dock's and the menu bar's
 * backdrop blur with one of those squares within its reach differed, in 2,386 of 2,417 frames (GPU
 * path: 498,692 and 431,316 in 2,387), none anywhere else. "gpu" is the same run with
 * gpu_active() on: paint.c's GPU path (the layer's sentinel, the app holes, no skipped nodes) on the
 * same CPU canvas the GPU uploads. "gpu-drag" is the GPU path with GPU drag (paint_root_gdrag, docs/GPU.md): a stub GPU
 * keeps the UI texture (what was uploaded of the canvas) and the layer's and the copy's textures (only
 * their dirty rectangles), and writes the window's rectangle with canvas_gdrag_model (gpu.c's shader in C);
 * what it shows must hash as drag_golden_gpu.txt. Shorter runs cover what makes the layer paint again: a text
 * that changes a child's size, a child moving inside the window, a terminal whose cursor blinks
 * (damage from outside the tree), a square turning across the window's edge, the window crossing
 * x = 0 and y = 0 while the square turns, a child removed after frames that only copy the layer and
 * then the square turning again, and the whole drag on a display at 200 %. The runs after those change
 * what is below the window or above it (the copy of what is below, below_ready; see their list below);
 * their hashes come from e24af69 plus step 9 (the code before the copy was built in parts and used
 * where 001633b's rule painted everything), with the earlier runs' hashes unchanged. In them the GPU
 * path's list of app windows is hashed with the pixels. Every frame that uses the copy where
 * 001633b's rule painted everything is also painted that old way on a copy of the canvas
 * (paint_below_verify), and not one pixel may differ; LORD_BELOW_SWITCH=0 runs the test without it.
 * The pixels that differ from a full repaint (paint_root_full, as LORD_PAINT_VERIFY) are printed as a
 * second number, which may only go down (step 9 took the long run from 2,265 to 2,217 on the CPU
 * path and from 70,940 to 70,882 on the GPU path; the corners' restore took them to 248 and 70,864;
 * what is left: the largest at the Dock's rounded corners, under its backdrop blur, while the window
 * is near it, and on the GPU path the dragged window's shadow one level darker).
 *   build/engine-test                 checks the hashes (skipped without hashes for this architecture)
 *   make drag-golden                  writes this architecture's hashes: only on the reference code
 *                                     (001633b plus step 9 and the corners' restore), never to make a
 *                                     changed frame pass */
#if defined(__x86_64__)
#define GOLD_ARCH "x86_64"
#elif defined(__aarch64__)
#define GOLD_ARCH "aarch64"
#else
#define GOLD_ARCH "other"
#endif

enum { DW = 1024, DH = 640, DRAG_FRAMES = 480, CASE_FRAMES = 60, FULL_EVERY = 16 };
/* The runs after k2 change what is below the dragged window, or above it, while it moves (the copy of
 * what is below, below_ready): "below" a progress bar in the window below that changes width every 3
 * frames, its title every 30, an app window and a terminal whose cursor blinks in it; "after" the
 * menu bar's text every 2 frames; "belowtf" a square turning on the desktop under the drag's path and
 * one turned but still, which the window's left edge crosses; "blur" a blurred panel on the desktop
 * whose text changes every 40 frames; "zraise" the bar, and the window below raised above the dragged
 * one and lowered again; "mix" and "k2mix" (200 %) the bar, the menu bar, the squares and the panel;
 * "anc" the bar and the panel, with both windows in a rounded container that clips them and has a
 * border (an ancestor painted around the dragged window); "tfgrow" the still square, and a dot on it
 * that changes color every 20 frames (a part of the copy that cuts the square's image). */
/* The runs after DC_N have no golden hashes: GPU drag (paint_root_gdrag) is compared with the GPU path itself
 * in them (test_gdrag_extra): "pause" stops the drag for 30 frames with nothing changing (frames with no UI
 * damage, which only draw the window again on the GPU), "two" is the drag on two displays side by side
 * (the window crosses from one to the other: the layer, the copy and GPU drag follow the one with most of it),
 * "app" drags over the window below with its app window, "ancedge" slowly away from the rounded,
 * bordered container's top-left corner, "corner" over a rounded overflow:hidden box painted after the
 * window that paints nothing itself (its corners' save and restore still change the pixels below). Their frames are compared as the GPU shows them: the app
 * windows below the UI texture, the UI blended over them, GPU drag's rectangle written over both. */
enum { DC_DRAG, DC_TEXT, DC_CHILD, DC_TERM, DC_EDGE, DC_NEG, DC_REMOVE, DC_K2,
       DC_BELOW, DC_AFTER, DC_BELOWTF, DC_BLUR, DC_ZRAISE, DC_MIX, DC_K2MIX, DC_ANC, DC_TFGROW, DC_N,
       DC_PAUSE = DC_N, DC_TWO, DC_APP, DC_ANCEDGE, DC_CORNER, DC_ALL };
static const char *const dc_name[DC_ALL] = { "drag", "text", "child", "term", "edge", "neg", "remove", "k2",
                                             "below", "after", "belowtf", "blur", "zraise", "mix", "k2mix", "anc", "tfgrow",
                                             "pause", "two", "app", "ancedge", "corner" };
static const int dc_frames[DC_ALL] = { DRAG_FRAMES, CASE_FRAMES, CASE_FRAMES, CASE_FRAMES, CASE_FRAMES,
                                       CASE_FRAMES, CASE_FRAMES, CASE_FRAMES,
                                       180, 120, 180, 180, 120, 240, 120, 180, 180, 90, 240, 180, 90, 90 };
enum { DF_BAR = 1, DF_MENU = 2, DF_SQUARE = 4, DF_PANEL = 8, DF_RAISE = 16, DF_ANC = 32, DF_SMALL = 64, DF_TERM = 128, DF_DOT = 256 };
static const int dc_feat[DC_ALL] = { 0, 0, 0, 0, 0, 0, 0, 0,
                                   DF_BAR | DF_TERM, DF_MENU, DF_SQUARE | DF_SMALL, DF_PANEL | DF_SMALL, DF_BAR | DF_RAISE,
                                   DF_BAR | DF_MENU | DF_SQUARE | DF_PANEL | DF_SMALL,
                                   DF_BAR | DF_MENU | DF_SQUARE | DF_PANEL | DF_SMALL, DF_BAR | DF_ANC | DF_PANEL | DF_SMALL,
                                   DF_DOT | DF_SMALL, DF_TERM | DF_SMALL, DF_SMALL, DF_BAR | DF_SMALL, DF_ANC | DF_SMALL, DF_SMALL };
static int dc_k2(int dc) { return dc == DC_K2 || dc == DC_K2MIX; }
static int dc_hashes(void) {                        /* each run: its frames and the one after the drag */
    int n = 0;
    for (int dc = 0; dc < DC_N; dc++) n += dc_frames[dc] + 1;
    return n;
}

/* GPU drag (test_gdrag): the frame as the GPU shows it. tex is the GPU's UI texture (what was uploaded of the
 * scene c), fb the composed frame: tex with the window's rectangle written by canvas_gdrag_model (gpu.c's
 * shader in C) from two textures that get only paint.c's dirty rectangles, as gpu.c's. gdrag_on, gdragw:
 * main.c's bookkeeping of the scene's stale rectangle. */
typedef struct {
    OutView v;
    Canvas c, ref, fbc;
    uint32_t *px, *rpx, *tex, *fb, *cmp;
    int gdrag_on;
    Rect gdragw;
    GpuDrag g;
} DOut;
static int test_gdrag;
/* the frame pipeline (main.c, fpipe.h): each frame painted is presented only after the next frame's changes
 * (the pointer, JS) are made, right before that frame is painted, as a frame prepared during a pending flip
 * is submitted at the flip; its pixels must be the same */
static int test_pipe;
typedef struct { int on, taken, last, why; Rect pr; } DPend;
static DPend dpend[4];
static struct { uint32_t *b, *l; int bw, bh, lw, lh, owner[2]; } gdragtex;   /* the stub GPU's GPU drag textures (owner: 0 the copy, 1 the layer) */
static long gdrag_fed;                                 /* pixels sent between frames (paint_gdrag_feed) */
static long gdrag_whole;                               /* GPU drag frames that had to send a texture whole */
static long gdrag_quads;
typedef struct { Node *root, *win, *spin, *edge, *child, *term, *caption, *below, *btitle, *fill, *mtext, *square, *ptext, *dot; } DScene;

static uint64_t frame_hash(const Canvas *c) {
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < c->h; y++) {
        const uint32_t *row = c->px + (size_t)y * c->stride;
        for (int x = 0; x < c->w; x++) { h ^= row[x]; h *= 1099511628211ull; }
    }
    /* the GPU path's list of app windows (none in the runs before "below": their hashes stay) */
    const unsigned char *b = (const unsigned char *)wdraw;
    for (size_t i = 0; i < sizeof *wdraw * (size_t)nwdraw; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

/* lord.setStyle with a number (js.c): set, then node_restyled decides what to place or repaint */
static void js_set(Node *n, const char *k, double v) {
    Style before = n->s;
    StyleExt xb;
    if (n->s.ext) memcpy(&xb, n->s.ext, STYLE_EXT_VALUES);
    if (style_set_number(&n->s, k, v)) { printf("unknown style %s\n", k); fails++; }
    node_restyled(n, &before, before.ext ? &xb : NULL);
}

static void copy_rect(uint32_t *dst, int dstride, const uint32_t *src, int sstride, Rect r) {
    for (int y = r.y0; y < r.y1; y++)
        memcpy(dst + (size_t)y * dstride + r.x0, src + (size_t)y * sstride + r.x0, (size_t)(r.x1 - r.x0) * 4);
}

/* The stub GPU's texture t (*tw × *th): all of px if its size changed, it was another display's or
 * full, else the dirty rectangles (gpu.c's gdrag_upload). */
static long gdrag_frame_px, gdrag_frame_max;              /* pixels the GPU drag frames sent: this one, the most one did */
static void gdrag_tex_up(uint32_t **t, int *tw, int *th, const uint32_t *px, int w, int h, const Rect *r, int n, int full) {
    if (*tw != w || *th != h) {
        free(*t);
        *t = malloc((size_t)w * h * 4);
        *tw = w;
        *th = h;
        full = 1;
    }
    if (!*t) return;
    if (full) { memcpy(*t, px, (size_t)w * h * 4); gdrag_frame_px += (long)w * h; return; }
    for (int i = 0; i < n; i++) {
        Rect q = { r[i].x0 < 0 ? 0 : r[i].x0, r[i].y0 < 0 ? 0 : r[i].y0, r[i].x1 > w ? w : r[i].x1, r[i].y1 > h ? h : r[i].y1 };
        if (q.x1 > q.x0 && q.y1 > q.y0) { copy_rect(*t, w, px, w, q); gdrag_frame_px += (long)(q.x1 - q.x0) * (q.y1 - q.y0); }
    }
}

/* gpu.c's gpu_gdrag_feed on the stub GPU (ctx: the display's index): a texture that is not this display's
 * at this size is made again, filled with a pattern no frame has (so a pixel the feed missed shows) */
static int gdrag_tex_feed(void *ctx, int which, const uint32_t *px, int w, int h, Rect r) {
    int index = *(const int *)ctx;
    uint32_t **t = which ? &gdragtex.l : &gdragtex.b;
    int *tw = which ? &gdragtex.lw : &gdragtex.bw, *th = which ? &gdragtex.lh : &gdragtex.bh;
    if (gdragtex.owner[which] != index || *tw != w || *th != h) {
        if (*tw != w || *th != h) {
            free(*t);
            *t = malloc((size_t)w * h * 4);
            *tw = *t ? w : 0;
            *th = *t ? h : 0;
            if (!*t) return -1;
            for (size_t i = 0; i < (size_t)w * h; i++) (*t)[i] = 0x7f13ff13u;
        }
        gdragtex.owner[which] = index;
        return 1;
    }
    Rect q = { r.x0 < 0 ? 0 : r.x0, r.y0 < 0 ? 0 : r.y0, r.x1 > w ? w : r.x1, r.y1 > h ? h : r.y1 };
    if (q.x1 > q.x0 && q.y1 > q.y0) {
        copy_rect(*t, w, px, w, q);
        gdrag_fed += (long)(q.x1 - q.x0) * (q.y1 - q.y0);
    }
    return 0;
}

/* main.c's presentation on the GPU (render_frame, gpu_frame_ex): the painted rectangles (pr, or the
 * strips) go to the UI texture; with GPU drag the two textures get their dirty rectangles and the window is
 * written over the texture's stale rectangle in the composed frame. A frame refused only for what the
 * textures lack (GDRAG_R_FEED) then sends a slice of it (paint_gdrag_feed), as main.c does after queueing the flip. */
static void gdrag_present(DOut *o, int index, Rect pr, int taken, int last, int why) {
    int W = o->c.w, H = o->c.h;
    if (!taken) {
        copy_rect(o->tex, W, o->c.px, o->c.stride, pr);
        if (last == 0 && why == GDRAG_R_FEED && paint_gdrag_backlog(index) > 0)
            paint_gdrag_feed(index, GDRAG_FEED_PX, gdrag_tex_feed, &index);
    } else {
        const GpuDrag *g = &o->g;
        for (int i = 0; i < g->nstrip; i++) copy_rect(o->tex, W, o->c.px, o->c.stride, g->strip[i]);
        /* paint.c sent the rest first: a GPU drag frame never has more than GDRAG_FEED_PX left */
        long left = 0;
        if (g->bfull) left += (long)g->bw * g->bh;
        else for (int i = 0; i < g->nb; i++) left += (long)(g->bdirty[i].x1 - g->bdirty[i].x0) * (g->bdirty[i].y1 - g->bdirty[i].y0);
        CHECK(left <= GDRAG_FEED_PX, "a GPU drag frame with %ld px of the copy left to send", left);
        int bother = gdragtex.owner[0] != index, lother = gdragtex.owner[1] != index;
        if (bother || lother || g->bfull || g->lfull) gdrag_whole++;
        gdrag_tex_up(&gdragtex.b, &gdragtex.bw, &gdragtex.bh, g->bpx, g->bw, g->bh, g->bdirty, g->nb, g->bfull || bother);
        gdrag_tex_up(&gdragtex.l, &gdragtex.lw, &gdragtex.lh, g->lpx, g->lw, g->lh, g->ldirty, g->nl, g->lfull || lother);
        gdragtex.owner[0] = gdragtex.owner[1] = index;
        if (gdrag_frame_px > gdrag_frame_max) gdrag_frame_max = gdrag_frame_px;
        gdrag_frame_px = 0;
        paint_gdrag_done();
        gdrag_quads++;
    }
    memcpy(o->fb, o->tex, (size_t)W * H * 4);
    if (taken) {
        const GpuDrag *g = &o->g;
        for (int y = g->w.y0; y < g->w.y1; y++)
            for (int x = g->w.x0; x < g->w.x1; x++)
                o->fb[(size_t)y * W + x] = canvas_gdrag_model(gdragtex.l[(size_t)(y - g->ly) * gdragtex.lw + (x - g->lx)],
                                                           gdragtex.b[(size_t)y * gdragtex.bw + x]);
        o->gdragw = g->w;
    }
    o->gdrag_on = taken;
}

/* One display's paint_out (the damaged box in physical px, cleared and painted with paint_root_clear;
 * with GPU drag, main.c's paint_out and render_frame: the stale rectangle joins the damage, a display with no
 * damage keeps its frame, unless the GPU drag textures are another display's) */
static void drag_paint_out(Node *root, DOut *o, int index, Box d) {
    int empty = d.x1 <= d.x0 || d.y1 <= d.y0;
    if (test_gdrag && o->gdrag_on) {
        float gx0, gy0, gx1, gy1;
        out_logical_box(&o->v, o->gdragw, &gx0, &gy0, &gx1, &gy1);
        if (!empty) d = (Box){ fminf(d.x0, gx0), fminf(d.y0, gy0), fmaxf(d.x1, gx1), fmaxf(d.y1, gy1) };
        else if (gdragtex.owner[0] != index || gdragtex.owner[1] != index) { d = (Box){ gx0, gy0, gx1, gy1 }; o->gdrag_on = 0; }   /* gdrag_stale */
        /* else: drawn again from the textures, the frame stays as it is */
    }
    if (d.x1 <= d.x0 || d.y1 <= d.y0) return;
    Rect pr = out_phys_rect(&o->v, d.x0, d.y0, d.x1, d.y1);
    if (pr.x1 <= pr.x0 || pr.y1 <= pr.y0) return;
    float lx0, ly0, lx1, ly1;
    out_logical_box(&o->v, pr, &lx0, &ly0, &lx1, &ly1);
    paint_view = (PaintView){ (float)o->v.x, (float)o->v.y, (float)o->v.k, (float)o->v.dx, (float)o->v.dy, index };
    ui.damage = (Box){ lx0, ly0, lx1, ly1 };
    canvas_set_clip(&o->c, pr);
    int taken = 0;
    if (test_gdrag) taken = paint_root_gdrag(&o->c, root, &o->g);
    else paint_root_clear(&o->c, root, 0x000000);
    canvas_set_clip(&o->c, (Rect){ 0, 0, o->c.w, o->c.h });
    ui.damage = (Box){ 0, 0, 0, 0 };
    paint_view = (PaintView){ 0, 0, 1, 0, 0, 0 };
    if (!test_gdrag) return;
    int why, sub, last = paint_gdrag_last(&why, &sub);
    if (test_pipe && index < 4) dpend[index] = (DPend){ 1, taken, last, why, pr };
    else gdrag_present(o, index, pr, taken, last, why);
}

/* the frame pipeline: the frames painted are presented now (main.c's out_submit) */
static void drag_present(DOut *const *os, int n) {
    for (int i = 0; i < n && i < 4; i++)
        if (dpend[i].on) {
            dpend[i].on = 0;
            gdrag_present(os[i], i, dpend[i].pr, dpend[i].taken, dpend[i].last, dpend[i].why);
        }
}

/* main.c's frame: paint_ui (layout, damage, the layer's display: the one with most of the window), then
 * paint_out on each display */
static void drag_paint_n(Node *root, DOut *const *os, int n) {
    if (!ui.dirty) return;
    ui.dirty = 0;
    layout_root(root, DW, DH);
    Box d = { 0, 0, 0, 0 };
    if (paint_damage(root)) d = ui.damage;
    ui.damage = (Box){ 0, 0, 0, 0 };
    if (paint_layer_node()) {
        int b = 0;
        Node *w = node_get(paint_layer_node());
        if (n > 1 && w) {
            ORect r[4];
            for (int i = 0; i < n && i < 4; i++) r[i] = (ORect){ os[i]->v.x, os[i]->v.y, os[i]->v.w, os[i]->v.h };
            b = out_best(r, n < 4 ? n : 4, w->prev_box.x0, w->prev_box.y0, w->prev_box.x1, w->prev_box.y1);
        }
        paint_layer_view(b >= 0 ? b : 0);
    }
    for (int i = 0; i < n; i++) drag_paint_out(root, os[i], i, d);
}

/* Pixels of the frame that differ from a full repaint of the tree (paint_root_full); *worst: the
 * largest difference of a channel seen so far. */
static long drag_full_diff(Node *root, DOut *o, int *worst) {
    Box keep = term_at;                     /* the terminal notes where it was painted: not this check */
    canvas_clear(&o->ref, 0x000000);
    paint_view = (PaintView){ (float)o->v.x, (float)o->v.y, (float)o->v.k, (float)o->v.dx, (float)o->v.dy, 0 };
    paint_root_full(&o->ref, root);
    paint_view = (PaintView){ 0, 0, 1, 0, 0, 0 };
    term_at = keep;
    long n = 0;
    const uint32_t *px = test_gdrag ? o->fb : o->px;          /* GPU drag: the frame as shown */
    for (size_t i = 0; i < (size_t)o->c.w * o->c.h; i++) {
        if (px[i] == o->rpx[i]) continue;
        n++;
        for (int sh = 0; sh < 32; sh += 8) {
            int d = abs((int)((px[i] >> sh) & 255) - (int)((o->rpx[i] >> sh) & 255));
            if (d > *worst) *worst = d;
        }
    }
    return n;
}

static Node *dview(Node *parent, float w, float h, uint32_t bg, float radius) {
    Node *n = view(parent);
    if (w > 0) sn(n, "width", w);
    if (h > 0) sn(n, "height", h);
    if (bg) sn(n, "backgroundColor", bg);
    if (radius > 0) sn(n, "borderRadius", radius);
    return n;
}
static Node *dtext(Node *parent, const char *s, float size, float weight, uint32_t color) {
    Node *t = text(parent, s, size);
    sn(t, "color", color);
    if (weight > 0) sn(t, "fontWeight", weight);
    return t;
}
static void dshadow(Node *n, uint32_t color, float y, float blur) {
    sn(n, "shadowColor", color);
    sn(n, "shadowOffsetY", y);
    sn(n, "shadowBlur", blur);
}

/* The desktop and LordBench's scene window at (x, y), with what case dc adds to it. */
static DScene drag_scene(int dc, float x, float y) {
    static const uint32_t accents[6] = { 0xff0a84ff, 0xff30d158, 0xffff9f0a, 0xffbf5af2, 0xffff453a, 0xff64d2ff };
    DScene s = { 0 };
    ui_reset();
    s.root = view(NULL);
    ui.root = s.root;
    sn(s.root, "backgroundColor", 0xff2b3a4f);
    for (int i = 0; i < 5; i++) {                       /* desktop icons with their names */
        Node *ic = box_in(s.root, 24, 50 + i * 100, 64, 64, accents[i]);
        sn(ic, "borderRadius", 14);
        Node *l = dtext(s.root, "Documents", 12, 0, 0xffffffff);
        ss(l, "position", "absolute");
        sn(l, "left", 16); sn(l, "top", 118 + i * 100); sn(l, "width", 80);
        ss(l, "textAlign", "center");
    }
    int feat = dc_feat[dc];
    Node *wins = s.root;                                /* the windows' parent */
    if (feat & DF_ANC) {
        wins = box_in(s.root, 0, 0, DW, DH, 0);
        ss(wins, "overflow", "hidden");
        sn(wins, "borderRadius", dc == DC_ANCEDGE ? 96 : 24);    /* ancedge: its corner reaches the window */
        sn(wins, "borderWidth", 3);
        sn(wins, "borderColor", 0x80ffffff);
    }
    /* a window below: rounded, overflow hidden, with a shadow (its corners are restored around the drag) */
    Node *below = box_in(wins, 110, 330, 520, 280, 0xff1e1e20);
    ss(below, "overflow", "hidden");
    sn(below, "borderRadius", 12);
    dshadow(below, 0x66000000, 16, 40);
    Node *bt = dview(below, 0, 28, 0xff3a3a3c, 0);
    s.btitle = dtext(bt, "Files", 13, 600, 0xffe0e0e0);
    s.below = below;
    for (int i = 0; i < 8; i++) {
        Node *row = dview(below, 0, 30, i & 1 ? 0xff252528 : 0, 0);
        ss(row, "flexDirection", "row"); ss(row, "alignItems", "center");
        sn(row, "paddingLeft", 12); sn(row, "gap", 8);
        dview(row, 16, 16, 0xff5b8def, 3);
        dtext(row, "Quarterly report, final version.pdf", 13, 0, 0xffd0d0d0);
    }
    if (feat & DF_BAR) {
        /* performance.jsx's bar: a rounded track that clips its fill, and an app window */
        Node *track = box_in(below, 200, 262, 300, 6, 0xff3a3a3c);
        ss(track, "overflow", "hidden");
        sn(track, "borderRadius", 3);
        s.fill = box_in(track, 0, 0, 120, 6, 0xff0a84ff);
        Node *surf = node_new(NODE_SURFACE);
        surf->term = 7;
        node_append(below, surf);
        ss(surf, "position", "absolute");
        sn(surf, "left", 340); sn(surf, "top", 120); sn(surf, "width", 150); sn(surf, "height", 90);
    }
    if (feat & DF_TERM) {                               /* a terminal in the window below */
        s.term = node_new(NODE_TERM);
        s.term->term = 2;
        node_append(below, s.term);
        ss(s.term, "position", "absolute");
        sn(s.term, "left", 20); sn(s.term, "top", 200); sn(s.term, "width", 160); sn(s.term, "height", 40);
    }
    if (feat & DF_PANEL) {                              /* a blurred widget on the desktop, with a clock */
        Node *panel = box_in(s.root, 6, 290, 90, 110, 0x40ffffff);      /* the window leaves it at times */
        sn(panel, "borderRadius", 16);
        sn(panel, "backdropBlur", 16);
        ss(panel, "alignItems", "center"); ss(panel, "justifyContent", "center");
        s.ptext = dtext(panel, "12:00", 24, 600, 0xffffffff);
    }
    if (feat & DF_SQUARE) {                             /* a square turning on the desktop */
        s.square = box_in(s.root, 100, 240, 56, 56, 0xff30d158);
        sn(s.square, "borderRadius", 10);
    }
    if (feat & (DF_SQUARE | DF_DOT)) {
        Node *still = box_in(s.root, 60, 420, 50, 50, 0xffff9f0a);   /* the window's left edge crosses it */
        sn(still, "borderRadius", 8);
        js_set(still, "rotate", 30);
    }
    if (feat & DF_DOT) {
        s.dot = box_in(s.root, 104, 428, 14, 14, 0xffff453a);
        sn(s.dot, "borderRadius", 7);
    }
    /* the scene window: the theme's frame (title bar) and scene.jsx's content */
    /* DF_SMALL: smaller, with a smaller shadow and lower (drag_run), so that its frames' box mostly
     * keeps away from what the menu bar's and the Dock's blur read */
    s.win = box_in(wins, x, y, feat & DF_SMALL ? 480 : 760, feat & DF_SMALL ? 300 : 480, 0xff2c2c2e);
    ss(s.win, "overflow", "hidden");
    sn(s.win, "borderRadius", 12);
    sn(s.win, "borderWidth", 1);
    sn(s.win, "borderColor", 0x30ffffff);
    if (feat & DF_SMALL) dshadow(s.win, 0x73000000, 12, 24);
    else dshadow(s.win, 0x73000000, 24, 56);
    Node *title = dview(s.win, 0, 28, 0xff3a3a3c, 0);
    ss(title, "alignItems", "center"); ss(title, "justifyContent", "center");
    dtext(title, "System Information", 13, 600, 0xffe0e0e0);
    Node *body = view(s.win);
    sn(body, "flexGrow", 1);
    sn(body, "paddingLeft", 20); sn(body, "paddingRight", 20); sn(body, "paddingTop", 20); sn(body, "paddingBottom", 20);
    sn(body, "gap", 12);
    ss(body, "alignItems", "center"); ss(body, "justifyContent", "center");
    s.spin = dview(body, 72, 72, 0xff0a84ff, 16);
    ss(s.spin, "alignItems", "center"); ss(s.spin, "justifyContent", "center");
    dview(s.spin, 24, 24, 0xffffffff, 12);
    Node *cards = view(body);
    ss(cards, "flexDirection", "row"); ss(cards, "flexWrap", "wrap"); ss(cards, "justifyContent", "center");
    sn(cards, "gap", 12);
    for (int i = 0; i < 6; i++) {
        Node *c = dview(cards, 180, 96, 0x14ffffff, 12);
        sn(c, "paddingLeft", 12); sn(c, "paddingRight", 12); sn(c, "paddingTop", 12); sn(c, "paddingBottom", 12);
        sn(c, "gap", 4);
        dshadow(c, 0x40000000, 2, 8);
        dview(c, 28, 28, accents[i], 8);
        dtext(c, "Desktop impact", 15, 600, 0xffffffff);
        char label[32];
        snprintf(label, sizeof label, "Card %d", i + 1);
        Node *cap = dtext(c, label, 12, 0, 0x99ffffff);
        if (!i) s.caption = cap;
    }
    if (dc == DC_CHILD || dc == DC_REMOVE) {            /* a badge that slides inside the window, or is removed */
        s.child = box_in(s.win, 40, 44, 14, 14, 0xffff453a);
        sn(s.child, "borderRadius", 7);
    }
    if (dc == DC_NEG || dc == DC_REMOVE || dc == DC_K2) {
        /* odd widths centered in a column: boxes, shadows and text at half pixels, which a move by
         * whole pixels must not round differently (across x = 0, at 200 %) */
        Node *col = box_in(s.win, 24, 40, 210, 200, 0);
        ss(col, "alignItems", "center");
        sn(col, "gap", 7);
        for (int i = 0; i < 4; i++) {
            Node *c = dview(col, 101 + 2 * i, 31, 0x22ffffff, 6);
            ss(c, "alignItems", "center"); ss(c, "justifyContent", "center");
            dshadow(c, 0x50000000, 2, 7);
            dtext(c, i & 1 ? "fig iq Wj 1.5" : "Half pixel label", 13, 0, 0xffffffff);
        }
    }
    if (dc == DC_TERM) {                                /* a terminal, its cursor blinking */
        s.term = node_new(NODE_TERM);
        s.term->term = 1;
        node_append(s.win, s.term);
        ss(s.term, "position", "absolute");
        sn(s.term, "left", 16); sn(s.term, "top", 412); sn(s.term, "width", 320); sn(s.term, "height", 52);
    }
    if (dc == DC_EDGE) {                                /* a turning square across the window's right edge */
        s.edge = box_in(s.win, 732, 120, 40, 40, 0xffbf5af2);
        sn(s.edge, "borderRadius", 6);
    }
    if (dc == DC_CORNER) {                              /* after the window, no colour: only its corners */
        Node *clip = box_in(s.root, x + 120, y + 80, 200, 150, 0);
        ss(clip, "overflow", "hidden");
        sn(clip, "borderRadius", 60);
    }
    /* the Dock and the menu bar: shell layers above the windows, with backdrop blur (not in ancedge:
     * nothing painted after the window near the corner it hugs) */
    if (dc == DC_ANCEDGE) return s;
    Node *dockl = layer_node(s.root, 10, DW, DH);
    Node *dock = box_in(dockl, (DW - 600) / 2, DH - 72, 600, 64, 0x80303034);
    sn(dock, "borderRadius", 16);
    sn(dock, "backdropBlur", 20);
    for (int i = 0; i < 6; i++) dview(dock, 48, 48, accents[i], 12);
    /* pause: 300 more nodes painted after the window, each in its own box below the Dock (LordOS's real
     * menu bar and Dock are a few hundred; GPU drag's list of them once held 128 and refused every frame) */
    if (dc == DC_PAUSE) for (int i = 0; i < 300; i++) box_in(dockl, 12 + 3 * i, DH - 6, 2, 2, accents[i % 6]);
    ss(dock, "flexDirection", "row"); ss(dock, "alignItems", "center"); ss(dock, "justifyContent", "center");
    sn(dock, "gap", 12);
    Node *barl = layer_node(s.root, 20, DW, DH);
    Node *bar = box_in(barl, 0, 0, DW, 28, 0x99202024);
    sn(bar, "backdropBlur", 20);
    ss(bar, "flexDirection", "row"); ss(bar, "alignItems", "center");
    sn(bar, "paddingLeft", 14); sn(bar, "gap", 18);
    dtext(bar, "LordOS", 13, 700, 0xffffffff);
    dtext(bar, "File", 13, 0, 0xffffffff);
    s.mtext = dtext(bar, "Edit", 13, 0, 0xffffffff);
    return s;
}

/* What the GPU shows (test_compose): the app windows of the list (a solid colour each) below the UI
 * texture, the texture blended over them (ONE, ONE_MINUS_SRC_ALPHA, rounded), and with GPU drag the window's
 * rectangle written over both (blending off: its colour, opaque). */
static int test_compose;
static uint64_t composed_hash(DOut *o) {
    int W = o->c.w, H = o->c.h;
    const uint32_t *t = test_gdrag ? o->tex : o->c.px;    /* without GPU drag every painted pixel was uploaded */
    uint32_t *f = o->cmp;
    memset(f, 0, (size_t)W * H * 4);
    const float k = (float)o->v.k;
    for (int i = 0; i < nwdraw; i++) {
        const WDraw *d = &wdraw[i];
        int x0 = (int)floorf((d->x - o->v.x) * k), y0 = (int)floorf((d->y - o->v.y) * k);
        int x1 = (int)ceilf((d->x + d->w - o->v.x) * k), y1 = (int)ceilf((d->y + d->h - o->v.y) * k);
        for (int y = y0 < 0 ? 0 : y0; y < y1 && y < H; y++)
            for (int x = x0 < 0 ? 0 : x0; x < x1 && x < W; x++) f[(size_t)y * W + x] = 0xff000000u | (0x406080u + 0x111111u * (uint32_t)d->id);
    }
    for (size_t i = 0; i < (size_t)W * H; i++) {
        uint32_t s = t[i], d = f[i], ia = 255 - (s >> 24), o2 = 0xff000000u;
        for (int sh = 0; sh < 24; sh += 8) {
            uint32_t v = (s >> sh & 255) + ((d >> sh & 255) * ia + 127) / 255;
            o2 |= (v > 255 ? 255 : v) << sh;
        }
        f[i] = o2;
    }
    if (test_gdrag && o->gdrag_on)
        for (int y = o->gdragw.y0; y < o->gdragw.y1; y++)
            for (int x = o->gdragw.x0; x < o->gdragw.x1; x++) f[(size_t)y * W + x] = 0xff000000u | (o->fb[(size_t)y * W + x] & 0xffffff);
    Canvas c;
    canvas_init(&c, f, W, H, W);
    return frame_hash(&c);
}

/* The hash of what the displays show (with GPU drag, the composed frames) */
static uint64_t outs_hash(DOut *const *os, int nos) {
    uint64_t h = 0;
    for (int i = 0; i < nos; i++)
        h = h * 1099511628211ull ^ (test_compose ? composed_hash(os[i]) : frame_hash(test_gdrag ? &os[i]->fbc : &os[i]->c));
    return h;
}

/* One run: case dc's scene, the drag (n frames), then the frame after it ends, on the displays os (one
 * at 100 %, at 200 % for k2 and k2mix, two side by side for "two"). hash gets n + 1 values; the largest
 * count of pixels that differed from a full repaint (the first display) goes to *full, the largest
 * difference of a channel to *worst. */
static void drag_run_n(int dc, DOut *const *os, int nos, uint64_t *hash, long *full, int *worst) {
    const int n = dc_frames[dc];
    DOut *o = os[0];
    float x0 = (DW - 760) / 2, y0 = 28 + (DH - 28 - 480) / 2;             /* scenePath: centered in the work area */
    const float rx = (DW - 760) / 2 - 20, ry = (DH - 28 - 480) - 40;
    if (dc == DC_NEG) { x0 = -200; y0 = -120; }        /* partly off the display's top-left corner */
    if (dc_feat[dc] & DF_SMALL) y0 += 54;
    if (dc == DC_ANCEDGE) x0 = y0 = 26;                 /* its shadow over the container's rounded corner */
    DScene s = drag_scene(dc, x0, y0);
    term_cursor = 1;
    term_at = (Box){ 0, 0, 0, 0 };
    for (int i = 0; i < nos; i++) {
        canvas_set_clip(&os[i]->c, (Rect){ 0, 0, os[i]->c.w, os[i]->c.h });
        canvas_clear(&os[i]->c, 0x000000);
        os[i]->gdrag_on = 0;
    }
    gdragtex.owner[0] = gdragtex.owner[1] = -1;
    damage_add((Box){ 0, 0, DW, DH });
    ui.dirty = 1;
    drag_paint_n(s.root, os, nos);
    drag_present(os, nos);
    paint_layer_begin(s.win->id, 0);                    /* grab.c's setup */
    float gx = x0, gy = y0, rot = 0;
    unsigned pipe_epoch = paint_gdrag_epoch();
    *full = 0;
    *worst = 0;
    for (int f = 0; f < n; f++) {
        float dx, dy;
        int paused = dc == DC_PAUSE && f >= 30 && f < 60;   /* nothing changes: no UI damage */
        lbi_path(rx, ry, 6000, (paused ? 30 : f) * (1000.0 / 60), &dx, &dy);
        if (dc == DC_NEG) { dx = 6 * f; dy = 4 * f; }   /* its left edge crosses x = 0, its top y = 0 */
        if (dc == DC_ANCEDGE) { dx = 0.5f * f; dy = 0.25f * f; }
        float x = x0 + roundf(dx), y = y0 + roundf(dy);  /* grab_motion */
        if (x != gx || y != gy) {
            gx = x; gy = y;
            s.win->s.pos[EDGE_L] = (Dim){ x, UNIT_PX };
            s.win->s.pos[EDGE_T] = (Dim){ y, UNIT_PX };
            node_moved(s.win);
            ui.dirty = 1;
        }
        if (!paused) rot = fmodf(rot + 6, 360);         /* scene.jsx's square, every 16 ms */
        if ((dc == DC_DRAG || dc == DC_NEG || dc == DC_K2 || dc >= DC_BELOW) && !paused) js_set(s.spin, "rotate", rot);  /* the others: their own */
        int feat = dc_feat[dc];
        if ((feat & DF_BAR) && f % 3 == 1) js_set(s.fill, "width", 40 + (f * 37) % 260);
        if ((feat & DF_BAR) && f % 30 == 15) node_set_text(s.btitle->first, (f / 30) & 1 ? "Files" : "Files, 3 items selected");
        if ((feat & DF_MENU) && f % 2 == 0) node_set_text(s.mtext->first, (f / 2) & 1 ? "Edit" : "Edit 12:04");
        if (feat & DF_SQUARE) js_set(s.square, "rotate", fmodf(rot * 0.5f, 360));
        if ((feat & DF_PANEL) && f % 40 == 20) node_set_text(s.ptext->first, (f / 40) & 1 ? "12:00" : "12:01");
        if ((feat & DF_RAISE) && f == 40) js_set(s.below, "zIndex", 5);
        if ((feat & DF_RAISE) && f == 80) js_set(s.below, "zIndex", 0);
        if ((feat & DF_DOT) && f % 20 == 10) js_set(s.dot, "backgroundColor", (f / 20) & 1 ? 0xffff453a : 0xff64d2ff);
        if ((feat & DF_TERM) && f % 10 == 5 && term_at.x1 > term_at.x0 && !paused) {
            term_cursor = !term_cursor;                 /* the terminal below: outside damage */
            damage_add((Box){ floorf(term_at.x0), floorf(term_at.y0 + 8 - 2), ceilf(term_at.x1), ceilf(term_at.y0 + 24 + 2) });
            ui.dirty = 1;
        }
        if (dc == DC_REMOVE && f % 10 == 0) js_set(s.spin, "rotate", rot);  /* frames between only copy the layer */
        if (dc == DC_REMOVE && f == 35) node_detach(s.child);
        if (dc == DC_EDGE) js_set(s.edge, "rotate", rot);
        if (dc == DC_CHILD) js_set(s.child, "left", 40 + (f * 5) % 640);
        if (dc == DC_TEXT && f % 15 == 7)               /* lord.setText on the caption: one line or two */
            node_set_text(s.caption->first, (f / 15) & 1 ? "Card 1" : "Card 1, measured while a busy app runs");
        if (dc == DC_TERM && f % 10 == 5 && term_at.x1 > term_at.x0) {
            term_cursor = !term_cursor;                 /* term_damage: the cursor's row, where it was painted */
            damage_add((Box){ floorf(term_at.x0), floorf(term_at.y0 + 8 - 2), ceilf(term_at.x1), ceilf(term_at.y0 + 24 + 2) });
            ui.dirty = 1;
        }
        if (test_pipe) {
            /* the frame before is submitted only now, after this frame's changes: they must not touch what it
             * hands the GPU (paint_gdrag_epoch, else main.c paints it again) */
            CHECK(paint_gdrag_epoch() == pipe_epoch, "pipelined: frame %d's changes moved GPU drag's epoch", f);
            drag_present(os, nos);
            if (f) hash[f - 1] = outs_hash(os, nos);
            drag_paint_n(s.root, os, nos);
            pipe_epoch = paint_gdrag_epoch();
            continue;                                   /* (a full repaint now would be of the next frame's tree) */
        }
        drag_paint_n(s.root, os, nos);
        hash[f] = outs_hash(os, nos);
        if (nos == 1 && f % FULL_EVERY == FULL_EVERY - 1) { long d = drag_full_diff(s.root, o, worst); if (d > *full) *full = d; }
    }
    if (test_pipe) {
        drag_present(os, nos);
        hash[n - 1] = outs_hash(os, nos);
    }
    unsigned before_end = paint_gdrag_epoch();
    paint_layer_end();                                  /* grab_finish */
    CHECK(paint_gdrag_epoch() != before_end, "the drag's end moves GPU drag's epoch (a prepared GPU drag frame is painted again)");
    ui.dirty = 1;
    drag_paint_n(s.root, os, nos);
    drag_present(os, nos);
    hash[n] = outs_hash(os, nos);
    if (nos == 1) {
        long d = drag_full_diff(s.root, o, worst);
        if (d > *full) *full = d;
    }
}
static void drag_run(int dc, DOut *o, uint64_t *hash, long *full, int *worst) { drag_run_n(dc, &o, 1, hash, full, worst); }

/* The golden file: lines "<architecture> <run> <frame> <hash>"; # starts a comment. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = len >= 0 ? malloc((size_t)len + 1) : NULL;
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    if (b) b[len] = 0;
    fclose(f);
    return b;
}

static int gold_find(const char *file, const char *run, int frame, uint64_t *h) {
    char key[64];
    int kl = snprintf(key, sizeof key, "%s %s %d ", GOLD_ARCH, run, frame);
    for (const char *p = file; p && *p; ) {
        if (!strncmp(p, key, (size_t)kl)) {
            unsigned long long v;
            if (sscanf(p + kl, "%llx", &v) == 1) { *h = v; return 1; }
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}

/* Writes this architecture's lines, keeping the other architectures' and the comments. */
static int gold_write(const char *path, const char *variant, const uint64_t *hash) {
    char *old = read_file(path);
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { free(old); return -1; }
    if (!old || !*old)
        fprintf(f, "# engine-test test_drag_exact, %s path: FNV-1a hash of every frame of the dragged window.\n"
                   "# Written by `make drag-golden` from 001633b plus step 9 (2026-10-28: the corners of rounded windows\n"
                   "# below the dragged one, 1 level, towards a full repaint) and the corners' restore on the drag layer\n"
                   "# (no black squares outside the dragged window's rounded corners), and main's 0.8.6 blur-margin repaint\n"
                   "# (blur_near: GPU path, frame 0 of neg, mix and k2mix); later code must give the same\n"
                   "# pixels. Lines: architecture, run, frame, hash.\n", variant);
    size_t al = strlen(GOLD_ARCH);
    for (const char *p = old; p && *p; ) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len && !(len > al && !strncmp(p, GOLD_ARCH, al) && p[al] == ' ')) fprintf(f, "%.*s\n", (int)len, p);
        p = e ? e + 1 : NULL;
    }
    int k = 0;
    for (int dc = 0; dc < DC_N; dc++)
        for (int fr = 0; fr <= dc_frames[dc]; fr++) fprintf(f, "%s %s %d %016llx\n", GOLD_ARCH, dc_name[dc], fr, (unsigned long long)hash[k++]);
    free(old);
    if (fclose(f) || rename(tmp, path)) return -1;
    return 0;
}

/* A display of pw × ph physical px for the drag runs (0: out of memory) */
static int dout_init(DOut *o, OutView v) {
    memset(o, 0, sizeof *o);
    o->v = v;
    size_t n = (size_t)v.pw * v.ph;
    o->px = calloc(n, 4);
    o->rpx = calloc(n, 4);
    o->tex = calloc(n, 4);
    o->fb = calloc(n, 4);
    o->cmp = calloc(n, 4);
    if (!o->px || !o->rpx || !o->tex || !o->fb || !o->cmp) return 0;
    canvas_init(&o->c, o->px, v.pw, v.ph, v.pw);
    canvas_init(&o->ref, o->rpx, v.pw, v.ph, v.pw);
    canvas_init(&o->fbc, o->fb, v.pw, v.ph, v.pw);
    return 1;
}
static void dout_free(DOut *o) { free(o->px); free(o->rpx); free(o->tex); free(o->fb); free(o->cmp); }

/* GPU drag's frames counted since the last call, printed for a run (some_gdrag: at least one is expected) */
static void gdrag_report(const char *what, int some_gdrag) {
    GdragStats g;
    paint_gdrag_stats(&g);
    printf("drag-exact %s: GPU drag composed %ld of %ld frames that used the copy (%ld presented), strips %ld px "
           "(not: %ld layer, %ld clip, %ld ancestor, %ld app, %ld after, %ld post, %ld catching up); %ld px sent between "
           "frames, %ld GPU drag frames sent a texture whole, the most one GPU drag frame sent %ld px\n", what, g.frames, g.tried, gdrag_quads,
           g.strip_px, g.refused[GDRAG_R_LAYER], g.refused[GDRAG_R_CLIP], g.refused[GDRAG_R_ANCESTOR], g.refused[GDRAG_R_APP],
           g.refused[GDRAG_R_AFTER], g.refused[GDRAG_R_POST], g.refused[GDRAG_R_FEED], gdrag_fed, gdrag_whole, gdrag_frame_max);
    CHECK((g.frames > 0 || !some_gdrag) && g.frames == gdrag_quads, "drag-exact %s: GPU drag frames %ld, presented %ld", what, g.frames,
          gdrag_quads);
    gdrag_quads = gdrag_fed = gdrag_whole = gdrag_frame_max = 0;
}

/* The runs without golden hashes (pause, two displays): GPU drag must show what the GPU path shows, frame by
 * frame (that path is the one the golden hashes check in the other runs). */
static void test_gdrag_extra(void) {
    DOut a, b, c;
    int ok = dout_init(&a, out_view(0, 0, DW, DH, 100)) && dout_init(&b, out_view(0, 0, DW / 2, DH, 100)) &&
             dout_init(&c, out_view(DW / 2, 0, DW / 2, DH, 100));
    uint64_t *h0 = malloc(sizeof *h0 * 1024), *h1 = malloc(sizeof *h1 * 1024);
    if (!ok || !h0 || !h1) { CHECK(0, "gpu-drag-extra: out of memory"); goto done; }
    for (int dc = DC_N; dc < DC_ALL; dc++) {
        DOut *two[2] = { &b, &c }, *one[1] = { &a };
        DOut *const *os = dc == DC_TWO ? two : one;
        int nos = dc == DC_TWO ? 2 : 1;
        long full;
        int worst;
        test_gpu = 1;
        test_gdrag = 0;
        test_compose = nos == 1;
        drag_run_n(dc, os, nos, h0, &full, &worst);
        test_gdrag = 1;
        GdragStats st;
        paint_gdrag_stats(&st);
        gdrag_quads = 0;
        drag_run_n(dc, os, nos, h1, &full, &worst);
        test_gdrag = test_compose = 0;
        int bad = 0, first = -1;
        for (int f = 0; f <= dc_frames[dc]; f++) if (h0[f] != h1[f]) { if (first < 0) first = f; bad++; }
        char what[32];
        snprintf(what, sizeof what, "gpu-drag %s", dc_name[dc]);
        /* corner: the box is in every frame's damage, so GPU drag refuses them all (without that refusal the
         * frames differ from the GPU path) */
        gdrag_report(what, dc != DC_CORNER);
        CHECK(!bad, "drag-exact gpu-drag, run %s: %d of %d frames differ from the GPU path (the first: frame %d)", dc_name[dc], bad,
              dc_frames[dc] + 1, first);
    }
done:
    test_gpu = 0;
    dout_free(&a); dout_free(&b); dout_free(&c);
    free(h0); free(h1);
}

static void test_drag_exact(int write) {
    static const char *const variant[4] = { "cpu", "gpu", "gpu-drag", "gpu-drag pipelined" };
    DOut o, o2;                                         /* the display at 100 %, and at 200 % (DC_K2) */
    int ok = dout_init(&o, out_view(0, 0, DW, DH, 100));
    ok = dout_init(&o2, out_view(0, 0, 2 * DW, 2 * DH, 200)) && ok;
    uint64_t *hash = malloc(sizeof *hash * (size_t)dc_hashes());
    if (!ok || !hash) {
        CHECK(0, "drag-exact: out of memory");
        dout_free(&o); dout_free(&o2); free(hash);
        return;
    }
    /* gpu-drag: the GPU path with GPU drag (the window composited by canvas_gdrag_model), against the GPU's hashes */
    /* gpu-drag pipelined: the same, each frame presented after the next frame's changes (the frame pipeline) */
    for (int g = 0; g < (write ? 2 : 4); g++) {
        test_gpu = g > 0;
        test_gdrag = g >= 2;
        test_pipe = g == 3;
        char path[64];
        snprintf(path, sizeof path, "tests/drag_golden_%s.txt", variant[g >= 2 ? 1 : g]);
        long fullw[DC_N];
        int worst[DC_N];
        long parts = 0, renders = 0, blits = 0;
        int k = 0;
        paint_layer_parts();
        paint_layer_stats(&renders, &blits);
        BelowStats bs;
        paint_below_stats(&bs);
        paint_below_bad = paint_below_checked = 0;
        paint_below_verify = !test_gdrag;                  /* GPU drag's frames leave the window to the GPU: the hashes check them */
        GdragStats gdrags;
        paint_gdrag_stats(&gdrags);
        gdrag_quads = 0;
        if (getenv("LORD_BELOW_SWITCH")) paint_below_switch = atoi(getenv("LORD_BELOW_SWITCH"));
        for (int dc = 0; dc < DC_N; dc++) {
            drag_run(dc, dc_k2(dc) ? &o2 : &o, hash + k, &fullw[dc], &worst[dc]);
            k += dc_frames[dc] + 1;
        }
        parts = paint_layer_parts();
        paint_layer_stats(&renders, &blits);
        printf("drag-exact %s: %d frames; layer: %ld renders (%ld of them parts, %ld rebuilds of what is below), %ld copies\n",
               variant[g], k, renders % 1000, parts, renders / 1000, blits);
        paint_below_stats(&bs);
        paint_below_verify = 0;
        printf("drag-exact %s: what is below: %ld built whole, %ld in part; %ld frames switched to it (not: %ld ancestor, "
               "%ld blur, %ld transform, %ld unreached, %ld cost), %ld pixels differ in %ld of them painted the old way\n",
               variant[g], bs.full, bs.part, bs.switched, bs.refused[BR_ANCESTOR], bs.refused[BR_BLUR], bs.refused[BR_TRANSFORM],
               bs.refused[BR_UNREACHED], bs.refused[BR_COST], paint_below_bad, paint_below_checked);
        if (!test_gdrag)
            CHECK(!paint_below_bad && paint_below_checked == bs.switched, "drag-exact %s: frames switched to the copy of what is below "
                  "differ from painting everything: %ld pixels in %ld frames", variant[g], paint_below_bad, paint_below_checked);
        else gdrag_report(variant[g], 1);
        printf("drag-exact %s: most pixels differing from a full repaint (largest difference):", variant[g]);
        for (int dc = 0; dc < DC_N; dc++) printf(" %s %ld (%d)", dc_name[dc], fullw[dc], worst[dc]);
        printf("\n");
        if (write) {
            CHECK(!gold_write(path, variant[g], hash), "drag-exact: can't write %s", path);
            printf("drag-exact %s: wrote %d hashes for %s to %s\n", variant[g], k, GOLD_ARCH, path);
            continue;
        }
        char *file = read_file(path);
        uint64_t h;
        if (!file || !gold_find(file, dc_name[0], 0, &h)) {
            printf("drag-exact %s: skipped: needs golden hashes for %s in %s (make drag-golden on the reference code)\n",
                   variant[g], GOLD_ARCH, path);
            free(file);
            continue;
        }
        k = 0;
        for (int dc = 0; dc < DC_N; dc++) {
            int bad = 0, first = -1;
            for (int fr = 0; fr <= dc_frames[dc]; fr++, k++)
                if (!gold_find(file, dc_name[dc], fr, &h) || h != hash[k]) { if (first < 0) first = fr; bad++; }
            CHECK(!bad, "drag-exact %s, run %s: %d of %d frames differ from %s (the first: frame %d)",
                  variant[g], dc_name[dc], bad, dc_frames[dc] + 1, path, first);
        }
        free(file);
    }
    test_gpu = test_gdrag = test_pipe = 0;
    dout_free(&o);
    dout_free(&o2);
    free(hash);
    if (!write) test_gdrag_extra();
}

/* ---- The corners of a dragged window ----
 * The drag layer is painted on a transparent canvas, and a rounded window that clips its children
 * restores its corners (canvas_corners_restore) from what was there before its title bar filled them:
 * its shadow over transparency. The restore used to make those pixels opaque (it blended as if the
 * saved pixel were opaque), so a dragged window showed black squares outside its rounded corners. A
 * drag frame must show there what a full repaint shows: the light box below and the shadow. Both the
 * CPU and the GPU path, with one radius and with per-corner radii (canvas_corners_restore4). */
static void test_drag_corners(void) {
    DOut o;
    if (!dout_init(&o, out_view(0, 0, DW, DH, 100))) { CHECK(0, "drag-corners: out of memory"); dout_free(&o); return; }
    DOut *const os[1] = { &o };
    for (int v = 0; v < 4; v++) {
        test_gpu = v & 1;
        int per = v >> 1;
        const char *what = test_gpu ? (per ? "gpu per-corner" : "gpu") : (per ? "cpu per-corner" : "cpu");
        const float r4[4] = { 12, per ? 16 : 12, per ? 10 : 12, per ? 14 : 12 };
        ui_reset();
        Node *root = view(NULL);
        ui.root = root;
        sn(root, "backgroundColor", 0xff2b3a4f);
        box_in(root, 150, 100, 500, 360, 0xffd8e4f0);  /* light, so a dark corner shows */
        Node *win = box_in(root, 260, 200, 300, 180, 0xff2c2c2e);
        ss(win, "overflow", "hidden");
        if (per) {
            sn(win, "borderTopLeftRadius", r4[0]); sn(win, "borderTopRightRadius", r4[1]);
            sn(win, "borderBottomRightRadius", r4[2]); sn(win, "borderBottomLeftRadius", r4[3]);
        } else sn(win, "borderRadius", r4[0]);
        sn(win, "borderWidth", 1);
        sn(win, "borderColor", 0x30ffffff);
        dshadow(win, 0x73000000, 8, 24);
        dview(win, 0, 28, 0xff3a3a3c, 0);              /* the title bar fills the top corners' squares */
        sn(view(win), "flexGrow", 1);
        dview(win, 0, 24, 0xff252528, 0);              /* and a status bar the bottom ones */
        canvas_set_clip(&o.c, (Rect){ 0, 0, o.c.w, o.c.h });
        canvas_clear(&o.c, 0x000000);
        damage_add((Box){ 0, 0, DW, DH });
        ui.dirty = 1;
        drag_paint_n(root, os, 1);
        paint_layer_begin(win->id, 0);
        long renders = 0, blits = 0;
        paint_layer_stats(&renders, &blits);
        int worst = 0, dark = 0;
        for (int f = 1; f <= 3; f++) {                  /* the first frame renders the layer, the others copy it */
            int x = 260 + 7 * f, y = 200 + 5 * f;
            win->s.pos[EDGE_L] = (Dim){ (float)x, UNIT_PX };
            win->s.pos[EDGE_T] = (Dim){ (float)y, UNIT_PX };
            node_moved(win);
            ui.dirty = 1;
            drag_paint_n(root, os, 1);
            canvas_clear(&o.ref, 0x000000);
            paint_root_full(&o.ref, root);
            /* each corner's square (canvas_corners_save's, n = ceil(r) + 1) and one pixel around it */
            for (int q = 0; q < 4; q++) {
                int n = (int)ceilf(r4[q]) + 1;
                int qx = q == 0 || q == 3 ? x - 1 : x + 300 - n - 1;
                int qy = q < 2 ? y - 1 : y + 180 - n - 1;
                for (int j = qy; j < qy + n + 2; j++)
                    for (int i = qx; i < qx + n + 2; i++) {
                        uint32_t a = o.px[(size_t)j * o.c.w + i], b = o.rpx[(size_t)j * o.c.w + i];
                        for (int sh = 0; sh < 32; sh += 8) {
                            int d = abs((int)(a >> sh & 255) - (int)(b >> sh & 255));
                            if (d > worst) worst = d;
                        }
                    }
                /* the corner pixel itself, outside the curve: the light box under the shadow, not black */
                int cx = q == 0 || q == 3 ? x : x + 299, cy = q < 2 ? y : y + 179;
                if ((o.px[(size_t)cy * o.c.w + cx] & 0xff) < 0x60) dark++;
            }
        }
        paint_layer_stats(&renders, &blits);
        CHECK(renders % 1000 >= 1 && blits >= 3, "drag-corners %s: the frames did not use the layer (%ld renders, %ld copies)",
              what, renders % 1000, blits);
        /* rounding: the shadow is blended over the light box in another order (a level or two) */
        CHECK(worst <= 2 && !dark, "drag-corners %s: the dragged window's corners differ from a full repaint by %d levels "
              "(%d of 12 corner pixels dark)", what, worst, dark);
        paint_layer_end();
    }
    test_gpu = 0;
    dout_free(&o);
}

/* ---- A setStyle that only turns or scales a node (node_restyled's transform-only path) ----
 * Two copies of one tree: A takes rotate/scaleX/scaleY/transformOriginX through node_restyled (which
 * keeps the node's own layout and every measure when that gives the same results), B through what
 * node_restyled did before (node_invalidate on any change). Between the turns, both get the same
 * other changes (a width that moves by fractions of a pixel, a font size, the root's size), so text
 * measured within its half-pixel cache is in play. Every node must lay out exactly the same (box,
 * absolute position, matrices, text measure) and both trees must paint the same pixels. 'transform'
 * itself (apps/sysinfo/scene.jsx, sdk/cooling.jsx) is no key lord.setStyle takes: it changes nothing. */
typedef struct { Node *root, *col, *label, *spin[5]; } TScene;

static TScene tr_scene(void) {
    TScene t;
    t.root = view(NULL);
    sn(t.root, "backgroundColor", 0xfff2f2f7);
    ss(t.root, "flexDirection", "row"); ss(t.root, "flexWrap", "wrap");
    t.col = dview(t.root, 180, 0, 0xffffffff, 8);
    t.label = dtext(t.col, "A label that wraps over more than one line of this column", 13, 0, 0xff202020);
    t.spin[0] = dview(t.col, 40, 40, 0xff0a84ff, 10);                  /* a leaf */
    t.spin[1] = dview(t.root, 72, 72, 0xff34c759, 16);                  /* with a box inside */
    ss(t.spin[1], "alignItems", "center"); ss(t.spin[1], "justifyContent", "center");
    dview(t.spin[1], 24, 24, 0xffffffff, 12);
    Node *abs = dview(t.spin[1], 10, 10, 0xffff9500, 0);
    ss(abs, "position", "absolute"); sn(abs, "left", 2); sn(abs, "top", 3);
    t.spin[2] = dview(t.root, 0, 0, 0xffff3b30, 6);                     /* with text inside: invalidated */
    sn(t.spin[2], "paddingLeft", 4); sn(t.spin[2], "paddingRight", 4); sn(t.spin[2], "paddingTop", 2);
    dtext(t.spin[2], "Turning words", 12, 600, 0xffffffff);
    Node *tt = dtext(t.root, "Text around ", 14, 0, 0xff000000);       /* a box under a text node */
    t.spin[3] = dview(tt, 12, 12, 0xff5856d6, 0);
    t.spin[4] = dview(t.root, 0, 30, 0xffaf52de, 4);                    /* sized by flex */
    sn(t.spin[4], "flexGrow", 1);
    return t;
}

static int tr_same(const Node *a, const Node *b, int *bad) {
    int n = 1;
    if (a->type != b->type || a->x != b->x || a->y != b->y || a->w != b->w || a->h != b->h || a->ax != b->ax ||
        a->ay != b->ay || a->own_tf != b->own_tf || a->has_tf != b->has_tf ||
        (a->type == NODE_TEXT && (a->measure_w != b->measure_w || a->measure_h != b->measure_h)) ||
        (a->has_tf && (memcmp(&a->tf->own, &b->tf->own, sizeof a->tf->own) || memcmp(&a->tf->world, &b->tf->world, sizeof a->tf->world) ||
                       memcmp(&a->tf->inv, &b->tf->inv, sizeof a->tf->inv)))) (*bad)++;
    const Node *ca = a->first, *cb = b->first;
    for (; ca && cb; ca = ca->next, cb = cb->next) n += tr_same(ca, cb, bad);
    if (ca || cb) (*bad)++;
    return n;
}

/* what node_restyled did with a changed transform before the fast path */
static void tr_set_old(Node *n, const char *k, double v) {
    Style before = n->s;
    StyleExt xb;
    if (n->s.ext) memcpy(&xb, n->s.ext, STYLE_EXT_VALUES);
    style_set_number(&n->s, k, v);
    if (memcmp(&before, &n->s, sizeof before) || !before.ext || memcmp(&xb, n->s.ext, STYLE_EXT_VALUES)) node_invalidate(n);
}

static void test_transform_restyle(void) {
    ui_reset();
    TScene a = tr_scene(), b = tr_scene();
    enum { TW = 420, TH = 300 };
    uint32_t *pa = calloc((size_t)TW * TH, 4), *pb = calloc((size_t)TW * TH, 4);
    Canvas ca, cb;
    canvas_init(&ca, pa, TW, TH, TW);
    canvas_init(&cb, pb, TW, TH, TW);
    long bad = 0, nodes = 0, pxbad = 0;
    int fast = 0, slow = 0;
    const char *const key[] = { "rotate", "scaleX", "scaleY", "transformOriginX", "transformOriginY" };
    for (int f = 0; f < 120; f++) {
        if (f % 7 == 3) { sn(a.col, "width", 180 + (f % 5) * 0.3); sn(b.col, "width", 180 + (f % 5) * 0.3); }
        if (f % 11 == 5) { sn(a.label, "fontSize", 13 + (f % 3)); sn(b.label, "fontSize", 13 + (f % 3)); }
        for (int i = 0; i < 5; i++) {
            const char *k = key[f % 9 < 5 ? 0 : (f % 9 - 4)];
            double v = k == key[0] ? f * 7 + i * 13 : k == key[3] || k == key[4] ? 4 + (f % 5) * 2.5 : 0.55 + (f % 4) * 0.125;
            Node *n = a.spin[i];
            int had = n->s.ext != NULL, lok = n->layout_ok;
            ui.dirty = 0;
            js_set(n, k, v);
            tr_set_old(b.spin[i], k, v);
            if (had && lok && i != 2 && i != 3) {
                fast += n->layout_ok;
                CHECK(ui.dirty && n->paint_dirty && !n->parent->layout_ok, "transform-only change not repainted or placed");
            }
            if (i == 2 || i == 3) { slow++; CHECK(!n->layout_ok, "node %d: laid out from its cache with text in the way", i); }
        }
        float w = TW - (f % 4) * 0.2f, h = TH;
        layout_root(a.root, w, h);
        layout_root(b.root, w, h);
        int nb = 0;
        nodes += tr_same(a.root, b.root, &nb);
        bad += nb;
        paint_damage(a.root);
        paint_damage(b.root);
        ui.damage = (Box){ 0, 0, 0, 0 };
        canvas_clear(&ca, 0);
        canvas_clear(&cb, 0);
        paint_root_full(&ca, a.root);
        paint_root_full(&cb, b.root);
        for (size_t i = 0; i < (size_t)TW * TH; i++) pxbad += pa[i] != pb[i];
    }
    CHECK(bad == 0, "transform-only setStyle: %ld of %ld node layouts differ", bad, nodes);
    CHECK(pxbad == 0, "transform-only setStyle: %ld pixels differ", pxbad);
    CHECK(fast > 300, "the transform-only path was not taken (%d)", fast);
    CHECK(slow == 240, "text cases: %d", slow);

    /* 'transform' (not a key): nothing set, nothing marked */
    Node *n = a.spin[0];
    layout_root(a.root, TW, TH);
    paint_damage(a.root);
    ui.damage = (Box){ 0, 0, 0, 0 };
    ui.dirty = 0;
    Style before = n->s;
    StyleExt xb;
    memcpy(&xb, n->s.ext, STYLE_EXT_VALUES);
    int lok = n->layout_ok, plok = n->parent->layout_ok;
    CHECK(style_set_string(&n->s, "transform", "rotate(30deg)") != 0, "'transform' became a style key");
    node_restyled(n, &before, &xb);
    CHECK(!ui.dirty && !n->paint_dirty && n->layout_ok == lok && n->parent->layout_ok == plok &&
          !memcmp(&before, &n->s, sizeof before) && !memcmp(&xb, n->s.ext, STYLE_EXT_VALUES),
          "setStyle 'transform' changed something");
    Node *bare = dview(a.root, 10, 10, 0xff000000, 0);                   /* no StyleExt yet */
    layout_root(a.root, TW, TH);
    paint_damage(a.root);
    ui.dirty = 0;
    before = bare->s;
    style_set_string(&bare->s, "transform", "scale(2)");
    node_restyled(bare, &before, NULL);
    CHECK(!ui.dirty && !bare->paint_dirty && !bare->s.ext, "setStyle 'transform' on a plain node changed something");
    ui.damage = (Box){ 0, 0, 0, 0 };
    free(pa);
    free(pb);
}

#endif

/* ---- benchmark: a Files-like list and a window drag (docs/DESIGN.md, Engine notes) ---- */
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double median(double *v, int n) { qsort(v, (size_t)n, sizeof *v, cmp_d); return v[n / 2]; }

static Node *text(Node *parent, const char *s, float size) {
    Node *t = node_new(NODE_TEXT), *r = node_new(NODE_RAW);
    node_set_text(r, s);
    node_append(t, r);
    node_append(parent, t);
    sn(t, "fontSize", size);
    sn(t, "color", 0xff202020);
    return t;
}

typedef struct { int w, h; float x, k; uint32_t *px; Canvas c; } Disp;

static double t_layout, t_damage, t_paint;
static void frame_paint(Node *root, Disp *d, int nd) {
    double q0 = now_ms();
    layout_root(root, 1280, 800);
    double q1 = now_ms();
    Box dmg = { 0, 0, 0, 0 };
    if (paint_damage(root)) dmg = ui.damage;
    double q2 = now_ms();
    t_layout += q1 - q0;
    t_damage += q2 - q1;
    int ln = paint_layer_node();
    if (ln) {
        Node *w = node_get(ln);
        paint_layer_view(w && w->prev_box.x0 + (w->prev_box.x1 - w->prev_box.x0) / 2 >= 1280 / (float)nd * (nd - 1) && nd > 1 ? 1 : 0);
    }
    for (int i = 0; i < nd; i++) {
        if (dmg.x1 <= dmg.x0) break;
        float vx0 = d[i].x, vx1 = d[i].x + d[i].w / d[i].k;
        Box b = { fmaxf(dmg.x0, vx0), fmaxf(dmg.y0, 0), fminf(dmg.x1, vx1), fminf(dmg.y1, d[i].h / d[i].k) };
        if (b.x1 <= b.x0 || b.y1 <= b.y0) continue;
        paint_view = (PaintView){ d[i].x, 0, d[i].k, 0, 0, i };
        Rect r = { (int)floorf((b.x0 - d[i].x) * d[i].k), (int)floorf(b.y0 * d[i].k), (int)ceilf((b.x1 - d[i].x) * d[i].k), (int)ceilf(b.y1 * d[i].k) };
        if (r.x1 > d[i].w) r.x1 = d[i].w;
        if (r.y1 > d[i].h) r.y1 = d[i].h;
        ui.damage = b;
        canvas_set_clip(&d[i].c, r);
        paint_root_clear(&d[i].c, root, 0);
        canvas_set_clip(&d[i].c, (Rect){ 0, 0, d[i].w, d[i].h });
    }
    paint_view = (PaintView){ 0, 0, 1, 0, 0, 0 };
    ui.damage = (Box){ 0, 0, 0, 0 };
    t_paint += now_ms() - q2;
}

static void bench(void) {
    static const char *const words[] = { "Quarterly", "report", "holiday", "photos", "final", "version", "backup", "draft",
                                         "invoice", "scan", "notes", "presentation", "budget", "summary" };
    quiet = 1;
    ui_reset();
    Node *root = view(NULL);
    ui.root = root;
    sn(root, "backgroundColor", 0xff3a4a5a);
    /* Files: a 900×600 window with a 2000-row list view (icon, name with ellipsis, size, date) */
    Node *win = view(root);
    ss(win, "position", "absolute");
    sn(win, "left", 120); sn(win, "top", 80); sn(win, "width", 900); sn(win, "height", 600);
    sn(win, "backgroundColor", 0xfff5f5f5); sn(win, "borderRadius", 10);
    sn(win, "shadowColor", 0x66000000); sn(win, "shadowBlur", 24); sn(win, "shadowOffsetY", 8);
    ss(win, "overflow", "hidden");
    Node *bar = view(win);
    sn(bar, "height", 44); sn(bar, "backgroundColor", 0xffe8e8e8);
    sn(bar, "borderBottomWidth", 1); sn(bar, "borderBottomColor", 0xffd0d0d0);
    Node *list = view(win);
    sn(list, "flexGrow", 1); ss(list, "overflow", "hidden");
    Node *inner = view(list);
    char name[256];
    srand(7);
    for (int i = 0; i < 2000; i++) {
        Node *row = view(inner);
        ss(row, "flexDirection", "row"); ss(row, "alignItems", "center");
        sn(row, "height", 24); sn(row, "paddingLeft", 10); sn(row, "paddingRight", 10); sn(row, "gap", 8);
        if (i & 1) sn(row, "backgroundColor", 0xffefefef);
        Node *icon = view(row);
        sn(icon, "width", 16); sn(icon, "height", 16); sn(icon, "borderRadius", 3); sn(icon, "backgroundColor", 0xff5b8def);
        int len = 0, nw = 2 + rand() % 9;
        for (int k = 0; k < nw; k++) len += snprintf(name + len, sizeof name - (size_t)len, "%s%s", k ? " " : "", words[rand() % 14]);
        snprintf(name + len, sizeof name - (size_t)len, " %d.pdf", i);
        Node *t = text(row, name, 13);
        sn(t, "flexGrow", 1); sn(t, "flexShrink", 1); sn(t, "flexBasis", 0);
        ss(t, "whiteSpace", "nowrap");
        if (!getenv("BENCH_NO_ELLIPSIS")) ss(t, "textOverflow", "ellipsis");   /* ignored by an engine without it */
        Node *size = text(row, "1.2 MB", 12);
        sn(size, "width", 70); ss(size, "textAlign", "right");
        Node *date = text(row, "18 Oct 2026, 04:20", 12);
        sn(date, "width", 130);
    }
    /* a second window to drag over it (a dialog with some controls) */
    Node *dlg = view(root);
    ss(dlg, "position", "absolute");
    sn(dlg, "left", 300); sn(dlg, "top", 200); sn(dlg, "width", 520); sn(dlg, "height", 360);
    sn(dlg, "backgroundColor", 0xffffffff); sn(dlg, "borderRadius", 10); sn(dlg, "padding", 20); sn(dlg, "gap", 8);
    sn(dlg, "shadowColor", 0x66000000); sn(dlg, "shadowBlur", 24); sn(dlg, "shadowOffsetY", 8);
    for (int i = 0; i < 12; i++) {
        Node *r = view(dlg);
        ss(r, "flexDirection", "row"); sn(r, "height", 20); sn(r, "gap", 8);
        text(r, "Label of the setting", 13);
        Node *f = view(r);
        sn(f, "flexGrow", 1); sn(f, "borderWidth", 1); sn(f, "borderColor", 0xffc0c0c0); sn(f, "borderRadius", 5);
    }
    /* a Dock with blur, above the windows */
    Node *dock = view(root);
    ss(dock, "position", "absolute");
    sn(dock, "left", 340); sn(dock, "bottom", 8); sn(dock, "width", 600); sn(dock, "height", 64);
    sn(dock, "backgroundColor", 0x80ffffff); sn(dock, "borderRadius", 16); sn(dock, "backdropBlur", 20);

    Disp d[2];
    d[0] = (Disp){ 1280, 800, 0, 1, NULL, {0} };
    d[1] = (Disp){ 1920, 1200, 1280, 1.5f, NULL, {0} };
    for (int i = 0; i < 2; i++) { d[i].px = calloc((size_t)d[i].w * d[i].h, 4); canvas_init(&d[i].c, d[i].px, d[i].w, d[i].h, d[i].w); }

    /* layout: cold (every text measured) and warm (cache); medians, the machine may be busy */
    double t0 = now_ms();
    layout_root(root, 1280, 800);
    double cold = now_ms() - t0;
    enum { N = 41 };
    double v[N];
    for (int i = 0; i < N; i++) { node_invalidate(inner); double a = now_ms(); layout_root(root, 1280, 800); v[i] = now_ms() - a; }
    double warm = median(v, N);
    /* full paint of the screen */
    paint_damage(root);
    for (int i = 0; i < N; i++) {
        ui.damage = (Box){ 0, 0, 1280, 800 };
        double a = now_ms();
        paint_root_clear(&d[0].c, root, 0);
        v[i] = now_ms() - a;
    }
    printf("list: 2000 rows; layout cold %.2f ms, warm %.2f ms; full paint %.2f ms\n", cold, warm, median(v, N));
    /* scrolling the list: move the inner column and repaint (layout + damage + paint) */
    for (int i = 0; i < N; i++) {
        sn(inner, "marginTop", -24.0 * i);
        node_invalidate(inner);
        double a = now_ms();
        frame_paint(root, d, 1);
        v[i] = now_ms() - a;
    }
    printf("scroll frame: %.2f ms\n", median(v, N));

    /* drag: the dialog moves for 240 frames, on one display and on two (the second at 150 %) */
    for (int nd = 1; nd <= 2; nd++) {
        ui.damage = (Box){ 0, 0, 1280 * (float)nd, 800 };
        frame_paint(root, d, nd);
        paint_layer_begin(dlg->id, 0);
        long r, b;
        paint_layer_stats(&r, &b);
        double worst = 0, sum = 0;
        enum { F = 240 };
        static double fv[F];
        t_layout = t_damage = t_paint = 0;
        for (int f = 0; f < F; f++) {
            float ang = f * 0.05f;
            float x = 300 + 260 * cosf(ang) + (nd == 2 ? 340 : 0), y = 200 + 120 * sinf(ang);
            dlg->s.pos[EDGE_L] = (Dim){ x, UNIT_PX };
            dlg->s.pos[EDGE_T] = (Dim){ y, UNIT_PX };
#ifndef ENGINE_BEFORE
            node_moved(dlg);                    /* as grab.c does */
#endif
            ui.dirty = 1;
            double a = now_ms();
            frame_paint(root, d, nd);
            double ms = now_ms() - a;
            fv[f] = ms;
            sum += ms;
            if (f > 0 && ms > worst) worst = ms;
        }
        paint_layer_stats(&r, &b);
        paint_layer_end();
        frame_paint(root, d, nd);
        printf("drag (%d display%s): median %.2f ms per frame, mean %.2f (layout %.2f, damage %.2f, paint %.2f; worst %.2f after the first), layer renders %ld, blits %ld\n",
               nd, nd > 1 ? "s" : "", median(fv, F), sum / F, t_layout / F, t_damage / F, t_paint / F, worst, r, b);
    }
}

int main(int argc, char **argv) {
    const char *dir = getenv("LORD_FONTS") ? getenv("LORD_FONTS") : "../assets/fonts";
    fonts_set_builtin_dir(dir);
    if (text_init(dir)) { printf("engine-test: no fonts in %s\n", dir); return 1; }
    if (argc > 1 && !strcmp(argv[1], "bench")) { bench(); return 0; }
#ifdef ENGINE_BEFORE
    return 0;
#else
    if (argc > 1 && !strcmp(argv[1], "drag-golden")) { test_drag_exact(1); return fails ? 1 : 0; }
    test_drag_exact(0);                     /* first: the transform cache is keyed by node id */
    test_drag_corners();
    test_ellipsis();
    test_borders();
    test_logical();
    test_transform_math();
    test_transform_paint_hit();
    test_transform_self_restyle();
    test_layers();
    test_backdrop_band();
    test_multi_damage();
    test_minimize_damage();
    test_hidden_blur_damage();
    test_damage_pipeline();
    test_damage_fuzz();
    test_grow_wrap_height();
    test_overlay_scrollbar();
    test_transform_restyle();
    printf("engine-test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
#endif
}
