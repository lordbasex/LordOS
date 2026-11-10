/* caret.c — the edges of each character of a shaped line, from its glyphs in visual order
 * (textlayout.c after shaping and the bidi reordering, or text.c's Latin path). A text field
 * draws its caret and its selection rectangles from them (lord.text.carets, js_sys.c;
 * sdk/textfieldmodel.js), so they follow the text as it is drawn and not as if it ran left
 * to right. Tested by tests/bidi_test.c. */
#include "caret.h"

#include <stdlib.h>

#include "grapheme.h"

int caret_edges(const uint32_t *cp, int n, const uint8_t *lv, const CaretGlyph *g, int ng, float *x0, float *x1) {
    if (n <= 0) return 0;
    float *ox0 = malloc(sizeof *ox0 * (size_t)n), *ox1 = malloc(sizeof *ox1 * (size_t)n);
    int *own = malloc(sizeof *own * (size_t)n);
    uint8_t *brk = malloc((size_t)n + 1);
    if (!ox0 || !ox1 || !own || !brk) { free(ox0); free(ox1); free(own); free(brk); return -1; }
    /* 1. the interval each owning code point's glyphs cover */
    for (int i = 0; i < n; i++) { ox0[i] = 1e30f; ox1[i] = -1e30f; }
    float pen = 0;
    for (int k = 0; k < ng; k++) {
        int c = g[k].cp;
        float a = pen;
        pen += g[k].adv;
        if (c < 0 || c >= n) continue;
        if (a < ox0[c]) ox0[c] = a;
        if (pen > ox1[c]) ox1[c] = pen;
    }
    /* the code point whose glyphs draw i: i itself, or the nearest owner before it in the same run
     * (a character the shaper merged into its cluster), or the one after it, or either */
    for (int i = 0, last = -1; i < n; i++) { if (ox1[i] >= ox0[i]) last = i; own[i] = last; }
    for (int i = n - 1, next = -1; i >= 0; i--) {
        if (ox1[i] >= ox0[i]) next = i;
        int p = own[i];
        if (p == i) continue;
        own[i] = p >= 0 && lv[p] == lv[i] ? p : next >= 0 && lv[next] == lv[i] ? next : p >= 0 ? p : next;
    }
    grapheme_breaks(cp, n, brk);
    /* 2. a glyph cluster (code points with the same owner) split evenly between the graphemes in it */
    for (int s = 0; s < n;) {
        int e = s + 1;
        while (e < n && own[e] == own[s]) e++;
        int j = own[s];
        if (j < 0) { for (int i = s; i < e; i++) x0[i] = x1[i] = pen; s = e; continue; }
        int m = 1;
        for (int k = s + 1; k < e; k++) m += brk[k];
        float w = (ox1[j] - ox0[j]) / m;
        for (int i = s, p = 0; i < e; i++) {
            if (i > s) p += brk[i];
            if (lv[j] & 1) { x1[i] = ox1[j] - p * w; x0[i] = x1[i] - w; }
            else { x0[i] = ox0[j] + p * w; x1[i] = x0[i] + w; }
        }
        s = e;
    }
    /* 3. a grapheme drawn by several glyph clusters (a letter and a spacing mark, a flag without a
     * flag font) takes all of them */
    for (int s = 0; s < n;) {
        int e = s + 1;
        while (e < n && !brk[e]) e++;
        float a = x0[s], b = x1[s];
        for (int i = s + 1; i < e; i++) { if (x0[i] < a) a = x0[i]; if (x1[i] > b) b = x1[i]; }
        for (int i = s; i < e; i++) { x0[i] = a; x1[i] = b; }
        s = e;
    }
    free(ox0); free(ox1); free(own); free(brk);
    return 0;
}

int caret_runs(const uint8_t *lv, int n, const float *x0, const float *x1, CaretRun *out, int max) {
    int nr = 0;
    for (int s = 0; s < n && nr < max;) {
        int e = s + 1;
        float a = x0[s], b = x1[s];
        while (e < n && lv[e] == lv[s]) { if (x0[e] < a) a = x0[e]; if (x1[e] > b) b = x1[e]; e++; }
        CaretRun r = { s, e, lv[s] & 1, a, b - a };
        int k = nr++;
        while (k > 0 && out[k - 1].x > r.x) { out[k] = out[k - 1]; k--; }
        out[k] = r;
        s = e;
    }
    return nr;
}
