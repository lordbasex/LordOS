/* textinput_test.c — byte offsets and clamps for zwp_text_input_v3 (src/wl/tiutf8.c): what LordOS's
 * input method sends to a Linux app, and what is kept of the text, offsets and rectangle it sends. */
#include "../src/wl/tiutf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

int main(void) {
    size_t len;
    int b, e, cur, anc;
    uint32_t db, da;
    char out[TIU_SURROUNDING + 1];

    /* valid UTF-8 prefixes: ASCII, 2-, 3- and 4-byte characters whole; garbage cut where it starts */
    CHECK(tiu_valid_prefix("abc", 10) == 3);
    CHECK(tiu_valid_prefix("한국", 10) == 6);
    CHECK(tiu_valid_prefix("é😀", 10) == 6);
    CHECK(tiu_valid_prefix("한국", 4) == 3);                    /* never half a character */
    CHECK(tiu_valid_prefix("ab\xff" "cd", 10) == 2);
    CHECK(tiu_valid_prefix("a\xc0\xaf", 10) == 1);              /* overlong '/' */
    CHECK(tiu_valid_prefix("a\xe0\x80\xaf", 10) == 1);          /* overlong, 3 bytes */
    CHECK(tiu_valid_prefix("a\xed\xa0\x80", 10) == 1);          /* a surrogate */
    CHECK(tiu_valid_prefix("a\xf4\x90\x80\x80", 10) == 1);      /* past U+10FFFF */
    CHECK(tiu_valid_prefix("a\xe2\x82", 10) == 1);              /* cut short */
    CHECK(tiu_valid_prefix(NULL, 10) == 0);

    /* boundaries */
    CHECK(tiu_boundary("한", 3, 0) && tiu_boundary("한", 3, 3));
    CHECK(!tiu_boundary("한", 3, 1) && !tiu_boundary("한", 3, 2));
    CHECK(!tiu_boundary("한", 3, -1) && !tiu_boundary("한", 3, 4));

    /* preedit cursor: at the end, or over the focused segment (Japanese conversion) */
    tiu_preedit("にほんご", 511, 0, 0, &len, &b, &e);
    CHECK(len == 12 && b == 12 && e == 12);
    tiu_preedit("日本語を", 511, 3, 9, &len, &b, &e);
    CHECK(len == 12 && b == 3 && e == 9);
    tiu_preedit("日本語", 511, 1, 9, &len, &b, &e);                 /* not a boundary: at the end */
    CHECK(b == 9 && e == 9);
    tiu_preedit("日本語", 511, 3, 40, &len, &b, &e);                /* past the end */
    CHECK(b == 9 && e == 9);
    tiu_preedit("日本語", 511, 6, 3, &len, &b, &e);                 /* reversed */
    CHECK(b == 9 && e == 9);
    tiu_preedit("日本語", 7, 0, 3, &len, &b, &e);                   /* cut to the limit, still whole */
    CHECK(len == 6 && b == 0 && e == 3);
    tiu_preedit("", 511, 0, 0, &len, &b, &e);
    CHECK(len == 0 && b == 0 && e == 0);

    /* surrounding text: kept with its cursor and anchor when they are boundaries */
    CHECK(tiu_surrounding("hola 世界", 5, 8, out, sizeof out, &cur, &anc) == 1);
    CHECK(!strcmp(out, "hola 世界") && cur == 5 && anc == 8);
    CHECK(tiu_surrounding("hola 世界", 6, 6, out, sizeof out, &cur, &anc) == 0 && !out[0]);   /* inside 世 */
    CHECK(tiu_surrounding("abc", -1, 0, out, sizeof out, &cur, &anc) == 0);
    CHECK(tiu_surrounding("abc", 0, 4, out, sizeof out, &cur, &anc) == 0);
    CHECK(tiu_surrounding("ab\xff" "cd", 2, 2, out, sizeof out, &cur, &anc) == 1 && !strcmp(out, "ab"));
    CHECK(tiu_surrounding("ab\xff" "cd", 4, 4, out, sizeof out, &cur, &anc) == 0);           /* past the garbage */
    CHECK(tiu_surrounding(NULL, 0, 0, out, sizeof out, &cur, &anc) == 0);
    /* huge: cut to TIU_SURROUNDING on a boundary; a cursor past the cut is dropped */
    char *big = malloc(3 * 5000 + 1);
    for (int i = 0; i < 5000; i++) memcpy(big + 3 * i, "語", 3);
    big[3 * 5000] = 0;
    CHECK(tiu_surrounding(big, 30, 30, out, sizeof out, &cur, &anc) == 1);
    CHECK(strlen(out) == 3999 && cur == 30);
    CHECK(tiu_surrounding(big, 6000, 6000, out, sizeof out, &cur, &anc) == 0);
    CHECK(tiu_surrounding(big, 0, 0, out, 8, &cur, &anc) == 1 && !strcmp(out, "語語"));
    free(big);

    /* delete around the cursor: cut to the text and to whole characters */
    tiu_delete("ab한cd", 7, 5, 3, 1, &db, &da);
    CHECK(db == 3 && da == 1);
    tiu_delete("ab한cd", 7, 5, 2, 0, &db, &da);                    /* half of 한: nothing of it */
    CHECK(db == 0 && da == 0);
    tiu_delete("ab한cd", 7, 2, 0, 2, &db, &da);
    CHECK(db == 0 && da == 0);
    tiu_delete("ab한cd", 7, 2, 100, 100, &db, &da);
    CHECK(db == 2 && da == 5);
    tiu_delete("ab한cd", 7, 3, 1, 1, &db, &da);                    /* a cursor inside a character */
    CHECK(db == 0 && da == 0);
    tiu_delete("abc", 3, 1, 0xffffffffu, 0xffffffffu, &db, &da);
    CHECK(db == 1 && da == 2);

    /* rectangles */
    int r[4], in[4];
    tiu_rect(10, 20, 2, 18, r);
    CHECK(r[0] == 10 && r[1] == 20 && r[2] == 2 && r[3] == 18);
    tiu_rect(-2147483647 - 1, 2147483647, -5, 2147483647, r);
    CHECK(r[0] == -TIU_COORD && r[1] == TIU_COORD && r[2] == 0 && r[3] == TIU_COORD);
    tiu_rect_in(r, 800, 600, in);
    CHECK(in[0] == 0 && in[1] == 600 && in[2] == 0 && in[3] == 0);
    int rr[4] = { 790, 100, 40, 20 };
    tiu_rect_in(rr, 800, 600, in);
    CHECK(in[0] == 790 && in[1] == 100 && in[2] == 10 && in[3] == 20);
    tiu_rect_in(rr, -1, -1, in);
    CHECK(in[0] == 0 && in[1] == 0 && in[2] == 0 && in[3] == 0);

    if (fails) { printf("textinput: %d failures\n", fails); return 1; }
    printf("textinput: ok\n");
    return 0;
}
