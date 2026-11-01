/* rconnect_run.c — see rconnect.h. A program that reads what a server sent, or a secret, runs in its scope, with the system
 * read-only (sandbox_exec_child), and is killed at the deadline. */
#include "rconnect.h"
#include "../sec/cgroup.h"
#include "../sec/sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int rconnect_run(const RconnectRun *j) {
    int a[2], b[2];
    if (pipe2(a, O_CLOEXEC)) return -1;
    if (pipe2(b, O_CLOEXEC)) { close(a[0]); close(a[1]); return -1; }
    pid_t p = fork();
    if (!p) {
        dup2(a[0], 0); dup2(b[1], 1);
        syscall(SYS_close_range, 3, ~0u, 0);
        sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, NULL);
        if (app_scope_enter(RCONNECT_SCOPE) && getenv("LORDOS_SESSIOND")) _exit(126);
        sandbox_exec_child(j->offline ? 0 : 1);
        execve(j->argv[0], j->argv, environ);
        _exit(127);
    }
    close(a[0]); close(b[1]);
    if (p < 0) { close(a[1]); close(b[0]); return -1; }
    int w = !j->ilen || write(a[1], j->input, j->ilen) == (ssize_t)j->ilen ? 0 : -1;   /* far under a pipe's size */
    close(a[1]);
    long long end = now_ms() + j->timeout_ms;
    size_t o = 0;
    int timed = 0;
    for (;;) {
        long long left = end - now_ms();
        struct pollfd f = { b[0], POLLIN, 0 };
        int r = left > 0 ? poll(&f, 1, (int)left) : 0;
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { timed = 1; break; }
        ssize_t n = read(b[0], j->out + o, j->cap - 1 - o);
        if (n <= 0) break;
        o += (size_t)n;
        if (o >= j->cap - 1) break;
    }
    close(b[0]);
    j->out[o] = 0;
    /* its stdout is closed: it has what's left of the deadline to end */
    int st = 0;
    for (;;) {
        pid_t g = waitpid(p, &st, WNOHANG);
        if (g == p || (g < 0 && errno != EINTR)) break;
        if (timed || now_ms() >= end) { timed = 1; kill(p, SIGKILL); waitpid(p, &st, 0); break; }
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (timed || w || !WIFEXITED(st) || (WEXITSTATUS(st) && !j->any_status)) return -1;
    return (int)o;
}
