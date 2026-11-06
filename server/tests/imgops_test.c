/* imgops_test.c — the image editor's pixel operations (src/media/imgops.c, issue #30): clipping of
 * a selection, the solid-fill and pixelate redactions (nothing of the original left inside, nothing
 * touched outside), resize (sizes, flat colors stay exact, area average when shrinking, transparency
 * kept) and quarter turns (every pixel lands where lord-media's edit job puts it).
 *
 *   make -C server build/imgops-test && server/build/imgops-test
 */
#include "../src/media/imgops.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 777;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint32_t opaque_px(void) { return 0xff000000u | (rnd() & 0xffffffu); }

static uint32_t *noise(int w, int h) {
    uint32_t *p = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) p[i] = opaque_px();
    return p;
}

static void test_clip(void) {
    int x0, y0, x1, y1;
    CHECK(img_clip(100, 50, 10, 5, 30, 25, &x0, &y0, &x1, &y1) && x0 == 10 && y0 == 5 && x1 == 30 && y1 == 25, "plain rect");
    CHECK(img_clip(100, 50, 30, 25, 10, 5, &x0, &y0, &x1, &y1) && x0 == 10 && y1 == 25, "corners in any order");
    CHECK(img_clip(100, 50, 9.5f, 4.2f, 20.1f, 10.7f, &x0, &y0, &x1, &y1) && x0 == 9 && y0 == 4 && x1 == 21 && y1 == 11, "rounded outwards");
    CHECK(img_clip(100, 50, -40, -40, 500, 500, &x0, &y0, &x1, &y1) && x0 == 0 && y0 == 0 && x1 == 100 && y1 == 50, "clamped to the image");
    CHECK(!img_clip(100, 50, 200, 10, 300, 20, &x0, &y0, &x1, &y1), "outside: nothing");
    CHECK(!img_clip(100, 50, 10, 10, 10, 40, &x0, &y0, &x1, &y1), "zero width: nothing");
    CHECK(!img_clip(100, 50, NAN, 0, 10, 10, &x0, &y0, &x1, &y1), "NaN refused");
    CHECK(!img_clip(100, 50, 0, 0, INFINITY, 10, &x0, &y0, &x1, &y1), "infinity refused");
    CHECK(!img_clip(0, 50, 0, 0, 10, 10, &x0, &y0, &x1, &y1), "empty image");
}

static void test_fill(void) {
    int w = 64, h = 40;
    uint32_t *p = noise(w, h), *orig = malloc((size_t)w * h * 4);
    memcpy(orig, p, (size_t)w * h * 4);
    img_fill(p, w, h, 10, 8, 30.5f, 20, 0x00123456u);           /* alpha forced to 255 */
    int inside_ok = 1, outside_ok = 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int in = x >= 10 && x < 31 && y >= 8 && y < 20;
            if (in && p[y * w + x] != 0xff123456u) inside_ok = 0;
            if (!in && p[y * w + x] != orig[y * w + x]) outside_ok = 0;
        }
    CHECK(inside_ok, "fill: every pixel inside is the opaque color");
    CHECK(outside_ok, "fill: nothing outside changes");
    memcpy(p, orig, (size_t)w * h * 4);
    img_fill(p, w, h, NAN, 0, 10, 10, 0xff000000u);
    CHECK(!memcmp(p, orig, (size_t)w * h * 4), "fill: NaN does nothing");
    img_fill(p, w, h, -100, -100, 1000, 1000, 0xff000000u);
    int all = 1;
    for (int i = 0; i < w * h; i++) if (p[i] != 0xff000000u) all = 0;
    CHECK(all, "fill: a huge rect covers the image and stays inside it");
    free(p); free(orig);
}

static void test_pixelate(void) {
    int w = 50, h = 30, b = 8;
    uint32_t *p = noise(w, h), *orig = malloc((size_t)w * h * 4);
    memcpy(orig, p, (size_t)w * h * 4);
    img_pixelate(p, w, h, 5, 4, 41, 29, b);
    int outside_ok = 1, cells_flat = 1, avg_ok = 1, changed = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int in = x >= 5 && x < 41 && y >= 4 && y < 29;
            if (!in && p[y * w + x] != orig[y * w + x]) outside_ok = 0;
            if (in && p[y * w + x] != orig[y * w + x]) changed++;
            if (in) {
                int cx = 5 + (x - 5) / b * b, cy = 4 + (y - 4) / b * b;
                if (p[y * w + x] != p[cy * w + cx]) cells_flat = 0;
            }
        }
    /* the first cell's color is the rounded mean of the original cell */
    unsigned long s[3] = { 0, 0, 0 };
    for (int y = 4; y < 12; y++) for (int x = 5; x < 13; x++) { uint32_t q = orig[y * w + x]; s[0] += (q >> 16) & 255; s[1] += (q >> 8) & 255; s[2] += q & 255; }
    uint32_t c = p[4 * w + 5];
    for (int k = 0; k < 3; k++) if ((unsigned long)((c >> (16 - 8 * k)) & 255) != (s[k] + 32) / 64) avg_ok = 0;
    CHECK(outside_ok, "pixelate: nothing outside changes");
    CHECK(cells_flat, "pixelate: each cell is one color (the cell grid starts at the rect's corner)");
    CHECK(avg_ok, "pixelate: a cell is its average");
    CHECK(changed > (36 * 25) * 9 / 10, "pixelate: the original detail is gone (%d changed)", changed);
    CHECK((c >> 24) == 255, "pixelate: opaque stays opaque");
    /* a transparent half stays premultiplied (no channel above alpha) */
    uint32_t t[16];
    for (int i = 0; i < 16; i++) t[i] = i % 2 ? 0xffffffffu : 0;
    img_pixelate(t, 4, 4, 0, 0, 4, 4, 4);
    uint32_t a = t[0] >> 24, r = (t[0] >> 16) & 255;
    CHECK(a == 128 && r == 128, "pixelate: half transparent white → 50 %% premultiplied (got %08x)", t[0]);
    free(p); free(orig);
}

static void test_resize(void) {
    /* flat color stays exact at every size */
    int sw = 37, sh = 23;
    uint32_t *flat = malloc((size_t)sw * sh * 4);
    for (int i = 0; i < sw * sh; i++) flat[i] = 0xff3c78b4u;
    int sizes[][2] = { { 10, 6 }, { 37, 23 }, { 100, 61 }, { 1, 1 }, { 300, 2 } };
    for (unsigned k = 0; k < sizeof sizes / sizeof *sizes; k++) {
        int dw = sizes[k][0], dh = sizes[k][1];
        uint32_t *d = malloc((size_t)dw * dh * 4);
        int r = img_resize(flat, sw, sh, d, dw, dh), ok = r == 0;
        for (int i = 0; ok && i < dw * dh; i++) if (d[i] != 0xff3c78b4u) ok = 0;
        CHECK(ok, "resize: flat color exact at %d × %d", dw, dh);
        free(d);
    }
    free(flat);
    /* halving = the mean of each 2 × 2 block */
    uint32_t q[16], h[4];
    for (int i = 0; i < 16; i++) q[i] = 0xff000000u | (uint32_t)(i * 16) << 16 | (uint32_t)(255 - i * 16) << 8 | 7;
    CHECK(img_resize(q, 4, 4, h, 2, 2) == 0, "resize 4 → 2");
    int mean_ok = 1;
    for (int by = 0; by < 2; by++)
        for (int bx = 0; bx < 2; bx++) {
            unsigned s = 0;
            for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) s += (q[(by * 2 + y) * 4 + bx * 2 + x] >> 16) & 255;
            if (((h[by * 2 + bx] >> 16) & 255) != (s + 2) / 4) mean_ok = 0;
        }
    CHECK(mean_ok, "resize: shrinking by 2 averages each 2 × 2 block");
    /* doubling a 2-pixel ramp gives a monotone ramp between the two ends */
    uint32_t two[2] = { 0xff000000u, 0xffff0000u }, four[4];
    img_resize(two, 2, 1, four, 4, 1);
    CHECK(four[0] == 0xff000000u && four[3] == 0xffff0000u && ((four[1] >> 16) & 255) < ((four[2] >> 16) & 255), "resize: bilinear enlarge keeps the ends and the order");
    /* transparency kept, premultiplied */
    uint32_t tr[4] = { 0, 0, 0xffffffffu, 0xffffffffu }, one;
    img_resize(tr, 2, 2, &one, 1, 1);
    CHECK(one >> 24 == 128 && ((one >> 16) & 255) <= 128, "resize: half transparent stays 50 %% (got %08x)", one);
    /* bad sizes refused, dst untouched */
    uint32_t d = 0x12345678u;
    CHECK(img_resize(two, 2, 1, &d, 0, 1) == -1 && d == 0x12345678u, "resize: width 0 refused");
    CHECK(img_resize(two, 2, 1, &d, 1, IMG_MAX_DIM + 1) == -1, "resize: over the limit refused");
    CHECK(img_resize(NULL, 2, 1, &d, 1, 1) == -1, "resize: no source refused");
    /* a big photo-like shrink: sizes and that it runs */
    int bw = 1600, bh = 1200;
    uint32_t *big = noise(bw, bh), *small = malloc(640 * 480 * 4);
    CHECK(img_resize(big, bw, bh, small, 640, 480) == 0, "resize 1600 × 1200 → 640 × 480");
    free(big); free(small);
}

static void test_rotate(void) {
    int w = 5, h = 3;
    uint32_t src[15], dst[15];
    for (int i = 0; i < 15; i++) src[i] = 0xff000000u | (uint32_t)i;
    int ow, oh;
    img_rotated_size(w, h, 1, &ow, &oh);
    CHECK(ow == 3 && oh == 5, "turn: sides swap");
    img_rotate(src, w, h, 1, dst);
    /* clockwise: the bottom-left pixel goes to the top-left, the top-left to the top-right */
    CHECK(dst[0] == src[(h - 1) * w] && dst[ow - 1] == src[0], "turn 90: clockwise");
    uint32_t back[15], t2[15], t3[15];
    img_rotate(dst, ow, oh, 3, back);
    CHECK(!memcmp(back, src, sizeof src), "turn 90 then 270: the original");
    img_rotate(src, w, h, 2, t2);
    CHECK(t2[0] == src[14] && t2[14] == src[0], "turn 180");
    img_rotate(src, w, h, -1, t3);
    img_rotate(src, w, h, 3, back);
    CHECK(!memcmp(t3, back, sizeof src), "turn -1 = 3");
    img_rotate(src, w, h, 4, back);
    CHECK(!memcmp(back, src, sizeof src), "turn 4 = none");
}

int main(void) {
    test_clip();
    test_fill();
    test_pixelate();
    test_resize();
    test_rotate();
    printf("imgops-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
