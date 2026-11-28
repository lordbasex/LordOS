/* logclean.c — text from a community theme on its way to the session log (logclean.h) */
#include "logclean.h"

#include <string.h>

/* the length of the UTF-8 sequence at p (n bytes left) and its code point, or 0 for a malformed one */
static size_t utf8_at(const unsigned char *p, size_t n, unsigned *cp) {
    unsigned c = p[0];
    size_t l = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
    if (!l || l > n) return 0;
    unsigned v = l == 1 ? c : l == 2 ? c & 0x1f : l == 3 ? c & 0x0f : c & 0x07;
    for (size_t i = 1; i < l; i++) { if ((p[i] & 0xc0) != 0x80) return 0; v = (v << 6) | (p[i] & 0x3f); }
    static const unsigned minv[5] = { 0, 0, 0x80, 0x800, 0x10000 };
    if (v < minv[l] || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return 0;
    *cp = v;
    return l;
}

static int bad_cp(unsigned v) {
    return v < 0x20 || v == 0x7f || (v >= 0x80 && v <= 0x9f) || v == 0x2028 || v == 0x2029 || (v >= 0x202a && v <= 0x202e) ||
           (v >= 0x2066 && v <= 0x2069);
}

size_t logclean_line(const char *in, size_t n, char *out, size_t cap) {
    if (!out || cap < 5) return 0;
    size_t o = 0, i = 0;
    const unsigned char *p = (const unsigned char *)(in ? in : "");
    if (!in) n = 0;
    while (i < n) {
        unsigned cp = 0;
        size_t l = utf8_at(p + i, n - i, &cp);
        if (!l || bad_cp(cp)) {
            if (o + 1 >= cap - 4) break;
            out[o++] = '?';
            i += l ? l : 1;
            continue;
        }
        if (o + l >= cap - 4) {           /* cut: an ellipsis says so */
            memcpy(out + o, "\xe2\x80\xa6", 3);
            o += 3;
            break;
        }
        memcpy(out + o, p + i, l);
        o += l;
        i += l;
    }
    out[o] = 0;
    return o;
}

void logclean_print(FILE *f, const char *prefix, const char *s) {
    char line[LOGCLEAN_LINE_MAX];
    const char *p = s ? s : "";
    int k = 0;
    for (;;) {
        size_t l = strcspn(p, "\n");
        if (k == LOGCLEAN_LINES_MAX) {
            int more = 0;
            for (const char *q = p; *q; q++) if (*q == '\n') more++;
            fprintf(f, "%s(%d more lines)\n", prefix ? prefix : "", more + 1);
            return;
        }
        logclean_line(p, l, line, sizeof line);
        fprintf(f, "%s%s\n", prefix ? prefix : "", line);
        k++;
        if (!p[l]) return;
        p += l + 1;
    }
}
