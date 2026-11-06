/* dbus.c — own minimal D-Bus client (see dbus.h). */
#include "dbus.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ================= signatures ================= */

static int align_of(char c) {
    switch (c) {
    case 'y': case 'g': case 'v': return 1;
    case 'n': case 'q': return 2;
    case 'b': case 'i': case 'u': case 'h': case 's': case 'o': case 'a': return 4;
    case 'x': case 't': case 'd': case '(': case '{': return 8;
    default: return 0;
    }
}
static int is_basic(char c) { return c && strchr("ybnqiuxtdsogh", c) != NULL; }

static int sig_one(const char *s, size_t *n, int depth, int in_array) {
    if (depth > DBUS_MAX_DEPTH) return -1;
    char c = s[0];
    if (is_basic(c) || c == 'v') { *n = 1; return 0; }
    if (c == 'a') {
        size_t k;
        if (s[1] == '{') {
            if (!is_basic(s[2])) return -1;
            if (sig_one(s + 3, &k, depth + 1, 0)) return -1;
            if (s[3 + k] != '}') return -1;
            *n = 4 + k;
            return 0;
        }
        if (sig_one(s + 1, &k, depth + 1, 1)) return -1;
        *n = 1 + k;
        return 0;
    }
    if (c == '(') {
        size_t p = 1, k;
        if (s[1] == ')') return -1;
        while (s[p] && s[p] != ')') { if (sig_one(s + p, &k, depth + 1, 0)) return -1; p += k; }
        if (s[p] != ')') return -1;
        *n = p + 1;
        return 0;
    }
    (void)in_array;
    return -1;
}

int dbus_sig_single(const char *sig, size_t *n) { return sig_one(sig, n, 0, 0); }

int dbus_sig_valid(const char *sig) {
    size_t len = strlen(sig), p = 0, k;
    if (len > 255) return 0;
    while (p < len) { if (sig_one(sig + p, &k, 0, 0)) return 0; p += k; }
    return 1;
}

int dbus_path_valid(const char *p) {
    if (!p || p[0] != '/') return 0;
    if (!p[1]) return 1;
    int prev_slash = 1;
    for (const char *s = p + 1; *s; s++) {
        char c = *s;
        if (c == '/') { if (prev_slash) return 0; prev_slash = 1; continue; }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return 0;
        prev_slash = 0;
    }
    return !prev_slash;
}

int dbus_name_valid(const char *n) {
    size_t len = n ? strlen(n) : 0;
    if (!len || len > 255) return 0;
    int dots = 0, start = 1;
    for (const char *s = n; *s; s++) {
        char c = *s;
        if (c == '.') { if (start) return 0; dots++; start = 1; continue; }
        int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
        if (!alpha && !(c >= '0' && c <= '9')) return 0;
        if (start && !alpha) return 0;
        start = 0;
    }
    return dots >= 1 && !start;
}

/* ================= body validation ================= */

typedef struct { const uint8_t *b; size_t len, pos; int be; } V;

static uint32_t rd32(const uint8_t *p, int be) {
    return be ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
              : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

static int v_align(V *v, int a) {
    size_t np = (v->pos + (size_t)a - 1) & ~((size_t)a - 1);
    if (np > v->len) return -1;
    for (size_t i = v->pos; i < np; i++) if (v->b[i]) return -1;   /* zero padding */
    v->pos = np;
    return 0;
}

static int v_str(V *v, int path) {
    if (v_align(v, 4) || v->pos + 4 > v->len) return -1;
    uint32_t n = rd32(v->b + v->pos, v->be);
    v->pos += 4;
    if (n > v->len - v->pos || v->len - v->pos - n < 1) return -1;
    const uint8_t *s = v->b + v->pos;
    if (memchr(s, 0, n) || s[n]) return -1;
    if (path && !dbus_path_valid((const char *)s)) return -1;
    v->pos += n + 1;
    return 0;
}

static int v_sig(V *v, int single) {
    if (v->pos >= v->len) return -1;
    uint8_t n = v->b[v->pos++];
    if (n > v->len - v->pos || v->len - v->pos - n < 1) return -1;
    const char *s = (const char *)v->b + v->pos;
    if (memchr(s, 0, n) || s[n]) return -1;
    if (!dbus_sig_valid(s)) return -1;
    if (single) { size_t k; if (!n || sig_one(s, &k, 0, 0) || k != n) return -1; }
    v->pos += (size_t)n + 1;
    return 0;
}

static int v_type(V *v, const char *sig, size_t *used, int depth);

static int v_type(V *v, const char *sig, size_t *used, int depth) {
    if (depth > DBUS_MAX_DEPTH) return -1;
    char c = sig[0];
    size_t k;
    if (sig_one(sig, &k, 0, 0)) return -1;
    *used = k;
    switch (c) {
    case 'y': if (v->pos + 1 > v->len) return -1; v->pos++; return 0;
    case 'n': case 'q': if (v_align(v, 2) || v->pos + 2 > v->len) return -1; v->pos += 2; return 0;
    case 'b': {
        if (v_align(v, 4) || v->pos + 4 > v->len) return -1;
        uint32_t x = rd32(v->b + v->pos, v->be);
        if (x > 1) return -1;
        v->pos += 4;
        return 0;
    }
    case 'i': case 'u': case 'h': if (v_align(v, 4) || v->pos + 4 > v->len) return -1; v->pos += 4; return 0;
    case 'x': case 't': case 'd': if (v_align(v, 8) || v->pos + 8 > v->len) return -1; v->pos += 8; return 0;
    case 's': return v_str(v, 0);
    case 'o': return v_str(v, 1);
    case 'g': return v_sig(v, 0);
    case 'v': {
        size_t sp = v->pos;
        if (v_sig(v, 1)) return -1;
        const char *inner = (const char *)v->b + sp + 1;
        size_t u;
        return v_type(v, inner, &u, depth + 1);
    }
    case 'a': {
        if (v_align(v, 4) || v->pos + 4 > v->len) return -1;
        uint32_t n = rd32(v->b + v->pos, v->be);
        v->pos += 4;
        if (n > 64u * 1024 * 1024) return -1;
        const char *el = sig + 1;
        if (v_align(v, align_of(el[0]))) return -1;
        if (n > v->len - v->pos) return -1;
        size_t end = v->pos + n;
        while (v->pos < end) {
            size_t u;
            if (el[0] == '{') {
                if (v_align(v, 8)) return -1;
                if (v_type(v, el + 1, &u, depth + 1)) return -1;
                size_t u2;
                if (v_type(v, el + 1 + u, &u2, depth + 1)) return -1;
            } else if (v_type(v, el, &u, depth + 1)) return -1;
            if (v->pos > end) return -1;
        }
        return 0;
    }
    case '(': {
        if (v_align(v, 8)) return -1;
        size_t p = 1;
        while (sig[p] != ')') {
            size_t u;
            if (v_type(v, sig + p, &u, depth + 1)) return -1;
            p += u;
        }
        return 0;
    }
    default: return -1;
    }
}

static int v_body(V *v, const char *sig) {
    size_t p = 0, n = strlen(sig);
    while (p < n) { size_t u; if (v_type(v, sig + p, &u, 0)) return -1; p += u; }
    return 0;
}

int dbus_body_valid(const char *sig, const uint8_t *body, size_t len, int big_endian) {
    if (!dbus_sig_valid(sig)) return 0;
    V v = { body, len, 0, big_endian };
    if (v_body(&v, sig)) return 0;
    return v.pos == len;
}

/* ================= messages ================= */

long dbus_msg_size(const uint8_t *buf, size_t len) {
    if (len < 16) return 0;
    if (buf[0] != 'l' && buf[0] != 'B') return -1;
    if (buf[3] != 1) return -1;
    int be = buf[0] == 'B';
    uint32_t body = rd32(buf + 4, be), fields = rd32(buf + 12, be);
    if (fields > 64u * 1024 * 1024 || body > DBUS_MAX_MSG) return -1;
    uint64_t total = ((16u + (uint64_t)fields + 7) & ~(uint64_t)7) + body;
    if (total > DBUS_MAX_MSG) return -1;
    return (long)total;
}

static const char *const field_sig[10] = { NULL, "o", "s", "s", "s", "u", "s", "s", "g", "u" };

int dbus_msg_parse(const uint8_t *buf, size_t len, DMsg *m) {
    int declared;
    return dbus_msg_parse_fds(buf, len, m, 0, &declared);
}

int dbus_msg_parse_fds(const uint8_t *buf, size_t len, DMsg *m, int max_fds, int *declared) {
    memset(m, 0, sizeof *m);
    *declared = 0;
    long total = dbus_msg_size(buf, len);
    if (total <= 0 || (size_t)total != len) return -1;
    int be = buf[0] == 'B';
    uint8_t type = buf[1];
    if (type < DBUS_CALL || type > DBUS_SIGNAL) return -1;
    uint32_t serial = rd32(buf + 8, be);
    if (!serial) return -1;
    m->raw = malloc(len + 1);
    if (!m->raw) return -1;
    memcpy(m->raw, buf, len);
    m->raw[len] = 0;
    m->raw_len = len;
    m->type = type;
    m->flags = buf[2];
    m->big_endian = (uint8_t)be;
    m->serial = serial;
    uint32_t flen = rd32(buf + 12, be);
    size_t hend = 16 + flen;
    /* validate a(yv) from byte 12 with the message alignment */
    V v = { m->raw, hend, 12, be };
    size_t u;
    if (v_type(&v, "a(yv)", &u, 0) || v.pos != hend) goto bad;
    /* zero padding up to 8 */
    size_t bstart = (hend + 7) & ~(size_t)7;
    for (size_t i = hend; i < bstart; i++) if (m->raw[i]) goto bad;
    /* walk the fields */
    size_t p = 16;
    uint32_t got_fds = 0;
    while (p < hend) {
        p = (p + 7) & ~(size_t)7;
        uint8_t code = m->raw[p++];
        uint8_t sl = m->raw[p];
        const char *vs = (const char *)m->raw + p + 1;
        p += 1 + (size_t)sl + 1;
        if (code >= 1 && code <= 9) {
            if (strcmp(vs, field_sig[code])) goto bad;
            if (code == 5 || code == 9) {
                p = (p + 3) & ~(size_t)3;
                uint32_t x = rd32(m->raw + p, be);
                p += 4;
                if (code == 5) m->reply_serial = x; else got_fds = x;
            } else if (code == 8) {
                uint8_t n = m->raw[p];
                m->sig = (const char *)m->raw + p + 1;
                p += 1 + (size_t)n + 1;
            } else {
                p = (p + 3) & ~(size_t)3;
                uint32_t n = rd32(m->raw + p, be);
                const char *s = (const char *)m->raw + p + 4;
                p += 4 + (size_t)n + 1;
                switch (code) {
                case 1: m->path = s; break;
                case 2: m->iface = s; break;
                case 3: m->member = s; break;
                case 4: m->error = s; break;
                case 6: m->dest = s; break;
                case 7: m->sender = s; break;
                }
            }
        } else {
            /* unknown field: skip it (already validated) */
            V w = { m->raw, hend, p, be };
            size_t k;
            if (v_type(&w, vs, &k, 1)) goto bad;
            p = w.pos;
        }
    }
    /* fds only where the connection asked for them (dbus_open_fds), and at most max_fds */
    if (got_fds > (uint32_t)(max_fds > 0 ? max_fds : 0)) goto bad;
    switch (type) {
    case DBUS_CALL: if (!m->path || !m->member) goto bad; break;
    case DBUS_RETURN: if (!m->reply_serial) goto bad; break;
    case DBUS_ERROR: if (!m->reply_serial || !m->error) goto bad; break;
    case DBUS_SIGNAL: if (!m->path || !m->iface || !m->member) goto bad; break;
    }
    if (m->iface && !dbus_name_valid(m->iface)) goto bad;
    if (m->error && !dbus_name_valid(m->error)) goto bad;
    if (m->member) {
        size_t ml = strlen(m->member);
        if (!ml || ml > 255) goto bad;
        for (const char *s = m->member; *s; s++)
            if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_')) goto bad;
        if (m->member[0] >= '0' && m->member[0] <= '9') goto bad;
    }
    m->body = m->raw + bstart;
    m->body_len = (uint32_t)(len - bstart);
    if (!m->sig) { if (m->body_len) goto bad; m->sig = ""; }
    else if (!dbus_body_valid(m->sig, m->body, m->body_len, be)) goto bad;
    if (!got_fds && strchr(m->sig, 'h')) goto bad;      /* an fd's index with no fds */
    *declared = (int)got_fds;
    return 0;
bad:
    dbus_msg_free(m);
    return -1;
}

void dbus_msg_drop_fds(DMsg *m) {
    for (int i = 0; i < m->n_fds && i < DBUS_MAX_FDS; i++) if (m->fds[i] >= 0) close(m->fds[i]);
    m->n_fds = 0;
}

void dbus_msg_free(DMsg *m) {
    dbus_msg_drop_fds(m);
    if (m->raw) { explicit_bzero(m->raw, m->raw_len); free(m->raw); }
    memset(m, 0, sizeof *m);
}

/* ================= typed reader ================= */

void dr_init(DRd *r, const DMsg *m) { r->b = m->body; r->len = m->body_len; r->pos = 0; r->be = m->big_endian; r->err = 0; }

static int r_align(DRd *r, int a) {
    size_t np = (r->pos + (size_t)a - 1) & ~((size_t)a - 1);
    if (np > r->len) { r->err = 1; return -1; }
    r->pos = np;
    return 0;
}
static int r_need(DRd *r, size_t n) { if (r->err || n > r->len - r->pos) { r->err = 1; return -1; } return 0; }

uint8_t dr_u8(DRd *r) { if (r_need(r, 1)) return 0; return r->b[r->pos++]; }
uint32_t dr_u32(DRd *r) {
    if (r->err || r_align(r, 4) || r_need(r, 4)) return 0;
    uint32_t x = rd32(r->b + r->pos, r->be);
    r->pos += 4;
    return x;
}
int dr_bool(DRd *r) { return dr_u32(r) != 0; }
int32_t dr_i32(DRd *r) { return (int32_t)dr_u32(r); }
uint64_t dr_u64(DRd *r) {
    if (r->err || r_align(r, 8) || r_need(r, 8)) return 0;
    uint64_t lo = rd32(r->b + r->pos + (r->be ? 4 : 0), r->be), hi = rd32(r->b + r->pos + (r->be ? 0 : 4), r->be);
    r->pos += 8;
    return hi << 32 | lo;
}
const char *dr_str(DRd *r, uint32_t *len) {
    uint32_t n = dr_u32(r);
    if (r->err || r_need(r, (size_t)n + 1)) { if (len) *len = 0; return ""; }
    const char *s = (const char *)r->b + r->pos;
    r->pos += (size_t)n + 1;
    if (len) *len = n;
    return s;
}
const char *dr_sig(DRd *r) {
    uint8_t n = dr_u8(r);
    if (r->err || r_need(r, (size_t)n + 1)) return "";
    const char *s = (const char *)r->b + r->pos;
    r->pos += (size_t)n + 1;
    return s;
}
const uint8_t *dr_bytes(DRd *r, uint32_t *len) {
    uint32_t n = dr_u32(r);
    if (r->err || r_need(r, n)) { *len = 0; return (const uint8_t *)""; }
    const uint8_t *p = r->b + r->pos;
    r->pos += n;
    *len = n;
    return p;
}
size_t dr_arr(DRd *r, int elem_align) {
    uint32_t n = dr_u32(r);
    if (r->err || r_align(r, elem_align) || r_need(r, n)) return r->pos;
    return r->pos + n;
}
int dr_more(DRd *r, size_t end) { return !r->err && r->pos < end; }
void dr_struct(DRd *r) { r_align(r, 8); }
const char *dr_variant(DRd *r) { return dr_sig(r); }
void dr_skip(DRd *r, const char *sig) {
    if (r->err) return;
    V v = { r->b, r->len, r->pos, r->be };
    size_t u;
    if (v_type(&v, sig, &u, 0)) { r->err = 1; return; }
    r->pos = v.pos;
}

/* ================= writer ================= */

void dw_init(DBuf *w) { memset(w, 0, sizeof *w); }
void dw_free(DBuf *w) { if (w->p) { explicit_bzero(w->p, w->cap); free(w->p); } memset(w, 0, sizeof *w); }

static int w_need(DBuf *w, size_t n) {
    if (w->oom) return -1;
    if (n > DBUS_MAX_MSG || w->n + n > DBUS_MAX_MSG) { w->oom = 1; return -1; }
    if (w->n + n <= w->cap) return 0;
    size_t nc = w->cap ? w->cap : 256;
    while (nc < w->n + n) nc *= 2;
    uint8_t *p = malloc(nc);
    if (!p) { w->oom = 1; return -1; }
    if (w->p) { memcpy(p, w->p, w->n); explicit_bzero(w->p, w->cap); free(w->p); }   /* no realloc: the old data is wiped */
    w->p = p;
    w->cap = nc;
    return 0;
}
static void w_put(DBuf *w, const void *p, size_t n) { if (!n || w_need(w, n)) return; memcpy(w->p + w->n, p, n); w->n += n; }

void dw_align(DBuf *w, int a) {
    size_t np = (w->n + (size_t)a - 1) & ~((size_t)a - 1);
    if (np == w->n || w_need(w, np - w->n)) return;
    memset(w->p + w->n, 0, np - w->n);
    w->n = np;
}
void dw_u8(DBuf *w, uint8_t v) { w_put(w, &v, 1); }
void dw_u32(DBuf *w, uint32_t v) {
    dw_align(w, 4);
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    w_put(w, b, 4);
}
void dw_bool(DBuf *w, int v) { dw_u32(w, v ? 1 : 0); }
void dw_i32(DBuf *w, int32_t v) { dw_u32(w, (uint32_t)v); }
void dw_u64(DBuf *w, uint64_t v) {
    dw_align(w, 8);
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    w_put(w, b, 8);
}
void dw_strn(DBuf *w, const char *s, size_t n) {
    dw_u32(w, (uint32_t)n);
    w_put(w, s, n);
    dw_u8(w, 0);
}
void dw_str(DBuf *w, const char *s) { dw_strn(w, s ? s : "", s ? strlen(s) : 0); }
void dw_sig(DBuf *w, const char *s) {
    size_t n = strlen(s);
    if (n > 255) { w->oom = 1; return; }
    dw_u8(w, (uint8_t)n);
    w_put(w, s, n);
    dw_u8(w, 0);
}
void dw_bytes(DBuf *w, const uint8_t *p, size_t n) { dw_u32(w, (uint32_t)n); w_put(w, p, n); }
size_t dw_arr_begin(DBuf *w, int elem_align) {
    dw_align(w, 4);
    size_t mark = w->n;
    dw_u32(w, 0);
    dw_align(w, elem_align);
    return mark << 4 | (size_t)elem_align;
}
void dw_arr_end(DBuf *w, size_t mark) {
    size_t off = mark >> 4, a = mark & 15;
    if (w->oom || off + 4 > w->n) return;
    size_t start = (off + 4 + a - 1) & ~(a - 1);
    uint32_t len = (uint32_t)(w->n - start);
    w->p[off] = (uint8_t)len; w->p[off + 1] = (uint8_t)(len >> 8); w->p[off + 2] = (uint8_t)(len >> 16); w->p[off + 3] = (uint8_t)(len >> 24);
}
void dw_struct(DBuf *w) { dw_align(w, 8); }
void dw_variant(DBuf *w, const char *sig) { dw_sig(w, sig); }

static void field_s(DBuf *w, uint8_t code, const char *sig, const char *val) {
    if (!val) return;
    dw_struct(w);
    dw_u8(w, code);
    dw_sig(w, sig);
    if (sig[0] == 'g') dw_sig(w, val); else dw_str(w, val);
}

int dbus_build(const DHdr *h, const DBuf *body, DBuf *out) {
    dw_init(out);
    uint8_t fixed[4] = { 'l', h->type, h->flags, 1 };
    w_put(out, fixed, 4);
    dw_u32(out, body ? (uint32_t)body->n : 0);
    dw_u32(out, h->serial);
    size_t mark = dw_arr_begin(out, 8);
    field_s(out, 1, "o", h->path);
    field_s(out, 2, "s", h->iface);
    field_s(out, 3, "s", h->member);
    field_s(out, 4, "s", h->error);
    if (h->reply_serial) { dw_struct(out); dw_u8(out, 5); dw_sig(out, "u"); dw_u32(out, h->reply_serial); }
    field_s(out, 6, "s", h->dest);
    field_s(out, 7, "s", h->sender);
    if (h->sig && h->sig[0]) field_s(out, 8, "g", h->sig);
    dw_arr_end(out, mark);
    dw_align(out, 8);
    if (body && body->n) w_put(out, body->p, body->n);
    if (body && body->oom) out->oom = 1;
    return out->oom ? -1 : 0;
}

/* ================= connection ================= */

int dbus_connect_session(void) {
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    socklen_t alen = 0;
    const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
    char path[sizeof a.sun_path];
    int abstract = 0;
    path[0] = 0;
    if (addr) {
        /* several addresses separated by ';': the first usable unix: one */
        const char *p = addr;
        while (p && *p && !path[0]) {
            const char *end = strchr(p, ';');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            if (n > 5 && !strncmp(p, "unix:", 5)) {
                const char *kv = p + 5;
                while (kv < p + n) {
                    const char *c = memchr(kv, ',', (size_t)(p + n - kv));
                    size_t kl = c ? (size_t)(c - kv) : (size_t)(p + n - kv);
                    int isabs = kl > 9 && !strncmp(kv, "abstract=", 9);
                    if ((kl > 5 && !strncmp(kv, "path=", 5)) || isabs) {
                        const char *v = kv + (isabs ? 9 : 5);
                        size_t vl = kl - (isabs ? 9 : 5), o = 0;
                        for (size_t i = 0; i < vl && o + 1 < sizeof path; i++) {
                            if (v[i] == '%' && i + 2 < vl) { unsigned x; if (sscanf(v + i + 1, "%2x", &x) == 1) { path[o++] = (char)x; i += 2; continue; } }
                            path[o++] = v[i];
                        }
                        path[o] = 0;
                        abstract = isabs;
                        break;
                    }
                    if (!c) break;
                    kv = c + 1;
                }
            }
            p = end ? end + 1 : NULL;
        }
    }
    if (!path[0]) {
        const char *rt = getenv("XDG_RUNTIME_DIR");
        if (!rt || rt[0] != '/') return -1;
        if ((size_t)snprintf(path, sizeof path, "%s/bus", rt) >= sizeof path) return -1;
    }
    size_t pl = strlen(path);
    if (abstract) {
        if (pl + 1 > sizeof a.sun_path) return -1;
        a.sun_path[0] = 0;
        memcpy(a.sun_path + 1, path, pl);
        alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + pl);
    } else {
        if (pl >= sizeof a.sun_path) return -1;
        memcpy(a.sun_path, path, pl + 1);
        alen = (socklen_t)sizeof a;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&a, alen)) { close(fd); return -1; }
    return fd;
}

/* System bus (BlueZ, power-profiles-daemon; Settings, see docs/SETTINGS.md):
 * DBUS_SYSTEM_BUS_ADDRESS if it is a unix:path=, else /run/dbus/system_bus_socket. */
int dbus_connect_system(void) {
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    const char *addr = getenv("DBUS_SYSTEM_BUS_ADDRESS"), *path = "/run/dbus/system_bus_socket";
    char buf[sizeof a.sun_path];
    if (addr && !strncmp(addr, "unix:path=", 10)) {
        size_t n = strcspn(addr + 10, ",;");
        if (n && n < sizeof buf) { memcpy(buf, addr + 10, n); buf[n] = 0; path = buf; }
    }
    if (strlen(path) >= sizeof a.sun_path) return -1;
    memcpy(a.sun_path, path, strlen(path) + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    return fd;
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int wait_fd(int fd, short ev, double deadline) {
    for (;;) {
        double left = deadline - now_ms();
        if (left <= 0) return -1;
        struct pollfd p = { fd, ev, 0 };
        int r = poll(&p, 1, (int)left + 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        if (p.revents & (POLLERR | POLLNVAL)) return -1;
        return 0;
    }
}

static int write_all(int fd, const void *buf, size_t n, double deadline) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { if (wait_fd(fd, POLLOUT, deadline)) return -1; continue; }
            return -1;
        }
        p += k; n -= (size_t)k;
    }
    return 0;
}

/* One SASL line ("...\r\n"), one byte at a time so as not to swallow any of the binary protocol. */
static int read_line(int fd, char *out, size_t cap, double deadline) {
    size_t n = 0;
    for (;;) {
        char c;
        ssize_t k = read(fd, &c, 1);
        if (k < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { if (wait_fd(fd, POLLIN, deadline)) return -1; continue; }
            return -1;
        }
        if (k == 0) return -1;
        if (n + 1 >= cap) return -1;
        out[n++] = c;
        if (n >= 2 && out[n - 2] == '\r' && out[n - 1] == '\n') { out[n - 2] = 0; return 0; }
    }
}

static int open_conn(DConn *c, int fd, int timeout_ms, int want_fds) {
    memset(c, 0, sizeof *c);
    c->fd = fd;
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    double deadline = now_ms() + timeout_ms;
    char uid[16], hex[40], line[512];
    snprintf(uid, sizeof uid, "%u", (unsigned)getuid());
    size_t k = 0;
    for (const char *s = uid; *s; s++) k += (size_t)snprintf(hex + k, sizeof hex - k, "%02x", (unsigned char)*s);
    char msg[96];
    int ml = snprintf(msg, sizeof msg, "AUTH EXTERNAL %s\r\n", hex);
    if (write_all(fd, "", 1, deadline) || write_all(fd, msg, (size_t)ml, deadline)) return -1;
    if (read_line(fd, line, sizeof line, deadline) || strncmp(line, "OK ", 3)) return -1;
    if (want_fds) {                     /* "ERROR" (the bus can't) leaves a plain connection */
        if (write_all(fd, "NEGOTIATE_UNIX_FD\r\n", 19, deadline) || read_line(fd, line, sizeof line, deadline)) return -1;
        c->unix_fds = !strcmp(line, "AGREE_UNIX_FD");
    }
    if (write_all(fd, "BEGIN\r\n", 7, deadline)) return -1;
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus", .member = "Hello", .dest = "org.freedesktop.DBus" };
    uint32_t s = dbus_send(c, &h, NULL);
    if (!s) return -1;
    DMsg m;
    double left = deadline - now_ms();
    if (dbus_wait_reply(c, s, &m, left > 0 ? (int)left : 1) != 1) { dbus_msg_free(&m); return -1; }
    DRd r;
    dr_init(&r, &m);
    if (strcmp(m.sig, "s")) { dbus_msg_free(&m); return -1; }
    snprintf(c->unique, sizeof c->unique, "%s", dr_str(&r, NULL));
    dbus_msg_free(&m);
    return 0;
}

int dbus_open(DConn *c, int fd, int timeout_ms) { return open_conn(c, fd, timeout_ms, 0); }
int dbus_open_fds(DConn *c, int fd, int timeout_ms) { return open_conn(c, fd, timeout_ms, 1); }

void dbus_close(DConn *c) {
    for (int i = 0; i < c->fdq_n && i < DBUS_MAX_FDS; i++) close(c->fdq[i]);
    c->fdq_n = 0;
    if (c->fd >= 0) close(c->fd);
    if (c->in) { explicit_bzero(c->in, c->in_cap); free(c->in); }
    if (c->out) { explicit_bzero(c->out, c->out_cap); free(c->out); }
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

int dbus_pending_out(const DConn *c) { return c->out_n > c->out_off; }

int dbus_flush(DConn *c) {
    if (c->dead) return -1;
    while (c->out_off < c->out_n) {
        ssize_t k = send(c->fd, c->out + c->out_off, c->out_n - c->out_off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (k < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            c->dead = 1;
            return -1;
        }
        c->out_off += (size_t)k;
    }
    if (c->out_off == c->out_n) { if (c->out) explicit_bzero(c->out, c->out_n); c->out_off = c->out_n = 0; }
    else if (c->out_off > c->out_cap / 2) {
        size_t rest = c->out_n - c->out_off;
        memmove(c->out, c->out + c->out_off, rest);
        explicit_bzero(c->out + rest, c->out_n - rest);
        c->out_n = rest;
        c->out_off = 0;
    }
    return 0;
}

uint32_t dbus_send(DConn *c, DHdr *h, const DBuf *body) {
    if (c->dead) return 0;
    if (!h->serial) { if (++c->serial == 0) c->serial = 1; h->serial = c->serial; }
    DBuf msg;
    if (dbus_build(h, body, &msg)) { dw_free(&msg); return 0; }
    if (c->out_n + msg.n > 64u * 1024 * 1024) { dw_free(&msg); c->dead = 1; return 0; }
    if (c->out_n + msg.n > c->out_cap) {
        size_t nc = c->out_cap ? c->out_cap : 4096;
        while (nc < c->out_n + msg.n) nc *= 2;
        uint8_t *p = malloc(nc);
        if (!p) { dw_free(&msg); return 0; }
        if (c->out) { memcpy(p, c->out, c->out_n); explicit_bzero(c->out, c->out_cap); free(c->out); }
        c->out = p;
        c->out_cap = nc;
    }
    memcpy(c->out + c->out_n, msg.p, msg.n);
    c->out_n += msg.n;
    dw_free(&msg);
    dbus_flush(c);
    return h->serial;
}

static int take(DConn *c, DMsg *m) {
    long sz = dbus_msg_size(c->in, c->in_n);
    if (sz < 0) { c->dead = 1; return -1; }
    if (sz == 0 || (size_t)sz > c->in_n) return 0;
    int declared = 0;
    int r = dbus_msg_parse_fds(c->in, (size_t)sz, m, c->unix_fds ? DBUS_MAX_FDS : 0, &declared);
    size_t rest = c->in_n - (size_t)sz;
    memmove(c->in, c->in + sz, rest);
    explicit_bzero(c->in + rest, (size_t)sz);
    c->in_n = rest;
    if (r) { c->dead = 1; return -1; }       /* the bus already validates: something invalid is a serious error */
    /* the fds came with the message's first bytes, in order: they are the oldest ones queued */
    if (declared > c->fdq_n) { dbus_msg_free(m); c->dead = 1; return -1; }
    for (int i = 0; i < declared; i++) m->fds[i] = c->fdq[i];
    m->n_fds = declared;
    c->fdq_n -= declared;
    memmove(c->fdq, c->fdq + declared, (size_t)c->fdq_n * sizeof c->fdq[0]);
    return 1;
}

/* recv, and on a connection that passes fds, recvmsg keeping the fds that came (more than the queue
 * holds are closed: the message that needs them is then refused in take). */
static ssize_t recv_in(DConn *c, void *buf, size_t n) {
    if (!c->unix_fds) return recv(c->fd, buf, n, MSG_DONTWAIT);
    struct iovec io = { buf, n };
    union { struct cmsghdr h; char b[CMSG_SPACE(16 * sizeof(int))]; } cb;
    struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cb.b, .msg_controllen = sizeof cb.b };
    ssize_t k = recvmsg(c->fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (k < 0) return k;
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&mh); h; h = CMSG_NXTHDR(&mh, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) continue;
        int nf = (int)((h->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        for (int i = 0; i < nf; i++) {
            int f;
            memcpy(&f, CMSG_DATA(h) + i * sizeof(int), sizeof f);
            if (c->fdq_n < DBUS_MAX_FDS) c->fdq[c->fdq_n++] = f;
            else close(f);
        }
    }
    return k;
}

int dbus_next(DConn *c, DMsg *m) {
    memset(m, 0, sizeof *m);
    if (c->dead) return -1;
    for (;;) {
        int t = take(c, m);
        if (t) return t;
        if (c->in_cap - c->in_n < 65536) {
            if (c->in_cap > DBUS_MAX_MSG + 65536) { c->dead = 1; return -1; }
            size_t nc = c->in_cap ? c->in_cap * 2 : 65536;
            uint8_t *p = malloc(nc);
            if (!p) { c->dead = 1; return -1; }
            if (c->in) { memcpy(p, c->in, c->in_n); explicit_bzero(c->in, c->in_cap); free(c->in); }
            c->in = p;
            c->in_cap = nc;
        }
        ssize_t k = recv_in(c, c->in + c->in_n, c->in_cap - c->in_n);
        if (k < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            c->dead = 1;
            return -1;
        }
        if (k == 0) { c->dead = 1; return -1; }
        c->in_n += (size_t)k;
    }
}

int dbus_wait_reply(DConn *c, uint32_t serial, DMsg *m, int timeout_ms) {
    double deadline = now_ms() + timeout_ms;
    for (;;) {
        dbus_flush(c);
        int r = dbus_next(c, m);
        if (r < 0) return -1;
        if (r == 1) {
            if ((m->type == DBUS_RETURN || m->type == DBUS_ERROR) && m->reply_serial == serial) return m->type == DBUS_RETURN ? 1 : 0;
            dbus_msg_free(m);
            continue;
        }
        if (wait_fd(c->fd, dbus_pending_out(c) ? POLLIN | POLLOUT : POLLIN, deadline)) return -1;
    }
}

uint32_t dbus_reply(DConn *c, const DMsg *call, const char *sig, const DBuf *body) {
    if (call->flags & DBUS_NO_REPLY) return 0;
    DHdr h = { .type = DBUS_RETURN, .reply_serial = call->serial, .dest = call->sender, .sig = sig };
    return dbus_send(c, &h, body);
}

uint32_t dbus_error(DConn *c, const DMsg *call, const char *name, const char *text) {
    if (call->flags & DBUS_NO_REPLY) return 0;
    DBuf b;
    dw_init(&b);
    dw_str(&b, text ? text : "");
    DHdr h = { .type = DBUS_ERROR, .reply_serial = call->serial, .dest = call->sender, .error = name, .sig = "s" };
    uint32_t s = dbus_send(c, &h, &b);
    dw_free(&b);
    return s;
}

uint32_t dbus_signal(DConn *c, const char *path, const char *iface, const char *member, const char *sig, const DBuf *body) {
    DHdr h = { .type = DBUS_SIGNAL, .path = path, .iface = iface, .member = member, .sig = sig };
    return dbus_send(c, &h, body);
}
