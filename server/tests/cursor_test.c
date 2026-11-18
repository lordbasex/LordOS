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

#include <stdarg.h>
static void checkf(int ok, const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    check(ok, msg);
}

/* A theme's accent arrow (cursor_set_accent, Lord Workspaces 0.8.9): the default pointer takes the accent as its fill
 * and its own shape; the text cursor stays LordOS's; off brings the black arrow back. */
static void test_cursor_accent(void) {
    cursor_set_accent(0, 0);
    const CursorImage *black = cursor_image_pct(CUR_DEFAULT, 100);
    int black_w = black ? black->w : 0;
    uint32_t text_before = 0;
    const CursorImage *t0 = cursor_image_pct(CUR_TEXT, 100);
    for (int i = 0; t0 && i < t0->w * t0->h; i++) text_before += t0->px[i] & 0xFF;
    checkf(cursor_set_accent(1, 0xE2711A) == 1, "the accent arrow is set");
    checkf(cursor_set_accent(1, 0xE2711A) == 0, "setting the same accent again changes nothing");
    const CursorImage *a = cursor_image_pct(CUR_DEFAULT, 100);
    int tinted = 0;
    for (int i = 0; a && i < a->w * a->h; i++) {
        uint32_t p = a->px[i], A = p >> 24, R = p >> 16 & 255, B = p & 255;
        if (A == 255 && R > 200 && B < 60) tinted++;       /* opaque orange: the fill */
    }
    checkf(a && tinted > 20, "the accent arrow is filled with the accent (%d orange pixels)", tinted);
    checkf(a && a->w == black_w, "the accent arrow has the arrow's size (%d, %d)", a ? a->w : 0, black_w);
    uint32_t text_after = 0;
    const CursorImage *t1 = cursor_image_pct(CUR_TEXT, 100);
    for (int i = 0; t1 && i < t1->w * t1->h; i++) text_after += t1->px[i] & 0xFF;
    checkf(text_after == text_before, "the text cursor is not tinted");
    cursor_set_accent(0, 0);
    const CursorImage *b = cursor_image_pct(CUR_DEFAULT, 100);
    int orange = 0;
    for (int i = 0; b && i < b->w * b->h; i++) { uint32_t p = b->px[i]; if ((p >> 16 & 255) > 200 && (p & 255) < 60) orange++; }
    checkf(b && orange == 0, "off: the black arrow is back (%d orange pixels)", orange);
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

    test_cursor_accent();
    printf("cursor-test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
