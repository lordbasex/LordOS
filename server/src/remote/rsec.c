/* rsec.c — strict parsers for what the browser sends to the remote desktop (see rsec.h). */
#include "rsec.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- HTTP request head ---- */

static int is_tchar(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c));
}

/* a header value into a fixed field; -431 if it doesn't fit, -400 if repeated */
static int put_field(char *dst, size_t cap, const char *v, size_t vl, unsigned *seen, unsigned bit) {
    if (*seen & bit) return -400;
    *seen |= bit;
    if (vl >= cap) return -431;
    memcpy(dst, v, vl);
    dst[vl] = 0;
    return 0;
}

int rs_parse_head(const char *buf, size_t len, RsReq *r) {
    memset(r, 0, sizeof *r);
    r->content_length = -1;
    size_t lim = len < RS_MAX_HEAD ? len : RS_MAX_HEAD, end = 0;
    int found = 0;
    /* bytes allowed in a head: visible ASCII, space, tab, and CR only as part of CRLF */
    for (size_t i = 0; i < lim; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\r') {
            if (i + 1 < lim && buf[i + 1] != '\n') return -400;
            if (i + 3 < lim && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') { end = i; found = 1; break; }
            continue;
        }
        if (c == '\n') { if (!i || buf[i - 1] != '\r') return -400; continue; }
        if (c == '\t') continue;
        if (c < 0x20 || c > 0x7e) return -400;
    }
    if (!found) return len >= RS_MAX_HEAD ? -431 : RS_NEED_MORE;
    if (end == 0) return -400;                       /* no request line (leading CRLF) */

    /* request line: METHOD SP /target SP HTTP/1.x */
    const char *p = buf, *eol = memchr(buf, '\r', end + 1);
    size_t ml = 0;
    while (p + ml < eol && p[ml] >= 'A' && p[ml] <= 'Z') ml++;
    if (!ml || ml >= sizeof r->method || p[ml] != ' ') return -400;
    memcpy(r->method, p, ml);
    const char *tg = p + ml + 1, *sp = tg;
    while (sp < eol && *sp != ' ') sp++;
    if (sp == tg || sp >= eol || *tg != '/') return -400;
    size_t tl = (size_t)(sp - tg);
    if (tl > 400) return -414;
    const char *q = memchr(tg, '?', tl);
    if (memchr(tg, '#', tl) || memchr(tg, '\t', tl)) return -400;
    size_t pl = q ? (size_t)(q - tg) : tl, ql = q ? tl - pl - 1 : 0;
    if (pl >= sizeof r->path || ql >= sizeof r->query) return -414;
    memcpy(r->path, tg, pl);
    if (q) memcpy(r->query, q + 1, ql);
    const char *ver = sp + 1;
    int http11;
    if ((size_t)(eol - ver) == 8 && !memcmp(ver, "HTTP/1.1", 8)) http11 = 1;
    else if ((size_t)(eol - ver) == 8 && !memcmp(ver, "HTTP/1.0", 8)) http11 = 0;
    else return -400;

    /* header fields */
    unsigned seen = 0;
    int count = 0;
    for (const char *line = eol + 2; line < buf + end + 2;) {
        const char *le = memchr(line, '\r', (size_t)(buf + end + 2 - line));
        if (!le) return -400;
        if (le == line) break;
        if (++count > RS_MAX_HEADERS) return -431;
        if (*line == ' ' || *line == '\t') return -400;            /* obsolete line folding */
        const char *colon = memchr(line, ':', (size_t)(le - line));
        if (!colon || colon == line) return -400;
        for (const char *c = line; c < colon; c++) if (!is_tchar((unsigned char)*c)) return -400;   /* also no space before ':' */
        size_t nl = (size_t)(colon - line);
        const char *v = colon + 1, *ve = le;
        while (v < ve && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        size_t vl = (size_t)(ve - v);
        int rc = 0;
#define HDR(nm) (nl == sizeof(nm) - 1 && !strncasecmp(line, nm, nl))
        if (HDR("Host")) rc = put_field(r->host, sizeof r->host, v, vl, &seen, 1u << 0);
        else if (HDR("Origin")) rc = put_field(r->origin, sizeof r->origin, v, vl, &seen, 1u << 1);
        else if (HDR("Cookie")) rc = put_field(r->cookie, sizeof r->cookie, v, vl, &seen, 1u << 2);
        else if (HDR("Upgrade")) rc = put_field(r->upgrade, sizeof r->upgrade, v, vl, &seen, 1u << 3);
        else if (HDR("Connection")) rc = put_field(r->connection, sizeof r->connection, v, vl, &seen, 1u << 4);
        else if (HDR("Sec-WebSocket-Key")) rc = put_field(r->wskey, sizeof r->wskey, v, vl, &seen, 1u << 5);
        else if (HDR("Sec-WebSocket-Version")) rc = put_field(r->wsversion, sizeof r->wsversion, v, vl, &seen, 1u << 6);
        else if (HDR("Content-Type")) rc = put_field(r->ctype, sizeof r->ctype, v, vl, &seen, 1u << 7);
        else if (HDR("Sec-Fetch-Site")) rc = put_field(r->fetch_site, sizeof r->fetch_site, v, vl, &seen, 1u << 8);
        else if (HDR("Transfer-Encoding")) return -501;
        else if (HDR("Content-Length")) {
            if (seen & (1u << 9)) return -400;
            seen |= 1u << 9;
            if (!vl || vl > 9) return vl > 9 ? -413 : -400;
            long n = 0;
            for (size_t i = 0; i < vl; i++) {
                if (v[i] < '0' || v[i] > '9') return -400;
                n = n * 10 + (v[i] - '0');
            }
            if (n > RS_MAX_BODY) return -413;
            r->content_length = n;
        }
#undef HDR
        if (rc) return rc;
        line = le + 2;
    }
    if (http11 && !r->host[0]) return -400;
    r->head_len = end + 4;
    return (int)r->head_len;
}

int rs_valid_username(const char *u) {
    size_t n = u ? strlen(u) : 0;
    if (!n || n > 32 || !((u[0] >= 'a' && u[0] <= 'z') || u[0] == '_')) return 0;
    for (const char *p = u; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
    return 1;
}

static int valid_port(const char *p) {
    size_t n = strlen(p);
    if (!n || n > 5 || p[0] == '0') return 0;
    long v = 0;
    for (size_t i = 0; i < n; i++) { if (p[i] < '0' || p[i] > '9') return 0; v = v * 10 + (p[i] - '0'); }
    return v > 0 && v < 65536;
}

int rs_valid_host(const char *h) {
    size_t n = h ? strlen(h) : 0;
    if (!n || n > 260) return 0;
    char buf[264];
    memcpy(buf, h, n + 1);
    if (buf[0] == '[') {
        char *rb = strchr(buf, ']');
        if (!rb) return 0;
        *rb = 0;
        unsigned char a[16];
        if (inet_pton(AF_INET6, buf + 1, a) != 1) return 0;
        if (!rb[1]) return 1;
        return rb[1] == ':' && valid_port(rb + 2);
    }
    char *colon = strchr(buf, ':');
    if (colon) { *colon = 0; if (!valid_port(colon + 1)) return 0; }
    size_t l = strlen(buf);
    if (!l || l > 253 || buf[0] == '-' || buf[0] == '.' || buf[l - 1] == '-') return 0;
    for (size_t i = 0; i < l; i++) {
        char c = buf[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return 0;
        if (c == '.' && buf[i + 1] == '.') return 0;
    }
    return 1;
}

int rs_same_origin(const char *origin, const char *host) {
    if (!origin || !host || !rs_valid_host(host)) return 0;
    if (strncmp(origin, "https://", 8)) return 0;
    return !strcasecmp(origin + 8, host);
}

int rs_has_token(const char *list, const char *token) {
    size_t tl = strlen(token);
    for (const char *p = list; p && *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        const char *te = e;
        while (te > p && (te[-1] == ' ' || te[-1] == '\t')) te--;
        if ((size_t)(te - p) == tl && !strncasecmp(p, token, tl)) return 1;
        p = e;
    }
    return 0;
}

static int b64c(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/'; }

int rs_valid_wskey(const char *k) {
    if (!k || strlen(k) != 24 || k[22] != '=' || k[23] != '=') return 0;
    for (int i = 0; i < 22; i++) if (!b64c(k[i])) return 0;
    return strchr("AQgw", k[21]) != NULL;             /* the last 4 bits must be zero */
}

int rs_form_type(const char *ct) {
    static const char base[] = "application/x-www-form-urlencoded";
    if (!ct || strncasecmp(ct, base, sizeof base - 1)) return 0;
    const char *p = ct + sizeof base - 1;
    while (*p == ' ') p++;
    if (!*p) return 1;
    if (*p++ != ';') return 0;
    while (*p == ' ') p++;
    if (strncasecmp(p, "charset=", 8)) return 0;
    p += 8;
    int quoted = *p == '"';
    if (quoted) p++;
    if (strncasecmp(p, "utf-8", 5)) return 0;
    p += 5;
    if (quoted && *p++ != '"') return 0;
    while (*p == ' ') p++;
    return !*p;
}

int rs_cookie_token(const char *cookie, const char *name, char out[65]) {
    out[0] = 0;
    size_t nl = strlen(name);
    int found = 0;
    for (const char *p = cookie ? cookie : ""; *p;) {
        while (*p == ' ' || *p == ';') p++;
        const char *e = p;
        while (*e && *e != ';') e++;
        const char *eq = memchr(p, '=', (size_t)(e - p));
        if (eq && (size_t)(eq - p) == nl && !memcmp(p, name, nl)) {
            const char *v = eq + 1, *ve = e;
            while (ve > v && ve[-1] == ' ') ve--;
            if (found++ || ve - v != 64) return -2;
            for (const char *c = v; c < ve; c++) if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'))) return -2;
            memcpy(out, v, 64);
            out[64] = 0;
        }
        p = e;
    }
    return found ? 0 : -1;
}

/* ---- login form ---- */

static int hexv(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* decodes v[0..vl) into out (cap bytes); returns the length, -1 malformed, -2 too long */
static long form_decode(const char *v, size_t vl, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < vl; i++) {
        char c = v[i];
        if (c == '%') {
            if (i + 2 >= vl) return -1;
            int h = hexv(v[i + 1]), l = hexv(v[i + 2]);
            if (h < 0 || l < 0) return -1;
            c = (char)(h << 4 | l);
            i += 2;
        } else if (c == '+') c = ' ';
        if (o >= cap) return -2;
        out[o++] = c;
    }
    return (long)o;
}

void rs_log_clean(const char *in, size_t inlen, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < inlen && o + 1 < n; i++) {
        unsigned char c = (unsigned char)in[i];
        out[o++] = c >= 0x20 && c < 0x7f && c != '"' && c != '\\' ? (char)c : '?';
    }
    if (n) out[o] = 0;
}

int rs_parse_login(const char *body, size_t len, char user[33], char pass[RS_PASS_MAX + 1], size_t *passlen, char rawuser[33]) {
    user[0] = pass[0] = rawuser[0] = 0;
    *passlen = 0;
    if (len > RS_MAX_BODY) return RS_FORM_BAD;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)body[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~*%+=&", c)) || !c) return RS_FORM_BAD;
    }
    int have_u = 0, have_p = 0;
    char ubuf[64];
    long ul = -1, pl = -1;
    for (size_t i = 0; i < len;) {
        size_t e = i;
        while (e < len && body[e] != '&') e++;
        const char *f = body + i;
        size_t fl = e - i;
        const char *eq = memchr(f, '=', fl);
        if (!eq) return RS_FORM_BAD;
        size_t kl = (size_t)(eq - f);
        if (kl == 4 && !memcmp(f, "user", 4)) {
            if (have_u++) return RS_FORM_BAD;
            ul = form_decode(eq + 1, fl - 5, ubuf, sizeof ubuf - 1);
            if (ul == -1) return RS_FORM_BAD;
        } else if (kl == 4 && !memcmp(f, "pass", 4)) {
            if (have_p++) return RS_FORM_BAD;
            pl = form_decode(eq + 1, fl - 5, pass, RS_PASS_MAX);
            if (pl == -1) return RS_FORM_BAD;
        } else return RS_FORM_BAD;
        i = e + 1;
        if (e == len - 1 && body[e] == '&') return RS_FORM_BAD;      /* trailing '&' */
    }
    if (!have_u || !have_p) { explicit_bzero(pass, RS_PASS_MAX + 1); return RS_FORM_BAD; }
    if (ul >= 0) { ubuf[ul] = 0; rs_log_clean(ubuf, (size_t)ul, rawuser, 33); }
    else rs_log_clean("(too long)", 10, rawuser, 33);
    if (ul <= 0 || ul > 32 || memchr(ubuf, 0, (size_t)ul) || !rs_valid_username(ubuf)) { explicit_bzero(pass, RS_PASS_MAX + 1); return RS_FORM_USER; }
    memcpy(user, ubuf, (size_t)ul + 1);
    if (pl <= 0 || memchr(pass, 0, (size_t)pl)) { explicit_bzero(pass, RS_PASS_MAX + 1); return RS_FORM_PASS; }
    pass[pl] = 0;
    *passlen = (size_t)pl;
    return RS_FORM_OK;
}

/* ---- WebSocket ---- */

int rs_utf8_valid(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = p[i];
        if (c < 0x80) { i++; continue; }
        size_t k;
        uint32_t cp, min;
        if ((c & 0xe0) == 0xc0) { k = 1; cp = c & 0x1f; min = 0x80; }
        else if ((c & 0xf0) == 0xe0) { k = 2; cp = c & 0x0f; min = 0x800; }
        else if ((c & 0xf8) == 0xf0) { k = 3; cp = c & 0x07; min = 0x10000; }
        else return 0;
        if (n - i <= k) return 0;
        for (size_t j = 1; j <= k; j++) {
            if ((p[i + j] & 0xc0) != 0x80) return 0;
            cp = cp << 6 | (p[i + j] & 0x3f);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        i += k + 1;
    }
    return 1;
}

void rs_ws_init(RsWs *w, size_t max) { memset(w, 0, sizeof *w); w->max = max; }

void rs_ws_free(RsWs *w) {
    if (w->msg) { explicit_bzero(w->msg, w->msg_cap); free(w->msg); }
    memset(w, 0, sizeof *w);
}

int rs_ws_consume(RsWs *w, unsigned char *buf, size_t n, size_t *used, RsWsOn on, void *ud) {
    size_t off = 0;
    int rc = 0;
    while (n - off >= 2) {
        unsigned char *f = buf + off;
        size_t avail = n - off;
        int fin = f[0] & 0x80, op = f[0] & 15;
        if (f[0] & 0x70) { rc = 1002; break; }                  /* no extensions were negotiated */
        if (!(f[1] & 0x80)) { rc = 1002; break; }               /* client frames are masked */
        if ((op > 2 && op < 8) || op > 10) { rc = 1002; break; }
        uint64_t len = f[1] & 127;
        size_t hl = 2;
        if (len == 126) {
            if (avail < 4) break;
            len = (uint64_t)f[2] << 8 | f[3];
            hl = 4;
            if (len < 126) { rc = 1002; break; }                /* not the shortest encoding */
        } else if (len == 127) {
            if (avail < 10) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = len << 8 | f[2 + i];
            hl = 10;
            if (len >> 63 || len < 65536) { rc = 1002; break; }
        }
        int control = op >= 8;
        if (control && (!fin || len > 125)) { rc = 1002; break; }
        if (len > w->max) { rc = 1009; break; }
        if (avail - hl < 4 || avail - hl - 4 < len) break;      /* frame not complete yet */
        unsigned char *m = f + hl, *p = f + hl + 4;
        for (uint64_t i = 0; i < len; i++) p[i] ^= m[i & 3];
        off += hl + 4 + (size_t)len;
        if (control) {
            unsigned char c[126];
            memcpy(c, p, (size_t)len);
            c[len] = 0;
            on(ud, op, c, (size_t)len);
            if (op == 8) break;
            continue;
        }
        if (op == 0 && !w->frag_op) { rc = 1002; break; }       /* continuation of nothing */
        if (op != 0 && w->frag_op) { rc = 1002; break; }        /* new message inside a fragmented one */
        if (op != 0) { w->frag_op = op; w->msg_len = 0; }
        if (w->msg_len + len > w->max) { rc = 1009; break; }
        if (w->msg_len + len + 1 > w->msg_cap) {
            size_t cap = w->msg_cap ? w->msg_cap : 4096;
            while (cap < w->msg_len + len + 1) cap *= 2;
            unsigned char *nb = realloc(w->msg, cap);
            if (!nb) { rc = 1011; break; }
            w->msg = nb;
            w->msg_cap = cap;
        }
        memcpy(w->msg + w->msg_len, p, (size_t)len);
        w->msg_len += (size_t)len;
        if (!fin) continue;
        w->msg[w->msg_len] = 0;
        int mop = w->frag_op;
        w->frag_op = 0;
        if (mop == 1 && !rs_utf8_valid(w->msg, w->msg_len)) { rc = 1007; break; }
        on(ud, mop, w->msg, w->msg_len);
        w->msg_len = 0;
    }
    *used = off;
    return rc;
}

/* ---- flat JSON ---- */

static void jws(const char *j, size_t n, size_t *i) { while (*i < n && (j[*i] == ' ' || j[*i] == '\t' || j[*i] == '\n' || j[*i] == '\r')) (*i)++; }

static int hex4(const char *j, size_t n, size_t i, unsigned *v) {
    if (i + 4 > n) return -1;
    *v = 0;
    for (int k = 0; k < 4; k++) { int h = hexv(j[i + k]); if (h < 0) return -1; *v = *v << 4 | (unsigned)h; }
    return 0;
}

static int put_utf8(char *sb, size_t cap, size_t *so, unsigned cp) {
    if (*so + 5 > cap) return -1;
    if (cp < 0x80) sb[(*so)++] = (char)cp;
    else if (cp < 0x800) { sb[(*so)++] = (char)(0xc0 | cp >> 6); sb[(*so)++] = (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { sb[(*so)++] = (char)(0xe0 | cp >> 12); sb[(*so)++] = (char)(0x80 | ((cp >> 6) & 63)); sb[(*so)++] = (char)(0x80 | (cp & 63)); }
    else { sb[(*so)++] = (char)(0xf0 | cp >> 18); sb[(*so)++] = (char)(0x80 | ((cp >> 12) & 63)); sb[(*so)++] = (char)(0x80 | ((cp >> 6) & 63)); sb[(*so)++] = (char)(0x80 | (cp & 63)); }
    return 0;
}

int rs_json_parse(const char *j, size_t n, RsJson *o, char *sb, size_t cap) {
    memset(o, 0, sizeof *o);
    size_t i = 0, so = 0;
    jws(j, n, &i);
    if (i >= n || j[i++] != '{') return -1;
    jws(j, n, &i);
    if (i < n && j[i] == '}') { i++; goto done; }
    for (;;) {
        if (o->n >= RS_JMAX) return -1;
        jws(j, n, &i);
        if (i >= n || j[i++] != '"') return -1;
        size_t ks = i;
        while (i < n && j[i] >= 'a' && j[i] <= 'z') i++;
        size_t kl = i - ks;
        if (!kl || kl > 11 || i >= n || j[i++] != '"') return -1;
        int k = o->n;
        memcpy(o->key[k], j + ks, kl);
        o->key[k][kl] = 0;
        for (int m = 0; m < k; m++) if (!strcmp(o->key[m], o->key[k])) return -1;
        jws(j, n, &i);
        if (i >= n || j[i++] != ':') return -1;
        jws(j, n, &i);
        if (i >= n) return -1;
        if (j[i] == '"') {
            i++;
            size_t start = so;
            for (;;) {
                if (i >= n) return -1;
                unsigned char c = (unsigned char)j[i];
                if (c == '"') { i++; break; }
                if (c < 0x20) return -1;
                if (c != '\\') { if (so + 2 > cap) return -1; sb[so++] = (char)c; i++; continue; }
                if (++i >= n) return -1;
                char e = j[i++];
                unsigned cp;
                switch (e) {
                case '"': cp = '"'; break;
                case '\\': cp = '\\'; break;
                case '/': cp = '/'; break;
                case 'b': cp = 8; break;
                case 'f': cp = 12; break;
                case 'n': cp = 10; break;
                case 'r': cp = 13; break;
                case 't': cp = 9; break;
                case 'u':
                    if (hex4(j, n, i, &cp)) return -1;
                    i += 4;
                    if (cp >= 0xdc00 && cp <= 0xdfff) return -1;           /* lone low surrogate */
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        unsigned lo;
                        if (i + 2 > n || j[i] != '\\' || j[i + 1] != 'u' || hex4(j, n, i + 2, &lo) || lo < 0xdc00 || lo > 0xdfff) return -1;
                        i += 6;
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    }
                    if (!cp) return -1;
                    break;
                default: return -1;
                }
                if (put_utf8(sb, cap, &so, cp)) return -1;
            }
            if (so + 1 > cap) return -1;
            sb[so] = 0;
            o->is_str[k] = 1;
            o->str[k] = sb + start;
            o->slen[k] = so - start;
            so++;
        } else {
            int neg = 0;
            if (j[i] == '-') { neg = 1; i++; }
            size_t ds = i;
            long long v = 0;
            while (i < n && j[i] >= '0' && j[i] <= '9') {
                if (i - ds >= 16) return -1;
                v = v * 10 + (j[i] - '0');
                i++;
            }
            size_t dl = i - ds;
            if (!dl || (dl > 1 && j[ds] == '0')) return -1;
            if (i < n && (j[i] == '.' || j[i] == 'e' || j[i] == 'E')) return -1;
            if (v >= (1LL << 53)) return -1;
            o->num[k] = neg ? -v : v;
        }
        o->n++;
        jws(j, n, &i);
        if (i >= n) return -1;
        if (j[i] == ',') { i++; continue; }
        if (j[i] == '}') { i++; break; }
        return -1;
    }
done:
    jws(j, n, &i);
    return i == n ? 0 : -1;
}

const char *rs_json_str(const RsJson *o, const char *key, size_t *len) {
    for (int i = 0; i < o->n; i++)
        if (o->is_str[i] && !strcmp(o->key[i], key)) { if (len) *len = o->slen[i]; return o->str[i]; }
    return NULL;
}

int rs_json_num(const RsJson *o, const char *key, long long *v) {
    for (int i = 0; i < o->n; i++)
        if (!o->is_str[i] && !strcmp(o->key[i], key)) { *v = o->num[i]; return 0; }
    return -1;
}

static long long clampll(long long v, long long lo, long long hi) { return v < lo ? lo : v > hi ? hi : v; }

int rs_browser_input(const RsJson *j, int W, int H, RsInput *in) {
    memset(in, 0, sizeof *in);
    size_t tl = 0;
    const char *t = rs_json_str(j, "t", &tl);
    long long a, b;
    size_t pl = 0;
    if (!t || tl > 6) return RS_IN_NONE;
    if (!strcmp(t, "m")) {
        if (W <= 0 || H <= 0 || rs_json_num(j, "x", &a) || rs_json_num(j, "y", &b)) return RS_IN_NONE;
        in->a = (int)clampll(a, 0, W - 1);
        in->b = (int)clampll(b, 0, H - 1);
        return in->type = RS_IN_PTR;
    }
    if (!strcmp(t, "b")) {
        if (rs_json_num(j, "b", &a) || rs_json_num(j, "d", &b) || a < 0 || a > 2) return RS_IN_NONE;
        in->a = (int)a;
        in->b = b != 0;
        return in->type = RS_IN_BTN;
    }
    if (!strcmp(t, "w")) {
        if (rs_json_num(j, "d", &a) || !a) return RS_IN_NONE;
        in->a = (int)clampll(a, -10, 10);
        return in->type = RS_IN_WHEEL;
    }
    if (!strcmp(t, "s") || !strcmp(t, "pz")) {
        int sc = t[0] == 's';
        long long v1, v2;
        if (rs_json_num(j, "p", &a) || a < 1 || a > 3) return RS_IN_NONE;
        if (rs_json_num(j, sc ? "x" : "s", &v1) || rs_json_num(j, sc ? "y" : "r", &v2)) return RS_IN_NONE;
        in->a = (int)a;
        if (sc) {
            in->v1 = (int32_t)clampll(v1, -4000LL * 256, 4000LL * 256);
            in->v2 = (int32_t)clampll(v2, -4000LL * 256, 4000LL * 256);
            return in->type = RS_IN_SCROLL;
        }
        in->v1 = (int32_t)clampll(v1, 3277, 20LL * 65536);          /* scale 0.05 .. 20 */
        in->v2 = (int32_t)clampll(v2, -360LL * 256 * 4, 360LL * 256 * 4);
        return in->type = RS_IN_PINCH;
    }
    if (!strcmp(t, "k")) {
        /* keyboard keys only (KEY_ESC .. KEY_MICMUTE): no BTN_* codes */
        if (rs_json_num(j, "c", &a) || rs_json_num(j, "d", &b) || a < 1 || a > 248) return RS_IN_NONE;
        in->a = (int)a;
        in->b = b != 0;
        return in->type = RS_IN_KEY;
    }
    if (!strcmp(t, "resize")) {
        if (rs_json_num(j, "w", &a) || rs_json_num(j, "h", &b) || a < 640 || b < 480 || a > 3840 || b > 2400) return RS_IN_NONE;
        in->a = (int)(a & ~1LL);
        in->b = (int)(b & ~1LL);
        /* optional: the browser screen's refresh rate (24..360 Hz), for the video's frame rate */
        long long hz = 0;
        if (!rs_json_num(j, "hz", &hz) && (hz < 24 || hz > 360)) return RS_IN_NONE;
        in->v1 = (int32_t)hz;
        return in->type = RS_IN_RESIZE;
    }
    if (!strcmp(t, "ping")) {
        if (rs_json_num(j, "ts", &a) || a < 0) return RS_IN_NONE;
        in->ts = a;
        return in->type = RS_IN_PING;
    }
    if (!strcmp(t, "clip")) {
        size_t l = 0;
        const char *x = rs_json_str(j, "text", &l);
        if (!x || !l || l > RS_CLIP_MAX) return RS_IN_NONE;
        in->text = x;
        in->tlen = l;
        return in->type = RS_IN_CLIP;
    }
    /* the video path's signaling (docs/REMOTE.md, "Video (WebRTC)"); the SDP and the candidate are
     * parsed strictly afterwards by rtcwire.c */
    if (!strcmp(t, "rtc")) {
        size_t l = 0;
        const char *x = rs_json_str(j, "sdp", &l);
        if (!x || l < 10 || l > RS_SDP_MAX) return RS_IN_NONE;
        if (rs_json_str(j, "p", &pl) || (!rs_json_num(j, "p", &a) && (a < 1 || a > 1000000))) return RS_IN_NONE;
        in->a = rs_json_num(j, "p", &a) ? 0 : (int)a;           /* the peer it is for (0: not said) */
        in->text = x;
        in->tlen = l;
        return in->type = RS_IN_RTC;
    }
    if (!strcmp(t, "ice")) {
        size_t l = 0;
        const char *x = rs_json_str(j, "c", &l);
        if (!x || l > 512) return RS_IN_NONE;
        if (rs_json_str(j, "p", &pl) || (!rs_json_num(j, "p", &a) && (a < 1 || a > 1000000))) return RS_IN_NONE;
        in->a = rs_json_num(j, "p", &a) ? 0 : (int)a;
        in->text = x;
        in->tlen = l;
        return in->type = RS_IN_ICE;
    }
    if (!strcmp(t, "vmode")) {
        if (rs_json_num(j, "on", &a) || (a != 0 && a != 1)) return RS_IN_NONE;
        in->a = (int)a;
        return in->type = RS_IN_VMODE;
    }
    return RS_IN_NONE;
}
