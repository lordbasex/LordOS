/* mcursor_test.c — a Machines guest's pointer made ready for lord-server's cursor (src/media/mcursor.c):
 * premultiplied, cut to what shows, the hotspot kept, scaled with the picture and never above the cursor plane. */
#include "../src/media/mcursor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(cond, what) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL: %s\n", what); } } while (0)

/* a w × h cursor, all transparent, with a box of (b, g, r, a) from (x0, y0) to (x1, y1) inclusive */
static uint8_t *cursor(int w, int h, int x0, int y0, int x1, int y1, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    uint8_t *p = calloc((size_t)w * h, 4);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) { uint8_t *q = p + ((size_t)y * w + x) * 4; q[0] = b; q[1] = g; q[2] = r; q[3] = a; }
    return p;
}

int main(void) {
    McurImage m, s;

    /* a 64 × 64 cursor with a 10 × 16 arrow at (2, 3), hotspot at (2, 3): cut to the arrow */
    uint8_t *c = cursor(64, 64, 2, 3, 11, 18, 0x20, 0x40, 0x80, 0xff);
    CHECK(mcur_prepare(c, 64, 64, 2, 3, &m) == 0, "prepare: accepted");
    CHECK(m.w == 10 && m.h == 16, "prepare: cut to what is visible");
    CHECK(m.hx == 0 && m.hy == 0, "prepare: hotspot moved with the cut");
    CHECK(m.px[0] == 0xff804020u, "prepare: opaque pixel as ARGB");
    mcur_free(&m);
    free(c);

    /* the hotspot outside the visible part stays inside the image */
    c = cursor(32, 32, 10, 10, 19, 19, 0, 0, 0, 0xff);
    CHECK(mcur_prepare(c, 32, 32, 15, 2, &m) == 0, "hotspot above: accepted");
    CHECK(m.hx == 5 && m.hy == 0 && m.h == 18 && m.w == 10, "hotspot above: the image grows up to it");
    mcur_free(&m);
    free(c);

    /* straight alpha → premultiplied */
    c = cursor(4, 4, 0, 0, 3, 3, 0xff, 0xff, 0xff, 0x80);
    CHECK(mcur_prepare(c, 4, 4, 0, 0, &m) == 0 && m.px[0] == 0x80808080u, "half transparent white: premultiplied");
    mcur_free(&m);
    free(c);

    /* fully transparent: the guest hid its pointer */
    c = cursor(16, 16, 0, 0, -1, -1, 0, 0, 0, 0);
    CHECK(mcur_prepare(c, 16, 16, 0, 0, &m) == 1 && m.px == NULL, "transparent: hidden");
    free(c);

    /* refused: hotspot outside, too big, nothing */
    c = cursor(16, 16, 0, 0, 3, 3, 0, 0, 0, 0xff);
    CHECK(mcur_prepare(c, 16, 16, 16, 0, &m) == -1, "hotspot outside: refused");
    CHECK(mcur_prepare(c, 300, 16, 0, 0, &m) == -1, "too wide: refused");
    CHECK(mcur_prepare(NULL, 16, 16, 0, 0, &m) == -1, "no pixels: refused");
    free(c);

    /* scale: the picture's scale, 1 near 1, at most 64 on the long side */
    CHECK(mcur_fit_scale(10, 16, 1.03f, 64) == 1, "fit: within 5 % of 1 is 1");
    CHECK(mcur_fit_scale(10, 16, 2, 64) == 2, "fit: 2 when it fits");
    CHECK(mcur_fit_scale(32, 32, 3, 64) == 2, "fit: capped at 64");
    CHECK(mcur_fit_scale(10, 16, 0, 64) == 1, "fit: unknown scale is 1");

    c = cursor(20, 20, 0, 0, 9, 19, 0, 0, 0xff, 0xff);
    CHECK(mcur_prepare(c, 20, 20, 4, 6, &m) == 0, "for scaling: accepted");
    CHECK(mcur_scale(&m, 2, &s) == 0 && s.w == 20 && s.h == 40, "scale 2: twice the size");
    CHECK(s.hx == 9 && s.hy == 13, "scale 2: hotspot at the middle of its pixel");
    CHECK(s.px[(size_t)20 * 10 + 10] == 0xffff0000u, "scale 2: inside stays opaque red");
    mcur_free(&s);
    CHECK(mcur_scale(&m, 1, &s) == 0 && s.w == m.w && s.hx == 4 && s.hy == 6 && !memcmp(s.px, m.px, (size_t)m.w * m.h * 4), "scale 1: a copy");
    mcur_free(&s);
    CHECK(mcur_scale(&m, 0.5f, &s) == 0 && s.w == 5 && s.h == 10 && s.hx == 2 && s.hy == 3, "scale 0.5: half, hotspot with it");
    mcur_free(&s);
    mcur_free(&m);
    free(c);

    printf("mcursor_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
