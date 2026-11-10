/* svgrast_test.c — SVG icons in lord-media (src/media/helper/lm_svg.c), without the helper around them: a valid
 * icon, a symbolic one recolored, the aspect kept, malformed markup, huge numbers, deep nesting, too many elements
 * and a file over 1 MB. Hostile cases must come back quickly with an error code (or a picture), never crash.
 * build/media-test svg runs the same kind of files through lord-media and its sandbox (test-full). */
#include "../src/media/helper/lm_svg.h"

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
    CHECK(lm_svg_scan(speaker, strlen(speaker), &n, &d) == 0 && n == 4 && d == 1, "speaker: %d elements, depth %d", n, d);
    const char *nest = "<svg><g><g><rect/></g></g></svg>";
    CHECK(lm_svg_scan(nest, strlen(nest), &n, &d) == 0 && d == 3, "nested: depth %d", d);
    const char *cut = "<svg><path d=\"M0 0";
    CHECK(lm_svg_scan(cut, strlen(cut), &n, &d) == -1, "a tag that never ends");
    const char *cdata = "<svg><![CDATA[ <<<< ]]><rect/></svg>";
    CHECK(lm_svg_scan(cdata, strlen(cdata), &n, &d) == 0 && n == 3, "CDATA skipped: %d elements", n);
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

static void test_malformed(void) {
    expect_err("empty", "", 0, "media.notAnImage");
    expect_err("not markup", "hello", 5, "media.notAnImage");
    const char *cut = "<svg width=\"16\" height=\"16\"><path d=\"M 0 0 L 16";
    expect_err("cut inside a tag", cut, strlen(cut), "media.svgTooComplex");
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

int main(void) {
    test_scan();
    test_valid();
    test_symbolic();
    test_malformed();
    test_numbers();
    test_sizes();
    if (fails) { printf("svgrast-test: %d failed\n", fails); return 1; }
    printf("svgrast-test: ok\n");
    return 0;
}
