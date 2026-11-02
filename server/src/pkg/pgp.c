/* pgp.c — OpenPGP v4 keyrings and signatures to verify repositories (see pgp.h).
 *
 * The keyring is taken as trusted (it's a root file installed by the debian-archive-keyring
 * package, just as gpgv does): from it we read the keys, their subkeys, what they're for
 * (key flags), when they expire and whether they're revoked, according to the signatures the
 * key itself made over itself. What arrives from the network (InRelease) is verified: a v4
 * signature over the canonical text, the key has to exist, be able to sign, not be revoked or
 * expired, and the signature can't be expired or come from the future. */
#include "pgp.h"
#include "../errcode.h"
#include "crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- SHA-1 (only for v4 fingerprints) ---- */

static void sha1(const uint8_t *const parts[], const size_t lens[], int nparts, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t blk[64];
    size_t bl = 0;
    uint64_t total = 0;
    for (int pass = 0; pass <= nparts; pass++) {
        const uint8_t *p;
        size_t n;
        uint8_t pad[72];
        if (pass < nparts) { p = parts[pass]; n = lens[pass]; total += n; }
        else {
            size_t padlen = (bl < 56 ? 56 : 120) - bl;
            memset(pad, 0, sizeof pad);
            pad[0] = 0x80;
            uint64_t bits = total * 8;
            for (int i = 0; i < 8; i++) pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
            p = pad;
            n = padlen + 8;
        }
        for (size_t i = 0; i < n; i++) {
            blk[bl++] = p[i];
            if (bl < 64) continue;
            bl = 0;
            uint32_t w[80];
            for (int t = 0; t < 16; t++) w[t] = (uint32_t)blk[4 * t] << 24 | (uint32_t)blk[4 * t + 1] << 16 | (uint32_t)blk[4 * t + 2] << 8 | blk[4 * t + 3];
            for (int t = 16; t < 80; t++) { uint32_t x = w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16]; w[t] = x << 1 | x >> 31; }
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int t = 0; t < 80; t++) {
                uint32_t f, k;
                if (t < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
                else if (t < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
                else if (t < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
                else { f = b ^ c ^ d; k = 0xCA62C1D6; }
                uint32_t tmp = (a << 5 | a >> 27) + f + e + k + w[t];
                e = d; d = c; c = b << 30 | b >> 2; b = a; a = tmp;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        }
    }
    for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

/* ---- packets ---- */

typedef struct { int tag; const uint8_t *body; size_t len; } Pkt;

static int next_packet(const uint8_t *d, size_t len, size_t *pos, Pkt *p) {
    size_t i = *pos;
    if (i >= len) return 0;
    uint8_t h = d[i++];
    if (!(h & 0x80)) return -1;
    size_t l;
    if (h & 0x40) {
        p->tag = h & 0x3f;
        if (i >= len) return -1;
        uint8_t a = d[i++];
        if (a < 192) l = a;
        else if (a < 224) { if (i >= len) return -1; l = ((size_t)(a - 192) << 8) + d[i++] + 192; }
        else if (a == 255) { if (i + 4 > len) return -1; l = (size_t)d[i] << 24 | (size_t)d[i + 1] << 16 | (size_t)d[i + 2] << 8 | d[i + 3]; i += 4; }
        else return -1;                 /* partial length: not used in keys or signatures */
    } else {
        p->tag = (h >> 2) & 0xf;
        int lt = h & 3;
        if (lt == 3) return -1;
        size_t nb = lt == 0 ? 1 : lt == 1 ? 2 : 4;
        if (i + nb > len) return -1;
        l = 0;
        for (size_t k = 0; k < nb; k++) l = l << 8 | d[i + k];
        i += nb;
    }
    if (l > len - i) return -1;
    p->body = d + i;
    p->len = l;
    *pos = i + l;
    return 1;
}

/* MPI: 2 bytes of bit count + the bytes. */
static int read_mpi(const uint8_t **p, const uint8_t *end, const uint8_t **v, size_t *vlen) {
    if (end - *p < 2) return -1;
    size_t bits = (size_t)(*p)[0] << 8 | (*p)[1];
    size_t n = (bits + 7) / 8;
    if ((size_t)(end - *p - 2) < n) return -1;
    *v = *p + 2;
    *vlen = n;
    *p += 2 + n;
    return 0;
}

typedef struct {
    int type, pubalg, hashalg;
    const uint8_t *head;                /* version … hashed subpackets */
    size_t headlen;
    uint8_t left16[2];
    const uint8_t *mpi, *end;
    int64_t created, sig_expires, key_expires;
    int key_flags, has_created;
    uint8_t issuer_fpr[20], issuer_id[8];
    int has_fpr, has_id;
} Sig;

static void parse_subpackets(Sig *s, const uint8_t *p, const uint8_t *end, int hashed) {
    while (p < end) {
        size_t l;
        if (*p < 192) l = *p++;
        else if (*p < 255) { if (end - p < 2) return; l = ((size_t)(p[0] - 192) << 8) + p[1] + 192; p += 2; }
        else { if (end - p < 5) return; l = (size_t)p[1] << 24 | (size_t)p[2] << 16 | (size_t)p[3] << 8 | p[4]; p += 5; }
        if (!l || (size_t)(end - p) < l) return;
        int type = p[0] & 0x7f;
        const uint8_t *v = p + 1;
        size_t vl = l - 1;
        uint32_t u32 = vl >= 4 ? (uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3] : 0;
        if (hashed) {                   /* what matters for security, only from the hashed area */
            if (type == 2 && vl >= 4) { s->created = u32; s->has_created = 1; }
            else if (type == 3 && vl >= 4) s->sig_expires = u32;
            else if (type == 9 && vl >= 4) s->key_expires = u32;
            else if (type == 27 && vl >= 1) s->key_flags = v[0];
        }
        if (type == 33 && vl == 21 && v[0] == 4) { memcpy(s->issuer_fpr, v + 1, 20); s->has_fpr = 1; }
        else if (type == 16 && vl == 8) { memcpy(s->issuer_id, v, 8); s->has_id = 1; }
        p += l;
    }
}

static int parse_sig(const uint8_t *b, size_t len, Sig *s) {
    memset(s, 0, sizeof *s);
    s->key_flags = -1;
    if (len < 6 || b[0] != 4) return -1;
    s->type = b[1];
    s->pubalg = b[2];
    s->hashalg = b[3];
    size_t hl = (size_t)b[4] << 8 | b[5];
    if (6 + hl + 2 > len) return -1;
    s->head = b;
    s->headlen = 6 + hl;
    parse_subpackets(s, b + 6, b + 6 + hl, 1);
    size_t ul = (size_t)b[6 + hl] << 8 | b[7 + hl];
    const uint8_t *u = b + 8 + hl;
    if ((size_t)(b + len - u) < ul + 2) return -1;
    parse_subpackets(s, u, u + ul, 0);
    s->left16[0] = u[ul];
    s->left16[1] = u[ul + 1];
    s->mpi = u + ul + 2;
    s->end = b + len;
    return 0;
}

static int issued_by(const Sig *s, const PgpKey *k) {
    if (s->has_fpr) return !memcmp(s->issuer_fpr, k->fpr, 20);
    if (s->has_id) return !memcmp(s->issuer_id, k->fpr + 12, 8);
    return 0;
}

/* ---- keyring ---- */

static int parse_key(const uint8_t *b, size_t len, PgpKey *k) {
    memset(k, 0, sizeof *k);
    if (len < 6 || b[0] != 4) return -1;
    k->created = (int64_t)((uint32_t)b[1] << 24 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 8 | b[4]);
    k->alg = b[5];
    const uint8_t *p = b + 6, *end = b + len;
    uint8_t hdr[3] = { 0x99, (uint8_t)(len >> 8), (uint8_t)len };
    const uint8_t *parts[2] = { hdr, b };
    size_t lens[2] = { 3, len };
    sha1(parts, lens, 2, k->fpr);
    if (k->alg == 1 || k->alg == 3) {
        k->alg = PGP_RSA;
        if (read_mpi(&p, end, &k->n, &k->nlen) || read_mpi(&p, end, &k->e, &k->elen)) return -1;
        return 0;
    }
    if (k->alg == PGP_EDDSA) {
        static const uint8_t oid[] = { 0x2B, 0x06, 0x01, 0x04, 0x01, 0xDA, 0x47, 0x0F, 0x01 };
        if (end - p < 1 || p[0] != sizeof oid || end - p < 1 + (long)sizeof oid || memcmp(p + 1, oid, sizeof oid)) return -1;
        p += 1 + sizeof oid;
        const uint8_t *v;
        size_t vl;
        if (read_mpi(&p, end, &v, &vl) || vl != 33 || v[0] != 0x40) return -1;
        memcpy(k->ed, v + 1, 32);
        return 0;
    }
    return 1;                           /* another algorithm: ignored */
}

static long dearmor(const char *in, size_t len, uint8_t **out);

long pgp_dearmor(const char *in, size_t len, uint8_t **out) {
    return len >= 5 && !memcmp(in, "-----", 5) ? dearmor(in, len, out) : -1;
}

int pgp_keyring_add_mem(PgpKeyring *kr, const char *data, size_t len) {
    uint8_t *buf;
    long n;
    if (len >= 5 && !memcmp(data, "-----", 5)) {
        n = dearmor(data, len, &buf);
        if (n < 0) return -1;
    } else {
        buf = malloc(len ? len : 1);
        if (!buf) return -1;
        memcpy(buf, data, len);
        n = (long)len;
    }
    uint8_t **nb = realloc(kr->bufs, sizeof *nb * (size_t)(kr->nbufs + 1));
    if (!nb) { free(buf); return -1; }
    kr->bufs = nb;
    kr->bufs[kr->nbufs++] = buf;

    size_t pos = 0;
    Pkt p;
    int primary = -1, cur = -1, r;
    int64_t best_self = -1, best_bind = -1;
    while ((r = next_packet(buf, (size_t)n, &pos, &p)) > 0) {
        if (p.tag == 6 || p.tag == 14) {
            PgpKey k;
            if (p.tag == 6) primary = -1;
            cur = -1;
            if (p.tag == 14 && primary < 0) continue;      /* subkey without a valid primary */
            if (parse_key(p.body, p.len, &k)) continue;
            k.primary = p.tag == 6;
            k.can_sign = 1;
            if (kr->n == kr->cap) {
                int cap = kr->cap ? kr->cap * 2 : 32;
                PgpKey *nk = realloc(kr->keys, sizeof *nk * (size_t)cap);
                if (!nk) return -1;
                kr->keys = nk;
                kr->cap = cap;
            }
            kr->keys[kr->n] = k;
            cur = kr->n++;
            if (p.tag == 6) { primary = cur; best_self = -1; }
            else best_bind = -1;
        } else if (p.tag == 2 && cur >= 0 && primary >= 0) {
            Sig s;
            if (parse_sig(p.body, p.len, &s) || !issued_by(&s, &kr->keys[primary])) continue;
            PgpKey *k = &kr->keys[cur];
            if (cur == primary) {
                if (s.type == 0x20) k->revoked = 1;
                else if ((s.type >= 0x10 && s.type <= 0x13) || s.type == 0x1f) {
                    if (s.created < best_self) continue;
                    best_self = s.created;
                    k->expires = s.key_expires ? k->created + s.key_expires : 0;
                    if (s.key_flags >= 0) k->can_sign = (s.key_flags & 0x02) != 0;
                }
            } else {
                if (s.type == 0x28) k->revoked = 1;
                else if (s.type == 0x18) {
                    if (s.created < best_bind) continue;
                    best_bind = s.created;
                    k->expires = s.key_expires ? k->created + s.key_expires : 0;
                    k->can_sign = s.key_flags >= 0 ? (s.key_flags & 0x02) != 0 : 1;
                }
            }
        }
        /* subkeys inherit their primary's revocation */
        if (primary >= 0 && kr->keys[primary].revoked)
            for (int i = primary; i < kr->n; i++) kr->keys[i].revoked = 1;
    }
    return r < 0 ? -1 : 0;
}

int pgp_keyring_add_file(PgpKeyring *kr, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 65536;
            if (cap > (16u << 20)) { free(buf); fclose(f); return -1; }
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(f); return -1; }
            buf = nb;
        }
        size_t r = fread(buf + len, 1, cap - len, f);
        if (!r) break;
        len += r;
    }
    fclose(f);
    int r = pgp_keyring_add_mem(kr, buf, len);
    free(buf);
    return r;
}

void pgp_keyring_free(PgpKeyring *kr) {
    for (int i = 0; i < kr->nbufs; i++) free(kr->bufs[i]);
    free(kr->bufs);
    free(kr->keys);
    memset(kr, 0, sizeof *kr);
}

/* ---- armor ---- */

long pgp_base64_decode(const char *in, size_t len, uint8_t *out, size_t cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        else return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (long)o;
}

static uint32_t crc24(const uint8_t *p, size_t n) {
    uint32_t crc = 0xB704CE;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint32_t)p[i] << 16;
        for (int j = 0; j < 8; j++) { crc <<= 1; if (crc & 0x1000000) crc ^= 0x1864CFB; }
    }
    return crc & 0xFFFFFF;
}

/* From "-----BEGIN ...-----" to "-----END": headers, empty line, base64 and "=CRC". */
static long dearmor(const char *in, size_t len, uint8_t **out) {
    const char *end = in + len, *p = memchr(in, '\n', len);
    if (!p) return -1;
    p++;
    /* "Key: value" headers up to an empty line (there may be none) */
    const char *q = p;
    int saw_header = 0;
    while (q < end) {
        const char *nl = memchr(q, '\n', (size_t)(end - q));
        if (!nl) return -1;
        size_t l = (size_t)(nl - q);
        if (l && q[l - 1] == '\r') l--;
        if (!l) { p = nl + 1; break; }
        if (!memchr(q, ':', l)) { if (!saw_header) break; return -1; }
        saw_header = 1;
        q = nl + 1;
    }
    const char *b64 = p, *b64end = p, *crc = NULL;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) nl = end;
        if (!strncmp(p, "-----END ", 9)) break;
        if (*p == '=') crc = p + 1;
        else b64end = nl;
        p = nl < end ? nl + 1 : end;
    }
    size_t cap = (size_t)(b64end - b64) * 3 / 4 + 4;
    *out = malloc(cap);
    if (!*out) return -1;
    long n = pgp_base64_decode(b64, (size_t)(b64end - b64), *out, cap);
    if (n >= 0 && crc) {
        uint8_t c[3];
        if (pgp_base64_decode(crc, 4, c, 3) != 3 || crc24(*out, (size_t)n) != ((uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2])) n = -1;
    }
    if (n < 0) { free(*out); *out = NULL; }
    return n;
}

/* ---- verification ---- */

static int verify_sig(const PgpKey *k, const Sig *s, const uint8_t *digest) {
    const uint8_t *p = s->mpi;
    if (k->alg == PGP_RSA && s->pubalg == 1) {
        const uint8_t *v;
        size_t vl;
        if (read_mpi(&p, s->end, &v, &vl)) return 0;
        return rsa_verify(k->n, k->nlen, k->e, k->elen, v, vl, s->hashalg, digest);
    }
    if (k->alg == PGP_EDDSA && s->pubalg == PGP_EDDSA) {
        const uint8_t *r, *sv;
        size_t rl, sl;
        if (read_mpi(&p, s->end, &r, &rl) || read_mpi(&p, s->end, &sv, &sl) || rl > 32 || sl > 32) return 0;
        uint8_t sig[64] = { 0 };
        memcpy(sig + 32 - rl, r, rl);
        memcpy(sig + 64 - sl, sv, sl);
        return ed25519_verify(k->ed, sig, digest, s->hashalg == HASH_SHA256 ? 32 : 64);
    }
    return 0;
}

static void hex_fpr(const uint8_t *f, char out[41]) { hex_encode(f, 20, out); }

int pgp_verify_clearsigned(const PgpKeyring *kr, const char *msg, size_t len, int64_t now,
                           char **body, size_t *body_len, char *err, size_t errlen) {
    static const char head[] = "-----BEGIN PGP SIGNED MESSAGE-----";
    static const char sighead[] = "-----BEGIN PGP SIGNATURE-----";
    *body = NULL;
    *body_len = 0;
    err[0] = 0;
    const char *end = msg + len, *p = msg;
    if (len < sizeof head || memcmp(msg, head, sizeof head - 1) || (msg[sizeof head - 1] != '\n' && msg[sizeof head - 1] != '\r')) {
        lerr_set(err, errlen, "pkg.pgp.notClearsigned", NULL);
        return -1;
    }
    p = memchr(msg, '\n', len) + 1;
    /* Hash: headers up to the empty line */
    for (;;) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) { lerr_set(err, errlen, "pkg.pgp.incomplete", NULL); return -1; }
        size_t l = (size_t)(nl - p);
        if (l && p[l - 1] == '\r') l--;
        p = nl + 1;
        if (!l) break;
    }
    /* text: up to the signature's armor */
    char *text = malloc((size_t)(end - p) + 2), *canon = malloc((size_t)(end - p) * 2 + 2);
    if (!text || !canon) { free(text); free(canon); lerr_set(err, errlen, "sys.outOfMemory", NULL); return -1; }
    size_t tl = 0, cl = 0;
    const char *sig = NULL;
    int first = 1;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        size_t l = (size_t)(le - p);
        if (l >= sizeof sighead - 1 && !memcmp(p, sighead, sizeof sighead - 1)) { sig = p; break; }
        const char *s = p;
        if (l >= 2 && s[0] == '-' && s[1] == ' ') { s += 2; l -= 2; }
        else if (l && s[0] == '-') { free(text); free(canon); lerr_set(err, errlen, "pkg.pgp.unescapedDash", NULL); return -1; }
        while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r')) l--;
        if (!first) { canon[cl++] = '\r'; canon[cl++] = '\n'; }
        first = 0;
        memcpy(canon + cl, s, l); cl += l;
        memcpy(text + tl, s, l); tl += l;
        text[tl++] = '\n';
        if (!nl) break;
        p = nl + 1;
    }
    if (!sig) { free(text); free(canon); lerr_set(err, errlen, "pkg.pgp.noSignature", NULL); return -1; }
    /* signature armor; after END there can only be whitespace */
    const char *endmark = NULL;
    for (const char *q = sig; q + 27 <= end; q++)
        if (!memcmp(q, "-----END PGP SIGNATURE-----", 27)) { endmark = q + 27; break; }
    if (!endmark) { free(text); free(canon); lerr_set(err, errlen, "pkg.pgp.signatureIncomplete", NULL); return -1; }
    for (const char *q = endmark; q < end; q++)
        if (*q != '\n' && *q != '\r' && *q != ' ' && *q != '\t') { free(text); free(canon); lerr_set(err, errlen, "pkg.pgp.trailingData", NULL); return -1; }
    uint8_t *raw;
    long rawlen = dearmor(sig, (size_t)(endmark - sig), &raw);
    if (rawlen < 0) { free(text); free(canon); lerr_set(err, errlen, "pkg.pgp.badEncoding", NULL); return -1; }

    int good = 0, bad = 0, unknown = 0;
    size_t pos = 0;
    Pkt pk;
    int r;
    char why[160] = "";
    while ((r = next_packet(raw, (size_t)rawlen, &pos, &pk)) > 0) {
        if (pk.tag != 2) continue;
        Sig s;
        if (parse_sig(pk.body, pk.len, &s) || (s.type != 0x01 && s.type != 0x00) ||
            (s.hashalg != HASH_SHA256 && s.hashalg != HASH_SHA512) || !s.has_created) { unknown++; continue; }
        const PgpKey *k = NULL;
        for (int i = 0; i < kr->n && !k; i++) if (issued_by(&s, &kr->keys[i])) k = &kr->keys[i];
        if (!k) { unknown++; continue; }
        char fpr[41];
        hex_fpr(k->fpr, fpr);
        if (k->revoked) { lerr_set(why, sizeof why, "pkg.pgp.keyRevoked", "key", fpr, NULL); continue; }
        if (k->expires && k->expires <= now) { lerr_set(why, sizeof why, "pkg.pgp.keyExpired", "key", fpr, NULL); continue; }
        if (!k->can_sign) { lerr_set(why, sizeof why, "pkg.pgp.keyNotSigning", "key", fpr, NULL); continue; }
        if (s.created > now + 86400) { lerr_set(why, sizeof why, "pkg.pgp.futureSignature", "key", fpr, NULL); continue; }
        if (s.sig_expires && s.created + s.sig_expires <= now) { lerr_set(why, sizeof why, "pkg.pgp.signatureExpired", "key", fpr, NULL); continue; }
        if (s.created < k->created) { bad++; continue; }
        /* hash: text (canonical if it's a text signature) + signature header + v4 trailer */
        uint8_t dig[64], trailer[6] = { 4, 0xff, (uint8_t)(s.headlen >> 24), (uint8_t)(s.headlen >> 16), (uint8_t)(s.headlen >> 8), (uint8_t)s.headlen };
        const char *data = s.type == 0x01 ? canon : text;
        size_t dlen = s.type == 0x01 ? cl : tl;
        if (s.hashalg == HASH_SHA256) {
            Sha256 h;
            sha256_init(&h);
            sha256_update(&h, data, dlen);
            sha256_update(&h, s.head, s.headlen);
            sha256_update(&h, trailer, 6);
            sha256_final(&h, dig);
        } else {
            Sha512 h;
            sha512_init(&h);
            sha512_update(&h, data, dlen);
            sha512_update(&h, s.head, s.headlen);
            sha512_update(&h, trailer, 6);
            sha512_final(&h, dig);
        }
        if (dig[0] != s.left16[0] || dig[1] != s.left16[1] || !verify_sig(k, &s, dig)) {
            bad++;
            lerr_set(why, sizeof why, "pkg.pgp.badSignature", "key", fpr, NULL);
            continue;
        }
        good++;
    }
    free(raw);
    free(canon);
    if (r < 0) { free(text); lerr_set(err, errlen, "pkg.pgp.malformed", NULL); return -1; }
    if (bad) { free(text); snprintf(err, errlen, "%s", why); return -1; }
    if (!good) {
        free(text);
        if (why[0]) snprintf(err, errlen, "%s", why);
        else lerr_set(err, errlen, "pkg.pgp.unknownKeys", "count", lerr_int(unknown), NULL);
        return 0;
    }
    *body = text;
    *body_len = tl;
    return good;
}
