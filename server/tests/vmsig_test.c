/* vmsig_test.c — the OpenPGP check of a person's system (src/vm/vmsig.c over pkg/pgp.c) with keys and signatures this
 * test makes itself (Ed25519, OpenPGP v4): a good signature, a changed checksum file, a signature by another key, a key
 * block that holds both keys, a fingerprint that isn't the key's, a signature from the future, an armored key, broken
 * input; subkeys and self-signatures of the pasted key verified (grafted, falsely bound, without back-signature,
 * expired, revoked; the makers' real keys and Debian's archive keys still read); lord-server's strict reading of the
 * helper's answer and the SHA-256 lookup in the checksum file; then, given its path, lord-pgpv itself on sealed memfds
 * (in its sandbox). Under ASan and UBSan (mk/pgpv.mk). */
#include "../src/vm/vmsig.h"
#include "../src/pkg/pgp.h"
#include "../src/pkg/crypto.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* pkt: the bare key packet; blk: the key as a block a person would paste (key, user ID, a verified self-signature) */
typedef struct { uint8_t seed[32], pub[32]; uint8_t pkt[64]; size_t pktlen; char fpr[41]; uint8_t blk[512]; size_t blklen; } Key;

static const uint8_t ED_OID[] = { 0x2B, 0x06, 0x01, 0x04, 0x01, 0xDA, 0x47, 0x0F, 0x01 };

/* a v4 EdDSA public key packet (tag 6) and its fingerprint, read back through pgp.c */
static void make_key(Key *k, uint8_t fill, uint32_t created) {
    for (int i = 0; i < 32; i++) k->seed[i] = (uint8_t)(fill + i * 7);
    ed25519_public(k->seed, k->pub);
    uint8_t body[64];
    size_t n = 0;
    body[n++] = 4;
    body[n++] = (uint8_t)(created >> 24); body[n++] = (uint8_t)(created >> 16); body[n++] = (uint8_t)(created >> 8); body[n++] = (uint8_t)created;
    body[n++] = 22;
    body[n++] = sizeof ED_OID;
    memcpy(body + n, ED_OID, sizeof ED_OID); n += sizeof ED_OID;
    body[n++] = 0x01; body[n++] = 0x07;              /* 263 bits: 0x40 and the point */
    body[n++] = 0x40;
    memcpy(body + n, k->pub, 32); n += 32;
    k->pkt[0] = 0xC6;
    k->pkt[1] = (uint8_t)n;
    memcpy(k->pkt + 2, body, n);
    k->pktlen = n + 2;
    PgpKeyring kr = { 0 };
    if (pgp_keyring_add_mem(&kr, (const char *)k->pkt, k->pktlen) || kr.n != 1) { fprintf(stderr, "key not read back\n"); exit(1); }
    hex_encode(kr.keys[0].fpr, 20, k->fpr);
    for (int i = 0; i < 40; i++) if (k->fpr[i] >= 'a') k->fpr[i] = (char)(k->fpr[i] - 32);
    pgp_keyring_free(&kr);
}

static void self_sign(Key *k, const Key *signer, uint8_t flags, uint32_t created, uint32_t expires_in);
static void make_signed_key(Key *k, uint8_t fill, uint32_t created) { make_key(k, fill, created); self_sign(k, k, 0x03, created, 0); }

static uint8_t fpr_bytes(const char *h, int i) {
    char b[3] = { h[2 * i], h[2 * i + 1], 0 };
    return (uint8_t)strtoul(b, NULL, 16);
}

/* a new-format packet */
static size_t put_pkt(uint8_t *out, int tag, const uint8_t *b, size_t n) {
    size_t o = 0;
    out[o++] = (uint8_t)(0xC0 | tag);
    if (n < 192) out[o++] = (uint8_t)n;
    else { out[o++] = (uint8_t)(((n - 192) >> 8) + 192); out[o++] = (uint8_t)(n - 192); }
    memcpy(out + o, b, n);
    return o + n;
}

/* the key packet as signatures over keys hash it: 0x99, 2-byte length, body */
static size_t key_hash(const Key *k, uint8_t *out) {
    size_t n = k->pktlen - 2;
    out[0] = 0x99; out[1] = (uint8_t)(n >> 8); out[2] = (uint8_t)n;
    memcpy(out + 3, k->pkt + 2, n);
    return n + 3;
}

/* the body of a v4 signature by `signer` (Ed25519, SHA-256) of `type` over parts, naming `issuer` (the signer's
 * fingerprint unless forged), with extra hashed subpackets and an unhashed area */
static size_t sig_body(const Key *signer, uint8_t type, uint32_t created, const char *issuer, const uint8_t *hsub, size_t hsl,
                       const uint8_t *usub, size_t usl, const uint8_t *const parts[], const size_t lens[], int np, uint8_t *out) {
    uint8_t head[128];
    size_t hn = 0;
    head[hn++] = 4; head[hn++] = type; head[hn++] = 22; head[hn++] = HASH_SHA256;
    size_t hl_at = hn;
    hn += 2;
    head[hn++] = 5; head[hn++] = 2;
    head[hn++] = (uint8_t)(created >> 24); head[hn++] = (uint8_t)(created >> 16); head[hn++] = (uint8_t)(created >> 8); head[hn++] = (uint8_t)created;
    head[hn++] = 22; head[hn++] = 33; head[hn++] = 4;
    for (int i = 0; i < 20; i++) head[hn++] = fpr_bytes(issuer ? issuer : signer->fpr, i);
    if (hsl) memcpy(head + hn, hsub, hsl);
    hn += hsl;
    size_t hl = hn - hl_at - 2;
    head[hl_at] = (uint8_t)(hl >> 8); head[hl_at + 1] = (uint8_t)hl;
    uint8_t trailer[6] = { 4, 0xff, (uint8_t)(hn >> 24), (uint8_t)(hn >> 16), (uint8_t)(hn >> 8), (uint8_t)hn }, dig[32], sig[64];
    Sha256 h;
    sha256_init(&h);
    for (int i = 0; i < np; i++) sha256_update(&h, parts[i], lens[i]);
    sha256_update(&h, head, hn);
    sha256_update(&h, trailer, 6);
    sha256_final(&h, dig);
    ed25519_sign(signer->seed, dig, 32, sig);
    size_t n = 0;
    memcpy(out, head, hn); n = hn;
    out[n++] = (uint8_t)(usl >> 8); out[n++] = (uint8_t)usl;
    if (usl) memcpy(out + n, usub, usl);
    n += usl;
    out[n++] = dig[0]; out[n++] = dig[1];
    out[n++] = 0x01; out[n++] = 0x00; memcpy(out + n, sig, 32); n += 32;
    out[n++] = 0x01; out[n++] = 0x00; memcpy(out + n, sig + 32, 32); n += 32;
    return n;
}

static const char UID[] = "Test system <test@example.org>";

/* k's block: its key packet, a user ID and a positive certification (0x13) with key flags `flags`, made by `signer`
 * (k itself unless forged) naming k as issuer; expires_in > 0 adds a key expiration time */
static void self_sign(Key *k, const Key *signer, uint8_t flags, uint32_t created, uint32_t expires_in) {
    uint8_t kh[80], uh[5 + sizeof UID], hs[16], body[400];
    size_t khl = key_hash(k, kh), hsl = 0, ul = sizeof UID - 1;
    uh[0] = 0xB4; uh[1] = 0; uh[2] = 0; uh[3] = 0; uh[4] = (uint8_t)ul;
    memcpy(uh + 5, UID, ul);
    hs[hsl++] = 2; hs[hsl++] = 27; hs[hsl++] = flags;
    if (expires_in) { hs[hsl++] = 5; hs[hsl++] = 9; hs[hsl++] = (uint8_t)(expires_in >> 24); hs[hsl++] = (uint8_t)(expires_in >> 16); hs[hsl++] = (uint8_t)(expires_in >> 8); hs[hsl++] = (uint8_t)expires_in; }
    const uint8_t *parts[2] = { kh, uh };
    size_t lens[2] = { khl, 5 + ul };
    size_t bl = sig_body(signer, 0x13, created, k->fpr, hs, hsl, NULL, 0, parts, lens, 2, body);
    size_t o = 0;
    memcpy(k->blk, k->pkt, k->pktlen); o = k->pktlen;
    o += put_pkt(k->blk + o, 13, (const uint8_t *)UID, ul);
    o += put_pkt(k->blk + o, 2, body, bl);
    k->blklen = o;
}

/* appends subkey `sub` to the block in out (holding olen bytes) with a binding (0x18) by `binder` naming `issuer`, key
 * flags `flags`, a back-signature (0x19) by `back` when not NULL, an expiration when expires_in > 0; returns the length */
static size_t add_subkey(uint8_t *out, size_t olen, const Key *primary, const Key *sub, const Key *binder, const char *issuer,
                         uint8_t flags, const Key *back, uint32_t created, uint32_t expires_in) {
    uint8_t kh[80], sh[80], hs[16], us[200], body[400], emb[200];
    size_t khl = key_hash(primary, kh), shl = key_hash(sub, sh), hsl = 0, usl = 0;
    const uint8_t *parts[2] = { kh, sh };
    size_t lens[2] = { khl, shl };
    hs[hsl++] = 2; hs[hsl++] = 27; hs[hsl++] = flags;
    if (expires_in) { hs[hsl++] = 5; hs[hsl++] = 9; hs[hsl++] = (uint8_t)(expires_in >> 24); hs[hsl++] = (uint8_t)(expires_in >> 16); hs[hsl++] = (uint8_t)(expires_in >> 8); hs[hsl++] = (uint8_t)expires_in; }
    if (back) {
        size_t el = sig_body(back, 0x19, created, NULL, NULL, 0, NULL, 0, parts, lens, 2, emb);
        us[usl++] = (uint8_t)(el + 1);                /* < 192 for Ed25519 */
        us[usl++] = 32;
        memcpy(us + usl, emb, el); usl += el;
    }
    size_t bl = sig_body(binder, 0x18, created, issuer, hs, hsl, us, usl, parts, lens, 2, body);
    uint8_t subpkt[64];
    memcpy(subpkt, sub->pkt, sub->pktlen);
    subpkt[0] = 0xCE;                                 /* tag 14: public subkey */
    memcpy(out + olen, subpkt, sub->pktlen); olen += sub->pktlen;
    olen += put_pkt(out + olen, 2, body, bl);
    return olen;
}

/* appends a subkey revocation (0x28) of `sub` by `signer` */
static size_t add_subkey_revocation(uint8_t *out, size_t olen, const Key *primary, const Key *sub, const Key *signer, const char *issuer, uint32_t created) {
    uint8_t kh[80], sh[80], body[400];
    size_t khl = key_hash(primary, kh), shl = key_hash(sub, sh);
    const uint8_t *parts[2] = { kh, sh };
    size_t lens[2] = { khl, shl };
    size_t bl = sig_body(signer, 0x28, created, issuer, NULL, 0, NULL, 0, parts, lens, 2, body);
    return olen + put_pkt(out + olen, 2, body, bl);
}

/* a v4 binary (0x00) detached signature packet by k over data, made at `created` */
static size_t make_sig(const Key *k, const char *data, size_t dl, uint32_t created, uint8_t *out) {
    uint8_t head[64];
    size_t hn = 0;
    head[hn++] = 4; head[hn++] = 0x00; head[hn++] = 22; head[hn++] = HASH_SHA256;
    size_t hl_at = hn;
    hn += 2;
    head[hn++] = 5; head[hn++] = 2;                  /* creation time */
    head[hn++] = (uint8_t)(created >> 24); head[hn++] = (uint8_t)(created >> 16); head[hn++] = (uint8_t)(created >> 8); head[hn++] = (uint8_t)created;
    head[hn++] = 22; head[hn++] = 33; head[hn++] = 4; /* issuer fingerprint */
    for (int i = 0; i < 20; i++) head[hn++] = fpr_bytes(k->fpr, i);
    size_t hl = hn - hl_at - 2;
    head[hl_at] = (uint8_t)(hl >> 8); head[hl_at + 1] = (uint8_t)hl;
    uint8_t trailer[6] = { 4, 0xff, (uint8_t)(hn >> 24), (uint8_t)(hn >> 16), (uint8_t)(hn >> 8), (uint8_t)hn }, dig[32], sig[64];
    Sha256 h;
    sha256_init(&h);
    sha256_update(&h, data, dl);
    sha256_update(&h, head, hn);
    sha256_update(&h, trailer, 6);
    sha256_final(&h, dig);
    ed25519_sign(k->seed, dig, 32, sig);
    uint8_t body[200];
    size_t n = 0;
    memcpy(body, head, hn); n = hn;
    body[n++] = 0; body[n++] = 0;                    /* no unhashed subpackets */
    body[n++] = dig[0]; body[n++] = dig[1];
    body[n++] = 0x01; body[n++] = 0x00; memcpy(body + n, sig, 32); n += 32;
    body[n++] = 0x01; body[n++] = 0x00; memcpy(body + n, sig + 32, 32); n += 32;
    out[0] = 0xC2;
    out[1] = (uint8_t)n;
    memcpy(out + 2, body, n);
    return n + 2;
}

/* ASCII armor of a public key block (base64 and its CRC-24) */
static size_t armor(const uint8_t *in, size_t n, char *out) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = (size_t)sprintf(out, "-----BEGIN PGP PUBLIC KEY BLOCK-----\n\n");
    int col = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = b64[v >> 18 & 63]; out[o++] = b64[v >> 12 & 63];
        out[o++] = i + 1 < n ? b64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? b64[v & 63] : '=';
        if ((col += 4) == 64) { out[o++] = '\n'; col = 0; }
    }
    if (col) out[o++] = '\n';
    uint32_t crc = 0xB704CE;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint32_t)in[i] << 16;
        for (int b = 0; b < 8; b++) { crc <<= 1; if (crc & 0x1000000) crc ^= 0x1864CFB; }
    }
    crc &= 0xFFFFFF;
    out[o++] = '=';
    out[o++] = b64[crc >> 18 & 63]; out[o++] = b64[crc >> 12 & 63]; out[o++] = b64[crc >> 6 & 63]; out[o++] = b64[crc & 63];
    o += (size_t)sprintf(out + o, "\n-----END PGP PUBLIC KEY BLOCK-----\n");
    return o;
}

static const char SUMS[] =
    "1111111111111111111111111111111111111111111111111111111111111111  other.iso\n"
    "9d4e8f2a7c1b3e5d6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6 *linuxmint-22.1-cinnamon-64bit.iso\r\n"
    "SHA256 (bsd-style.iso) = abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd\n";
#define ISO_SHA "9d4e8f2a7c1b3e5d6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6"

static int verify(const Key *key, const char *keybytes, size_t kl, const char *sums, size_t sl, const uint8_t *sig, size_t gl, const char *fpr, int64_t now, char *why) {
    char got[41];
    int ok = vmsig_verify(keybytes, kl, sums, sl, (const char *)sig, gl, fpr, now, got, why, 32);
    if (ok) CHECK(key && !strcmp(got, key->fpr), "answers the key's fingerprint");
    return ok;
}

static void test_verify(void) {
    const uint32_t T0 = 1700000000u;
    const int64_t now = (int64_t)T0 + 3600;
    Key a, b;
    make_signed_key(&a, 0x11, T0 - 1000);
    make_signed_key(&b, 0x5a, T0 - 1000);
    CHECK(strcmp(a.fpr, b.fpr) && vmsig_fpr_ok(a.fpr), "two keys");
    uint8_t sa[200], sb[200], sf[200];
    size_t sal = make_sig(&a, SUMS, sizeof SUMS - 1, T0, sa), sbl = make_sig(&b, SUMS, sizeof SUMS - 1, T0, sb);
    char why[32];

    CHECK(verify(&a, (const char *)a.blk, a.blklen, SUMS, sizeof SUMS - 1, sa, sal, a.fpr, now, why), "good signature (%s)", why);
    char lower[41];
    for (int i = 0; i < 41; i++) lower[i] = (char)(a.fpr[i] >= 'A' && a.fpr[i] <= 'F' ? a.fpr[i] + 32 : a.fpr[i]);
    CHECK(verify(&a, (const char *)a.blk, a.blklen, SUMS, sizeof SUMS - 1, sa, sal, lower, now, why), "fingerprint in lower case");

    /* a changed checksum file */
    char changed[sizeof SUMS];
    memcpy(changed, SUMS, sizeof SUMS);
    changed[80] ^= 1;
    CHECK(!verify(NULL, (const char *)a.blk, a.blklen, changed, sizeof SUMS - 1, sa, sal, a.fpr, now, why) && !strcmp(why, "badSignature"), "changed list: %s", why);
    /* a changed signature */
    uint8_t bad[200];
    memcpy(bad, sa, sal);
    bad[sal - 5] ^= 0x40;
    CHECK(!verify(NULL, (const char *)a.blk, a.blklen, SUMS, sizeof SUMS - 1, bad, sal, a.fpr, now, why), "changed signature: %s", why);

    /* signed by another key: B's signature, A's key and fingerprint */
    CHECK(!verify(NULL, (const char *)a.blk, a.blklen, SUMS, sizeof SUMS - 1, sb, sbl, a.fpr, now, why) && !strcmp(why, "otherKey"), "foreign key: %s", why);
    /* a key block holding both: only the named key counts */
    uint8_t both[1024];
    memcpy(both, a.blk, a.blklen);
    memcpy(both + a.blklen, b.blk, b.blklen);
    CHECK(!verify(NULL, (const char *)both, a.blklen + b.blklen, SUMS, sizeof SUMS - 1, sb, sbl, a.fpr, now, why) && !strcmp(why, "otherKey"), "B signed, A named: %s", why);
    CHECK(verify(&b, (const char *)both, a.blklen + b.blklen, SUMS, sizeof SUMS - 1, sb, sbl, b.fpr, now, why), "B signed, B named (%s)", why);
    /* a fingerprint that isn't in the block */
    CHECK(!verify(NULL, (const char *)a.blk, a.blklen, SUMS, sizeof SUMS - 1, sa, sal, b.fpr, now, why) && !strcmp(why, "noKey"), "fingerprint not in the key: %s", why);
    /* a v6 fingerprint (64 hex): pgp.c reads v4 only */
    CHECK(!verify(NULL, (const char *)a.blk, a.blklen, SUMS, sizeof SUMS - 1, sa, sal, ISO_SHA, now, why) && !strcmp(why, "unsupported"), "v6: %s", why);
    /* a signature made in the future */
    size_t sfl = make_sig(&a, SUMS, sizeof SUMS - 1, (uint32_t)(now + 3 * 86400), sf);
    CHECK(!verify(NULL, (const char *)a.blk, a.blklen, SUMS, sizeof SUMS - 1, sf, sfl, a.fpr, now, why) && !strcmp(why, "futureSignature"), "future: %s", why);
    /* armored key */
    char arm[2048];
    size_t al = armor(a.blk, a.blklen, arm);
    CHECK(verify(&a, arm, al, SUMS, sizeof SUMS - 1, sa, sal, a.fpr, now, why), "armored key (%s)", why);
    arm[60] = arm[60] == 'A' ? 'B' : 'A';
    CHECK(!verify(NULL, arm, al, SUMS, sizeof SUMS - 1, sa, sal, a.fpr, now, why), "a damaged armored key: %s", why);
    /* broken or missing input */
    CHECK(!verify(NULL, "", 0, SUMS, sizeof SUMS - 1, sa, sal, a.fpr, now, why) && !strcmp(why, "noKey"), "no key: %s", why);
    CHECK(!verify(NULL, "garbage", 7, SUMS, sizeof SUMS - 1, sa, sal, a.fpr, now, why), "garbage key: %s", why);
    CHECK(!verify(NULL, (const char *)a.blk, a.blklen, SUMS, sizeof SUMS - 1, sa, 0, a.fpr, now, why) && !strcmp(why, "malformed"), "empty signature: %s", why);
    CHECK(!verify(NULL, (const char *)a.blk, a.blklen, SUMS, sizeof SUMS - 1, sa, sal - 10, a.fpr, now, why), "cut signature: %s", why);
    CHECK(!verify(NULL, (const char *)a.pkt, VMSIG_KEY_MAX + 1, SUMS, sizeof SUMS - 1, sa, sal, a.fpr, now, why) && !strcmp(why, "tooLarge"), "key too large: %s", why);
    /* every truncation of the signature and the key: no crash, never valid */
    for (size_t l = 0; l < sal; l++) CHECK(!verify(NULL, (const char *)a.blk, a.blklen, SUMS, sizeof SUMS - 1, sa, l, a.fpr, now, why), "signature cut at %zu", l);
    for (size_t l = 0; l < a.blklen; l++) CHECK(!verify(NULL, (const char *)a.blk, l, SUMS, sizeof SUMS - 1, sa, sal, a.fpr, now, why), "key cut at %zu", l);
}

/* Subkeys and self-signatures of untrusted key material (Claude Security F3/F4, 2026-11-22): only what a verified
 * signature vouches for may sign under the pinned fingerprint. */
static void test_subkeys(void) {
    const uint32_t T0 = 1700000000u;
    const int64_t now = (int64_t)T0 + 3600;
    Key a, b, c;
    make_signed_key(&a, 0x11, T0 - 1000);
    make_signed_key(&b, 0x5a, T0 - 1000);
    make_key(&c, 0x77, T0 - 500);                     /* the subkey (an attacker's, or A's own) */
    uint8_t sc[200], sa[200], blk[2048];
    size_t scl = make_sig(&c, SUMS, sizeof SUMS - 1, T0, sc), sal = make_sig(&a, SUMS, sizeof SUMS - 1, T0, sa), bl;
    char why[32];
    #define BLK(...) (memcpy(blk, a.blk, a.blklen), bl = add_subkey(blk, a.blklen, __VA_ARGS__))
    #define VER(k) verify(k, (const char *)blk, bl, SUMS, sizeof SUMS - 1, sc, scl, a.fpr, now, why)

    /* a properly bound signing subkey: binding by A, key flag "sign", back-signature by the subkey */
    BLK(&a, &c, &a, NULL, 0x02, &c, T0 - 400, 0);
    CHECK(VER(&a), "bound signing subkey (%s)", why);
    CHECK(verify(&a, (const char *)blk, bl, SUMS, sizeof SUMS - 1, sa, sal, a.fpr, now, why), "the primary still signs (%s)", why);
    /* every truncation of that block: never valid with the subkey's signature */
    for (size_t l = 0; l < bl; l++)
        CHECK(!verify(NULL, (const char *)blk, l, SUMS, sizeof SUMS - 1, sc, scl, a.fpr, now, why), "subkey block cut at %zu", l);
    /* revoked by A: refused; a "revocation" made by B under A's name is not one */
    size_t whole = bl;
    bl = add_subkey_revocation(blk, whole, &a, &c, &a, NULL, T0 - 100);
    CHECK(!VER(NULL) && !strcmp(why, "keyRevoked"), "revoked subkey: %s", why);
    bl = add_subkey_revocation(blk, whole, &a, &c, &b, a.fpr, T0 - 100);
    CHECK(VER(&a), "a forged revocation is ignored (%s)", why);

    /* grafted: the subkey packet after A's genuine block, no binding at all */
    memcpy(blk, a.blk, a.blklen);
    memcpy(blk + a.blklen, c.pkt, c.pktlen);
    blk[a.blklen] = 0xCE;
    bl = a.blklen + c.pktlen;
    CHECK(!VER(NULL) && !strcmp(why, "otherKey"), "grafted subkey, no binding: %s", why);
    /* a fake binding: made by B, naming A as its issuer (with a real back-signature) */
    BLK(&a, &c, &b, a.fpr, 0x02, &c, T0 - 400, 0);
    CHECK(!VER(NULL) && !strcmp(why, "otherKey"), "binding signed by another key: %s", why);
    /* a binding made by B under its own name */
    BLK(&a, &c, &b, NULL, 0x02, &c, T0 - 400, 0);
    CHECK(!VER(NULL) && !strcmp(why, "otherKey"), "binding by B: %s", why);
    /* a real binding with the sign flag but no back-signature: the subkey never agreed */
    BLK(&a, &c, &a, NULL, 0x02, NULL, T0 - 400, 0);
    CHECK(!VER(NULL) && !strcmp(why, "keyNotSigning"), "no back-signature: %s", why);
    /* a back-signature made by another key */
    BLK(&a, &c, &a, NULL, 0x02, &b, T0 - 400, 0);
    CHECK(!VER(NULL) && !strcmp(why, "keyNotSigning"), "back-signature by B: %s", why);
    /* bound for encryption only (flags 0x0C), even with a back-signature */
    BLK(&a, &c, &a, NULL, 0x0C, &c, T0 - 400, 0);
    CHECK(!VER(NULL) && !strcmp(why, "keyNotSigning"), "encryption subkey: %s", why);
    /* expired: the binding gives 100 s of life */
    BLK(&a, &c, &a, NULL, 0x02, &c, T0 - 400, 100);
    CHECK(!VER(NULL) && !strcmp(why, "keyExpired"), "expired subkey: %s", why);
    /* a binding from before the subkey existed, or from the future, doesn't count */
    BLK(&a, &c, &a, NULL, 0x02, &c, T0 - 600, 0);
    CHECK(!VER(NULL) && !strcmp(why, "otherKey"), "binding older than the subkey: %s", why);
    BLK(&a, &c, &a, NULL, 0x02, &c, (uint32_t)(now + 3 * 86400), 0);
    CHECK(!VER(NULL) && !strcmp(why, "otherKey"), "binding from the future: %s", why);
    /* a bound subkey under a primary that expired: it lives no longer than its primary */
    Key ae = a;
    self_sign(&ae, &ae, 0x03, T0 - 900, 200);
    memcpy(blk, ae.blk, ae.blklen);
    bl = add_subkey(blk, ae.blklen, &ae, &c, &ae, NULL, 0x02, &c, T0 - 400, 0);
    CHECK(!VER(NULL) && !strcmp(why, "keyExpired"), "subkey of an expired primary: %s", why);
    #undef BLK
    #undef VER

    /* the primary key: no self-signature, a forged one, or one that doesn't allow signing */
    CHECK(!verify(NULL, (const char *)a.pkt, a.pktlen, SUMS, sizeof SUMS - 1, sa, sal, a.fpr, now, why) && !strcmp(why, "noKey"), "no self-signature: %s", why);
    Key af = a;
    self_sign(&af, &b, 0x03, T0 - 900, 0);
    CHECK(!verify(NULL, (const char *)af.blk, af.blklen, SUMS, sizeof SUMS - 1, sa, sal, a.fpr, now, why) && !strcmp(why, "noKey"), "self-signature by B: %s", why);
    Key ac = a;
    self_sign(&ac, &ac, 0x01, T0 - 900, 0);
    CHECK(!verify(NULL, (const char *)ac.blk, ac.blklen, SUMS, sizeof SUMS - 1, sa, sal, a.fpr, now, why) && !strcmp(why, "keyNotSigning"), "certify-only primary: %s", why);
    /* a forged newer self-signature after the genuine one changes nothing */
    uint8_t two[1024];
    memcpy(two, a.blk, a.blklen);
    memcpy(two + a.blklen, ac.blk + ac.pktlen, ac.blklen - ac.pktlen);   /* the certify-only one, made by A: genuine */
    size_t tl = a.blklen + ac.blklen - ac.pktlen;
    CHECK(!verify(NULL, (const char *)two, tl, SUMS, sizeof SUMS - 1, sa, sal, a.fpr, now, why) && !strcmp(why, "keyNotSigning"), "the newest genuine self-signature wins: %s", why);
    memcpy(two + a.blklen, af.blk + af.pktlen, af.blklen - af.pktlen);   /* B's, under A's name: ignored */
    tl = a.blklen + af.blklen - af.pktlen;
    CHECK(verify(&a, (const char *)two, tl, SUMS, sizeof SUMS - 1, sa, sal, a.fpr, now, why), "a forged self-signature is ignored (%s)", why);
    printf("vmsig-test: grafted, falsely bound, unconsenting, expired and revoked subkeys refused; a bound subkey verified\n");
}

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = malloc(4u << 20);
    *n = b ? fread(b, 1, 4u << 20, f) : 0;
    fclose(f);
    return b;
}

/* Real keys, read as untrusted material: the makers' keys Machines pins (their lists in tests/vmimage verify through
 * vmsig), and Debian's archive keys, which sign with subkeys bound with back-signatures. */
static void test_real_keys(void) {
    static const struct { const char *key, *sums, *sig, *fpr; } lists[] = {
        { "debian-cd.asc", "debian-amd64.sums", "debian-amd64.sig", "DF9B9C49EAA9298432589D76DA87E80D6294BE9B" },
        { "ubuntu-cdimage.asc", "ubuntu-amd64.sums", "ubuntu-amd64.sig", "843938DF228D22F7B3742BC0D94AA3F0EFE21092" },
        { "kali-archive.asc", "kali.sums", "kali.sig", "827C8569F2518CC677FECA1AED65462EC8D5E4C5" },
    };
    const int64_t now = 1794009600LL;                 /* 2026-11-07, as vmcatalog-test */
    int done = 0;
    for (size_t i = 0; i < sizeof lists / sizeof *lists; i++) {
        char path[256], why[32], got[41];
        size_t kl = 0, sl = 0, gl = 0;
        snprintf(path, sizeof path, "../packaging/keys/machines/%s", lists[i].key);
        char *k = slurp(path, &kl);
        snprintf(path, sizeof path, "tests/vmimage/%s", lists[i].sums);
        char *m = slurp(path, &sl);
        snprintf(path, sizeof path, "tests/vmimage/%s", lists[i].sig);
        char *g = slurp(path, &gl);
        if (k && m && g) {
            CHECK(vmsig_verify(k, kl, m, sl, g, gl, lists[i].fpr, now, got, why, sizeof why) && !strcmp(got, lists[i].fpr), "%s: real list (%s)", lists[i].key, why);
            m[0] ^= 1;
            CHECK(!vmsig_verify(k, kl, m, sl, g, gl, lists[i].fpr, now, got, why, sizeof why), "%s: changed list", lists[i].key);
            done++;
        }
        free(k); free(m); free(g);
    }
    /* debian-cd: its subkey is bound for encryption only, so it stays but may not sign */
    size_t kl = 0;
    char *k = slurp("../packaging/keys/machines/debian-cd.asc", &kl);
    if (k) {
        PgpKeyring kr = { 0 };
        CHECK(!pgp_keyring_add_untrusted(&kr, k, kl, now) && kr.n == 2 && kr.keys[0].primary && kr.keys[0].can_sign && !kr.keys[1].primary && !kr.keys[1].can_sign,
              "debian-cd: primary signs, encryption subkey kept but not signing (%d keys)", kr.n);
        pgp_keyring_free(&kr);
        free(k);
    }
    printf("vmsig-test: %d real lists verified with the makers' keys as untrusted input\n", done);
    /* Debian's archive keyrings (binary): every key and subkey the trusted reading sees is also bound when read as
     * untrusted, and the automatic signing subkeys may sign */
    static const char *const rings[] = { "/usr/share/keyrings/debian-archive-trixie-automatic.pgp", "/usr/share/keyrings/debian-archive-trixie-security-automatic.pgp",
                                         "/usr/share/keyrings/debian-archive-bookworm-automatic.pgp", "/usr/share/keyrings/debian-archive-trixie-stable.pgp" };
    for (size_t i = 0; i < sizeof rings / sizeof *rings; i++) {
        size_t n = 0;
        char *b = slurp(rings[i], &n);
        if (!b) { printf("vmsig-test: skipped %s: needs debian-archive-keyring\n", rings[i]); continue; }
        PgpKeyring t = { 0 }, u = { 0 };
        int signing_sub = 0;
        CHECK(!pgp_keyring_add_mem(&t, b, n) && !pgp_keyring_add_untrusted(&u, b, n, now), "%s read", rings[i]);
        CHECK(t.n == u.n && u.n > 0, "%s: %d keys trusted, %d untrusted", rings[i], t.n, u.n);
        for (int j = 0; j < u.n && j < t.n; j++) {
            CHECK(!memcmp(u.keys[j].fpr, t.keys[j].fpr, 20) && u.keys[j].can_sign == t.keys[j].can_sign && u.keys[j].expires == t.keys[j].expires,
                  "%s: key %d read the same way", rings[i], j);
            signing_sub += !u.keys[j].primary && u.keys[j].can_sign;
        }
        if (strstr(rings[i], "automatic")) CHECK(signing_sub > 0, "%s: a signing subkey", rings[i]);
        printf("vmsig-test: %s: %d keys, %d signing subkeys, all bound\n", rings[i], u.n, signing_sub);
        pgp_keyring_free(&t);
        pgp_keyring_free(&u);
        free(b);
    }
}

static void test_reply(void) {
    const char *F = "0123456789ABCDEF0123456789ABCDEF01234567";
    char why[32], line[96];
    vmsig_answer(1, F, line, sizeof line);
    CHECK(vmsig_reply(line, strlen(line), F, why, sizeof why) == 1, "valid answer read");
    CHECK(vmsig_reply(line, strlen(line), "0123456789abcdef0123456789abcdef01234567", why, sizeof why) == 1, "asked in lower case");
    CHECK(vmsig_reply(line, strlen(line), "1123456789ABCDEF0123456789ABCDEF01234567", why, sizeof why) == -1, "another fingerprint than asked");
    vmsig_answer(0, "otherKey", line, sizeof line);
    CHECK(vmsig_reply(line, strlen(line), F, why, sizeof why) == 0 && !strcmp(why, "otherKey"), "invalid answer read");
    static const char *const junk[] = { "", "valid\n", "valid 0123456789ABCDEF0123456789ABCDEF01234567", "valid 0123456789abcdef0123456789abcdef01234567\n",
        "valid 0123456789ABCDEF0123456789ABCDEF01234567 \n", "valid 0123456789ABCDEF0123456789ABCDEF01234567\nvalid x\n",
        "VALID 0123456789ABCDEF0123456789ABCDEF01234567\n", "invalid whatever\n", "invalid \n", "invalid otherKey extra\n",
        "valid 0123456789ABCDEF0123456789ABCDEF0123456Z\n", "<script>\n", NULL };
    for (int i = 0; junk[i]; i++) CHECK(vmsig_reply(junk[i], strlen(junk[i]), F, why, sizeof why) == -1 && !strcmp(why, "badSignature"), "junk refused: '%s'", junk[i]);
    char nul[] = "valid 0123456789ABCDEF0123456789ABCDEF01234567\n";
    nul[10] = 0;
    CHECK(vmsig_reply(nul, sizeof nul - 1, F, why, sizeof why) == -1, "a NUL in the answer");
}

static void test_sums(void) {
    CHECK(vmsig_sums_has_hash(SUMS, sizeof SUMS - 1, ISO_SHA), "GNU line with * and CRLF");
    CHECK(vmsig_sums_has_hash(SUMS, sizeof SUMS - 1, "9D4E8F2A7C1B3E5D6F708192A3B4C5D6E7F8091A2B3C4D5E6F708192A3B4C5D6"), "any case");
    CHECK(vmsig_sums_has_hash(SUMS, sizeof SUMS - 1, "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd"), "BSD line");
    CHECK(!vmsig_sums_has_hash(SUMS, sizeof SUMS - 1, "2222222222222222222222222222222222222222222222222222222222222222"), "not listed");
    CHECK(!vmsig_sums_has_hash(SUMS, sizeof SUMS - 1, "9d4e8f2a"), "a prefix");
    CHECK(!vmsig_sums_has_hash("9d4e8f2a7c1b3e5d6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6\n", 65, ISO_SHA), "a hash with no name");
    CHECK(!vmsig_sums_has_hash("# 9d4e8f2a7c1b3e5d6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6  x.iso\n", 70, ISO_SHA), "inside a comment");
    CHECK(!vmsig_sums_has_hash(NULL, 0, ISO_SHA), "no file");
}

/* lord-pgpv on sealed memfds, as lord-server starts it */
static int sealed(const void *b, size_t n) {
    int fd = memfd_create("t", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || write(fd, b, n) != (ssize_t)n || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) || lseek(fd, 0, SEEK_SET)) return -1;
    return fd;
}
static int run_helper(const char *bin, const void *key, size_t kl, const void *sig, size_t gl, const char *fpr, char *out, size_t on) {
    int k = sealed(key, kl), m = sealed(SUMS, sizeof SUMS - 1), g = sealed(sig, gl), in[2], o[2];
    if (k < 0 || m < 0 || g < 0 || pipe2(in, O_CLOEXEC) || pipe2(o, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (!pid) {
        int a = fcntl(in[0], F_DUPFD, 10), b = fcntl(o[1], F_DUPFD, 10), kk = fcntl(k, F_DUPFD, 10), mm = fcntl(m, F_DUPFD, 10), gg = fcntl(g, F_DUPFD, 10);
        int null = open("/dev/null", O_WRONLY);
        if (dup2(a, 0) != 0 || dup2(b, 1) != 1 || dup2(null, 2) != 2 || dup2(kk, 3) != 3 || dup2(mm, 4) != 4 || dup2(gg, 5) != 5) _exit(127);
        syscall(SYS_close_range, 6, ~0u, 0);
        char *const args[] = { "lord-pgpv", NULL }, *const env[] = { NULL };
        execve(bin, args, env);
        _exit(127);
    }
    close(in[0]); close(o[1]); close(k); close(m); close(g);
    char req[64];
    int rl = snprintf(req, sizeof req, "%s %lld\n", fpr, (long long)1700003600);
    if (write(in[1], req, (size_t)rl) != rl) return -1;
    close(in[1]);
    size_t n = 0;
    ssize_t r;
    while (n < on - 1 && (r = read(o[0], out + n, on - 1 - n)) > 0) n += (size_t)r;
    out[n] = 0;
    close(o[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return (int)n;
}

static void test_helper(const char *bin) {
    const uint32_t T0 = 1700000000u;
    Key a, b;
    make_signed_key(&a, 0x11, T0 - 1000);
    make_signed_key(&b, 0x5a, T0 - 1000);
    uint8_t sa[200], sb[200];
    size_t sal = make_sig(&a, SUMS, sizeof SUMS - 1, T0, sa), sbl = make_sig(&b, SUMS, sizeof SUMS - 1, T0, sb);
    char out[128], why[32], arm[2048];
    size_t al = armor(a.blk, a.blklen, arm);
    int n = run_helper(bin, arm, al, sa, sal, a.fpr, out, sizeof out);
    CHECK(n > 0 && vmsig_reply(out, (size_t)n, a.fpr, why, sizeof why) == 1, "lord-pgpv: good signature answered valid ('%s')", out);
    n = run_helper(bin, arm, al, sb, sbl, a.fpr, out, sizeof out);
    CHECK(n > 0 && vmsig_reply(out, (size_t)n, a.fpr, why, sizeof why) == 0 && !strcmp(why, "otherKey"), "lord-pgpv: foreign key ('%s')", out);
    uint8_t bad[200];
    memcpy(bad, sa, sal);
    bad[sal - 3] ^= 1;
    n = run_helper(bin, arm, al, bad, sal, a.fpr, out, sizeof out);
    CHECK(n > 0 && vmsig_reply(out, (size_t)n, a.fpr, why, sizeof why) == 0, "lord-pgpv: bad signature ('%s')", out);
    /* a subkey grafted under A's genuine block, with no binding: lord-pgpv must not answer valid A */
    Key c;
    make_key(&c, 0x77, T0 - 500);
    uint8_t sc[200], g[1024];
    size_t scl = make_sig(&c, SUMS, sizeof SUMS - 1, T0, sc);
    memcpy(g, a.blk, a.blklen);
    memcpy(g + a.blklen, c.pkt, c.pktlen);
    g[a.blklen] = 0xCE;
    n = run_helper(bin, g, a.blklen + c.pktlen, sc, scl, a.fpr, out, sizeof out);
    CHECK(n > 0 && vmsig_reply(out, (size_t)n, a.fpr, why, sizeof why) == 0 && !strcmp(why, "otherKey"), "lord-pgpv: grafted subkey ('%s')", out);
    printf("lord-pgpv: valid, foreign key, bad signature and grafted subkey answered as expected in its sandbox\n");
}

int main(int argc, char **argv) {
    test_verify();
    test_subkeys();
    test_real_keys();
    test_reply();
    test_sums();
    if (argc > 1) test_helper(argv[1]);
    if (fails) { fprintf(stderr, "vmsig-test: %d failed\n", fails); return 1; }
    printf("vmsig-test: all passed\n");
    return 0;
}
