/* dbusmin.c — lordos-guest's own D-Bus client, just enough for the performance profile (dbusmin.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dbusmin.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ---- writing ---- */

void dbw_init(DbW *w, uint8_t *p, size_t cap) { w->p = p; w->n = 0; w->cap = cap; w->bad = 0; }
static void w_byte(DbW *w, uint8_t b) { if (w->n >= w->cap) { w->bad = 1; return; } w->p[w->n++] = b; }
static void w_pad(DbW *w, size_t a) { while (!w->bad && w->n % a) w_byte(w, 0); }
static void w_u32(DbW *w, uint32_t v) { w_pad(w, 4); for (int i = 0; i < 4; i++) w_byte(w, (uint8_t)(v >> (8 * i))); }
static void w_mem(DbW *w, const void *p, size_t n) {
    if (!n) return;
    if (w->bad || n > w->cap - w->n) { w->bad = 1; return; }
    memcpy(w->p + w->n, p, n);
    w->n += n;
}
/* only printable ASCII goes out (every string this side sends is one of its constants) */
static int ascii(const char *s, size_t max) {
    size_t l = strlen(s);
    if (l > max) return 0;
    for (size_t i = 0; i < l; i++) if (s[i] < 0x20 || s[i] > 0x7e) return 0;
    return 1;
}
void dbw_str(DbW *w, const char *s) {
    if (!s || !ascii(s, 4096)) { w->bad = 1; return; }
    size_t l = strlen(s);
    w_u32(w, (uint32_t)l);
    w_mem(w, s, l + 1);
}
void dbw_sig(DbW *w, const char *s) {
    if (!s || !ascii(s, 255)) { w->bad = 1; return; }
    size_t l = strlen(s);
    w_byte(w, (uint8_t)l);
    w_mem(w, s, l + 1);
}
void dbw_bytes(DbW *w, const uint8_t *p, size_t n) {
    if (n > DB_MSG_MAX) { w->bad = 1; return; }
    w_u32(w, (uint32_t)n);
    w_mem(w, p, n);
}
void dbw_variant_str(DbW *w, const char *s) { dbw_sig(w, "s"); dbw_str(w, s); }

/* names as D-Bus takes them: a path ("/a/b_c"), a member ("Get"), a bus or interface name ("a.b-c.D") */
static int name_ok(const char *s, int kind) {
    size_t l = s ? strlen(s) : 0;
    if (!l || l > 255) return 0;
    if (kind == 'o') {
        if (s[0] != '/' || (l > 1 && s[l - 1] == '/') || strstr(s, "//")) return 0;
        for (size_t i = 0; i < l; i++) { char c = s[i]; if (!(c == '/' || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return 0; }
        return 1;
    }
    if (s[0] >= '0' && s[0] <= '9') return 0;
    for (size_t i = 0; i < l; i++) {
        char c = s[i];
        int al = c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (kind == 'm' ? !al : !(al || c == '.' || (kind == 'd' && c == '-'))) return 0;
    }
    if (kind != 'm' && (!strchr(s, '.') || s[0] == '.' || s[l - 1] == '.' || strstr(s, ".."))) return 0;
    return 1;
}
static int sig_ok(const char *s) {
    if (!s || strlen(s) > 32) return 0;
    for (; *s; s++) if (!strchr("ybnqiuxtdsogav(){}", *s)) return 0;
    return 1;
}

static void field(DbW *w, uint8_t code, char type, const char *v) {
    w_pad(w, 8);
    w_byte(w, code); w_byte(w, 1); w_byte(w, (uint8_t)type); w_byte(w, 0);
    if (type == 'g') dbw_sig(w, v);
    else dbw_str(w, v);
}

size_t db_call_build(uint8_t *out, size_t cap, uint32_t serial, const char *dest, const char *path, const char *iface,
                     const char *member, const char *sig, const uint8_t *body, size_t body_n) {
    if (!serial || !name_ok(path, 'o') || !name_ok(member, 'm') || (dest && !name_ok(dest, 'd')) || (iface && !name_ok(iface, 'i')) ||
        (sig && !sig_ok(sig)) || (body_n && (!sig || !*sig || !body)) || body_n > DB_MSG_MAX) return 0;
    DbW w;
    dbw_init(&w, out, cap);
    w_byte(&w, 'l'); w_byte(&w, DB_CALL); w_byte(&w, 0); w_byte(&w, 1);
    w_u32(&w, (uint32_t)body_n);
    w_u32(&w, serial);
    w_u32(&w, 0);                       /* the header fields' length, below */
    field(&w, 1, 'o', path);
    if (iface) field(&w, 2, 's', iface);
    field(&w, 3, 's', member);
    if (dest) field(&w, 6, 's', dest);
    if (sig && *sig) field(&w, 8, 'g', sig);
    if (w.bad) return 0;
    uint32_t fl = (uint32_t)(w.n - 16);
    for (int i = 0; i < 4; i++) out[12 + i] = (uint8_t)(fl >> (8 * i));
    w_pad(&w, 8);
    w_mem(&w, body, body_n);
    return w.bad ? 0 : w.n;
}

/* ---- reading ---- */

static uint32_t rd32(const uint8_t *p, int big) {
    return big ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
               : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}
/* moves *o to a multiple of a, over zero bytes only, not past end */
static int skip_pad(const uint8_t *p, size_t *o, size_t a, size_t end) {
    size_t t = (*o + a - 1) & ~(a - 1);
    if (t > end) return -1;
    for (size_t i = *o; i < t; i++) if (p[i]) return -1;
    *o = t;
    return 0;
}
/* a string (s, o) or a signature (g) at *o: its start and length; NUL-terminated, no NUL inside */
static int rd_text(const uint8_t *p, size_t *o, size_t end, int sig, int big, const char **s, uint32_t *len) {
    uint32_t l;
    if (sig) { if (*o + 1 > end) return -1; l = p[*o]; *o += 1; }
    else { if (skip_pad(p, o, 4, end) || *o + 4 > end) return -1; l = rd32(p + *o, big); *o += 4; }
    if (l >= end - *o || p[*o + l] || memchr(p + *o, 0, l)) return -1;
    *s = (const char *)p + *o;
    *len = l;
    *o += l + 1;
    return 0;
}

int db_msg_parse(const uint8_t *p, size_t n, DbMsg *m) {
    memset(m, 0, sizeof *m);
    if (!p) return -1;
    if (n < 16) return 0;
    int big = p[0] == 'B';
    if ((p[0] != 'l' && !big) || p[1] < DB_CALL || p[1] > DB_SIGNAL || p[3] != 1) return -1;
    uint32_t blen = rd32(p + 4, big), serial = rd32(p + 8, big), flen = rd32(p + 12, big);
    if (!serial || flen > DB_MSG_MAX || blen > DB_MSG_MAX) return -1;
    size_t hend = 16 + (size_t)flen, bstart = (hend + 7) & ~(size_t)7, total = bstart + blen;
    if (total > DB_MSG_MAX) return -1;
    if (n < total) return 0;
    size_t o = 16;
    int have_sig = 0;
    while (o < hend) {
        if (skip_pad(p, &o, 8, hend) || o + 4 > hend) return -1;
        uint8_t code = p[o];
        char t = (char)p[o + 2];
        if (!code || p[o + 1] != 1 || p[o + 3]) return -1;
        o += 4;
        const char *s = NULL;
        uint32_t l = 0, v = 0;
        switch (t) {
        case 's': case 'o': if (rd_text(p, &o, hend, 0, big, &s, &l)) return -1; break;
        case 'g': if (rd_text(p, &o, hend, 1, big, &s, &l)) return -1; break;
        case 'u': case 'i': case 'b': if (skip_pad(p, &o, 4, hend) || o + 4 > hend) return -1; v = rd32(p + o, big); o += 4; break;
        case 'y': if (o + 1 > hend) return -1; o += 1; break;
        case 'n': case 'q': if (skip_pad(p, &o, 2, hend) || o + 2 > hend) return -1; o += 2; break;
        case 'x': case 't': case 'd': if (skip_pad(p, &o, 8, hend) || o + 8 > hend) return -1; o += 8; break;
        default: return -1;
        }
        if ((code == 4 && t != 's') || (code == 5 && t != 'u') || (code == 8 && t != 'g')) return -1;
        if (code == 4) { if (l >= sizeof m->err) return -1; memcpy(m->err, s, l); m->err[l] = 0; }
        if (code == 5) { if (!v) return -1; m->reply_serial = v; }
        if (code == 8) { if (l >= sizeof m->sig || !sig_ok(s)) return -1; memcpy(m->sig, s, l); m->sig[l] = 0; have_sig = 1; }
    }
    if (o != hend) return -1;
    for (size_t i = hend; i < bstart; i++) if (p[i]) return -1;
    if (blen && !have_sig) return -1;
    if ((p[1] == DB_RETURN || p[1] == DB_ERROR) && !m->reply_serial) return -1;
    if (p[1] == DB_ERROR && !m->err[0]) return -1;
    m->type = p[1];
    m->big = big;
    m->serial = serial;
    m->body = p + bstart;
    m->body_n = blen;
    return (int)total;
}

/* the reply's one variant, nested variants unwrapped, holding the type `want`: *o at its value */
static int body_variant(const DbMsg *m, char want, size_t *o) {
    if (strcmp(m->sig, "v") || !m->body) return -1;
    *o = 0;
    for (int depth = 0; depth < 4; depth++) {
        const char *s;
        uint32_t l;
        if (rd_text(m->body, o, m->body_n, 1, m->big, &s, &l) || l != 1) return -1;
        if (s[0] == want) return 0;
        if (s[0] != 'v') return -1;
    }
    return -1;
}
int db_reply_str(const DbMsg *m, char *out, size_t cap) {
    size_t o;
    const char *s;
    uint32_t l;
    if (body_variant(m, 's', &o) || rd_text(m->body, &o, m->body_n, 0, m->big, &s, &l) || l >= cap) return -1;
    memcpy(out, s, l);
    out[l] = 0;
    return 0;
}
int db_reply_bool(const DbMsg *m, int *b) {
    size_t o;
    if (body_variant(m, 'b', &o) || skip_pad(m->body, &o, 4, m->body_n) || o + 4 > m->body_n) return -1;
    uint32_t v = rd32(m->body + o, m->big);
    if (v > 1) return -1;
    *b = (int)v;
    return 0;
}

/* ---- dconf ---- */

size_t db_dconf_bool(uint8_t *out, size_t cap, const char *key, int value) {
    size_t k = key ? strlen(key) : 0;
    if (k < 2 || key[0] != '/' || key[k - 1] == '/' || strstr(key, "//") || !ascii(key, 200) || strchr(key, ' ')) return 0;
    /* a{smv}, one entry: the dict entry {smv} is the key (s, NUL-terminated), padding to 8, then Just a variant of b
     * (the boolean's byte, the variant's 0 and type "b", the maybe's 0), then the struct's one framing offset (the key's
     * end); the array has the entry's end as its one framing offset. Offsets are one byte: under 256 bytes in all */
    size_t a = (k + 1 + 7) & ~(size_t)7, body = a + 4, entry = body + 1, all = entry + 1;
    if (all > 255 || all > cap) return 0;
    memset(out, 0, all);
    memcpy(out, key, k);
    out[a] = value ? 1 : 0;
    out[a + 2] = 'b';
    out[body] = (uint8_t)(k + 1);
    out[entry] = (uint8_t)entry;
    return all;
}

/* ---- the connection ---- */

static long long mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int wait_fd(int fd, short ev, long long deadline) {
    for (;;) {
        long long left = deadline - mono_ms();
        if (left <= 0) return -1;
        struct pollfd p = { fd, ev, 0 };
        int r = poll(&p, 1, (int)left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        if (p.revents & (POLLERR | POLLNVAL)) return -1;
        return 0;
    }
}
static int write_all(int fd, const void *p, size_t n, long long deadline) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) { if (wait_fd(fd, POLLOUT, deadline)) return -1; continue; }
        if (w <= 0) return -1;
        b += w; n -= (size_t)w;
    }
    return 0;
}
static int read_more(Db *b, long long deadline) {
    if (b->in_n >= sizeof b->in) return -1;
    for (;;) {
        ssize_t r = recv(b->fd, b->in + b->in_n, sizeof b->in - b->in_n, 0);
        if (r > 0) { b->in_n += (size_t)r; return 0; }
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) { if (wait_fd(b->fd, POLLIN, deadline)) return -1; continue; }
        return -1;
    }
}

void db_close(Db *b) {
    if (b->fd >= 0) close(b->fd);
    b->fd = -1;
    b->in_n = b->used = 0;
}

int db_call(Db *b, const char *dest, const char *path, const char *iface, const char *member, const char *sig,
            const uint8_t *body, size_t body_n, DbMsg *reply, int timeout_ms) {
    memset(reply, 0, sizeof *reply);
    if (b->fd < 0) return -1;
    /* the last call's reply goes now */
    if (b->used) { memmove(b->in, b->in + b->used, b->in_n - b->used); b->in_n -= b->used; b->used = 0; }
    static uint8_t msg[8192];
    uint32_t serial = ++b->serial;
    size_t n = db_call_build(msg, sizeof msg, serial, dest, path, iface, member, sig, body, body_n);
    long long deadline = mono_ms() + timeout_ms;
    if (!n || write_all(b->fd, msg, n, deadline)) return -1;
    for (;;) {
        DbMsg m;
        int used = db_msg_parse(b->in, b->in_n, &m);
        if (used < 0) return -1;
        if (!used) { if (read_more(b, deadline)) return -1; continue; }
        if ((m.type == DB_RETURN || m.type == DB_ERROR) && m.reply_serial == serial) {
            *reply = m;
            b->used = (size_t)used;
            return m.type == DB_RETURN ? 0 : 1;
        }
        /* a signal (NameAcquired after Hello), or an answer to nothing of ours: skipped */
        memmove(b->in, b->in + used, b->in_n - (size_t)used);
        b->in_n -= (size_t)used;
    }
}

int db_open(Db *b, const char *path, int timeout_ms) {
    b->fd = -1; b->serial = 0; b->in_n = b->used = 0;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (!path || path[0] != '/' || strlen(path) >= sizeof a.sun_path) return -1;
    memcpy(a.sun_path, path, strlen(path) + 1);
    b->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (b->fd < 0) return -1;
    long long deadline = mono_ms() + timeout_ms;
    /* a local socket's connect doesn't wait (unless its queue is full); then every read and write has the deadline */
    if (connect(b->fd, (struct sockaddr *)&a, sizeof a) || fcntl(b->fd, F_SETFL, O_NONBLOCK)) { db_close(b); return -1; }
    char auth[64], uid[16];
    int ul = snprintf(uid, sizeof uid, "%u", (unsigned)getuid());
    int al = snprintf(auth, sizeof auth, "%cAUTH EXTERNAL ", 0);
    for (int i = 0; i < ul; i++) al += snprintf(auth + al, sizeof auth - (size_t)al, "%02x", (unsigned char)uid[i]);
    al += snprintf(auth + al, sizeof auth - (size_t)al, "\r\n");
    if (write_all(b->fd, auth, (size_t)al, deadline)) { db_close(b); return -1; }
    /* the server's answer, one line: "OK <guid>" */
    for (;;) {
        if (read_more(b, deadline) || b->in_n > 512) { db_close(b); return -1; }
        uint8_t *e = memmem(b->in, b->in_n, "\r\n", 2);
        if (!e) continue;
        int ok = b->in_n >= 3 && !memcmp(b->in, "OK ", 3) && (size_t)(e - b->in) + 2 == b->in_n;
        b->in_n = 0;
        if (!ok || write_all(b->fd, "BEGIN\r\n", 7, deadline)) { db_close(b); return -1; }
        break;
    }
    DbMsg r;
    long long left = deadline - mono_ms();
    if (left <= 0 || db_call(b, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", NULL, NULL, 0, &r, (int)left)) {
        db_close(b);
        return -1;
    }
    return 0;
}
