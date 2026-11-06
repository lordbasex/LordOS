/* cloud_rc.c — the only client of rclone's rc (docs/CLOUD.md 4.2): one request per connection to the Unix socket,
 * HTTP/1.0 POST with Basic auth, a JSON body, the whole answer read back. The method is an RcMethod: its path comes
 * from the closed table in cloud_pure.c. */
#include "cloud_rc.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define RC_MAX_ANSWER (32u << 20)

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* waits for fd: 1 ready, 0 out of time */
static int wait_fd(int fd, short ev, long long end) {
    for (;;) {
        long long left = end - now_ms();
        if (left <= 0) return 0;
        struct pollfd p = { fd, ev, 0 };
        int r = poll(&p, 1, (int)left);
        if (r > 0) return 1;
        if (r < 0 && errno != EINTR) return 0;
    }
}

int rc_call(const RcConn *c, int m, const char *body, char **out, size_t *olen, int timeout_ms) {
    *out = NULL;
    if (olen) *olen = 0;
    if (!body) body = "{}";
    size_t bl = strlen(body);
    char hdr[512];
    int hl = rc_build_header(hdr, sizeof hdr, m, c->auth, bl);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (hl < 0 || strlen(c->sock) >= sizeof a.sun_path) return -1;
    memcpy(a.sun_path, c->sock, strlen(c->sock) + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    long long end = now_ms() + timeout_ms;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) && errno != EINPROGRESS && errno != EAGAIN) { close(fd); return -1; }
    struct iovec v[2] = { { hdr, (size_t)hl }, { (void *)body, bl } };
    size_t sent = 0, total = (size_t)hl + bl;
    while (sent < total) {
        if (!wait_fd(fd, POLLOUT, end)) { close(fd); return -1; }
        struct iovec w[2];
        int k = 0;
        size_t skip = sent;
        for (int i = 0; i < 2; i++) {
            if (skip >= v[i].iov_len) { skip -= v[i].iov_len; continue; }
            w[k].iov_base = (char *)v[i].iov_base + skip;
            w[k].iov_len = v[i].iov_len - skip;
            skip = 0;
            k++;
        }
        ssize_t n = writev(fd, w, k);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) { close(fd); return -1; }
        sent += (size_t)n;
    }
    /* no shutdown(SHUT_WR): Go's server reads a half-close as the client gone and cancels the call; HTTP/1.0 ends
     * with the server closing */
    size_t cap = 65536, len = 0;
    char *b = malloc(cap);
    if (!b) { close(fd); return -1; }
    for (;;) {
        if (len + 1 >= cap) {
            if (cap >= RC_MAX_ANSWER) { free(b); close(fd); return -1; }
            char *nb = realloc(b, cap * 2);
            if (!nb) { free(b); close(fd); return -1; }
            b = nb;
            cap *= 2;
        }
        if (!wait_fd(fd, POLLIN, end)) { free(b); close(fd); return -1; }
        ssize_t n = read(fd, b + len, cap - 1 - len);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n < 0) { free(b); close(fd); return -1; }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(fd);
    b[len] = 0;
    int status;
    char *bd;
    size_t bdl;
    if (rc_parse_response(b, len, &status, &bd, &bdl)) { free(b); return -1; }
    memmove(b, bd, bdl + 1);
    *out = b;
    if (olen) *olen = bdl;
    return status;
}
