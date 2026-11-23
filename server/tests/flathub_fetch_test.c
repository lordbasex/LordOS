/* flathub_fetch_test.c — lord-fetch's FQ_FLATHUB job (E10, src/fetch/fhget.c and fetch.c), no network:
 *  1. the exchange (fh_exchange) over a plain socket to a stand-in server that answers with the recorded answers of
 *     tests/data/flathub/api-2026-11-22/ and with hostile ones: the exact GET path of the closed list and the Host,
 *     no redirect followed (301/302/307/308 are failures, nothing more is read), the 1 MB cap with a Content-Length,
 *     chunked or until close, a bad status, an answer that isn't JSON, a list number outside the list;
 *  2. build/lord-fetch itself: requests it must refuse ({"list": N} outside the list, of another type, with other
 *     keys, without a socket) and a peer that doesn't speak TLS.
 *   build/flathub-fetch-test [path to build/lord-fetch] (run from server/) */
#include "../src/fetch/fhget.h"
#include "../src/fetch/fetchproto.h"
#include "../src/doc/wire.h"

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(x) do { checks++; if (!(x)) { fails++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

#define DIR "tests/data/flathub/api-2026-11-22/"
#define NOV22 1795355100LL       /* 2026-11-22 13:45:00 UTC */

static char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    char *b = malloc(4 << 20);
    *n = b ? fread(b, 1, 4 << 20, f) : 0;
    fclose(f);
    return b;
}

static int write_all(int fd, const void *p, size_t n) {
    const char *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w <= 0) return -1;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

/* ---- the stand-in server: one request, one canned answer ---- */

enum { A_LEN, A_CHUNKED, A_CLOSE, A_REDIRECT, A_BIGLEN, A_BIGCLOSE, A_BIGCHUNK, A_STATUS };
typedef struct {
    int fd, kind, status;
    const char *body;
    size_t blen;
    char request[2048];         /* what the client sent */
    size_t rlen;
} Srv;

static void *serve(void *arg) {
    Srv *s = arg;
    /* the request, up to its empty line */
    while (s->rlen < sizeof s->request - 1 && !strstr(s->request, "\r\n\r\n")) {
        ssize_t r = read(s->fd, s->request + s->rlen, sizeof s->request - 1 - s->rlen);
        if (r <= 0) break;
        s->rlen += (size_t)r;
        s->request[s->rlen] = 0;
    }
    char h[512];
    switch (s->kind) {
    case A_LEN:
        snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n", s->blen);
        write_all(s->fd, h, strlen(h));
        write_all(s->fd, s->body, s->blen);
        break;
    case A_CHUNKED: {
        snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
        write_all(s->fd, h, strlen(h));
        for (size_t off = 0; off < s->blen; off += 100) {
            size_t k = s->blen - off < 100 ? s->blen - off : 100;
            snprintf(h, sizeof h, "%zx\r\n", k);
            write_all(s->fd, h, strlen(h));
            write_all(s->fd, s->body + off, k);
            write_all(s->fd, "\r\n", 2);
        }
        write_all(s->fd, "0\r\n\r\n", 5);
        break;
    }
    case A_CLOSE:
        snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
        write_all(s->fd, h, strlen(h));
        write_all(s->fd, s->body, s->blen);
        break;
    case A_REDIRECT:
        snprintf(h, sizeof h, "HTTP/1.1 %d Moved\r\nLocation: https://attacker.example/api/v2/collection/popular\r\nContent-Length: 0\r\n\r\n", s->status);
        write_all(s->fd, h, strlen(h));
        break;
    case A_BIGLEN:
        snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", FH_ANSWER_MAX + 1);
        write_all(s->fd, h, strlen(h));
        break;
    case A_BIGCLOSE:
    case A_BIGCHUNK: {
        snprintf(h, sizeof h, s->kind == A_BIGCLOSE ? "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" : "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
        write_all(s->fd, h, strlen(h));
        static char junk[65536];
        memset(junk, ' ', sizeof junk);
        for (int i = 0; i < 20; i++) {                 /* 1.25 MB, more than the cap */
            if (s->kind == A_BIGCHUNK && write_all(s->fd, "10000\r\n", 7)) break;
            if (write_all(s->fd, junk, sizeof junk)) break;
            if (s->kind == A_BIGCHUNK && write_all(s->fd, "\r\n", 2)) break;
        }
        break;
    }
    case A_STATUS:
        snprintf(h, sizeof h, "HTTP/1.1 %d Oops\r\nContent-Length: 2\r\n\r\n{}", s->status);
        write_all(s->fd, h, strlen(h));
        break;
    }
    shutdown(s->fd, SHUT_WR);
    return NULL;
}

/* one exchange of list against an answer of kind: 0 with ids, or -1 with err; s->request gets the request */
static int exchange(int list, int kind, int status, const char *body, size_t blen, FhIds *ids, char *err, Srv *s) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) return -2;
    memset(s, 0, sizeof *s);
    s->fd = sv[1];
    s->kind = kind;
    s->status = status;
    s->body = body;
    s->blen = blen;
    pthread_t th;
    pthread_create(&th, NULL, serve, s);
    Conn *c = conn_new(sv[0], FH_HOST, 5000);
    err[0] = 0;
    int rc = fh_exchange(c, list, NOV22, ids, err, 24);
    conn_close(c);                       /* closes sv[0] */
    pthread_join(th, NULL);
    close(sv[1]);
    return rc;
}

static void test_exchange(void) {
    static FhIds ids;
    static Srv s;
    char err[24];
    size_t n, wn;
    char *pop = slurp(DIR "popular.json", &n), *week = slurp(DIR "apps-of-the-week.json", &wn);
    CHECK(pop && week);
    if (!pop || !week) return;
    /* a recorded answer with a length: the GET of the closed list's path, to flathub.org, 250 ids back */
    CHECK(exchange(FH_POPULAR, A_LEN, 200, pop, n, &ids, err, &s) == 0 && ids.n == 250 && !strcmp(ids.id[0], "org.vinegarhq.Sober"));
    CHECK(!strncmp(s.request, "GET /api/v2/collection/popular HTTP/1.1\r\n", 41) && strstr(s.request, "\r\nHost: flathub.org\r\n"));
    /* chunked, and the picks' date from the clock */
    CHECK(exchange(FH_WEEK, A_CHUNKED, 200, week, wn, &ids, err, &s) == 0 && ids.n == 5 && !strcmp(ids.id[0], "io.github.sitraorg.sitra"));
    CHECK(!strncmp(s.request, "GET /api/v2/app-picks/apps-of-the-week/2026-11-22 HTTP/1.1\r\n", 60));
    CHECK(exchange(FH_DAY, A_CLOSE, 200, "{\"app_id\":\"nl.andreasknoben.Laser\",\"day\":\"2026-11-22\"}", 54, &ids, err, &s) == 0 && ids.n == 1);
    CHECK(!strncmp(s.request, "GET /api/v2/app-picks/app-of-the-day/2026-11-22 HTTP/1.1\r\n", 58));
    const char *paths[] = { "trending", "recently-added", "recently-updated" };
    for (int i = 0; i < 3; i++) {
        char want[96];
        snprintf(want, sizeof want, "GET /api/v2/collection/%s HTTP/1.1\r\n", paths[i]);
        CHECK(exchange(FH_TRENDING + i, A_LEN, 200, "{\"hits\":[]}", 11, &ids, err, &s) == 0 && ids.n == 0 && !strncmp(s.request, want, strlen(want)));
    }
    /* no redirect is followed: a failure, and only one request was made */
    int codes[] = { 301, 302, 303, 307, 308 };
    for (int i = 0; i < 5; i++) {
        CHECK(exchange(FH_POPULAR, A_REDIRECT, codes[i], NULL, 0, &ids, err, &s) == -1 && !strcmp(err, "redirect") && ids.n == 0);
        CHECK(strstr(s.request, "GET ") == s.request && !strstr(s.request + 4, "GET "));     /* one request, nothing after */
    }
    /* the size cap: a Content-Length over 1 MB, an answer that goes on until it closes, chunks that add up past it */
    CHECK(exchange(FH_POPULAR, A_BIGLEN, 200, NULL, 0, &ids, err, &s) == -1 && !strcmp(err, "big"));
    CHECK(exchange(FH_POPULAR, A_BIGCLOSE, 200, NULL, 0, &ids, err, &s) == -1 && !strcmp(err, "big"));
    CHECK(exchange(FH_POPULAR, A_BIGCHUNK, 200, NULL, 0, &ids, err, &s) == -1 && !strcmp(err, "big"));
    /* other statuses, answers that aren't the list's JSON */
    CHECK(exchange(FH_POPULAR, A_STATUS, 500, NULL, 0, &ids, err, &s) == -1 && !strcmp(err, "status"));
    CHECK(exchange(FH_POPULAR, A_STATUS, 404, NULL, 0, &ids, err, &s) == -1 && !strcmp(err, "status"));
    CHECK(exchange(FH_POPULAR, A_LEN, 200, "<html>hi</html>", 15, &ids, err, &s) == -1 && !strcmp(err, "format"));
    CHECK(exchange(FH_DAY, A_LEN, 200, pop, n, &ids, err, &s) == -1 && !strcmp(err, "format"));
    /* a number outside the closed list: refused before anything is sent */
    CHECK(exchange(FH_LISTS, A_LEN, 200, pop, n, &ids, err, &s) == -1 && !strcmp(err, "bad-request") && s.rlen == 0);
    CHECK(exchange(-1, A_LEN, 200, pop, n, &ids, err, &s) == -1 && !strcmp(err, "bad-request") && s.rlen == 0);
    free(pop);
    free(week);
}

/* ---- build/lord-fetch ---- */

static int ask(int ch, uint32_t id, const char *req, int with_fd, char *err, size_t en) {
    int pair[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    int rc = doc_send(ch, FQ_FLATHUB, id, req, strlen(req), with_fd ? &pair[0] : NULL, with_fd ? 1 : 0);
    close(pair[0]);
    close(pair[1]);                       /* a peer that says nothing and goes */
    if (rc) return -1;
    DocMsg m;
    struct pollfd pf = { ch, POLLIN, 0 };
    if (poll(&pf, 1, 35000) != 1 || doc_recv(ch, &m)) return -1;
    rc = (int)m.h.op;
    const char *e = m.json ? strstr(m.json, "\"error\":\"") : NULL;
    snprintf(err, en, "%s", e ? e + 9 : "");
    char *q = strchr(err, '"');
    if (q) *q = 0;
    CHECK(m.h.id == id && !m.nfds);
    doc_msg_free(&m);
    return rc;
}

static void test_helper(const char *bin) {
    if (access(bin, X_OK)) { printf("flathub-fetch-test: lord-fetch skipped: needs %s\n", bin); return; }
    int sv[2];
    CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv));
    pid_t p = fork();
    if (p == 0) {
        dup2(sv[1], 3);
        char *argv[] = { "lord-fetch", NULL }, *envp[] = { NULL };
        execve(bin, argv, envp);
        _exit(127);
    }
    close(sv[1]);
    const char *bad[] = { "{\"list\":6}", "{\"list\":-1}", "{\"list\":1.5}", "{\"list\":\"1\"}", "{\"list\":true}", "{\"list\":[0]}",
                          "{\"list\":0,\"path\":\"/etc\"}", "{\"list\":0,\"host\":\"attacker.example\"}", "{}", "[0]", "not json",
                          "{\"list\":1e300}", "{\"list\":4294967296}", NULL };
    char err[32];
    for (int i = 0; bad[i]; i++) CHECK(ask(sv[0], (uint32_t)i + 1, bad[i], 1, err, sizeof err) == FR_ERR && !strcmp(err, "bad-request"));
    CHECK(ask(sv[0], 50, "{\"list\":0}", 0, err, sizeof err) == FR_ERR && !strcmp(err, "bad-request"));      /* no socket */
    /* a good request on a peer that isn't TLS: the handshake fails, nothing else happens */
    CHECK(ask(sv[0], 51, "{\"list\":0}", 1, err, sizeof err) == FR_ERR && !strcmp(err, "tls"));
    CHECK(ask(sv[0], 52, "{\"list\":5}", 1, err, sizeof err) == FR_ERR && !strcmp(err, "tls"));
    doc_send(sv[0], FQ_END, 0, "{}", 2, NULL, 0);
    int st;
    waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(sv[0]);
    printf("flathub-fetch-test: lord-fetch refuses bad requests and a peer without TLS: ok\n");
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    unsetenv("LORDOS_SESSIOND");
    test_exchange();
    test_helper(argc > 1 ? argv[1] : "build/lord-fetch");
    printf("flathub-fetch-test: %d/%d ok\n", checks - fails, checks);
    return fails ? 1 : 0;
}
