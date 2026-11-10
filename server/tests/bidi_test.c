/* bidi_test.c — runs the Unicode conformance tests against bidi.c and grapheme.c, and checks where
 * caret.c puts the characters of mixed-direction lines (the caret and selection of a text field).
 *
 *   build/bidi-test [BidiCharacterTest.txt] [GraphemeBreakTest.txt] [-v]
 *
 * (https://www.unicode.org/Public/<version>/ucd/BidiCharacterTest.txt and auxiliary/GraphemeBreakTest.txt).
 * Each BidiCharacterTest line holds the characters, the paragraph direction, the resolved level, the
 * per-character levels after L1 ("x" = removed by X9) and the visual order; each GraphemeBreakTest line,
 * code points with "÷" (a break) or "×" (none) between them. Without files only the caret cases run. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ui/bidi.h"
#include "../src/ui/caret.h"
#include "../src/ui/grapheme.h"

#define MAXN 512

static int verbose;

/* ---- GraphemeBreakTest.txt ---- */
static int grapheme_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 2; }
    static char line[4096];
    long total = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        uint32_t cp[MAXN];
        uint8_t want[MAXN + 1], got[MAXN + 1];
        int n = 0;
        for (char *p = line; *p && n < MAXN;) {
            if ((unsigned char)p[0] == 0xC3 && ((unsigned char)p[1] == 0xB7 || (unsigned char)p[1] == 0x97)) {
                want[n] = (unsigned char)p[1] == 0xB7;          /* ÷ U+00F7 or × U+00D7 */
                p += 2;
                continue;
            }
            char *e;
            unsigned long v = strtoul(p, &e, 16);
            if (e == p) { p++; continue; }
            cp[n++] = (uint32_t)v;
            p = e;
        }
        if (!n) continue;
        grapheme_breaks(cp, n, got);
        int ok = 1;
        for (int k = 0; k <= n; k++) if (!!got[k] != !!want[k]) ok = 0;
        total++;
        if (!ok) {
            bad++;
            if (verbose && bad <= 20) {
                printf("FAIL:");
                for (int k = 0; k < n; k++) printf(" %s %04X", got[k] ? "\xc3\xb7" : "\xc3\x97", cp[k]);
                printf(" \xc3\xb7\n");
            }
        }
    }
    fclose(f);
    printf("GraphemeBreakTest: %ld cases, %ld pass, %ld fail\n", total, total - bad, bad);
    return bad ? 1 : 0;
}

/* ---- caret.c: each code point's edges, with glyphs 10 px wide ---- */
static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { uint32_t cp[32]; int n; uint8_t lv[32]; float x0[32], x1[32]; int rtl; } Line;

/* one glyph per code point (marks, joiners and variation selectors drawn on the one before), in the
 * visual order bidi.c gives the paragraph */
static void lay(Line *L, const uint32_t *cp, int n, int para) {
    memcpy(L->cp, cp, sizeof *cp * (size_t)n);
    L->n = n;
    L->rtl = bidi_levels(cp, n, para, L->lv, NULL);
    int order[32];
    bidi_line(cp, L->lv, 0, n, L->rtl, order);
    CaretGlyph g[32];
    for (int k = 0; k < n; k++) {
        uint32_t c = cp[order[k]];
        int on = (c >= 0x300 && c <= 0x36F) || (c >= 0x591 && c <= 0x5C7) || c == 0x200D || c == 0xFE0F || (c >= 0x1F3FB && c <= 0x1F3FF);
        g[k] = (CaretGlyph){ on ? -1 : order[k], on ? 0 : 10 };
    }
    caret_edges(cp, n, L->lv, g, n, L->x0, L->x1);
}
static int at(const Line *L, int i, float a, float b) { return fabsf(L->x0[i] - a) < 0.01f && fabsf(L->x1[i] - b) < 0.01f; }

static void caret_cases(void) {
    Line L;
    /* Hebrew with a number in a right-to-left field: "שלום 123" draws "123 םולש" */
    static const uint32_t heb[] = { 0x05E9, 0x05DC, 0x05D5, 0x05DD, ' ', '1', '2', '3' };
    lay(&L, heb, 8, -1);
    CHECK(L.rtl == 1, "Hebrew: paragraph level %d", L.rtl);
    CHECK(at(&L, 5, 0, 10) && at(&L, 6, 10, 20) && at(&L, 7, 20, 30), "Hebrew: 123 not on the left, left to right");
    CHECK(at(&L, 4, 30, 40) && at(&L, 3, 40, 50) && at(&L, 0, 70, 80), "Hebrew: the letters not right to left (shin %.0f..%.0f)", L.x0[0], L.x1[0]);
    CaretRun r[8];
    int nr = caret_runs(L.lv, L.n, L.x0, L.x1, r, 8);
    CHECK(nr == 2 && r[0].start == 5 && r[0].end == 8 && !r[0].rtl && r[0].x == 0 && r[0].w == 30
          && r[1].start == 0 && r[1].end == 5 && r[1].rtl && r[1].x == 30 && r[1].w == 50, "Hebrew: %d runs", nr);
    /* Latin with Hebrew: "abc אבג" draws "abc גבא" */
    static const uint32_t mix[] = { 'a', 'b', 'c', ' ', 0x05D0, 0x05D1, 0x05D2 };
    lay(&L, mix, 7, -1);
    CHECK(L.rtl == 0 && at(&L, 0, 0, 10) && at(&L, 3, 30, 40), "mixed: the Latin moved");
    CHECK(at(&L, 4, 60, 70) && at(&L, 6, 40, 50), "mixed: alef at %.0f, gimel at %.0f", L.x0[4], L.x0[6]);
    nr = caret_runs(L.lv, L.n, L.x0, L.x1, r, 8);
    CHECK(nr == 2 && !r[0].rtl && r[1].rtl && r[1].start == 4 && r[1].x == 40, "mixed: %d runs", nr);
    /* an Arabic number inside Arabic: "عدد ١٢" keeps ١٢ left to right on the left */
    static const uint32_t ar[] = { 0x0639, 0x062F, 0x062F, ' ', 0x0661, 0x0662 };
    lay(&L, ar, 6, -1);
    CHECK(at(&L, 4, 0, 10) && at(&L, 5, 10, 20) && at(&L, 0, 50, 60), "Arabic digits: %.0f %.0f", L.x0[4], L.x0[5]);

    /* a ZWJ family is one glyph: all seven code points share its edges */
    static const uint32_t fam[] = { 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466, 'x' };
    CaretGlyph fg[] = { { 0, 20 }, { 7, 10 } };
    float x0[16], x1[16];
    uint8_t lv0[16] = { 0 };
    caret_edges(fam, 8, lv0, fg, 2, x0, x1);
    int same = 1;
    for (int i = 0; i < 7; i++) if (x0[i] != 0 || x1[i] != 20) same = 0;
    CHECK(same && x0[7] == 20 && x1[7] == 30, "ZWJ family split: %.0f..%.0f", x0[6], x1[6]);
    /* the same family drawn as four glyphs (no emoji font): still one cluster */
    CaretGlyph fg4[] = { { 0, 10 }, { 2, 10 }, { 4, 10 }, { 6, 10 } };
    caret_edges(fam, 7, lv0, fg4, 4, x0, x1);
    CHECK(x0[0] == 0 && x1[0] == 40 && x0[4] == 0 && x1[6] == 40, "family as four glyphs: %.0f..%.0f", x0[0], x1[0]);
    /* two flags, each two regional indicators drawn as two glyphs */
    static const uint32_t flags[] = { 0x1F1E6, 0x1F1F7, 0x1F1E7, 0x1F1F7 };
    CaretGlyph flg[] = { { 0, 10 }, { 1, 10 }, { 2, 10 }, { 3, 10 } };
    caret_edges(flags, 4, lv0, flg, 4, x0, x1);
    CHECK(x0[1] == 0 && x1[0] == 20 && x0[2] == 20 && x1[3] == 40, "flags: %.0f..%.0f %.0f..%.0f", x0[0], x1[1], x0[2], x1[3]);
    /* a letter with a combining mark of its own glyph: one cluster */
    static const uint32_t emark[] = { 'e', 0x0301, 'f' };
    CaretGlyph eg[] = { { 0, 10 }, { 1, 4 }, { 2, 10 } };
    caret_edges(emark, 3, lv0, eg, 3, x0, x1);
    CHECK(x0[0] == 0 && x1[0] == 14 && x0[1] == 0 && x1[1] == 14 && x0[2] == 14, "e + mark: %.0f..%.0f", x0[0], x1[0]);
    /* lam-alef: one glyph for two graphemes, split with the lam on the right (right to left) */
    static const uint32_t la[] = { 0x0644, 0x0627 };
    uint8_t lv1[2] = { 1, 1 };
    CaretGlyph lg[] = { { 0, 20 } };
    caret_edges(la, 2, lv1, lg, 1, x0, x1);
    CHECK(x0[0] == 10 && x1[0] == 20 && x0[1] == 0 && x1[1] == 10, "lam-alef: lam %.0f..%.0f alef %.0f..%.0f", x0[0], x1[0], x0[1], x1[1]);
    /* "fi" as one glyph, left to right: f on the left */
    static const uint32_t fi[] = { 'f', 'i' };
    CaretGlyph fig[] = { { 0, 12 } };
    caret_edges(fi, 2, lv0, fig, 1, x0, x1);
    CHECK(x0[0] == 0 && x1[0] == 6 && x0[1] == 6 && x1[1] == 12, "fi: %.0f..%.0f", x0[0], x1[0]);
    /* combining marks drawn on their letter (Hebrew niqqud): the letter's edges */
    static const uint32_t niq[] = { 0x05E9, 0x05B8, 0x05DC };
    lay(&L, niq, 3, -1);
    CHECK(at(&L, 0, 10, 20) && at(&L, 1, 10, 20) && at(&L, 2, 0, 10), "niqqud: %.0f..%.0f", L.x0[1], L.x1[1]);
    /* grapheme boundaries the arrows stop at */
    uint8_t b[16];
    static const uint32_t skin[] = { 'a', 0x1F44D, 0x1F3FD, 0x2764, 0xFE0F, 'e', 0x0301 };
    int nc = grapheme_breaks(skin, 7, b);
    CHECK(nc == 4 && b[0] && b[1] && !b[2] && b[3] && !b[4] && b[5] && !b[6] && b[7], "skin tone, heart, e + accent: %d clusters", nc);
    static const uint32_t dev[] = { 0x0915, 0x094D, 0x0937, 0x093F };   /* a Devanagari conjunct with its vowel sign (GB9c) */
    CHECK(grapheme_breaks(dev, 4, b) == 1, "Devanagari conjunct split");
    static const uint32_t three[] = { 0x1F1E6, 0x1F1F7, 0x1F1E7 };      /* a flag and a lone indicator */
    CHECK(grapheme_breaks(three, 3, b) == 2 && b[2], "three regional indicators");
    printf("caret: %d checks, %d failures\n", checks, fails);
}

/* ---- BidiCharacterTest.txt ---- */
static int bidi_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 2; }
    static char line[16384];
    long total = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *fld[5];
        char *s = line;
        int nf = 0;
        for (; nf < 5; nf++) { fld[nf] = s; char *sc = strchr(s, ';'); if (!sc) { nf++; break; } *sc = 0; s = sc + 1; }
        if (nf < 5) continue;
        uint32_t cp[MAXN];
        int n = 0;
        for (char *p = fld[0]; *p && n < MAXN;) {
            char *e;
            unsigned long v = strtoul(p, &e, 16);
            if (e == p) break;
            cp[n++] = (uint32_t)v;
            p = e;
        }
        int dir = atoi(fld[1]), want_para = atoi(fld[2]);
        int want_lv[MAXN], nl = 0;
        for (char *p = strtok(fld[3], " \n"); p && nl < MAXN; p = strtok(NULL, " \n")) want_lv[nl++] = *p == 'x' ? -1 : atoi(p);
        int want_ord[MAXN], no = 0;
        for (char *p = strtok(fld[4], " \n"); p && no < MAXN; p = strtok(NULL, " \n")) want_ord[no++] = atoi(p);

        uint8_t lv[MAXN], rm[MAXN];
        int para = bidi_levels(cp, n, dir == 2 ? -1 : dir, lv, rm);
        int order[MAXN];
        bidi_line(cp, lv, 0, n, para, order);
        int ok = para == want_para && nl == n;
        for (int i = 0; ok && i < n; i++) {
            if (want_lv[i] < 0) { if (!rm[i]) ok = 0; }
            else if (rm[i] || lv[i] != want_lv[i]) ok = 0;
        }
        int k = 0;
        for (int i = 0; ok && i < n; i++) {
            if (rm[order[i]]) continue;
            if (k >= no || order[i] != want_ord[k]) ok = 0;
            k++;
        }
        if (ok && k != no) ok = 0;
        total++;
        if (!ok) {
            bad++;
            if (verbose && bad <= 20) {
                printf("FAIL: %s; dir %d; para %d (want %d)\n  levels:", fld[0], dir, para, want_para);
                for (int i = 0; i < n; i++) printf(" %s%d", rm[i] ? "x" : "", lv[i]);
                printf("\n  quiero: ");
                for (int i = 0; i < nl; i++) { if (want_lv[i] < 0) printf(" x"); else printf(" %d", want_lv[i]); }
                printf("\n");
            }
        }
    }
    fclose(f);
    printf("BidiCharacterTest: %ld cases, %ld pass, %ld fail\n", total, total - bad, bad);
    return bad ? 1 : 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-v")) verbose = 1;
    caret_cases();
    int r = fails ? 1 : 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) continue;
        int k = strstr(argv[i], "Grapheme") ? grapheme_file(argv[i]) : bidi_file(argv[i]);
        if (k > r) r = k;
    }
    return r;
}
