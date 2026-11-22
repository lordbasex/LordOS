/* fpspawner.c — the Spawn helper inside a Flatpak app's view (see fpspawner.h; docs/FLATPAK.md, "Sub-sandboxes";
 * AGENTS.md E9). */
#include "fpspawner.h"
#include "fpak.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif
#ifndef F_GET_SEALS
#define F_GET_SEALS 1034
#endif
#ifndef F_SEAL_WRITE
#define F_SEAL_WRITE 0x0008
#endif

extern char **environ;

/* what the helper executes: always /usr/bin/flatpak (tests/fpspawner_test.c builds it with its own probe) */
#ifndef FPSW_EXEC
#define FPSW_EXEC FPAK_FLATPAK
#endif

int fpsw_spawn_ok(const FpswMsg *m, int nfds) {
    if (!m || m->magic != FPSW_MAGIC || m->type != FPSW_SPAWN || m->nfd > FPSW_MAX_FDS) return 0;
    for (uint32_t i = 0; i < m->nfd; i++) {
        if (m->target[i] > 1023) return 0;
        for (uint32_t j = 0; j < i; j++) if (m->target[j] == m->target[i]) return 0;
        if (m->inst_fd >= 0 && m->target[i] == (uint32_t)m->inst_fd) return 0;
    }
    if (m->inst_fd != -1 && (m->inst_fd < 3 || m->inst_fd > 1023)) return 0;
    return nfds == 1 + (int)m->nfd + (m->inst_fd >= 0);
}

long fpsw_pack(char *const argv[], char *const envp[], char *out, size_t cap) {
    size_t used = 0;
    for (int part = 0; part < 2; part++) {
        char *const *l = part ? envp : argv;
        for (int i = 0; l && l[i]; i++) {
            size_t n = strlen(l[i]);
            if (!n || used + n + 1 > cap) return -1;      /* an empty string ends a list */
            memcpy(out + used, l[i], n + 1);
            used += n + 1;
        }
        if (used + 1 > cap) return -1;
        out[used++] = 0;
    }
    return (long)used;
}

int fpsw_unpack(char *buf, size_t n, char **argv, int amax, char **envp, int emax) {
    if (!buf || !n || buf[n - 1]) return -1;
    size_t at = 0;
    for (int part = 0; part < 2; part++) {
        char **l = part ? envp : argv;
        int max = part ? emax : amax, k = 0;
        for (;;) {
            if (at >= n) return -1;
            char *s = buf + at;
            size_t len = strlen(s);
            at += len + 1;
            if (!len) break;
            if (k >= max - 1) return -1;
            if (part) { const char *eq = strchr(s, '='); if (!eq || eq == s) return -1; }
            l[k++] = s;
        }
        l[k] = NULL;
    }
    if (at != n || !argv[0] || strcmp(argv[0], FPAK_FLATPAK) || !argv[1] || strcmp(argv[1], "run")) return -1;
    return 0;
}

/* ---- the helper ---- */

static int send_msg(int sock, uint32_t type, uint32_t seq, int32_t status, int fd) {
    FpswMsg m;
    memset(&m, 0, sizeof m);
    m.magic = FPSW_MAGIC; m.type = type; m.seq = seq; m.status = status; m.inst_fd = -1;
    struct iovec iov = { &m, sizeof m };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } u;
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (fd >= 0) {
        memset(&u, 0, sizeof u);
        mh.msg_control = u.b;
        mh.msg_controllen = sizeof u.b;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof fd);
    }
    for (;;) {
        ssize_t r = sendmsg(sock, &mh, MSG_NOSIGNAL);
        if (r == (ssize_t)sizeof m) return 0;
        if (r < 0 && errno == EINTR) continue;
        return -1;
    }
}

static int pidfd_of(pid_t p) { return (int)syscall(SYS_pidfd_open, p, 0); }

/* In the new process: the fds where the app asked, then `flatpak run`. Never returns. */
static void child(const FpswMsg *m, const int *src, int inst, char **argv, char **envp) {
    setpgid(0, 0);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    for (int s = 1; s < 32; s++) signal(s, SIG_DFL);
    /* everything this needs goes above 1100 first (close-on-exec), so placing the app's fds at the numbers it asked
     * for (any of 0-1023) can't overwrite one of them; everything else closes at exec */
    syscall(SYS_close_range, 3, ~0u, CLOSE_RANGE_CLOEXEC);
    struct rlimit rl0, rl;
    if (getrlimit(RLIMIT_NOFILE, &rl0)) _exit(124);
    rl = rl0;
    if (rl.rlim_cur < 1200) {
        if (rl.rlim_max < 1200) _exit(124);
        rl.rlim_cur = rl.rlim_max < 4096 ? rl.rlim_max : 4096;
        if (setrlimit(RLIMIT_NOFILE, &rl)) _exit(124);
    }
    int hi[FPSW_MAX_FDS];
    for (uint32_t i = 0; i < m->nfd; i++) if ((hi[i] = fcntl(src[i], F_DUPFD_CLOEXEC, 1100)) < 0) _exit(125);
    int inst_hi = inst >= 0 ? fcntl(inst, F_DUPFD_CLOEXEC, 1100) : -1;
    if (inst >= 0 && inst_hi < 0) _exit(125);
    int o = open("/dev/null", O_RDWR | O_CLOEXEC), null = o >= 0 ? fcntl(o, F_DUPFD_CLOEXEC, 1100) : -1;
    if (o >= 0) close(o);
    int has0 = 0;
    for (uint32_t i = 0; i < m->nfd; i++) {
        if (dup2(hi[i], (int)m->target[i]) < 0) _exit(126);
        if (m->target[i] == 0) has0 = 1;
    }
    /* stdout and stderr not asked for stay the app's (its log); stdin is /dev/null */
    if (!has0 && null >= 0) dup2(null, 0);
    if (inst_hi >= 0 && dup2(inst_hi, m->inst_fd) < 0) _exit(126);
    if (chdir("/")) {}
    if (setrlimit(RLIMIT_NOFILE, &rl0)) _exit(126);
    execve(FPSW_EXEC, argv, envp);
    _exit(127);
}

typedef struct { pid_t pid; int pidfd; uint32_t seq; } Kid;

void fpsw_main(int sock) {
    prctl(PR_SET_NAME, "lordos-spawn");
    prctl(PR_SET_DUMPABLE, 0);                     /* nothing in the view reads or traces it */
    /* no parent-death signal: it serves the app as long as anything of it runs (a launcher like VS Code's `code` leaves
     * at once and its Electron goes on asking); the view's PID 1 ends it once nothing else is left there
     * (sandbox_view_helper), and the kernel with the view */
    /* no capability, not even in the view's own user namespace (the --launch-view process created it) */
    struct __user_cap_header_struct ch = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct cd[2];
    memset(cd, 0, sizeof cd);
    for (int c = 0; c <= 63; c++) prctl(PR_CAPBSET_DROP, c, 0, 0, 0);
    if (syscall(SYS_capset, &ch, cd)) _exit(120);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);
    fcntl(sock, F_SETFD, FD_CLOEXEC);
    int self = pidfd_of(getpid());
    if (self < 0 || send_msg(sock, FPSW_HELLO, 0, 0, self)) _exit(121);
    close(self);
    Kid kids[FPSW_MAX_KIDS];
    int nk = 0;
    char *buf = mmap(NULL, FPSW_PAYLOAD_MAX, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) _exit(122);
    static char *argv[FPSW_ARGV_MAX], *envp[FPSW_ENV_MAX];
    for (;;) {
        struct pollfd p[1 + FPSW_MAX_KIDS];
        p[0] = (struct pollfd){ sock, POLLIN, 0 };
        for (int i = 0; i < nk; i++) p[1 + i] = (struct pollfd){ kids[i].pidfd, POLLIN, 0 };
        if (poll(p, (nfds_t)(1 + nk), -1) < 0) { if (errno == EINTR) continue; _exit(123); }
        for (int i = nk - 1; i >= 0; i--) {
            if (!p[1 + i].revents) continue;
            int st = 0;
            pid_t w = waitpid(kids[i].pid, &st, WNOHANG);
            if (w == 0) continue;
            if (send_msg(sock, FPSW_EXITED, kids[i].seq, w > 0 ? st : 0, -1)) _exit(0);
            close(kids[i].pidfd);
            kids[i] = kids[--nk];
        }
        if (!p[0].revents) continue;
        FpswMsg m;
        union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int) * (FPSW_MAX_FDS + 2))]; } u;
        struct iovec iov = { &m, sizeof m };
        struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.b, .msg_controllen = sizeof u.b };
        ssize_t r = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
        if (r < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (r <= 0) _exit(0);                       /* lord-server went: the session is ending */
        int fds[FPSW_MAX_FDS + 2], nf = 0, bad = (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 || r != (ssize_t)sizeof m;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) { bad = 1; continue; }
            int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int j = 0; j < k; j++) {
                int fd;
                memcpy(&fd, CMSG_DATA(c) + j * sizeof(int), sizeof fd);
                if (nf < FPSW_MAX_FDS + 2) fds[nf++] = fd; else { close(fd); bad = 1; }
            }
        }
        int step = 0;
        if (bad || !fpsw_spawn_ok(&m, nf)) step = 1;
        else if (nk >= FPSW_MAX_KIDS) step = 2;
        else {
            /* the payload: a sealed memfd, read whole */
            struct stat st;
            int seals = fcntl(fds[0], F_GET_SEALS);
            if (seals < 0 || !(seals & F_SEAL_WRITE) || fstat(fds[0], &st) || st.st_size <= 0 || st.st_size > FPSW_PAYLOAD_MAX) step = 3;
            else {
                size_t got = 0;
                for (ssize_t k; got < (size_t)st.st_size && (k = pread(fds[0], buf + got, (size_t)st.st_size - got, (off_t)got)) > 0;) got += (size_t)k;
                if (got != (size_t)st.st_size || fpsw_unpack(buf, got, argv, FPSW_ARGV_MAX, envp, FPSW_ENV_MAX)) step = 4;
            }
        }
        if (!step) {
            pid_t kid = fork();
            if (kid == 0) { close(sock); child(&m, fds + 1, m.inst_fd >= 0 ? fds[1 + m.nfd] : -1, argv, envp); }
            int pfd = kid > 0 ? pidfd_of(kid) : -1;
            if (kid < 0) step = 5;
            else if (pfd < 0) { kill(kid, SIGKILL); waitpid(kid, NULL, 0); step = 6; }
            else {
                kids[nk++] = (Kid){ kid, pfd, m.seq };
                if (send_msg(sock, FPSW_STARTED, m.seq, 0, pfd)) _exit(0);
            }
        }
        for (int j = 0; j < nf; j++) close(fds[j]);
        if (step && send_msg(sock, FPSW_FAILED, m.seq, step, -1)) _exit(0);
    }
}
