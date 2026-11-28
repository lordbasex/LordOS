/* proto.c — sending and receiving messages between lord-server and lord-media (see proto.h). Both
 * binaries compile it. It validates lengths and descriptors: a broken or compromised helper can't
 * make lord-server read outside a buffer. */
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

int mp_ring_layout(int64_t fmt, int64_t w, int64_t h, int64_t slots, int64_t colors, MpRing *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (w < 1 || h < 1 || w > MP_MAX_DIM || h > MP_MAX_DIM || slots < 1 || slots > MP_RING_SLOTS) return -1;
    if (fmt == MF_BGRA) {
        if (colors != 0) return -1;
        out->slot_bytes = (size_t)w * (size_t)h * 4;
        out->y_stride = (int)w * 4;
    } else if (fmt == MF_NV12) {
        if ((w & 1) || (h & 1) || w < 2 || h < 2) return -1;
        if (colors < 0 || (colors & ~(int64_t)(0xff | MC_FULL)) || (colors & 0xff) >= MC_MATRICES) return -1;
        out->y_off = 0;
        out->y_stride = (int)w;
        out->uv_off = (size_t)w * (size_t)h;
        out->uv_stride = (int)w;
        out->slot_bytes = (size_t)w * (size_t)h + (size_t)w * (size_t)(h / 2);
        out->matrix = (int)(colors & 0xff);
        out->full = (colors & MC_FULL) != 0;
    } else return -1;
    out->fmt = (int)fmt;
    out->w = (int)w;
    out->h = (int)h;
    out->slots = (int)slots;
    return 0;
}

int mp_dma_check(const void *payload, size_t len, int nfds, const int64_t size[MP_DMA_MAX_OBJ], int max_w, int max_h,
                 int (*plane_ok)(uint32_t fourcc, uint64_t modifier)) {
    MpDma d;
    if (!payload || len != sizeof d || !size) return -1;
    memcpy(&d, payload, sizeof d);
    if (d.magic != MP_DMA_MAGIC || d.pad) return -1;
    if (max_w > MP_MAX_DIM) max_w = MP_MAX_DIM;
    if (max_h > MP_MAX_DIM) max_h = MP_MAX_DIM;
    if (d.w < 2 || d.h < 2 || (d.w & 1) || (d.h & 1) || d.w > (uint32_t)max_w || d.h > (uint32_t)max_h) return -1;
    if ((d.colors & ~(uint32_t)(0xff | MC_FULL)) || (d.colors & 0xff) >= MC_MATRICES) return -1;
    if (d.nobj < 1 || d.nobj > MP_DMA_MAX_OBJ || (int)d.nobj != nfds) return -1;
    int deep = d.plane[0].fourcc == MP_FOURCC_R16;
    if (!(d.plane[0].fourcc == MP_FOURCC_R8 || deep)) return -1;
    if (d.plane[1].fourcc != (deep ? MP_FOURCC_GR1616 : MP_FOURCC_GR88)) return -1;
    uint64_t bpp = deep ? 2 : 1;
    int used[MP_DMA_MAX_OBJ] = { 0 };
    uint64_t lo[2], hi[2];
    for (int i = 0; i < 2; i++) {
        uint32_t o = d.plane[i].obj;
        if (o >= d.nobj) return -1;
        if (size[o] <= 0 || (uint64_t)size[o] > MP_DMA_MAX_BYTES) return -1;
        uint64_t rows = i ? d.h / 2 : d.h, row = (uint64_t)d.w * bpp;      /* U-V: w/2 pairs of two samples */
        uint64_t pitch = d.plane[i].pitch, off = d.plane[i].offset;
        if (pitch < row || pitch > MP_DMA_MAX_PITCH) return -1;
        uint64_t end = off + pitch * (rows - 1) + row;                      /* no overflow: every term is < 2^32 */
        if (end > (uint64_t)size[o]) return -1;
        if (plane_ok && !plane_ok(d.plane[i].fourcc, d.modifier[o])) return -1;
        lo[i] = off;
        hi[i] = off + pitch * rows;
        used[o] = 1;
    }
    if (d.plane[0].obj == d.plane[1].obj && lo[0] < hi[1] && lo[1] < hi[0]) return -1;
    for (uint32_t i = 0; i < d.nobj; i++) if (!used[i]) return -1;
    return 0;
}
