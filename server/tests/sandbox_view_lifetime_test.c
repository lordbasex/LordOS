/* sandbox_view_lifetime_test.c — how long the launcher's view lives (sandbox_files_isolate's PID 1, docs/FLATPAK.md
 * "Sub-sandboxes"). Before 2026-11-22 PID 1 left as soon as the app's first process did, and the kernel then killed
 * everything else in the view: VS Code's Flatpak (its `code` runs cli.js, which starts Electron detached and leaves)
 * opened a window and vanished. Now:
 *   1. a process the app left behind runs to its end, and PID 1 leaves after it with the app's status;
 *   2. once the app is gone and only its Spawn helper is left (sandbox_view_helper), PID 1 ends the helper and leaves;
 *   3. a SIGTERM to PID 1 after the app is gone reaches what it left behind;
 *   4. a SIGTERM to PID 1 while the app runs: the app gets it and, once it leaves, PID 1 leaves at once (the rest goes
 *      with the view, as before: stopping an app never waits for what it started).
 * Plus the children-list parser (sandbox_view_others_in). Needs unprivileged user namespaces (exit 77 without). */
#include "../src/sec/sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* The launcher's side: the view's first process forks PID 1 and _exits; PID 1 is reparented to us only if we are a
 * subreaper, so the test makes itself one and waits for PID 1 (any child but the first). */
static pid_t start_view(const char *home, int mode, int rep) {
    pid_t t = fork();
    if (t != 0) return t;
    SandboxFilesCfg cfg = { .home = home, .keep_home = 1 };
    if (sandbox_files_isolate(&cfg)) { if (write(rep, "S", 1) < 0) {} _exit(0); }
    /* the app, PID 2 of the view */
    if (write(rep, "V", 1) < 0) {}
    if (mode == 1) {                        /* leaves a process behind that writes "B" after 1.5 s */
        sandbox_view_helper(0);
        if (fork() == 0) { struct timespec d = { 1, 500000000 }; nanosleep(&d, NULL); if (write(rep, "B", 1) < 0) {} _exit(0); }
        _exit(7);
    }
    if (mode == 2) {                        /* a "helper" that would wait forever; the app leaves */
        pid_t h = fork();
        if (h == 0) { for (;;) pause(); }
        sandbox_view_helper(h);
        _exit(5);
    }
    sandbox_view_helper(0);
    if (mode == 4) {                        /* runs until a SIGTERM, with a child that ignores SIGTERM */
        if (fork() == 0) { signal(SIGTERM, SIG_IGN); for (;;) pause(); }
        for (;;) pause();
    }
    /* mode 3: leaves a process that waits; PID 1 gets a SIGTERM later */
    if (fork() == 0) { for (;;) pause(); }
    _exit(3);
}

int main(void) {
    char base[4096];                        /* not under /tmp: the view mounts its own there */
    if (!getcwd(base, sizeof base - 40)) return 1;
    strcat(base, "/build/sbx-life-test.XXXXXX");
    if (!mkdtemp(base)) { perror("mkdtemp"); return 1; }

    /* the parser */
    CHECK(sandbox_view_others_in("", 5) == 0, "an empty list has no other child");
    CHECK(sandbox_view_others_in("5 ", 5) == 0, "only the helper is no other child");
    CHECK(sandbox_view_others_in("5 9 ", 5) == 1, "a second child is another child");
    CHECK(sandbox_view_others_in("9\n", 0) == 1, "any child without a helper");
    CHECK(sandbox_view_others_in("x", 5) == 1, "an unreadable list counts as others (wait)");
    CHECK(sandbox_view_others_in(NULL, 5) == 1, "no list counts as others (wait)");

    if (prctl(PR_SET_CHILD_SUBREAPER, 1)) { printf("sandbox-view-lifetime-test: could not become a subreaper\n"); return 1; }
    for (int mode = 1; mode <= 4; mode++) {
        int sent = 0;
        int p[2];
        if (pipe(p)) return 1;
        double t0 = now_s();
        pid_t t = start_view(base, mode, p[1]);
        close(p[1]);
        int st;
        waitpid(t, &st, 0);                 /* the first process: it leaves once PID 1 is up */
        char c = 0;
        if (read(p[0], &c, 1) != 1 || c == 'S') {
            printf("sandbox-view-lifetime-test: skipped: needs unprivileged user namespaces\n");
            rmdir(base);
            return 77;
        }
        if (mode >= 3) {                    /* PID 1 is our child now: SIGTERM it (in 3, after the app is gone) */
            struct timespec d = { 0, 600000000 };
            nanosleep(&d, NULL);
        }
        pid_t init = -1;
        double limit = t0 + 8;
        for (;;) {                          /* find PID 1 (reparented to us) and, in mode 3, signal it */
            pid_t w = waitpid(-1, &st, WNOHANG);
            if (w > 0) { init = w; break; }
            if (mode >= 3 && !sent) {       /* our one child now is the view's PID 1 */
                char path[64], buf[256] = "";
                snprintf(path, sizeof path, "/proc/%d/task/%d/children", (int)getpid(), (int)getpid());
                int fd = open(path, O_RDONLY | O_CLOEXEC);
                if (fd >= 0) { ssize_t k = read(fd, buf, sizeof buf - 1); buf[k > 0 ? k : 0] = 0; close(fd); }
                pid_t pid1 = (pid_t)atoi(buf);
                if (pid1 > 0) { kill(pid1, SIGTERM); sent = 1; }
            }
            if (now_s() > limit) break;
            struct timespec d = { 0, 20000000 };
            nanosleep(&d, NULL);
        }
        double dt = now_s() - t0;
        char rest[8] = "";
        ssize_t n = read(p[0], rest, sizeof rest - 1);
        rest[n > 0 ? n : 0] = 0;
        close(p[0]);
        if (mode == 1) {
            CHECK(init > 0, "1: PID 1 left");
            CHECK(strchr(rest, 'B') != NULL, "1: the process the app left behind ran to its end");
            CHECK(init > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 7, "1: PID 1 left with the app's status");
            CHECK(dt >= 1.4, "1: PID 1 waited for what the app left behind");
        } else if (mode == 2) {
            CHECK(init > 0, "2: PID 1 left with only the helper there (it ended it)");
            CHECK(init > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 5, "2: PID 1 left with the app's status");
            CHECK(dt < 4, "2: PID 1 didn't wait for the helper");
        } else if (mode == 4) {
            CHECK(init > 0 && dt < 4, "4: stopped: PID 1 left once the app did, without waiting for the rest");
            CHECK(init > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 128 + SIGTERM, "4: PID 1 left with the app's status (SIGTERM)");
        } else {
            CHECK(init > 0, "3: a SIGTERM to PID 1 ended what the app left behind, and PID 1 left");
            CHECK(init > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 3, "3: PID 1 left with the app's status");
        }
    }
    rmdir(base);
    printf("sandbox-view-lifetime-test: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
