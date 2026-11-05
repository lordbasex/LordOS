/* qmp.c — see qmp.h. */
#include "qmp.h"
#include "../doc/wire.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

int qmp_open(Qmp *q, int fd) {
    memset(q, 0, sizeof *q);
    q->fd = fd;
    q->buf = malloc(QMP_LINE_MAX + 1);
    return q->buf ? 0 : -1;
}

void qmp_close(Qmp *q) {
    if (q->fd >= 0) close(q->fd);
    free(q->buf);
    q->buf = NULL;
    q->fd = -1;
}

static int send_text(int fd, const char *s, size_t len, int pass) {
    struct iovec io = { (void *)s, len };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } cm;
    struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1 };
    if (pass >= 0) {
        memset(&cm, 0, sizeof cm);
        mh.msg_control = cm.b;
        mh.msg_controllen = sizeof cm.b;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &pass, sizeof pass);
    }
    /* commands are short and the socket is ours alone: a full buffer means QEMU stopped reading */
    size_t off = 0;
    while (off < len) {
        ssize_t w = sendmsg(fd, &mh, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; if (errno == EAGAIN) { struct pollfd p = { fd, POLLOUT, 0 }; if (poll(&p, 1, 1000) <= 0) return -1; continue; } return -1; }
        off += (size_t)w;
        io.iov_base = (char *)s + off; io.iov_len = len - off;
        mh.msg_control = NULL; mh.msg_controllen = 0;     /* the descriptor went with the first bytes */
    }
    return 0;
}

int qmp_command(Qmp *q, const char *cmd, const char *args, int fd) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"execute\":"); jb_str(&b, cmd);
    if (args) { jb_raw(&b, ",\"arguments\":"); jb_raw(&b, args); }
    jb_raw(&b, "}\n");
    int r = b.oom ? -1 : send_text(q->fd, b.p, b.n, fd);
    jb_free(&b);
    return r;
}

int qmp_raw(Qmp *q, const char *json) {
    size_t n = strlen(json);
    if (!n || memchr(json, '\n', n)) return -1;
    if (send_text(q->fd, json, n, -1)) return -1;
    return send_text(q->fd, "\n", 1, -1);
}

int qmp_fill(Qmp *q) {
    for (;;) {
        if (q->n >= QMP_LINE_MAX) {
            /* full: complete lines are taken first (qmp_next), the rest stays in the socket */
            if (memchr(q->buf, '\n', q->n)) return 0;
            /* no newline in a whole buffer: drop it and skip to the next line */
            q->n = 0;
            q->overflow = 1;
            q->refused++;
        }
        ssize_t r = recv(q->fd, q->buf + q->n, QMP_LINE_MAX - q->n, MSG_DONTWAIT);
        if (r > 0) {
            if (q->overflow) {
                char *nl = memchr(q->buf + q->n, '\n', (size_t)r);
                if (!nl) continue;
                size_t keep = (size_t)r - (size_t)(nl + 1 - (q->buf + q->n));
                memmove(q->buf, nl + 1, keep);
                q->n = keep;
                q->overflow = 0;
                continue;
            }
            q->n += (size_t)r;
            continue;
        }
        if (r == 0) return -1;
        if (errno == EINTR) continue;
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    }
}

int qmp_next(Qmp *q, JDoc *d) {
    for (;;) {
        char *nl = q->n ? memchr(q->buf, '\n', q->n) : NULL;
        if (!nl) return 0;
        size_t len = (size_t)(nl - q->buf);
        int ok = len > 0 && !json_parse(q->buf, len, d, NULL, 0);
        if (ok && d->root->type != J_OBJ) { json_free(d); ok = 0; }
        memmove(q->buf, nl + 1, q->n - len - 1);
        q->n -= len + 1;
        q->lines++;
        if (ok) return 1;
        if (len) q->refused++;
    }
}

int qmp_kind(const JVal *root) {
    if (!root || root->type != J_OBJ) return QMP_MSG_OTHER;
    if (json_get(root, "QMP")) return QMP_MSG_GREETING;
    if (json_get(root, "return")) return QMP_MSG_RETURN;
    if (json_get(root, "error")) return QMP_MSG_ERROR;
    if (json_str(root, "event")) return QMP_MSG_EVENT;
    return QMP_MSG_OTHER;
}

const char *qmp_error_desc(const JVal *root) {
    const JVal *e = json_get(root, "error");
    const char *s = e ? json_str(e, "desc") : NULL;
    return s ? s : "";
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* waits for a message of `want` (events are skipped) */
static int wait_for(Qmp *q, int want, long long deadline, char *err, size_t errlen) {
    for (;;) {
        JDoc d;
        int r = qmp_next(q, &d);
        if (r > 0) {
            int k = qmp_kind(d.root);
            if (k == QMP_MSG_ERROR) { snprintf(err, errlen, "QMP: %s", qmp_error_desc(d.root)); json_free(&d); return -1; }
            json_free(&d);
            if (k == want) return 0;
            continue;
        }
        long long left = deadline - now_ms();
        if (left <= 0) { snprintf(err, errlen, "QEMU's control socket didn't answer"); return -1; }
        struct pollfd p = { q->fd, POLLIN, 0 };
        if (poll(&p, 1, (int)left) < 0 && errno != EINTR) { snprintf(err, errlen, "poll: %s", strerror(errno)); return -1; }
        if (qmp_fill(q) < 0) { snprintf(err, errlen, "QEMU closed its control socket"); return -1; }
    }
}

int qmp_handshake(Qmp *q, int timeout_ms, char *err, size_t errlen) {
    long long deadline = now_ms() + timeout_ms;
    if (wait_for(q, QMP_MSG_GREETING, deadline, err, errlen)) return -1;
    if (qmp_command(q, "qmp_capabilities", NULL, -1)) { snprintf(err, errlen, "can't write to QEMU's control socket"); return -1; }
    return wait_for(q, QMP_MSG_RETURN, deadline, err, errlen);
}
