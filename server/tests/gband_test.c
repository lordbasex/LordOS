/* gband_test.c — the rows each linear copy of a vGPU's picture lacks (src/gpu/gband.h; ui/js_vm.c, gout).
 *
 * A model of the two copies: each row of each copy holds the frame number it was last copied from, and the picture's
 * rows the frame number they last changed in. Whatever the frames' bands, after every copy the copy about to be shown
 * must hold the current picture in every row (a row it lacks would show an old frame, or for a copy just made,
 * whatever its memory held: the noise of 2026-11-22). */
#include "../src/gpu/gband.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

#define H 64
static int pic[H];                      /* the frame each row of the picture last changed in */
static int copy[2][H];                  /* the frame each row of a copy came from; -1: never written (garbage) */

static unsigned rnd = 12345;
static int r(int n) { rnd = rnd * 1103515245u + 12345u; return (int)((rnd >> 16) % (unsigned)n); }

static void basics(void) {
    GBand b;
    gband_all(&b, 1080);
    CHECK(b.y0 == 0 && b.y1 == 1080, "all");
    GBand c = gband_copy(&b, 500, 510, 1080);
    CHECK(c.y0 == 0 && c.y1 == 1080, "a new copy is copied whole, not only the frame's band: %d..%d", c.y0, c.y1);
    gband_none(&b);
    CHECK(gband_empty(&b), "none");
    c = gband_copy(&b, 500, 510, 1080);
    CHECK(c.y0 == 500 && c.y1 == 510, "nothing lacking: the frame's band");
    gband_add(&b, 10, 20);
    gband_add(&b, 100, 110);
    CHECK(b.y0 == 10 && b.y1 == 110, "two bands: their hull %d..%d", b.y0, b.y1);
    gband_add(&b, 50, 50);
    CHECK(b.y0 == 10 && b.y1 == 110, "an empty band changes nothing");
    c = gband_copy(&b, 200, 300, 1080);
    CHECK(c.y0 == 10 && c.y1 == 300, "lacking and changed: %d..%d", c.y0, c.y1);
    c = gband_copy(&b, -5, 2000, 1080);
    CHECK(c.y0 == 0 && c.y1 == 1080, "kept within the picture");
    GBand two[2];
    gband_none(&two[0]); gband_all(&two[1], 1080);
    gband_copied(two, 1, 40, 60);
    CHECK(gband_empty(&two[1]) && two[0].y0 == 40 && two[0].y1 == 60, "copied: the copy whole, the shown one lacks the band");
    gband_none(&two[1]); gband_copied(two, 0, 0, 1080);
    CHECK(gband_empty(&two[0]) && two[1].y0 == 0 && two[1].y1 == 1080, "the other copy lacks the band, shown or not");
}

/* js_vm.c's loop over many frames, with new copies (an attach, a re-import) and source changes at random */
static void model(void) {
    GBand st[2];
    int shown = -1, frame = 0;
    for (int k = 0; k < 2; k++) { gband_all(&st[k], H); for (int y = 0; y < H; y++) copy[k][y] = -1; }
    for (int y = 0; y < H; y++) pic[y] = 0;
    for (int it = 0; it < 200000; it++) {
        frame++;
        int y0 = r(H), y1 = y0 + 1 + r(H - y0);
        if (r(4) == 0) { y0 = 0; y1 = H; }             /* QEMU's vfio display: the whole picture */
        for (int y = y0; y < y1; y++) pic[y] = frame;
        int ev = r(50);
        if (ev == 0) {                                  /* a new attach: both copies made again (gout_free) */
            for (int k = 0; k < 2; k++) { gband_all(&st[k], H); for (int y = 0; y < H; y++) copy[k][y] = -1; }
            shown = -1;
        } else if (ev == 1) {                           /* one copy imported again (a new GPU context) */
            int k = r(2);
            gband_all(&st[k], H);
            if (shown == k) shown = -1;
        } else if (ev == 2) {                           /* another dmabuf: nothing known about either copy */
            gband_all(&st[0], H); gband_all(&st[1], H);
            /* a different buffer: every row of the picture counts as changed */
            for (int y = 0; y < H; y++) pic[y] = frame;
        }
        int k = shown == 0 ? 1 : 0;
        GBand c = gband_copy(&st[k], y0, y1, H);
        for (int y = c.y0; y < c.y1; y++) copy[k][y] = pic[y];
        gband_copied(st, k, y0, y1);
        shown = k;
        for (int y = 0; y < H; y++)
            if (copy[k][y] != pic[y]) { CHECK(0, "frame %d: copy %d row %d holds %d, the picture %d", frame, k, y, copy[k][y], pic[y]); return; }
    }
}

int main(void) {
    basics();
    model();
    if (fails) { fprintf(stderr, "gband-test: %d failure(s)\n", fails); return 1; }
    printf("gband-test: ok\n");
    return 0;
}
