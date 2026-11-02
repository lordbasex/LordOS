/* xz.c — decompressors for Debian's indexes: our own xz (LZMA2) and gzip (stb_image's
 * inflate, which is already in axia-server for PNGs). All in memory: repository files are
 * a few tens of MB.
 *
 * xz: container with one or more streams, blocks with a single LZMA2 filter (what Debian's
 * repositories use; no BCJ or delta). Each block's check (CRC32 or CRC64) is verified, even
 * though the compressed file already comes verified by the signed InRelease's SHA-256.
 * The LZMA decoder follows the LZMA specification (Igor Pavlov) and the structure of xz-embedded (public domain). */
#include "xz.h"
#include "stb_image.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *in, *in_end;
    uint32_t range, code;
    int bad;
} Rc;

static inline uint8_t rc_byte(Rc *rc) {
    if (rc->in >= rc->in_end) { rc->bad = 1; return 0; }
    return *rc->in++;
}

static inline void rc_norm(Rc *rc) {
    if (rc->range < (1u << 24)) { rc->range <<= 8; rc->code = rc->code << 8 | rc_byte(rc); }
}

static inline int rc_bit(Rc *rc, uint16_t *p) {
    rc_norm(rc);
    uint32_t bound = (rc->range >> 11) * *p;
    if (rc->code < bound) { rc->range = bound; *p += (2048 - *p) >> 5; return 0; }
    rc->range -= bound;
    rc->code -= bound;
    *p -= *p >> 5;
    return 1;
}

static inline uint32_t rc_tree(Rc *rc, uint16_t *p, uint32_t limit) {
    uint32_t s = 1;
    do s = s << 1 | (uint32_t)rc_bit(rc, &p[s]); while (s < limit);
    return s - limit;
}

/* Reverse tree; p[off + s] with s from 1 (off may be -1, as in dist_special). */
static inline uint32_t rc_tree_rev(Rc *rc, uint16_t *p, long off, int bits) {
    uint32_t s = 1, r = 0;
    for (int i = 0; i < bits; i++) {
        int b = rc_bit(rc, &p[off + (long)s]);
        s = s << 1 | (uint32_t)b;
        r |= (uint32_t)b << i;
    }
    return r;
}

static inline uint32_t rc_direct(Rc *rc, int bits) {
    uint32_t r = 0;
    while (bits--) {
        rc_norm(rc);
        rc->range >>= 1;
        rc->code -= rc->range;
        uint32_t t = 0u - (rc->code >> 31);
        rc->code += rc->range & t;
        r = (r << 1) + (t + 1);
    }
    return r;
}

typedef struct { uint16_t choice, choice2, low[16][8], mid[16][8], high[256]; } LenDec;

typedef struct {
    int lc, lp, pb, state;
    uint32_t rep[4];
    uint16_t is_match[12][16], is_rep[12], is_rep0[12], is_rep1[12], is_rep2[12], is_rep0_long[12][16];
    uint16_t slot[4][64], special[114], align[16];
    LenDec len, replen;
    uint16_t lit[16][0x300];            /* lc + lp ≤ 4 in LZMA2 */
} Lzma;

static void lzma_reset(Lzma *z) {
    z->state = 0;
    memset(z->rep, 0, sizeof z->rep);
    uint16_t *p = &z->is_match[0][0], *e = &z->lit[15][0x2ff] + 1;
    while (p < e) *p++ = 1024;
}

static uint32_t len_dec(Rc *rc, LenDec *l, uint32_t ps) {
    if (!rc_bit(rc, &l->choice)) return rc_tree(rc, l->low[ps], 8);
    if (!rc_bit(rc, &l->choice2)) return 8 + rc_tree(rc, l->mid[ps], 8);
    return 16 + rc_tree(rc, l->high, 256);
}

typedef struct { uint8_t *buf; size_t len, cap, max, dict_start; } Out;

static int out_reserve(Out *o, size_t more) {
    if (o->len + more <= o->cap) return 0;
    if (o->len + more > o->max) return -1;
    size_t cap = o->cap ? o->cap : 1 << 20;
    while (cap < o->len + more) cap *= 2;
    if (cap > o->max) cap = o->max;
    uint8_t *nb = realloc(o->buf, cap);
    if (!nb) return -1;
    o->buf = nb;
    o->cap = cap;
    return 0;
}

/* One LZMA chunk of LZMA2: decodes exactly `unpacked` bytes. */
static int lzma_chunk(Lzma *z, Rc *rc, Out *o, size_t unpacked) {
    if (out_reserve(o, unpacked)) return -1;
    size_t end = o->len + unpacked;
    uint8_t *d = o->buf;
    size_t pos = o->len;
    uint32_t pbmask = (1u << z->pb) - 1, lpmask = (1u << z->lp) - 1;
    while (pos < end && !rc->bad) {
        uint32_t ps = (uint32_t)pos & pbmask;
        if (!rc_bit(rc, &z->is_match[z->state][ps])) {
            uint32_t prev = pos > o->dict_start ? d[pos - 1] : 0;
            uint16_t *p = z->lit[(((uint32_t)pos & lpmask) << z->lc) + (prev >> (8 - z->lc))];
            uint32_t sym;
            if (z->state < 7) sym = rc_tree(rc, p, 0x100);
            else {
                if (pos - o->dict_start <= z->rep[0]) return -1;
                uint32_t mb = (uint32_t)d[pos - z->rep[0] - 1] << 1, off = 0x100;
                sym = 1;
                do {
                    uint32_t mbit = mb & off;
                    mb <<= 1;
                    if (rc_bit(rc, &p[off + mbit + sym])) { sym = sym << 1 | 1; off = mbit; }
                    else { sym <<= 1; off ^= mbit; }
                } while (sym < 0x100);
            }
            d[pos++] = (uint8_t)sym;
            z->state = z->state < 4 ? 0 : z->state < 10 ? z->state - 3 : z->state - 6;
            continue;
        }
        uint32_t len;
        if (!rc_bit(rc, &z->is_rep[z->state])) {
            z->rep[3] = z->rep[2]; z->rep[2] = z->rep[1]; z->rep[1] = z->rep[0];
            len = len_dec(rc, &z->len, ps);
            z->state = z->state < 7 ? 7 : 10;
            uint32_t ls = len < 3 ? len : 3;
            uint32_t slot = rc_tree(rc, z->slot[ls], 64);
            if (slot < 4) z->rep[0] = slot;
            else {
                int bits = (int)(slot >> 1) - 1;
                uint32_t dist = (2 | (slot & 1)) << bits;
                if (slot < 14) dist += rc_tree_rev(rc, z->special, (long)dist - (long)slot - 1, bits);
                else {
                    dist += rc_direct(rc, bits - 4) << 4;
                    dist += rc_tree_rev(rc, z->align, 0, 4);
                }
                z->rep[0] = dist;
                if (dist == 0xFFFFFFFFu) return -1;          /* end marker: not allowed in LZMA2 */
            }
        } else {
            if (!rc_bit(rc, &z->is_rep0[z->state])) {
                if (!rc_bit(rc, &z->is_rep0_long[z->state][ps])) {
                    z->state = z->state < 7 ? 9 : 11;
                    if (pos - o->dict_start <= z->rep[0]) return -1;
                    d[pos] = d[pos - z->rep[0] - 1];
                    pos++;
                    continue;
                }
            } else {
                uint32_t dist;
                if (!rc_bit(rc, &z->is_rep1[z->state])) dist = z->rep[1];
                else {
                    if (!rc_bit(rc, &z->is_rep2[z->state])) dist = z->rep[2];
                    else { dist = z->rep[3]; z->rep[3] = z->rep[2]; }
                    z->rep[2] = z->rep[1];
                }
                z->rep[1] = z->rep[0];
                z->rep[0] = dist;
            }
            len = len_dec(rc, &z->replen, ps);
            z->state = z->state < 7 ? 8 : 11;
        }
        len += 2;
        if (pos - o->dict_start <= z->rep[0] || len > end - pos) return -1;
        const uint8_t *src = d + pos - z->rep[0] - 1;
        for (uint32_t i = 0; i < len; i++) d[pos + i] = src[i];
        pos += len;
    }
    o->len = pos;
    return rc->bad ? -1 : 0;
}

static int lzma2(const uint8_t **pp, const uint8_t *end, Out *o, Lzma *z) {
    const uint8_t *p = *pp;
    int need_dict = 1, need_props = 1;
    for (;;) {
        if (p >= end) return -1;
        uint8_t c = *p++;
        if (c == 0) break;
        if (c == 1 || c == 2) {
            if (end - p < 2) return -1;
            size_t n = ((size_t)p[0] << 8 | p[1]) + 1;
            p += 2;
            if (c == 1) { o->dict_start = o->len; need_dict = 0; }
            else if (need_dict) return -1;
            if ((size_t)(end - p) < n || out_reserve(o, n)) return -1;
            memcpy(o->buf + o->len, p, n);
            o->len += n;
            p += n;
            continue;
        }
        if (c < 0x80) return -1;
        if (end - p < 4) return -1;
        size_t unpacked = ((size_t)(c & 0x1f) << 16 | (size_t)p[0] << 8 | p[1]) + 1;
        size_t packed = ((size_t)p[2] << 8 | p[3]) + 1;
        p += 4;
        int mode = (c >> 5) & 3;
        if (mode == 3) { o->dict_start = o->len; need_dict = 0; }
        else if (need_dict) return -1;
        if (mode >= 2) {
            if (p >= end) return -1;
            int pr = *p++;
            if (pr >= 225) return -1;
            z->lc = pr % 9; pr /= 9;
            z->lp = pr % 5;
            z->pb = pr / 5;
            if (z->lc + z->lp > 4) return -1;
            need_props = 0;
        } else if (need_props) return -1;
        if (mode >= 1) lzma_reset(z);
        if ((size_t)(end - p) < packed || packed < 5) return -1;
        Rc rc = { p, p + packed, 0xFFFFFFFFu, 0, 0 };
        if (rc_byte(&rc) != 0) return -1;
        for (int i = 0; i < 4; i++) rc.code = rc.code << 8 | rc_byte(&rc);
        if (lzma_chunk(z, &rc, o, unpacked)) return -1;
        p += packed;
    }
    *pp = p;
    return 0;
}

static int varint(const uint8_t **p, const uint8_t *end, uint64_t *v) {
    *v = 0;
    for (int i = 0; i < 9; i++) {
        if (*p >= end) return -1;
        uint8_t b = *(*p)++;
        *v |= (uint64_t)(b & 0x7f) << (7 * i);
        if (!(b & 0x80)) return 0;
    }
    return -1;
}

static uint32_t crc32(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return ~c;
}

static uint64_t crc64(const uint8_t *p, size_t n) {
    static uint64_t t[256];
    if (!t[1]) for (uint64_t i = 0; i < 256; i++) { uint64_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xC96C5795D7870F42ull ^ (c >> 1) : c >> 1; t[i] = c; }
    uint64_t c = ~0ull;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return ~c;
}

int xz_decode(const uint8_t *in, size_t len, uint8_t **out, size_t *outlen, size_t max) {
    static const uint8_t magic[6] = { 0xFD, '7', 'z', 'X', 'Z', 0 };
    Out o = { NULL, 0, 0, max, 0 };
    Lzma *z = malloc(sizeof *z);
    if (!z) return -1;
    const uint8_t *p = in, *end = in + len;
    int streams = 0;
    while (p < end) {
        if (streams && !*p) {           /* padding between streams */
            if (end - p < 4 || p[1] || p[2] || p[3]) goto fail;
            p += 4;
            continue;
        }
        if (end - p < 12 || memcmp(p, magic, 6) || p[6] != 0) goto fail;
        int check = p[7] & 0x0f;
        if (crc32(p + 6, 2) != ((uint32_t)p[8] | (uint32_t)p[9] << 8 | (uint32_t)p[10] << 16 | (uint32_t)p[11] << 24)) goto fail;
        size_t checklen = check == 0 ? 0 : check == 1 ? 4 : check == 4 ? 8 : check == 10 ? 32 : (size_t)4 << ((check - 1) / 3);
        p += 12;
        for (;;) {                      /* blocks */
            if (p >= end) goto fail;
            if (*p == 0) break;         /* index */
            const uint8_t *bstart = p;
            size_t hsize = ((size_t)*p + 1) * 4;
            if ((size_t)(end - p) < hsize) goto fail;
            const uint8_t *h = p + 1, *hend = p + hsize - 4;
            if (crc32(p, hsize - 4) != ((uint32_t)hend[0] | (uint32_t)hend[1] << 8 | (uint32_t)hend[2] << 16 | (uint32_t)hend[3] << 24)) goto fail;
            uint8_t flags = *h++;
            uint64_t v;
            if (flags & 0x3c) goto fail;
            if ((flags & 0x40) && varint(&h, hend, &v)) goto fail;
            if ((flags & 0x80) && varint(&h, hend, &v)) goto fail;
            if ((flags & 3) != 0) goto fail;                 /* a single filter */
            uint64_t id, psize;
            if (varint(&h, hend, &id) || varint(&h, hend, &psize) || id != 0x21 || psize != 1 || h >= hend) goto fail;
            p += hsize;
            size_t ostart = o.len;
            if (lzma2(&p, end, &o, z)) goto fail;
            while ((p - bstart) & 3) { if (p >= end || *p) goto fail; p++; }
            if ((size_t)(end - p) < checklen) goto fail;
            if (check == 1 || check == 4) {             /* CRC32 or CRC64 (little-endian) */
                uint64_t want = 0, got = check == 1 ? crc32(o.buf + ostart, o.len - ostart) : crc64(o.buf + ostart, o.len - ostart);
                for (size_t i = 0; i < checklen; i++) want |= (uint64_t)p[i] << (8 * i);
                if (want != got) goto fail;
            }
            p += checklen;
        }
        /* index: 0, count, (sizes)…, padding, CRC32; then the 12-byte footer */
        p++;
        const uint8_t *istart = p - 1;
        uint64_t nrec, a, b;
        if (varint(&p, end, &nrec)) goto fail;
        for (uint64_t i = 0; i < nrec; i++) if (varint(&p, end, &a) || varint(&p, end, &b)) goto fail;
        while ((p - istart) & 3) { if (p >= end) goto fail; p++; }
        if (end - p < 4 + 12) goto fail;
        if (crc32(istart, (size_t)(p - istart)) != ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24)) goto fail;
        p += 4;
        /* footer: CRC32, index size, flags (same as the header's), "YZ" */
        if (p[10] != 'Y' || p[11] != 'Z' || p[8] != 0 || (p[9] & 0x0f) != check ||
            crc32(p + 4, 6) != ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24)) goto fail;
        p += 12;
        streams++;
    }
    free(z);
    if (!streams) { free(o.buf); return -1; }
    *out = o.buf ? o.buf : malloc(1);
    *outlen = o.len;
    return 0;
fail:
    free(z);
    free(o.buf);
    return -1;
}

int gz_decode(const uint8_t *in, size_t len, uint8_t **out, size_t *outlen, size_t max) {
    if (len < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8 || len > 0x7fffffff) return -1;
    uint8_t flg = in[3];
    size_t p = 10;
    if (flg & 4) { if (p + 2 > len) return -1; p += 2 + (size_t)(in[p] | in[p + 1] << 8); }
    if (flg & 8) { while (p < len && in[p]) p++; p++; }
    if (flg & 16) { while (p < len && in[p]) p++; p++; }
    if (flg & 2) p += 2;
    if (p + 8 > len) return -1;
    /* the original size (mod 2^32) is at the end: it serves as a first estimate */
    uint32_t isize = (uint32_t)in[len - 4] | (uint32_t)in[len - 3] << 8 | (uint32_t)in[len - 2] << 16 | (uint32_t)in[len - 1] << 24;
    if (isize > max) return -1;
    int n = 0;
    char *r = stbi_zlib_decode_noheader_malloc((const char *)in + p, (int)(len - p - 8), &n);
    if (!r) return -1;
    if ((size_t)n > max) { free(r); return -1; }
    *out = (uint8_t *)r;
    *outlen = (size_t)n;
    return 0;
}
