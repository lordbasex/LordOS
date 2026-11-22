/* wlicon_test.c — src/wl/wlicon.h: an app's own window icon copied, re-encoded and size-limited (xdg-toplevel-icon,
 * docs/WAYLAND.md "Window icons"). */
#include <stdio.h>
#include "wl/wlicon.h"

static int fails;
static void check(const char *what, long got, long want) {
    if (got == want) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}

int main(void) {
    /* premultiplied ARGB → straight RGBA */
    uint32_t px[4] = { 0x80800000u, 0x00000000u, 0xff00ff00u, 0x40ff0000u };   /* half red; clear; green; red above alpha */
    WlIcon *ic = wlicon_from_argb(px, 2, 8, 1, 0);
    check("copied", ic != NULL, 1);
    check("half-transparent red: straight 255", ic->rgba[0], 255);
    check("... alpha 128", ic->rgba[3], 128);
    check("clear stays clear", ic->rgba[4] + ic->rgba[5] + ic->rgba[6] + ic->rgba[7], 0);
    check("green opaque", ic->rgba[9] * 1000L + ic->rgba[11], 255255);
    check("a colour above its alpha is cut to 255", ic->rgba[12], 255);
    check("its own copy: the client's pixels change, the icon doesn't", (px[0] = 0, ic->rgba[0]), 255);
    WlIcon *op = wlicon_from_argb(px, 2, 8, 1, 1);
    check("XRGB: every pixel opaque", op->rgba[7], 255);
    /* limits */
    static uint32_t big[300 * 300];
    check("past 256 px: not taken", wlicon_from_argb(big, 300, 1200, 1, 0) == NULL, 1);
    check("a stride shorter than a row: not taken", wlicon_from_argb(big, 64, 100, 1, 0) == NULL, 1);
    check("no pixels: not taken", wlicon_from_argb(NULL, 64, 256, 1, 0) == NULL, 1);
    size_t before = wlicon_bytes;
    /* the session's budget: 16 MB of 256 px icons is 64 of them */
    WlIcon *many[80];
    int n = 0;
    for (; n < 80; n++) if (!(many[n] = wlicon_from_argb(big, 256, 1024, 1, 0))) break;
    check("the budget stops new icons", n < 80 && wlicon_bytes <= WLICON_BUDGET, 1);
    for (int i = 0; i < n; i++) wlicon_unref(many[i]);
    check("freed: the budget back", (long)wlicon_bytes, (long)before);
    /* references: an icon object and a window share one copy */
    wlicon_ref(ic);
    wlicon_unref(ic);
    check("still held after one unref", ic->refs, 1);
    wlicon_unref(ic);
    wlicon_unref(op);
    check("all freed", (long)wlicon_bytes, 0);
    /* which buffer is kept */
    check("better: the first", wlicon_better(0, 1, 48, 1), 1);
    check("better: 128 over 64", wlicon_better(64, 1, 128, 1), 1);
    check("better: 64 over 48", wlicon_better(48, 1, 64, 1), 1);
    check("better: not 300 (past the limit)", wlicon_better(64, 1, 300, 1), 0);
    check("better: not 32 over 64", wlicon_better(64, 1, 32, 1), 0);
    check("better: 192 over 48 (64 from 128 against 80)", wlicon_better(48, 1, 192, 1), 1);
    printf("%s\n", fails ? "wlicon-test: FAILED" : "wlicon-test: ok");
    return fails ? 1 : 0;
}
