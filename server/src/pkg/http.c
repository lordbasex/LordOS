/* http.c — HTTP/1.1 client and downloads with dropped privileges (see http.h).
 *
 * The child (nobody) speaks HTTP: GET with Host, keep-alive, redirects (up to 5; http:// may go to
 * https://, https:// never back to http://), Content-Length, chunked or until the connection closes,
 * If-Modified-Since, and a 30 s maximum time without data. An https:// URL (0.8.5): the child connects
 * the TCP socket itself and hands it to lord-tls (src/tls/tlsproxy.c: fixed path next to lord-server,
 * no arguments, empty environment, the helpers' sandbox), which verifies the server's certificate and
 * host name and relays plain HTTP over a socketpair; the HTTP code below is the same for both. It sends the parent messages over the pipe:
 *   [type 1 byte][item 4][length 4][data]   type H = "status date length", D = data, E = end
 * (E with text = error). A file: URL (a local repository) is read by the same child, as nobody. */
#include "http.h"
#include "../errcode.h"
#include "crypto.h"
#include "../sec/cgroup.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <grp.h>
#include <netdb.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT_MS 30000

/* ---- dates ---- */

int64_t http_parse_date(const char *s) {
    static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    char m[4] = "";
    const char *c = strchr(s, ',');
    if (c) s = c + 1;
    if (sscanf(s, " %d %3s %d %d:%d:%d", &tm.tm_mday, m, &tm.tm_year, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 0;
    const char *p = strstr(mon, m);
    if (!p || !m[0]) return 0;
    tm.tm_mon = (int)(p - mon) / 3;
    tm.tm_year -= 1900;
    return (int64_t)timegm(&tm);
}

void http_format_date(int64_t t, char *out, size_t n) {
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    static const char *const days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mons[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    snprintf(out, n, "%s, %02d %s %04d %02d:%02d:%02d GMT", days[tm.tm_wday], tm.tm_mday, mons[tm.tm_mon],
             tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ---- child: HTTP ---- */

typedef struct {
    int fd;
    int tls;                            /* fd is lord-tls's plaintext end (an https:// connection) */
    pid_t helper;                       /* lord-tls, or 0 */
    char host[256], port[8];
    char buf[16384];
    size_t pos, len;
    int out;                            /* pipe to the parent */
} Conn;

static int send_msg(int out, char type, uint32_t item, const void *data, uint32_t len) {
    uint8_t h[9] = { (uint8_t)type, (uint8_t)item, (uint8_t)(item >> 8), (uint8_t)(item >> 16), (uint8_t)(item >> 24),
                     (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24) };
    const uint8_t *parts[2] = { h, data };
    size_t lens[2] = { 9, len };
    for (int i = 0; i < 2; i++)
        for (size_t o = 0; o < lens[i];) {
            ssize_t w = write(out, parts[i] + o, lens[i] - o);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return -1;
            o += (size_t)w;
        }
    return 0;
}

static void conn_close(Conn *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->pos = c->len = 0;
    if (c->helper > 0) { kill(c->helper, SIGTERM); waitpid(c->helper, NULL, 0); }
    c->helper = 0;
}

/* lord-tls: next to this binary (lord-server), root's and not writable by others */
static int tls_helper_path(char *out, size_t cap) {
    char exe[PATH_MAX];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k <= 0) return -1;
    exe[k] = 0;
    char *slash = strrchr(exe, '/');
    if (!slash) return -1;
    *slash = 0;
    if ((size_t)snprintf(out, cap, "%s/lord-tls", exe) >= cap) return -1;
    struct stat st;
    if (stat(out, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 022) || (st.st_uid != 0 && st.st_uid != getuid())) return -1;
    return 0;
}

/* Wraps the connected TCP socket `tcp` in TLS: lord-tls gets it as fd 3 and a socketpair as fd 4,
 * reads "<host>\n" and answers '1' when the certificate checks out. Returns the plaintext end, or -1. */
static int tls_start(Conn *c, int tcp, const char *host, char *err, size_t errlen) {
    char path[PATH_MAX];
    if (tls_helper_path(path, sizeof path)) { close(tcp); lerr_set(err, errlen, "pkg.http.noTls", NULL); return -1; }
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp)) { close(tcp); lerr_set(err, errlen, "pkg.http.connectFailed", "host", host, NULL); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(tcp); close(sp[0]); close(sp[1]); lerr_set(err, errlen, "pkg.http.connectFailed", "host", host, NULL); return -1; }
    if (pid == 0) {
        int a = fcntl(tcp, F_DUPFD, 10), b = fcntl(sp[1], F_DUPFD, 10);
        if (a < 0 || b < 0 || dup2(a, 3) != 3 || dup2(b, 4) != 4) _exit(127);
        syscall(SYS_close_range, 5, ~0u, 0);
        const char *const argv[] = { "lord-tls", NULL };
        char *const envp[] = { NULL };
        execve(path, (char *const *)argv, envp);
        _exit(127);
    }
    close(tcp);
    close(sp[1]);
    c->helper = pid;
    char line[300];
    int ln = snprintf(line, sizeof line, "%s\n", host);
    char ok = 0;
    struct pollfd p = { sp[0], POLLIN, 0 };
    if (write(sp[0], line, (size_t)ln) != ln || poll(&p, 1, 25000) != 1 || read(sp[0], &ok, 1) != 1 || ok != '1') {
        close(sp[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        c->helper = 0;
        lerr_set(err, errlen, "pkg.http.tlsFailed", "host", host, NULL);
        return -1;
    }
    return sp[0];
}

static int conn_open(Conn *c, const char *host, const char *port, int tls, char *err, size_t errlen) {
    if (c->fd >= 0 && c->tls == tls && !strcmp(c->host, host) && !strcmp(c->port, port)) return 0;
    conn_close(c);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo(host, port, &hints, &res);
    if (r) { lerr_set(err, errlen, "pkg.http.hostNotFound", "host", host, NULL); return -1; }
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) && errno != EINPROGRESS) { close(fd); continue; }
        struct pollfd p = { fd, POLLOUT, 0 };
        int e = 0;
        socklen_t el = sizeof e;
        if (poll(&p, 1, 15000) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) || e) { close(fd); continue; }
        c->fd = fd;
        break;
    }
    freeaddrinfo(res);
    if (c->fd < 0) { lerr_set(err, errlen, "pkg.http.connectFailed", "host", host, NULL); return -1; }
    c->tls = tls;
    if (tls && (c->fd = tls_start(c, c->fd, host, err, errlen)) < 0) return -1;
    snprintf(c->host, sizeof c->host, "%s", host);
    snprintf(c->port, sizeof c->port, "%s", port);
    return 0;
}

/* Reads into the buffer (returns new bytes, 0 = closed, -1 = error/timeout). */
static ssize_t conn_fill(Conn *c) {
    if (c->pos == c->len) c->pos = c->len = 0;
    if (c->pos > 0 && c->len == sizeof c->buf) {
        memmove(c->buf, c->buf + c->pos, c->len - c->pos);
        c->len -= c->pos;
        c->pos = 0;
    }
    if (c->len == sizeof c->buf) return -1;
    struct pollfd p = { c->fd, POLLIN, 0 };
    int r = poll(&p, 1, TIMEOUT_MS);
    if (r <= 0) return -1;
    ssize_t n = read(c->fd, c->buf + c->len, sizeof c->buf - c->len);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return conn_fill(c);
    if (n > 0) c->len += (size_t)n;
    return n;
}

static int conn_write(Conn *c, const char *s, size_t n) {
    while (n) {
        ssize_t w = write(c->fd, s, n);
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            struct pollfd p = { c->fd, POLLOUT, 0 };
            if (poll(&p, 1, TIMEOUT_MS) <= 0) return -1;
            continue;
        }
        if (w <= 0) return -1;
        s += w;
        n -= (size_t)w;
    }
    return 0;
}

/* A CRLF-terminated line (without it). */
static int conn_line(Conn *c, char *out, size_t cap) {
    for (;;) {
        char *nl = memchr(c->buf + c->pos, '\n', c->len - c->pos);
        if (nl) {
            size_t l = (size_t)(nl - (c->buf + c->pos));
            size_t k = l && nl[-1] == '\r' ? l - 1 : l;
            if (k >= cap) return -1;
            memcpy(out, c->buf + c->pos, k);
            out[k] = 0;
            c->pos += l + 1;
            return 0;
        }
        if (c->len - c->pos >= cap) return -1;
        if (conn_fill(c) <= 0) return -1;
    }
}

static int parse_url(const char *url, char *host, size_t hn, char *port, size_t pn, const char **path, int *tls) {
    *tls = !strncmp(url, "https://", 8);
    if (!*tls && strncmp(url, "http://", 7)) return -1;
    const char *h = url + (*tls ? 8 : 7), *slash = strchr(h, '/');
    if (!slash) slash = h + strlen(h);
    size_t l = (size_t)(slash - h);
    const char *colon = memchr(h, ':', l);
    size_t hl = colon ? (size_t)(colon - h) : l;
    if (!hl || hl >= hn) return -1;
    memcpy(host, h, hl);
    host[hl] = 0;
    for (size_t i = 0; i < hl; i++) if (!isalnum((unsigned char)host[i]) && !strchr("-.", host[i])) return -1;
    if (colon) {
        size_t pl = l - hl - 1;
        if (!pl || pl >= pn) return -1;
        memcpy(port, colon + 1, pl);
        port[pl] = 0;
        for (size_t i = 0; i < pl; i++) if (!isdigit((unsigned char)port[i])) return -1;
    } else snprintf(port, pn, *tls ? "443" : "80");
    *path = *slash ? slash : "/";
    return 0;
}

/* A file: source (a repository on a local disk or a USB stick, as apt's file: method): read as
 * nobody too, so only world-readable files; sent like a download, and the parent checks it the same
 * way (size, SHA-256 against the signed index). Only from a configured source: an HTTP redirect
 * never leads here. */
static void get_file(Conn *c, const FetchItem *it, uint32_t idx) {
    char err[200] = "";
    const char *path = it->url + 5;
    while (path[0] == '/' && path[1] == '/') path++;          /* file:///x and file:/x */
    int fd = path[0] == '/' && !strstr(path, "/../") ? open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK) : -1;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        lerr_set(err, sizeof err, "pkg.http.status", "status", "404", NULL);
    } else if (it->ims > 0 && (int64_t)st.st_mtime <= it->ims) {
        send_msg(c->out, 'H', idx, "304 0 0", 7);
    } else if ((uint64_t)st.st_size > it->max) {
        lerr_set(err, sizeof err, "pkg.http.tooLarge", NULL);
    } else {
        char h[96];
        snprintf(h, sizeof h, "200 %lld %lld", (long long)st.st_mtime, (long long)st.st_size);
        if (send_msg(c->out, 'H', idx, h, (uint32_t)strlen(h))) _exit(1);
        for (;;) {
            ssize_t r = read(fd, c->buf, sizeof c->buf);
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) { lerr_set(err, sizeof err, "pkg.http.connectionLost", NULL); break; }
            if (r == 0) break;
            if (send_msg(c->out, 'D', idx, c->buf, (uint32_t)r)) _exit(1);
        }
    }
    if (fd >= 0) close(fd);
    send_msg(c->out, 'E', idx, err, (uint32_t)strlen(err));
}

/* Downloads one item: sends H, D… and E. */
static void get_one(Conn *c, const FetchItem *it, uint32_t idx) {
    if (!strncmp(it->url, "file:", 5)) { get_file(c, it, idx); return; }
    char url[1100], err[200] = "", line[4096];
    snprintf(url, sizeof url, "%s", it->url);
    int was_tls = 0;
    for (int redirects = 0;; redirects++) {
        char host[256], port[8];
        const char *path;
        int tls;
        if (parse_url(url, host, sizeof host, port, sizeof port, &path, &tls)) { lerr_set(err, sizeof err, "pkg.http.badUrl", NULL); break; }
        if (was_tls && !tls) { lerr_set(err, sizeof err, "pkg.http.downgrade", NULL); break; }   /* https never back to http */
        was_tls = tls;
        const char *defport = tls ? "443" : "80";
        int retry = c->fd >= 0;             /* a reused connection may have been closed */
    again:
        if (conn_open(c, host, port, tls, err, sizeof err)) break;
        /* '+' as %2B, as apt does: an S3 origin (lordos.org) reads a bare '+' as a space, so
         * pool/rpi/k/kms++-utils/… would be a 404 */
        char epath[3 * sizeof url];         /* every byte of the URL could be a '+' */
        size_t en = 0;
        for (const char *q = path; *q; q++) {
            if (*q == '+') { memcpy(epath + en, "%2B", 3); en += 3; }
            else epath[en++] = *q;
        }
        epath[en] = 0;
        char req[3600], ims[80] = "";
        if (it->ims > 0) {
            char d[48];
            http_format_date(it->ims, d, sizeof d);
            snprintf(ims, sizeof ims, "If-Modified-Since: %s\r\n", d);
        }
        int rl = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s%s%s\r\nUser-Agent: LordOS-pkg/0.7\r\n"
                          "Accept-Encoding: identity\r\nConnection: keep-alive\r\n%s\r\n",
                          epath, host, strcmp(port, defport) ? ":" : "", strcmp(port, defport) ? port : "", ims);
        if (rl >= (int)sizeof req || conn_write(c, req, (size_t)rl) || conn_line(c, line, sizeof line)) {
            conn_close(c);
            if (retry) { retry = 0; goto again; }
            lerr_set(err, sizeof err, "pkg.http.noResponse", "host", host, NULL);
            break;
        }
        int status = 0, minor = 1;
        if (sscanf(line, "HTTP/1.%d %d", &minor, &status) != 2) { conn_close(c); lerr_set(err, sizeof err, "pkg.http.badResponse", NULL); break; }
        int64_t clen = -1, lastmod = 0;
        int chunked = 0, close_after = minor == 0;
        char location[1024] = "";
        for (;;) {
            if (conn_line(c, line, sizeof line)) { conn_close(c); lerr_set(err, sizeof err, "pkg.http.badHeaders", NULL); goto out; }
            if (!line[0]) break;
            char *v = strchr(line, ':');
            if (!v) continue;
            *v++ = 0;
            while (*v == ' ' || *v == '\t') v++;
            if (!strcasecmp(line, "Content-Length")) clen = strtoll(v, NULL, 10);
            else if (!strcasecmp(line, "Transfer-Encoding") && strcasestr(v, "chunked")) chunked = 1;
            else if (!strcasecmp(line, "Connection") && strcasestr(v, "close")) close_after = 1;
            else if (!strcasecmp(line, "Location")) snprintf(location, sizeof location, "%s", v);
            else if (!strcasecmp(line, "Last-Modified")) lastmod = http_parse_date(v);
        }
        int has_body = status != 304 && status != 204 && !(status >= 100 && status < 200);
        int redirect = (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && location[0];
        int deliver = status == 200;
        if (deliver) {
            char h[96];
            snprintf(h, sizeof h, "%d %lld %lld", status, (long long)lastmod, (long long)clen);
            if (send_msg(c->out, 'H', idx, h, (uint32_t)strlen(h))) _exit(1);
        }
        uint64_t got = 0;
        if (has_body) {
            /* body: delivered (200) or discarded (redirect, error) */
            int64_t remaining = chunked ? 0 : clen;
            for (;;) {
                if (chunked && remaining == 0) {
                    if (got && conn_line(c, line, sizeof line)) goto broken;            /* CRLF of the previous chunk */
                    if (conn_line(c, line, sizeof line)) goto broken;
                    remaining = strtoll(line, NULL, 16);
                    if (remaining < 0) goto broken;
                    if (remaining == 0) {
                        do { if (conn_line(c, line, sizeof line)) goto broken; } while (line[0]);   /* trailers */
                        break;
                    }
                }
                if (!chunked && clen >= 0 && remaining == 0) break;
                if (c->pos == c->len) {
                    ssize_t r = conn_fill(c);
                    if (r == 0 && !chunked && clen < 0) break;                       /* until close */
                    if (r <= 0) goto broken;
                }
                size_t n = c->len - c->pos;
                if ((chunked || clen >= 0) && (int64_t)n > remaining) n = (size_t)remaining;
                if (deliver) {
                    if (got + n > it->max) { lerr_set(err, sizeof err, "pkg.http.tooLarge", NULL); conn_close(c); goto out; }
                    if (send_msg(c->out, 'D', idx, c->buf + c->pos, (uint32_t)n)) _exit(1);
                }
                got += n;
                c->pos += n;
                if (chunked || clen >= 0) remaining -= (int64_t)n;
            }
            if (!chunked && clen < 0) close_after = 1;
        }
        if (close_after) conn_close(c);
        if (redirect && redirects < 5) {
            if (location[0] == '/') snprintf(url, sizeof url, "%s://%s%s%s%s", tls ? "https" : "http", host, strcmp(port, defport) ? ":" : "", strcmp(port, defport) ? port : "", location);
            else snprintf(url, sizeof url, "%s", location);
            continue;
        }
        if (status == 304) {
            char h[48];
            snprintf(h, sizeof h, "304 0 0");
            send_msg(c->out, 'H', idx, h, (uint32_t)strlen(h));
        } else if (!deliver) lerr_set(err, sizeof err, "pkg.http.status", "status", lerr_int(status), NULL);
        break;
    broken:
        conn_close(c);
        lerr_set(err, sizeof err, "pkg.http.connectionLost", NULL);
        break;
    }
out:
    send_msg(c->out, 'E', idx, err, (uint32_t)strlen(err));
}

/* Switches to nobody: no groups, unable to go back to root. */
static int drop_to_nobody(void) {
    struct passwd *pw = getpwnam("nobody");
    uid_t uid = pw ? pw->pw_uid : 65534;
    gid_t gid = pw ? pw->pw_gid : 65534;
    if (setgroups(0, NULL) || setresgid(gid, gid, gid) || setresuid(uid, uid, uid)) return -1;
    if (setuid(0) == 0 || geteuid() == 0) return -1;
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    return chdir("/");
}

/* ---- parent: writes, counts, verifies ---- */

static int read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int fetch_batch(FetchItem *items, int n, FetchProgress cb, void *ud) {
    if (n <= 0) return 0;
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return 0;
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        total += items[i].expect;
        items[i].status = 0;
        items[i].size = 0;
        items[i].last_modified = 0;
        lerr_set(items[i].err, sizeof items[i].err, "pkg.http.notDownloaded", NULL);
    }
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return 0; }
    if (pid == 0) {
        close(p[0]);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
        int out = fcntl(p[1], F_DUPFD_CLOEXEC, 3);
        dup2(out, 3);
        syscall(SYS_close_range, 4, ~0u, 0);
        signal(SIGPIPE, SIG_IGN);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        /* (0.8.5) in lordos-net-store.scope, so the Store and the updates are their own app for the
         * firewall and Monitor → Network (lordos:store); outside a LordOS session, where it is */
        if (getenv("LORDOS_SESSIOND")) cgroup_enter_root("lordos-net-store.scope");
        if (drop_to_nobody()) _exit(2);
        Conn c = { .fd = -1, .out = 3, .tls = 0, .helper = 0 };
        for (int i = 0; i < n; i++) get_one(&c, &items[i], (uint32_t)i);
        conn_close(&c);
        _exit(0);
    }
    close(p[1]);
    int fd = -1, cur = -1, ok = 0;
    uint64_t done = 0;
    Sha256 sh;
    char tmp[600];
    uint8_t *buf = malloc(1 << 16);
    struct timespec last = { 0, 0 };
    for (;;) {
        uint8_t h[9];
        if (!buf || read_full(p[0], h, 9)) break;
        uint32_t item = (uint32_t)h[1] | (uint32_t)h[2] << 8 | (uint32_t)h[3] << 16 | (uint32_t)h[4] << 24;
        uint32_t len = (uint32_t)h[5] | (uint32_t)h[6] << 8 | (uint32_t)h[7] << 16 | (uint32_t)h[8] << 24;
        if (item >= (uint32_t)n || len > (1 << 16)) break;
        if (read_full(p[0], buf, len)) break;
        FetchItem *it = &items[item];
        if (h[0] == 'H') {
            char t[96];
            snprintf(t, sizeof t, "%.*s", (int)(len < 95 ? len : 95), buf);
            long long lm = 0, cl = 0;
            sscanf(t, "%d %lld %lld", &it->status, &lm, &cl);
            it->last_modified = lm;
            if (it->status == 200) {
                if (fd >= 0) close(fd);
                snprintf(tmp, sizeof tmp, "%s.part", it->dest);
                unlink(tmp);
                fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
                cur = (int)item;
                sha256_init(&sh);
                if (fd < 0) { lerr_set(it->err, sizeof it->err, "pkg.http.createFailed", "path", tmp, NULL); it->status = 0; }
            }
        } else if (h[0] == 'D') {
            if ((int)item != cur || fd < 0) continue;
            if (it->size + len > it->max) { close(fd); fd = -1; unlink(tmp); it->status = 0; lerr_set(it->err, sizeof it->err, "pkg.http.tooLarge", NULL); continue; }
            for (uint32_t o = 0; o < len;) {
                ssize_t w = write(fd, buf + o, len - o);
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) { close(fd); fd = -1; unlink(tmp); it->status = 0; lerr_set(it->err, sizeof it->err, "pkg.http.writeFailed", NULL); break; }
                o += (uint32_t)w;
            }
            sha256_update(&sh, buf, len);
            it->size += len;
            done += len;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (cb && (now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000 >= 250) {
                last = now;
                cb(done, total, ud);
            }
        } else if (h[0] == 'E') {
            if (len) {
                snprintf(it->err, sizeof it->err, "%.*s", (int)(len < 199 ? len : 199), buf);
                if (it->status == 200) it->status = 0;
            } else it->err[0] = 0;
            if ((int)item == cur && fd >= 0) {
                int good = it->status == 200 && fsync(fd) == 0;
                close(fd);
                fd = -1;
                if (good) {
                    sha256_final(&sh, it->sha256);
                    if (rename(tmp, it->dest)) { unlink(tmp); it->status = 0; lerr_set(it->err, sizeof it->err, "pkg.http.saveFailed", "path", it->dest, NULL); }
                } else unlink(tmp);
            }
            if (it->status == 200 || it->status == 304) ok++;
        }
    }
    if (fd >= 0) { close(fd); unlink(tmp); }
    free(buf);
    close(p[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (cb) cb(done, total, ud);
    return ok;
}
