/* castproto.c — the ring and the messages between lord-server and lord-screencast (see castproto.h). */
#include "castproto.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

int cast_layout(uint32_t w, uint32_t h, CastLayout *l) {
    if (w < CAST_MIN_DIM || h < CAST_MIN_DIM || w > CAST_MAX_DIM || h > CAST_MAX_DIM) return -1;
    l->stride = w * 4;
    l->slot_bytes = ((uint64_t)l->stride * h + 4095) & ~(uint64_t)4095;
    l->total = CAST_RING_HDR + l->slot_bytes * CAST_SLOTS;
    return 0;
}

int cast_ring_ok(const void *map, uint64_t len, uint32_t w, uint32_t h) {
    CastLayout l;
    if (!map || cast_layout(w, h, &l) || len < l.total) return -1;
    CastRingHdr hd;
    memcpy(&hd, map, sizeof hd);
    return hd.magic == CAST_RING_MAGIC && hd.w == w && hd.h == h && hd.stride == l.stride && hd.slots == CAST_SLOTS
           && hd.slot_bytes == l.slot_bytes ? 0 : -1;
}

/* ---- lord-server's side: what the helper may say now ---- */

int cast_check_helper(CastPeer *p, const CastMsg *m, int nfds) {
    int s = m->stream;
    switch (m->type) {
    case CM_NODE:
        /* once per stream, a real id (PW_ID_ANY and the core's are not a stream's) */
        if (nfds || s >= p->nstreams || p->node[s] || m->a == 0 || m->a == 0xffffffffu) return -1;
        p->node[s] = m->a;
        return 0;
    case CM_WANT:
        if (nfds || s >= p->nstreams || !p->node[s] || m->a > 1) return -1;
        if (m->a && m->c == 0 && m->b != 0) return -1;      /* a rate over zero */
        return 0;
    case CM_DONE:
        if (nfds || s >= p->nstreams || m->a >= CAST_SLOTS || !p->busy[s][m->a]) return -1;
        p->busy[s][m->a] = 0;
        return 0;
    case CM_REMOTE:
        if (nfds > 1 || !m->a) return -1;
        for (int i = 0; i < CAST_MAX_REMOTES; i++)
            if (p->remote[i] == m->a) { p->remote[i] = 0; return 0; }
        return -1;                      /* nobody asked for it */
    case CM_FAIL:
        return nfds ? -1 : 0;
    default:
        return -1;
    }
}

int cast_slot_take(CastPeer *p, int s) {
    if (s < 0 || s >= p->nstreams) return -1;
    for (int i = 0; i < CAST_SLOTS; i++)
        if (!p->busy[s][i]) { p->busy[s][i] = 1; return i; }
    return -1;
}

int cast_remote_add(CastPeer *p, uint32_t req) {
    if (!req) return -1;
    for (int i = 0; i < CAST_MAX_REMOTES; i++) if (p->remote[i] == req) return -1;
    for (int i = 0; i < CAST_MAX_REMOTES; i++)
        if (!p->remote[i]) { p->remote[i] = req; return 0; }
    return -1;
}

/* ---- the helper's side: what lord-server may say now ---- */

int cast_check_server(CastSide *x, const CastMsg *m, int nfds) {
    int s = m->stream;
    CastLayout l;
    switch (m->type) {
    case CM_HELLO:
        if (x->hello || nfds != 1 || m->a < 1 || m->a > CAST_MAX_STREAMS) return -1;
        x->hello = 1;
        x->nstreams = (int)m->a;
        return 0;
    case CM_STREAM:
        if (!x->hello || nfds != 1 || s >= x->nstreams || cast_layout(m->w, m->h, &l) || m->a < 1 || m->a > CAST_MAX_FPS
            || (m->b != CAST_SRC_MONITOR && m->b != CAST_SRC_WINDOW)) return -1;
        x->have[s] = 1;
        x->w[s] = m->w;
        x->h[s] = m->h;
        return 0;
    case CM_FRAME:
        if (nfds || s >= x->nstreams || !x->have[s] || m->a >= CAST_SLOTS || m->w != x->w[s] || m->h != x->h[s]) return -1;
        return 0;
    case CM_CLIENT:
        return x->hello && nfds == 1 && m->a ? 0 : -1;
    case CM_STOP:
        return nfds ? -1 : 0;
    default:
        return -1;
    }
}

double cast_period_ms(uint32_t num, uint32_t den) {
    double fps = num && den ? (double)num / den : CAST_MAX_FPS;
    if (fps > CAST_MAX_FPS) fps = CAST_MAX_FPS;
    if (fps < 1) fps = 1;
    return 1000.0 / fps;
}

/* ---- the channel ---- */

int cast_send(int sock, const CastMsg *m, int fd) {
    struct iovec io = { (void *)m, sizeof *m };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } cb;
    memset(&cb, 0, sizeof cb);
    struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1 };
    if (fd >= 0) {
        mh.msg_control = cb.b;
        mh.msg_controllen = sizeof cb.b;
        struct cmsghdr *h = CMSG_FIRSTHDR(&mh);
        h->cmsg_level = SOL_SOCKET;
        h->cmsg_type = SCM_RIGHTS;
        h->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(h), &fd, sizeof fd);
    }
    for (;;) {
        ssize_t k = sendmsg(sock, &mh, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (k < 0 && errno == EINTR) continue;
        return k == (ssize_t)sizeof *m ? 0 : -1;
    }
}

int cast_recv(int sock, CastMsg *m, int *fd) {
    *fd = -1;
    CastMsg tmp;
    struct iovec io = { &tmp, sizeof tmp };
    union { struct cmsghdr h; char b[CMSG_SPACE(4 * sizeof(int))]; } cb;
    struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cb.b, .msg_controllen = sizeof cb.b };
    ssize_t k;
    do k = recvmsg(sock, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC); while (k < 0 && errno == EINTR);
    if (k < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    int got[8], ng = 0;
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&mh); h; h = CMSG_NXTHDR(&mh, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) continue;
        int nf = (int)((h->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        for (int i = 0; i < nf; i++) {
            int f;
            memcpy(&f, CMSG_DATA(h) + i * sizeof(int), sizeof f);
            if (ng < 8) got[ng++] = f; else close(f);
        }
    }
    int bad = k != (ssize_t)sizeof tmp || (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || ng > 1;
    if (bad) {
        for (int i = 0; i < ng; i++) close(got[i]);
        return -1;
    }
    *m = tmp;
    if (ng) *fd = got[0];
    return 1;
}
