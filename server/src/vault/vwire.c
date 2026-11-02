/* vwire.c — axia-vault protocol messages (see vwire.h). */
#include "vwire.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

const VwAlloc vw_malloc = { malloc, realloc, free };

int vw_send(int sock, uint32_t kind, uint32_t id, const char *json, size_t len, const int *fds, int nfds) {
    if (len > VW_MAX || nfds > VW_MAX_FDS || nfds < 0) { errno = EMSGSIZE; return -1; }
    size_t off = 0;
    int first = 1;
    do {
        size_t k = len - off > VW_CHUNK ? VW_CHUNK : len - off;
        VHdr h = { kind, id, (uint32_t)k, off + k < len ? VWF_MORE : 0 };
        struct iovec iov[2] = { { &h, sizeof h }, { (void *)(json ? json + off : ""), k } };
        union { struct cmsghdr c; char b[CMSG_SPACE(sizeof(int) * VW_MAX_FDS)]; } u;
        struct msghdr mh = { .msg_iov = iov, .msg_iovlen = k ? 2 : 1 };
        if (first && nfds) {
            memset(&u, 0, sizeof u);
            mh.msg_control = u.b;
            mh.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
            struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
            memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);
        }
        ssize_t r;
        do r = sendmsg(sock, &mh, MSG_NOSIGNAL); while (r < 0 && errno == EINTR);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* socket full: wait a bit (the other side reads in its thread or loop) */
            struct timespec ts = { 0, 2000000 };
            nanosleep(&ts, NULL);
            int tries = 0;
            do { r = sendmsg(sock, &mh, MSG_NOSIGNAL); if (r < 0 && errno == EAGAIN) nanosleep(&ts, NULL); } while (r < 0 && (errno == EAGAIN || errno == EINTR) && ++tries < 2500);
        }
        if (r < 0) return -1;
        off += k;
        first = 0;
    } while (off < len);
    return 0;
}

void vw_msg_free(VMsg *m, const VwAlloc *a) {
    if (m->json) { explicit_bzero(m->json, m->len); a->free(m->json); }
    for (int i = 0; i < m->nfds; i++) if (m->fds[i] >= 0) close(m->fds[i]);
    memset(m, 0, sizeof *m);
}

void vw_rx_free(VwRx *rx, const VwAlloc *a) {
    if (rx->active) {
        if (rx->cur.json) explicit_bzero(rx->cur.json, rx->cap);
        vw_msg_free(&rx->cur, a);
    }
    memset(rx, 0, sizeof *rx);
}

/* Reads one packet. 1 packet read, 0 nothing (EAGAIN), -1 closed/error */
static int read_packet(int sock, VwRx *rx, const VwAlloc *a, int flags) {
    VHdr h;
    char buf[VW_CHUNK];
    union { struct cmsghdr c; char b[CMSG_SPACE(sizeof(int) * VW_MAX_FDS)]; } u;
    struct iovec iov[2] = { { &h, sizeof h }, { buf, sizeof buf } };
    struct msghdr mh = { .msg_iov = iov, .msg_iovlen = 2, .msg_control = u.b, .msg_controllen = sizeof u.b };
    ssize_t r;
    do r = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC | flags); while (r < 0 && errno == EINTR);
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    if (r == 0) return -1;
    int fds[VW_MAX_FDS], nf = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        int *p = (int *)CMSG_DATA(c);
        for (size_t i = 0; i < n; i++) { if (nf < VW_MAX_FDS) fds[nf++] = p[i]; else close(p[i]); }
    }
    int bad = (size_t)r < sizeof h || h.len != (size_t)r - sizeof h || (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
    if (!bad && rx->active && (h.id != rx->cur.id || h.kind != rx->cur.kind)) bad = 1;
    if (!bad && rx->active && nf) bad = 1;                 /* fds only in the first one */
    if (!bad && rx->cur.len + h.len > VW_MAX) bad = 1;
    if (bad) {
        for (int i = 0; i < nf; i++) close(fds[i]);
        explicit_bzero(buf, sizeof buf);
        return -1;
    }
    if (!rx->active) {
        memset(&rx->cur, 0, sizeof rx->cur);
        rx->cur.kind = h.kind;
        rx->cur.id = h.id;
        rx->cap = 0;
        for (int i = 0; i < nf; i++) rx->cur.fds[i] = fds[i];
        rx->cur.nfds = nf;
        rx->active = 1;
    }
    size_t need = rx->cur.len + h.len + 1;
    if (need > rx->cap) {
        size_t nc = rx->cap ? rx->cap : 1024;
        while (nc < need) nc *= 2;
        char *p = a->alloc(nc);
        if (!p) { explicit_bzero(buf, sizeof buf); return -1; }
        if (rx->cur.json) { memcpy(p, rx->cur.json, rx->cur.len); explicit_bzero(rx->cur.json, rx->cap); a->free(rx->cur.json); }
        rx->cur.json = p;
        rx->cap = nc;
    }
    memcpy(rx->cur.json + rx->cur.len, buf, h.len);
    rx->cur.len += h.len;
    rx->cur.json[rx->cur.len] = 0;
    explicit_bzero(buf, h.len);
    return (h.flags & VWF_MORE) ? 2 : 1;
}

int vw_feed(int sock, VwRx *rx, VMsg *m, const VwAlloc *a) {
    for (;;) {
        int r = read_packet(sock, rx, a, MSG_DONTWAIT);
        if (r == 0) return 0;
        if (r < 0) { vw_rx_free(rx, a); return -1; }
        if (r == 1) {
            if (!rx->cur.json) { rx->cur.json = a->alloc(1); if (rx->cur.json) rx->cur.json[0] = 0; }
            *m = rx->cur;
            memset(&rx->cur, 0, sizeof rx->cur);
            rx->active = 0;
            rx->cap = 0;
            return 1;
        }
    }
}

int vw_recv(int sock, VMsg *m, const VwAlloc *a) {
    VwRx rx;
    memset(&rx, 0, sizeof rx);
    for (;;) {
        int r = read_packet(sock, &rx, a, 0);
        if (r < 0) { int closed = !rx.active; vw_rx_free(&rx, a); return closed ? 1 : -1; }
        if (r == 1) {
            if (!rx.cur.json) { rx.cur.json = a->alloc(1); if (rx.cur.json) rx.cur.json[0] = 0; }
            *m = rx.cur;
            return 0;
        }
    }
}
