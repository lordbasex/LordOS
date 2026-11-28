/* spellproto.c — lord-spell's frames, built and parsed strictly (see spellproto.h). Pure: no I/O, no allocation. */
#include "spellproto.h"

#include <string.h>

int spell_utf8_ok(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t c = s[i];
        if (c == 0) return 0;
        if (c < 0x80) { i++; continue; }
        int k = c >= 0xf0 && c <= 0xf4 ? 3 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xc2 && c <= 0xdf ? 1 : -1;
        if (k < 0 || i + (size_t)k >= n) return 0;
        for (int j = 1; j <= k; j++) if ((s[i + j] & 0xc0) != 0x80) return 0;
        uint8_t d = s[i + 1];
        if ((c == 0xe0 && d < 0xa0) || (c == 0xed && d >= 0xa0) || (c == 0xf0 && d < 0x90) || (c == 0xf4 && d >= 0x90)) return 0;
        i += (size_t)k + 1;
    }
    return 1;
}

int spell_tag_ok(const char *tag) {
    if (!tag) return 0;
    size_t n = strlen(tag), i = 0;
    if (n >= SPELL_TAG_MAX) return 0;
    while (i < n && tag[i] >= 'a' && tag[i] <= 'z') i++;
    if (i < 2 || i > 3) return 0;
    if (i == n) return 1;
    if (tag[i++] != '_') return 0;
    size_t r = i;
    while (i < n && tag[i] >= 'A' && tag[i] <= 'Z') i++;
    return i == n && i - r >= 2 && i - r <= 3;
}

/* bytes allowed in a word: any non-ASCII of well-formed UTF-8 that isn't a C1 control, ASCII letters and digits, the
 * apostrophe, the hyphen and the full stop; spaces only where allowed (a suggestion), never two together or at an end */
static int text_ok(const char *w, size_t n, int spaces) {
    if (!w || n == 0 || n > SPELL_WORD_MAX || !spell_utf8_ok((const uint8_t *)w, n)) return 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)w[i];
        if (c == 0xc2 && i + 1 < n && (uint8_t)w[i + 1] < 0xa0) return 0;      /* U+0080..U+009F */
        if (c >= 0x80) continue;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '\'' || c == '-' || c == '.') continue;
        if (c == ' ' && spaces && i > 0 && i + 1 < n && w[i + 1] != ' ') continue;
        return 0;
    }
    return 1;
}
int spell_word_ok(const char *w, size_t n) { return text_ok(w, n, 0); }
int spell_sugg_ok(const char *w, size_t n) { return text_ok(w, n, 1); }

static int op_known(int op) { return op == 'L' || op == 'R' || op == 'C' || op == 'c' || op == 'G' || op == 'g'; }

int spell_head(const uint8_t h[SPELL_HEAD], int *op, size_t *len) {
    size_t l = (size_t)h[2] | (size_t)h[3] << 8;
    if (h[0] != SPELL_MAGIC || !op_known(h[1]) || l > SPELL_PAYLOAD_MAX) return -1;
    *op = h[1];
    *len = l;
    return 0;
}

int spell_frame(uint8_t *out, size_t cap, int op, const uint8_t *payload, size_t len) {
    if (!op_known(op) || len > SPELL_PAYLOAD_MAX || cap < SPELL_HEAD + len) return -1;
    out[0] = SPELL_MAGIC;
    out[1] = (uint8_t)op;
    out[2] = (uint8_t)(len & 0xff);
    out[3] = (uint8_t)(len >> 8);
    if (len) memmove(out + SPELL_HEAD, payload, len);
    return (int)(SPELL_HEAD + len);
}

/* ---- (length, bytes) items ---- */

static int put_item(uint8_t *out, size_t cap, size_t *at, const char *s, size_t n) {
    if (n > 255 || *at + 1 + n > cap) return -1;
    out[(*at)++] = (uint8_t)n;
    memcpy(out + *at, s, n);
    *at += n;
    return 0;
}
/* an item into a NUL-terminated buffer of max + 1 bytes, checked by ok */
static int get_item(const uint8_t *p, size_t len, size_t *at, char *dst, size_t max, int (*ok)(const char *, size_t)) {
    if (*at >= len) return -1;
    size_t n = p[(*at)++];
    if (n == 0 || n > max || *at + n > len) return -1;
    memcpy(dst, p + *at, n);
    dst[n] = 0;
    *at += n;
    return ok(dst, n) ? 0 : -1;
}
static int tag_item_ok(const char *s, size_t n) { return n < SPELL_TAG_MAX && spell_tag_ok(s); }

int spell_build_load(uint8_t *out, size_t cap, const SpellTag *tags, int n) {
    if (n < 1 || n > SPELL_LANGS_MAX || cap < 1) return -1;
    size_t at = 0;
    out[at++] = (uint8_t)n;
    for (int i = 0; i < n; i++) if (!spell_tag_ok(tags[i]) || put_item(out, cap, &at, tags[i], strlen(tags[i]))) return -1;
    return (int)at;
}
int spell_parse_load(const uint8_t *p, size_t len, SpellTag *tags, int *n) {
    if (len < 1 || p[0] < 1 || p[0] > SPELL_LANGS_MAX) return -1;
    size_t at = 1;
    int k = p[0];
    for (int i = 0; i < k; i++) if (get_item(p, len, &at, tags[i], SPELL_TAG_MAX - 1, tag_item_ok)) return -1;
    if (at != len) return -1;
    *n = k;
    return 0;
}

static int build_bytes(uint8_t *out, size_t cap, const uint8_t *v, int n, int max) {
    if (n < 1 || n > max || cap < (size_t)n + 1) return -1;
    out[0] = (uint8_t)n;
    for (int i = 0; i < n; i++) { if (v[i] > 1) return -1; out[1 + i] = v[i]; }
    return n + 1;
}
static int parse_bytes(const uint8_t *p, size_t len, int n, uint8_t *v) {
    if (n < 1 || len != (size_t)n + 1 || p[0] != n) return -1;
    for (int i = 0; i < n; i++) { if (p[1 + i] > 1) return -1; v[i] = p[1 + i]; }
    return 0;
}
int spell_build_status(uint8_t *out, size_t cap, const uint8_t *ok, int n) { return build_bytes(out, cap, ok, n, SPELL_LANGS_MAX); }
int spell_parse_status(const uint8_t *p, size_t len, int n, uint8_t *ok) { return n > SPELL_LANGS_MAX ? -1 : parse_bytes(p, len, n, ok); }
int spell_build_checked(uint8_t *out, size_t cap, const uint8_t *good, int n) { return build_bytes(out, cap, good, n, SPELL_BATCH_MAX); }
int spell_parse_checked(const uint8_t *p, size_t len, int n, uint8_t *good) { return n > SPELL_BATCH_MAX ? -1 : parse_bytes(p, len, n, good); }

static int mask_ok(unsigned m) { return m >= 1 && m < (1u << SPELL_LANGS_MAX); }

int spell_build_check(uint8_t *out, size_t cap, unsigned mask, const SpellWord *words, int n) {
    if (!mask_ok(mask) || n < 1 || n > SPELL_BATCH_MAX || cap < 2) return -1;
    size_t at = 0;
    out[at++] = (uint8_t)mask;
    out[at++] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(words[i]);
        if (!spell_word_ok(words[i], l) || put_item(out, cap, &at, words[i], l)) return -1;
    }
    return at > SPELL_PAYLOAD_MAX ? -1 : (int)at;
}
int spell_parse_check(const uint8_t *p, size_t len, unsigned *mask, SpellWord *words, int *n) {
    if (len < 2 || !mask_ok(p[0]) || p[1] < 1 || p[1] > SPELL_BATCH_MAX) return -1;
    size_t at = 2;
    int k = p[1];
    for (int i = 0; i < k; i++) if (get_item(p, len, &at, words[i], SPELL_WORD_MAX, spell_word_ok)) return -1;
    if (at != len) return -1;
    *mask = p[0];
    *n = k;
    return 0;
}

int spell_build_suggest(uint8_t *out, size_t cap, unsigned mask, const char *word) {
    size_t l = word ? strlen(word) : 0, at = 0;
    if (!mask_ok(mask) || !spell_word_ok(word, l) || cap < 1) return -1;
    out[at++] = (uint8_t)mask;
    return put_item(out, cap, &at, word, l) ? -1 : (int)at;
}
int spell_parse_suggest(const uint8_t *p, size_t len, unsigned *mask, SpellWord word) {
    if (len < 2 || !mask_ok(p[0])) return -1;
    size_t at = 1;
    if (get_item(p, len, &at, word, SPELL_WORD_MAX, spell_word_ok) || at != len) return -1;
    *mask = p[0];
    return 0;
}

int spell_build_suggestions(uint8_t *out, size_t cap, const SpellWord *s, int n) {
    if (n < 0 || n > SPELL_SUGG_MAX || cap < 1) return -1;
    size_t at = 0;
    out[at++] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(s[i]);
        if (!spell_sugg_ok(s[i], l) || put_item(out, cap, &at, s[i], l)) return -1;
    }
    return (int)at;
}
int spell_parse_suggestions(const uint8_t *p, size_t len, SpellWord *s, int *n) {
    if (len < 1 || p[0] > SPELL_SUGG_MAX) return -1;
    size_t at = 1;
    int k = p[0];
    for (int i = 0; i < k; i++) if (get_item(p, len, &at, s[i], SPELL_WORD_MAX, spell_sugg_ok)) return -1;
    if (at != len) return -1;
    *n = k;
    return 0;
}
