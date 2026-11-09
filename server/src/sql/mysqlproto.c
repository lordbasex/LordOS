/* mysqlproto.c — see mysqlproto.h. */
#include "mysqlproto.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

static int fail(MyConn *c, int code, const char *why) {
    snprintf(c->why, sizeof c->why, "%s", why);
    return code;
}

/* ---- bounded reading ---- */

int my_rd_u8(MyRd *r, uint8_t *v) {
    if (r->n - r->pos < 1) return -1;
    *v = r->p[r->pos++];
    return 0;
}

int my_rd_u16(MyRd *r, uint16_t *v) {
    if (r->n - r->pos < 2) return -1;
    *v = (uint16_t)(r->p[r->pos] | r->p[r->pos + 1] << 8);
    r->pos += 2;
    return 0;
}

int my_rd_u24(MyRd *r, uint32_t *v) {
    if (r->n - r->pos < 3) return -1;
    *v = (uint32_t)r->p[r->pos] | (uint32_t)r->p[r->pos + 1] << 8 | (uint32_t)r->p[r->pos + 2] << 16;
    r->pos += 3;
    return 0;
}

int my_rd_u32(MyRd *r, uint32_t *v) {
    if (r->n - r->pos < 4) return -1;
    *v = (uint32_t)r->p[r->pos] | (uint32_t)r->p[r->pos + 1] << 8 | (uint32_t)r->p[r->pos + 2] << 16 | (uint32_t)r->p[r->pos + 3] << 24;
    r->pos += 4;
    return 0;
}

int my_rd_lenenc(MyRd *r, uint64_t *v, int *null) {
    uint8_t b;
    if (null) *null = 0;
    if (my_rd_u8(r, &b)) return -1;
    if (b < 0xfb) { *v = b; return 0; }
    if (b == 0xfb) { if (!null) return -1; *null = 1; *v = 0; return 0; }
    if (b == 0xfc) { uint16_t x; if (my_rd_u16(r, &x)) return -1; *v = x; return 0; }
    if (b == 0xfd) { uint32_t x; if (my_rd_u24(r, &x)) return -1; *v = x; return 0; }
    if (b == 0xfe) {
        uint32_t lo, hi;
        if (my_rd_u32(r, &lo) || my_rd_u32(r, &hi)) return -1;
        *v = (uint64_t)hi << 32 | lo;
        return 0;
    }
    return -1;                                   /* 0xff: not an integer */
}

int my_rd_fixed(MyRd *r, size_t n, MySlice *s) {
    if (r->n - r->pos < n) return -1;
    s->p = r->p + r->pos;
    s->n = n;
    r->pos += n;
    return 0;
}

int my_rd_lenenc_str(MyRd *r, MySlice *s, int *null) {
    uint64_t n;
    int isnull = 0;
    if (my_rd_lenenc(r, &n, null ? &isnull : NULL)) return -1;
    if (null) *null = isnull;
    if (isnull) { s->p = NULL; s->n = 0; return 0; }
    if (n > (uint64_t)(r->n - r->pos)) return -1;      /* also catches lengths past 2^32 */
    return my_rd_fixed(r, (size_t)n, s);
}

int my_rd_nulstr(MyRd *r, MySlice *s) {
    const uint8_t *z = memchr(r->p + r->pos, 0, r->n - r->pos);
    if (!z) return -1;
    s->p = r->p + r->pos;
    s->n = (size_t)(z - s->p);
    r->pos += s->n + 1;
    return 0;
}

/* a server-given name into a fixed buffer: printable ASCII only */
static void copy_name(char *out, size_t cap, const uint8_t *p, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < cap; i++) out[k++] = p[i] >= 0x20 && p[i] < 0x7f ? (char)p[i] : '?';
    out[k] = 0;
}

/* ---- packets ---- */

void my_init(MyConn *c) {
    memset(c, 0, sizeof *c);
    c->max = MY_PACKET_MAX;
}

void my_free(MyConn *c) {
    if (c->pkt) { explicit_bzero(c->pkt, c->cap); free(c->pkt); }
    c->pkt = NULL;
    c->len = c->cap = 0;
}

static int grow(MyConn *c, size_t need) {
    if (need <= c->cap) return 0;
    size_t nc = c->cap ? c->cap : 4096;
    while (nc < need) nc *= 2;
    if (nc > c->max + 1) nc = c->max + 1;
    if (nc < need) return -1;
    uint8_t *np = malloc(nc);
    if (!np) return -1;
    if (c->pkt) { memcpy(np, c->pkt, c->len); explicit_bzero(c->pkt, c->cap); free(c->pkt); }
    c->pkt = np;
    c->cap = nc;
    return 0;
}

int my_read_packet(MyConn *c) {
    size_t total = 0;
    int big = 0;
    c->len = 0;
    if (grow(c, 1)) return fail(c, MY_E_IO, "memory");
    for (;;) {
        uint8_t h[4];
        if (c->rd(c->io, h, 4)) return fail(c, MY_E_IO, "read");
        size_t n = (size_t)h[0] | (size_t)h[1] << 8 | (size_t)h[2] << 16;
        if (h[3] != c->seq) return fail(c, MY_E_SEQ, "packet out of sequence");
        c->seq++;
        if (!big && n > c->max - total) big = 1;
        if (big) {
            uint8_t skip[4096];
            for (size_t left = n; left;) {
                size_t k = left < sizeof skip ? left : sizeof skip;
                if (c->rd(c->io, skip, k)) return fail(c, MY_E_IO, "read");
                left -= k;
            }
        } else {
            if (grow(c, total + n + 1)) return fail(c, MY_E_IO, "memory");
            if (n && c->rd(c->io, c->pkt + total, n)) return fail(c, MY_E_IO, "read");
            total += n;
            c->len = total;
        }
        if (n < 0xffffff) break;                 /* a full-size piece continues in the next one */
    }
    if (big) { c->len = 0; c->pkt[0] = 0; return fail(c, MY_E_BIG, "packet too large"); }
    c->len = total;
    c->pkt[total] = 0;
    return MY_OK;
}

int my_write_packet(MyConn *c, const void *data, size_t n) {
    const uint8_t *p = data;
    for (;;) {
        size_t k = n < 0xffffff ? n : 0xffffff;
        uint8_t small[4 + 1024];
        uint8_t h[4] = { (uint8_t)k, (uint8_t)(k >> 8), (uint8_t)(k >> 16), c->seq++ };
        if (k <= 1024) {
            memcpy(small, h, 4);
            if (k) memcpy(small + 4, p, k);
            int r = c->wr(c->io, small, 4 + k);
            explicit_bzero(small, sizeof small);
            if (r) return fail(c, MY_E_IO, "write");
        } else if (c->wr(c->io, h, 4) || c->wr(c->io, p, k)) return fail(c, MY_E_IO, "write");
        p += k;
        n -= k;
        if (k < 0xffffff) return MY_OK;          /* exactly 0xffffff: an empty piece ends it */
    }
}

int my_parse_handshake(const uint8_t *p, size_t n, MyHandshake *h) {
    MyRd r = { p, n, 0 };
    uint8_t proto, filler, charset, alen;
    uint16_t lo, hi = 0, status = 0;
    MySlice ver, a1, a2, rsv, plug;
    memset(h, 0, sizeof *h);
    if (my_rd_u8(&r, &proto)) return MY_E_PROTO;
    if (proto == 0xff) return MY_E_SQL;
    if (proto != 10) return MY_E_UNSUPPORTED;
    if (my_rd_nulstr(&r, &ver) || my_rd_u32(&r, &h->thread_id) || my_rd_fixed(&r, 8, &a1) || my_rd_u8(&r, &filler) ||
        my_rd_u16(&r, &lo)) return MY_E_PROTO;
    h->caps = lo;
    if (r.pos < r.n) {
        if (my_rd_u8(&r, &charset) || my_rd_u16(&r, &status) || my_rd_u16(&r, &hi) || my_rd_u8(&r, &alen) ||
            my_rd_fixed(&r, 10, &rsv)) return MY_E_PROTO;
        h->charset = charset;
        h->status = status;
        h->caps |= (uint32_t)hi << 16;
        if (h->caps & MY_CLIENT_SECURE_CONNECTION) {
            size_t l2 = alen > 8 ? (size_t)alen - 8 : 0;
            if (l2 < 13) l2 = 13;
            if (my_rd_fixed(&r, l2, &a2)) return MY_E_PROTO;
            memcpy(h->scramble, a1.p, 8);
            memcpy(h->scramble + 8, a2.p, 12);
        }
        if (h->caps & MY_CLIENT_PLUGIN_AUTH) {
            /* some servers leave out the final NUL: then the name runs to the end */
            if (my_rd_nulstr(&r, &plug)) { plug.p = r.p + r.pos; plug.n = r.n - r.pos; r.pos = r.n; }
            copy_name(h->plugin, sizeof h->plugin, plug.p, plug.n);
        }
    }
    const uint8_t *v = ver.p;
    size_t vn = ver.n;
    h->mariadb = memmem(v, vn, "MariaDB", 7) != NULL;
    /* MariaDB 10+ announces itself as "5.5.5-10.x.y-MariaDB" to old clients */
    if (h->mariadb && vn > 6 && !memcmp(v, "5.5.5-", 6)) { v += 6; vn -= 6; }
    copy_name(h->version, sizeof h->version, v, vn);
    return MY_OK;
}

int my_parse_err(const uint8_t *p, size_t n, MyErr *e) {
    MyRd r = { p, n, 0 };
    uint8_t b;
    uint16_t code;
    memset(e, 0, sizeof *e);
    if (my_rd_u8(&r, &b) || b != 0xff || my_rd_u16(&r, &code)) return MY_E_PROTO;
    e->code = code;
    if (r.n - r.pos >= 6 && r.p[r.pos] == '#') {
        copy_name(e->state, sizeof e->state, r.p + r.pos + 1, 5);
        r.pos += 6;
    }
    size_t k = 0;
    for (size_t i = r.pos; i < r.n && k + 1 < sizeof e->msg && r.p[i]; i++) e->msg[k++] = (char)r.p[i];
    e->msg[k] = 0;
    return MY_OK;
}

int my_parse_ok(const uint8_t *p, size_t n, MyOk *o) {
    MyRd r = { p, n, 0 };
    uint8_t b;
    memset(o, 0, sizeof *o);
    if (my_rd_u8(&r, &b) || (b != 0x00 && b != 0xfe)) return MY_E_PROTO;
    if (my_rd_lenenc(&r, &o->affected, NULL) || my_rd_lenenc(&r, &o->insert_id, NULL) ||
        my_rd_u16(&r, &o->status) || my_rd_u16(&r, &o->warnings)) return MY_E_PROTO;
    o->info = r.p + r.pos;                       /* no CLIENT_SESSION_TRACK: the rest is the message */
    o->info_len = r.n - r.pos;
    return MY_OK;
}

int my_parse_eof(const uint8_t *p, size_t n, uint16_t *warnings, uint16_t *status) {
    MyRd r = { p, n, 0 };
    uint8_t b;
    *warnings = *status = 0;
    if (my_rd_u8(&r, &b) || b != 0xfe || n >= 9) return MY_E_PROTO;
    if (n >= 5 && (my_rd_u16(&r, warnings) || my_rd_u16(&r, status))) return MY_E_PROTO;
    return MY_OK;
}

int my_parse_column(const uint8_t *p, size_t n, MyColumn *col) {
    MyRd r = { p, n, 0 };
    uint64_t fixed;
    memset(col, 0, sizeof *col);
    if (my_rd_lenenc_str(&r, &col->catalog, NULL) || my_rd_lenenc_str(&r, &col->db, NULL) ||
        my_rd_lenenc_str(&r, &col->table, NULL) || my_rd_lenenc_str(&r, &col->org_table, NULL) ||
        my_rd_lenenc_str(&r, &col->name, NULL) || my_rd_lenenc_str(&r, &col->org_name, NULL) ||
        my_rd_lenenc(&r, &fixed, NULL) || fixed < 12 || fixed > (uint64_t)(r.n - r.pos) ||
        my_rd_u16(&r, &col->charset) || my_rd_u32(&r, &col->length) || my_rd_u8(&r, &col->type) ||
        my_rd_u16(&r, &col->flags) || my_rd_u8(&r, &col->decimals)) return MY_E_PROTO;
    return MY_OK;
}

int my_is_row_end(const MyConn *c) {
    if (c->len < 1 || c->pkt[0] != 0xfe) return 0;
    return (c->caps & MY_CLIENT_DEPRECATE_EOF) ? c->len < 0xffffff : c->len < 9;
}

int my_is_binary(uint8_t type, uint16_t charset) {
    if (charset != 63) return 0;
    switch (type) {
    case 15: case 16: case 249: case 250: case 251: case 252: case 253: case 254: case 255: return 1;
    default: return 0;
    }
}

const char *my_type_name(uint8_t type, uint16_t flags, uint16_t charset) {
    int bin = charset == 63;
    switch (type) {
    case 0: case 246: return "DECIMAL";
    case 1: return "TINYINT";
    case 2: return "SMALLINT";
    case 3: return "INT";
    case 4: return "FLOAT";
    case 5: return "DOUBLE";
    case 6: return "NULL";
    case 7: case 17: return "TIMESTAMP";
    case 8: return "BIGINT";
    case 9: return "MEDIUMINT";
    case 10: case 14: return "DATE";
    case 11: case 19: return "TIME";
    case 12: case 18: return "DATETIME";
    case 13: return "YEAR";
    case 15: case 253: return bin ? "VARBINARY" : "VARCHAR";
    case 16: return "BIT";
    case 245: return "JSON";
    case 247: return "ENUM";
    case 248: return "SET";
    case 249: return bin ? "TINYBLOB" : "TINYTEXT";
    case 250: return bin ? "MEDIUMBLOB" : "MEDIUMTEXT";
    case 251: return bin ? "LONGBLOB" : "LONGTEXT";
    case 252: return bin ? "BLOB" : "TEXT";
    case 254:
        if (flags & MY_ENUM) return "ENUM";
        if (flags & MY_SET) return "SET";
        return bin ? "BINARY" : "CHAR";
    case 255: return "GEOMETRY";
    default: return "UNKNOWN";
    }
}

/* ---- auth ---- */

static void digest(const EVP_MD *md, const void *a, size_t an, const void *b, size_t bn, uint8_t *out) {
    EVP_MD_CTX *x = EVP_MD_CTX_new();
    unsigned int k = 0;
    if (!x) { memset(out, 0, (size_t)EVP_MD_get_size(md)); return; }
    EVP_DigestInit_ex(x, md, NULL);
    if (an) EVP_DigestUpdate(x, a, an);
    if (bn) EVP_DigestUpdate(x, b, bn);
    EVP_DigestFinal_ex(x, out, &k);
    EVP_MD_CTX_free(x);
}

/* SHA1(pw) XOR SHA1(scramble + SHA1(SHA1(pw))) */
void my_scramble_native(const uint8_t *scr, size_t sl, const char *pw, uint8_t out[20]) {
    uint8_t h1[20], h2[20], h3[20];
    digest(EVP_sha1(), pw, strlen(pw), NULL, 0, h1);
    digest(EVP_sha1(), h1, 20, NULL, 0, h2);
    digest(EVP_sha1(), scr, sl, h2, 20, h3);
    for (int i = 0; i < 20; i++) out[i] = h1[i] ^ h3[i];
    explicit_bzero(h1, sizeof h1);
    explicit_bzero(h2, sizeof h2);
    explicit_bzero(h3, sizeof h3);
}

/* SHA256(pw) XOR SHA256(SHA256(SHA256(pw)) + scramble) */
void my_scramble_sha2(const uint8_t *scr, size_t sl, const char *pw, uint8_t out[32]) {
    uint8_t h1[32], h2[32], h3[32];
    digest(EVP_sha256(), pw, strlen(pw), NULL, 0, h1);
    digest(EVP_sha256(), h1, 32, NULL, 0, h2);
    digest(EVP_sha256(), h2, 32, scr, sl, h3);
    for (int i = 0; i < 32; i++) out[i] = h1[i] ^ h3[i];
    explicit_bzero(h1, sizeof h1);
    explicit_bzero(h2, sizeof h2);
    explicit_bzero(h3, sizeof h3);
}

int my_rsa_password(const uint8_t *pem, size_t pemlen, const char *pw, const uint8_t *scr, size_t sl,
                     uint8_t *out, size_t *outlen) {
    size_t pl = strlen(pw) + 1;
    if (!sl || pemlen > 16384 || pl > 512) return -1;
    BIO *b = BIO_new_mem_buf(pem, (int)pemlen);
    EVP_PKEY *k = b ? PEM_read_bio_PUBKEY(b, NULL, NULL, NULL) : NULL;
    BIO_free(b);
    if (!k) return -1;
    uint8_t buf[512];
    for (size_t i = 0; i < pl; i++) buf[i] = (uint8_t)((i + 1 < pl ? (uint8_t)pw[i] : 0) ^ scr[i % sl]);
    int rc = -1;
    EVP_PKEY_CTX *x = EVP_PKEY_CTX_new(k, NULL);
    size_t n = 0;
    if (x && EVP_PKEY_encrypt_init(x) == 1 && EVP_PKEY_CTX_set_rsa_padding(x, RSA_PKCS1_OAEP_PADDING) == 1 &&
        EVP_PKEY_encrypt(x, NULL, &n, buf, pl) == 1 && n <= 1024 && EVP_PKEY_encrypt(x, out, &n, buf, pl) == 1) {
        *outlen = n;
        rc = 0;
    }
    explicit_bzero(buf, sizeof buf);
    EVP_PKEY_CTX_free(x);
    EVP_PKEY_free(k);
    return rc;
}

/* the auth data of a plugin: 0 bytes for an empty password */
static size_t plugin_auth(const char *plugin, const uint8_t *scr, size_t sl, const char *pw, uint8_t out[32]) {
    if (!pw[0]) return 0;
    if (!strcmp(plugin, "caching_sha2_password")) { my_scramble_sha2(scr, sl, pw, out); return 32; }
    my_scramble_native(scr, sl, pw, out);
    return 20;
}

static int known_plugin(const char *p) { return !strcmp(p, "mysql_native_password") || !strcmp(p, "caching_sha2_password"); }

int my_connect(MyConn *c, const MyLogin *login, MyHandshake *hs) {
    int rc;
    c->seq = 0;
    if ((rc = my_read_packet(c))) return rc == MY_E_BIG ? fail(c, MY_E_PROTO, "greeting too large") : rc;
    rc = my_parse_handshake(c->pkt, c->len, hs);
    if (rc == MY_E_SQL) { if (my_parse_err(c->pkt, c->len, &c->err)) return fail(c, MY_E_PROTO, "bad error"); return fail(c, MY_E_SQL, "refused"); }
    if (rc == MY_E_UNSUPPORTED) return fail(c, rc, "protocol version");
    if (rc) return fail(c, rc, "bad greeting");
    if (!(hs->caps & MY_CLIENT_PROTOCOL_41) || !(hs->caps & MY_CLIENT_SECURE_CONNECTION)) return fail(c, MY_E_UNSUPPORTED, "server too old");
    const char *user = login->user ? login->user : "", *pw = login->password ? login->password : "", *db = login->db;
    size_t ul = strlen(user), dl = db ? strlen(db) : 0;
    if (ul > 256 || dl > 256 || strlen(pw) > 500) return fail(c, MY_E_PROTO, "argument too long");
    uint32_t want = MY_CLIENT_LONG_PASSWORD | MY_CLIENT_LONG_FLAG | MY_CLIENT_PROTOCOL_41 | MY_CLIENT_TRANSACTIONS |
                    MY_CLIENT_SECURE_CONNECTION | MY_CLIENT_MULTI_STATEMENTS | MY_CLIENT_MULTI_RESULTS |
                    MY_CLIENT_PS_MULTI_RESULTS | MY_CLIENT_PLUGIN_AUTH | MY_CLIENT_PLUGIN_AUTH_LENENC | MY_CLIENT_DEPRECATE_EOF;
    if (dl) want |= MY_CLIENT_CONNECT_WITH_DB;
    /* never CLIENT_LOCAL_FILES: a server can't ask for files */
    c->caps = want & hs->caps;
    c->caps |= MY_CLIENT_PROTOCOL_41 | MY_CLIENT_SECURE_CONNECTION;
    uint8_t head[32];
    memset(head, 0, sizeof head);
    if (login->tls && (hs->caps & MY_CLIENT_SSL)) {
        c->caps |= MY_CLIENT_SSL;
        uint32_t cp = c->caps, mx = 1u << 24;
        memcpy(head, (uint8_t[]){ (uint8_t)cp, (uint8_t)(cp >> 8), (uint8_t)(cp >> 16), (uint8_t)(cp >> 24),
                                  (uint8_t)mx, (uint8_t)(mx >> 8), (uint8_t)(mx >> 16), (uint8_t)(mx >> 24), 45 }, 9);
        if ((rc = my_write_packet(c, head, 32))) return rc;
        if (!c->tls) return fail(c, MY_E_TLS, "no TLS");
        if ((rc = c->tls(c->io))) return fail(c, rc, rc == MY_E_CERT ? "certificate" : "tls handshake");
        c->is_tls = 1;
    } else if (login->tls >= 2) return fail(c, MY_E_TLS, "server offers no TLS");
    char plugin[64];
    snprintf(plugin, sizeof plugin, "%s", known_plugin(hs->plugin) ? hs->plugin : "mysql_native_password");
    uint8_t scr[20], auth[32];
    size_t sl = 20;
    memcpy(scr, hs->scramble, 20);
    size_t al = plugin_auth(plugin, scr, sl, pw, auth);
    /* HandshakeResponse41 */
    size_t cap = 32 + ul + 1 + 1 + al + dl + 1 + sizeof plugin + 1;
    uint8_t *m = calloc(1, cap);
    if (!m) return fail(c, MY_E_OOM, "memory");
    uint32_t cp = c->caps, mx = 1u << 24;
    uint8_t h9[9] = { (uint8_t)cp, (uint8_t)(cp >> 8), (uint8_t)(cp >> 16), (uint8_t)(cp >> 24),
                      (uint8_t)mx, (uint8_t)(mx >> 8), (uint8_t)(mx >> 16), (uint8_t)(mx >> 24), 45 };
    memcpy(m, h9, 9);
    size_t o = 32;
    memcpy(m + o, user, ul);
    o += ul + 1;
    m[o++] = (uint8_t)al;                        /* lenenc or a length byte: both one byte here */
    memcpy(m + o, auth, al);
    o += al;
    if (c->caps & MY_CLIENT_CONNECT_WITH_DB) { memcpy(m + o, db, dl); o += dl + 1; }
    if (c->caps & MY_CLIENT_PLUGIN_AUTH) { size_t pl = strlen(plugin); memcpy(m + o, plugin, pl); o += pl + 1; }
    rc = my_write_packet(c, m, o);
    explicit_bzero(m, cap);
    free(m);
    explicit_bzero(auth, sizeof auth);
    if (rc) return rc;
    int switched = 0, want_key = 0;
    for (int round = 0; round < 6; round++) {
        if ((rc = my_read_packet(c))) { rc = rc == MY_E_BIG ? fail(c, MY_E_PROTO, "auth packet too large") : rc; goto out; }
        uint8_t t = c->len ? c->pkt[0] : 0xee;
        if (want_key) {
            /* the public key: 0x01 + PEM */
            if (t != 0x01 || c->len < 2) { rc = fail(c, MY_E_PROTO, "no public key"); goto out; }
            uint8_t enc[1024];
            size_t el = 0;
            if (my_rsa_password(c->pkt + 1, c->len - 1, pw, scr, sl, enc, &el)) { rc = fail(c, MY_E_AUTH, "bad public key"); goto out; }
            rc = my_write_packet(c, enc, el);
            if (rc) goto out;
            want_key = 0;
            continue;
        }
        if (t == 0x00) {
            MyOk ok;
            if (my_parse_ok(c->pkt, c->len, &ok)) { rc = fail(c, MY_E_PROTO, "bad OK"); goto out; }
            rc = MY_OK;
            goto out;
        }
        if (t == 0xff) {
            if (my_parse_err(c->pkt, c->len, &c->err)) { rc = fail(c, MY_E_PROTO, "bad error"); goto out; }
            rc = fail(c, MY_E_AUTH, "denied");
            goto out;
        }
        if (t == 0xfe) {
            /* auth switch: once, to a plugin we know */
            MyRd r = { c->pkt, c->len, 1 };
            MySlice name;
            if (switched++ || c->len == 1 || my_rd_nulstr(&r, &name)) { rc = fail(c, MY_E_PROTO, "bad auth switch"); goto out; }
            copy_name(plugin, sizeof plugin, name.p, name.n);
            if (!known_plugin(plugin)) { rc = fail(c, MY_E_UNSUPPORTED, "auth plugin"); goto out; }
            size_t dn = r.n - r.pos;
            if (dn && r.p[r.n - 1] == 0) dn--;
            if (dn < 20) { rc = fail(c, MY_E_PROTO, "short scramble"); goto out; }
            memcpy(scr, r.p + r.pos, 20);
            al = plugin_auth(plugin, scr, sl, pw, auth);
            rc = my_write_packet(c, auth, al);
            explicit_bzero(auth, sizeof auth);
            if (rc) goto out;
            continue;
        }
        if (t == 0x01 && !strcmp(plugin, "caching_sha2_password") && c->len == 2) {
            if (c->pkt[1] == 3) continue;        /* fast auth: the OK follows */
            if (c->pkt[1] == 4) {                /* full auth */
                if (c->is_tls) {
                    rc = my_write_packet(c, pw, strlen(pw) + 1);
                    if (rc) goto out;
                } else {
                    uint8_t ask = 0x02;
                    if ((rc = my_write_packet(c, &ask, 1))) goto out;
                    want_key = 1;
                }
                continue;
            }
        }
        rc = fail(c, MY_E_PROTO, "unexpected auth packet");
        goto out;
    }
    rc = fail(c, MY_E_PROTO, "too many auth rounds");
out:
    explicit_bzero(scr, sizeof scr);
    explicit_bzero(auth, sizeof auth);
    return rc;
}

/* ---- queries ---- */

/* ---- JSON output (MyBuf: like doc/wire's JBuf, with a larger limit for result sets) ---- */

static int mb_grow(MyBuf *b, size_t extra) {
    if (b->oom) return -1;
    size_t lim = b->limit ? b->limit : ((size_t)16 << 20);
    if (extra > lim || b->n + extra + 1 > lim) { b->oom = 1; return -1; }
    if (b->n + extra + 1 <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + extra + 1) cap *= 2;
    if (cap > lim) cap = lim;
    char *p = realloc(b->p, cap);
    if (!p) { b->oom = 1; return -1; }
    b->p = p;
    b->cap = cap;
    return 0;
}

void mb_init(MyBuf *b, size_t limit) { memset(b, 0, sizeof *b); b->limit = limit; }
void mb_free(MyBuf *b) { free(b->p); memset(b, 0, sizeof *b); }

void mb_rawn(MyBuf *b, const char *s, size_t n) {
    if (mb_grow(b, n)) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

void mb_raw(MyBuf *b, const char *s) { mb_rawn(b, s, strlen(s)); }

void mb_printf(MyBuf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= sizeof tmp) { b->oom = 1; return; }    /* only short numbers go through here */
    mb_rawn(b, tmp, (size_t)k);
}

/* length of a valid UTF-8 sequence at s (no overlongs, surrogates or past U+10FFFF), 0 if invalid */
static int utf8_len(const unsigned char *s, size_t n) {
    unsigned c = s[0];
    if (c < 0x80) return 1;
    int k = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 ? 2 : 0;
    if (!k || c > 0xf4 || (size_t)k > n) return 0;
    for (int i = 1; i < k; i++) if ((s[i] & 0xc0) != 0x80) return 0;
    if (k == 3 && ((c == 0xe0 && s[1] < 0xa0) || (c == 0xed && s[1] >= 0xa0))) return 0;
    if (k == 4 && ((c == 0xf0 && s[1] < 0x90) || (c == 0xf4 && s[1] >= 0x90))) return 0;
    return k;
}

/* "text" with escapes; invalid UTF-8 and NUL become U+FFFD (text columns; binary ones go as hex) */
void mb_strn(MyBuf *b, const char *str, size_t n) {
    const unsigned char *s = (const unsigned char *)str;
    mb_rawn(b, "\"", 1);
    size_t run = 0;                              /* plain bytes waiting to be copied at once */
    for (size_t i = 0; i < n;) {
        unsigned char ch = s[i];
        if (ch >= 0x20 && ch < 0x80 && ch != '"' && ch != '\\') { run++; i++; continue; }
        if (ch >= 0x80) {
            int k = utf8_len(s + i, n - i);
            if (k) { run += (size_t)k; i += (size_t)k; continue; }
        }
        if (run) { mb_rawn(b, (const char *)s + i - run, run); run = 0; }
        if (ch == '"' || ch == '\\') { char e[2] = { '\\', (char)ch }; mb_rawn(b, e, 2); }
        else if (ch == '\n') mb_rawn(b, "\\n", 2);
        else if (ch == '\t') mb_rawn(b, "\\t", 2);
        else if (ch == '\r') mb_rawn(b, "\\r", 2);
        else if (ch && ch < 0x20) { char e[8]; snprintf(e, sizeof e, "\\u%04x", ch); mb_rawn(b, e, 6); }
        else mb_rawn(b, "\xef\xbf\xbd", 3);
        i++;
    }
    if (run) mb_rawn(b, (const char *)s + n - run, run);
    mb_rawn(b, "\"", 1);
}

static void jb_slice(MyBuf *b, MySlice s) { mb_strn(b, (const char *)s.p, s.n); }

static void jb_hex(MyBuf *b, const uint8_t *p, size_t n) {
    static const char hx[] = "0123456789abcdef";
    char tmp[1024];
    mb_raw(b, "\"\\u0000hex:");
    for (size_t i = 0; i < n;) {
        size_t k = 0;
        for (; i < n && k + 2 <= sizeof tmp; i++) { tmp[k++] = hx[p[i] >> 4]; tmp[k++] = hx[p[i] & 15]; }
        mb_rawn(b, tmp, k);
    }
    mb_raw(b, "\"");
}

static void jb_column(MyBuf *b, const MyColumn *c) {
    mb_raw(b, "{\"name\":");
    jb_slice(b, c->name);
    mb_raw(b, ",\"orgName\":");
    jb_slice(b, c->org_name);
    mb_raw(b, ",\"table\":");
    jb_slice(b, c->table);
    mb_raw(b, ",\"orgTable\":");
    jb_slice(b, c->org_table);
    mb_raw(b, ",\"db\":");
    jb_slice(b, c->db);
    unsigned f = c->flags;
    mb_printf(b, ",\"type\":%u,\"typeName\":\"%s\",\"length\":%u,\"decimals\":%u,\"charset\":%u,"
                 "\"flags\":{\"notNull\":%s,\"pk\":%s,\"unique\":%s,\"multiKey\":%s,\"unsigned\":%s,\"zerofill\":%s,"
                 "\"binary\":%s,\"autoInc\":%s,\"blob\":%s}}",
              c->type, my_type_name(c->type, c->flags, c->charset), c->length, c->decimals, c->charset,
              f & MY_NOT_NULL ? "true" : "false", f & MY_PRI_KEY ? "true" : "false", f & MY_UNIQUE_KEY ? "true" : "false",
              f & MY_MULTIPLE_KEY ? "true" : "false", f & MY_UNSIGNED ? "true" : "false", f & MY_ZEROFILL ? "true" : "false",
              f & MY_BINARY ? "true" : "false", f & MY_AUTO_INCREMENT ? "true" : "false", f & MY_BLOB ? "true" : "false");
}

static void jb_tail(MyBuf *b, uint64_t affected, uint64_t insert_id, unsigned warnings, const uint8_t *info, size_t il, int truncated) {
    mb_printf(b, ",\"affected\":%llu,\"insertId\":%llu,\"warnings\":%u,\"info\":", (unsigned long long)affected,
              (unsigned long long)insert_id, warnings);
    mb_strn(b, (const char *)(info ? info : (const uint8_t *)""), info ? il : 0);
    mb_printf(b, ",\"truncated\":%s}", truncated ? "true" : "false");
}

/* one row of ncols values: checked whole before anything is written */
static int row_ok(const uint8_t *p, size_t n, int ncols) {
    MyRd r = { p, n, 0 };
    for (int i = 0; i < ncols; i++) {
        MySlice s;
        int null;
        if (my_rd_lenenc_str(&r, &s, &null)) return 0;
    }
    return r.pos == r.n;
}

static void jb_row(MyBuf *b, const uint8_t *p, size_t n, int ncols, const uint8_t *bin) {
    MyRd r = { p, n, 0 };
    mb_raw(b, "[");
    for (int i = 0; i < ncols; i++) {
        MySlice s = { NULL, 0 };
        int null = 1;
        if (my_rd_lenenc_str(&r, &s, &null)) null = 1;     /* row_ok checked it already */
        if (i) mb_raw(b, ",");
        if (null) mb_raw(b, "null");
        else if (bin[i]) jb_hex(b, s.p, s.n);
        else jb_slice(b, s);
    }
    mb_raw(b, "]");
}

/* a command packet: the command byte and its argument, sequence 0 */
static int my_command_send(MyConn *c, uint8_t cmd, const char *arg, size_t len) {
    uint8_t *m = malloc(len + 1);
    if (!m) return fail(c, MY_E_OOM, "memory");
    m[0] = cmd;
    if (len) memcpy(m + 1, arg, len);
    c->seq = 0;
    int rc = my_write_packet(c, m, len + 1);
    free(m);
    return rc;
}

int my_command(MyConn *c, uint8_t cmd, const char *arg, size_t len) {
    int rc = my_command_send(c, cmd, arg, len);
    if (rc) return rc;
    if ((rc = my_read_packet(c))) return rc == MY_E_BIG ? fail(c, MY_E_PROTO, "reply too large") : rc;
    if (c->len && c->pkt[0] == 0xff) { if (my_parse_err(c->pkt, c->len, &c->err)) return fail(c, MY_E_PROTO, "bad error"); return MY_E_SQL; }
    MyOk ok;
    if (!c->len || c->pkt[0] != 0x00 || my_parse_ok(c->pkt, c->len, &ok)) return fail(c, MY_E_PROTO, "bad OK");
    return MY_OK;
}

void my_quit(MyConn *c) {
    uint8_t q = MY_COM_QUIT;
    c->seq = 0;
    my_write_packet(c, &q, 1);
}

int my_query(MyConn *c, const char *sql, size_t len, long max_rows, size_t max_bytes, MyBuf *out) {
    int rc = my_command_send(c, MY_COM_QUERY, sql, len);
    if (rc) return rc;
    uint8_t *bin = NULL;
    int nres = 0;
    mb_raw(out, "[");
    for (;;) {
        uint16_t status = 0;
        if ((rc = my_read_packet(c))) { if (rc == MY_E_BIG) rc = fail(c, MY_E_PROTO, "reply too large"); goto out; }
        if (!c->len) { rc = fail(c, MY_E_PROTO, "empty packet"); goto out; }
        uint8_t t = c->pkt[0];
        if (t == 0xff) {
            if (my_parse_err(c->pkt, c->len, &c->err)) { rc = fail(c, MY_E_PROTO, "bad error"); goto out; }
            rc = MY_E_SQL;
            goto out;
        }
        if (t == 0xfb) {
            /* LOAD DATA LOCAL: never asked for (no CLIENT_LOCAL_FILES); answered with an empty file */
            if ((rc = my_write_packet(c, "", 0))) goto out;
            continue;
        }
        if (nres++) mb_raw(out, ",");
        if (t == 0x00) {
            MyOk ok;
            if (my_parse_ok(c->pkt, c->len, &ok)) { rc = fail(c, MY_E_PROTO, "bad OK"); goto out; }
            mb_raw(out, "{\"columns\":[],\"rows\":[]");
            jb_tail(out, ok.affected, ok.insert_id, ok.warnings, ok.info, ok.info_len, 0);
            status = ok.status;
        } else {
            MyRd r = { c->pkt, c->len, 0 };
            uint64_t nc;
            if (my_rd_lenenc(&r, &nc, NULL) || r.pos != r.n || !nc || nc > MY_MAX_COLUMNS) { rc = fail(c, MY_E_PROTO, "column count"); goto out; }
            int ncols = (int)nc;
            bin = calloc((size_t)ncols, 1);
            if (!bin) { rc = fail(c, MY_E_OOM, "memory"); goto out; }
            mb_raw(out, "{\"columns\":[");
            for (int i = 0; i < ncols; i++) {
                MyColumn col;
                if ((rc = my_read_packet(c))) { if (rc == MY_E_BIG) rc = fail(c, MY_E_PROTO, "column too large"); goto out; }
                if (my_parse_column(c->pkt, c->len, &col)) { rc = fail(c, MY_E_PROTO, "bad column"); goto out; }
                if (i) mb_raw(out, ",");
                jb_column(out, &col);
                bin[i] = (uint8_t)my_is_binary(col.type, col.charset);
            }
            mb_raw(out, "],\"rows\":[");
            if (!(c->caps & MY_CLIENT_DEPRECATE_EOF)) {
                uint16_t w, s;
                if ((rc = my_read_packet(c))) { if (rc == MY_E_BIG) rc = fail(c, MY_E_PROTO, "bad EOF"); goto out; }
                if (my_parse_eof(c->pkt, c->len, &w, &s)) { rc = fail(c, MY_E_PROTO, "bad EOF"); goto out; }
            }
            long kept = 0;
            int trunc = 0;
            uint16_t warnings = 0;
            for (;;) {
                rc = my_read_packet(c);
                if (rc == MY_E_BIG) { trunc = 1; continue; }       /* a row past MY_PACKET_MAX: read and dropped */
                if (rc) goto out;
                if (my_is_row_end(c)) {
                    if (c->caps & MY_CLIENT_DEPRECATE_EOF) {
                        MyOk ok;
                        if (my_parse_ok(c->pkt, c->len, &ok)) { rc = fail(c, MY_E_PROTO, "bad end of rows"); goto out; }
                        warnings = ok.warnings;
                        status = ok.status;
                    } else if (my_parse_eof(c->pkt, c->len, &warnings, &status)) { rc = fail(c, MY_E_PROTO, "bad EOF"); goto out; }
                    break;
                }
                if (c->len && c->pkt[0] == 0xff) {
                    /* the statement failed while sending rows (killed, out of memory): what came is kept */
                    if (my_parse_err(c->pkt, c->len, &c->err)) { rc = fail(c, MY_E_PROTO, "bad error"); goto out; }
                    mb_raw(out, "]");
                    jb_tail(out, 0, 0, 0, NULL, 0, trunc);
                    rc = MY_E_SQL;
                    goto out;
                }
                if (!row_ok(c->pkt, c->len, ncols)) { rc = fail(c, MY_E_PROTO, "bad row"); goto out; }
                if (kept >= max_rows || out->n >= max_bytes) { trunc = 1; continue; }
                size_t mark = out->n;
                if (kept) mb_raw(out, ",");
                jb_row(out, c->pkt, c->len, ncols, bin);
                if (out->oom) { rc = fail(c, MY_E_OOM, "memory"); goto out; }
                if (out->n > max_bytes) { out->n = mark; out->p[mark] = 0; trunc = 1; continue; }
                kept++;
            }
            mb_raw(out, "]");
            jb_tail(out, 0, 0, warnings, NULL, 0, trunc);
            free(bin);
            bin = NULL;
        }
        if (!(status & MY_SERVER_MORE_RESULTS)) break;
    }
out:
    mb_raw(out, "]");
    free(bin);
    if (out->oom) return fail(c, MY_E_OOM, "memory");
    return rc;
}
