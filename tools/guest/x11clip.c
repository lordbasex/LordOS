/* x11clip.c — the X CLIPBOARD's owner for lordos-guest (see x11clip.h). The few requests it needs, written by hand:
 * InternAtom, CreateWindow (InputOnly), SetSelectionOwner, ChangeProperty, ChangeWindowAttributes (PropertyChange on a
 * reader's window, for INCR) and SendEvent (SelectionNotify). Everything the server sends is read in 32-byte units
 * (plus a reply's extra length) and checked against what's buffered before use. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "x11clip.h"
#include "../../server/src/vm/vmagent.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int xfd = -1;
static long long tried_at;
static uint32_t root, wid, max_req;     /* max_req: bytes one request may carry */
static uint16_t seq;
static uint8_t in[65536];
static size_t in_n;
static int owned;
static uint8_t own_forms;

enum { A_CLIPBOARD, A_TARGETS, A_UTF8, A_TEXTPLAIN, A_TEXT, A_STRING, A_HTML, A_PNG, A_INCR, A_ATOM, A_MULTIPLE, A_TIMESTAMP, A_INTEGER, A_N };
static const char *const ANAMES[A_N] = { "CLIPBOARD", "TARGETS", "UTF8_STRING", "text/plain;charset=utf-8", "TEXT", "STRING", "text/html",
                                         "image/png", "INCR", "ATOM", "MULTIPLE", "TIMESTAMP", "INTEGER" };
static uint32_t atom[A_N];

/* a request waiting for the host's data, and a transfer in pieces (INCR) */
typedef struct { uint32_t requestor, property, target, type, time; int form; long long at; } Req;
#define REQ_MAX 16
static Req reqs[REQ_MAX];
static int req_n;
typedef struct { uint32_t requestor, property, type; int form; size_t off; long long at; int done_sent; } Xfer;
#define XFER_MAX 8
static Xfer xfers[XFER_MAX];
static int xfer_n;
static struct { uint8_t *d; size_t n; int state; } data[4];    /* per AGF_* form: 0 not asked, 1 asked, 2 have, 3 refused */

static long long mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static size_t pad4(size_t n) { return (n + 3) & ~(size_t)3; }

static int send_all(const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w = send(xfd, b, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) { struct pollfd q = { xfd, POLLOUT, 0 }; if (poll(&q, 1, 2000) <= 0) return -1; continue; }
        if (w <= 0) return -1;
        b += w; n -= (size_t)w;
    }
    return 0;
}
/* a request: head (its length field filled here) and an optional tail of n bytes, padded to 4 */
static int request(uint8_t *head, size_t hn, const void *tail, size_t n) {
    static const uint8_t zero[4];
    w16(head + 2, (uint32_t)((hn + pad4(n)) / 4));
    seq++;
    if (send_all(head, hn) || (n && send_all(tail, n)) || (pad4(n) - n && send_all(zero, pad4(n) - n))) return -1;
    return 0;
}

/* the cookie for display number dpy from an Xauthority file: MIT-MAGIC-COOKIE-1, a local or wildcard entry of that
 * number (or of any number); 16 bytes, 0 found */
static int cookie(const char *dpy, uint8_t out[16]) {
    char path[512];
    const char *xa = getenv("XAUTHORITY"), *home = getenv("HOME");
    if (xa && *xa) snprintf(path, sizeof path, "%s", xa);
    else if (home && *home) snprintf(path, sizeof path, "%s/.Xauthority", home);
    else return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    static uint8_t b[65536];
    ssize_t n = read(fd, b, sizeof b);
    close(fd);
    int found = -1;
    for (size_t o = 0; n > 0 && o + 2 <= (size_t)n;) {
        uint16_t fam = be16(b + o);
        const uint8_t *f[4];
        uint16_t l[4];
        o += 2;
        int ok = 1;
        for (int i = 0; i < 4 && ok; i++) {
            if (o + 2 > (size_t)n) { ok = 0; break; }
            l[i] = be16(b + o); o += 2;
            if (o + l[i] > (size_t)n) { ok = 0; break; }
            f[i] = b + o; o += l[i];
        }
        if (!ok) break;
        int local = fam == 256 || fam == 65535;
        int num = !l[1] || (l[1] == strlen(dpy) && !memcmp(f[1], dpy, l[1]));
        if (local && num && l[2] == 18 && !memcmp(f[2], "MIT-MAGIC-COOKIE-1", 18) && l[3] == 16) {
            memcpy(out, f[3], 16);
            found = 0;
            if (l[1]) break;            /* an entry of this very display wins over a wildcard */
        }
    }
    return found;
}

/* waits for count bytes of the server's answer (setup, a reply) until end (CLOCK_MONOTONIC ms). The caller's deadline
 * covers all its reads: a display that keeps sending events and never the reply can't hold the helper longer */
static int wait_in(size_t count, long long end) {
    while (in_n < count) {
        if (count > sizeof in) return -1;
        struct pollfd q = { xfd, POLLIN, 0 };
        long long left = end - mono();
        if (left <= 0 || poll(&q, 1, (int)left) <= 0) return -1;
        ssize_t r = recv(xfd, in + in_n, sizeof in - in_n, 0);
        if (r <= 0) return -1;
        in_n += (size_t)r;
    }
    return 0;
}
static void consume(size_t k) { memmove(in, in + k, in_n - k); in_n -= k; }

static int xconnect(const char *disp, char num[16]) {
    const char *p = disp;
    if (!strncmp(p, "unix:", 5)) p += p[5] == '/' ? 5 : 4;     /* unix:/path is that path; unix:N is :N */
    if (*p == '/') {                    /* a socket's path (some launchers pass that) */
        const char *x = strrchr(p, 'X');
        snprintf(num, 16, "%.15s", x ? x + 1 : "0");
    } else {
        if (*p != ':') return -1;       /* a TCP display: never */
        size_t k = 0;
        for (p++; *p >= '0' && *p <= '9' && k < 15; p++) num[k++] = *p;
        num[k] = 0;
        if (!k) return -1;
    }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    char path[sizeof a.sun_path];
    if (*disp == '/' || !strncmp(disp, "unix:/", 6)) snprintf(path, sizeof path, "%s", disp + (*disp == '/' ? 0 : 5));
    else snprintf(path, sizeof path, "/tmp/.X11-unix/X%s", num);
    for (int abstract = 0; abstract < 2; abstract++) {
        int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s < 0) return -1;
        memset(a.sun_path, 0, sizeof a.sun_path);
        size_t l = strlen(path);
        if (l + 1 >= sizeof a.sun_path) { close(s); return -1; }
        memcpy(a.sun_path + abstract, path, l);
        socklen_t al = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + abstract + l + !abstract);
        if (!connect(s, (struct sockaddr *)&a, al)) return s;
        close(s);
    }
    return -1;
}

static uint32_t intern(const char *name) {
    uint8_t h[8] = { 16, 0 };
    size_t n = strlen(name);
    w16(h + 4, (uint32_t)n);
    uint16_t want = (uint16_t)(seq + 1);
    if (request(h, 8, name, n)) return 0;
    /* replies and events in order: only this reply matters during setup, and it comes within 2 s */
    long long end = mono() + 2000;
    for (;;) {
        if (wait_in(32, end)) return 0;
        uint8_t t = in[0];
        size_t len = 32 + (t == 1 ? (size_t)r32(in + 4) * 4 : 0);
        if (wait_in(len, end)) return 0;
        uint32_t v = r32(in + 8);
        int mine = (t == 1 || t == 0) && r16(in + 2) == want;
        consume(len);
        if (mine) return t == 1 ? v : 0;
    }
}

void xc_close(void) {
    if (xfd >= 0) close(xfd);
    xfd = -1;
    owned = 0;
    in_n = 0;
    req_n = xfer_n = 0;
    for (int i = 0; i < 4; i++) { free(data[i].d); data[i].d = NULL; data[i].n = 0; data[i].state = 0; }
}

int xc_fd(void) { return xfd; }
int xc_owned(void) { return xfd >= 0 && owned; }

int xc_open(void) {
    if (xfd >= 0) return 0;
    const char *disp = getenv("DISPLAY");
    if (!disp || !*disp) return -1;
    long long now = mono();
    if (tried_at && now - tried_at < 10000) return -1;
    tried_at = now;
    char num[16];
    xfd = xconnect(disp, num);
    if (xfd < 0) return -1;
    uint8_t ck[16];
    int auth = !cookie(num, ck);
    uint8_t h[12] = { 'l', 0, 11, 0, 0, 0 };
    w16(h + 6, auth ? 18 : 0);
    w16(h + 8, auth ? 16 : 0);
    in_n = 0;
    uint8_t name[20] = "MIT-MAGIC-COOKIE-1";
    long long end = mono() + 2000;
    if (send_all(h, 12) || (auth && (send_all(name, 20) || send_all(ck, 16))) || wait_in(8, end)) { xc_close(); return -1; }
    size_t more = (size_t)r16(in + 6) * 4;
    if (in[0] != 1 || wait_in(8 + more, end) || more < 32) { fprintf(stderr, "lordos-guest: the X display refused the connection\n"); xc_close(); return -1; }
    const uint8_t *s = in + 8;
    uint32_t base = r32(s + 4), mask = r32(s + 8);
    uint16_t vlen = r16(s + 16), mreq = r16(s + 18);
    uint8_t nform = s[21];
    size_t scr = 32 + pad4(vlen) + (size_t)nform * 8;
    if (scr + 4 > more || !mask) { xc_close(); return -1; }
    root = r32(s + scr);
    max_req = (uint32_t)mreq * 4;
    consume(8 + more);
    wid = base | (mask & (uint32_t)-mask);      /* the lowest id of ours */
    for (int i = 0; i < A_N; i++) if (!(atom[i] = intern(ANAMES[i]))) { xc_close(); return -1; }
    /* an InputOnly window of our own, never mapped: the selection's owner */
    uint8_t cw[32] = { 1, 0 };
    w32(cw + 4, wid); w32(cw + 8, root);
    w16(cw + 12, 0); w16(cw + 14, 0); w16(cw + 16, 1); w16(cw + 18, 1); w16(cw + 20, 0); w16(cw + 22, 2 /* InputOnly */);
    w32(cw + 24, 0); w32(cw + 28, 0);
    if (request(cw, 32, NULL, 0)) { xc_close(); return -1; }
    fprintf(stderr, "lordos-guest: the host's clipboard on demand through the X display %s\n", disp);
    return 0;
}

static void notify(uint32_t requestor, uint32_t target, uint32_t property, uint32_t time) {
    uint8_t r[44] = { 25, 0 };
    w32(r + 4, requestor);
    w32(r + 8, 0);
    uint8_t *e = r + 12;
    e[0] = 31;                          /* SelectionNotify */
    w32(e + 4, time); w32(e + 8, requestor); w32(e + 12, atom[A_CLIPBOARD]); w32(e + 16, target); w32(e + 20, property);
    request(r, 44, NULL, 0);
}

static int change(uint32_t w, uint32_t prop, uint32_t type, int format, const void *d, size_t bytes) {
    uint8_t h[24] = { 18, 0 /* Replace */ };
    w32(h + 4, w); w32(h + 8, prop); w32(h + 12, type);
    h[16] = (uint8_t)format;
    w32(h + 20, (uint32_t)(bytes / (size_t)(format / 8)));
    return request(h, 24, d, bytes);
}

/* the most one ChangeProperty carries here (a request may be max_req bytes; pieces of 64 KB keep it simple) */
static size_t chunk(void) { size_t m = max_req > 1024 ? max_req - 64 : 65536; return m < 65536 ? m : 65536; }

static void answer(const Req *q) {
    int f = q->form;
    if (data[f].state != 2) { notify(q->requestor, q->target, 0, q->time); return; }
    if (data[f].n <= chunk()) {
        change(q->requestor, q->property, q->type, 8, data[f].d, data[f].n);
        notify(q->requestor, q->target, q->property, q->time);
        return;
    }
    /* INCR: its size first, then the pieces each time the reader deletes the property */
    if (xfer_n == XFER_MAX) { notify(q->requestor, q->target, 0, q->time); return; }
    uint8_t ca[12] = { 2, 0 };
    w32(ca + 4, q->requestor); w32(ca + 8, 0x800 /* event-mask */);
    uint32_t mask = 0x400000;           /* PropertyChange */
    request(ca, 12, &mask, 4);
    uint32_t size = (uint32_t)data[f].n;
    change(q->requestor, q->property, atom[A_INCR], 32, &size, 4);
    notify(q->requestor, q->target, q->property, q->time);
    xfers[xfer_n++] = (Xfer){ q->requestor, q->property, q->type, f, 0, mono(), 0 };
}

static void xfer_next(int i) {
    Xfer *x = &xfers[i];
    int f = x->form;
    size_t left = data[f].state == 2 && x->off < data[f].n ? data[f].n - x->off : 0, k = left < chunk() ? left : chunk();
    change(x->requestor, x->property, x->type, 8, k ? data[f].d + x->off : NULL, k);
    x->off += k;
    x->at = mono();
    if (!k) xfers[i] = xfers[--xfer_n];        /* the empty piece: done */
}

static int form_of(uint32_t target, uint32_t *type) {
    if (target == atom[A_UTF8] || target == atom[A_TEXTPLAIN] || target == atom[A_TEXT] || target == atom[A_STRING]) {
        *type = target == atom[A_TEXT] ? atom[A_UTF8] : target;
        return (own_forms & AGM_TEXT) ? AGF_TEXT : 0;
    }
    *type = target;
    if (target == atom[A_HTML]) return (own_forms & AGM_HTML) ? AGF_HTML : 0;
    if (target == atom[A_PNG]) return (own_forms & AGM_PNG) ? AGF_PNG : 0;
    return 0;
}

static void on_request(const uint8_t *e) {
    uint32_t time = r32(e + 4), owner = r32(e + 8), requestor = r32(e + 12), sel = r32(e + 16), target = r32(e + 20), prop = r32(e + 24);
    if (!prop) prop = target;           /* an obsolete reader */
    if (!owned || owner != wid || sel != atom[A_CLIPBOARD]) { notify(requestor, target, 0, time); return; }
    if (target == atom[A_TARGETS]) {
        uint32_t t[8];
        int n = 0;
        t[n++] = atom[A_TARGETS]; t[n++] = atom[A_TIMESTAMP];
        if (own_forms & AGM_TEXT) { t[n++] = atom[A_UTF8]; t[n++] = atom[A_TEXTPLAIN]; t[n++] = atom[A_TEXT]; }
        if (own_forms & AGM_HTML) t[n++] = atom[A_HTML];
        if (own_forms & AGM_PNG) t[n++] = atom[A_PNG];
        change(requestor, prop, atom[A_ATOM], 32, t, (size_t)n * 4);
        notify(requestor, target, prop, time);
        return;
    }
    if (target == atom[A_TIMESTAMP]) { uint32_t z = 0; change(requestor, prop, atom[A_INTEGER], 32, &z, 4); notify(requestor, target, prop, time); return; }
    uint32_t type;
    int f = form_of(target, &type);
    if (!f) { notify(requestor, target, 0, time); return; }       /* MULTIPLE, a form not offered, anything else */
    Req q = { requestor, prop, target, type, time, f, mono() };
    if (data[f].state >= 2) { answer(&q); return; }
    if (req_n == REQ_MAX) { notify(requestor, target, 0, time); return; }
    reqs[req_n++] = q;
}

int xc_own(uint8_t forms) {
    if (xc_open()) return -1;
    xc_supply(AGF_TEXT, NULL, 0); xc_supply(AGF_HTML, NULL, 0); xc_supply(AGF_PNG, NULL, 0);    /* the last offer's readers: none */
    for (int i = 0; i < 4; i++) { free(data[i].d); data[i].d = NULL; data[i].n = 0; data[i].state = 0; }
    xfer_n = 0;
    own_forms = forms;
    uint8_t r[16] = { 22, 0 };
    w32(r + 4, wid); w32(r + 8, atom[A_CLIPBOARD]); w32(r + 12, 0 /* CurrentTime */);
    if (request(r, 16, NULL, 0)) { xc_close(); return -1; }
    owned = 1;
    return 0;
}

int xc_read(void) {
    if (xfd < 0) return -1;
    ssize_t r = recv(xfd, in + in_n, sizeof in - in_n, MSG_DONTWAIT);
    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) { fprintf(stderr, "lordos-guest: the X display went away\n"); xc_close(); return -1; }
    if (r > 0) in_n += (size_t)r;
    while (in_n >= 32) {
        size_t len = 32 + (in[0] == 1 ? (size_t)r32(in + 4) * 4 : 0);
        if (len > sizeof in) { xc_close(); return -1; }
        if (in_n < len) break;
        uint8_t code = in[0] & 0x7f;
        if (code == 0) {                /* an error: a reader's window that went (BadWindow) ends its transfer */
            uint32_t bad = r32(in + 4);
            for (int i = 0; i < xfer_n; i++) if (xfers[i].requestor == bad) xfers[i--] = xfers[--xfer_n];
        } else if (code == 29) {        /* SelectionClear: another program copied */
            if (r32(in + 8) == wid && r32(in + 12) == atom[A_CLIPBOARD]) {
                owned = 0;
                xc_supply(AGF_TEXT, NULL, 0); xc_supply(AGF_HTML, NULL, 0); xc_supply(AGF_PNG, NULL, 0);
            }
        } else if (code == 30) on_request(in);
        else if (code == 28 && in[16] == 1) {     /* PropertyNotify, deleted: the reader took a piece */
            uint32_t w = r32(in + 4), a = r32(in + 8);
            for (int i = 0; i < xfer_n; i++) if (xfers[i].requestor == w && xfers[i].property == a) { xfer_next(i); break; }
        }
        consume(len);
    }
    return xfd >= 0 ? 0 : -1;
}

int xc_wanted(void) {
    for (int i = 0; i < req_n; i++) {
        int f = reqs[i].form;
        if (data[f].state == 0) { data[f].state = 1; return f; }
    }
    return 0;
}

void xc_supply(int form, const uint8_t *d, size_t n) {
    if (form < AGF_TEXT || form > AGF_PNG) return;
    if (xfd >= 0 && owned && d && n && data[form].state != 2) {
        uint8_t *c = malloc(n);
        if (c) { memcpy(c, d, n); free(data[form].d); data[form].d = c; data[form].n = n; data[form].state = 2; }
        else data[form].state = 3;
    } else if (data[form].state != 2) data[form].state = !d && xfd >= 0 && owned ? 3 : 0;
    for (int i = 0; i < req_n; i++) {
        if (reqs[i].form != form) continue;
        Req q = reqs[i];
        reqs[i--] = reqs[--req_n];
        if (xfd >= 0) answer(&q);
    }
}

void xc_expire(long long now, long long wait) {
    if (xfd < 0) return;
    for (int f = AGF_TEXT; f <= AGF_PNG; f++)
        for (int i = 0; i < req_n; i++) if (reqs[i].form == f && now - reqs[i].at > wait) { xc_supply(f, NULL, 0); break; }
    for (int i = 0; i < xfer_n; i++) if (now - xfers[i].at > 10000) xfers[i--] = xfers[--xfer_n];
}
