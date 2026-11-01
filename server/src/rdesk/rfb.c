/* rfb.c — see rfb.h. */
#include "rfb.h"
#include "proto.h"

#include <stdlib.h>
#include <string.h>

/* ---- DES, for VNC's challenge only: the protocol requires it ---- */
static const uint8_t IP[64] = { 58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6,64,56,48,40,32,24,16,8,57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7 };
static const uint8_t FP[64] = { 40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,38,6,46,14,54,22,62,30,37,5,45,13,53,21,61,29,36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,34,2,42,10,50,18,58,26,33,1,41,9,49,17,57,25 };
static const uint8_t E[48] = { 32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,12,13,14,15,16,17,16,17,18,19,20,21,20,21,22,23,24,25,24,25,26,27,28,29,28,29,30,31,32,1 };
static const uint8_t P[32] = { 16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,2,8,24,14,32,27,3,9,19,13,30,6,22,11,4,25 };
static const uint8_t PC1[56] = { 57,49,41,33,25,17,9,1,58,50,42,34,26,18,10,2,59,51,43,35,27,19,11,3,60,52,44,36,63,55,47,39,31,23,15,7,62,54,46,38,30,22,14,6,61,53,45,37,29,21,13,5,28,20,12,4 };
static const uint8_t PC2[48] = { 14,17,11,24,1,5,3,28,15,6,21,10,23,19,12,4,26,8,16,7,27,20,13,2,41,52,31,37,47,55,30,40,51,45,33,48,44,49,39,56,34,53,46,42,50,36,29,32 };
static const uint8_t SHIFT[16] = { 1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1 };
static const uint8_t SBOX[8][64] = {
 {14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
 {15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
 {10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
 {7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14},
 {2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
 {12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
 {4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
 {13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11} };

static uint64_t perm(uint64_t in, int inbits, const uint8_t *t, int n) {
    uint64_t o = 0;
    for (int i = 0; i < n; i++) o = o << 1 | ((in >> (inbits - t[i])) & 1);
    return o;
}
void des_encrypt(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]) {
    uint64_t k = 0, b = 0, sub[16];
    for (int i = 0; i < 8; i++) { k = k << 8 | key[i]; b = b << 8 | in[i]; }
    uint64_t cd = perm(k, 64, PC1, 56);
    uint32_t c = (uint32_t)(cd >> 28), d = (uint32_t)(cd & 0xfffffff);
    for (int i = 0; i < 16; i++) {
        c = ((c << SHIFT[i]) | (c >> (28 - SHIFT[i]))) & 0xfffffff;
        d = ((d << SHIFT[i]) | (d >> (28 - SHIFT[i]))) & 0xfffffff;
        sub[i] = perm((uint64_t)c << 28 | d, 56, PC2, 48);
    }
    b = perm(b, 64, IP, 64);
    uint32_t l = (uint32_t)(b >> 32), r = (uint32_t)b;
    for (int i = 0; i < 16; i++) {
        uint64_t x = perm(r, 32, E, 48) ^ sub[i];
        uint32_t f = 0;
        for (int s = 0; s < 8; s++) {
            int v = (int)(x >> (42 - s * 6)) & 63;
            f = f << 4 | SBOX[s][(v & 32) | ((v & 1) << 4) | ((v >> 1) & 15)];
        }
        uint32_t t = r;
        r = l ^ (uint32_t)perm(f, 32, P, 32);
        l = t;
    }
    b = perm((uint64_t)r << 32 | l, 64, FP, 64);
    for (int i = 7; i >= 0; i--) { out[i] = (uint8_t)b; b >>= 8; }
}

void rfb_challenge(const char *password, const uint8_t in[16], uint8_t out[16]) {
    uint8_t k[8] = { 0 };
    for (int i = 0; i < 8 && password[i]; i++) {
        uint8_t c = (uint8_t)password[i], r = 0;
        for (int b = 0; b < 8; b++) if (c & (1 << b)) r |= (uint8_t)(0x80 >> b);   /* VNC reverses the key's bits */
        k[i] = r;
    }
    des_encrypt(k, in, out);
    des_encrypt(k, in + 8, out + 8);
    memset(k, 0, sizeof k);
}

/* ---- RFB ---- */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void w32(uint8_t *p, uint32_t v) { w16(p, v >> 16); w16(p + 2, v); }

static int skip(Rfb *r, size_t n) { uint8_t t[512]; while (n) { size_t k = n < sizeof t ? n : sizeof t; if (r->rd(r->ud, t, k)) return -1; n -= k; } return 0; }

static int resize(Rfb *r, int w, int h) {
    if (w < 1 || h < 1 || w > RD_MAX_W || h > RD_MAX_H) return -1;
    r->px = r->fb(r->ud, w, h);
    if (!r->px) return -1;
    r->w = w; r->h = h;
    return 0;
}

int rfb_handshake(Rfb *r, const char *password) {
    uint8_t b[64];
    if (r->rd(r->ud, b, 12) || memcmp(b, "RFB 003.", 8)) return -1;
    if (r->wr(r->ud, "RFB 003.008\n", 12)) return -1;
    if (r->rd(r->ud, b, 1)) return -1;
    int n = b[0];
    if (!n) return -1;
    uint8_t types[255];
    if (r->rd(r->ud, types, (size_t)n)) return -1;
    int vnc = 0, none = 0;
    for (int i = 0; i < n; i++) { vnc |= types[i] == 2; none |= types[i] == 1; }
    if (password[0] ? !vnc && !none : !none && !vnc) return -1;
    uint8_t pick = vnc && (password[0] || !none) ? 2 : 1;
    if (r->wr(r->ud, &pick, 1)) return -1;
    if (pick == 2) {
        uint8_t c[16], o[16];
        if (r->rd(r->ud, c, 16)) return -1;
        rfb_challenge(password, c, o);
        if (r->wr(r->ud, o, 16)) return -1;
    }
    if (r->rd(r->ud, b, 4)) return -1;
    if (be32(b)) return 1;
    uint8_t one = 1;                    /* shared */
    if (r->wr(r->ud, &one, 1) || r->rd(r->ud, b, 24)) return -1;
    int w = (int)be16(b), h = (int)be16(b + 2);
    uint32_t nl = be32(b + 20);
    if (nl > 4096 || skip(r, nl) || resize(r, w, h)) return -1;
    /* 32 bits, little-endian, true colour: BGRX in memory */
    uint8_t pf[20] = { 0, 0, 0, 0, 32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0 };
    if (r->wr(r->ud, pf, 20)) return -1;
    uint8_t enc[4 + 16] = { 2, 0, 0, 4 };
    w32(enc + 4, 1); w32(enc + 8, 5); w32(enc + 12, 0); w32(enc + 16, (uint32_t)-223);
    return r->wr(r->ud, enc, sizeof enc) ? -1 : 0;
}

int rfb_update_request(Rfb *r, int inc) {
    uint8_t m[10] = { 3, (uint8_t)inc };
    w16(m + 6, (uint32_t)r->w); w16(m + 8, (uint32_t)r->h);
    return r->wr(r->ud, m, 10);
}

static void fill(Rfb *r, int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++) { uint32_t *d = r->px + (size_t)(y + j) * r->w + x; for (int i = 0; i < w; i++) d[i] = c; }
}

static int rect_raw(Rfb *r, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        uint32_t *d = r->px + (size_t)(y + j) * r->w + x;
        if (r->rd(r->ud, d, (size_t)w * 4)) return -1;
        for (int i = 0; i < w; i++) d[i] |= 0xff000000u;
    }
    return 0;
}

static int rect_copy(Rfb *r, int x, int y, int w, int h) {
    uint8_t b[4];
    if (r->rd(r->ud, b, 4)) return -1;
    int sx = (int)be16(b), sy = (int)be16(b + 2);
    if (sx + w > r->w || sy + h > r->h) return -1;
    size_t row = (size_t)w * 4;
    for (int j = 0; j < h; j++) {
        int k = sy < y ? h - 1 - j : j;
        memmove(r->px + (size_t)(y + k) * r->w + x, r->px + (size_t)(sy + k) * r->w + sx, row);
    }
    return 0;
}

static int rd_px(Rfb *r, uint32_t *c) {
    if (r->rd(r->ud, c, 4)) return -1;
    *c |= 0xff000000u;
    return 0;
}

static int rect_hextile(Rfb *r, int x, int y, int w, int h) {
    uint32_t bg = 0xff000000u, fg = 0xffffffffu;
    for (int ty = 0; ty < h; ty += 16) for (int tx = 0; tx < w; tx += 16) {
        int tw = w - tx < 16 ? w - tx : 16, th = h - ty < 16 ? h - ty : 16, X = x + tx, Y = y + ty;
        uint8_t f;
        if (r->rd(r->ud, &f, 1)) return -1;
        if (f & 1) { if (rect_raw(r, X, Y, tw, th)) return -1; continue; }
        if ((f & 2) && rd_px(r, &bg)) return -1;
        fill(r, X, Y, tw, th, bg);
        if ((f & 4) && rd_px(r, &fg)) return -1;
        if (!(f & 8)) continue;
        uint8_t n;
        if (r->rd(r->ud, &n, 1)) return -1;
        for (int i = 0; i < n; i++) {
            uint32_t c = fg;
            uint8_t b[2];
            if ((f & 16) && rd_px(r, &c)) return -1;
            if (r->rd(r->ud, b, 2)) return -1;
            int sx = b[0] >> 4, sy = b[0] & 15, sw = (b[1] >> 4) + 1, sh = (b[1] & 15) + 1;
            if (sx + sw > tw || sy + sh > th) return -1;
            fill(r, X + sx, Y + sy, sw, sh, c);
        }
    }
    return 0;
}

int rfb_message(Rfb *r) {
    uint8_t t;
    if (r->rd(r->ud, &t, 1)) return -1;
    if (t == 2) return 0;               /* bell */
    if (t == 3) {                       /* clipboard text: not taken */
        uint8_t b[7];
        if (r->rd(r->ud, b, 7)) return -1;
        return skip(r, be32(b + 3));
    }
    if (t == 1) {                       /* colour map: not with true colour */
        uint8_t b[5];
        if (r->rd(r->ud, b, 5)) return -1;
        return skip(r, (size_t)be16(b + 3) * 6);
    }
    if (t) return -1;
    uint8_t b[3], h[12];
    if (r->rd(r->ud, b, 3)) return -1;
    int n = (int)be16(b + 1);
    for (int i = 0; i < n; i++) {
        if (r->rd(r->ud, h, 12)) return -1;
        int x = (int)be16(h), y = (int)be16(h + 2), w = (int)be16(h + 4), ht = (int)be16(h + 6);
        int32_t enc = (int32_t)be32(h + 8);
        if (enc == -223) { if (resize(r, w, ht)) return -1; continue; }
        if (x + w > r->w || y + ht > r->h) return -1;
        int e = enc == 0 ? rect_raw(r, x, y, w, ht) : enc == 1 ? rect_copy(r, x, y, w, ht) : enc == 5 ? rect_hextile(r, x, y, w, ht) : -1;
        if (e) return -1;
    }
    r->frame(r->ud);
    return rfb_update_request(r, 1);
}

int rfb_key(Rfb *r, uint32_t sym, int down) {
    uint8_t m[8] = { 4, (uint8_t)!!down };
    w32(m + 4, sym);
    return r->wr(r->ud, m, 8);
}
int rfb_pointer(Rfb *r, int x, int y, int buttons) {
    uint8_t m[6] = { 5, (uint8_t)buttons };
    w16(m + 2, (uint32_t)x); w16(m + 4, (uint32_t)y);
    return r->wr(r->ud, m, 6);
}
