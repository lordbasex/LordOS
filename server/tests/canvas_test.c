/* canvas_test.c — the SIMD rows of canvas.c (NEON on ARM64, NEON=1) must give the same result,
 * bit for bit, as the portable C ones. On x86-64 (no SIMD of its own) it compares C against C and also
 * checks the exact division by 255 that the NEON version uses. See docs/RASPBERRY.md.
 * GPU drag's shader in C (canvas_gdrag_model, docs/GPU.md "The dragged window on the GPU") must give blit_row's
 * pixel at k = 256 for every valid premultiplied source channel (c ≤ alpha) over every destination
 * channel, and every source alpha over every destination alpha: checked exhaustively against both.
 *
 *   make -C server NEON=1 build/canvas-test && server/build/canvas-test   (on the Raspberry Pi)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const char *canvas_test_simd(void);
void canvas_test_blit_row(int simd, uint32_t *row, const uint32_t *s, int n, uint32_t k);
void canvas_test_mask_row(int simd, uint32_t *row, const uint8_t *m, int n, uint32_t argb);
uint32_t canvas_gdrag_model(uint32_t src, uint32_t dst);

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* valid premultiplied pixel (each channel ≤ alpha); sometimes opaque or transparent */
static uint32_t premul(void) {
    uint32_t r = rnd() % 8, a = r == 0 ? 0 : r == 1 ? 255 : rnd() & 255;
    uint32_t c0 = a ? rnd() % (a + 1) : 0, c1 = a ? rnd() % (a + 1) : 0, c2 = a ? rnd() % (a + 1) : 0;
    return a << 24 | c0 << 16 | c1 << 8 | c2;
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void) {
    int fails = 0;
    /* (x·0x8081) >> 23 == x / 255 over the whole range mask_row uses (m·ca + 127 ≤ 65152) */
    for (uint32_t x = 0; x < 65536; x++)
        if ((x * 0x8081u) >> 23 != x / 255) { printf("division by 255: fails at %u\n", x); fails++; break; }

    enum { N = 1037 };                      /* not a multiple of 4: also tests the tail */
    static uint32_t src[N], dst[N], a[N], b[N];
    static uint8_t mask[N];
    for (int it = 0; it < 4000; it++) {
        int n = 1 + (int)(rnd() % N);
        uint32_t k = it % 5 == 0 ? 256 : it % 7 == 0 ? 0 : rnd() % 257;
        for (int i = 0; i < n; i++) { src[i] = premul(); dst[i] = rnd(); }
        memcpy(a, dst, sizeof dst);
        memcpy(b, dst, sizeof dst);
        canvas_test_blit_row(0, a, src, n, k);
        canvas_test_blit_row(1, b, src, n, k);
        for (int i = 0; i < n; i++)
            if (a[i] != b[i]) {
                if (fails++ < 10) printf("blit: i=%d k=%u src=%08x dst=%08x C=%08x SIMD=%08x\n", i, k, src[i], dst[i], a[i], b[i]);
                break;
            }
        uint32_t argb = rnd();
        if (it % 6 == 0) argb |= 0xff000000u;
        for (int i = 0; i < n; i++) { uint32_t r = rnd() % 4; mask[i] = r == 0 ? 0 : r == 1 ? 255 : (uint8_t)rnd(); }
        memcpy(a, dst, sizeof dst);
        memcpy(b, dst, sizeof dst);
        canvas_test_mask_row(0, a, mask, n, argb);
        canvas_test_mask_row(1, b, mask, n, argb);
        for (int i = 0; i < n; i++)
            if (a[i] != b[i]) {
                if (fails++ < 10) printf("mask: i=%d argb=%08x m=%u dst=%08x C=%08x SIMD=%08x\n", i, argb, mask[i], dst[i], a[i], b[i]);
                break;
            }
    }

    /* GPU drag's model against blit_row_c and the SIMD row, exhaustively: for every alpha a and colour c ≤ a,
     * a row of 256 destinations with every channel value d (all three channels, so each sum is checked
     * on each), the destination alpha (7d + a) & 255 (every value once per a) */
    long gdragcases = 0, gdragbad = 0;
    for (uint32_t a = 0; a < 256; a++)
        for (uint32_t c = 0; c <= a; c++) {
            static uint32_t gs[256], gd[256], gc[256], gn[256];
            for (uint32_t i = 0; i < 256; i++) {
                gs[i] = a << 24 | c << 16 | c << 8 | c;
                gd[i] = ((7 * i + a) & 255) << 24 | i << 16 | (255 - i) << 8 | i;
            }
            memcpy(gc, gd, sizeof gd);
            memcpy(gn, gd, sizeof gd);
            canvas_test_blit_row(0, gc, gs, 256, 256);
            canvas_test_blit_row(1, gn, gs, 256, 256);
            for (int i = 0; i < 256; i++) {
                uint32_t m = canvas_gdrag_model(gs[i], gd[i]);
                gdragcases++;
                if (m != gc[i] || m != gn[i]) {
                    if (gdragbad++ < 10) printf("GPU drag model: src=%08x dst=%08x C=%08x SIMD=%08x model=%08x\n", gs[i], gd[i], gc[i], gn[i], m);
                }
            }
        }
    /* and random valid sources with unrelated channels */
    for (int it = 0; it < 200000; it++) {
        uint32_t sp = premul(), dp = rnd(), cr = dp, nr = dp;
        canvas_test_blit_row(0, &cr, &sp, 1, 256);
        canvas_test_blit_row(1, &nr, &sp, 1, 256);
        uint32_t m = canvas_gdrag_model(sp, dp);
        gdragcases++;
        if ((m != cr || m != nr) && gdragbad++ < 10) printf("GPU drag model: src=%08x dst=%08x C=%08x SIMD=%08x model=%08x\n", sp, dp, cr, nr, m);
    }
    printf("canvas-test: GPU drag model: %ld cases, %ld differ\n", gdragcases, gdragbad);
    fails += (int)gdragbad;

    /* time for one 1920×1080 screen with each (not a test: it measures) */
    static uint32_t big_s[1920], big_d[1920];
    static uint8_t big_m[1920];
    for (int i = 0; i < 1920; i++) { big_s[i] = premul(); big_d[i] = rnd(); big_m[i] = (uint8_t)rnd(); }
    for (int simd = 0; simd < 2; simd++) {
        double t0 = now();
        for (int f = 0; f < 20; f++)
            for (int y = 0; y < 1080; y++) canvas_test_blit_row(simd, big_d, big_s, 1920, 200);
        double t1 = now();
        for (int f = 0; f < 20; f++)
            for (int y = 0; y < 1080; y++) canvas_test_mask_row(simd, big_d, big_m, 1920, 0x80336699u);
        double t2 = now();
        printf("%s: blit %.2f ms/screen, mask %.2f ms/screen\n", simd ? canvas_test_simd() : "C",
               (t1 - t0) * 1000 / 20, (t2 - t1) * 1000 / 20);
    }
    if (fails) printf("canvas-test: %d differences\n", fails);
    else printf("canvas-test: %s matches C\n", canvas_test_simd());
    return fails != 0;
}
