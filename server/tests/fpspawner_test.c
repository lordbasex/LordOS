/* fpspawner_test.c — a Flatpak app's Spawn helper (src/fpspawner.c): the header and payload checks, and the helper
 * itself over a real socket pair, the way lord-server talks to it (fpspawn.c). The helper is built here with
 * FPSW_EXEC set to this program, which then runs as the "flatpak" it would start and reports what it was given: the fds
 * at the numbers Chromium's and zypak's Spawn ask for (0-2 and their IPC sockets), --instance-id-fd's pipe, stdin on
 * /dev/null when not asked for, its own process group, and its exit status coming back as EXITED with the right seq. */
#include "../src/fpspawner.h"
#include "../src/fpak.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
#endif

static int pass, failc;
#define CHECK(x) do { if (x) pass++; else { failc++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

/* ---- the probe: this program as the started "flatpak" ---- */

/* argv: flatpak run <report fd> <fd>:<marker>... ; writes "<fd>=<what it read>" for each, then stdin, pgid, exits 42 */
static int probe(int argc, char **argv) {
    int rep = atoi(argv[2]);
    char out[1024];
    int n = 0;
    for (int i = 3; i < argc; i++) {
        int fd = atoi(argv[i]);
        char buf[32] = "";
        if (fd == 0) continue;
        ssize_t r = read(fd, buf, sizeof buf - 1);
        n += snprintf(out + n, sizeof out - (size_t)n, "%d=%s;", fd, r > 0 ? buf : "-");
    }
    struct stat a, b;
    int null0 = !fstat(0, &a) && !stat("/dev/null", &b) && a.st_rdev == b.st_rdev && S_ISCHR(a.st_mode);
    n += snprintf(out + n, sizeof out - (size_t)n, "null0=%d;pgid=%d;env=%s;", null0, getpgid(0) == getpid(), getenv("FPSW_T") ? getenv("FPSW_T") : "");
    /* nothing else open but what was placed (and the report pipe) */
    int extra = 0;
    for (int fd = 3; fd < 1024; fd++) {
        if (fcntl(fd, F_GETFD) < 0 || fd == rep) continue;
        int placed = 0;
        for (int i = 3; i < argc; i++) if (atoi(argv[i]) == fd) placed = 1;
        extra += !placed;
    }
    n += snprintf(out + n, sizeof out - (size_t)n, "extra=%d", extra);
    if (write(rep, out, (size_t)n) < 0) {}
    return 42;
}

/* ---- the lord-server side ---- */

static int send_spawn(int s, uint32_t seq, char *const argv[], char *const envp[], int nfd, const uint32_t *target, const int *src,
                      int inst_num, int inst_w, int seal) {
    static char buf[65536];
    long n = fpsw_pack(argv, envp, buf, sizeof buf);
    if (n < 0) return -1;
    int m = memfd_create("t", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (m < 0 || write(m, buf, (size_t)n) != n) return -1;
    if (seal) fcntl(m, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL);
    FpswMsg h;
    memset(&h, 0, sizeof h);
    h.magic = FPSW_MAGIC; h.type = FPSW_SPAWN; h.seq = seq; h.nfd = (uint32_t)nfd; h.inst_fd = inst_w >= 0 ? inst_num : -1;
    int fds[FPSW_MAX_FDS + 2], nf = 0;
    fds[nf++] = m;
    for (int i = 0; i < nfd; i++) { h.target[i] = target[i]; fds[nf++] = src[i]; }
    if (inst_w >= 0) fds[nf++] = inst_w;
    union { struct cmsghdr c; char b[CMSG_SPACE(sizeof(int) * (FPSW_MAX_FDS + 2))]; } u;
    memset(&u, 0, sizeof u);
    struct iovec iov = { &h, sizeof h };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.b, .msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nf) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nf);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nf);
    ssize_t r = sendmsg(s, &mh, MSG_NOSIGNAL);
    close(m);
    return r == (ssize_t)sizeof h ? 0 : -1;
}

/* the next message (2 s at most); *fd: the fd it carried */
static int recv_msg(int s, FpswMsg *m, int *fd) {
    *fd = -1;
    struct pollfd p = { s, POLLIN, 0 };
    if (poll(&p, 1, 2000) != 1) return -1;
    union { struct cmsghdr c; char b[CMSG_SPACE(sizeof(int) * 4)]; } u;
    struct iovec iov = { m, sizeof *m };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.b, .msg_controllen = sizeof u.b };
    ssize_t r = recvmsg(s, &mh, MSG_CMSG_CLOEXEC);
    if (r != (ssize_t)sizeof *m) return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    if (c && c->cmsg_type == SCM_RIGHTS) memcpy(fd, CMSG_DATA(c), sizeof *fd);
    return 0;
}

static pid_t pid_of_pidfd(int fd) {
    char p[64], buf[1024];
    snprintf(p, sizeof p, "/proc/self/fdinfo/%d", fd);
    int f = open(p, O_RDONLY);
    ssize_t n = f >= 0 ? read(f, buf, sizeof buf - 1) : -1;
    if (f >= 0) close(f);
    if (n <= 0) return -1;
    buf[n] = 0;
    char *l = strstr(buf, "\nPid:");
    return l ? (pid_t)atoi(l + 5) : -1;
}

static void checks_pure(void) {
    FpswMsg m;
    memset(&m, 0, sizeof m);
    m.magic = FPSW_MAGIC; m.type = FPSW_SPAWN; m.inst_fd = -1;
    CHECK(fpsw_spawn_ok(&m, 1));                                     /* only the payload */
    CHECK(!fpsw_spawn_ok(&m, 2));                                    /* an fd it didn't announce */
    m.nfd = 3; m.target[0] = 0; m.target[1] = 1; m.target[2] = 2;
    CHECK(fpsw_spawn_ok(&m, 4));                                     /* Chromium's 0, 1, 2 */
    m.nfd = 4; m.target[3] = 5;
    CHECK(fpsw_spawn_ok(&m, 5));                                     /* and its zygote socket */
    m.inst_fd = 3;
    CHECK(fpsw_spawn_ok(&m, 6));                                     /* NOTIFY_START */
    CHECK(!fpsw_spawn_ok(&m, 5));
    m.inst_fd = 5;
    CHECK(!fpsw_spawn_ok(&m, 6));                                    /* the instance fd on a placed one */
    m.inst_fd = 2;
    CHECK(!fpsw_spawn_ok(&m, 6));                                    /* nor on stdio */
    m.inst_fd = 1024;
    CHECK(!fpsw_spawn_ok(&m, 6));
    m.inst_fd = -1; m.target[3] = 1;
    CHECK(!fpsw_spawn_ok(&m, 5));                                    /* the same number twice */
    m.target[3] = 1024;
    CHECK(!fpsw_spawn_ok(&m, 5));                                    /* past 1023 */
    m.target[3] = 7; m.nfd = FPSW_MAX_FDS + 1;
    CHECK(!fpsw_spawn_ok(&m, (int)m.nfd + 1));
    m.nfd = 4; m.magic = 1;
    CHECK(!fpsw_spawn_ok(&m, 5));
    m.magic = FPSW_MAGIC; m.type = FPSW_HELLO;
    CHECK(!fpsw_spawn_ok(&m, 5));
    CHECK(!fpsw_spawn_ok(NULL, 1));

    char buf[256], *a[8], *e[8];
    char *argv[] = { FPAK_FLATPAK, "run", "--sandbox", "x.y.z", NULL }, *envp[] = { "A=1", "B=", NULL };
    long n = fpsw_pack(argv, envp, buf, sizeof buf);
    CHECK(n > 0);
    CHECK(n > 0 && !fpsw_unpack(buf, (size_t)n, a, 8, e, 8) && !strcmp(a[2], "--sandbox") && !strcmp(a[3], "x.y.z") && !a[4] &&
          !strcmp(e[0], "A=1") && !strcmp(e[1], "B=") && !e[2]);
    CHECK(n > 0 && fpsw_unpack(buf, (size_t)n, a, 4, e, 8) < 0);      /* more arguments than room */
    CHECK(n > 0 && fpsw_unpack(buf, (size_t)n - 1, a, 8, e, 8) < 0);  /* cut short */
    char *bad1[] = { "/bin/sh", "run", NULL };
    n = fpsw_pack(bad1, envp, buf, sizeof buf);
    CHECK(n > 0 && fpsw_unpack(buf, (size_t)n, a, 8, e, 8) < 0);      /* only /usr/bin/flatpak */
    char *bad2[] = { FPAK_FLATPAK, "build", NULL };
    n = fpsw_pack(bad2, envp, buf, sizeof buf);
    CHECK(n > 0 && fpsw_unpack(buf, (size_t)n, a, 8, e, 8) < 0);      /* and only "run" */
    char *badenv[] = { "=x", NULL };
    n = fpsw_pack(argv, badenv, buf, sizeof buf);
    CHECK(n > 0 && fpsw_unpack(buf, (size_t)n, a, 8, e, 8) < 0);
    char *noeq[] = { "NOEQ", NULL };
    n = fpsw_pack(argv, noeq, buf, sizeof buf);
    CHECK(n > 0 && fpsw_unpack(buf, (size_t)n, a, 8, e, 8) < 0);
    char *empty[] = { FPAK_FLATPAK, "", NULL };
    CHECK(fpsw_pack(empty, envp, buf, sizeof buf) < 0);               /* an empty string would end the list */
    CHECK(fpsw_pack(argv, envp, buf, 10) < 0);
    char extra[] = { 'x', 0, 0, 0, 'y', 0 };
    CHECK(fpsw_unpack(extra, sizeof extra, a, 8, e, 8) < 0);          /* bytes after the environment */
}

static void checks_live(void) {
    int sv[2];
    CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv));
    pid_t h = fork();
    if (h == 0) { close(sv[0]); fpsw_main(sv[1]); }
    close(sv[1]);
    int s = sv[0];
    FpswMsg m;
    int fd;
    /* HELLO with the helper's own pidfd */
    CHECK(!recv_msg(s, &m, &fd) && m.type == FPSW_HELLO && fd >= 0 && pid_of_pidfd(fd) == h);
    if (fd >= 0) close(fd);
    /* /proc/<helper>: not dumpable */
    char p[64], st[4096];
    snprintf(p, sizeof p, "/proc/%d/status", (int)h);
    int f = open(p, O_RDONLY);
    ssize_t sn = f >= 0 ? read(f, st, sizeof st - 1) : -1;
    if (f >= 0) close(f);
    if (sn > 0) st[sn] = 0; else st[0] = 0;
    CHECK(strstr(st, "CapEff:\t0000000000000000") != NULL);          /* no capability */

    /* Chromium's shape: 0, 1, 2 and a socket at 5, NOTIFY_START at the lowest free number (3) */
    int rep[2], in[2], sock5[2], inst[2];
    CHECK(!pipe(rep) && !pipe(in) && !socketpair(AF_UNIX, SOCK_STREAM, 0, sock5) && !pipe(inst));
    if (write(sock5[1], "zyg", 3) != 3) {}
    char repnum[16];
    snprintf(repnum, sizeof repnum, "%d", 900);
    char *argv[] = { FPAK_FLATPAK, "run", repnum, "5", "3", NULL }, *envp[] = { "FPSW_T=ok", NULL };
    uint32_t tg[] = { 1, 2, 5, 900 };
    int src[] = { rep[1], rep[1], sock5[0], rep[1] };
    CHECK(!send_spawn(s, 7, argv, envp, 4, tg, src, 3, inst[1], 1));
    close(inst[1]);
    CHECK(!recv_msg(s, &m, &fd) && m.type == FPSW_STARTED && m.seq == 7 && fd >= 0);
    pid_t kid = fd >= 0 ? pid_of_pidfd(fd) : -1;
    CHECK(kid > 0);
    if (fd >= 0) close(fd);
    /* the "flatpak" writes to the --instance-id-fd it was given: here it can't, it's the probe; read 3 as a pipe end */
    close(rep[1]);
    char out[1024] = "";
    ssize_t r = 0, k;
    while ((k = read(rep[0], out + r, sizeof out - 1 - (size_t)r)) > 0) r += k;
    out[r > 0 ? r : 0] = 0;
    CHECK(strstr(out, "5=zyg;") != NULL);                            /* the zygote socket at 5 */
    CHECK(strstr(out, "null0=1;") != NULL);                          /* stdin not asked for: /dev/null */
    CHECK(strstr(out, "pgid=1;") != NULL);                           /* its own process group (SpawnSignal to the group) */
    CHECK(strstr(out, "env=ok;") != NULL);                           /* the environment lord-server built */
    CHECK(strstr(out, "extra=0") != NULL);                           /* nothing of the helper's leaks in */
    CHECK(strstr(out, "3=-;") != NULL);                              /* fd 3 is --instance-id-fd's write end (nothing to read) */
    CHECK(!recv_msg(s, &m, &fd) && m.type == FPSW_EXITED && m.seq == 7 && WIFEXITED(m.status) && WEXITSTATUS(m.status) == 42);
    close(rep[0]);
    close(inst[0]);

    /* refused: an unsealed payload, a bad header, a target listed twice */
    int p2[2];
    CHECK(!pipe(p2));
    uint32_t t1[] = { 1 };
    int s1[] = { p2[1] };
    CHECK(!send_spawn(s, 8, argv, envp, 1, t1, s1, -1, -1, 0));
    CHECK(!recv_msg(s, &m, &fd) && m.type == FPSW_FAILED && m.seq == 8 && m.status == 3);
    uint32_t t2[] = { 1, 1 };
    int s2[] = { p2[1], p2[1] };
    CHECK(!send_spawn(s, 9, argv, envp, 2, t2, s2, -1, -1, 1));
    CHECK(!recv_msg(s, &m, &fd) && m.type == FPSW_FAILED && m.seq == 9 && m.status == 1);
    char *sh[] = { "/bin/sh", "run", NULL };
    CHECK(!send_spawn(s, 10, sh, envp, 1, t1, s1, -1, -1, 1));
    CHECK(!recv_msg(s, &m, &fd) && m.type == FPSW_FAILED && m.seq == 10 && m.status == 4);

    /* a signal through the pidfd lord-server keeps (SpawnSignal) ends it, and EXITED says so */
    char *argv2[] = { FPAK_FLATPAK, "run", "sleep", NULL };
    CHECK(!send_spawn(s, 11, argv2, envp, 0, NULL, NULL, -1, -1, 1));
    CHECK(!recv_msg(s, &m, &fd) && m.type == FPSW_STARTED && m.seq == 11 && fd >= 0);
    if (fd >= 0) { CHECK(!syscall(SYS_pidfd_send_signal, fd, SIGTERM, NULL, 0)); close(fd); }
    CHECK(!recv_msg(s, &m, &fd) && m.type == FPSW_EXITED && m.seq == 11 && WIFSIGNALED(m.status) && WTERMSIG(m.status) == SIGTERM);

    /* lord-server goes: the helper ends */
    close(s);
    int hs = 0;
    CHECK(waitpid(h, &hs, 0) == h && WIFEXITED(hs) && WEXITSTATUS(hs) == 0);
    close(p2[0]); close(p2[1]); close(in[0]); close(in[1]); close(sock5[0]); close(sock5[1]);
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "run")) {
        if (!strcmp(argv[2], "sleep")) { pause(); return 0; }
        return probe(argc, argv);
    }
    checks_pure();
    checks_live();
    printf("fpspawner-test: %d passed, %d failed\n", pass, failc);
    return failc != 0;
}
