/* lanparse.c — see lanparse.h. */
#include "lanparse.h"
#include "lanaddr.h"

#include <stdio.h>
#include <string.h>

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* UTF-16LE (n code units) → UTF-8, cleaned for the interface */
static void utf16_text(const uint8_t *p, size_t units, char *out, size_t cap) {
    char tmp[512];
    size_t o = 0;
    for (size_t i = 0; i < units && o + 5 < sizeof tmp; i++) {
        uint32_t c = get16(p + i * 2);
        if (!c) break;
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < units) {
            uint32_t d = get16(p + (i + 1) * 2);
            if (d >= 0xdc00 && d < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00); i++; }
            else c = 0xfffd;
        } else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
        if (c < 0x80) tmp[o++] = (char)c;
        else if (c < 0x800) { tmp[o++] = (char)(0xc0 | c >> 6); tmp[o++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { tmp[o++] = (char)(0xe0 | c >> 12); tmp[o++] = (char)(0x80 | ((c >> 6) & 63)); tmp[o++] = (char)(0x80 | (c & 63)); }
        else { tmp[o++] = (char)(0xf0 | c >> 18); tmp[o++] = (char)(0x80 | ((c >> 12) & 63)); tmp[o++] = (char)(0x80 | ((c >> 6) & 63)); tmp[o++] = (char)(0x80 | (c & 63)); }
    }
    tmp[o] = 0;
    if (!lan_clean_text(tmp, out, cap)) out[0] = 0;
}

/* ASCII → UTF-16LE. Returns bytes written. */
static size_t to_utf16(uint8_t *out, const char *s) {
    size_t o = 0;
    for (; *s; s++) { out[o++] = (uint8_t)*s; out[o++] = 0; }
    return o;
}

/* ---- ONC RPC ---- */

enum { PMAP_PROG = 100000, MOUNT_PROG = 100005 };

static size_t rpc_head(uint8_t *o, uint32_t xid, uint32_t prog, uint32_t vers, uint32_t proc) {
    be32(o + 4, xid);
    be32(o + 8, 0);                     /* CALL */
    be32(o + 12, 2);                    /* RPC version */
    be32(o + 16, prog);
    be32(o + 20, vers);
    be32(o + 24, proc);
    memset(o + 28, 0, 16);              /* cred and verifier: AUTH_NULL */
    return 44;
}

size_t rpc_portmap_call(uint8_t *out, size_t cap, uint32_t xid) {
    if (cap < 60) return 0;
    size_t n = rpc_head(out, xid, PMAP_PROG, 2, 3);
    be32(out + n, MOUNT_PROG);
    be32(out + n + 4, 3);
    be32(out + n + 8, 6);               /* TCP */
    be32(out + n + 12, 0);
    n += 16;
    be32(out, 0x80000000u | (uint32_t)(n - 4));
    return n;
}

size_t rpc_export_call(uint8_t *out, size_t cap, uint32_t xid) {
    if (cap < 44) return 0;
    size_t n = rpc_head(out, xid, MOUNT_PROG, 3, 5);
    be32(out, 0x80000000u | (uint32_t)(n - 4));
    return n;
}

/* The results' offset in an accepted, successful reply; -1 otherwise. */
static long rpc_results(const uint8_t *b, size_t len, uint32_t xid) {
    if (len < 24 || rd32(b) != xid || rd32(b + 4) != 1 || rd32(b + 8) != 0) return -1;
    uint32_t vl = rd32(b + 16);
    if (vl > 400 || vl & 3 || 20 + (size_t)vl + 4 > len) return -1;
    size_t o = 20 + vl;
    if (rd32(b + o) != 0) return -1;
    return (long)(o + 4);
}

int rpc_portmap_reply(const uint8_t *b, size_t len, uint32_t xid) {
    long o = rpc_results(b, len, xid);
    if (o < 0 || (size_t)o + 4 > len) return -1;
    uint32_t p = rd32(b + o);
    return p > 65535 ? -1 : (int)p;
}

int rpc_export_reply(const uint8_t *b, size_t len, uint32_t xid, LanExport *out, int max) {
    long ro = rpc_results(b, len, xid);
    if (ro < 0) return -1;
    size_t o = (size_t)ro;
    int n = 0;
    for (;;) {
        if (o + 4 > len) return -1;
        uint32_t more = rd32(b + o);
        o += 4;
        if (!more) return n;
        if (more != 1 || o + 4 > len) return -1;
        uint32_t l = rd32(b + o);
        o += 4;
        if (l > 1024 || o + ((size_t)l + 3) / 4 * 4 > len) return -1;
        char path[1025];
        memcpy(path, b + o, l);
        path[l] = 0;
        o += ((size_t)l + 3) / 4 * 4;
        int open = 0, groups = 0;
        for (;;) {
            if (o + 4 > len) return -1;
            uint32_t g = rd32(b + o);
            o += 4;
            if (!g) break;
            if (g != 1 || o + 4 > len || ++groups > 4096) return -1;
            uint32_t gl = rd32(b + o);
            o += 4;
            if (gl > 1024 || o + ((size_t)gl + 3) / 4 * 4 > len) return -1;
            if (gl == 1 && b[o] == '*') open = 1;
            o += ((size_t)gl + 3) / 4 * 4;
        }
        if (!groups) open = 1;                  /* no list: exported to everyone */
        if (n < max && path[0] == '/' && strlen(path) == l && lan_clean_text(path, out[n].path, sizeof out[n].path)) { out[n].open = open; n++; }
    }
}

/* ---- SMB2 ---- */

enum { C_NEGOTIATE = 0, C_SESSION = 1, C_TREE = 3, C_CREATE = 5, C_IOCTL = 11 };

static void smb_head(uint8_t *p, uint16_t cmd, uint64_t msgid, uint64_t session, uint32_t tree) {
    memset(p, 0, SMB_HDR);
    memcpy(p, "\xfeSMB", 4);
    put16(p + 4, SMB_HDR);
    put16(p + 12, cmd);
    put16(p + 14, 16);                  /* credits requested */
    put64(p + 24, msgid);
    put32(p + 36, tree);
    put64(p + 40, session);
}

static void transport(uint8_t *out, size_t body) {
    out[0] = 0;
    out[1] = (uint8_t)(body >> 16);
    out[2] = (uint8_t)(body >> 8);
    out[3] = (uint8_t)body;
}

int smb_parse(const uint8_t *b, size_t len, SmbMsg *m) {
    if (len < SMB_HDR || memcmp(b, "\xfeSMB", 4) || get16(b + 4) != SMB_HDR) return -1;
    m->status = get32(b + 8);
    m->cmd = (uint16_t)get16(b + 12);
    m->tree = get32(b + 36);
    m->session = get64(b + 40);
    m->body = b + SMB_HDR;
    m->blen = len - SMB_HDR;
    return 0;
}

size_t smb_negotiate(uint8_t *out, size_t cap, uint64_t msgid) {
    static const uint16_t dialects[] = { 0x0202, 0x0210, 0x0300, 0x0302 };
    size_t n = 4 + SMB_HDR + 36 + sizeof dialects;
    if (cap < n) return 0;
    uint8_t *h = out + 4, *b = h + SMB_HDR;
    smb_head(h, C_NEGOTIATE, msgid, 0, 0);
    memset(b, 0, 36);
    put16(b, 36);
    put16(b + 2, 4);
    put16(b + 4, 1);                    /* signing enabled, not required */
    memcpy(b + 12, "LordOS-Network\0\1", 16);
    for (size_t i = 0; i < 4; i++) put16(b + 36 + i * 2, dialects[i]);
    transport(out, n - 4);
    return n;
}

/* Listing after signing in signs its messages, and these two dialects sign with HMAC-SHA256 alone. Signing is
 * asked for as required: in 2.0.2 and 2.1 a server signs its final SESSION_SETUP answer only when the session
 * requires signing (MS-SMB2 3.3.5.5.3), and that signature is the proof that it knew the password. Many
 * desktop systems don't require it by default, so without this every correct password was refused (GitHub #101). */
size_t smb_negotiate_signed(uint8_t *out, size_t cap, uint64_t msgid) {
    size_t n = smb_negotiate(out, cap, msgid);
    if (!n) return 0;
    uint8_t *b = out + 4 + SMB_HDR;
    put16(b + 2, 2);
    put16(b + 4, 3);                    /* signing enabled and required */
    n -= 4;
    transport(out, n - 4);
    return n;
}

size_t smb_session_setup(uint8_t *out, size_t cap, uint64_t msgid, uint64_t session, const uint8_t *blob, size_t blen) {
    size_t n = 4 + SMB_HDR + 24 + blen;
    if (cap < n) return 0;
    uint8_t *h = out + 4, *b = h + SMB_HDR;
    smb_head(h, C_SESSION, msgid, session, 0);
    memset(b, 0, 24);
    put16(b, 25);
    b[3] = 1;                           /* signing enabled */
    put16(b + 12, SMB_HDR + 24);
    put16(b + 14, (uint32_t)blen);
    memcpy(b + 24, blob, blen);
    transport(out, n - 4);
    return n;
}

size_t smb_session_setup_signed(uint8_t *out, size_t cap, uint64_t msgid, uint64_t session, const uint8_t *blob, size_t blen) {
    size_t n = smb_session_setup(out, cap, msgid, session, blob, blen);
    if (n) out[4 + SMB_HDR + 3] = 3;    /* signing enabled and required, as in smb_negotiate_signed */
    return n;
}

size_t smb_tree_connect(uint8_t *out, size_t cap, uint64_t msgid, uint64_t session, const char *ip) {
    char unc[32];
    int w = snprintf(unc, sizeof unc, "\\\\%s\\IPC$", ip);
    if (w < 0 || (size_t)w >= sizeof unc) return 0;
    size_t pl = (size_t)w * 2, n = 4 + SMB_HDR + 8 + pl;
    if (cap < n) return 0;
    uint8_t *h = out + 4, *b = h + SMB_HDR;
    smb_head(h, C_TREE, msgid, session, 0);
    memset(b, 0, 8);
    put16(b, 9);
    put16(b + 4, SMB_HDR + 8);
    put16(b + 6, (uint32_t)pl);
    to_utf16(b + 8, unc);
    transport(out, n - 4);
    return n;
}

size_t smb_create_pipe(uint8_t *out, size_t cap, uint64_t msgid, uint64_t session, uint32_t tree) {
    size_t nl = 12, n = 4 + SMB_HDR + 56 + nl;      /* "srvsvc" */
    if (cap < n) return 0;
    uint8_t *h = out + 4, *b = h + SMB_HDR;
    smb_head(h, C_CREATE, msgid, session, tree);
    memset(b, 0, 56);
    put16(b, 57);
    put32(b + 4, 2);                    /* impersonation */
    put32(b + 24, 0x0012019f);          /* read, write, attributes, synchronize */
    put32(b + 32, 7);                   /* share everything */
    put32(b + 36, 1);                   /* open if it exists */
    put32(b + 40, 0x40);                /* not a folder */
    put16(b + 44, SMB_HDR + 56);
    put16(b + 46, (uint32_t)nl);
    to_utf16(b + 56, "srvsvc");
    transport(out, n - 4);
    return n;
}

size_t smb_pipe_ioctl(uint8_t *out, size_t cap, uint64_t msgid, uint64_t session, uint32_t tree, const uint8_t fid[16], const uint8_t *in, size_t ilen) {
    size_t n = 4 + SMB_HDR + 56 + ilen;
    if (cap < n) return 0;
    uint8_t *h = out + 4, *b = h + SMB_HDR;
    smb_head(h, C_IOCTL, msgid, session, tree);
    memset(b, 0, 56);
    put16(b, 57);
    put32(b + 4, 0x0011c017);           /* FSCTL_PIPE_TRANSCEIVE */
    memcpy(b + 8, fid, 16);
    put32(b + 24, SMB_HDR + 56);
    put32(b + 28, (uint32_t)ilen);
    put32(b + 44, 60000);               /* the most the answer may have */
    put32(b + 48, 1);                   /* it is an FSCTL */
    memcpy(b + 56, in, ilen);
    transport(out, n - 4);
    return n;
}

int smb_negotiate_reply(const SmbMsg *m, int *signing) {
    if (m->status || m->cmd != C_NEGOTIATE || m->blen < 64 || get16(m->body) != 65) return 0;
    *signing = (get16(m->body + 2) & 2) != 0;
    return (int)get16(m->body + 4);
}

int smb_session_reply(const SmbMsg *m, const uint8_t **blob, size_t *blen, unsigned *flags) {
    if (m->cmd != C_SESSION || m->blen < 8 || get16(m->body) != 9) return -1;
    *flags = get16(m->body + 2);
    size_t off = get16(m->body + 4), l = get16(m->body + 6);
    *blob = NULL;
    *blen = 0;
    if (l) {
        if (off < SMB_HDR + 8 || off - SMB_HDR + l > m->blen) return -1;
        *blob = m->body + (off - SMB_HDR);
        *blen = l;
    }
    return 0;
}

int smb_create_reply(const SmbMsg *m, uint8_t fid[16]) {
    if (m->status || m->cmd != C_CREATE || m->blen < 88 || get16(m->body) != 89) return -1;
    memcpy(fid, m->body + 64, 16);
    return 0;
}

int smb_ioctl_reply(const SmbMsg *m, const uint8_t **out, size_t *olen) {
    if (m->cmd != C_IOCTL || m->blen < 48 || get16(m->body) != 49) return -1;
    if (m->status && m->status != 0x80000005u) return -1;   /* a truncated answer is still parsed */
    size_t off = get32(m->body + 32), l = get32(m->body + 36);
    if (off < SMB_HDR + 48 || off - SMB_HDR > m->blen || l > m->blen - (off - SMB_HDR)) return -1;
    *out = m->body + (off - SMB_HDR);
    *olen = l;
    return 0;
}

/* ---- NTLMSSP ---- */

/* UNICODE, REQUEST_TARGET, NTLM, ANONYMOUS, EXTENDED_SESSIONSECURITY, TARGET_INFO, 128, 56 */
#define NTLM_FLAGS 0xa0880a05u

size_t ntlm_negotiate(uint8_t *o, size_t cap) {
    if (cap < 32) return 0;
    memset(o, 0, 32);
    memcpy(o, "NTLMSSP", 8);
    put32(o + 8, 1);
    put32(o + 12, NTLM_FLAGS);
    return 32;
}

size_t ntlm_anonymous_auth(uint8_t *o, size_t cap) {
    if (cap < 65) return 0;
    memset(o, 0, 65);
    memcpy(o, "NTLMSSP", 8);
    put32(o + 8, 3);
    /* one zero byte of LM response, every other field empty, all pointing past the header */
    put16(o + 12, 1); put16(o + 14, 1); put32(o + 16, 64);
    for (size_t f = 20; f <= 52; f += 8) put32(o + f + 4, 65);
    put32(o + 60, NTLM_FLAGS);
    return 65;
}

int ntlm_challenge_names(const uint8_t *b, size_t len, char *name, size_t nn, char *domain, size_t dn) {
    name[0] = domain[0] = 0;
    if (len < 48 || memcmp(b, "NTLMSSP", 8) || get32(b + 8) != 2) return -1;
    size_t il = get16(b + 40), io = get32(b + 44);
    if (!il) return 0;
    if (io > len || il > len - io) return -1;
    const uint8_t *p = b + io;
    int have_dns = 0;
    for (size_t o = 0; o + 4 <= il;) {
        uint32_t id = get16(p + o), l = get16(p + o + 2);
        if (!id) break;
        if (o + 4 + l > il) return -1;
        if (id == 1 && !name[0]) utf16_text(p + o + 4, l / 2, name, nn);
        else if (id == 2) utf16_text(p + o + 4, l / 2, domain, dn);
        else if (id == 3 && !have_dns) { char t[256]; utf16_text(p + o + 4, l / 2, t, sizeof t); if (t[0] && !name[0]) { snprintf(name, nn, "%s", t); have_dns = 1; } }
        o += 4 + (size_t)l;
    }
    return 0;
}

/* ---- DCE/RPC ---- */

static size_t dce_head(uint8_t *o, uint8_t type, size_t frag, uint32_t call) {
    o[0] = 5; o[1] = 0; o[2] = type; o[3] = 3;
    o[4] = 0x10; o[5] = 0; o[6] = 0; o[7] = 0;
    put16(o + 8, (uint32_t)frag);
    put16(o + 10, 0);
    put32(o + 12, call);
    return 16;
}

size_t dce_bind(uint8_t *o, size_t cap) {
    static const uint8_t srvsvc[20] = { 0xc8, 0x4f, 0x32, 0x4b, 0x70, 0x16, 0xd3, 0x01, 0x12, 0x78, 0x5a, 0x47, 0xbf, 0x6e, 0xe1, 0x88, 3, 0, 0, 0 };
    static const uint8_t ndr[20] = { 0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60, 2, 0, 0, 0 };
    size_t n = 16 + 8 + 4 + 44;
    if (cap < n) return 0;
    memset(o, 0, n);
    dce_head(o, 11, n, 1);
    put16(o + 16, 60000);               /* the answer comes whole, in one fragment */
    put16(o + 18, 60000);
    o[24] = 1;                          /* one context */
    o[30] = 1;                          /* one transfer syntax */
    memcpy(o + 32, srvsvc, 20);
    memcpy(o + 52, ndr, 20);
    return n;
}

int dce_bind_ack(const uint8_t *b, size_t len) {
    if (len < 28 || b[0] != 5 || b[2] != 12) return -1;
    size_t al = get16(b + 24), o = 26 + al;
    o = (o + 3) & ~(size_t)3;
    if (o + 8 > len) return -1;
    return b[o] >= 1 && get16(b + o + 4) == 0 ? 0 : -1;
}

/* NDR conformant-varying UTF-16 string, with its padding */
static size_t ndr_wstr(uint8_t *o, const char *s) {
    size_t l = strlen(s) + 1;
    put32(o, (uint32_t)l);
    put32(o + 4, 0);
    put32(o + 8, (uint32_t)l);
    size_t w = to_utf16(o + 12, s);
    put16(o + 12 + w, 0);
    w += 2;
    size_t t = 12 + w;
    while (t & 3) o[t++] = 0;
    return t;
}

size_t dce_share_enum(uint8_t *o, size_t cap, const char *ip) {
    char srv[24];
    int w = snprintf(srv, sizeof srv, "\\\\%s", ip);
    if (w < 0 || (size_t)w >= sizeof srv) return 0;
    if (cap < 24 + 4 + 12 + 2 * (size_t)w + 8 + 40) return 0;
    uint8_t *s = o + 24;
    size_t n = 0;
    put32(s, 0x20000); n += 4;          /* server name */
    n += ndr_wstr(s + n, srv);
    put32(s + n, 1); n += 4;            /* level */
    put32(s + n, 1); n += 4;            /* union discriminant */
    put32(s + n, 0x20004); n += 4;      /* the level-1 container */
    put32(s + n, 0); n += 4;            /* entries read */
    put32(s + n, 0); n += 4;            /* buffer: none */
    put32(s + n, 0xffffffffu); n += 4;  /* preferred maximum */
    put32(s + n, 0x20008); n += 4;      /* resume handle */
    put32(s + n, 0); n += 4;
    size_t all = 24 + n;
    dce_head(o, 0, all, 2);
    put32(o + 16, (uint32_t)n);
    put16(o + 20, 0);                   /* context 0 */
    put16(o + 22, 15);                  /* NetrShareEnum */
    return all;
}

/* One NDR string at *o into text; moves *o past it and its padding. */
static int ndr_read(const uint8_t *b, size_t len, size_t *o, char *out, size_t cap) {
    size_t p = *o;
    if (p + 12 > len) return -1;
    uint32_t off = get32(b + p + 4), act = get32(b + p + 8);
    p += 12;
    if (off > 4096 || act > 4096 || (len - p) / 2 < (size_t)act) return -1;
    utf16_text(b + p, act, out, cap);
    p += (size_t)act * 2;
    p = (p + 3) & ~(size_t)3;
    *o = p;
    return 0;
}

int dce_share_reply(const uint8_t *b, size_t len, LanShare *out, int max) {
    if (len < 24 + 24 || b[0] != 5 || b[2] != 2) return -1;
    size_t o = 24;
    if (get32(b + o) != 1) return -1;                   /* level */
    o += 8;                                              /* level, discriminant */
    if (!get32(b + o)) return -1;                       /* the container */
    uint32_t count = get32(b + o + 4), bufp = get32(b + o + 8);
    o += 12;
    if (!count || !bufp) return 0;
    if (count > 4096 || o + 4 > len) return -1;
    uint32_t max_count = get32(b + o);
    o += 4;
    if (max_count < count || (len - o) / 12 < count) return -1;
    size_t ents = o;
    o += (size_t)count * 12;
    int n = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = b + ents + i * 12;
        char name[128] = "", comment[128] = "";
        if (get32(e) && ndr_read(b, len, &o, name, sizeof name)) return -1;
        if (get32(e + 8) && ndr_read(b, len, &o, comment, sizeof comment)) return -1;
        uint32_t type = get32(e + 4);
        size_t l = strlen(name);
        if ((type & 0x0fffffffu) != 0 || (type & 0x80000000u) || !l || name[l - 1] == '$' || n >= max) continue;
        snprintf(out[n].name, sizeof out[n].name, "%s", name);
        snprintf(out[n].comment, sizeof out[n].comment, "%s", comment);
        n++;
    }
    return n;
}
