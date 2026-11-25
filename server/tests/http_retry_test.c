/* http_retry_test.c — the downloader's request line and its fresh redirects (src/pkg/http.c get_one), against a small
 * HTTP server on loopback in a child process (no network):
 *   - a '+' in the path goes as %2B (an S3 origin reads a bare '+' as a space), a '+' in the query as it came;
 *   - a signed URL an origin redirected to, refused with 403 or a 5xx, is asked for again from the original URL (a
 *     freshly signed redirect), at most twice more, after 0.5 s and 1.5 s; a refusal without a redirect, or a 404
 *     after one, is not;
 *   - the redirect limit still holds.
 * Built with -DLORD_HTTP_TEST so the download child runs as the user who runs the test.
 *
 *   build/http-retry-test */
#include "../src/pkg/http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int pass, fail;
#define T(name, cond) do { if (cond) pass++; else { fail++; printf("FAIL  %s\n", name); } } while (0)

static const char BODY[] = "4b2a... SHA256SUMS\n";
static int port;
static char dir[256], logp[300], dest[300];

/* The server: GET /orig/<x> → 302 to http://127.0.0.1:<port>/signed/<n>?rscd=attachment%3B+filename%3Dx (n counts
 * the redirects); /signed/… → `status` for the first `refuse` requests, then 200; /direct → `status` always;
 * /loop → 302 to /loop again; anything else → 200. Every request line goes to the log, one per line. */
static pid_t serve(int status, int refuse) {
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(ls, (struct sockaddr *)&a, sizeof a);
    socklen_t al = sizeof a;
    getsockname(ls, (struct sockaddr *)&a, &al);
    port = ntohs(a.sin_port);
    listen(ls, 8);
    unlink(logp);
    pid_t pid = fork();
    if (pid) { close(ls); return pid; }
    signal(SIGPIPE, SIG_IGN);
    int redirects = 0, signed_n = 0;
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) _exit(0);
        char req[4096];
        size_t n = 0;
        while (n < sizeof req - 1) {
            ssize_t r = read(c, req + n, sizeof req - 1 - n);
            if (r <= 0) break;
            n += (size_t)r;
            req[n] = 0;
            if (strstr(req, "\r\n\r\n")) break;
        }
        req[n] = 0;
        char *eol = strstr(req, "\r\n");
        if (eol) *eol = 0;
        FILE *l = fopen(logp, "a");
        if (l) { fprintf(l, "%s\n", req); fclose(l); }
        char h[512];
        const char *body = "";
        if (!strncmp(req, "GET /orig/", 10)) {
            snprintf(h, sizeof h, "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:%d/signed/%d?rscd=attachment%%3B+filename%%3Dx\r\n"
                     "Content-Length: 0\r\nConnection: close\r\n\r\n", port, redirects++);
        } else if (!strncmp(req, "GET /loop", 9)) {
            snprintf(h, sizeof h, "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        } else if ((!strncmp(req, "GET /signed/", 12) && signed_n++ < refuse) || !strncmp(req, "GET /direct", 11)) {
            body = "<Error><Code>AuthenticationFailed</Code></Error>";
            snprintf(h, sizeof h, "HTTP/1.1 %d Refused\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", status, strlen(body));
        } else {
            body = BODY;
            snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(body));
        }
        if (write(c, h, strlen(h)) < 0 || write(c, body, strlen(body)) < 0) {}
        close(c);
    }
}

static void stop(pid_t p) { kill(p, SIGKILL); waitpid(p, NULL, 0); }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double took;
static void fetch(FetchItem *it, const char *scheme, const char *path) {
    memset(it, 0, sizeof *it);
    snprintf(it->url, sizeof it->url, "%s://127.0.0.1:%d%s", scheme, port, path);
    snprintf(it->dest, sizeof it->dest, "%s", dest);
    it->max = 1 << 20;
    unlink(dest);
    double t0 = now_s();
    fetch_batch(it, 1, NULL, NULL);
    took = now_s() - t0;
}

/* how many logged request lines start with `prefix` */
static int count(const char *prefix) {
    FILE *l = fopen(logp, "r");
    if (!l) return 0;
    char line[4096];
    int k = 0;
    while (fgets(line, sizeof line, l)) if (!strncmp(line, prefix, strlen(prefix))) k++;
    fclose(l);
    return k;
}
static int logged(const char *s) {
    FILE *l = fopen(logp, "r");
    if (!l) return 0;
    char line[4096];
    int k = 0;
    while (fgets(line, sizeof line, l)) if (strstr(line, s)) k = 1;
    fclose(l);
    return k;
}
static int saved(const FetchItem *it) {
    struct stat st;
    return it->status == 200 && !stat(dest, &st) && st.st_size == (off_t)strlen(BODY);
}

int main(void) {
    snprintf(dir, sizeof dir, "/tmp/http-retry-%d", (int)getpid());
    mkdir(dir, 0700);
    snprintf(logp, sizeof logp, "%s/requests", dir);
    snprintf(dest, sizeof dest, "%s/SHA256SUMS", dir);
    FetchItem it;

    /* '+': %2B in the path, as it came in the query (and after a redirect too) */
    pid_t s = serve(403, 0);
    fetch(&it, "http", "/pool/k/kms++-utils/a+b.deb?x=c+d&y=%2B");
    T("plus: saved", saved(&it));
    T("plus: %2B in the path", logged("GET /pool/k/kms%2B%2B-utils/a%2Bb.deb?"));
    T("plus: the query as it came", logged("?x=c+d&y=%2B HTTP/1.1"));
    fetch(&it, "http", "/orig/x+y");
    T("plus: redirect followed", saved(&it) && count("GET /orig/x%2By ") == 1);
    T("plus: the signed URL's query as it came", logged("GET /signed/0?rscd=attachment%3B+filename%3Dx HTTP/1.1"));
    stop(s);

    /* a 403 after a redirect, twice: asked again from the original URL each time, then saved */
    s = serve(403, 2);
    fetch(&it, "http", "/orig/SHA256SUMS");
    T("403 twice: saved on the third chain", saved(&it) && it.err[0] == 0);
    T("403 twice: three requests to the original URL", count("GET /orig/SHA256SUMS ") == 3);
    T("403 twice: a fresh signed URL each time", logged("/signed/1?") && logged("/signed/2?"));
    T("403 twice: waited 0.5 s + 1.5 s", took >= 1.9 && took < 10);
    stop(s);

    /* a 503 after a redirect once */
    s = serve(503, 1);
    fetch(&it, "http", "/orig/SHA256SUMS.sig");
    T("503 once: saved", saved(&it) && count("GET /orig/SHA256SUMS.sig ") == 2);
    T("503 once: waited 0.5 s only", took >= 0.45 && took < 1.4);
    stop(s);

    /* always refused: bounded, the status reported, nothing saved */
    s = serve(403, 1000);
    fetch(&it, "http", "/orig/SHA256SUMS");
    T("403 always: fails with 403", it.status == 0 && strstr(it.err, "403") && access(dest, F_OK) != 0);
    T("403 always: one try and two more", count("GET /orig/") == 3 && count("GET /signed/") == 3);
    stop(s);

    /* a 404 after a redirect is an answer, not a refusal: not asked again */
    s = serve(404, 1000);
    fetch(&it, "http", "/orig/SHA256SUMS");
    T("404 after a redirect: not asked again", it.status == 0 && strstr(it.err, "404") && count("GET /orig/") == 1);
    stop(s);

    /* a refusal without a redirect: not asked again (an origin's own 403 is its answer) */
    s = serve(403, 0);
    fetch(&it, "http", "/direct");
    T("403 without a redirect: one request", it.status == 0 && count("GET /direct") == 1 && took < 0.4);
    stop(s);

    /* the redirect limit still holds (5 per chain), and a loop isn't a refusal: not started again */
    s = serve(403, 0);
    fetch(&it, "http", "/loop");
    T("redirect loop: stops after 5 redirects", it.status == 0 && count("GET /loop") == 6);
    stop(s);

    unlink(dest); unlink(logp); rmdir(dir);
    printf("== http-retry-test: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
