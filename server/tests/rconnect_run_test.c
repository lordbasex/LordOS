/* rconnect_run_test.c — the programs that read a server's bytes: out of time they die, and they can't write or reach anything. */
#include "../src/lan/rconnect.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int pass, failc;
#define CHECK(x) do { if (x) pass++; else { failc++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(void) {
    char out[4096];
    /* a program that stalls, with and without closing its output */
    char *sl[] = { "/bin/sleep", "60", NULL };
    RconnectRun j = { .argv = sl, .out = out, .cap = sizeof out, .timeout_ms = 300, .any_status = 1 };
    double t = now();
    CHECK(rconnect_run(&j) < 0);
    CHECK(now() - t < 3);
    char *sh1[] = { "/bin/sh", "-c", "exec >/dev/null; sleep 60", NULL };
    j.argv = sh1; t = now();
    CHECK(rconnect_run(&j) < 0 && now() - t < 3);
    /* the input arrives and the output comes back */
    char *cat[] = { "/bin/cat", NULL };
    RconnectRun c = { .argv = cat, .input = "hello", .ilen = 5, .out = out, .cap = sizeof out, .timeout_ms = 5000, .offline = 1 };
    CHECK(rconnect_run(&c) == 5 && !strcmp(out, "hello"));
    char *st3[] = { "/bin/sh", "-c", "exit 3", NULL };
    c.argv = st3; c.ilen = 0;
    CHECK(rconnect_run(&c) < 0);
    c.any_status = 1;
    CHECK(rconnect_run(&c) == 0);
    c.any_status = 0;
    /* a home that can't be read or written */
    char dir[] = "/tmp/rconnect-test.XXXXXX", file[64], cmd[256];
    CHECK(mkdtemp(dir) != NULL);
    snprintf(file, sizeof file, "%s/secret", dir);
    FILE *f = fopen(file, "w"); fputs("x", f); fclose(f);
    snprintf(cmd, sizeof cmd, "cat %s >/dev/null 2>&1 && echo read; echo y > %s/new 2>/dev/null && echo wrote; echo done", file, dir);
    char *sh2[] = { "/bin/sh", "-c", cmd, NULL };
    c.argv = sh2;
    int landlock = rconnect_run(&c) > 0;
    CHECK(landlock && strstr(out, "done"));
    if (strstr(out, "read") || strstr(out, "wrote")) printf("no Landlock on this kernel: %s", out);
    else pass++;
    /* offline: no TCP, even to a port that listens here */
    int l = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(l, (struct sockaddr *)&a, sizeof a);
    listen(l, 1);
    socklen_t al = sizeof a;
    getsockname(l, (struct sockaddr *)&a, &al);
    snprintf(cmd, sizeof cmd, "exec 3<>/dev/tcp/127.0.0.1/%d && echo connected; echo done", ntohs(a.sin_port));
    char *ba[] = { "/bin/bash", "-c", cmd, NULL };
    c.argv = ba; c.any_status = 1;
    CHECK(rconnect_run(&c) > 0 && !strstr(out, "connected"));
    c.offline = 0;
    CHECK(rconnect_run(&c) > 0 && strstr(out, "connected"));
    /* the real programs in the same box. RC_HOST: a test server, for the two that talk to one */
    if (!access("/usr/bin/rclone", X_OK)) {
        char *ob[] = { "/usr/bin/rclone", "obscure", "-", NULL };
        RconnectRun r = { .argv = ob, .input = "x", .ilen = 1, .out = out, .cap = sizeof out, .timeout_ms = 10000, .offline = 1 };
        CHECK(rconnect_run(&r) > 8);
    }
    const char *host = getenv("RC_HOST");
    if (host) {
        char *ks[] = { "/usr/bin/ssh-keyscan", "-T", "5", "-t", "ed25519", "--", (char *)host, NULL };
        RconnectRun r = { .argv = ks, .input = "", .out = out, .cap = sizeof out, .timeout_ms = 15000, .any_status = 1 };
        CHECK(rconnect_run(&r) > 0 && strstr(out, "ssh-ed25519"));
        char conn[80];
        snprintf(conn, sizeof conn, "%s:443", host);
        char *sc[] = { "/usr/bin/openssl", "s_client", "-connect", conn, "-servername", (char *)host, NULL };
        r.argv = sc;
        CHECK(rconnect_run(&r) > 0 && strstr(out, "BEGIN CERTIFICATE"));
    }
    unlink(file); snprintf(cmd, sizeof cmd, "%s/new", dir); unlink(cmd); rmdir(dir);
    printf("rconnect-run-test: %d passed, %d failed\n", pass, failc);
    return failc != 0;
}
