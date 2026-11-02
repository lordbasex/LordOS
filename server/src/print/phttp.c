/* phttp.c — see phttp.h. */
#include "phttp.h"
#include "../sec/netconn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int phttp_unix(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    return fd;
}

int phttp_lan_ipv4(const char *ip) {
    struct in_addr in;
    if (!ip || inet_pton(AF_INET, ip, &in) != 1) return 0;
    uint32_t a = ntohl(in.s_addr);
    return (a >> 24) == 10 || (a >> 20) == (172u << 4 | 1) || (a >> 16) == (192u << 8 | 168) ||
           (a >> 16) == (169u << 8 | 254) || (a >> 24) == 127;
}

int phttp_tcp(const char *ip, int port, int timeout_ms) {
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (port < 1 || port > 65535 || inet_pton(AF_INET, ip, &a.sin_addr) != 1) return -1;
    /* made and connected by the printers' connector, in lordos-net-print.scope (sec/netconn.h) */
    int fd = netconn_socket(NC_PRINT, AF_INET, SOCK_STREAM, (struct sockaddr *)&a, sizeof a);
    if (fd < 0) return -1;
    struct pollfd p = { fd, POLLOUT, 0 };
    int err = 0;
    socklen_t el = sizeof err;
    if (poll(&p, 1, timeout_ms) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) || err) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return fd;
}

static int wait_io(int fd, short ev, double deadline) {
    for (;;) {
        int left = (int)(deadline - now_ms());
        if (left <= 0) return -1;
        struct pollfd p = { fd, ev, 0 };
        int r = poll(&p, 1, left);
        if (r > 0) return 0;
        if (r < 0 && errno == EINTR) continue;
        return -1;
    }
}

static int send_all(int fd, const void *d, size_t n, double deadline) {
    const char *p = d;
    while (n) {
        if (wait_io(fd, POLLOUT, deadline)) return -1;
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

typedef struct { int fd; char buf[16384]; size_t a, b; double deadline; } Rd;

static int rd_fill(Rd *r) {
    if (r->a < r->b) return 0;
    if (wait_io(r->fd, POLLIN, r->deadline)) return -1;
    ssize_t n;
    do n = recv(r->fd, r->buf, sizeof r->buf, 0); while (n < 0 && errno == EINTR);
    if (n <= 0) return -1;
    r->a = 0;
    r->b = (size_t)n;
    return 0;
}

static int rd_line(Rd *r, char *out, size_t n) {
    size_t k = 0;
    for (;;) {
        if (rd_fill(r)) return -1;
        char c = r->buf[r->a++];
        if (c == '\n') break;
        if (k + 1 < n) out[k++] = c;
        else return -1;                              /* line too long */
    }
    if (k && out[k - 1] == '\r') k--;
    out[k] = 0;
    return 0;
}

static int rd_bytes(Rd *r, char *out, size_t n) {
    while (n) {
        if (rd_fill(r)) return -1;
        size_t k = r->b - r->a < n ? r->b - r->a : n;
        memcpy(out, r->buf + r->a, k);
        r->a += k;
        out += k;
        n -= k;
    }
    return 0;
}

void presp_free(PResp *r) { free(r->body); memset(r, 0, sizeof *r); }

static int append(PResp *r, size_t *cap, const char *d, size_t n, size_t max) {
    if (r->len + n > max) return -1;
    if (r->len + n + 1 > *cap) {
        size_t c = *cap ? *cap * 2 : 8192;
        while (c < r->len + n + 1) c *= 2;
        char *p = realloc(r->body, c);
        if (!p) return -1;
        r->body = p;
        *cap = c;
    }
    memcpy(r->body + r->len, d, n);
    r->len += n;
    r->body[r->len] = 0;
    return 0;
}

int phttp_request(int fd, const char *method, const char *host, const char *path, const char *headers,
                  const char *ctype, const void *body, size_t blen, int docfd, size_t doclen,
                  size_t max, int timeout_ms, PResp *r) {
    memset(r, 0, sizeof *r);
    double deadline = now_ms() + timeout_ms;
    char head[2048];
    size_t total = blen + (docfd >= 0 ? doclen : 0);
    int hl = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: LordOS/0.7\r\nConnection: close\r\n%s%s%s",
                      method, path, host, ctype ? "Content-Type: " : "", ctype ? ctype : "", ctype ? "\r\n" : "");
    if (hl < 0 || (size_t)hl >= sizeof head) return -1;
    if (body || docfd >= 0 || !strcmp(method, "POST") || !strcmp(method, "PUT"))
        hl += snprintf(head + hl, sizeof head - (size_t)hl, "Content-Length: %zu\r\n", total);
    if (headers) hl += snprintf(head + hl, sizeof head - (size_t)hl, "%s", headers);
    hl += snprintf(head + hl, sizeof head - (size_t)hl, "\r\n");
    if ((size_t)hl >= sizeof head) return -1;
    if (send_all(fd, head, (size_t)hl, deadline)) return -1;
    if (blen && send_all(fd, body, blen, deadline)) return -1;
    if (docfd >= 0) {
        char buf[1 << 15];
        off_t off = 0;
        while ((size_t)off < doclen) {
            ssize_t k = pread(docfd, buf, sizeof buf, off);
            if (k <= 0) return -1;
            if (send_all(fd, buf, (size_t)k, deadline + 600000)) return -1;   /* big jobs take long */
            off += k;
        }
    }
    Rd *rd = malloc(sizeof *rd);
    if (!rd) return -1;
    rd->fd = fd;
    rd->a = rd->b = 0;
    rd->deadline = deadline + (docfd >= 0 ? 600000 : 0);
    char line[1024];
    /* 100 Continue and other 1xx replies are skipped */
    do {
        if (rd_line(rd, line, sizeof line) || strncmp(line, "HTTP/1.", 7) || strlen(line) < 12) { free(rd); return -1; }
        r->status = atoi(line + 9);
        if (r->status >= 100 && r->status < 200) while (!rd_line(rd, line, sizeof line) && line[0]) {}
    } while (r->status >= 100 && r->status < 200);
    long long clen = -1;
    int chunked = 0, headers_n = 0;
    for (;;) {
        if (rd_line(rd, line, sizeof line)) { free(rd); return -1; }
        if (!line[0]) break;
        if (++headers_n > 100) { free(rd); return -1; }
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (!strcasecmp(line, "Content-Length")) clen = strtoll(v, NULL, 10);
        else if (!strcasecmp(line, "Transfer-Encoding") && strcasestr(v, "chunked")) chunked = 1;
        else if (!strcasecmp(line, "Location")) snprintf(r->location, sizeof r->location, "%s", v);
        else if (!strcasecmp(line, "WWW-Authenticate")) snprintf(r->www_auth, sizeof r->www_auth, "%s", v);
        else if (!strcasecmp(line, "Content-Type")) snprintf(r->content_type, sizeof r->content_type, "%s", v);
        else if (!strcasecmp(line, "Retry-After")) r->retry_after = atoi(v);
    }
    size_t cap = 0;
    char *tmp = malloc(1 << 16);
    int ok = tmp ? 0 : -1;
    if (!ok && chunked) {
        for (;;) {
            if (rd_line(rd, line, sizeof line)) { ok = -1; break; }
            char *end;
            long long n = strtoll(line, &end, 16);
            if (end == line || n < 0 || (size_t)n > max) { ok = -1; break; }
            if (!n) { while (!rd_line(rd, line, sizeof line) && line[0]) {} break; }
            while (n > 0) {
                size_t k = n > (1 << 16) ? (1 << 16) : (size_t)n;
                if (rd_bytes(rd, tmp, k) || append(r, &cap, tmp, k, max)) { ok = -1; break; }
                n -= (long long)k;
            }
            if (ok) break;
            if (rd_line(rd, line, sizeof line)) { ok = -1; break; }
        }
    } else if (!ok && clen >= 0) {
        if ((size_t)clen > max) ok = -1;
        while (!ok && clen > 0) {
            size_t k = clen > (1 << 16) ? (1 << 16) : (size_t)clen;
            if (rd_bytes(rd, tmp, k) || append(r, &cap, tmp, k, max)) ok = -1;
            clen -= (long long)k;
        }
    } else if (!ok && r->status != 204 && r->status != 304) {
        /* until it closes */
        for (;;) {
            if (rd_fill(rd)) break;
            size_t k = rd->b - rd->a;
            if (append(r, &cap, rd->buf + rd->a, k, max)) { ok = -1; break; }
            rd->a = rd->b;
        }
    }
    free(tmp);
    free(rd);
    if (ok) { int st = r->status; presp_free(r); r->status = st ? -st : 0; return -1; }
    return 0;
}
