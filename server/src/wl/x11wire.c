/* x11wire.c — X11 wire protocol helpers for the window manager (see x11wire.h). */
#include "x11wire.h"

#include <stdlib.h>
#include <string.h>

/* ---- output buffer ---- */

static int xo_room(XOut *o, size_t more) {
    if (o->oom) return 0;
    if (o->n + more <= o->cap) return 1;
    size_t cap = o->cap ? o->cap : 4096;
    while (cap < o->n + more) cap *= 2;
    if (cap > (64u << 20)) { o->oom = 1; return 0; }        /* a stuck peer: never grow without bound */
    uint8_t *nb = realloc(o->b, cap);
    if (!nb) { o->oom = 1; return 0; }
    o->b = nb;
    o->cap = cap;
    return 1;
}

void xo_u8(XOut *o, uint8_t v) { if (xo_room(o, 1)) o->b[o->n++] = v; }
void xo_u16(XOut *o, uint16_t v) { if (xo_room(o, 2)) { o->b[o->n++] = (uint8_t)v; o->b[o->n++] = (uint8_t)(v >> 8); } }
void xo_u32(XOut *o, uint32_t v) {
    if (!xo_room(o, 4)) return;
    for (int i = 0; i < 4; i++) o->b[o->n++] = (uint8_t)(v >> (8 * i));
}
void xo_pad(XOut *o, size_t n) { if (xo_room(o, n)) { memset(o->b + o->n, 0, n); o->n += n; } }
void xo_bytes(XOut *o, const void *p, size_t n) {
    if (!xo_room(o, n + 3)) return;
    if (n) memcpy(o->b + o->n, p, n);
    o->n += n;
    xo_pad(o, (4 - n % 4) % 4);
}

void xo_begin(XOut *o, uint8_t opcode, uint8_t data) {
    o->start = o->n;
    xo_u8(o, opcode);
    xo_u8(o, data);
    xo_u16(o, 0);
}

void xo_end(XOut *o) {
    if (o->oom) return;
    size_t len = (o->n - o->start + 3) / 4;
    o->b[o->start + 2] = (uint8_t)len;
    o->b[o->start + 3] = (uint8_t)(len >> 8);
}

void xo_drop(XOut *o, size_t n) {
    if (n >= o->n) { o->n = 0; return; }
    memmove(o->b, o->b + n, o->n - n);
    o->n -= n;
}

void xo_free(XOut *o) { free(o->b); memset(o, 0, sizeof *o); }

/* ---- connection setup ---- */

void x11_setup_request(uint8_t out[12]) {
    memset(out, 0, 12);
    out[0] = 'l';               /* little endian */
    out[2] = 11;                /* protocol 11.0 */
}

int x11_parse_setup(const uint8_t *p, size_t n, XSetup *s, size_t *used) {
    if (n < 8) return 0;
    size_t total = 8 + 4 * (size_t)x11_get16(p + 6);
    if (n < total) return 0;
    *used = total;
    if (p[0] != 1 || total < 40) return -1;                 /* 0 failed, 2 authenticate */
    const uint8_t *d = p + 8;
    memset(s, 0, sizeof *s);
    s->id_base = x11_get32(d + 4);
    s->id_mask = x11_get32(d + 8);
    uint16_t vendor_len = x11_get16(d + 16);
    s->max_req_bytes = (uint32_t)x11_get16(d + 18) * 4;
    uint8_t nscreens = d[20], nformats = d[21];
    s->min_keycode = d[26];
    s->max_keycode = d[27];
    size_t off = 32 + ((size_t)vendor_len + 3) / 4 * 4 + (size_t)nformats * 8;
    if (!nscreens || off + 40 > total - 8 || !s->id_mask) return -1;
    const uint8_t *scr = d + off;
    s->root = x11_get32(scr);
    s->width = x11_get16(scr + 20);
    s->height = x11_get16(scr + 22);
    s->root_visual = x11_get32(scr + 32);
    s->root_depth = scr[38];
    return s->root ? 1 : -1;
}

size_t x11_packet_len(const uint8_t *p, size_t n) {
    if (n < 32) return 0;
    size_t len = 32;
    if (p[0] == X_Reply || (p[0] & 0x7f) == X_GenericEvent) len += 4 * (size_t)x11_get32(p + 4);
    return n >= len ? len : 0;
}

uint32_t x11_seq_widen(uint32_t last_sent, uint16_t s16) {
    uint32_t full = (last_sent & ~0xffffu) | s16;
    if (full > last_sent) full -= 0x10000u;                 /* it can only be one already sent */
    return full;
}

/* ---- properties ---- */

static void ascii_copy(const uint8_t *v, size_t n, char *out, size_t size) {
    size_t o = 0;
    for (size_t i = 0; i < n && v[i] && o + 1 < size; i++)
        if (v[i] >= 0x20 && v[i] < 0x7f) out[o++] = (char)v[i];
    if (size) out[o] = 0;
}

void x11_wm_class(const uint8_t *v, size_t n, char *inst, size_t ni, char *cls, size_t nc) {
    if (ni) inst[0] = 0;
    if (nc) cls[0] = 0;
    if (!v || !n) return;
    size_t a = 0;
    while (a < n && v[a]) a++;
    ascii_copy(v, a, inst, ni);
    if (a + 1 < n) {
        size_t b = a + 1, e = b;
        while (e < n && v[e]) e++;
        ascii_copy(v + b, e - b, cls, nc);
    }
}

/* Appends one code point as UTF-8 (control characters and surrogates are dropped). */
static size_t put_utf8(char *out, size_t o, size_t size, uint32_t cp) {
    if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0) || (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) return o;
    uint8_t b[4];
    size_t k;
    if (cp < 0x80) { b[0] = (uint8_t)cp; k = 1; }
    else if (cp < 0x800) { b[0] = (uint8_t)(0xc0 | cp >> 6); b[1] = (uint8_t)(0x80 | (cp & 0x3f)); k = 2; }
    else if (cp < 0x10000) { b[0] = (uint8_t)(0xe0 | cp >> 12); b[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f)); b[2] = (uint8_t)(0x80 | (cp & 0x3f)); k = 3; }
    else { b[0] = (uint8_t)(0xf0 | cp >> 18); b[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3f)); b[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f)); b[3] = (uint8_t)(0x80 | (cp & 0x3f)); k = 4; }
    if (o + k + 1 > size) return o;
    memcpy(out + o, b, k);
    return o + k;
}

void x11_title(const uint8_t *v, size_t n, int latin1, char *out, size_t size) {
    size_t o = 0;
    if (!size) return;
    for (size_t i = 0; v && i < n && o + 1 < size;) {
        uint32_t cp = v[i];
        if (!cp) break;
        if (latin1 || cp < 0x80) { i++; o = put_utf8(out, o, size, cp); continue; }
        int k = cp >= 0xf0 && cp < 0xf5 ? 4 : cp >= 0xe0 ? 3 : cp >= 0xc2 && cp < 0xe0 ? 2 : 0;
        if (cp >= 0xf5) k = 0;
        uint32_t c = k == 2 ? cp & 0x1f : k == 3 ? cp & 0x0f : cp & 0x07;
        int ok = k > 0 && i + (size_t)k <= n;
        for (int j = 1; ok && j < k; j++) {
            if ((v[i + (size_t)j] & 0xc0) != 0x80) ok = 0;
            else c = c << 6 | (v[i + (size_t)j] & 0x3f);
        }
        if (ok && ((k == 3 && c < 0x800) || (k == 4 && (c < 0x10000 || c > 0x10ffff)))) ok = 0;   /* overlong */
        if (!ok) { i++; o = put_utf8(out, o, size, 0xfffd); continue; }
        i += (size_t)k;
        o = put_utf8(out, o, size, c);
    }
    out[o] = 0;
}

int x11_motif_decorated(const uint32_t *v, size_t n) {
    if (!v || n < 3 || !(v[0] & 2)) return -1;              /* MWM_HINTS_DECORATIONS */
    return v[2] != 0;
}

void x11_size_hints(const uint32_t *v, size_t n, XSizeHints *h) {
    memset(h, 0, sizeof *h);
    if (!v || n < 9) return;
    if (v[0] & 16) { h->min_w = (int)v[5]; h->min_h = (int)v[6]; }      /* PMinSize */
    if (v[0] & 32) { h->max_w = (int)v[7]; h->max_h = (int)v[8]; }      /* PMaxSize */
    if (h->min_w < 0 || h->min_w > 16384) h->min_w = 0;
    if (h->min_h < 0 || h->min_h > 16384) h->min_h = 0;
    if (h->max_w < 0 || h->max_w > 32767) h->max_w = 0;
    if (h->max_h < 0 || h->max_h > 32767) h->max_h = 0;
}

int x11_wm_hints_input(const uint32_t *v, size_t n) {
    if (!v || n < 2 || !(v[0] & 1)) return 1;               /* InputHint absent: it takes focus */
    return v[1] != 0;
}

int x11_moveresize_edges(uint32_t d) {
    static const int edges[8] = { 1 | 4, 1, 1 | 8, 8, 2 | 8, 2, 2 | 4, 4 };
    if (d < 8) return edges[d];
    if (d == 8) return -1;                                  /* _NET_WM_MOVERESIZE_MOVE */
    return 0;                                               /* keyboard variants, cancel */
}

int x11_text_rank(const char *m) {
    if (!m) return 0;
    if (!strcmp(m, "UTF8_STRING")) return 5;
    if (!strcmp(m, "text/plain;charset=utf-8")) return 4;
    if (!strcmp(m, "text/plain")) return 2;
    if (!strcmp(m, "STRING")) return 3;
    if (!strcmp(m, "TEXT")) return 1;
    return 0;
}

int x11_text_target(const char *m) { return x11_text_rank(m) > 0; }

int x11_pick_display(int (*taken)(int n, void *ctx), void *ctx) {
    for (int n = 0; n < 64; n++) if (!taken(n, ctx)) return n;
    return -1;
}

/* ---- drag and drop (XDND) ---- */

int x11_xdnd_version(uint32_t aware) {
    if (aware < 3) return 0;
    return aware < XDND_VERSION ? (int)aware : XDND_VERSION;
}

void x11_xdnd_enter(uint32_t d[5], uint32_t src, int version, const uint32_t *types, int n) {
    d[0] = src;
    d[1] = (uint32_t)version << 24 | (n > 3 ? 1u : 0u);
    for (int i = 0; i < 3; i++) d[2 + i] = i < n ? types[i] : 0;
}

static uint32_t xy16(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > 32767) x = 32767;
    if (y > 32767) y = 32767;
    return (uint32_t)x << 16 | (uint32_t)y;
}

void x11_xdnd_position(uint32_t d[5], uint32_t src, int x, int y, uint32_t time, uint32_t action) {
    d[0] = src;
    d[1] = 0;
    d[2] = xy16(x, y);
    d[3] = time;
    d[4] = action;
}

void x11_xdnd_status(uint32_t d[5], uint32_t target, int accept, uint32_t action) {
    d[0] = target;
    d[1] = accept ? 1u : 0u;
    d[2] = d[3] = 0;
    d[4] = accept ? action : 0;
}

void x11_xdnd_finished(uint32_t d[5], uint32_t target, int accepted, uint32_t action) {
    d[0] = target;
    d[1] = accepted ? 1u : 0u;
    d[2] = accepted ? action : 0;
    d[3] = d[4] = 0;
}

int x11_xdnd_parse_enter(const uint32_t d[5], XdndEnter *e) {
    memset(e, 0, sizeof *e);
    e->win = d[0];
    e->version = (int)(d[1] >> 24);
    e->more = (int)(d[1] & 1);
    for (int i = 0; i < 3; i++) if (d[2 + i]) e->types[e->n++] = d[2 + i];
    if (e->version > XDND_VERSION) e->version = XDND_VERSION;
    return e->win && e->version >= 3;
}

void x11_xdnd_parse_status(const uint32_t d[5], XdndStatus *s) {
    s->win = d[0];
    s->accept = (int)(d[1] & 1);
    s->action = s->accept ? d[4] : 0;
}

void x11_xdnd_xy(uint32_t v, int *x, int *y) {
    *x = (int)(v >> 16);
    *y = (int)(v & 0xffff);
}

int x11_atom_list(const uint8_t *v, size_t bytes, int format, uint32_t type, uint32_t *out, int max) {
    int n = 0;
    if (!v || format != 32 || type != 4 /* ATOM */) return 0;
    for (size_t i = 0; i + 4 <= bytes && n < max; i += 4) {
        uint32_t a = x11_get32(v + i);
        if (a) out[n++] = a;
    }
    return n;
}

int x11_dnd_mime(const uint8_t *name, size_t n, char *out, size_t size) {
    static const char utf8[] = "text/plain;charset=utf-8";
    if (!size) return 0;
    out[0] = 0;
    if (!name || !n || n >= size) return 0;
    if (n == 11 && !memcmp(name, "UTF8_STRING", 11)) {
        if (sizeof utf8 > size) return 0;
        memcpy(out, utf8, sizeof utf8);
        return 1;
    }
    int slash = 0;
    for (size_t i = 0; i < n; i++) {
        if (name[i] <= 0x20 || name[i] >= 0x7f) return 0;
        if (name[i] == '/') slash = 1;
    }
    if (!slash || name[0] == '/') return 0;
    memcpy(out, name, n);
    out[n] = 0;
    return 1;
}

int x11_xres_pids(const uint8_t *r, size_t n, uint32_t *base, uint32_t *pid, int max) {
    if (!r || n < 32) return 0;
    uint32_t count = x11_get32(r + 8);
    if (count > (uint32_t)max) return -1;                   /* more clients than can be looked at */
    size_t off = 32;
    int k = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (off + 12 > n) break;
        uint32_t client = x11_get32(r + off), mask = x11_get32(r + off + 4), len = x11_get32(r + off + 8);
        off += 12;
        if (len % 4 || len > n - off) break;
        if (mask == 2 /* LocalClientPID */ && len == 4) { base[k] = client; pid[k] = x11_get32(r + off); k++; }
        off += len;
    }
    return k;
}

static uint32_t xres_pid_of(uint32_t win, const uint32_t *base, const uint32_t *pid, int n, uint32_t id_mask) {
    for (int i = 0; win && i < n; i++)
        if (x11_same_client(win, base[i], id_mask)) return pid[i] && pid[i] <= 0x7fffffffu ? pid[i] : 0;
    return 0;
}

int x11_same_process(uint32_t a, uint32_t b, const uint32_t *base, const uint32_t *pid, int n, uint32_t id_mask) {
    if (n < 0) return 0;
    uint32_t pa = xres_pid_of(a, base, pid, n, id_mask);
    return pa && pa == xres_pid_of(b, base, pid, n, id_mask);
}

int x11_xdnd_trusted(uint32_t owner, const uint32_t *base, const uint32_t *pid, int n, uint32_t id_mask,
                     int (*sandboxed)(int pid)) {
    int known = 0;
    if (!owner || n < 0) return 0;
    for (int i = 0; i < n; i++) {
        if (!pid[i] || pid[i] > 0x7fffffffu) continue;
        if (sandboxed((int)pid[i])) return 0;
        if (x11_same_client(owner, base[i], id_mask)) known = 1;
    }
    return known;
}

void x11_xdnd_owner(XdndOwner *o, uint32_t win, const uint32_t *base, const uint32_t *pid, int n, uint32_t id_mask,
                    int (*sandboxed)(int pid)) {
    o->win = win;
    o->pid = 0;
    o->trusted = x11_xdnd_trusted(win, base, pid, n, id_mask, sandboxed);
    for (int i = 0; win && i < n; i++)
        if (pid[i] && pid[i] <= 0x7fffffffu && x11_same_client(win, base[i], id_mask)) { o->pid = pid[i]; break; }
}

int x11_xdnd_same_owner(const XdndOwner *then, uint32_t cur, const XdndOwner *now) {
    if (!then->win || cur != then->win) return 0;
    if (!now) return 1;
    return now->win == then->win && now->pid == then->pid && now->trusted == then->trusted;
}

int x11_xdnd_notify(const uint8_t ev[32], uint32_t requestor, uint32_t selection, uint32_t target, uint32_t prop) {
    if (ev[0] != X_SelectionNotify) return -1;              /* the 0x80 bit: sent by a client (SendEvent) */
    if (x11_get32(ev + 8) != requestor || x11_get32(ev + 12) != selection || x11_get32(ev + 16) != target) return -1;
    uint32_t p = x11_get32(ev + 20);
    return p == prop ? 1 : p == 0 ? 0 : -1;
}
