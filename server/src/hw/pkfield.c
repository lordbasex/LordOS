/* pkfield.c — see pkfield.h. */
#define _GNU_SOURCE
#include "pkfield.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int pkf_open(PkField *f) {
    long page = sysconf(_SC_PAGESIZE);
    if (page < PKF_MAX) return -1;
    void *p = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    if (mlock(p, (size_t)page) || madvise(p, (size_t)page, MADV_DONTDUMP) || madvise(p, (size_t)page, MADV_DONTFORK)) {
        munmap(p, (size_t)page);
        return -1;
    }
    f->buf = p;
    f->len = 0;
    return 0;
}

void pkf_wipe(PkField *f) {
    if (f->buf) explicit_bzero(f->buf, PKF_MAX);
    f->len = 0;
}

/* Length of the UTF-8 sequence at s and its code point; 0 if invalid. */
static size_t utf8_at(const unsigned char *s, uint32_t *cp) {
    unsigned c = s[0];
    size_t n = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
    if (!n) return 0;
    uint32_t v = n == 1 ? c : c & (0xffu >> (n + 1));
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) return 0;
        v = v << 6 | (s[i] & 0x3f);
    }
    static const uint32_t least[5] = { 0, 0, 0x80, 0x800, 0x10000 };
    if (v < least[n] || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return 0;
    *cp = v;
    return n;
}

int pkf_add(PkField *f, const char *text) {
    size_t n = strlen(text);
    if (!f->buf || !n || f->len + n >= PKF_MAX) return 0;
    for (const unsigned char *s = (const unsigned char *)text; *s; ) {
        uint32_t cp;
        size_t k = utf8_at(s, &cp);
        if (!k || cp < 0x20 || cp == 0x7f) return 0;
        s += k;
    }
    memcpy(f->buf + f->len, text, n);
    f->len += n;
    return 1;
}

void pkf_back(PkField *f) {
    while (f->len) {
        unsigned char c = (unsigned char)f->buf[--f->len];
        f->buf[f->len] = 0;
        if ((c & 0xc0) != 0x80) break;
    }
}

size_t pkf_chars(const PkField *f) {
    size_t n = 0;
    for (size_t i = 0; i < f->len; i++) if (((unsigned char)f->buf[i] & 0xc0) != 0x80) n++;
    return n;
}

PkLine pk_line_kind(const char *l) {
    if (!strncmp(l, "PAM_PROMPT_ECHO_OFF", 19)) return PKL_ASK_HIDDEN;
    if (!strncmp(l, "PAM_PROMPT_ECHO_ON", 18)) return PKL_ASK_SHOWN;
    if (!strncmp(l, "PAM_ERROR_MSG", 13)) return PKL_ERROR;
    if (!strncmp(l, "PAM_TEXT_INFO", 13)) return PKL_INFO;
    if (!strncmp(l, "SUCCESS", 7)) return PKL_SUCCESS;
    if (!strncmp(l, "FAILURE", 7)) return PKL_FAILURE;
    return PKL_OTHER;
}

static int hidden_cp(uint32_t c) {
    return c < 0x20 || (c >= 0x7f && c < 0xa0) || c == 0xad || c == 0x61c || c == 0x180e ||
           (c >= 0x200b && c <= 0x200f) || (c >= 0x2028 && c <= 0x202e) ||
           (c >= 0x2060 && c <= 0x206f) || c == 0xfeff || (c >= 0xfff0 && c <= 0xfffb);
}

void pk_clean(char *out, size_t n, const char *in) {
    size_t o = 0;
    for (const unsigned char *s = (const unsigned char *)(in ? in : ""); *s; ) {
        uint32_t cp;
        size_t k = utf8_at(s, &cp);
        if (!k) { s++; continue; }
        if (!hidden_cp(cp)) {
            if (o + k + 1 > n) break;
            memcpy(out + o, s, k);
            o += k;
        }
        s += k;
    }
    if (n) out[o] = 0;
}

unsigned long long pk_stat_start(const char *stat) {
    const char *p = strrchr(stat, ')');
    if (!p) return 0;
    p++;                                            /* the space before field 3 */
    for (int field = 3; field < 22; field++) {
        p = strchr(p + 1, ' ');
        if (!p) return 0;
    }
    return strtoull(p + 1, NULL, 10);
}
