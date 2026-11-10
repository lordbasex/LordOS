/* xwscale_test.c — the size of an X pixel for X11 apps on HiDPI displays (src/wl/xwscale.c): the scale
 * picked, the conversions between logical and X pixels, and the RESOURCE_MANAGER text. */
#include "../src/wl/xwscale.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

int main(void) {
    /* the largest display, only when the apps scale themselves */
    int one[] = { 100 }, mixed[] = { 100, 175, 150 }, big[] = { 400 }, odd[] = { 0, -5 };
    CHECK(xws_pick(one, 1, 1) == 100);
    CHECK(xws_pick(mixed, 3, 1) == 175);
    CHECK(xws_pick(mixed, 3, 0) == 100);        /* scaled by LordOS */
    CHECK(xws_pick(big, 1, 1) == XWS_MAX);
    CHECK(xws_pick(odd, 2, 1) == 100);
    CHECK(xws_pick(NULL, 0, 1) == 100);
    CHECK(xws_pick(mixed, 0, 1) == 100);        /* no display yet */

    /* logical → X and back */
    CHECK(xws_to_x(400, 200) == 800);
    CHECK(xws_from_x(800, 200) == 400);
    CHECK(xws_to_x(401, 150) == 602);           /* 601.5, half away from zero */
    CHECK(xws_from_x(602, 150) == 401);
    CHECK(xws_to_x(-401, 150) == -602);         /* a display left of the main one */
    CHECK(xws_from_x(-602, 150) == -401);
    CHECK(xws_to_x(123, 100) == 123 && xws_from_x(123, 100) == 123);
    CHECK(xws_to_x(10, 50) == 10);              /* below 100 %: clamped to 100 */
    CHECK(xws_to_x(2000000000, 300) == 2147483647);   /* saturates, never wraps */
    for (int pct = 100; pct <= XWS_MAX; pct += 25)
        for (int v = -5000; v <= 5000; v += 7) {
            if (xws_from_x(xws_to_x(v, pct), pct) != v) { printf("FAIL round trip %d at %d %%\n", v, pct); fails++; break; }
        }
    CHECK(xws_to_xf(10.5, 200) == 21.0);
    CHECK(xws_from_xf(21.0, 200) == 10.5);
    CHECK(xws_to_xf(4.0, 125) == 5.0);

    /* Xft.dpi */
    CHECK(xws_dpi(100) == 96);
    CHECK(xws_dpi(125) == 120);
    CHECK(xws_dpi(150) == 144);
    CHECK(xws_dpi(175) == 168);
    CHECK(xws_dpi(200) == 192);
    char buf[64];
    CHECK(xws_resources(200, buf, sizeof buf) == strlen("Xft.dpi:\t192\n") && !strcmp(buf, "Xft.dpi:\t192\n"));
    CHECK(xws_resources(175, buf, sizeof buf) > 0 && !strcmp(buf, "Xft.dpi:\t168\n"));
    CHECK(xws_resources(100, buf, sizeof buf) == 0 && buf[0] == 0);   /* nothing set: X's default */
    CHECK(xws_resources(200, buf, 8) == 0 && buf[0] == 0);           /* doesn't fit: nothing */
    CHECK(xws_resources(200, NULL, 0) == 0);

    if (fails) { printf("xwscale: %d failure(s)\n", fails); return 1; }
    printf("xwscale: ok\n");
    return 0;
}
