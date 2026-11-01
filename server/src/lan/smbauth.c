/* smbauth.c — see smbauth.h. MD4, MD5 and RC4 are here because the protocol requires them for NTLMv2; nothing
 * else in LordOS may use them. Only the helper links this. */
#include "smbauth.h"
#include "../pkg/crypto.h"

#include <string.h>

static uint32_t rol(uint32_t x, int n) { return x << n | x >> (32 - n); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

/* both share padding: 0x80, zeros, the length in bits as a little-endian 64-bit number */
static void md_blocks(const uint8_t *d, size_t len, uint32_t st[4], void (*block)(uint32_t *, const uint8_t *)) {
    size_t i = 0;
    for (; len - i >= 64; i += 64) block(st, d + i);
    uint8_t t[128] = { 0 };
    size_t r = len - i;
    if (r) memcpy(t, d + i, r);
    t[r] = 0x80;
    size_t end = r < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) t[end - 8 + k] = (uint8_t)(bits >> (8 * k));
    block(st, t);
    if (end == 128) block(st, t + 64);
}
static void md_out(const uint32_t st[4], uint8_t out[16]) { for (int i = 0; i < 4; i++) put32(out + i * 4, st[i]); }
static const uint32_t MD_INIT[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };

static void md4_block(uint32_t *s, const uint8_t *p) {
    uint32_t x[16], a = s[0], b = s[1], c = s[2], d = s[3], t;
    for (int i = 0; i < 16; i++) x[i] = le32(p + i * 4);
    static const uint8_t o2[16] = { 0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15 }, o3[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
    static const uint8_t r1[4] = { 3, 7, 11, 19 }, r2[4] = { 3, 5, 9, 13 }, r3[4] = { 3, 9, 11, 15 };
#define STEP(f, k, ad, r) t = rol(a + (f) + x[k] + (ad), r); a = d; d = c; c = b; b = t
    for (int i = 0; i < 16; i++) { STEP((b & c) | (~b & d), i, 0, r1[i & 3]); }
    for (int i = 0; i < 16; i++) { STEP((b & c) | (b & d) | (c & d), o2[i], 0x5a827999u, r2[i & 3]); }
    for (int i = 0; i < 16; i++) { STEP(b ^ c ^ d, o3[i], 0x6ed9eba1u, r3[i & 3]); }
#undef STEP
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}

static void md5_block(uint32_t *s, const uint8_t *p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const uint8_t R[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        uint32_t t = d;
        d = c; c = b;
        b = b + rol(a + f + K[i] + le32(p + g * 4), R[(i >> 4) * 4 + (i & 3)]);
        a = t;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}

void md4(const void *data, size_t len, uint8_t out[16]) { uint32_t s[4]; memcpy(s, MD_INIT, sizeof s); md_blocks(data, len, s, md4_block); md_out(s, out); }
void md5(const void *data, size_t len, uint8_t out[16]) { uint32_t s[4]; memcpy(s, MD_INIT, sizeof s); md_blocks(data, len, s, md5_block); md_out(s, out); }

/* key ≤ 64 bytes here, and data is short: everything fits a stack buffer */
void hmac_md5(const void *key, size_t klen, const void *data, size_t len, uint8_t out[16]) {
    uint8_t k[64] = { 0 }, b[64 + 4096], in[16];
    if (klen > 64 || len > 4096) { memset(out, 0, 16); return; }
    memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) b[i] = k[i] ^ 0x36;
    memcpy(b + 64, data, len);
    md5(b, 64 + len, in);
    for (int i = 0; i < 64; i++) b[i] = k[i] ^ 0x5c;
    memcpy(b + 64, in, 16);
    md5(b, 80, out);
}

void hmac_sha256(const void *key, size_t klen, const void *data, size_t len, uint8_t out[32]) {
    uint8_t k[64] = { 0 }, pad[64], in[32];
    memcpy(k, key, klen > 64 ? 64 : klen);
    Sha256 s;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&s); sha256_update(&s, pad, 64); sha256_update(&s, data, len); sha256_final(&s, in);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&s); sha256_update(&s, pad, 64); sha256_update(&s, in, 32); sha256_final(&s, out);
}

static void rc4(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[256];
    for (int i = 0; i < 256; i++) s[i] = (uint8_t)i;
    for (int i = 0, j = 0; i < 256; i++) { j = (j + s[i] + key[i % 16]) & 255; uint8_t t = s[i]; s[i] = s[j]; s[j] = t; }
    for (int n = 0, i = 0, j = 0; n < 16; n++) {
        i = (i + 1) & 255; j = (j + s[i]) & 255;
        uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
        out[n] = in[n] ^ s[(s[i] + s[j]) & 255];
    }
}

size_t text_utf16(const char *z, uint8_t *out, size_t cap, int upper) {
    const uint8_t *s = (const uint8_t *)z;
    size_t o = 0;
    while (*s) {
        uint32_t c = *s;
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0) return 0;
        if (n) c &= 0x3fu >> n;
        for (int i = 1; i <= n; i++) { if ((s[i] & 0xc0) != 0x80) return 0; c = c << 6 | (s[i] & 63); }
        s += n + 1;
        if (upper) {
            if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7) || (c >= 0x3b1 && c <= 0x3c9 && c != 0x3c2) || (c >= 0x430 && c <= 0x44f)) c -= 0x20;
            else if (c >= 0x450 && c <= 0x45f) c -= 0x50;
        }
        if (o + 4 > cap || c > 0x10ffff) return 0;
        if (c >= 0x10000) { c -= 0x10000; put16(out + o, 0xd800 | c >> 10); put16(out + o + 2, 0xdc00 | (c & 1023)); o += 4; }
        else { put16(out + o, c); o += 2; }
    }
    return o ? o : (size_t)(z[0] == 0);   /* 1: an empty text */
}

/* an empty text is written as nothing; text_utf16 tells that apart from a failure by answering 1 */
static int field16(const char *s, uint8_t *out, size_t cap, int upper, size_t *n) {
    size_t r = text_utf16(s, out, cap, upper);
    if (!r) return -1;
    *n = s[0] ? r : 0;
    return 0;
}

int ntlmv2_hash(const char *user, const char *domain, const char *password, uint8_t out[16]) {
    uint8_t b[1024], nt[16];
    size_t n, u, d;
    if (field16(password, b, sizeof b, 0, &n)) return -1;
    md4(b, n, nt);
    if (field16(user, b, 512, 1, &u) || field16(domain, b + u, 512, 0, &d)) { memset(nt, 0, 16); return -1; }
    hmac_md5(nt, 16, b, u + d, out);
    memset(b, 0, sizeof b);
    memset(nt, 0, 16);
    return 0;
}

size_t ntlmv2_temp(uint8_t *t, size_t cap, uint64_t ft, const uint8_t client[8], const uint8_t *av, size_t avlen) {
    size_t n = 28 + avlen + 4;
    if (n > cap) return 0;
    memset(t, 0, n);
    t[0] = t[1] = 1;
    put32(t + 8, (uint32_t)ft);
    put32(t + 12, (uint32_t)(ft >> 32));
    memcpy(t + 16, client, 8);
    memcpy(t + 28, av, avlen);
    return n;
}

void ntlmv2_proof(const uint8_t hash[16], const uint8_t server[8], const uint8_t *temp, size_t tlen, uint8_t proof[16], uint8_t base[16]) {
    uint8_t b[8 + 3072];
    memcpy(b, server, 8);
    memcpy(b + 8, temp, tlen < 3072 ? tlen : 3072);
    hmac_md5(hash, 16, b, 8 + (tlen < 3072 ? tlen : 3072), proof);
    hmac_md5(hash, 16, proof, 16, base);
}

/* unicode, request target, sign, NTLM, extended security, target info, 128, key exchange, 56 */
#define FLAGS 0xe0880215u
#define F_KEYX 0x40000000u

size_t ntlm_user_negotiate(uint8_t *o, size_t cap) {
    if (cap < 32) return 0;
    memset(o, 0, 32);
    memcpy(o, "NTLMSSP", 8);
    put32(o + 8, 1);
    put32(o + 12, FLAGS);
    return 32;
}

static uint8_t *field(uint8_t *hdr, uint8_t *payload, const uint8_t *base, const void *data, size_t n) {
    put16(hdr, (uint32_t)n);
    put16(hdr + 2, (uint32_t)n);
    put32(hdr + 4, (uint32_t)(payload - base));
    if (n) memcpy(payload, data, n);
    return payload + n;
}

size_t ntlm_user_auth(uint8_t *o, size_t cap, const uint8_t *c, size_t clen, const char *user, const char *domain,
                      const char *password, const uint8_t rnd[24], uint8_t key[16]) {
    if (clen < 48 || memcmp(c, "NTLMSSP", 8) || le32(c + 8) != 2) return 0;
    uint32_t sflags = le32(c + 20), al = get16(c + 40), ao = le32(c + 44);
    if (ao > clen || al > clen - ao || al > 2048) return 0;
    const uint8_t *av = c + ao;
    /* the server's time when it gave one; a client's own clock isn't known to agree with it */
    uint64_t ft = 0;
    for (size_t p = 0; p + 4 <= al;) {
        uint32_t id = get16(av + p), l = get16(av + p + 2);
        if (!id) break;
        if (p + 4 + l > al) return 0;
        if (id == 7 && l == 8) ft = (uint64_t)le32(av + p + 4) | (uint64_t)le32(av + p + 8) << 32;
        p += 4 + (size_t)l;
    }
    uint8_t hash[16], temp[3072], proof[16], base[16], nt[3100], ub[512], db[512], wb[32];
    size_t ul, dl, wl = text_utf16("LORDOS", wb, sizeof wb, 0);
    if (ntlmv2_hash(user, domain, password, hash) || field16(user, ub, sizeof ub, 0, &ul) || field16(domain, db, sizeof db, 0, &dl)) return 0;
    size_t tl = ntlmv2_temp(temp, sizeof temp, ft, rnd, av, al);
    if (!tl) return 0;
    ntlmv2_proof(hash, c + 24, temp, tl, proof, base);
    memset(hash, 0, sizeof hash);
    memcpy(nt, proof, 16);
    memcpy(nt + 16, temp, tl);
    size_t need = 64 + 24 + 16 + tl + ul + dl + wl + 16;
    if (need > cap) return 0;
    memset(o, 0, 64);
    memcpy(o, "NTLMSSP", 8);
    put32(o + 8, 3);
    uint8_t zero[24] = { 0 }, enc[16], *p = o + 64;
    p = field(o + 12, p, o, zero, 24);
    p = field(o + 20, p, o, nt, 16 + tl);
    p = field(o + 28, p, o, db, dl);
    p = field(o + 36, p, o, ub, ul);
    p = field(o + 44, p, o, wb, wl);
    uint32_t flags = FLAGS;
    if (sflags & F_KEYX) { memcpy(key, rnd + 8, 16); rc4(base, key, enc); p = field(o + 52, p, o, enc, 16); }
    else { flags &= ~F_KEYX; memcpy(key, base, 16); p = field(o + 52, p, o, enc, 0); }
    put32(o + 60, flags);
    memset(base, 0, sizeof base);
    return (size_t)(p - o);
}

void smb_sign(uint8_t *m, size_t len, const uint8_t key[16]) {
    uint8_t mac[32];
    m[16] |= 8;                         /* SMB2_FLAGS_SIGNED */
    memset(m + 48, 0, 16);
    hmac_sha256(key, 16, m, len, mac);
    memcpy(m + 48, mac, 16);
}
