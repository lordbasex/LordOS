/* vmdamage_test.c — a machine picture's damage as a few rectangles (src/vm/vmdamage.h; MACHINES-GPU.md Part 1 step 4).
 *
 *   build/vmdamage-test [rounds]
 *
 * Fixed cases (a clock and a moving square stay two boxes; touching boxes join; the ninth box makes room), then a
 * fuzz: random rectangles in a random picture, after each one every pixel ever added is inside a kept box, no two kept
 * boxes are near each other, there are never more than VMDMG_MAX, and the kept area is never more than the bounding
 * box's. Built with ASan and UBSan. Exits 1 on the first failure. */
#include "../src/vm/vmdamage.h"
#include "../src/vm/vmproto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static unsigned seed = 0x5eed1234u;
static unsigned RND(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

static int64_t kept_area(const VmDamage *d) {
    int64_t a = 0;
    for (int i = 0; i < d->n; i++) a += vmdmg_area(d->r[i]);
    return a;
}
static int covered(const VmDamage *d, int x, int y) {
    for (int i = 0; i < d->n; i++)
        if (x >= d->r[i].x0 && x < d->r[i].x1 && y >= d->r[i].y0 && y < d->r[i].y1) return 1;
    return 0;
}

int main(int argc, char **argv) {
    long rounds = argc > 1 ? atol(argv[1]) : 4000;
    _Static_assert(VMDMG_MAX == VD_FRAME_RECTS, "vmdamage.h and vmproto.h agree");
    VmDamage d;

    /* a clock in the corner and a square moving far from it: two boxes, not one 612 × 492 */
    vmdmg_clear(&d);
    vmdmg_add(&d, 1800, 1050, 60, 20);
    vmdmg_add(&d, 200, 300, 64, 64);
    vmdmg_add(&d, 210, 300, 64, 64);
    CHECK(d.n == 2, "a clock and a moving square: %d boxes", d.n);
    CHECK(kept_area(&d) == 60 * 20 + 74 * 64, "their area: %lld", (long long)kept_area(&d));

    /* touching or within VMDMG_NEAR: one box */
    vmdmg_clear(&d);
    vmdmg_add(&d, 0, 0, 10, 10);
    vmdmg_add(&d, 10 + VMDMG_NEAR, 0, 10, 10);
    CHECK(d.n == 1 && d.r[0].x1 == 20 + VMDMG_NEAR, "near boxes join");
    vmdmg_add(&d, 100, 100, 10, 10);
    CHECK(d.n == 2, "a far one stays apart");
    /* a box that bridges two: all three become one, and the merge goes on through what it reaches */
    vmdmg_add(&d, 15, 15, 90, 90);
    CHECK(d.n == 1 && d.r[0].x0 == 0 && d.r[0].y0 == 0 && d.r[0].x1 == 110 && d.r[0].y1 == 110, "a bridge joins both (%d)", d.n);

    /* nine far boxes: the two closest join to make room */
    vmdmg_clear(&d);
    for (int i = 0; i < VMDMG_MAX; i++) vmdmg_add(&d, i * 200, 0, 10, 10);
    CHECK(d.n == VMDMG_MAX, "eight far boxes kept");
    vmdmg_add(&d, 1500, 30, 10, 10);           /* nearest to the box at 1400,0 */
    CHECK(d.n <= VMDMG_MAX, "the ninth fits (%d)", d.n);
    CHECK(covered(&d, 1505, 35) && covered(&d, 1405, 5) && covered(&d, 5, 5), "every box still covered");

    /* empty rectangles are ignored; clipping drops what is outside a smaller picture */
    vmdmg_clear(&d);
    vmdmg_add(&d, 5, 5, 0, 10);
    vmdmg_add(&d, 5, 5, 10, -1);
    CHECK(d.n == 0, "empty rectangles ignored");
    vmdmg_add(&d, 0, 0, 100, 100);
    vmdmg_add(&d, 500, 500, 100, 100);
    vmdmg_clip(&d, 200, 200);
    CHECK(d.n == 1 && d.r[0].x1 == 100, "clipped to a smaller picture");

    /* the fuzz */
    static uint8_t want[512 * 512];
    long bad = 0;
    for (long r = 0; r < rounds && !bad; r++) {
        int W = 16 + (int)(RND() % 497), H = 16 + (int)(RND() % 497);
        memset(want, 0, sizeof want);
        vmdmg_clear(&d);
        int n = 1 + (int)(RND() % 24);
        int bx0 = W, by0 = H, bx1 = 0, by1 = 0;
        for (int k = 0; k < n && !bad; k++) {
            unsigned mw = RND() % 4 ? 40 : (unsigned)W, mh = RND() % 4 ? 40 : (unsigned)H;
            int w = 1 + (int)(RND() % mw), h = 1 + (int)(RND() % mh);
            if (w > W) w = W;
            if (h > H) h = H;
            int x = (int)(RND() % (unsigned)(W - w + 1)), y = (int)(RND() % (unsigned)(H - h + 1));
            vmdmg_add(&d, x, y, w, h);
            for (int yy = y; yy < y + h; yy++) memset(want + (size_t)yy * W + x, 1, (size_t)w);
            if (x < bx0) bx0 = x;
            if (y < by0) by0 = y;
            if (x + w > bx1) bx1 = x + w;
            if (y + h > by1) by1 = y + h;
            if (d.n < 1 || d.n > VMDMG_MAX) { bad = 1; fprintf(stderr, "round %ld: %d boxes\n", r, d.n); break; }
            for (int i = 0; i < d.n && !bad; i++)
                for (int j = i + 1; j < d.n; j++)
                    if (vmdmg_near(d.r[i], d.r[j])) { bad = 1; fprintf(stderr, "round %ld: boxes %d and %d near\n", r, i, j); break; }
            for (int yy = 0; yy < H && !bad && (k % 4 == 3 || k == n - 1); yy++)
                for (int xx = 0; xx < W; xx++)
                    if (want[(size_t)yy * W + xx] && !covered(&d, xx, yy)) { bad = 1; fprintf(stderr, "round %ld: %d,%d lost\n", r, xx, yy); break; }
            for (int i = 0; i < d.n && !bad; i++)
                if (d.r[i].x0 < bx0 || d.r[i].y0 < by0 || d.r[i].x1 > bx1 || d.r[i].y1 > by1) { bad = 1; fprintf(stderr, "round %ld: a box past the bounds\n", r); }
        }
        vmdmg_clip(&d, W, H);
        for (int i = 0; i < d.n && !bad; i++)
            if (d.r[i].x0 < 0 || d.r[i].y0 < 0 || d.r[i].x1 > W || d.r[i].y1 > H || d.r[i].x1 <= d.r[i].x0 || d.r[i].y1 <= d.r[i].y0) bad = 1;
    }
    CHECK(!bad, "fuzz: %ld rounds, every change covered, boxes apart, at most %d, inside the bounds", rounds, VMDMG_MAX);

    printf("vmdamage_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
