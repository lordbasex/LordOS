/* http_resume_test.c — resumed downloads in the Store's downloader (src/pkg/http.c, FetchItem.resume), as Machines'
 * vm-image-get uses them: a cut download keeps dest.part, the next try asks for the rest with a Range request and the
 * SHA-256 is over the whole file; a server that ignores Range, answers the wrong range or 416 starts again cleanly;
 * without resume a cut leaves nothing. Against a small HTTP server on loopback in a child process (no network).
 * Built with -DLORD_HTTP_TEST so the download child runs as the user who runs the test.
 *
 *   build/http-resume-test */
#include "../src/pkg/http.h"
#include "../src/pkg/crypto.h"

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
#include <unistd.h>

static int pass, fail;
#define T(name, cond) do { if (cond) pass++; else { fail++; printf("FAIL  %s\n", name); } } while (0)

#define SIZE (3u << 20)
static unsigned char data[SIZE];

enum { M_RANGE, M_IGNORE, M_BADRANGE, M_416 };
static int port;

/* one server per case: mode says how it answers a Range request; cut > 0 closes the connection after that many body
 * bytes of the first answer */
static pid_t serve(int mode, size_t cut) {
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(ls, (struct sockaddr *)&a, sizeof a);
    socklen_t al = sizeof a;
    getsockname(ls, (struct sockaddr *)&a, &al);
    port = ntohs(a.sin_port);
    listen(ls, 8);
    pid_t pid = fork();
    if (pid) { close(ls); return pid; }
    signal(SIGPIPE, SIG_IGN);
    for (int answers = 0;; answers++) {
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
        unsigned long long from = 0;
        const char *rg = strcasestr(req, "\r\nRange: bytes=");
        int ranged = rg && sscanf(rg + 15, "%llu-", &from) == 1;
        char h[256];
        size_t off = 0, len = SIZE;
        if (ranged && mode == M_RANGE && from < SIZE) {
            off = from; len = SIZE - from;
            snprintf(h, sizeof h, "HTTP/1.1 206 Partial Content\r\nContent-Length: %zu\r\nContent-Range: bytes %zu-%u/%u\r\nConnection: close\r\n\r\n",
                     len, off, SIZE - 1, SIZE);
        } else if (ranged && mode == M_BADRANGE) {
            off = from / 2; len = SIZE - off;   /* not where it was asked */
            snprintf(h, sizeof h, "HTTP/1.1 206 Partial Content\r\nContent-Length: %zu\r\nContent-Range: bytes %zu-%u/%u\r\nConnection: close\r\n\r\n",
                     len, off, SIZE - 1, SIZE);
        } else if (ranged && mode == M_416) {
            snprintf(h, sizeof h, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nContent-Range: bytes */%u\r\n\r\n", SIZE);
            len = 0;
        } else snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Length: %u\r\nConnection: close\r\n\r\n", SIZE);
        if (write(c, h, strlen(h)) < 0) { close(c); continue; }
        size_t send = len;
        if (cut && !answers && cut < send) send = cut;
        for (size_t o = 0; o < send;) {
            ssize_t w = write(c, data + off + o, send - o > 65536 ? 65536 : send - o);
            if (w <= 0) break;
            o += (size_t)w;
        }
        close(c);
    }
}

static void stop(pid_t p) { kill(p, SIGKILL); waitpid(p, NULL, 0); }

static char dir[256], dest[300], part[310];
static uint8_t want[32];

static void fetch(FetchItem *it, int resume) {
    memset(it, 0, sizeof *it);
    snprintf(it->url, sizeof it->url, "http://127.0.0.1:%d/image.iso", port);
    snprintf(it->dest, sizeof it->dest, "%s", dest);
    it->max = SIZE + 1;
    it->resume = resume;
    fetch_batch(it, 1, NULL, NULL);
}

static long long size_of(const char *p) { struct stat st; return stat(p, &st) ? -1 : (long long)st.st_size; }
static int whole_ok(const FetchItem *it) {
    if (it->status != 200 || !!memcmp(it->sha256, want, 32) || size_of(dest) != SIZE || size_of(part) != -1) return 0;
    FILE *f = fopen(dest, "rb");
    static unsigned char got[SIZE];
    size_t n = f ? fread(got, 1, SIZE, f) : 0;
    if (f) fclose(f);
    return n == SIZE && !memcmp(got, data, SIZE);
}
/* a cut download of the first `at` bytes, kept as the part */
static long long cut_at(size_t at) {
    unlink(dest); unlink(part);
    pid_t s = serve(M_RANGE, at);
    FetchItem it;
    fetch(&it, 1);
    stop(s);
    return it.status == 200 ? -2 : size_of(part);
}

int main(void) {
    for (size_t i = 0; i < SIZE; i++) data[i] = (unsigned char)(i * 2654435761u >> 13);
    Sha256 h;
    sha256_init(&h); sha256_update(&h, data, SIZE); sha256_final(&h, want);
    snprintf(dir, sizeof dir, "/tmp/http-resume-%d", (int)getpid());
    mkdir(dir, 0700);
    snprintf(dest, sizeof dest, "%s/x.iso", dir);
    snprintf(part, sizeof part, "%s.part", dest);
    FetchItem it;

    /* a whole download, nothing kept from before */
    pid_t s = serve(M_RANGE, 0);
    fetch(&it, 1);
    stop(s);
    T("whole: saved, hash over the file", whole_ok(&it) && it.resumed == 0);

    /* cut, then the rest */
    long long kept = cut_at(1200000);
    T("cut: the part is kept", kept > 0 && kept <= 1200000 && size_of(dest) == -1);
    s = serve(M_RANGE, 0);
    fetch(&it, 1);
    stop(s);
    T("resumed: from the part", (long long)it.resumed == kept && kept > 0);
    T("resumed: whole file, hash over all of it, no part left", whole_ok(&it));

    /* cut twice, then the rest */
    kept = cut_at(500000);
    s = serve(M_RANGE, 700000);
    fetch(&it, 1);
    stop(s);
    long long kept2 = size_of(part);
    T("cut again: the part grew", it.status != 200 && kept2 > kept);
    s = serve(M_RANGE, 0);
    fetch(&it, 1);
    stop(s);
    T("second resume: whole and right", (long long)it.resumed == kept2 && whole_ok(&it));

    /* a server that ignores Range: from the start, cleanly */
    kept = cut_at(900000);
    s = serve(M_IGNORE, 0);
    fetch(&it, 1);
    stop(s);
    T("Range ignored: from the start", kept > 0 && it.resumed == 0 && whole_ok(&it));

    /* the wrong range: refused, asked again without one */
    kept = cut_at(900000);
    s = serve(M_BADRANGE, 0);
    fetch(&it, 1);
    stop(s);
    T("wrong range: from the start", kept > 0 && it.resumed == 0 && whole_ok(&it));

    /* 416: again without a range */
    kept = cut_at(900000);
    s = serve(M_416, 0);
    fetch(&it, 1);
    stop(s);
    T("416: from the start", kept > 0 && it.resumed == 0 && whole_ok(&it));

    /* a part that was changed: resumed, but the hash over the whole file isn't the file's */
    kept = cut_at(900000);
    FILE *f = fopen(part, "r+b");
    if (f) { fseek(f, 1000, SEEK_SET); fputc(data[1000] ^ 0xff, f); fclose(f); }
    s = serve(M_RANGE, 0);
    fetch(&it, 1);
    stop(s);
    T("changed part: the hash says so", it.status == 200 && it.resumed > 0 && memcmp(it.sha256, want, 32));

    /* no resume (the Store): a cut leaves nothing, and an old part isn't used */
    unlink(dest); unlink(part);
    s = serve(M_RANGE, 800000);
    fetch(&it, 0);
    stop(s);
    T("no resume: a cut leaves no part", it.status != 200 && size_of(part) == -1 && size_of(dest) == -1);
    kept = cut_at(800000);
    s = serve(M_RANGE, 0);
    fetch(&it, 0);
    stop(s);
    T("no resume: an old part isn't continued", kept > 0 && it.resumed == 0 && whole_ok(&it));

    /* a part as big as the most the item may be isn't continued */
    unlink(dest);
    f = fopen(part, "wb");
    if (f) { fwrite(data, 1, SIZE, f); fputc(0, f); fputc(0, f); fclose(f); }
    s = serve(M_RANGE, 0);
    fetch(&it, 1);
    stop(s);
    T("oversized part: from the start", it.resumed == 0 && whole_ok(&it));

    /* a symbolic link in place of the part isn't followed */
    unlink(dest); unlink(part);
    char target[320];
    snprintf(target, sizeof target, "%s/elsewhere", dir);
    f = fopen(target, "wb");
    if (f) { fwrite(data, 1, 1000, f); fclose(f); }
    if (symlink(target, part)) perror("symlink");
    s = serve(M_RANGE, 0);
    fetch(&it, 1);
    stop(s);
    T("symlinked part: not continued, not written through", it.resumed == 0 && whole_ok(&it) && size_of(target) == 1000);

    unlink(dest); unlink(part); unlink(target); rmdir(dir);
    printf("== http-resume-test: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
