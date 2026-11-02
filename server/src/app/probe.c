/* probe.c — System Information's sandbox probe (Checks, part 2; docs/BENCHMARK.md, "Checks:
 * configured versus effective"). Runs only in app mode for the system package `sysprobe`
 * (assets/sysapps/sysprobe, found by app_package_dir as a system app), after its sandbox is applied
 * exactly as for any isolated app, and before any app code would load. It tries what must fail and
 * sends what happened to lord-server over its own broker (BOP_PROBE), then exits. Nothing here
 * widens anything: every try is something the sandbox or the broker must refuse, plus two controls
 * that must work (its own data folder) so a refusal isn't mistaken for a broken probe.
 *
 *   kernel: the home folder, a system file, the root cgroup's procs, an internet and a unix
 *           socket, ptrace, a user namespace, mount, exec;
 *   helper: a child forked before the app's sandbox takes the decoding helpers' profile
 *           (sandbox_apply_helper) and tries a socket, exec, fork and the app's data folder;
 *   broker: a path outside its data folder, the clipboard without permission, the FIDO2 device,
 *           lord-maild.
 * lord-server adds what only it can read: the process's NoNewPrivs, Seccomp and capabilities and its
 * cgroup (brokersrv.c). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "probe.h"
#include "../sec/broker.h"
#include "../sec/sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct { char *p; size_t n, cap; int items; } Out;

static void raw(Out *o, const char *s) {
    size_t k = strlen(s);
    if (o->n + k + 1 < o->cap) { memcpy(o->p + o->n, s, k + 1); o->n += k; }
}
/* One item of the "tries" list (or several already joined with commas). */
static void item(Out *o, const char *json) {
    if (o->items++) raw(o, ",");
    raw(o, json);
}
static void put(Out *o, const char *id, int denied, int err) {
    char j[96];
    snprintf(j, sizeof j, "{\"id\":\"%s\",\"denied\":%s,\"errno\":%d}", id, denied ? "true" : "false", err);
    item(o, j);
}

/* A call that must fail: denied when it returned < 0 (errno kept). _FD: a call that returns an fd,
 * closed if it unexpectedly worked. */
#define MUST_FAIL(o, id, call) do { errno = 0; long r_ = (long)(call); put(o, id, r_ < 0, r_ < 0 ? errno : 0); } while (0)
#define MUST_FAIL_FD(o, id, call) do { errno = 0; int r_ = (call); int e_ = errno; put(o, id, r_ < 0, r_ < 0 ? e_ : 0); if (r_ >= 0) close(r_); } while (0)

static void kernel_tries(Out *o, const char *home, const char *data) {
    MUST_FAIL_FD(o, "fsHome", open(home, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    MUST_FAIL_FD(o, "fsSystem", open("/etc/passwd", O_RDONLY | O_CLOEXEC));
    MUST_FAIL_FD(o, "cgroupEscape", open("/sys/fs/cgroup/cgroup.procs", O_WRONLY | O_CLOEXEC));
    MUST_FAIL_FD(o, "netInet", socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    MUST_FAIL_FD(o, "netUnix", socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    MUST_FAIL(o, "ptrace", ptrace(PTRACE_ATTACH, 1, NULL, NULL));
    MUST_FAIL(o, "userns", unshare(CLONE_NEWUSER));
    MUST_FAIL(o, "mount", mount("none", "/tmp", "tmpfs", 0, NULL));
    char *const argv[] = { (char *)"/usr/bin/true", NULL };
    char *const envp[] = { NULL };
    MUST_FAIL(o, "exec", execve("/usr/bin/true", argv, envp));      /* returns only if refused */
    /* the control: its own data folder works */
    char f[4096];
    snprintf(f, sizeof f, "%s/.probe", data);
    int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int ok = fd >= 0 && write(fd, "ok", 2) == 2;
    if (fd >= 0) close(fd);
    unlink(f);
    put(o, "ownData", !ok, ok ? 0 : errno);
}

/* The decoding helpers' profile (sandbox_apply_helper, as lord-doc and lord-media take it), in a child
 * forked BEFORE the app's sandbox: the app's seccomp rightly refuses a second filter, and at that point
 * the process is single-threaded and has loaded nothing. The child writes its items, joined with
 * commas, to the pipe; probe_main reads them after the app's own tries. */
int probe_helper_start(const char *data, int *child) {
    int pp[2];
    *child = -1;
    if (pipe2(pp, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(pp[0]);
        SandboxHelperCfg cfg = { .name = "lordos-sysprobe-helper" };
        sandbox_apply_helper(&cfg);          /* exits if seccomp can't be applied (fails closed) */
        char buf[1024] = "";
        Out c = { buf, 0, sizeof buf, 0 };
        MUST_FAIL_FD(&c, "helperSocket", socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        char *const argv[] = { (char *)"/usr/bin/true", NULL };
        char *const envp[] = { NULL };
        MUST_FAIL(&c, "helperExec", execve("/usr/bin/true", argv, envp));
        pid_t g = fork();
        if (g == 0) _exit(0);
        if (g > 0) waitpid(g, NULL, 0);
        put(&c, "helperFork", g < 0, g < 0 ? errno : 0);
        char f[4096];
        snprintf(f, sizeof f, "%s/.probe-helper", data);
        MUST_FAIL_FD(&c, "helperData", open(f, O_WRONLY | O_CREAT | O_CLOEXEC, 0600));
        if (write(pp[1], buf, strlen(buf)) < 0) _exit(1);
        _exit(0);
    }
    close(pp[1]);
    if (pid < 0) { close(pp[0]); return -1; }
    *child = pid;
    return pp[0];
}

static void helper_collect(Out *o, int fd, int child) {
    char buf[1024];
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    if (child > 0) waitpid(child, NULL, 0);
    if (n > 0) { buf[n] = 0; item(o, buf); }
    else put(o, "helperRan", 1, 0);
}

/* One synchronous broker request; the reply's status, or -1. */
static int ask(int bfd, uint16_t op, const void *payload, uint32_t len) {
    static uint32_t req = 1;
    BrokerHdr h = { .reqid = req++, .op = op, .status = BROKER_REQ, .len = len };
    struct iovec iov[2] = { { &h, sizeof h }, { (void *)payload, len } };
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = len ? 2 : 1 };
    if (sendmsg(bfd, &m, MSG_NOSIGNAL) < 0) return -1;
    static char buf[65536];
    for (int tries = 0; tries < 64; tries++) {
        BrokerHdr r;
        struct iovec riov[2] = { { &r, sizeof r }, { buf, sizeof buf } };
        struct msghdr rm = { .msg_iov = riov, .msg_iovlen = 2 };
        ssize_t n = recvmsg(bfd, &rm, 0);
        if (n < (ssize_t)sizeof r) return -1;
        if (r.reqid == h.reqid) return r.status;     /* events (reqid 0) are skipped */
    }
    return -1;
}

static void broker_tries(Out *o, int bfd) {
    static const char esc[] = "../../.bashrc";
    int s = ask(bfd, BOP_FS_READ, esc, sizeof esc - 1);
    put(o, "brokerFsEscape", s == BST_DENIED, 0);
    s = ask(bfd, BOP_CLIP_GET, NULL, 0);
    put(o, "brokerClipboard", s == BST_DENIED, 0);
    s = ask(bfd, BOP_VAULT_FIDO, NULL, 0);
    put(o, "brokerFido", s == BST_DENIED, 0);
    static const char mail[] = "list\0";
    s = ask(bfd, BOP_MAIL, mail, sizeof mail - 1);
    put(o, "brokerMail", s == BST_DENIED || s == BST_ERROR, 0);
}

int probe_main(const char *home, const char *data, int sandbox_mask, int helper_fd, int helper_pid) {
    const char *s = getenv(BROKER_FD_ENV);
    int bfd = s ? atoi(s) : -1;
    if (bfd < 0) return 1;
    static char buf[8192];
    Out o = { buf, 0, sizeof buf, 0 };
    char head[96];
    snprintf(head, sizeof head, "{\"v\":1,\"sandbox\":%d,\"tries\":[", sandbox_mask);
    raw(&o, head);
    kernel_tries(&o, home, data);
    helper_collect(&o, helper_fd, helper_pid);
    broker_tries(&o, bfd);
    raw(&o, "]}");
    int st = ask(bfd, BOP_PROBE, buf, (uint32_t)o.n);
    return st == BST_OK ? 0 : 1;
}
