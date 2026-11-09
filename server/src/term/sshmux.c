/* sshmux.c — see sshmux.h. */
#include "sshmux.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MUX_VERSION 4u

/* ---- the wire ---- */

void mxb_reset(MuxBuf *b) { b->n = 0; b->oom = 0; }
void mxb_free(MuxBuf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static void mxb_put(MuxBuf *b, const void *d, size_t n) {
    if (b->oom) return;
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 128;
        while (c < b->n + n) c *= 2;
        uint8_t *p = realloc(b->p, c);
        if (!p) { b->oom = 1; return; }
        b->p = p;
        b->cap = c;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

void mxb_u8(MuxBuf *b, uint8_t v) { mxb_put(b, &v, 1); }

void mxb_u32(MuxBuf *b, uint32_t v) {
    uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    mxb_put(b, x, 4);
}

void mxb_str(MuxBuf *b, const char *s, size_t n) {
    mxb_u32(b, (uint32_t)n);
    if (n) mxb_put(b, s, n);
}

int mxr_u32(MuxRd *r, uint32_t *v) {
    if (r->n - r->off < 4) return -1;
    const uint8_t *p = r->p + r->off;
    *v = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    r->off += 4;
    return 0;
}

int mxr_str(MuxRd *r, const char **s, size_t *n) {
    uint32_t len;
    if (mxr_u32(r, &len) || len > r->n - r->off) return -1;
    *s = (const char *)r->p + r->off;
    *n = len;
    r->off += len;
    return 0;
}

void mux_hello(MuxBuf *b) { mxb_u32(b, MUX_MSG_HELLO); mxb_u32(b, MUX_VERSION); }
void mux_alive(MuxBuf *b, uint32_t id) { mxb_u32(b, MUX_C_ALIVE_CHECK); mxb_u32(b, id); }

/* as ssh's mux client: the flags are uint32s, no escape character, no environment */
void mux_new_session(MuxBuf *b, uint32_t id, int subsystem, const char *command) {
    mxb_u32(b, MUX_C_NEW_SESSION);
    mxb_u32(b, id);
    mxb_str(b, "", 0);                  /* reserved */
    mxb_u32(b, 0);                      /* no tty */
    mxb_u32(b, 0);                      /* no X11 */
    mxb_u32(b, 0);                      /* no agent */
    mxb_u32(b, subsystem ? 1 : 0);
    mxb_u32(b, 0xffffffffu);            /* no escape character */
    mxb_str(b, "", 0);                  /* terminal type */
    mxb_str(b, command, strlen(command));
}

void mux_fwd(MuxBuf *b, int open, uint32_t id, const char *listen_host, uint32_t listen_port, const char *connect_host, uint32_t connect_port) {
    mxb_u32(b, open ? MUX_C_OPEN_FWD : MUX_C_CLOSE_FWD);
    mxb_u32(b, id);
    mxb_u32(b, MUX_FWD_LOCAL);
    mxb_str(b, listen_host, strlen(listen_host));
    mxb_u32(b, listen_port);
    mxb_str(b, connect_host, strlen(connect_host));
    mxb_u32(b, connect_port);
}

int mux_quote_args(const char *const *argv, char *out, size_t cap) {
    size_t o = 0;
#define PUT(c) do { if (o + 1 >= cap) return -1; out[o++] = (c); } while (0)
    for (int i = 0; argv[i]; i++) {
        if (i) PUT(' ');
        PUT('\'');
        for (const unsigned char *p = (const unsigned char *)argv[i]; *p; p++) {
            /* a quote or a backslash is refused, not escaped: the server's login shell parses the command, and
             * its quoting rules differ (fish takes \' inside single quotes as a quote); without them '…' is a
             * plain string in sh, bash, zsh, fish and tcsh alike */
            if (*p < 0x20 || *p == 0x7f || *p == '\'' || *p == '\\') return -1;
            PUT((char)*p);
        }
        PUT('\'');
    }
#undef PUT
    if (o >= cap) return -1;
    out[o] = 0;
    return 0;
}

/* ---- the connection ---- */

static long left_ms(const struct timespec *end) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (end->tv_sec - now.tv_sec) * 1000 + (end->tv_nsec - now.tv_nsec) / 1000000;
}

static void deadline(struct timespec *end, int ms) {
    clock_gettime(CLOCK_MONOTONIC, end);
    end->tv_sec += ms / 1000;
    end->tv_nsec += (long)(ms % 1000) * 1000000;
    if (end->tv_nsec >= 1000000000) { end->tv_sec++; end->tv_nsec -= 1000000000; }
}

static int wait_fd(int fd, short ev, const struct timespec *end) {
    for (;;) {
        long ms = left_ms(end);
        if (ms <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd p = { fd, ev, 0 };
        int r = poll(&p, 1, (int)ms);
        if (r > 0) return 0;
        if (r < 0 && errno != EINTR) return -1;
    }
}

static int fail(Mux *m, const char *why) { snprintf(m->why, sizeof m->why, "%s", why); return -1; }

static int write_all(Mux *m, const uint8_t *p, size_t n, const struct timespec *end) {
    while (n) {
        if (wait_fd(m->fd, POLLOUT, end)) return fail(m, "timeout");
        ssize_t k = send(m->fd, p, n, MSG_NOSIGNAL);
        if (k < 0) { if (errno == EINTR || errno == EAGAIN) continue; return fail(m, "write"); }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static int read_all(Mux *m, uint8_t *p, size_t n, const struct timespec *end) {
    while (n) {
        if (wait_fd(m->fd, POLLIN, end)) return fail(m, "timeout");
        ssize_t k = recv(m->fd, p, n, 0);
        if (k == 0) return fail(m, "closed");
        if (k < 0) { if (errno == EINTR || errno == EAGAIN) continue; return fail(m, "read"); }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static int send_packet(Mux *m, const MuxBuf *body, const struct timespec *end) {
    if (body->oom) return fail(m, "memory");
    uint8_t len[4] = { (uint8_t)(body->n >> 24), (uint8_t)(body->n >> 16), (uint8_t)(body->n >> 8), (uint8_t)body->n };
    return write_all(m, len, 4, end) || write_all(m, body->p, body->n, end) ? -1 : 0;
}

/* a packet's body in *out (malloc'd); its type read */
static int recv_packet(Mux *m, uint8_t **out, MuxRd *r, uint32_t *type, const struct timespec *end) {
    uint8_t h[4];
    if (read_all(m, h, 4, end)) return -1;
    uint32_t n = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
    if (n < 4 || n > MUX_PACKET_MAX) return fail(m, "bad packet");
    uint8_t *p = malloc(n);
    if (!p) return fail(m, "memory");
    if (read_all(m, p, n, end)) { free(p); return -1; }
    *out = p;
    *r = (MuxRd){ p, n, 0 };
    if (mxr_u32(r, type)) { free(p); return fail(m, "bad packet"); }
    return 0;
}

static int send_fd(int sock, int fd) {
    char ch = 0;
    struct iovec v = { &ch, 1 };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } cb;
    memset(&cb, 0, sizeof cb);
    struct msghdr msg = { 0 };
    msg.msg_iov = &v;
    msg.msg_iovlen = 1;
    msg.msg_control = cb.buf;
    msg.msg_controllen = sizeof cb.buf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_len = CMSG_LEN(sizeof(int));
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    ssize_t k;
    while ((k = sendmsg(sock, &msg, MSG_NOSIGNAL)) < 0 && errno == EINTR) {}
    return k == 1 ? 0 : -1;
}

/* a failure's reason (PERMISSION_DENIED / FAILURE), kept short and printable */
static void reason(Mux *m, MuxRd *r, const char *prefix) {
    const char *s;
    size_t n;
    if (mxr_str(r, &s, &n)) { fail(m, prefix); return; }
    char t[120];
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < sizeof t; i++) t[k++] = (unsigned char)s[i] < 0x20 || s[i] == 0x7f ? ' ' : s[i];
    t[k] = 0;
    snprintf(m->why, sizeof m->why, "%s: %s", prefix, t);
}

int mux_connect(Mux *m, const char *path, int timeout_ms) {
    memset(m, 0, sizeof *m);
    m->fd = -1;
    m->next_id = 1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (!path || strlen(path) >= sizeof a.sun_path) return fail(m, "path");
    memcpy(a.sun_path, path, strlen(path) + 1);
    m->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m->fd < 0) return fail(m, "socket");
    if (connect(m->fd, (struct sockaddr *)&a, sizeof a)) { mux_close(m); return fail(m, "connect"); }
    struct ucred cr;
    socklen_t cl = sizeof cr;
    if (getsockopt(m->fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) || cr.uid != getuid()) { mux_close(m); return fail(m, "not ours"); }
    struct timespec end;
    deadline(&end, timeout_ms);
    MuxBuf b = { 0 };
    mux_hello(&b);
    int rc = send_packet(m, &b, &end);
    uint8_t *p = NULL;
    MuxRd r;
    uint32_t type, ver;
    if (!rc) rc = recv_packet(m, &p, &r, &type, &end);
    if (!rc && (type != MUX_MSG_HELLO || mxr_u32(&r, &ver) || ver != MUX_VERSION)) rc = fail(m, "version");
    free(p);
    p = NULL;
    if (!rc) {
        mxb_reset(&b);
        uint32_t id = m->next_id++;
        mux_alive(&b, id);
        rc = send_packet(m, &b, &end);
        uint32_t rid, pid;
        if (!rc) rc = recv_packet(m, &p, &r, &type, &end);
        if (!rc && (type != MUX_S_ALIVE || mxr_u32(&r, &rid) || rid != id || mxr_u32(&r, &pid) || !pid)) rc = fail(m, "alive");
        if (!rc) m->master = (pid_t)pid;
        free(p);
    }
    mxb_free(&b);
    if (rc) { int fd = m->fd; m->fd = -1; if (fd >= 0) close(fd); }
    return rc;
}

void mux_close(Mux *m) {
    if (m->fd >= 0) close(m->fd);
    m->fd = -1;
}

int mux_session(Mux *m, int subsystem, const char *command, int in, int out, int err, int timeout_ms) {
    if (m->fd < 0) return fail(m, "closed");
    struct timespec end;
    deadline(&end, timeout_ms);
    MuxBuf b = { 0 };
    uint32_t id = m->next_id++;
    mux_new_session(&b, id, subsystem, command);
    int rc = send_packet(m, &b, &end);
    mxb_free(&b);
    if (rc) return -1;
    if (send_fd(m->fd, in) || send_fd(m->fd, out) || send_fd(m->fd, err)) return fail(m, "send fds");
    uint8_t *p = NULL;
    MuxRd r;
    uint32_t type, rid, sid;
    if (recv_packet(m, &p, &r, &type, &end)) return -1;
    rc = 0;
    if (mxr_u32(&r, &rid) || rid != id) rc = fail(m, "out of sequence");
    else if (type == MUX_S_SESSION_OPENED) { if (mxr_u32(&r, &sid)) rc = fail(m, "bad packet"); }
    else if (type == MUX_S_PERMISSION_DENIED) { reason(m, &r, "denied"); rc = -1; }
    else if (type == MUX_S_FAILURE) { reason(m, &r, "failed"); rc = -1; }
    else rc = fail(m, "unexpected");
    free(p);
    return rc;
}

int mux_wait_exit(Mux *m, int timeout_ms) {
    if (m->fd < 0) return -1;
    struct timespec end;
    deadline(&end, timeout_ms);
    for (int i = 0; i < 8; i++) {
        uint8_t *p = NULL;
        MuxRd r;
        uint32_t type, sid, code;
        if (recv_packet(m, &p, &r, &type, &end)) return -1;
        int done = type == MUX_S_EXIT_MESSAGE && !mxr_u32(&r, &sid) && !mxr_u32(&r, &code);
        free(p);
        if (done) return (int)(code & 0xff);
        if (type != MUX_S_TTY_ALLOC_FAIL && type != MUX_S_OK) return fail(m, "unexpected");
    }
    return fail(m, "unexpected");
}

int mux_forward(Mux *m, int open, uint32_t listen_port, const char *connect_host, uint32_t connect_port, int timeout_ms) {
    if (m->fd < 0) return fail(m, "closed");
    if (!listen_port || listen_port > 65535 || !connect_port || connect_port > 65535) return fail(m, "port");
    struct timespec end;
    deadline(&end, timeout_ms);
    MuxBuf b = { 0 };
    uint32_t id = m->next_id++;
    mux_fwd(&b, open, id, "127.0.0.1", listen_port, connect_host, connect_port);
    int rc = send_packet(m, &b, &end);
    mxb_free(&b);
    if (rc) return -1;
    uint8_t *p = NULL;
    MuxRd r;
    uint32_t type, rid;
    if (recv_packet(m, &p, &r, &type, &end)) return -1;
    if (mxr_u32(&r, &rid) || rid != id) rc = fail(m, "out of sequence");
    else if (type == MUX_S_OK || type == MUX_S_REMOTE_PORT) rc = 0;
    else if (type == MUX_S_PERMISSION_DENIED) { reason(m, &r, "denied"); rc = -1; }
    else if (type == MUX_S_FAILURE) { reason(m, &r, "failed"); rc = -1; }
    else rc = fail(m, "unexpected");
    free(p);
    return rc;
}

int sshmux_find(const char *dir, pid_t want, char *out, size_t cap) {
    struct stat st;
    if (want <= 0 || lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) return -1;
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int seen = 0, found = -1;
    while (found < 0 && (e = readdir(d)) && seen < 32) {
        if (e->d_name[0] == '.') continue;
        seen++;
        char p[sizeof ((struct sockaddr_un *)0)->sun_path];
        if ((size_t)snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= sizeof p) continue;
        if (lstat(p, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != getuid()) continue;
        Mux m;
        if (mux_connect(&m, p, 1500)) continue;
        if (m.master == want && strlen(p) < cap) { memcpy(out, p, strlen(p) + 1); found = 0; }
        mux_close(&m);
    }
    closedir(d);
    return found;
}
