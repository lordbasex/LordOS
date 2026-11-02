/* wlregion_test.c — the wl_region behind wl_surface.set_input_region (src/wl/wlregion.h). */
#include "wl/wlregion.h"

#include <limits.h>
#include <stdio.h>

static int fails, total;

static void has(const WlRgn *r, double x, double y, int want, const char *what) {
    total++;
    if (wlrgn_has(r, x, y) != want) { fails++; printf("FAIL %s: (%g, %g) should be %s\n", what, x, y, want ? "in" : "out"); }
}

int main(void) {
    /* the case of a browser's content subsurface: an empty region, so the parent gets the pointer */
    WlRgn r = { 0 };
    has(&r, 0, 0, 0, "empty");
    has(&r, 553, 21, 0, "empty");

    /* edges: the right and bottom ones are outside, fractions count */
    wlrgn_push(&r, 0, 16, 13, 100, 50);
    has(&r, 16, 13, 1, "add");
    has(&r, 115.5, 62.5, 1, "add");
    has(&r, 116, 13, 0, "add");
    has(&r, 16, 63, 0, "add");
    has(&r, 15.9, 13, 0, "add");

    /* subtract, then add back: the order decides */
    wlrgn_push(&r, 1, 20, 20, 10, 10);
    has(&r, 25, 25, 0, "subtract");
    has(&r, 19, 25, 1, "subtract");
    wlrgn_push(&r, 0, 22, 22, 2, 2);
    has(&r, 23, 23, 1, "add over subtract");
    has(&r, 25, 25, 0, "add over subtract");

    /* a copy does not depend on the region: the app destroys it right after set_input_region */
    WlRgn c = { 0 };
    total++;
    if (wlrgn_copy(&c, &r)) { fails++; printf("FAIL copy\n"); }
    wlrgn_free(&r);
    has(&c, 23, 23, 1, "copy");
    has(&c, 25, 25, 0, "copy");

    /* copying an empty region over one with rectangles empties it */
    WlRgn e = { 0 };
    wlrgn_copy(&c, &e);
    has(&c, 23, 23, 0, "copy of empty");

    /* negative and huge rectangles; empty ones are nothing */
    wlrgn_push(&c, 0, 5, 5, 0, 10);
    wlrgn_push(&c, 0, 5, 5, -3, 10);
    has(&c, 5, 6, 0, "no size");
    wlrgn_push(&c, 0, INT_MIN, INT_MIN, INT_MAX, INT_MAX);
    wlrgn_push(&c, 0, INT_MAX - 1, INT_MAX - 1, INT_MAX, INT_MAX);
    has(&c, -5, -5, 1, "huge");
    has(&c, 2147483647.5, 2147483647.5, 1, "huge, no overflow");

    /* the limit */
    int rc = 0;
    for (int i = 0; i < WLRGN_MAX_OPS + 10 && !rc; i++) rc = wlrgn_push(&c, 0, i, 0, 1, 1);
    total++;
    if (rc != -1 || c.n != WLRGN_MAX_OPS) { fails++; printf("FAIL limit: rc %d, n %d\n", rc, c.n); }
    wlrgn_free(&c);

    printf("wlregion: %d/%d\n", total - fails, total);
    return fails ? 1 : 0;
}
