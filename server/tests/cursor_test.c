/* cursor_test.c — the pointer's size (src/cursor.c): it follows the display's scale up to 300 % (#47), times
 * the pointer size setting, and shake to find it still grows it. */
#include "../src/cursor.h"

#include <stdio.h>

static int fails, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fails++; printf("FAIL %s\n", what); } }

static int side(int shape, int pct) {
    const CursorImage *ci = cursor_image_pct(shape, pct);
    return ci ? ci->w : -1;
}

int main(void) {
    check(cursor_effective_pct(100) == 100 && side(CUR_DEFAULT, 100) == 32, "100 %: a 32 px arrow");
    check(cursor_effective_pct(200) == 200 && side(CUR_DEFAULT, 200) == 64, "200 %: 64 px");
    check(cursor_effective_pct(250) == 250 && side(CUR_DEFAULT, 250) == 80, "250 %: 80 px, larger than at 200 %");
    check(cursor_effective_pct(300) == 300 && side(CUR_DEFAULT, 300) == 96, "300 %: 96 px");
    check(cursor_effective_pct(400) == CURSOR_PCT_MAX, "above the largest display scale: capped");
    check(cursor_effective_pct(50) == 100, "below 100 %: as 100 %");
    const CursorImage *a = cursor_image_pct(CUR_TEXT, 300), *b = cursor_image_pct(CUR_TEXT, 300);
    check(a && a == b && a->h == 96, "every shape at 300 %, drawn once and kept");
    check(a && a->hx > 0 && a->hx < a->w && a->hy > 0 && a->hy < a->h, "its hotspot scales with it");

    /* the pointer size setting (Settings → Mouse & Trackpad) */
    cursor_set_size(200);
    check(cursor_effective_pct(100) == 200, "size 200 % on a 100 % display: 200 %");
    check(cursor_effective_pct(200) == CURSOR_PCT_MAX, "size 200 % on a 200 % display: the largest");
    cursor_set_size(75);
    check(cursor_effective_pct(100) == 75 && side(CUR_DEFAULT, 100) == 24, "size 75 %: 24 px");
    check(cursor_effective_pct(300) == 225, "size 75 % on a 300 % display: 225 %");
    cursor_set_size(100);

    /* shake to find it */
    unsigned serial = cursor_size_serial();
    check(cursor_set_boost(1) && cursor_size_serial() != serial, "a shake changes the size");
    check(cursor_effective_pct(100) == 200, "shaken at 100 %: 200 %");
    check(cursor_effective_pct(150) == 200, "shaken at 150 %: 200 %");
    check(cursor_effective_pct(200) == CURSOR_PCT_MAX, "shaken at 200 %: the largest, so it still grows");
    check(cursor_effective_pct(300) == CURSOR_PCT_MAX, "shaken at 300 %: stays the largest");
    cursor_set_boost(0.5);
    check(cursor_effective_pct(100) == 150, "half way: half way to 200 %");
    check(cursor_effective_pct(200) == 250, "half way from 200 %: 250 %");
    check(side(CUR_DEFAULT, 200) == 80, "and drawn at that size");
    cursor_set_boost(0);
    check(cursor_effective_pct(300) == 300 && side(CUR_DEFAULT, 300) == 96, "back to normal");

    printf("cursor-test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
