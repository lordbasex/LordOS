/* outputs_test.c — the layout math of several displays (src/outputs.c): scale rounding and the
 * recommended scale, logical ↔ physical conversion, damage rectangles, mirrored views, snapping
 * an arrangement, fixing overlaps, the pointer across displays. Run by `make test`
 * (mk/displays.mk). */
#include "outputs.h"

#include <math.h>
#include <stdio.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-6)

static void scale_tests(void) {
    CHECK(out_scale_valid(100) && out_scale_valid(125) && out_scale_valid(150) && out_scale_valid(175) && out_scale_valid(200));
    CHECK(!out_scale_valid(110) && !out_scale_valid(75) && !out_scale_valid(225) && !out_scale_valid(0));
    CHECK(out_scale_round(1.0) == 100);
    CHECK(out_scale_round(1.12) == 100);
    CHECK(out_scale_round(1.13) == 125);
    CHECK(out_scale_round(1.37) == 125);
    CHECK(out_scale_round(1.38) == 150);
    CHECK(out_scale_round(1.62) == 150);
    CHECK(out_scale_round(1.63) == 175);
    CHECK(out_scale_round(1.9) == 200);
    CHECK(out_scale_round(3.0) == 200);
    CHECK(out_scale_round(0.5) == 100);
    CHECK(out_scale_round(-1) == 100);
    /* recommended, from EDID sizes of real panels */
    CHECK(out_scale_recommended(1920, 1080, 531, 299) == 100);     /* 24" 1080p, 92 dpi */
    CHECK(out_scale_recommended(2560, 1440, 597, 336) == 100);     /* 27" 1440p, 109 dpi */
    CHECK(out_scale_recommended(3840, 2160, 597, 336) == 150);     /* 27" 4K, 163 dpi */
    CHECK(out_scale_recommended(3840, 2160, 344, 194) == 200);     /* 15.6" 4K laptop, 282 dpi */
    CHECK(out_scale_recommended(1920, 1080, 344, 194) == 125);     /* 15.6" 1080p laptop, 141 dpi */
    CHECK(out_scale_recommended(2880, 1800, 302, 189) == 200);     /* 14" 3K, 242 dpi */
    CHECK(out_scale_recommended(1366, 768, 293, 165) == 100);      /* 13.3" 1366×768: 118 dpi, but 1093 wide at 125 */
    CHECK(out_scale_recommended(1920, 1080, 0, 0) == 100);         /* no size in the EDID */
    CHECK(out_scale_recommended(1920, 1080, 10, 6) == 100);        /* projector: bogus size */
    CHECK(out_scale_recommended(1280, 800, 325, 203) == 100);      /* QEMU's EDID (100 dpi) */
    int lw, lh;
    out_logical_size(3840, 2160, 150, &lw, &lh);
    CHECK(lw == 2560 && lh == 1440);
    out_logical_size(2560, 1600, 175, &lw, &lh);
    CHECK(lw == 1463 && lh == 914);
    out_logical_size(1366, 768, 125, &lw, &lh);
    CHECK(lw == 1093 && lh == 614);
    out_logical_size(1920, 1080, 133, &lw, &lh);                   /* invalid → 100 */
    CHECK(lw == 1920 && lh == 1080);
}

static void view_tests(void) {
    OutView v = out_view(1280, 0, 3840, 2160, 150);
    CHECK(v.w == 2560 && v.h == 1440 && NEAR(v.k, 1.5) && v.dx == 0 && v.dy == 0);
    double px, py, lx, ly;
    out_to_phys(&v, 1280, 0, &px, &py);
    CHECK(NEAR(px, 0) && NEAR(py, 0));
    out_to_phys(&v, 1280 + 100.5, 10, &px, &py);
    CHECK(NEAR(px, 150.75) && NEAR(py, 15));
    out_to_logical(&v, 3840, 2160, &lx, &ly);
    CHECK(NEAR(lx, 1280 + 2560) && NEAR(ly, 1440));
    for (int i = 0; i < 50; i++) {                                   /* round trip */
        double x = 1280 + i * 37.25, y = i * 11.5;
        out_to_phys(&v, x, y, &px, &py);
        out_to_logical(&v, px, py, &lx, &ly);
        CHECK(NEAR(lx, x) && NEAR(ly, y));
    }
    /* damage: a logical box → covering physical pixels, padded by one when scaled, clamped */
    Rect r = out_phys_rect(&v, 1290, 10, 1300, 20);
    CHECK(r.x0 == 14 && r.y0 == 14 && r.x1 == 31 && r.y1 == 31);
    r = out_phys_rect(&v, 0, 0, 1280, 800);                          /* the other display: nothing */
    CHECK(r.x1 <= r.x0);
    r = out_phys_rect(&v, 1000, -50, 5000, 3000);                     /* clamped to the frame */
    CHECK(r.x0 == 0 && r.y0 == 0 && r.x1 == 3840 && r.y1 == 2160);
    OutView one = out_view(0, 0, 1280, 800, 100);
    r = out_phys_rect(&one, 10.5f, 10, 20, 20.5f);                    /* unscaled: no padding */
    CHECK(r.x0 == 10 && r.y0 == 10 && r.x1 == 20 && r.y1 == 21);
    float a, b, c, d;
    out_logical_box(&v, (Rect){ 15, 15, 30, 30 }, &a, &b, &c, &d);
    CHECK(a == 1290 && b == 10 && c == 1300 && d == 20);
    out_logical_box(&v, (Rect){ 1, 1, 2, 2 }, &a, &b, &c, &d);        /* partial logical pixels widen */
    CHECK(a == 1280 && b == 0 && c == 1282 && d == 2);
    /* 125 %: 1920 → 1536 */
    OutView q = out_view(0, 0, 1920, 1080, 125);
    CHECK(q.w == 1536 && q.h == 864 && NEAR(q.k, 1.25));
    /* mirror: the main display's 1280×800 on a 1920×1080 one: k = 1.35, centered horizontally */
    OutView m = out_view_mirror(1280, 800, 1920, 1080);
    CHECK(NEAR(m.k, 1.35) && m.dx == 96 && m.dy == 0 && m.x == 0 && m.w == 1280);
    out_to_phys(&m, 0, 0, &px, &py);
    CHECK(NEAR(px, 96) && NEAR(py, 0));
    out_to_phys(&m, 1280, 800, &px, &py);
    CHECK(NEAR(px, 1824) && NEAR(py, 1080));
    OutView same = out_view_mirror(1280, 800, 1280, 800);
    CHECK(NEAR(same.k, 1) && same.dx == 0 && same.dy == 0);
    OutView tall = out_view_mirror(1280, 800, 1280, 1024);
    CHECK(NEAR(tall.k, 1) && tall.dx == 0 && tall.dy == 112);
}

static void arrange_tests(void) {
    ORect r[3] = { { 0, 0, 1280, 800 }, { 0, 0, 2560, 1440 }, { 0, 0, 1920, 1080 } };
    int x, y;
    /* dropped a bit to the right of the main one, slightly down: sticks to its right edge, tops aligned */
    out_snap(r, 2, 1, 1300, 12, 24, &x, &y);
    CHECK(x == 1280 && y == 0);
    /* dropped far down: still touching the main display (never a gap) */
    out_snap(r, 2, 1, 1500, 2000, 24, &x, &y);
    CHECK(y == 800 || x == 1280);
    CHECK(!(x < 1280 && x + 2560 > 0 && y < 800 && y + 1440 > 0));    /* no overlap */
    /* dropped on top of it, on the left half: goes to the left side */
    out_snap(r, 2, 1, -2000, 100, 24, &x, &y);
    CHECK(x == -2560 && y == 100);
    /* above, centered: the centers align when close */
    out_snap(r, 2, 1, -640 + 5, -1500, 24, &x, &y);
    CHECK(x == 640 - 1280 && y == -1440);
    /* without alignment (snap 0) the position along the edge is kept */
    out_snap(r, 2, 1, 1300, 12, 0, &x, &y);
    CHECK(x == 1280 && y == 12);
    /* the edge must overlap at least one pixel: a corner-only touch slides back */
    out_snap(r, 2, 1, 1280, 900, 0, &x, &y);
    CHECK((x == 1280 && y == 799) || (x == 1279 && y == 800));
    /* three displays: the third can't overlap either */
    r[1].x = 1280; r[1].y = 0;
    out_snap(r, 3, 2, 1400, 100, 24, &x, &y);
    ORect m = { x, y, 1920, 1080 };
    for (int i = 0; i < 2; i++) CHECK(!(m.x < r[i].x + r[i].w && r[i].x < m.x + m.w && m.y < r[i].y + r[i].h && r[i].y < m.y + m.h));
    /* fix: the second display got bigger (a scale change) and now overlaps → re-snapped */
    ORect f[2] = { { 0, 0, 1280, 800 }, { 1000, 0, 2560, 1440 } };
    out_fix(f, 2);
    CHECK(f[0].x == 0 && f[0].y == 0);
    CHECK(f[1].x == 1280 && f[1].y == 0);
    /* a gap (the main one shrank) is closed too */
    ORect g[2] = { { 0, 0, 1024, 768 }, { 1280, 0, 1920, 1080 } };
    out_fix(g, 2);
    CHECK(g[1].x == 1024 && g[1].y == 0);
    /* fits: Settings' drops (sdk/displaylayout.js) touch an edge and overlap nothing */
    ORect q[2] = { { 0, 0, 1280, 800 }, { 0, 0, 1920, 1080 } };
    CHECK(out_fits(q, 2, 1, 1280, 0));
    CHECK(out_fits(q, 2, 1, 1280, 799));            /* one shared pixel */
    CHECK(out_fits(q, 2, 1, -320, 800));            /* stacked, centered */
    CHECK(!out_fits(q, 2, 1, 1280, 800));           /* a corner only */
    CHECK(!out_fits(q, 2, 1, 1281, 0));             /* a gap */
    CHECK(!out_fits(q, 2, 1, 1000, 0));             /* overlapping */
    /* such a position is kept exactly by out_snap without alignment (display_arrange uses snap 0) */
    out_snap(q, 2, 1, -320, 800, 0, &x, &y);
    CHECK(x == -320 && y == 800);
    out_snap(q, 2, 1, 1280, 500, 0, &x, &y);
    CHECK(x == 1280 && y == 500);
    /* valid: joined through shared edges; a chain main ← 2 ← 1 is kept by out_fix */
    ORect c[3] = { { 0, 0, 1280, 800 }, { 3200, 0, 1024, 768 }, { 1280, 0, 1920, 1080 } };
    CHECK(out_valid(c, 3));
    out_fix(c, 3);
    CHECK(c[1].x == 3200 && c[1].y == 0 && c[2].x == 1280 && c[2].y == 0);
    ORect gap[2] = { { 0, 0, 1280, 800 }, { 1300, 0, 1920, 1080 } };
    CHECK(!out_valid(gap, 2));
    ORect ov[2] = { { 0, 0, 1280, 800 }, { 1200, 0, 1920, 1080 } };
    CHECK(!out_valid(ov, 2));
    /* normalize: the arrangement's corner becomes (0, 0) */
    ORect n[2] = { { 0, 0, 1280, 800 }, { -2560, -200, 2560, 1440 } };
    int bw, bh;
    out_normalize(n, 2, &bw, &bh);
    CHECK(n[0].x == 2560 && n[0].y == 200 && n[1].x == 0 && n[1].y == 0 && bw == 3840 && bh == 1440);
    ORect au[3] = { { 9, 9, 1280, 800 }, { 9, 9, 1920, 1080 }, { 9, 9, 800, 600 } };
    out_auto(au, 3);
    CHECK(au[0].x == 0 && au[1].x == 1280 && au[2].x == 3200 && au[2].y == 0);
}

static void pointer_tests(void) {
    ORect r[2] = { { 0, 0, 1280, 800 }, { 1280, 0, 1920, 1080 } };
    CHECK(out_at(r, 2, 0, 0) == 0);
    CHECK(out_at(r, 2, 1279, 799) == 0);
    CHECK(out_at(r, 2, 1280, 799) == 1);
    CHECK(out_at(r, 2, 100, 900) == -1);                           /* below the shorter display */
    int x = 100, y = 900;
    out_clamp(r, 2, &x, &y);
    CHECK(x == 100 && y == 799);
    x = 1290; y = 1000;                                             /* inside the taller one: untouched */
    out_clamp(r, 2, &x, &y);
    CHECK(x == 1290 && y == 1000);
    x = 1270; y = 1000;                                             /* in the gap next to the taller one */
    out_clamp(r, 2, &x, &y);
    CHECK(x == 1280 && y == 1000);
    x = -50; y = -50;
    out_clamp(r, 2, &x, &y);
    CHECK(x == 0 && y == 0);
    x = 9999; y = 9999;
    out_clamp(r, 2, &x, &y);
    CHECK(x == 3199 && y == 1079);
    /* a window spanning both: the one with more of it */
    CHECK(out_best(r, 2, 1000, 100, 1500, 400) == 0);
    CHECK(out_best(r, 2, 1100, 100, 1700, 400) == 1);
    CHECK(out_best(r, 2, 5000, 0, 6000, 10) == -1);
}

int main(void) {
    scale_tests();
    view_tests();
    arrange_tests();
    pointer_tests();
    printf("outputs-test: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
