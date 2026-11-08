/* saytext.c — say's text: cleaning what a terminal or a file gives it, and splitting it into sentences, so the
 * first one plays while the next is synthesized (saytext.h). */
#include "saytext.h"

#include <stdint.h>
#include <string.h>

/* length of a valid UTF-8 sequence at p (up to n bytes), or 0 */
static size_t utf8_len(const unsigned char *p, size_t n) {
    unsigned c = p[0];
    size_t k = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (!k || k > n || (k == 2 && c < 0xC2) || c > 0xF4) return 0;
    for (size_t i = 1; i < k; i++) if ((p[i] & 0xC0) != 0x80) return 0;
    if (k == 3 && ((c == 0xE0 && p[1] < 0xA0) || (c == 0xED && p[1] >= 0xA0))) return 0;     /* overlong, surrogates */
    if (k == 4 && ((c == 0xF0 && p[1] < 0x90) || (c == 0xF4 && p[1] >= 0x90))) return 0;
    return k;
}

size_t say_clean(char *s, size_t len) {
    unsigned char *p = (unsigned char *)s;
    size_t o = 0, nl = 0;
    int space = 0;
    for (size_t i = 0; i < len;) {
        unsigned c = p[i];
        if (c == 0x1b) {                                  /* ESC: CSI "[...final", OSC "]...BEL|ST", else 2 bytes */
            if (i + 1 < len && p[i + 1] == '[') {
                i += 2;
                while (i < len && !(p[i] >= 0x40 && p[i] <= 0x7e)) i++;
                i++;
            } else if (i + 1 < len && p[i + 1] == ']') {
                i += 2;
                while (i < len && p[i] != 7 && !(p[i] == 0x1b && i + 1 < len && p[i + 1] == '\\')) i++;
                i += i < len && p[i] == 0x1b ? 2 : 1;
            } else i += 2;
            continue;
        }
        if (c == '\n') { nl++; space = 0; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { space = 1; i++; continue; }
        if (c < 0x20 || c == 0x7f) { i++; continue; }
        size_t k = utf8_len(p + i, len - i);
        if (!k) { i++; continue; }
        if (k == 2 && c == 0xC2 && p[i + 1] < 0xA0) { i += 2; continue; }   /* C1 controls */
        if (o) {
            if (nl) { size_t t = nl > 2 ? 2 : nl; while (t--) p[o++] = '\n'; }
            else if (space) p[o++] = ' ';
        }
        nl = 0;
        space = 0;
        memmove(p + o, p + i, k);
        o += k;
        i += k;
    }
    if (o < len) p[o] = 0;
    return o;
}

/* the sentence ends after s[i] (the last byte of . ! ? or …) when a space or a line break follows */
static int ends_sentence(const unsigned char *p, size_t i, size_t len) {
    int mark = p[i] == '.' || p[i] == '!' || p[i] == '?' ||
               (i >= 2 && p[i - 2] == 0xE2 && p[i - 1] == 0x80 && p[i] == 0xA6);              /* … */
    return mark && (i + 1 == len || p[i + 1] == ' ' || p[i + 1] == '\n');
}

size_t say_next(const char *s, size_t len, size_t max, size_t *skip) {
    const unsigned char *p = (const unsigned char *)s;
    size_t a = 0;
    while (a < len && (p[a] == ' ' || p[a] == '\n')) a++;
    *skip = a;
    if (a == len) return 0;
    if (max < 8) max = 8;
    size_t comma = 0, space = 0;
    for (size_t i = a; i < len; i++) {
        size_t n = i - a + 1;
        if (n > max) break;
        if (ends_sentence(p, i, len)) return n;
        if (p[i] == '\n' && i + 1 < len && p[i + 1] == '\n') return i - a;       /* a blank line */
        if (p[i] == '\n' && i > a && (p[i - 1] == ':' || p[i - 1] == ';')) return i - a;
        if ((p[i] == ',' || p[i] == ';' || p[i] == ':') && i + 1 < len && (p[i + 1] == ' ' || p[i + 1] == '\n')) comma = n;
        if (p[i] == ' ' || p[i] == '\n') space = i - a;
    }
    if (len - a <= max) return len - a;
    if (comma > max / 3) return comma;
    if (space > 0) return space;
    /* one long word: cut before max on a character boundary */
    size_t n = max;
    while (n > 1 && (p[a + n] & 0xC0) == 0x80) n--;
    return n;
}
