/* wire.c — see wire.h. */
#include "wire.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif

int doc_memfd_new(const char *name, size_t len) {
    int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)len)) { close(fd); return -1; }
    return fd;
}

int doc_seal(int fd) { return fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL); }

int doc_memfd(const char *name, const void *data, size_t len) {
    int fd = doc_memfd_new(name, len);
    if (fd < 0) return -1;
    const char *p = data;
    size_t off = 0;
    while (off < len) {
        ssize_t w = pwrite(fd, p + off, len - off, (off_t)off);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; close(fd); return -1; }
        off += (size_t)w;
    }
    if (doc_seal(fd)) { close(fd); return -1; }
    return fd;
}

long doc_check_sealed(int fd, size_t min) {
    int seals = fcntl(fd, F_GET_SEALS);
    if (seals < 0 || !(seals & F_SEAL_SHRINK)) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (size_t)st.st_size < min) return -1;
    return (long)st.st_size;
}

int doc_send(int sock, uint32_t op, uint32_t id, const char *json, size_t len, const int *fds, int nfds) {
    if (nfds < 0 || nfds > DOC_MAX_FDS - 1) { errno = EINVAL; return -1; }
    int all[DOC_MAX_FDS];
    int n = 0, jfd = -1;
    DocHdr h = { op, id, 0, 0 };
    if (json && len > DOC_INLINE_MAX) {
        jfd = doc_memfd("lord-doc-json", json, len);
        if (jfd < 0) return -1;
        all[n++] = jfd;
        h.flags |= DOCF_JSON_FD;
    } else if (json) h.len = (uint32_t)len;
    for (int i = 0; i < nfds; i++) all[n++] = fds[i];
    struct iovec iov[2] = { { &h, sizeof h }, { (void *)(json && !(h.flags & DOCF_JSON_FD) ? json : ""), h.len } };
    union { char buf[CMSG_SPACE(sizeof(int) * DOC_MAX_FDS)]; struct cmsghdr align; } u;
    struct msghdr msg = { .msg_iov = iov, .msg_iovlen = 2 };
    if (n) {
        memset(&u, 0, sizeof u);
        msg.msg_control = u.buf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)n);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)n);
        memcpy(CMSG_DATA(c), all, sizeof(int) * (size_t)n);
    }
    ssize_t r;
    do r = sendmsg(sock, &msg, MSG_NOSIGNAL); while (r < 0 && errno == EINTR);
    if (jfd >= 0) close(jfd);
    return r < 0 ? -1 : 0;
}

void doc_msg_free(DocMsg *m) {
    free(m->json);
    for (int i = 0; i < m->nfds; i++) if (m->fds[i] >= 0) close(m->fds[i]);
    memset(m, 0, sizeof *m);
}

int doc_recv(int sock, DocMsg *m) {
    memset(m, 0, sizeof *m);
    char *buf = malloc(sizeof(DocHdr) + DOC_INLINE_MAX + 1);
    if (!buf) return -1;
    union { char buf[CMSG_SPACE(sizeof(int) * DOC_MAX_FDS)]; struct cmsghdr align; } u;
    struct iovec iov = { buf, sizeof(DocHdr) + DOC_INLINE_MAX };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.buf, .msg_controllen = sizeof u.buf };
    ssize_t r;
    do r = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC); while (r < 0 && errno == EINTR);
    /* fds first: if the message is invalid they still have to be closed. A failed recvmsg leaves msg_controllen as
     * it was, over a buffer nothing wrote */
    for (struct cmsghdr *c = r < 0 ? NULL : CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        int tmp[DOC_MAX_FDS];
        if (k > DOC_MAX_FDS) k = DOC_MAX_FDS;
        memcpy(tmp, CMSG_DATA(c), sizeof(int) * (size_t)k);
        for (int i = 0; i < k; i++) { if (m->nfds < DOC_MAX_FDS) m->fds[m->nfds++] = tmp[i]; else close(tmp[i]); }
    }
    if (r == 0) { free(buf); doc_msg_free(m); return 1; }
    if (r < 0 || (size_t)r < sizeof(DocHdr) || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) { free(buf); doc_msg_free(m); return -1; }
    memcpy(&m->h, buf, sizeof m->h);
    size_t body = (size_t)r - sizeof(DocHdr);
    if (m->h.flags & DOCF_JSON_FD) {
        free(buf);
        if (m->nfds < 1) { doc_msg_free(m); return -1; }
        int jfd = m->fds[0];
        long sz = doc_check_sealed(jfd, 0);
        if (sz < 0 || sz > DOC_JSON_MAX) { doc_msg_free(m); return -1; }
        m->json = malloc((size_t)sz + 1);
        if (!m->json) { doc_msg_free(m); return -1; }
        size_t off = 0;
        while (off < (size_t)sz) {
            ssize_t g = pread(jfd, m->json + off, (size_t)sz - off, (off_t)off);
            if (g <= 0) { if (g < 0 && errno == EINTR) continue; doc_msg_free(m); return -1; }
            off += (size_t)g;
        }
        m->json[sz] = 0;
        m->len = (size_t)sz;
        close(jfd);
        memmove(m->fds, m->fds + 1, sizeof(int) * (size_t)(m->nfds - 1));
        m->nfds--;
        return 0;
    }
    if (m->h.len != body) { free(buf); doc_msg_free(m); return -1; }
    if (body) {
        memmove(buf, buf + sizeof(DocHdr), body);
        buf[body] = 0;
        m->json = buf;
        m->len = body;
    } else free(buf);
    return 0;
}

/* ---- JSON ---- */

void jb_init(JBuf *b) { memset(b, 0, sizeof *b); }
void jb_free(JBuf *b) { free(b->p); memset(b, 0, sizeof *b); }

static int jb_grow(JBuf *b, size_t extra) {
    if (b->oom) return -1;
    if (b->n + extra + 1 <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + extra + 1) cap *= 2;
    if (cap > DOC_JSON_MAX + 1024) { b->oom = 1; return -1; }
    char *p = realloc(b->p, cap);
    if (!p) { b->oom = 1; return -1; }
    b->p = p;
    b->cap = cap;
    return 0;
}

static void jb_put(JBuf *b, const char *s, size_t n) {
    if (jb_grow(b, n)) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

void jb_raw(JBuf *b, const char *s) { jb_put(b, s, strlen(s)); }
void jb_rawn(JBuf *b, const char *s, size_t n) { jb_put(b, s, n); }

void jb_printf(JBuf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    if ((size_t)k < sizeof tmp) { jb_put(b, tmp, (size_t)k); return; }
    if (jb_grow(b, (size_t)k)) return;
    va_start(ap, fmt);
    vsnprintf(b->p + b->n, (size_t)k + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)k;
}

void jb_num(JBuf *b, double v) {
    if (!isfinite(v)) v = 0;
    if (v == (double)(long long)v && fabs(v) < 1e15) jb_printf(b, "%lld", (long long)v);
    else jb_printf(b, "%.4f", v);
}

/* Length of a valid UTF-8 sequence in s (0 if it isn't one). */
static int utf8_len(const unsigned char *s, size_t n) {
    if (s[0] < 0x80) return 1;
    int k = s[0] >= 0xf0 && s[0] <= 0xf4 ? 4 : s[0] >= 0xe0 ? 3 : s[0] >= 0xc2 && s[0] < 0xe0 ? 2 : 0;
    if (!k || (size_t)k > n) return 0;
    for (int i = 1; i < k; i++) if ((s[i] & 0xc0) != 0x80) return 0;
    if (k == 3 && s[0] == 0xe0 && s[1] < 0xa0) return 0;
    if (k == 3 && s[0] == 0xed && s[1] >= 0xa0) return 0;          /* surrogates */
    if (k == 4 && s[0] == 0xf0 && s[1] < 0x90) return 0;
    if (k == 4 && s[0] == 0xf4 && s[1] >= 0x90) return 0;
    return k;
}

void jb_strn(JBuf *b, const char *str, size_t n) {
    const unsigned char *s = (const unsigned char *)str;
    jb_put(b, "\"", 1);
    for (size_t i = 0; i < n;) {
        unsigned char ch = s[i];
        if (ch == '"' || ch == '\\') { char e[2] = { '\\', (char)ch }; jb_put(b, e, 2); i++; continue; }
        if (ch < 0x20) {
            if (ch == '\n') jb_put(b, "\\n", 2);
            else if (ch == '\t') jb_put(b, "\\t", 2);
            else if (ch == '\r') jb_put(b, "\\r", 2);
            else if (!ch) jb_put(b, "\xef\xbf\xbd", 3);     /* NUL: U+FFFD (json_parse refuses an escaped NUL) */
            else jb_printf(b, "\\u%04x", ch);
            i++;
            continue;
        }
        int k = utf8_len(s + i, n - i);
        if (!k) { jb_put(b, "\xef\xbf\xbd", 3); i++; continue; }
        jb_put(b, (const char *)s + i, (size_t)k);
        i += (size_t)k;
    }
    jb_put(b, "\"", 1);
}

void jb_str(JBuf *b, const char *s) { jb_strn(b, s ? s : "", s ? strlen(s) : 0); }
