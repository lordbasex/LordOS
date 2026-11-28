/* svgrast_test.c — SVG icons in lord-media (src/media/helper/lm_svg.c), without the helper around them: a valid
 * icon, a symbolic one recolored, the aspect kept, malformed markup, huge numbers, deep nesting, too many elements
 * and a file over 1 MB, and markup built so that a scan of its own would count differently from nanosvg's
 * tokenizer (quotes, comments, CDATA, "<?" and "<!"). Hostile cases must come back quickly with an error code (or a picture), never crash.
 * build/media-test svg runs the same kind of files through lord-media and its sandbox (test-full). */
#include "../src/media/helper/lm_svg.h"
#include "nanosvg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); putchar('\n'); } } while (0)

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* the shape of the theme's audio-volume-high-symbolic: a 16 px speaker in one dark color */
static const char *speaker =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<svg height=\"16px\" viewBox=\"0 0 16 16\" width=\"16px\" xmlns=\"http://www.w3.org/2000/svg\">\n"
    "  <!-- a comment with a <tag> inside -->\n"
    "  <path d=\"m 7 1 l -3.2 3.6 h -1 c -1.1 0 -2 0.8 -2 2 v 2 c 0 1.1 0.9 2 2 2 h 1 l 3.2 3.6 z\" fill=\"#2e3436\"/>\n"
    "  <rect x=\"10\" y=\"3\" width=\"2\" height=\"10\" fill=\"#2e3436\" data-x=\"a>b\"/>\n"
    "</svg>\n";

static const char *raster(const char *svg, size_t len, int side, long color, uint8_t **px, int *w, int *h, double *ms) {
    double t0 = now_ms();
    const char *e = lm_svg_raster(svg, len, side, color, px, w, h);
    *ms = now_ms() - t0;
    return e;
}

/* a string of n copies of s between head and tail (malloc) */
static char *repeat(const char *head, const char *s, size_t n, const char *tail, size_t *len) {
    size_t hl = strlen(head), sl = strlen(s), tl = strlen(tail);
    char *b = malloc(hl + sl * n + tl + 1);
    memcpy(b, head, hl);
    for (size_t i = 0; i < n; i++) memcpy(b + hl + i * sl, s, sl);
    memcpy(b + hl + sl * n, tail, tl + 1);
    *len = hl + sl * n + tl;
    return b;
}

static void test_scan(void) {
    int n = 0, d = 0;
    /* nanosvg ends the <rect> at the '>' inside data-x="a>b", before its "/>": for nanosvg it stays open */
    CHECK(lm_svg_scan(speaker, strlen(speaker), &n, &d) == 0 && n == 4 && d == 2, "speaker: %d elements, depth %d", n, d);
    const char *nest = "<svg><g><g><rect/></g></g></svg>";
    CHECK(lm_svg_scan(nest, strlen(nest), &n, &d) == 0 && d == 3, "nested: depth %d", d);
    /* a tag the markup never ends: nanosvg never parses it, so it isn't counted */
    const char *cut = "<svg><path d=\"M0 0";
    CHECK(lm_svg_scan(cut, strlen(cut), &n, &d) == 0 && n == 1 && d == 1, "a tag that never ends: %d, depth %d", n, d);
    const char *cdata = "<svg><![CDATA[ <<<< ]]><rect/></svg>";
    CHECK(lm_svg_scan(cdata, strlen(cdata), &n, &d) == 0 && n == 3 && d == 1, "CDATA skipped: %d elements", n);
    /* self-closing tags open no level, with attributes, without, or after a space */
    const char *self = "<svg><g><rect/><rect x=\"1\"/><rect /><g/></g></svg>";
    CHECK(lm_svg_scan(self, strlen(self), &n, &d) == 0 && n == 8 && d == 2, "self-closing: %d elements, depth %d", n, d);
}

static void test_valid(void) {
    uint8_t *px;
    int w, h;
    double ms;
    const char *e = raster(speaker, strlen(speaker), 32, -1, &px, &w, &h, &ms);
    CHECK(!e && w == 32 && h == 32, "valid icon: %s %dx%d", e ? e : "ok", w, h);
    if (e) return;
    int painted = 0, dark = 0;
    for (int i = 0; i < w * h; i++) {
        uint8_t *p = px + i * 4;
        if (p[3] == 255) { painted++; if (p[0] == 0x36 && p[1] == 0x34 && p[2] == 0x2e) dark++; }
        CHECK(p[0] <= p[3] && p[1] <= p[3] && p[2] <= p[3], "premultiplied pixel %d", i);
    }
    CHECK(painted > 100 && dark == painted, "its own color: %d painted, %d #2e3436", painted, dark);
    /* the speaker's body is on the left: a pixel there is painted, the top right corner isn't */
    CHECK(px[(16 * w + 6) * 4 + 3] == 255 && px[(1 * w + 30) * 4 + 3] == 0, "where it's drawn");
    free(px);
    /* the aspect: 32 x 16 drawn into 64 */
    const char *wide = "<svg width=\"32\" height=\"16\"><rect width=\"32\" height=\"16\" fill=\"red\"/></svg>";
    e = raster(wide, strlen(wide), 64, -1, &px, &w, &h, &ms);
    CHECK(!e && w == 64 && h == 32, "aspect kept: %s %dx%d", e ? e : "ok", w, h);
    free(px);
}

static void test_symbolic(void) {
    uint8_t *px;
    int w, h;
    double ms;
    const char *e = raster(speaker, strlen(speaker), 24, 0xff8000, &px, &w, &h, &ms);
    CHECK(!e && w == 24, "symbolic: %s", e ? e : "ok");
    if (e) return;
    int painted = 0, wrong = 0;
    for (int i = 0; i < w * h; i++) {
        uint8_t *p = px + i * 4;
        if (p[3] < 64) continue;                /* at the faint edges premultiplying rounds too much to compare */
        painted++;
        /* B, G, R premultiplied: unpremultiplied it is ff8000 (rounding allowed at the edges) */
        double a = p[3] / 255.0;
        if (fabs(p[2] / a - 255) > 3 || fabs(p[1] / a - 128) > 3 || p[0] > 1) wrong++;
    }
    CHECK(painted > 50 && !wrong, "recolored to ff8000: %d painted, %d other", painted, wrong);
    free(px);
}

static void expect_err(const char *name, const char *svg, size_t len, const char *want) {
    uint8_t *px = NULL;
    int w = 0, h = 0;
    double ms;
    const char *e = raster(svg, len, 64, -1, &px, &w, &h, &ms);
    CHECK(e && (!want || !strcmp(e, want)), "%s: %s (wanted %s)", name, e ? e : "a picture", want ? want : "an error");
    CHECK(ms < 2000, "%s took %.0f ms", name, ms);
    CHECK(!px, "%s: no pixels with an error", name);
    free(px);
}

/* a hostile file that may draw or fail, but quickly and without crashing */
static void expect_survives(const char *name, const char *svg) {
    uint8_t *px = NULL;
    int w = 0, h = 0;
    double ms;
    const char *e = raster(svg, strlen(svg), 64, -1, &px, &w, &h, &ms);
    CHECK(ms < 2000, "%s took %.0f ms", name, ms);
    CHECK(e || (px && w >= 1 && h >= 1 && w <= 64 && h <= 64), "%s: %s %dx%d", name, e ? e : "ok", w, h);
    printf("  %-28s %s in %.1f ms\n", name, e ? e : "drawn", ms);
    free(px);
}

/* Markup built to fool a separate pre-scan that reads quotes, comments, CDATA, "<?" or "<!" the way XML does:
 * nanosvg ends every tag at its first '>', so each of these hides 100 000 nested <g> from such a scan while
 * nanosvg parses them all. Counted with nanosvg's own tokenizer they're seen, and refused. */
static void test_differential(void) {
    static const char *const heads[][2] = {
        { "a '>' inside a quote", "<svg width=\"16\" height=\"16\"><desc x=\"" },
        { "a '<' inside a quote", "<svg width=\"16\" height=\"16\"><rect x='<" },
        { "a comment", "<svg width=\"16\" height=\"16\"><!-- > " },
        { "CDATA", "<svg width=\"16\" height=\"16\"><![CDATA[ > " },
        { "a processing instruction", "<svg width=\"16\" height=\"16\"><?pi > " },
        { "a declaration with a quote", "<svg width=\"16\" height=\"16\"><!DOCTYPE svg \"> " },
        { "a closing declaration", "<svg width=\"16\" height=\"16\"></!x \"> " },
    };
    static const char *const tails[] = { "\"/>", "'/>", " -->", " ]]>", " ?>", "\">", "\">" };
    for (size_t i = 0; i < sizeof heads / sizeof *heads; i++) {
        char tail[160];
        snprintf(tail, sizeof tail, "%s<rect width=\"16\" height=\"16\"/></svg>", tails[i]);
        size_t len;
        char *svg = repeat(heads[i][1], "<g>", 100000, tail, &len);
        int n = 0, d = 0;
        CHECK(lm_svg_scan(svg, len, &n, &d) == -1 && n > 99000 && d > 99000, "%s: %d elements, depth %d", heads[i][0], n, d);
        expect_err(heads[i][0], svg, len, "media.svgTooComplex");
        free(svg);
        /* the same trick with flat elements: too many of them */
        svg = repeat(heads[i][1], "<rect width=\"1\" height=\"1\"/>", 30000, tail, &len);
        CHECK(lm_svg_scan(svg, len, &n, &d) == -1 && n > LM_SVG_MAX_ELEMENTS, "%s, flat: %d elements", heads[i][0], n);
        expect_err(heads[i][0], svg, len, "media.svgTooComplex");
        free(svg);
    }
    /* a quoted '>' and a comment's '>' end the tag for nanosvg: what follows is markup and counted */
    const char *q = "<svg><rect data-x=\"a><g><g>\"/><!-- > <g> --></svg>";
    int n = 0, d = 0;
    CHECK(lm_svg_scan(q, strlen(q), &n, &d) == 0 && n == 6 && d == 5, "a quoted '>': %d elements, depth %d", n, d);
}

static void test_malformed(void) {
    expect_err("empty", "", 0, "media.notAnImage");
    expect_err("not markup", "hello", 5, "media.notAnImage");
    const char *cut = "<svg width=\"16\" height=\"16\"><path d=\"M 0 0 L 16";
    expect_err("cut inside a tag", cut, strlen(cut), "media.notAnImage");
    const char *nul = "<svg>\0<rect/></svg>";
    expect_err("a NUL byte", nul, 19, "media.notAnImage");
    const char *noshape = "<svg width=\"16\" height=\"16\"></svg>";
    expect_err("nothing drawn", noshape, strlen(noshape), "media.notAnImage");
    uint8_t *px;
    int w, h;
    CHECK(lm_svg_raster(speaker, strlen(speaker), 8, -1, &px, &w, &h) != NULL, "a side under 16");
    CHECK(lm_svg_raster(speaker, strlen(speaker), 512, -1, &px, &w, &h) != NULL, "a side over 256");
    CHECK(lm_svg_raster(speaker, strlen(speaker), 32, 0x1000000, &px, &w, &h) != NULL, "a color over 0xffffff");
}

static void test_numbers(void) {
    const char *far = "<svg width=\"16\" height=\"16\"><path d=\"M 1e30 1e30 L -1e30 0 L 0 1e30 Z\"/></svg>";
    expect_err("points far away", far, strlen(far), "media.svgTooComplex");
    const char *tiny = "<svg width=\"1e-30\" height=\"1e-30\"><rect width=\"1\" height=\"1\"/></svg>";
    expect_err("a tiny width", tiny, strlen(tiny), NULL);
    expect_survives("infinite numbers", "<svg width=\"16\" height=\"16\"><path d=\"M 1e999 0 L 0 1e999 L 5 5 Z\"/></svg>");
    expect_survives("a huge stroke", "<svg width=\"16\" height=\"16\"><path d=\"M 0 0 L 16 16\" stroke=\"#000\" "
                                     "stroke-width=\"1e30\" stroke-linejoin=\"round\" stroke-linecap=\"round\"/></svg>");
    expect_survives("tiny dashes", "<svg width=\"16\" height=\"16\"><path d=\"M 0 0 L 16 16 L 0 16 Z\" stroke=\"#000\" "
                                   "stroke-dasharray=\"1e-9 1e-9\"/></svg>");
    expect_survives("a gradient gone wild",
                    "<svg width=\"16\" height=\"16\"><defs><linearGradient id=\"g\" x1=\"1e38\" x2=\"-1e38\" "
                    "gradientTransform=\"matrix(1e38 0 0 1e38 0 0)\"><stop offset=\"nan\" stop-color=\"#f00\"/>"
                    "<stop offset=\"1e999\" stop-color=\"#00f\"/></linearGradient></defs>"
                    "<rect width=\"16\" height=\"16\" fill=\"url(#g)\"/></svg>");
    expect_survives("a huge transform", "<svg width=\"16\" height=\"16\"><g transform=\"scale(1e20)\">"
                                        "<rect width=\"1\" height=\"1\"/></g></svg>");
}

static void test_sizes(void) {
    size_t len;
    char *deep = repeat("<svg width=\"16\" height=\"16\">", "<g>", 100000, "<rect width=\"16\" height=\"16\"/></svg>", &len);
    expect_err("deep nesting", deep, len, "media.svgTooComplex");
    free(deep);
    char *many = repeat("<svg width=\"16\" height=\"16\">", "<rect width=\"1\" height=\"1\"/>", 30000, "</svg>", &len);
    expect_err("too many elements", many, len, "media.svgTooComplex");
    free(many);
    char *points = repeat("<svg width=\"16\" height=\"16\"><path d=\"M 0 0", "L1 1", 70000, " Z\"/></svg>", &len);
    expect_err("too many points", points, len, "media.svgTooComplex");
    free(points);
    char *big = repeat("<svg width=\"16\" height=\"16\"><!--", "xxxxxxxxxxxxxxxx", 70000, "--><rect width=\"16\" height=\"16\"/></svg>", &len);
    expect_err("over 1 MB", big, len, "media.svgTooLarge");
    free(big);
}

/* A linear gradient on memory that held other data: nanosvg sets a gradient's focus (fx, fy) only for a radial one
 * and leaves it as malloc gave it for a linear one, which the rasterizer never reads. In lord-media's long-lived
 * worker that memory is a previous icon's, and a check of fx/fy refused real icons (GNOME Text Editor's, Print
 * Editor's) as too complex at some sizes and not others. Here freed blocks of the gradient's size are filled with
 * 0xff (NaN) first, so the parser gets them back. */
static void test_dirty_gradient(void) {
    static const char *svg =
        "<svg width=\"128\" height=\"128\" viewBox=\"0 0 128 128\" xmlns=\"http://www.w3.org/2000/svg\">"
        "<linearGradient id=\"a\" x1=\"0\" x2=\"128\" y1=\"0\" y2=\"0\" gradientUnits=\"userSpaceOnUse\">"
        "<stop offset=\"0\" stop-color=\"#c0bfbc\"/><stop offset=\"1\" stop-color=\"#ffffff\"/></linearGradient>"
        "<rect x=\"8\" y=\"8\" width=\"112\" height=\"112\" fill=\"url(#a)\"/></svg>";
    size_t gsize = sizeof(NSVGgradient) + sizeof(NSVGgradientStop);
    int ok = 0;
    for (int round = 0; round < 20; round++) {
        void *blocks[64];
        for (int i = 0; i < 64; i++) { blocks[i] = malloc(gsize); if (blocks[i]) memset(blocks[i], 0xff, gsize); }
        for (int i = 0; i < 64; i++) free(blocks[i]);
        uint8_t *px = NULL;
        int w = 0, h = 0;
        const char *e = lm_svg_raster(svg, strlen(svg), 96 + round * 4, -1, &px, &w, &h);
        if (!e && px && w > 0 && h > 0) ok++;
        else printf("  dirty gradient at %d px: %s\n", 96 + round * 4, e ? e : "no picture");
        free(px);
    }
    CHECK(ok == 20, "a linear gradient drawn every time on reused memory (%d of 20)", ok);
}

/* the shape of WhatsApp's site icon (QA ws174: a web app's cached SVG): only a viewBox, a filter nanosvg ignores, a
 * gradient fill by url(#id) in user space, arcs and an even-odd phone over the bubble. It must come back as a green
 * picture, transparent in its corners. */
static const char *whatsapp =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 175.216 175.552\">"
    "<defs><linearGradient id=\"b\" x1=\"85.915\" x2=\"86.535\" y1=\"32.567\" y2=\"137.092\" gradientUnits=\"userSpaceOnUse\">"
    "<stop offset=\"0\" stop-color=\"#57d163\"/><stop offset=\"1\" stop-color=\"#23b33a\"/></linearGradient>"
    "<filter id=\"a\" width=\"1.115\" height=\"1.114\" x=\"-.057\" y=\"-.057\" color-interpolation-filters=\"sRGB\">"
    "<feGaussianBlur stdDeviation=\"3.531\"/></filter></defs>"
    "<path fill=\"#b3b3b3\" filter=\"url(#a)\" d=\"M54.532 138.45l2.235 1.324A74.6 74.6 0 1 0 31.1 113.6l1.4 2.2-6 21.9z\"/>"
    "<path fill=\"#fff\" d=\"M12.966 161.238l10.439-38.114a73.42 73.42 0 1 1 29.4 28.6z\"/>"
    "<path fill=\"url(#b)\" d=\"M23.5 150.5l7.6-27.7a63.5 63.5 0 1 1 22.1 21.3z\"/>"
    "<path fill=\"#fff\" fill-rule=\"evenodd\" d=\"M68.772 55.603c-1.378-3.061-2.828-3.123-4.137-3.176l-3.524-.043"
    "c-1.226 0-3.218.46-4.902 2.3s-6.435 6.287-6.435 15.332 6.588 17.785 7.506 19.013 12.718 20.381 31.405 27.75"
    "c15.529 6.124 18.689 4.906 22.061 4.6s10.877-4.447 12.408-8.74 1.532-7.971 1.073-8.74-1.685-1.226-3.525-2.146"
    "-10.877-5.367-12.562-5.981-2.91-.919-4.137.921-4.746 5.979-5.819 7.206-2.144 1.381-3.984.462-7.76-2.861-14.784-9.124"
    "c-5.465-4.873-9.154-10.891-10.228-12.73s-.114-2.835.808-3.751c.825-.824 1.838-2.147 2.759-3.22s1.224-1.84 1.836-3.065"
    ".307-2.301-.153-3.22-4.032-10.011-5.666-13.647z\"/></svg>";

static void test_whatsapp(void) {
    uint8_t *px = NULL;
    int w = 0, h = 0;
    double ms;
    const char *e = raster(whatsapp, strlen(whatsapp), 64, -1, &px, &w, &h, &ms);
    CHECK(!e && px, "whatsapp: %s", e ? e : "no pixels");
    if (e || !px) return;
    CHECK(w == 64 && h == 64, "whatsapp: %dx%d", w, h);
    const uint8_t *corner = px;                                   /* top left: outside the bubble */
    CHECK(corner[3] == 0, "whatsapp: the corner is painted (alpha %u)", corner[3]);
    /* a point of the green bubble above and right of the phone, (135, 60) in the viewBox (premultiplied BGRA) */
    const uint8_t *g = px + ((size_t)(60 * h / 175) * w + 135 * w / 175) * 4;
    CHECK(g[3] > 200 && g[1] > g[2] + 40 && g[1] > g[0] + 40, "whatsapp: not green there (b %u g %u r %u a %u)", g[0], g[1], g[2], g[3]);
    long painted = 0;
    for (int i = 0; i < w * h; i++) painted += px[i * 4 + 3] > 0;
    CHECK(painted > w * h / 2, "whatsapp: only %ld of %d pixels painted", painted, w * h);
    free(px);
}

int main(void) {
    test_dirty_gradient();
    test_scan();
    test_differential();
    test_valid();
    test_symbolic();
    test_malformed();
    test_numbers();
    test_sizes();
    test_whatsapp();
    if (fails) { printf("svgrast-test: %d failed\n", fails); return 1; }
    printf("svgrast-test: ok\n");
    return 0;
}
