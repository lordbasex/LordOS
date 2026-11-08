/* term_paint.c — draws a terminal session inside its node (NODE_TERM). */
#include "term.h"
#include "../ui/ui.h"
#include "../ui/bidi.h"
#include "../ui/shape.h"
#include "../ui/unicode.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- primitives ---- */

static void fill(Canvas *c, int x0, int y0, int x1, int y1, uint32_t argb) {
    uint32_t a = argb >> 24;
    if (!a) return;
    if (x0 < c->clip.x0) x0 = c->clip.x0;
    if (y0 < c->clip.y0) y0 = c->clip.y0;
    if (x1 > c->clip.x1) x1 = c->clip.x1;
    if (y1 > c->clip.y1) y1 = c->clip.y1;
    if (x0 >= x1 || y0 >= y1) return;
    uint32_t rgb = argb & 0xffffff;
    for (int y = y0; y < y1; y++) {
        uint32_t *p = c->px + (size_t)y * c->stride;
        if (a == 255) { for (int x = x0; x < x1; x++) p[x] = rgb | 0xff000000u; continue; }
        uint32_t sr = (rgb >> 16) * a, sg = (rgb >> 8 & 0xff) * a, sb = (rgb & 0xff) * a, ia = 255 - a;
        for (int x = x0; x < x1; x++) {
            uint32_t d = p[x];
            p[x] = (a + ((d >> 24) * ia + 127) / 255) << 24 |
                   ((sr + (d >> 16 & 0xff) * ia) / 255) << 16 | ((sg + (d >> 8 & 0xff) * ia) / 255) << 8 | (sb + (d & 0xff) * ia) / 255;
        }
    }
}

static uint32_t mix(uint32_t a, uint32_t b, float t) {
    int r = (int)((a >> 16 & 0xff) * (1 - t) + (b >> 16 & 0xff) * t);
    int g = (int)((a >> 8 & 0xff) * (1 - t) + (b >> 8 & 0xff) * t);
    int bl = (int)((a & 0xff) * (1 - t) + (b & 0xff) * t);
    return 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
}

static float lum(uint32_t c) {
    float r = (c >> 16 & 0xff) / 255.0f, g = (c >> 8 & 0xff) / 255.0f, b = (c & 0xff) / 255.0f;
    return 0.2126f * r * r + 0.7152f * g * g + 0.0722f * b * b;     /* gamma ≈ 2 */
}

static float contrast(uint32_t a, uint32_t b) {
    float la = lum(a), lb = lum(b);
    return la > lb ? (la + 0.05f) / (lb + 0.05f) : (lb + 0.05f) / (la + 0.05f);
}

/* "Minimum contrast": moves the text towards white or black until the requested contrast. */
static uint32_t min_contrast(uint32_t fg, uint32_t bg, float amount) {
    if (amount <= 0) return fg;
    float target = 1 + amount * 20;
    if (contrast(fg, bg) >= target) return fg;
    uint32_t to = lum(bg) < 0.25f ? 0xffffffffu : 0xff000000u;
    float lo = 0, hi = 1;
    for (int i = 0; i < 7; i++) {
        float mid = (lo + hi) / 2;
        if (contrast(mix(fg, to, mid), bg) >= target) hi = mid; else lo = mid;
    }
    return mix(fg, to, hi);
}

/* ---- box and block drawing (U+2500–U+259F) so they join across cells ---- */

/* Weights per direction (left, right, up, down): 1 light, 2 heavy, 3 double. */
static const uint16_t box_lines[] = {
    /* 2500 */ 0x1100, 0x2200, 0x0011, 0x0022, 0x1100, 0x2200, 0x0011, 0x0022, 0x1100, 0x2200, 0x0011, 0x0022,
    /* 250C */ 0x0101, 0x0201, 0x0102, 0x0202, 0x1001, 0x2001, 0x1002, 0x2002,
    /* 2514 */ 0x0110, 0x0210, 0x0120, 0x0220, 0x1010, 0x2010, 0x1020, 0x2020,
    /* 251C */ 0x0111, 0x0211, 0x0121, 0x0112, 0x0122, 0x0221, 0x0212, 0x0222,
    /* 2524 */ 0x1011, 0x2011, 0x1021, 0x1012, 0x1022, 0x2021, 0x2012, 0x2022,
    /* 252C */ 0x1101, 0x2101, 0x1201, 0x2201, 0x1102, 0x2102, 0x1202, 0x2202,
    /* 2534 */ 0x1110, 0x2110, 0x1210, 0x2210, 0x1120, 0x2120, 0x1220, 0x2220,
    /* 253C */ 0x1111, 0x2111, 0x1211, 0x2211, 0x1121, 0x1112, 0x1122, 0x2121, 0x1221, 0x2112, 0x1212,
    /* 2547 */ 0x2221, 0x2212, 0x2122, 0x1222, 0x2222,
    /* 254C */ 0x1100, 0x2200, 0x0011, 0x0022,
    /* 2550 */ 0x3300, 0x0033, 0x0301, 0x0103, 0x0303, 0x3001, 0x1003, 0x3003, 0x0310, 0x0130, 0x0330,
    /* 255B */ 0x3010, 0x1030, 0x3030, 0x0311, 0x0133, 0x0333, 0x3011, 0x1033, 0x3033, 0x3301, 0x1103,
    /* 2566 */ 0x3303, 0x3310, 0x1130, 0x3330, 0x3311, 0x1133, 0x3333,
    /* 256D */ 0x0101, 0x1001, 0x1010, 0x0110,
};

static int box_draw(Canvas *c, uint32_t cp, int x, int y, int w, int h, uint32_t col) {
    int lt = w / 8 > 1 ? w / 8 : 1, ht = lt * 2;
    int mx = x + w / 2, my = y + h / 2;
    if (cp >= 0x2500 && cp <= 0x2570) {
        uint16_t v = box_lines[cp - 0x2500];
        int wl = v >> 12, wr = v >> 8 & 15, wu = v >> 4 & 15, wd = v & 15;
        int dbl = lt + 1;   /* spacing of double lines */
        for (int dir = 0; dir < 4; dir++) {
            int wgt = dir == 0 ? wl : dir == 1 ? wr : dir == 2 ? wu : wd;
            if (!wgt) continue;
            int th = wgt == 2 ? ht : lt;
            int offs[2] = { 0, 0 }, n = 1;
            if (wgt == 3) { offs[0] = -dbl; offs[1] = dbl; n = 2; }
            for (int k = 0; k < n; k++) {
                int o = offs[k];
                if (dir < 2) {
                    int y0 = my - th / 2 + o, x0 = dir == 0 ? x : mx - th / 2 - (wgt == 3 ? dbl : 0);
                    int x1 = dir == 0 ? mx + (th + 1) / 2 + (wgt == 3 ? dbl : 0) : x + w;
                    fill(c, x0, y0, x1, y0 + th, col);
                } else {
                    int x0 = mx - th / 2 + o, y0 = dir == 2 ? y : my - th / 2 - (wgt == 3 ? dbl : 0);
                    int y1 = dir == 2 ? my + (th + 1) / 2 + (wgt == 3 ? dbl : 0) : y + h;
                    fill(c, x0, y0, x0 + th, y1, col);
                }
            }
        }
        return 1;
    }
    if (cp >= 0x2574 && cp <= 0x257f) {
        static const uint16_t half[] = { 0x1000, 0x0010, 0x0100, 0x0001, 0x2000, 0x0020, 0x0200, 0x0002,
                                          0x1200, 0x0012, 0x2100, 0x0021 };
        uint16_t v = half[cp - 0x2574];
        int wl = v >> 12, wr = v >> 8 & 15, wu = v >> 4 & 15, wd = v & 15;
        if (wl) { int t = wl == 2 ? ht : lt; fill(c, x, my - t / 2, mx + 1, my - t / 2 + t, col); }
        if (wr) { int t = wr == 2 ? ht : lt; fill(c, mx, my - t / 2, x + w, my - t / 2 + t, col); }
        if (wu) { int t = wu == 2 ? ht : lt; fill(c, mx - t / 2, y, mx - t / 2 + t, my + 1, col); }
        if (wd) { int t = wd == 2 ? ht : lt; fill(c, mx - t / 2, my, mx - t / 2 + t, y + h, col); }
        return 1;
    }
    if (cp >= 0x2580 && cp <= 0x259f) {
        int x2 = x + w, y2 = y + h;
        switch (cp) {
        case 0x2580: fill(c, x, y, x2, y + h / 2, col); return 1;
        case 0x2588: fill(c, x, y, x2, y2, col); return 1;
        case 0x2590: fill(c, mx, y, x2, y2, col); return 1;
        case 0x2594: fill(c, x, y, x2, y + h / 8 + 1, col); return 1;
        case 0x2595: fill(c, x2 - w / 8 - 1, y, x2, y2, col); return 1;
        case 0x2591: case 0x2592: case 0x2593: {
            uint32_t a = cp == 0x2591 ? 0x40 : cp == 0x2592 ? 0x80 : 0xc0;
            fill(c, x, y, x2, y2, a << 24 | (col & 0xffffff));
            return 1;
        }
        default: break;
        }
        if (cp >= 0x2581 && cp <= 0x2587) { int k = (int)(cp - 0x2580); fill(c, x, y2 - h * k / 8, x2, y2, col); return 1; }
        if (cp >= 0x2589 && cp <= 0x258f) { int k = 8 - (int)(cp - 0x2588); fill(c, x, y, x + w * k / 8, y2, col); return 1; }
        /* quadrants: bits UL, UR, LL, LR */
        static const uint8_t quad[] = { 0x4, 0x8, 0x1, 0xd, 0x9, 0x7, 0xb, 0x2, 0x6, 0xe };
        uint8_t q = quad[cp - 0x2596];
        if (q & 1) fill(c, x, y, mx, my, col);
        if (q & 2) fill(c, mx, y, x2, my, col);
        if (q & 4) fill(c, x, my, mx, y2, col);
        if (q & 8) fill(c, mx, my, x2, y2, col);
        return 1;
    }
    if (cp == 0xe0b0 || cp == 0xe0b2) {     /* powerline arrows */
        for (int r = 0; r < h; r++) {
            float d = r < h / 2.0f ? r : h - 1 - r;
            int span = (int)roundf(d * 2 * w / h);
            if (cp == 0xe0b0) fill(c, x, y + r, x + span, y + r + 1, col);
            else fill(c, x + w - span, y + r, x + w, y + r + 1, col);
        }
        return 1;
    }
    return 0;
}

/* ---- images ---- */

static uint32_t *scale(const uint32_t *src, int sw, int sh, int dw, int dh) {
    uint32_t *out = malloc((size_t)dw * dh * 4);
    if (!out) return NULL;
    for (int y = 0; y < dh; y++) {
        int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint32_t p = src[(size_t)yy * sw + xx];
                    a += p >> 24; r += p >> 16 & 0xff; g += p >> 8 & 0xff; b += p & 0xff; n++;
                }
            out[(size_t)y * dw + x] = (a / n) << 24 | (r / n) << 16 | (g / n) << 8 | b / n;
        }
    }
    return out;
}

static void draw_images(Canvas *c, Term *t, int ox, int oy, int cw, int ch, float op) {
    long long top;
    vt_view_line(t, 0, &top);
    if (t->buf != &t->main) return;
    for (int i = 0; i < t->nimages; i++) {
        TermImage *im = &t->images[i];
        if (im->line + im->hc <= top || im->line >= top + t->rows) continue;
        int bw = im->wc * cw, bh = im->hc * ch;
        float s = fminf((float)bw / im->pw, (float)bh / im->ph);
        int dw = (int)(im->pw * s), dh = (int)(im->ph * s);
        if (dw < 1 || dh < 1) continue;
        if (!im->scaled || im->sw != dw || im->sh != dh) {
            free(im->scaled);
            im->scaled = scale(im->px, im->pw, im->ph, dw, dh);
            im->sw = dw;
            im->sh = dh;
            if (!im->scaled) continue;
        }
        int px = ox + im->col * cw, py = oy + (int)(im->line - top) * ch;
        canvas_blit(c, im->scaled, dw, dh, px, py, 0, op, 0);
    }
}

/* ---- session ---- */

static void cell_colors(const Term *t, const TermProfile *p, Cell cell, long long line, int col,
                        uint32_t *fg, uint32_t *bg, int *bg_default) {
    uint32_t f = cell.fg;
    if ((cell.attr & A_BOLD) && p->bold_bright && (f >> 24) == 1 && (f & 0xff) < 8) f = COLOR_IDX((f & 0xff) + 8);
    uint32_t F = vt_resolve(t, f, 1), B = vt_resolve(t, cell.bg, 0);
    if (!f && (cell.attr & A_BOLD) && p->bold) F = p->bold | 0xff000000u;
    int def = cell.bg == 0;
    if (t->modes & M_REVERSE) { uint32_t x = F; F = B; B = x; def = 0; }
    if (cell.attr & A_INVERSE) { uint32_t x = F; F = B; B = x; def = 0; }
    if (cell.attr & A_DIM) F = mix(F, B, 0.45f);
    int m = t->nmatches ? vt_match_at(t, line, col) : 0;
    if (m) { B = m == 2 ? 0xffff9f0a : 0xffffd60a; F = 0xff000000; def = 0; }
    else if (vt_selected(t, line, col)) {
        B = p->sel_bg | 0xff000000u;
        if (p->has_sel_fg) F = p->sel_fg | 0xff000000u;
        def = 0;
    }
    if (cell.link && !m) F = p->link | 0xff000000u;
    *fg = min_contrast(F, B, p->min_contrast);
    *bg = B;
    *bg_default = def;
}

/* ---- text: font choice, ligatures, graphemes and emoji ---- */

static int has_cp(int font, uint32_t cp) { return font >= 0 && text_font_has(font, cp); }

/* Is the color emoji font preferred? (emoji presentation: with VS16, or wide ones). */
static int wants_emoji(uint32_t base, const uint32_t *seq, int n, uint16_t attr) {
    for (int i = 1; i < n; i++) { if (seq[i] == 0xfe0f) return 1; if (seq[i] == 0xfe0e) return 0; }
    if (!(attr & A_WIDE)) return 0;
    return base >= 0x1f000 || (base >= 0x2300 && base <= 0x2bff);
}

/* Font for a character: the profile's (and the "non-ASCII" one), the bundled one, the
 * fallbacks and the emoji one. *fallback = 1 if it is not the profile's monospace (it is centered in the cell). */
static int complex_script(uint32_t cp);

static int pick_font(const TermProfile *p, uint32_t cp, uint16_t attr, int emoji, int *fallback) {
    int bold = (attr & A_BOLD) && p->use_bold, italic = (attr & A_ITALIC) && p->use_italic;
    int color = emoji || cp >= 0x1f000 ? term_color_font() : -1;
    *fallback = 0;
    if (emoji && has_cp(color, cp)) { *fallback = 1; return color; }
    int f;
    if (cp >= 0x80 && (f = term_profile_font(p, 1, bold, italic)) >= 0 && has_cp(f, cp)) return f;
    if (has_cp(f = term_profile_font(p, 0, bold, italic), cp)) return f;
    if (has_cp(f = term_profile_font(p, 0, 0, 0), cp)) return f;
    *fallback = 1;
    /* Arabic, Hebrew, Indic, Thai, Tibetan, Burmese, Khmer…: the Noto font for the script
     * first, so that a word does not mix fonts (DejaVu Sans Mono has some Arabic letters
     * and not others) */
    if (((cp >= 0x0590 && cp <= 0x0EFF) || complex_script(cp)) && (f = text_script_fallback(cp, bold)) >= 0) return f;
    for (int i = 0; (f = term_fallback_font(i)) >= 0; i++) if (has_cp(f, cp)) return f;
    if ((f = text_script_fallback(cp, bold)) >= 0) return f;       /* CJK and the rest */
    if (color < 0) color = term_color_font();
    if (has_cp(color, cp)) { *fallback = 1; return color; }
    return -1;
}

static void tofu(Canvas *c, int x, int ry, int w, int ch, uint32_t fg) {
    fill(c, x + 2, ry + 3, x + w - 2, ry + 4, fg);
    fill(c, x + 2, ry + ch - 4, x + w - 2, ry + ch - 3, fg);
    fill(c, x + 2, ry + 3, x + 3, ry + ch - 3, fg);
    fill(c, x + w - 3, ry + 3, x + w - 2, ry + ch - 3, fg);
}

/* What is drawn in a cell: font and glyphs (after substitutions). */
typedef struct {
    int font, fallback, fake_bold, n;
    uint16_t g[CLUSTER_LEN];
} CellText;

static CellText rowt[TERM_MAX_COLS];
static GlyphRun run;

static int run_set(int n) {
    if (n > run.cap) {
        uint16_t *g = realloc(run.g, (size_t)n * sizeof *g);
        if (!g) return -1;
        run.g = g;
        int *cl = realloc(run.cl, (size_t)n * sizeof *cl);
        if (!cl) return -1;
        run.cl = cl;
        run.cap = n;
    }
    run.len = n;
    return 0;
}

static int is_box(uint32_t cp) {     /* what box_draw() draws by hand */
    return (cp >= 0x2500 && cp <= 0x2570) || (cp >= 0x2574 && cp <= 0x259f) || cp == 0xe0b0 || cp == 0xe0b2;
}

/* ---- Arabic and Hebrew (bidi) ----
 * A row with right-to-left text is shown in visual order (Unicode bidi with a
 * left-to-right paragraph, like Konsole and mlterm) and Arabic runs with their joined
 * forms, drawn at their natural width inside the cells they occupy. The content does not
 * change (copy, search and the program see the logical order). It is not applied on the
 * alternate screen (vim, less, mc): those programs lay out their own screen. */

static int row_bidi, rvis[TERM_MAX_COLS];
static uint8_t rlv[TERM_MAX_COLS];

static int rtl_char(uint32_t cp) {
    if (cp < 0x0590 || (cp > 0x08FF && cp < 0xFB1D) || (cp > 0xFEFC && cp < 0x10800)) return 0;
    int b = bidi_class(cp);
    return b == BIDI_R || b == BIDI_AL;
}

/* vis[col] = visual column of logical cell col; 0 if the row stays as it is. */
int term_row_visual(const TermProfile *p, const Term *t, const Line *l, int cols, int *vis, uint8_t *lv) {
    if (!p->bidi || !l || t->buf == &t->alt) return 0;
    int n = l->len < cols ? l->len : cols, any = 0;
    static uint32_t cps[TERM_MAX_COLS];
    for (int i = 0; i < n; i++) {
        Cell c = l->c[i];
        uint32_t cp = c.cp;
        if (c.attr & A_WIDE_TAIL) cp = i > 0 ? cps[i - 1] : ' ';
        else if (cp & CP_CLUSTER) { uint32_t seq[CLUSTER_LEN]; vt_cluster_cps(cp, seq, CLUSTER_LEN); cp = seq[0]; }
        else if (cp < ' ') cp = ' ';
        cps[i] = cp;
        if (!any && rtl_char(cp)) any = 1;
    }
    if (!any) return 0;
    static int order[TERM_MAX_COLS];
    int para = bidi_levels(cps, n, 0, lv, NULL);
    bidi_line(cps, lv, 0, n, para, order);
    for (int k = 0; k < n; k++) vis[order[k]] = k;
    for (int i = n; i < cols; i++) { vis[i] = i; lv[i] = 0; }
    for (int i = 0; i + 1 < n; i++)          /* a wide character occupies its two cells in order */
        if ((l->c[i].attr & A_WIDE) && (l->c[i + 1].attr & A_WIDE_TAIL) && vis[i + 1] < vis[i]) { int tmp = vis[i]; vis[i] = vis[i + 1]; vis[i + 1] = tmp; }
    return 1;
}

typedef struct { int c0, c1, font, ng, drawn, rtl; uint16_t g[128]; float gx[128], gy[128], width; } Span;
#define MAX_SPANS 64
static Span spans[MAX_SPANS];
static int nspans;
static short span_of[TERM_MAX_COLS];
static Shaper tshaper;

/* Scripts that are shaped in runs: right-to-left ones that join letters or carry marks
 * (Arabic, Hebrew…) and those that reorder and combine (Indic, Sinhala, Thai, Lao,
 * Tibetan, Burmese, Khmer, Mongolian, Balinese, Javanese and the rest of the Universal Shaping Engine). */
static int rtl_span_script(int sc) { return sc == SCRIPT_ARAB || sc == SCRIPT_SYRC || sc == SCRIPT_NKOO || sc == SCRIPT_HEBR; }
static int complex_script(uint32_t cp) {
    return (cp >= 0x0900 && cp <= 0x10FF) || (cp >= 0x1700 && cp <= 0x18AF) || (cp >= 0x1900 && cp <= 0x1CFF) ||
           (cp >= 0xA800 && cp <= 0xABFF) || (cp >= 0x11000 && cp <= 0x11FFF);
}
static int joining_script(uint32_t cp) { return rtl_span_script(uc_script(cp)) || complex_script(cp); }
static int blank_cell(uint32_t cp) { return !(cp & CP_CLUSTER) && cp <= ' '; }   /* graphemes store an index */

/* Row with complex left-to-right scripts: identity order, but with runs. */
static int row_complex(const Line *l, int cols) {
    int n = l && l->len < cols ? l->len : cols;
    for (int i = 0; l && i < n; i++) {
        uint32_t cp = l->c[i].cp;
        if (cp & CP_CLUSTER) { uint32_t seq[CLUSTER_LEN]; vt_cluster_cps(cp, seq, CLUSTER_LEN); cp = seq[0]; }
        if (complex_script(cp)) {
            for (int k = 0; k < cols; k++) { rvis[k] = k; rlv[k] = 0; }
            return 1;
        }
    }
    return 0;
}

/* Runs of Arabic or Hebrew cells (same font and level): they are shaped together (forms
 * init/medi/fina, lam-alef, harakat, niqqud) and then drawn as one unit. */
static void shape_spans(const TermProfile *p, const Line *l, int n) {
    nspans = 0;
    for (int i = 0; i < n; i++) span_of[i] = -1;
    uint32_t seq[CLUSTER_LEN];
    for (int col = 0; col < n && nspans < MAX_SPANS;) {
        Cell cell = l->c[col];
        if (rowt[col].n <= 0 || (cell.attr & (A_WIDE | A_WIDE_TAIL))) { col++; continue; }
        vt_cluster_cps(cell.cp, seq, CLUSTER_LEN);
        if (!joining_script(seq[0]) || (rtl_span_script(uc_script(seq[0])) && !(rlv[col] & 1))) { col++; continue; }
        int font = rowt[col].font, end = col + 1;
        for (;;) {
            /* spaces between words of the same script stay inside the run */
            int k = end;
            while (k < n && blank_cell(l->c[k].cp) && rlv[k] == rlv[col]) k++;
            if (k >= n || rowt[k].n <= 0 || rowt[k].font != font || rlv[k] != rlv[col] || (l->c[k].attr & (A_WIDE | A_WIDE_TAIL))) break;
            vt_cluster_cps(l->c[k].cp, seq, CLUSTER_LEN);
            if (!joining_script(seq[0])) break;
            end = k + 1;
        }
        uint32_t cps[512];
        int m = 0;
        for (int i = col; i < end; i++) {
            if (blank_cell(l->c[i].cp)) { if (m < 512) cps[m++] = ' '; continue; }
            int k = vt_cluster_cps(l->c[i].cp, seq, CLUSTER_LEN);
            for (int j = 0; j < k && m < 512; j++) cps[m++] = seq[j];
        }
        Otf *o = text_font_otf(font);
        int rtl = rlv[col] & 1;
        int ng = o ? shape_run(&tshaper, o, cps, m, uc_script_tag(uc_script(cps[0])), rtl) : 0;
        if (ng <= 0 || ng > 128) { col = end; continue; }
        Span *sp = &spans[nspans];
        sp->c0 = col; sp->c1 = end; sp->font = font; sp->ng = ng; sp->drawn = 0; sp->rtl = rtl;
        float sc = text_font_scale(font, p->font_size), pen = 0;
        for (int k = 0; k < ng; k++) {          /* visual order: reversed if RTL */
            int v = rtl ? ng - 1 - k : k;
            const OtfPos *ps = &tshaper.pos[v];
            sp->g[v] = tshaper.run.g[v];
            if (ps->att >= 0) continue;
            sp->gx[v] = pen + ps->dx * sc;
            sp->gy[v] = ps->dy * sc;
            pen += ps->adv * sc;
        }
        for (int pass = 0; pass < 3; pass++)
            for (int v = 0; v < ng; v++) {
                const OtfPos *ps = &tshaper.pos[v];
                if (ps->att < 0) continue;
                sp->gx[v] = sp->gx[ps->att] + ps->dx * sc;
                sp->gy[v] = sp->gy[ps->att] + ps->dy * sc;
            }
        sp->width = pen;
        for (int i = col; i < end; i++) span_of[i] = (short)nspans;
        nspans++;
        col = end;
    }
}

/* Draws the run in its cells: right-aligned if RTL, left-aligned otherwise; if it is
 * wider than the cells, it is compressed. */
static void draw_span(Canvas *c, const TermProfile *p, Span *sp, int ox, int ry, int cw, int base, uint32_t fg) {
    int vmin = 1 << 30, vmax = -1;
    for (int i = sp->c0; i < sp->c1; i++) { if (rvis[i] < vmin) vmin = rvis[i]; if (rvis[i] > vmax) vmax = rvis[i]; }
    float S = (float)(vmax - vmin + 1) * cw, W = sp->width, k = W > S && W > 0 ? S / W : 1;
    float x0 = ox + vmin * cw + (sp->rtl ? S - W * k : 0);
    for (int i = 0; i < sp->ng; i++) {
        const Glyph *g = text_glyph_id(sp->font, sp->g[i], p->font_size);
        if (g->bmp) canvas_alpha_mask(c, (int)roundf(x0 + sp->gx[i] * k) + g->xoff, ry + base - (int)roundf(sp->gy[i]) + g->yoff, g->bmp, g->w, g->h, g->w, fg);
    }
    sp->drawn = 1;
}

/* Prepares the row: chooses fonts, composes graphemes (ccmp) and applies ligatures (calt) over
 * runs of cells with the same font. `brk` = column that breaks ligatures (cursor). */
static void shape_row(const TermProfile *p, const Line *l, int cols, int brk) {
    int n = l->len < cols ? l->len : cols;
    for (int col = 0; col < n; col++) {
        CellText *ct = &rowt[col];
        Cell cell = l->c[col];
        ct->n = 0;
        if (cell.attr & A_WIDE_TAIL || cell.cp <= ' ' || cell.attr & A_HIDDEN) continue;
        uint32_t seq[CLUSTER_LEN];
        int k = vt_cluster_cps(cell.cp, seq, CLUSTER_LEN);
        if (k == 1 && is_box(seq[0])) continue;
        int emoji = wants_emoji(seq[0], seq, k, cell.attr);
        ct->font = pick_font(p, seq[0], cell.attr, emoji, &ct->fallback);
        ct->fake_bold = 0;
        if (ct->font < 0) { ct->n = -1; continue; }
        if ((cell.attr & A_BOLD) && p->use_bold && ct->font == term_profile_font(p, 0, 0, 0) && term_profile_font(p, 0, 1, 0) == ct->font)
            ct->fake_bold = 1;      /* the family has no bold: it is synthesized */
        int m = 0;
        for (int i = 0; i < k && m < CLUSTER_LEN; i++) {
            if (seq[i] == 0xfe0f || seq[i] == 0xfe0e) continue;
            uint32_t gid = text_font_glyph(ct->font, seq[i]);
            if (!gid && i > 0) {
                /* combining mark the font lacks: looked up in the fallbacks */
                continue;
            }
            ct->g[m++] = (uint16_t)gid;
        }
        ct->n = m;
        if (m > 1 && text_font_can_shape(ct->font, OTF_CCMP) && !run_set(m)) {
            for (int i = 0; i < m; i++) { run.g[i] = ct->g[i]; run.cl[i] = 0; }
            text_font_shape(ct->font, OTF_CCMP, &run);
            ct->n = run.len < CLUSTER_LEN ? run.len : CLUSTER_LEN;
            for (int i = 0; i < ct->n; i++) ct->g[i] = run.g[i];
        }
    }
    if (row_bidi) shape_spans(p, l, n);
    if (!p->ligatures) return;
    for (int col = 0; col < n;) {
        CellText *ct = &rowt[col];
        if (ct->n != 1 || ct->fallback || (l->c[col].cp & CP_CLUSTER) || col == brk || (row_bidi && span_of[col] >= 0) ||
            !text_font_can_shape(ct->font, OTF_LIGA)) { col++; continue; }
        int font = ct->font, end = col + 1;
        while (end < n && end != brk && rowt[end].n == 1 && rowt[end].font == font && !(l->c[end].cp & CP_CLUSTER) &&
               !(l->c[end].attr & (A_WIDE | A_WIDE_TAIL)))
            end++;
        if (end - col >= 2 && !run_set(end - col)) {
            for (int i = col; i < end; i++) { run.g[i - col] = rowt[i].g[0]; run.cl[i - col] = i; }
            text_font_shape(font, OTF_LIGA, &run);
            for (int i = col; i < end; i++) rowt[i].n = 0;
            for (int i = 0; i < run.len; i++) {
                int cc = run.cl[i];
                if (cc < col || cc >= end) continue;
                CellText *d = &rowt[cc];
                if (d->n < CLUSTER_LEN) d->g[d->n++] = run.g[i];
            }
        }
        col = end;
    }
}

static void draw_cell_text(Canvas *c, const TermProfile *p, const CellText *ct, uint32_t cp, uint16_t attr,
                           int x, int ry, int cw, int ch, int base, uint32_t fg, float op) {
    int width = attr & A_WIDE ? 2 : 1;
    if (ct->n == 0 && is_box(cp) && box_draw(c, cp, x, ry, cw * width, ch, fg)) return;
    if (ct->n < 0) { tofu(c, x, ry, cw * width, ch, fg); return; }
    if (ct->n == 0) return;
    float size = p->font_size;
    if (text_font_is_color(ct->font)) {
        /* emoji: the bitmap is fitted to the slot (2 cells if wide); several glyphs side by side */
        int each = cw * width / ct->n;
        for (int i = 0; i < ct->n; i++) {
            const Glyph *g = text_glyph_color(ct->font, ct->g[i], each, ch);
            if (!g->argb) continue;
            canvas_blit(c, g->argb, g->w, g->h, x + i * each + (each - g->w) / 2, ry + (ch - g->h) / 2, 0, op, 0);
        }
        return;
    }
    float base_x = 0, base_adv = 0;
    for (int i = 0; i < ct->n; i++) {
        const Glyph *g = text_glyph_id(ct->font, ct->g[i], size);
        /* combining marks: zero-advance ones (JetBrains Mono) are drawn backwards from the
         * end of the base character; those with their own width (Fira Code) from their origin */
        float pen = i == 0 ? 0 : g->adv < 0.5f ? base_x + base_adv : base_x;
        int gx = x + (int)pen + g->xoff;
        if (i == 0 && (ct->fallback || width == 2)) {
            base_x = (cw * width - g->adv) / 2;
            gx = x + (int)base_x + g->xoff;
        }
        if (g->bmp) {
            canvas_alpha_mask(c, gx, ry + base + g->yoff, g->bmp, g->w, g->h, g->w, fg);
            if (ct->fake_bold) canvas_alpha_mask(c, gx + 1, ry + base + g->yoff, g->bmp, g->w, g->h, g->w, fg);
        }
        if (i == 0) base_adv = g->adv;
    }
}

static void decorations(Canvas *c, const TermProfile *p, uint16_t attr, int x, int ry, int w, int ch, int base, uint32_t fg) {
    int th = p->font_size >= 16 ? 2 : 1;
    int uy = ry + base + (ch - base > 4 ? 2 : 1);
    if (uy + th > ry + ch) uy = ry + ch - th;
    if (attr & A_UNDER) fill(c, x, uy, x + w, uy + th, fg);
    if (attr & A_UNDER2) { fill(c, x, uy - 1, x + w, uy - 1 + th, fg); fill(c, x, uy + 1 + th, x + w, uy + 1 + 2 * th, fg); }
    if (attr & A_CURLY)
        for (int i = 0; i < w; i++) {
            int dy = (int)roundf(sinf((x + i) * 0.7f) * 1.5f);
            fill(c, x + i, uy + dy, x + i + 1, uy + dy + th, fg);
        }
    if (attr & A_STRIKE) { int sy = ry + base - (int)(p->font_size * 0.3f); fill(c, x, sy, x + w, sy + th, fg); }
    if (attr & A_OVERLINE) fill(c, x, ry, x + w, ry + th, fg);
}

static int cursor_shape(const Term *t, const TermProfile *p, int *blink) {
    int s = t->cursor_shape;
    if (s >= 1 && s <= 6) { *blink = s % 2 == 1; return s <= 2 ? CURSOR_BLOCK : s <= 4 ? CURSOR_UNDERLINE : CURSOR_BEAM; }
    *blink = p->cursor_blink || (t->modes & M_CURSOR_BLINK);
    return p->cursor_style;
}

/* Cell corners: on a scaled display the logical grid times the scale, rounded per cell, so rows and
 * columns land where the logical layout (mouse, damage) expects them. At 100 % exactly col * cw. */
#define COLX(col) (ox + (int)lroundf((col) * colw))
#define ROWY(row) (oy + (int)lroundf((row) * rowh))

void term_paint(Canvas *c, int id, float x, float y, float w, float h, float op) {
    Session *s = term_get(id);
    Rect old = canvas_push_clip(c, x, y, w, h);
    if (!s) { fill(c, (int)x, (int)y, (int)(x + w), (int)(y + h), 0xff000000u); canvas_set_clip(c, old); return; }
    Term *t = &s->t;
    TermProfile *p = &s->prof;
    float fcw, fch, fbase;
    term_metrics(p, &fcw, &fch, &fbase);
    /* rows and columns come from the logical cell (the same on every display); a scaled display
     * (paint_view.k, docs/DISPLAYS.md) draws with the profile's font and padding at its size */
    const float k = paint_view.k;
    int lcw = (int)fcw, lch = (int)fch, lpad = (int)p->padding;
    t->cell_w = fcw;
    t->cell_h = fch;
    int cols = t->cols, rows = t->rows;
    term_grid_size(w / k, h / k, lpad, lcw, lch, &cols, &rows);
    static TermProfile scaled;
    if (k != 1) {
        scaled = *p;
        scaled.font_size = p->font_size * k;
        scaled.padding = p->padding * k;
        p = &scaled;
        term_metrics(p, &fcw, &fch, &fbase);
    }
    int cw = (int)fcw, ch = (int)fch, base = (int)fbase, pad = (int)p->padding;
    const float colw = k != 1 ? lcw * k : (float)cw, rowh = k != 1 ? lch * k : (float)ch;
    if (k != 1) { cw = (int)lroundf(colw); ch = (int)ceilf(rowh); }
    if (cols != t->cols || rows != t->rows) {
        vt_resize(t, cols, rows);
        if (s->gateway) t->events |= EV_RESIZE;      /* tmux pane: the app notifies tmux */
    }
    term_size_sync(s, 0);                            /* the pty has the grid's size (a SIGWINCH if it changed) */
    /* where the terminal is, for the mouse: logical */
    s->gx = (x - paint_view.dx) / k + paint_view.x; s->gy = (y - paint_view.dy) / k + paint_view.y; s->gw = w / k; s->gh = h / k;

    uint32_t dbg = vt_resolve(t, 0, 0), dfg = vt_resolve(t, 0, 1);
    if (t->modes & M_REVERSE) { uint32_t tmp = dbg; dbg = dfg; dfg = tmp; }
    uint32_t alpha = (uint32_t)(p->bg_opacity * op * 255 + 0.5f);
    fill(c, (int)floorf(x), (int)floorf(y), (int)ceilf(x + w), (int)ceilf(y + h), alpha << 24 | (dbg & 0xffffff));

    int ox = (int)x + pad, oy = (int)y + pad;
    int blink_cursor;
    int shape = cursor_shape(t, p, &blink_cursor);
    /* Instant Replay: a saved frame is drawn instead of the live screen. */
    const Frame *rf = s->replay >= 0 && s->replay < s->nframes ? &s->frames[(s->frame_head + s->replay) % REPLAY_FRAMES] : NULL;
    int crow = rf ? rf->cy : t->cy + t->view;
    int show_cursor = !(t->modes & M_HIDECURSOR) && !t->copy_active && crow < t->rows && (t->focused ? (!blink_cursor || s->blink_on) : 1);
    uint32_t ccolor = t->cursor_set ? t->cursor_color : (p->cursor | 0xff000000u);

    for (int row = 0; row < t->rows; row++) {
        int ry = ROWY(row), ch = ROWY(row + 1) - ry;
        if (ry >= c->clip.y1 || ry + ch <= c->clip.y0) continue;
        long long abs;
        Line fake, *l;
        if (rf) {
            fake = (Line){ rf->cells + (size_t)row * rf->cols, (uint16_t)(rf->cols < t->cols ? rf->cols : t->cols), 0, 0, 0 };
            l = row < rf->rows ? &fake : NULL;
            abs = -1;
        } else l = vt_view_line(t, row, &abs);
        if (p->cursor_guide && row == crow && t->focused)
            fill(c, (int)x, ry, (int)(x + w), ry + ch, 0x30000000u | (p->guide & 0xffffff));
        row_bidi = !rf && (term_row_visual(p, t, l, t->cols, rvis, rlv) || (p->bidi && t->buf != &t->alt && row_complex(l, t->cols)));
        if (row_bidi) {                         /* background cell by cell, at its visual position */
            for (int col = 0; col < t->cols; col++) {
                Cell cell = col < l->len ? l->c[col] : (Cell){0};
                uint32_t fg, bg;
                int def;
                cell_colors(t, p, cell, abs, col, &fg, &bg, &def);
                if (!def) fill(c, COLX(rvis[col]), ry, COLX(rvis[col] + 1), ry + ch, bg | (uint32_t)(op * 255) << 24);
            }
        }
        /* background by cells (runs of the same color) */
        for (int col = 0; !row_bidi && col < t->cols;) {
            Cell cell = l && col < l->len ? l->c[col] : (Cell){0};
            uint32_t fg, bg;
            int def;
            cell_colors(t, p, cell, abs, col, &fg, &bg, &def);
            int start = col++;
            if (def) continue;
            while (col < t->cols) {
                Cell n2 = l && col < l->len ? l->c[col] : (Cell){0};
                uint32_t f2, b2;
                int d2;
                cell_colors(t, p, n2, abs, col, &f2, &b2, &d2);
                if (d2 || b2 != bg) break;
                col++;
            }
            int x1 = COLX(col);
            if (col == t->cols) x1 = (int)(x + w) - (pad > 2 ? pad - 2 : 0);
            fill(c, COLX(start), ry, x1, ry + ch, bg | (uint32_t)(op * 255) << 24);
        }
        if (!l) continue;
        /* text */
        int text_blink_off = p->blink_text && !s->blink_on;
        shape_row(p, l, t->cols, row == crow && show_cursor ? (rf ? rf->cx : t->cx) : -1);
        for (int col = 0; col < t->cols && col < l->len; col++) {
            Cell cell = l->c[col];
            if (cell.attr & A_WIDE_TAIL) continue;
            uint32_t fg, bg;
            int def;
            cell_colors(t, p, cell, abs, col, &fg, &bg, &def);
            int ccol = rf ? rf->cx : t->cx;
            if (row == crow && col == ccol && show_cursor && shape == CURSOR_BLOCK && t->focused)
                fg = p->has_cursor_text ? p->cursor_text | 0xff000000u : dbg;
            fg = (fg & 0xffffff) | (uint32_t)(op * 255) << 24;
            int vc = row_bidi ? rvis[col] : col;
            int cx0 = COLX(vc), width = cell.attr & A_WIDE ? 2 : 1;
            if (row == crow && col == ccol && show_cursor && shape == CURSOR_BLOCK && t->focused)
                fill(c, cx0, ry, COLX(vc + width), ry + ch, ccolor);
            int visible = cell.cp > ' ' && !(cell.attr & A_HIDDEN) && !((cell.attr & A_BLINK) && text_blink_off);
            if (row_bidi && span_of[col] >= 0) {
                if (visible && !spans[span_of[col]].drawn) draw_span(c, p, &spans[span_of[col]], ox, ry, cw, base, fg);
            } else if (visible)
                draw_cell_text(c, p, &rowt[col], cell.cp, cell.attr, cx0, ry, cw, ch, base, fg, op);
            if (cell.attr & (A_UNDER | A_UNDER2 | A_CURLY | A_STRIKE | A_OVERLINE))
                decorations(c, p, cell.attr, cx0, ry, COLX(vc + width) - cx0, ch, base, fg);
        }
        /* shell marks in the left margin */
        if (l->flags & (LINE_PROMPT | LINE_MARK) && pad >= 4) {
            uint32_t mc = l->flags & LINE_FAIL ? 0xffff453a : l->flags & LINE_MARK && !(l->flags & LINE_PROMPT) ? 0xff8e8e93 : 0xff0a84ff;
            int mh = ch / 2, my = ry + (ch - mh) / 2;
            for (int k = 0; k < mh; k++) {
                int d = k < mh / 2 ? k : mh - 1 - k;
                fill(c, (int)x + 1, my + k, (int)x + 2 + d * (pad - 3) / (mh / 2 + 1), my + k + 1, mc);
            }
        }
    }

    /* unfilled cursor: underline, bar or hollow box (unfocused) */
    if (show_cursor && crow < t->rows) {
        int ccx = rf ? rf->cx : t->cx;
        Line *l = rf ? NULL : vt_view_line(t, crow, NULL);
        int vcx = ccx;
        if (l && ccx < t->cols && term_row_visual(p, t, l, t->cols, rvis, rlv)) vcx = rvis[ccx];
        int cx0 = COLX(vcx), ry = ROWY(crow);
        int width = l && ccx < l->len && (l->c[ccx].attr & A_WIDE) ? 2 : 1;
        if (!t->focused && shape == CURSOR_BLOCK) {
            fill(c, cx0, ry, COLX(vcx + width), ry + 1, ccolor);
            fill(c, cx0, ry + ch - 1, COLX(vcx + width), ry + ch, ccolor);
            fill(c, cx0, ry, cx0 + 1, ry + ch, ccolor);
            fill(c, COLX(vcx + width) - 1, ry, COLX(vcx + width), ry + ch, ccolor);
        } else if (shape == CURSOR_UNDERLINE) fill(c, cx0, ry + ch - 2, COLX(vcx + width), ry + ch, ccolor);
        else if (shape == CURSOR_BEAM) fill(c, cx0, ry, cx0 + 2, ry + ch, ccolor);
        else if (!l || ccx >= l->len) {
            /* block over an empty cell (the text did not draw it) */
            if (t->focused) fill(c, cx0, ry, COLX(vcx + 1), ry + ch, ccolor);
        }
    }

    if (!rf) draw_images(c, t, ox, oy, cw, ch, op);

    /* copy mode cursor */
    if (t->copy_active) {
        long long top;
        vt_view_line(t, 0, &top);
        long long r = t->copy_line - top;
        if (r >= 0 && r < t->rows) {
            int cx0 = COLX(t->copy_col), ry = ROWY((int)r);
            fill(c, cx0, ry, COLX(t->copy_col + 1), ry + ch, 0x80ff9f0a);
            fill(c, cx0, ry + ch - 2, COLX(t->copy_col + 1), ry + ch, 0xffff9f0a);
        }
    }

    /* timestamps on the right (View → Show timestamps) */
    if (p->timestamps && !rf) {
        for (int row = 0; row < t->rows; row++) {
            Line *l = vt_view_line(t, row, NULL);
            if (!l || !l->time) continue;
            time_t tt = l->time;
            struct tm tm;
            localtime_r(&tt, &tm);
            char buf[16];
            strftime(buf, sizeof buf, "%H:%M:%S", &tm);
            float tw, th;
            text_measure(buf, 400, 10, 0, 0, 1, &tw, &th);
            int ry = ROWY(row), tx = (int)(x + w) - pad - (int)tw - 6;
            fill(c, tx - 4, ry, (int)(x + w) - pad, ry + ch, 0xc0000000u | (dbg & 0xffffff));
            text_draw(c, buf, 400, 10, 0, (float)tx, (float)ry + (ch - th) / 2, 0, TEXT_LEFT, 1, 0xa0000000u | (dfg & 0xffffff));
        }
    }

    /* scroll bar while looking at the history: an overlay like ScrollView's (docs/DESIGN.md, "Scroll
     * bars"), 6 px wide and 2 px from the edge, only while scrolled back */
    int total = t->buf->count;
    if (t->view > 0 && total > t->rows) {
        float track = h - 4, bar = fmaxf(24, track * t->rows / total);
        float pos = (float)(total - t->rows - t->view) / (total - t->rows);
        int by = (int)(y + 2 + (track - bar) * pos);
        fill(c, (int)(x + w) - 8, by, (int)(x + w) - 2, by + (int)bar, 0x80000000u | ((lum(dbg) < 0.25f ? 0xffffff : 0x000000)));
    }

    s->drawn_cx = t->cx;
    s->drawn_cy = t->cy;
    canvas_set_clip(c, old);
}
#undef COLX
#undef ROWY
