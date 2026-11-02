/* imgverify.c — see imgverify.h. */
#include "imgverify.h"
#include "../pkg/crypto.h"

#include <stdlib.h>
#include <string.h>

static int b64v(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int img_sig_verify(const uint8_t *sums, size_t n, const char *sig, size_t sn, const uint8_t pub[32]) {
    if (!sums || !sig || n == 0 || n > IMG_SUMS_MAX || sn > IMG_SIG_MAX) return -1;
    while (sn && (sig[sn - 1] == '\n' || sig[sn - 1] == '\r')) sn--;
    if (sn != 88 || sig[86] != '=' || sig[87] != '=') return -1;      /* 64 bytes = 86 chars + "==" */
    uint8_t raw[66];
    size_t o = 0;
    for (size_t i = 0; i < 88; i += 4) {
        int v[4];
        for (int k = 0; k < 4; k++) {
            unsigned char c = (unsigned char)sig[i + (size_t)k];
            v[k] = c == '=' && i == 84 && k >= 2 ? 0 : b64v(c);
            if (v[k] < 0) return -1;
        }
        uint32_t t = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
        raw[o++] = (uint8_t)(t >> 16);
        raw[o++] = (uint8_t)(t >> 8);
        raw[o++] = (uint8_t)t;
    }
    /* the two padding characters leave 64 bytes; the extra ones must be zero bits */
    if (raw[64] || raw[65]) return -1;
    return ed25519_verify(pub, raw, sums, n) == 1 ? 0 : -1;
}

int img_valid_name(const char *name) {
    size_t n = strlen(name);
    if (n < 1 || n > 128 || name[0] == '.' || name[0] == '-') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' || c == '~' || c == '-')) return 0;
    }
    if (strstr(name, "..")) return 0;
    return 1;
}

static int ends(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    return n > m && !strcmp(s + n - m, suf);
}

int img_kind(const char *name) {
    if (ends(name, ".img.xz")) return 2;
    if (ends(name, ".img") || ends(name, ".iso")) return 1;
    return 0;
}

int img_valid_tag(const char *t) {
    size_t n = strlen(t);
    if (n < 6 || n > 32 || t[0] != 'v') return 0;
    const char *p = t + 1;
    for (int part = 0; part < 4; part++) {      /* three numbers, or four for a point release (v0.8.4.1-beta) */
        int d = 0;
        while (*p >= '0' && *p <= '9') { p++; d++; }
        if (!d || d > 4) return 0;
        if (part < 2) { if (*p != '.') return 0; p++; }
        else if (part == 2) { if (*p != '.') break; p++; }
    }
    if (!*p) return 1;
    if (*p != '-' || !p[1]) return 0;
    for (p++; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.')) return 0;
    return 1;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int img_sums_find(const char *s, size_t n, const char *name, uint8_t out[32]) {
    if (!s || n > IMG_SUMS_MAX) return -2;
    size_t nl = strlen(name);
    int found = 0;
    for (size_t i = 0; i < n;) {
        size_t e = i;
        while (e < n && s[e] != '\n') e++;
        size_t len = e - i;
        const char *l = s + i;
        i = e + 1;
        if (len && l[len - 1] == '\r') len--;
        if (!len) continue;
        if (len < 67) return -2;
        uint8_t h[32];
        for (int k = 0; k < 32; k++) {
            int a = hexv(l[2 * k]), b = hexv(l[2 * k + 1]);
            if (a < 0 || b < 0) return -2;
            h[k] = (uint8_t)(a << 4 | b);
        }
        if (l[64] != ' ' || (l[65] != ' ' && l[65] != '*')) return -2;
        const char *fn = l + 66;
        size_t fl = len - 66;
        if (fl == nl && !memcmp(fn, name, nl)) {
            if (found) return -2;
            memcpy(out, h, 32);
            found = 1;
        }
    }
    return found ? 0 : -1;
}

/* ---- CRC32 (IEEE, as xz uses) ---- */

uint32_t img_crc32(uint32_t crc, const void *p, size_t n) {
    static uint32_t tab[256];
    static int init;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        init = 1;
    }
    const uint8_t *b = p;
    crc = ~crc;
    while (n--) crc = tab[(crc ^ *b++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/* ---- .xz index ---- */

static int varint(const uint8_t **p, const uint8_t *end, uint64_t *v) {
    *v = 0;
    for (int i = 0; i < 9; i++) {
        if (*p >= end) return -1;
        uint8_t b = *(*p)++;
        *v |= (uint64_t)(b & 0x7f) << (7 * i);
        if (!(b & 0x80)) return (i && !b) ? -1 : 0;      /* no non-minimal encoding */
    }
    return -1;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int img_xz_size(ImgRead rd, void *ud, uint64_t fsize, uint64_t *out) {
    uint64_t pos = fsize, total = 0;
    int streams = 0;
    if (fsize < 32 || fsize % 4) return -1;
    while (pos > 0) {
        uint8_t f[12];
        if (pos < 12 || rd(ud, pos - 12, f, 12)) return -1;
        /* stream padding (zeros, a multiple of 4) after a stream; a footer always ends in "YZ" */
        if (!memcmp(f + 8, "\0\0\0\0", 4)) { pos -= 4; continue; }
        if (f[10] != 'Y' || f[11] != 'Z') return -1;
        if (img_crc32(0, f + 4, 6) != le32(f)) return -1;
        uint64_t isize = ((uint64_t)le32(f + 4) + 1) * 4;
        if (isize > 1 << 24 || isize + 24 > pos) return -1;           /* 16 MiB of index at most */
        uint8_t *ix = malloc((size_t)isize);
        if (!ix) return -1;
        if (rd(ud, pos - 12 - isize, ix, (size_t)isize)) { free(ix); return -1; }
        const uint8_t *p = ix, *end = ix + isize - 4;
        uint64_t nrec, blocks = 0, usum = 0;
        int ok = *p++ == 0 && varint(&p, end, &nrec) == 0 && nrec <= (1u << 24);
        for (uint64_t r = 0; ok && r < nrec; r++) {
            uint64_t un, us;
            ok = varint(&p, end, &un) == 0 && varint(&p, end, &us) == 0 && un > 0;
            if (!ok) break;
            blocks += (un + 3) & ~3ull;
            if (us > UINT64_MAX - usum) { ok = 0; break; }
            usum += us;
        }
        while (ok && (size_t)(p - ix) % 4) ok = *p++ == 0;
        ok = ok && p == end && img_crc32(0, ix, isize - 4) == le32(end);
        free(ix);
        if (!ok) return -1;
        /* the stream header, whose flags must match the footer's */
        uint64_t ssize = 12 + blocks + isize + 12;
        if (ssize > pos) return -1;
        uint8_t h[12];
        if (rd(ud, pos - ssize, h, 12)) return -1;
        if (memcmp(h, "\xfd" "7zXZ\0", 6) || h[6] != f[8] || h[7] != f[9] || img_crc32(0, h + 6, 2) != le32(h + 8)) return -1;
        if (usum > UINT64_MAX - total) return -1;
        total += usum;
        pos -= ssize;
        streams++;
        if (streams > 1024) return -1;
    }
    if (!streams) return -1;
    *out = total;
    return 0;
}
