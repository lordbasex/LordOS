/* mailnet_test.c — axia-maild's wire readers against hostile servers (docs/MAIL.md): HTTP chunk
 * sizes and Content-Length (overflow, garbage, negative), header limits, IMAP literal sizes near
 * SIZE_MAX, and lines that never end. Everything runs over a local socketpair: no network and a
 * global alarm, so `make test` can never hang on it. */
#include "mail/http.h"
#include "mail/imapio.h"
#include "mail/net.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* A "server" child that writes `resp` (len bytes, then repeats `tail` n times) and closes. */
static Conn *serve(const char *resp, size_t len, const char *tail, long reps) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return NULL;
    pid_t p = fork();
    if (p == 0) {
        close(sv[0]);
        signal(SIGPIPE, SIG_IGN);
        /* drain what the client sends */
        if (fork() == 0) { char b[4096]; while (read(sv[1], b, sizeof b) > 0) {} _exit(0); }
        if (len && write(sv[1], resp, len) < 0) _exit(1);
        for (long i = 0; tail && i < reps; i++) if (write(sv[1], tail, strlen(tail)) < 0) break;
        shutdown(sv[1], SHUT_WR);
        _exit(0);
    }
    close(sv[1]);
    return conn_new(sv[0], "test", 5000);
}

static int http_case(const char *resp, size_t max, HttpResp *r) {
    Conn *c = serve(resp, strlen(resp), NULL, 0);
    int rc = http_request(c, "GET", "test", "/", NULL, NULL, 0, max, 5000, r);
    conn_close(c);
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    return rc;
}

int main(void) {
    alarm(60);
    signal(SIGPIPE, SIG_IGN);
    unsigned long long v;
    CHECK(http_chunk_size("1a\r\n", &v) == 0 && v == 26);
    CHECK(http_chunk_size("0\r\n", &v) == 0 && v == 0);
    CHECK(http_chunk_size("10;name=val\r\n", &v) == 0 && v == 16);
    CHECK(http_chunk_size("ffffffffffffffff\r\n", &v) == 0 && v == 0xffffffffffffffffull);   /* parsed, then rejected by size */
    CHECK(http_chunk_size("1ffffffffffffffff\r\n", &v) == -1);       /* 17 digits: ERANGE */
    CHECK(http_chunk_size("-1\r\n", &v) == -1);
    CHECK(http_chunk_size("zz\r\n", &v) == -1);
    CHECK(http_chunk_size("12 garbage\r\n", &v) == -1);
    CHECK(http_chunk_size("\r\n", &v) == -1);
    CHECK(http_content_length("123", &v) == 0 && v == 123);
    CHECK(http_content_length(" 42 ", &v) == 0 && v == 42);
    CHECK(http_content_length("-5", &v) == -1);
    CHECK(http_content_length("99999999999999999999999", &v) == -1);
    CHECK(http_content_length("12abc", &v) == -1);
    CHECK(http_content_length("", &v) == -1);

    HttpResp r;
    CHECK(http_case("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", 1 << 20, &r) == 0 && r.blen == 11 && !strcmp(r.body, "hello world"));
    http_resp_free(&r);
    CHECK(http_case("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nffffffffffffffff\r\nxx", 1 << 20, &r) == -2);
    CHECK(http_case("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nfffffffffffffff0\r\nxx", 1 << 20, &r) == -2);
    CHECK(http_case("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\nfffffffffffffffb\r\nx", 1 << 20, &r) == -2);
    CHECK(http_case("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5 junk\r\nhello\r\n0\r\n\r\n", 1 << 20, &r) == -1);
    CHECK(http_case("HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551615\r\n\r\nx", 1 << 20, &r) == -1);
    CHECK(http_case("HTTP/1.1 200 OK\r\nContent-Length: 9223372036854775807\r\n\r\nx", 1 << 20, &r) == -2);
    CHECK(http_case("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\nx", 1 << 20, &r) == -1);
    CHECK(http_case("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nabcd", 1 << 20, &r) == 0 && r.blen == 4);
    http_resp_free(&r);
    CHECK(http_case("HTTP/1.1 200 OK\r\nContent-Length: 400\r\n\r\nabcd", 100, &r) == -2);
    /* endless headers and an endless header line */
    {
        Conn *c = serve("HTTP/1.1 200 OK\r\n", 17, "X-A: b\r\n", 100000);
        CHECK(http_request(c, "GET", "test", "/", NULL, NULL, 0, 1 << 20, 5000, &r) == -1);
        conn_close(c);
        c = serve("HTTP/1.1 200 OK\r\nX-Long: ", 25, "aaaaaaaaaaaaaaaa", 100000);
        CHECK(http_request(c, "GET", "test", "/", NULL, NULL, 0, 1 << 20, 5000, &r) == -1);
        conn_close(c);
        c = serve("HTTP/1.1 200 OK\r\n\r\n", 19, "0123456789abcdef", 100000);    /* body until close, over the cap */
        CHECK(http_request(c, "GET", "test", "/", NULL, NULL, 0, 4096, 5000, &r) == -2);
        conn_close(c);
    }
    /* IMAP literals */
    CHECK(imap_literal_size("* 1 FETCH (BODY[] {12}\r\n", 24) == 12);
    CHECK(imap_literal_size("* 1 FETCH (BODY[] {12+}\r\n", 25) == 12);
    CHECK(imap_literal_size("* OK done\r\n", 11) == -1);
    CHECK(imap_literal_size("a {}\r\n", 6) == -1);
    CHECK(imap_literal_size("a {18446744073709551615}\r\n", 26) == -2);
    CHECK(imap_literal_size("a {99999999999}\r\n", 17) == 99999999999LL);
    char err[96];
    size_t len;
    const char *ok = "* 1 FETCH (BODY[] {5}\r\nhello)\r\n";
    Conn *c = serve(ok, strlen(ok), NULL, 0);
    char *resp = imap_read_response(c, &len, 1 << 20, 1 << 20, err, sizeof err);
    CHECK(resp && len == strlen(ok) && !memcmp(resp, ok, len) && resp[len] == 0);
    free(resp);
    conn_close(c);
    static const char *bad[] = {
        "* 1 FETCH (BODY[] {18446744073709551615}\r\nx",
        "* 1 FETCH (BODY[] {18446744073709551614}\r\nx",
        "* 1 FETCH (BODY[] {4294967296}\r\nx",
        "* 1 FETCH (BODY[] {2000000}\r\nshort",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        c = serve(bad[i], strlen(bad[i]), NULL, 0);
        resp = imap_read_response(c, &len, 1 << 20, 1 << 20, err, sizeof err);
        CHECK(resp == NULL);
        free(resp);
        conn_close(c);
    }
    /* literals of every size around the buffer growth boundaries (off-by-one hunting under ASan) */
    for (int n = 0; n < 1100; n += (n < 70 ? 1 : 37)) {
        char head[64];
        int hl = snprintf(head, sizeof head, "* 1 FETCH (BODY[] {%d}\r\n", n);
        char *msg = malloc((size_t)hl + (size_t)n + 4);
        memcpy(msg, head, (size_t)hl);
        memset(msg + hl, 'x', (size_t)n);
        memcpy(msg + hl + n, ")\r\n", 3);
        c = serve(msg, (size_t)hl + (size_t)n + 3, NULL, 0);
        resp = imap_read_response(c, &len, 1 << 20, 1 << 20, err, sizeof err);
        CHECK(resp && len == (size_t)hl + (size_t)n + 3 && resp[len] == 0);
        free(resp);
        conn_close(c);
        free(msg);
    }
    /* a line that never ends */
    c = serve("* OK ", 5, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 100000);
    resp = imap_read_response(c, &len, 65536, 1 << 20, err, sizeof err);
    CHECK(resp == NULL);
    conn_close(c);
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    printf("mailnet-test: %d/%d checks ok\n", checks - fails, checks);
    return fails ? 1 : 0;
}
