/* webproxy.c — lord-web's gate in front of WebKit's remote inspector (webguard.h; docs/WEBAPPS.md, "Developer tools").
 *
 * A listening socket on 127.0.0.1 (a port the kernel picks) in lord-web's own GLib loop: no thread, no other process.
 * Each connection is one GSource with the browser's socket and, once its request passed webguard_check, one to WebKit's
 * inspector on 127.0.0.1:<backend>. At most WEBPROXY_CONNS connections; a request head must arrive within
 * WEBPROXY_HEAD_S and fit in WEBGUARD_HEAD_MAX; a plain request's answer must end within WEBPROXY_REPLY_S. A plain
 * request is forwarded with "Connection: close" and nothing the browser sends after its head reaches WebKit; a
 * WebSocket's bytes go both ways only after WebKit answered 101 (the browser sends nothing before it: anything it does
 * ends the connection). */
#include "web.h"
#include "webguard.h"

#include <errno.h>
#include <glib-unix.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/socket.h>

#define WEBPROXY_CONNS    16
#define WEBPROXY_HEAD_S   10
#define WEBPROXY_REPLY_S  60
#define WEBPROXY_BUF      (32 * 1024)

struct WebProxy {
    int lfd;
    unsigned backend;
    int nconn;
    WebGuardKeys keys;
};

enum { C_HEAD, C_REPLY, C_CONNECTING, C_FORWARD, C_WAIT101, C_TUNNEL };

typedef struct {
    GSource src;
    WebProxy *px;
    int cfd, bfd;
    gpointer ctag, btag;
    int state;
    char head[WEBGUARD_HEAD_MAX];
    size_t hlen;
    char c2b[WEBGUARD_HEAD_MAX + 64];   /* the forwarded head, then a WebSocket's bytes */
    size_t c2b_len, c2b_off;
    char b2c[WEBPROXY_BUF];
    size_t b2c_len, b2c_off;
    int upgrade;                        /* a WebSocket upgrade: wait for WebKit's 101 */
    int c_eof, b_eof;
} Conn;

static int random_token(char *out) {
    unsigned char r[WEBGUARD_RAND_BYTES];
    size_t got = 0;
    while (got < sizeof r) {
        ssize_t n = getrandom(r + got, sizeof r - got, 0);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        got += (size_t)n;
    }
    int ok = webguard_b64url(r, sizeof r, out, WEBGUARD_TOKEN_LEN + 1) == WEBGUARD_TOKEN_LEN;
    explicit_bzero(r, sizeof r);
    return ok ? 0 : -1;
}

static void conn_finalize(GSource *s) {
    Conn *c = (Conn *)s;
    if (c->cfd >= 0) close(c->cfd);
    if (c->bfd >= 0) close(c->bfd);
    c->px->nconn--;
}

static void set_deadline(Conn *c, int secs) {
    g_source_set_ready_time(&c->src, secs ? g_get_monotonic_time() + (gint64)secs * G_USEC_PER_SEC : -1);
}

static void reply(Conn *c, size_t n) {
    c->b2c_len = n;
    c->b2c_off = 0;
    c->state = C_REPLY;
}

static int backend_connect(Conn *c) {
    c->bfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (c->bfd < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)c->px->backend), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (connect(c->bfd, (struct sockaddr *)&a, sizeof a) && errno != EINPROGRESS) return -1;
    c->btag = g_source_add_unix_fd(&c->src, c->bfd, G_IO_OUT);
    c->state = C_CONNECTING;
    set_deadline(c, WEBPROXY_REPLY_S);
    return 0;
}

/* the head is complete: its verdict */
static int on_head(Conn *c, size_t end) {
    WebProxy *px = c->px;
    int v = webguard_check(c->head, end, &px->keys);
    if (v == WEBGUARD_UPGRADE && c->hlen > end) v = WEBGUARD_DENY;    /* bytes before WebKit's 101 */
    if (v == WEBGUARD_OPEN) {
        reply(c, webguard_open_reply(&px->keys, c->b2c, sizeof c->b2c));
        explicit_bzero(px->keys.secret, sizeof px->keys.secret);        /* once */
        fprintf(stderr, "lord-web: developer tools: the browser opened them\n");
        return c->b2c_len ? 0 : -1;
    }
    if (v == WEBGUARD_FORWARD) c->c2b_len = webguard_rewrite(c->head, end, c->c2b, sizeof c->c2b);
    else if (v == WEBGUARD_UPGRADE) { memcpy(c->c2b, c->head, end); c->c2b_len = end; }
    if (v == WEBGUARD_DENY || !c->c2b_len) {
        fprintf(stderr, "lord-web: developer tools: a request without this window's cookie, or not from its own tab, refused\n");
        reply(c, webguard_deny_reply(c->b2c, sizeof c->b2c));
        return c->b2c_len ? 0 : -1;
    }
    c->c2b_off = 0;
    c->upgrade = v == WEBGUARD_UPGRADE;
    return backend_connect(c);
}

static int rd(int fd, char *buf, size_t n, int *eof) {
    ssize_t k = recv(fd, buf, n, 0);
    if (k > 0) return (int)k;
    if (k == 0) { *eof = 1; return 0; }
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
}

static int wr(int fd, const char *buf, size_t n) {
    ssize_t k = send(fd, buf, n, MSG_NOSIGNAL);
    if (k >= 0) return (int)k;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
}

/* one round of I/O; -1 ends the connection */
static int step(Conn *c) {
    GIOCondition cr = g_source_query_unix_fd(&c->src, c->ctag);
    GIOCondition br = c->btag ? g_source_query_unix_fd(&c->src, c->btag) : 0;
    if (cr & G_IO_ERR) return -1;

    if (c->state == C_HEAD && (cr & (G_IO_IN | G_IO_HUP))) {
        int k = rd(c->cfd, c->head + c->hlen, sizeof c->head - c->hlen, &c->c_eof);
        if (k < 0 || c->c_eof) return -1;
        c->hlen += (size_t)k;
        long end = webguard_head_end(c->head, c->hlen);
        if (end < 0) { reply(c, webguard_deny_reply(c->b2c, sizeof c->b2c)); }
        else if (end > 0 && on_head(c, (size_t)end)) return -1;
    }
    if (c->state == C_CONNECTING && (br & (G_IO_OUT | G_IO_ERR | G_IO_HUP))) {
        int err = 0;
        socklen_t el = sizeof err;
        if (getsockopt(c->bfd, SOL_SOCKET, SO_ERROR, &err, &el) || err) return -1;
        c->state = c->upgrade ? C_WAIT101 : C_FORWARD;
    }
    if (c->state == C_FORWARD || c->state == C_WAIT101 || c->state == C_TUNNEL) {
        /* the browser's side: in a tunnel, its bytes for WebKit; before, anything from it ends the connection */
        if (cr & (G_IO_IN | G_IO_HUP)) {
            if (c->state != C_TUNNEL || !(cr & G_IO_IN)) return -1;    /* (a hang-up with nothing left to read) */
            if (c->c2b_len < sizeof c->c2b) {
                int k = rd(c->cfd, c->c2b + c->c2b_len, sizeof c->c2b - c->c2b_len, &c->c_eof);
                if (k < 0) return -1;
                c->c2b_len += (size_t)k;
            }
        }
        if (c->c2b_off < c->c2b_len && (br & G_IO_OUT)) {
            int k = wr(c->bfd, c->c2b + c->c2b_off, c->c2b_len - c->c2b_off);
            if (k < 0) return -1;
            c->c2b_off += (size_t)k;
            if (c->c2b_off == c->c2b_len) c->c2b_off = c->c2b_len = 0;
        }
        /* WebKit's side */
        if ((br & (G_IO_IN | G_IO_HUP | G_IO_ERR)) && c->b2c_len < sizeof c->b2c) {
            size_t at = c->b2c_len;
            int k = rd(c->bfd, c->b2c + at, sizeof c->b2c - at, &c->b_eof);
            if (k < 0) return -1;
            c->b2c_len += (size_t)k;
            if (c->state == C_WAIT101 && k > 0) {
                int sw = webguard_switched(c->b2c, c->b2c_len);
                if (sw == 1) { c->state = C_TUNNEL; set_deadline(c, 0); }
                else if (sw == 0) c->state = C_FORWARD;
            }
        }
        /* WebKit hung up: poll reports that on every round whatever is asked, so while the browser hasn't taken what we
         * hold, its socket leaves the loop (parked, still open: the kernel may hold more of its answer); at its end the
         * socket goes */
        if (c->b_eof || ((br & G_IO_HUP) && c->b2c_len == sizeof c->b2c)) {
            g_source_remove_unix_fd(&c->src, c->btag);
            c->btag = NULL;
            if (c->b_eof) { close(c->bfd); c->bfd = -1; }
        }
    }
    if (c->b2c_off < c->b2c_len && (cr & G_IO_OUT)) {
        int k = wr(c->cfd, c->b2c + c->b2c_off, c->b2c_len - c->b2c_off);
        if (k < 0) return -1;
        c->b2c_off += (size_t)k;
        /* keep the start of WebKit's answer until it says whether it switched */
        if (c->b2c_off == c->b2c_len && c->state != C_WAIT101) c->b2c_off = c->b2c_len = 0;
    }
    if (c->bfd >= 0 && !c->btag && c->b2c_len < sizeof c->b2c) {
        /* a parked socket of a WebKit that hung up: what is left reads without waiting */
        int k = rd(c->bfd, c->b2c + c->b2c_len, sizeof c->b2c - c->b2c_len, &c->b_eof);
        if (k < 0) return -1;
        c->b2c_len += (size_t)k;
        if (c->b_eof) { close(c->bfd); c->bfd = -1; }
    }
    int b2c_empty = c->b2c_off == c->b2c_len;
    if (c->state == C_REPLY && b2c_empty) return -1;
    if (c->b_eof && b2c_empty) return -1;
    if (c->c_eof && c->c2b_off == c->c2b_len) return -1;
    return 0;
}

static void watch(Conn *c) {
    GIOCondition ce = 0, be = 0;
    if (c->state == C_HEAD || c->state == C_FORWARD || c->state == C_WAIT101 || (c->state == C_TUNNEL && !c->c_eof && c->c2b_len < sizeof c->c2b))
        ce |= G_IO_IN;
    if (c->b2c_off < c->b2c_len) ce |= G_IO_OUT;
    g_source_modify_unix_fd(&c->src, c->ctag, ce);
    if (!c->btag) return;
    if (c->state == C_CONNECTING || c->c2b_off < c->c2b_len) be |= G_IO_OUT;
    if (c->state != C_CONNECTING && !c->b_eof && c->b2c_len < sizeof c->b2c) be |= G_IO_IN;
    g_source_modify_unix_fd(&c->src, c->btag, be);
}

static gboolean conn_dispatch(GSource *s, GSourceFunc cb, gpointer d) {
    (void)cb; (void)d;
    Conn *c = (Conn *)s;
    gint64 due = g_source_get_ready_time(s);
    if (due >= 0 && g_get_monotonic_time() >= due) return G_SOURCE_REMOVE;    /* too slow */
    if (step(c)) return G_SOURCE_REMOVE;
    watch(c);
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs conn_funcs = { .dispatch = conn_dispatch, .finalize = conn_finalize };

static gboolean on_accept(gint fd, GIOCondition cond, gpointer d) {
    (void)cond;
    WebProxy *px = d;
    for (;;) {
        int cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) break;
        if (px->nconn >= WEBPROXY_CONNS) { close(cfd); continue; }
        GSource *s = g_source_new(&conn_funcs, sizeof(Conn));
        Conn *c = (Conn *)s;
        c->px = px;
        c->cfd = cfd;
        c->bfd = -1;
        c->state = C_HEAD;
        px->nconn++;
        c->ctag = g_source_add_unix_fd(s, cfd, G_IO_IN);
        set_deadline(c, WEBPROXY_HEAD_S);
        g_source_attach(s, NULL);
        g_source_unref(s);
    }
    return G_SOURCE_CONTINUE;
}

WebProxy *webproxy_start(unsigned backend) {
    static WebProxy px;
    if (px.lfd > 0) return &px;
    char cookie[WEBGUARD_TOKEN_LEN + 1], secret[WEBGUARD_TOKEN_LEN + 1];
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, WEBPROXY_CONNS) || getsockname(fd, (struct sockaddr *)&a, &al)
        || random_token(cookie) || random_token(secret) || webguard_set_keys(&px.keys, ntohs(a.sin_port), secret, cookie)) {
        if (fd >= 0) close(fd);
        return NULL;
    }
    explicit_bzero(px.keys.secret, sizeof px.keys.secret);      /* none until webproxy_open_url */
    explicit_bzero(secret, sizeof secret);
    explicit_bzero(cookie, sizeof cookie);
    px.lfd = fd;
    px.backend = backend;
    g_unix_fd_add(fd, G_IO_IN, on_accept, &px);
    fprintf(stderr, "lord-web: developer tools: the browser's way in is 127.0.0.1:%u, WebKit's own port %u is one browsers refuse\n", px.keys.port, backend);
    return &px;
}

int webproxy_open_url(WebProxy *px, char *out, size_t n) {
    if (!px || random_token(px->keys.secret)) return -1;
    return webguard_open_url(&px->keys, out, n);
}
