/* proto.c — sending and receiving messages between axia-server and axia-media (see proto.h). Both
 * binaries compile it. It validates lengths and descriptors: a broken or compromised helper can't
 * make axia-server read outside a buffer. */
#include "proto.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

int mp_send(int sock, uint32_t type, uint32_t id, const int64_t v[6], const void *payload, size_t len,
            const int *fds, int nfds) {
    if (sock < 0 || len > MP_PAYLOAD_MAX || nfds < 0 || nfds > MP_MAX_FDS) return -1;
    MediaMsg m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.id = id;
    if (v) memcpy(m.v, v, sizeof m.v);
    m.len = (uint32_t)len;
    struct iovec iov[2] = { { &m, sizeof m }, { (void *)payload, len } };
    struct msghdr h;
    memset(&h, 0, sizeof h);
    h.msg_iov = iov;
    h.msg_iovlen = len ? 2 : 1;
    union { char buf[CMSG_SPACE(sizeof(int) * MP_MAX_FDS)]; struct cmsghdr align; } u;
    int real[MP_MAX_FDS], n = 0;
    for (int i = 0; i < nfds; i++) if (fds[i] >= 0) real[n++] = fds[i];
    if (n) {
        memset(&u, 0, sizeof u);
        h.msg_control = u.buf;
        h.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)n);
        struct cmsghdr *c = CMSG_FIRSTHDR(&h);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)n);
        memcpy(CMSG_DATA(c), real, sizeof(int) * (size_t)n);
    }
    for (;;) {
        ssize_t r = sendmsg(sock, &h, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        return r == (ssize_t)(sizeof m + len) ? 0 : -1;
    }
}

int mp_sendv(int sock, uint32_t type, uint32_t id, int64_t a, int64_t b, int64_t c, int64_t d) {
    int64_t v[6] = { a, b, c, d, 0, 0 };
    return mp_send(sock, type, id, v, NULL, 0, NULL, 0);
}

int mp_recv(int sock, MediaMsg *m, char *payload, size_t cap, int fds[MP_MAX_FDS], int block) {
    for (int i = 0; i < MP_MAX_FDS; i++) fds[i] = -1;
    if (cap < MP_PAYLOAD_MAX + 1) return -1;
    struct iovec iov[2] = { { m, sizeof *m }, { payload, MP_PAYLOAD_MAX } };
    union { char buf[CMSG_SPACE(sizeof(int) * MP_MAX_FDS)]; struct cmsghdr align; } u;
    struct msghdr h;
    memset(&h, 0, sizeof h);
    h.msg_iov = iov;
    h.msg_iovlen = 2;
    h.msg_control = u.buf;
    h.msg_controllen = sizeof u.buf;
    ssize_t r;
    do r = recvmsg(sock, &h, (block ? 0 : MSG_DONTWAIT) | MSG_CMSG_CLOEXEC);
    while (r < 0 && errno == EINTR);
    if (r == 0) return 0;
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : 0;
    int nf = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        size_t k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        int *p = (int *)CMSG_DATA(c);
        for (size_t i = 0; i < k; i++) {
            if (nf < MP_MAX_FDS) fds[nf++] = p[i];
            else close(p[i]);
        }
    }
    int bad = (h.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || r < (ssize_t)sizeof *m ||
              m->len > MP_PAYLOAD_MAX || (size_t)r != sizeof *m + m->len;
    if (bad) {
        for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) { close(fds[i]); fds[i] = -1; }
        return -1;
    }
    payload[m->len] = 0;
    return 1;
}

int mp_param(const char *params, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    for (const char *p = params; p && *p;) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (ll > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t vl = ll - kl - 1;
            if (vl >= n) vl = n - 1;
            memcpy(out, p + kl + 1, vl);
            out[vl] = 0;
            return 0;
        }
        p = nl ? nl + 1 : NULL;
    }
    if (n) out[0] = 0;
    return -1;
}

long long mp_param_int(const char *params, const char *key, long long def) {
    char b[32];
    if (mp_param(params, key, b, sizeof b)) return def;
    char *end;
    long long v = strtoll(b, &end, 10);
    return end == b ? def : v;
}

double mp_param_num(const char *params, const char *key, double def) {
    char b[48];
    if (mp_param(params, key, b, sizeof b)) return def;
    char *end;
    double v = strtod(b, &end);
    return end == b ? def : v;
}
