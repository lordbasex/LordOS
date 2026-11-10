/* tiutf8.c — byte offsets and clamps for zwp_text_input_v3 (see tiutf8.h). No Wayland here: the
 * test (tests/textinput_test.c) links it alone. */
#include "tiutf8.h"

#include <string.h>

/* Length of the valid UTF-8 sequence at s (at most n bytes), 0 if it isn't one. */
static size_t seq_len(const unsigned char *s, size_t n) {
    unsigned c = s[0];
    if (c < 0x80) return 1;
    size_t k = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (!k || k > n) return 0;
    for (size_t i = 1; i < k; i++) if ((s[i] & 0xc0) != 0x80) return 0;
    if (c == 0xe0 && s[1] < 0xa0) return 0;         /* overlong */
    if (c == 0xed && s[1] > 0x9f) return 0;         /* surrogates */
    if (c == 0xf0 && s[1] < 0x90) return 0;         /* overlong */
    if (c == 0xf4 && s[1] > 0x8f) return 0;         /* past U+10FFFF */
    return k;
}

size_t tiu_valid_prefix(const char *s, size_t max) {
    if (!s) return 0;
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0;
    while (i < max && u[i]) {
        size_t k = seq_len(u + i, max - i);
        if (!k) break;
        i += k;
    }
    return i;
}

int tiu_boundary(const char *s, size_t len, int64_t off) {
    if (off < 0 || (uint64_t)off > len) return 0;
    return (size_t)off == len || ((unsigned char)s[off] & 0xc0) != 0x80;
}

void tiu_preedit(const char *s, size_t max, int fb, int fe, size_t *len, int *begin, int *end) {
    size_t n = tiu_valid_prefix(s, max);
    *len = n;
    if (fb < fe && tiu_boundary(s, n, fb) && tiu_boundary(s, n, fe)) { *begin = fb; *end = fe; return; }
    *begin = *end = (int)n;
}

int tiu_surrounding(const char *text, int32_t cursor, int32_t anchor, char *out, size_t size, int *cur, int *anc) {
    *cur = *anc = 0;
    if (!size) return 0;
    out[0] = 0;
    if (!text) return 0;
    size_t max = size - 1 < TIU_SURROUNDING ? size - 1 : TIU_SURROUNDING;
    size_t n = tiu_valid_prefix(text, max);
    if (!tiu_boundary(text, n, cursor) || !tiu_boundary(text, n, anchor)) return 0;
    memcpy(out, text, n);
    out[n] = 0;
    *cur = cursor;
    *anc = anchor;
    return 1;
}

void tiu_delete(const char *s, size_t len, int cursor, uint32_t before, uint32_t after, uint32_t *b, uint32_t *a) {
    *b = *a = 0;
    if (!tiu_boundary(s, len, cursor)) return;
    size_t start = before >= (uint32_t)cursor ? 0 : (size_t)cursor - before;
    size_t stop = after >= len - (size_t)cursor ? len : (size_t)cursor + after;
    while (start < (size_t)cursor && !tiu_boundary(s, len, (int64_t)start)) start++;
    while (stop > (size_t)cursor && !tiu_boundary(s, len, (int64_t)stop)) stop--;
    *b = (uint32_t)((size_t)cursor - start);
    *a = (uint32_t)(stop - (size_t)cursor);
}

static int clampi(int32_t v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : (int)v; }

void tiu_rect(int32_t x, int32_t y, int32_t w, int32_t h, int out[4]) {
    out[0] = clampi(x, -TIU_COORD, TIU_COORD);
    out[1] = clampi(y, -TIU_COORD, TIU_COORD);
    out[2] = clampi(w, 0, TIU_COORD);
    out[3] = clampi(h, 0, TIU_COORD);
}

void tiu_rect_in(const int r[4], int lw, int lh, int out[4]) {
    if (lw < 0) lw = 0;
    if (lh < 0) lh = 0;
    out[0] = clampi(r[0], 0, lw);
    out[1] = clampi(r[1], 0, lh);
    out[2] = clampi(r[2], 0, lw - out[0]);
    out[3] = clampi(r[3], 0, lh - out[1]);
}
