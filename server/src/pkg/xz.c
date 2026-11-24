/* xz.c — decompressors for Debian's indexes: our own xz (LZMA2) and gzip (stb_image's
 * inflate, which is already in lord-server for PNGs). All in memory: repository files are
 * a few tens of MB.
 *
 * xz: container with one or more streams, blocks with a single LZMA2 filter (what Debian's
 * repositories use; no BCJ or delta). Each block's check (CRC32 or CRC64) is verified, even
 * though the compressed file already comes verified by the signed InRelease's SHA-256.
 * The LZMA decoder follows the LZMA specification (Igor Pavlov) and the structure of xz-embedded (public domain). */
#include "xz.h"
#ifndef XZ_NO_GZ                     /* lord-vmunpack builds xz.c without gzip (no stb_image) */
#include "stb_image.h"
#endif

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

static uint32_t crc32_up(uint32_t c, const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1; t[i] = k; }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return ~c;
}
static uint64_t crc64_up(uint64_t c, const uint8_t *p, size_t n) {
    static uint64_t t[256];
    if (!t[1]) for (uint64_t i = 0; i < 256; i++) { uint64_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xC96C5795D7870F42ull ^ (k >> 1) : k >> 1; t[i] = k; }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return ~c;
}
static uint32_t crc32(const uint8_t *p, size_t n) { return crc32_up(0, p, n); }
static uint64_t crc64(const uint8_t *p, size_t n) { return crc64_up(0, p, n); }
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

/* ---- streaming (xzs_*, xz.h): the same decoder over a window instead of the whole output ----
 * The input comes through a read callback into a small buffer (a block header is at most 1 KiB, an LZMA2 chunk at
 * most 64 KiB packed); the output goes into a window of the block's dictionary plus one chunk, of which only bytes
 * already handed out slide away. Every block starts with a dictionary reset (LZMA2 refuses anything else), so the
 * window is emptied there. Checks: none, CRC32 or CRC64 (verified); SHA-256 and the reserved ones are refused. */
#define XZS_IN_CAP     (128u * 1024)
#define XZS_CHUNK      ((2u << 20) + (64u << 10))   /* the most one LZMA2 chunk decodes to, with room for a stored one */
#define XZS_MAX_BLOCKS (1u << 22)

enum { XS_STREAM = 0, XS_BLOCK, XS_DATA, XS_PADDING, XS_DONE, XS_ERR };

typedef struct { uint64_t unpadded, uncompressed; } XzsRec;

struct XzStream {
    int (*rd)(void *ctx, uint8_t *buf, size_t n);
    void *ctx;
    uint8_t ib[XZS_IN_CAP];
    size_t ipos, iend;
    int ieof;
    uint64_t ibase;                         /* the input's offset of ib[0] */
    Out o;
    size_t emitted, max_dict;
    Lzma z;
    int st, err, check, streams;
    size_t checklen;
    uint8_t sflags[2];
    /* the block being decoded */
    int need_dict, need_props;
    uint64_t b_start, b_data, b_out, b_csize, b_usize;   /* sizes UINT64_MAX when the header doesn't say */
    size_t b_hsize;
    uint32_t b_crc32;
    uint64_t b_crc64;
    XzsRec *rec;
    size_t nrec, caprec;
};

static uint64_t xzs_pos(const XzStream *s) { return s->ibase + s->ipos; }

/* at least n bytes in ib[ipos..]: 0, else -1 (truncated, or the reader failed: s->err says which) */
static int xzs_need(XzStream *s, size_t n) {
    if (s->iend - s->ipos >= n) return 0;
    if (n > XZS_IN_CAP) { s->err = XZS_E_DATA; return -1; }
    if (s->ipos) {
        memmove(s->ib, s->ib + s->ipos, s->iend - s->ipos);
        s->ibase += s->ipos;
        s->iend -= s->ipos;
        s->ipos = 0;
    }
    while (s->iend < n) {
        if (s->ieof) { s->err = XZS_E_TRUNCATED; return -1; }
        int r = s->rd(s->ctx, s->ib + s->iend, XZS_IN_CAP - s->iend);
        if (r < 0) { s->err = XZS_E_IO; return -1; }
        if (r == 0) s->ieof = 1;
        s->iend += (size_t)r;
    }
    return 0;
}

static int xzs_varint(XzStream *s, uint64_t *v, uint32_t *crc) {
    *v = 0;
    for (int i = 0; i < 9; i++) {
        if (xzs_need(s, 1)) return -1;
        uint8_t b = s->ib[s->ipos++];
        if (crc) *crc = crc32_up(*crc, &b, 1);
        if (i && !b) { s->err = XZS_E_DATA; return -1; }      /* not the shortest form */
        *v |= (uint64_t)(b & 0x7f) << (7 * i);
        if (!(b & 0x80)) return 0;
    }
    s->err = XZS_E_DATA;
    return -1;
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

XzStream *xzs_new(int (*rd)(void *ctx, uint8_t *buf, size_t n), void *ctx, size_t max_dict) {
    if (!rd) return NULL;
    XzStream *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->rd = rd;
    s->ctx = ctx;
    s->max_dict = max_dict;
    return s;
}

void xzs_free(XzStream *s) {
    if (!s) return;
    free(s->o.buf);
    free(s->rec);
    free(s);
}

uint64_t xzs_in_pos(const XzStream *s) { return s ? xzs_pos(s) : 0; }
int xzs_error(const XzStream *s) { return s ? s->err : XZS_E_DATA; }

static int xzs_fail(XzStream *s, int e) { if (!s->err) s->err = e ? e : XZS_E_DATA; s->st = XS_ERR; return -1; }

/* the stream header; after a stream: the end of the input, stream padding, or the next stream */
static int xzs_stream(XzStream *s) {
    static const uint8_t magic[6] = { 0xFD, '7', 'z', 'X', 'Z', 0 };
    if (s->streams) {
        for (;;) {                                           /* stream padding: zeros, four at a time */
            if (xzs_need(s, 1)) {
                if (s->err == XZS_E_TRUNCATED && s->iend == s->ipos) { s->err = 0; s->st = XS_DONE; return 0; }
                return xzs_fail(s, s->err);
            }
            if (s->ib[s->ipos]) break;
            if (xzs_need(s, 4)) { s->err = 0; return xzs_fail(s, XZS_E_TRAILING); }
            if (s->ib[s->ipos + 1] || s->ib[s->ipos + 2] || s->ib[s->ipos + 3]) return xzs_fail(s, XZS_E_TRAILING);
            s->ipos += 4;
        }
    }
    if (xzs_need(s, 12)) { if (s->streams && s->err == XZS_E_TRUNCATED) s->err = XZS_E_TRAILING; return xzs_fail(s, s->err); }
    const uint8_t *p = s->ib + s->ipos;
    if (memcmp(p, magic, 6)) return xzs_fail(s, s->streams ? XZS_E_TRAILING : XZS_E_DATA);
    if (p[6] != 0 || (p[7] & 0xf0) || crc32(p + 6, 2) != le32(p + 8)) return xzs_fail(s, XZS_E_DATA);
    s->check = p[7] & 0x0f;
    if (s->check != 0 && s->check != 1 && s->check != 4) return xzs_fail(s, XZS_E_CHECK);
    s->checklen = s->check == 0 ? 0 : s->check == 1 ? 4 : 8;
    s->sflags[0] = p[6];
    s->sflags[1] = p[7];
    s->ipos += 12;
    s->nrec = 0;
    s->st = XS_BLOCK;
    return 0;
}

/* the index (its indicator already read: the records must be the blocks seen) and the stream footer */
static int xzs_index(XzStream *s) {
    uint64_t start = xzs_pos(s) - 1, n, a, b;
    uint8_t zero = 0;
    uint32_t crc = crc32_up(0, &zero, 1);
    if (xzs_varint(s, &n, &crc)) return xzs_fail(s, s->err);
    if (n != s->nrec) return xzs_fail(s, XZS_E_DATA);
    for (uint64_t i = 0; i < n; i++) {
        if (xzs_varint(s, &a, &crc) || xzs_varint(s, &b, &crc)) return xzs_fail(s, s->err);
        if (a != s->rec[i].unpadded || b != s->rec[i].uncompressed) return xzs_fail(s, XZS_E_DATA);
    }
    while ((xzs_pos(s) - start) & 3) {
        if (xzs_need(s, 1)) return xzs_fail(s, s->err);
        if (s->ib[s->ipos]) return xzs_fail(s, XZS_E_DATA);
        crc = crc32_up(crc, s->ib + s->ipos, 1);
        s->ipos++;
    }
    uint64_t isize = xzs_pos(s) - start + 4;
    if (xzs_need(s, 4 + 12)) return xzs_fail(s, s->err);
    const uint8_t *p = s->ib + s->ipos;
    if (le32(p) != crc) return xzs_fail(s, XZS_E_DATA);
    p += 4;
    /* footer: CRC32 of the next 6 bytes, the index's size / 4 - 1, the header's flags, "YZ" */
    if (crc32(p + 4, 6) != le32(p) || ((uint64_t)le32(p + 4) + 1) * 4 != isize || p[8] != s->sflags[0] || p[9] != s->sflags[1] ||
        p[10] != 'Y' || p[11] != 'Z') return xzs_fail(s, XZS_E_DATA);
    s->ipos += 16;
    s->streams++;
    s->st = XS_STREAM;
    return 0;
}

static int xzs_block(XzStream *s) {
    if (xzs_need(s, 1)) return xzs_fail(s, s->err);
    if (s->ib[s->ipos] == 0) { s->ipos++; return xzs_index(s); }
    size_t hsize = ((size_t)s->ib[s->ipos] + 1) * 4;
    if (xzs_need(s, hsize)) return xzs_fail(s, s->err);
    const uint8_t *p = s->ib + s->ipos, *h = p + 1, *hend = p + hsize - 4;
    if (crc32(p, hsize - 4) != le32(hend)) return xzs_fail(s, XZS_E_DATA);
    uint8_t flags = *h++;
    uint64_t csize = UINT64_MAX, usize = UINT64_MAX, id, psize;
    if ((flags & 0x3c) || (flags & 3)) return xzs_fail(s, XZS_E_DATA);         /* reserved bits; one filter only */
    if ((flags & 0x40) && (varint(&h, hend, &csize) || !csize)) return xzs_fail(s, XZS_E_DATA);
    if ((flags & 0x80) && varint(&h, hend, &usize)) return xzs_fail(s, XZS_E_DATA);
    if (varint(&h, hend, &id) || varint(&h, hend, &psize) || id != 0x21 || psize != 1 || h >= hend) return xzs_fail(s, XZS_E_DATA);
    uint8_t dp = *h++;
    if (dp > 40) return xzs_fail(s, XZS_E_DATA);
    while (h < hend) if (*h++) return xzs_fail(s, XZS_E_DATA);                 /* the header's padding is zeros */
    uint64_t dict = dp == 40 ? 0xFFFFFFFFull : (uint64_t)(2 | (dp & 1)) << (dp / 2 + 11);
    if (dict > s->max_dict) return xzs_fail(s, XZS_E_DICT);
    if (s->nrec >= XZS_MAX_BLOCKS) return xzs_fail(s, XZS_E_DATA);
    size_t cap = (size_t)dict + XZS_CHUNK + 4096;        /* + 4 KiB: the window slides by whole 4 KiB */
    if (s->o.cap < cap) {
        free(s->o.buf);
        s->o.buf = malloc(cap);
        s->o.cap = s->o.buf ? cap : 0;
        if (!s->o.buf) return xzs_fail(s, XZS_E_MEM);
    }
    s->o.max = s->o.cap;
    s->o.len = s->o.dict_start = s->emitted = 0;             /* everything before was handed out */
    s->b_start = xzs_pos(s);
    s->ipos += hsize;
    s->b_hsize = hsize;
    s->b_data = xzs_pos(s);
    s->b_csize = csize;
    s->b_usize = usize;
    s->b_out = 0;
    s->b_crc32 = 0;
    s->b_crc64 = 0;
    s->need_dict = s->need_props = 1;
    s->st = XS_DATA;
    return 0;
}

/* the bytes just decoded, o.buf[from..o.len), into the block's check and size */
static void xzs_account(XzStream *s, size_t from) {
    size_t n = s->o.len - from;
    if (s->check == 1) s->b_crc32 = crc32_up(s->b_crc32, s->o.buf + from, n);
    else if (s->check == 4) s->b_crc64 = crc64_up(s->b_crc64, s->o.buf + from, n);
    s->b_out += n;
}

/* one LZMA2 control byte and its chunk */
static int xzs_chunk(XzStream *s) {
    if (xzs_need(s, 1)) return xzs_fail(s, s->err);
    uint8_t c = s->ib[s->ipos];
    if (c == 0) { s->ipos++; s->st = XS_PADDING; return 0; }
    /* room for one more chunk: what was handed out and lies beyond the dictionary slides away, by a multiple of
     * 4 KiB so that the position's low bits (the literal and match states' pb and lp) stay the same */
    size_t dict = s->o.cap - XZS_CHUNK - 4096;
    if (s->o.len > dict) {
        size_t shift = (s->o.len - dict) & ~(size_t)4095, keep = s->o.len - shift;
        memmove(s->o.buf, s->o.buf + shift, keep);
        s->o.len = keep;
        s->o.dict_start = s->o.dict_start > shift ? s->o.dict_start - shift : 0;
        s->emitted -= shift;
    }
    size_t from = s->o.len;
    if (c == 1 || c == 2) {
        if (xzs_need(s, 3)) return xzs_fail(s, s->err);
        size_t n = ((size_t)s->ib[s->ipos + 1] << 8 | s->ib[s->ipos + 2]) + 1;
        if (c == 1) { s->o.dict_start = s->o.len; s->need_dict = 0; }
        else if (s->need_dict) return xzs_fail(s, XZS_E_DATA);
        if (xzs_need(s, 3 + n)) return xzs_fail(s, s->err);
        memcpy(s->o.buf + s->o.len, s->ib + s->ipos + 3, n);
        s->o.len += n;
        s->ipos += 3 + n;
        xzs_account(s, from);
        return 0;
    }
    if (c < 0x80) return xzs_fail(s, XZS_E_DATA);
    if (xzs_need(s, 5)) return xzs_fail(s, s->err);
    const uint8_t *p = s->ib + s->ipos + 1;
    size_t unpacked = ((size_t)(c & 0x1f) << 16 | (size_t)p[0] << 8 | p[1]) + 1;
    size_t packed = ((size_t)p[2] << 8 | p[3]) + 1;
    int mode = (c >> 5) & 3;
    size_t hl = 5 + (mode >= 2);
    if (xzs_need(s, hl + packed)) return xzs_fail(s, s->err);
    p = s->ib + s->ipos + 5;
    if (mode == 3) { s->o.dict_start = s->o.len; s->need_dict = 0; }
    else if (s->need_dict) return xzs_fail(s, XZS_E_DATA);
    if (mode >= 2) {
        int pr = *p++;
        if (pr >= 225) return xzs_fail(s, XZS_E_DATA);
        s->z.lc = pr % 9; pr /= 9;
        s->z.lp = pr % 5;
        s->z.pb = pr / 5;
        if (s->z.lc + s->z.lp > 4) return xzs_fail(s, XZS_E_DATA);
        s->need_props = 0;
    } else if (s->need_props) return xzs_fail(s, XZS_E_DATA);
    if (mode >= 1) lzma_reset(&s->z);
    if (packed < 5) return xzs_fail(s, XZS_E_DATA);
    Rc rc = { p, p + packed, 0xFFFFFFFFu, 0, 0 };
    if (rc_byte(&rc) != 0) return xzs_fail(s, XZS_E_DATA);
    for (int i = 0; i < 4; i++) rc.code = rc.code << 8 | rc_byte(&rc);
    if (lzma_chunk(&s->z, &rc, &s->o, unpacked)) return xzs_fail(s, XZS_E_DATA);
    s->ipos += hl + packed;
    xzs_account(s, from);
    return 0;
}

/* the block's padding and check, its sizes against its header, and its record for the index */
static int xzs_block_end(XzStream *s) {
    uint64_t comp = xzs_pos(s) - s->b_data;
    if ((s->b_csize != UINT64_MAX && comp != s->b_csize) || (s->b_usize != UINT64_MAX && s->b_out != s->b_usize)) return xzs_fail(s, XZS_E_DATA);
    while ((xzs_pos(s) - s->b_start) & 3) {
        if (xzs_need(s, 1)) return xzs_fail(s, s->err);
        if (s->ib[s->ipos++]) return xzs_fail(s, XZS_E_DATA);
    }
    if (xzs_need(s, s->checklen)) return xzs_fail(s, s->err);
    uint64_t want = 0;
    for (size_t i = 0; i < s->checklen; i++) want |= (uint64_t)s->ib[s->ipos + i] << (8 * i);
    if ((s->check == 1 && want != s->b_crc32) || (s->check == 4 && want != s->b_crc64)) return xzs_fail(s, XZS_E_DATA);
    s->ipos += s->checklen;
    if (s->nrec == s->caprec) {
        size_t nc = s->caprec ? s->caprec * 2 : 64;
        XzsRec *r = realloc(s->rec, nc * sizeof *r);
        if (!r) return xzs_fail(s, XZS_E_MEM);
        s->rec = r;
        s->caprec = nc;
    }
    s->rec[s->nrec].unpadded = s->b_hsize + comp + s->checklen;
    s->rec[s->nrec].uncompressed = s->b_out;
    s->nrec++;
    s->st = XS_BLOCK;
    return 0;
}

ssize_t xzs_read(XzStream *s, uint8_t *out, size_t n) {
    if (!s || s->st == XS_ERR) return -1;
    while (s->emitted == s->o.len) {
        int r;
        switch (s->st) {
        case XS_STREAM: r = xzs_stream(s); break;
        case XS_BLOCK: r = xzs_block(s); break;
        case XS_DATA: r = xzs_chunk(s); break;
        case XS_PADDING: r = xzs_block_end(s); break;
        case XS_DONE: return 0;
        default: return -1;
        }
        if (r) return -1;
    }
    size_t k = s->o.len - s->emitted;
    if (k > n) k = n;
    memcpy(out, s->o.buf + s->emitted, k);
    s->emitted += k;
    return (ssize_t)k;
}

#ifndef XZ_NO_GZ
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
#endif
