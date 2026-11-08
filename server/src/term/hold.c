/* hold.c — sessions that survive a lord-server restart (a session server).
 *
 * For each session a holder is launched: the same binary re-executed with --term-holder,
 * with no privileges and none of the server's memory. It only keeps the PTY master open (so
 * the shell does not get SIGHUP if the server crashes or restarts) and listens on a unix
 * socket in the user's private folder. On startup, the server connects to each socket and
 * receives the master via SCM_RIGHTS; the screen contents are saved on exit to a .state
 * file next to the socket.
 *
 * The holder never reads or writes the PTY and runs nothing: it exits when the shell
 * closes the terminal (POLLHUP) or when the server asks it to close the session ('q').
 * It only accepts connections from the same user (SO_PEERCRED). */
#include "term.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static char dir[256];

/* $XDG_RUNTIME_DIR/lordos-term (the session's, /run/user/<uid> for lord-sessiond's: a tmpfs wiped
 * when the machine reboots) or, if there is none, ~/.cache/lordos/term. Always 0700. The session's
 * runtime folder, as for the rest of lord-server's sockets: a lord-server started with a runtime
 * folder of its own (a test, another session) does not find the holders of the user's desktop
 * (0.8.8.6). */
const char *hold_dir(void) {
    if (dir[0]) return dir;
    char run[200];
    struct stat st;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0] == '/' && strlen(rt) < sizeof run - 16) snprintf(run, sizeof run, "%s", rt);
    else snprintf(run, sizeof run, "/run/user/%d", (int)getuid());
    if (!stat(run, &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid()) snprintf(dir, sizeof dir, "%s/lordos-term", run);
    else {
        const char *home = getenv("HOME");
        if (!home) return NULL;
        char p[256];
        snprintf(p, sizeof p, "%s/.cache", home);
        mkdir(p, 0700);
        snprintf(p, sizeof p, "%s/.cache/lordos", home);
        mkdir(p, 0700);
        snprintf(dir, sizeof dir, "%s/.cache/lordos/term", home);
    }
    mkdir(dir, 0700);
    if (stat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) { dir[0] = 0; return NULL; }
    return dir;
}

static int sock_path(const char *key, struct sockaddr_un *a) {
    const char *d = hold_dir();
    if (!d) return -1;
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    return snprintf(a->sun_path, sizeof a->sun_path, "%s/%s.sock", d, key) < (int)sizeof a->sun_path ? 0 : -1;
}

void hold_state_path(const char *key, char *out, size_t n) {
    const char *d = hold_dir();
    snprintf(out, n, "%s/%s.state", d ? d : "/nonexistent", key);
}

static int valid_key(const char *k) {
    size_t n = strlen(k);
    if (!n || n > 40) return 0;
    for (; *k; k++) if (!((*k >= '0' && *k <= '9') || (*k >= 'a' && *k <= 'f') || *k == '-')) return 0;
    return 1;
}

/* ---- holder ---- */

static int send_fd(int sock, int fd, const char *msg) {
    struct iovec iov = { (void *)msg, strlen(msg) };
    char buf[CMSG_SPACE(sizeof(int))];
    memset(buf, 0, sizeof buf);
    struct msghdr m = { 0 };
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = buf;
    m.msg_controllen = sizeof buf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    return sendmsg(sock, &m, MSG_NOSIGNAL) < 0 ? -1 : 0;
}

/* The server at the other end of ctrl has closed it (exited, crashed): its end reads EOF or fails. The
 * server never writes to it but the 'q' that closes the session, which is only peeked here. */
static int peer_gone(int ctrl) {
    struct pollfd p = { ctrl, POLLIN, 0 };
    if (poll(&p, 1, 0) <= 0) return 0;
    if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) return 1;
    char c;
    return recv(ctrl, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
}

/* argv: --term-holder <key> <shell pid>; fds: 3 master, 4 listening socket, 5 control. */
int hold_main(int argc, char **argv) {
    if (argc < 4 || !valid_key(argv[2])) return 2;
    setsid();
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    int master = 3, lsock = 4, ctrl = 5;
    char msg[64];
    snprintf(msg, sizeof msg, "LORDTERM %d\n", atoi(argv[3]));
    struct sockaddr_un addr;
    if (sock_path(argv[2], &addr)) return 1;
    for (;;) {
        struct pollfd p[3] = { { master, 0, 0 }, { lsock, POLLIN, 0 }, { ctrl, POLLIN, 0 } };
        if (poll(p, ctrl >= 0 ? 3 : 2, -1) < 0) { if (errno == EINTR) continue; break; }
        if (p[0].revents & (POLLHUP | POLLERR | POLLNVAL)) break;           /* the shell exited */
        if (ctrl >= 0 && p[2].revents) {
            char c = 0;
            ssize_t r = read(ctrl, &c, 1);
            if (r == 1 && c == 'q') break;                                   /* close the session */
            if (r <= 0) { close(ctrl); ctrl = -1; }                          /* the server went away */
        }
        if (p[1].revents & POLLIN) {
            int s = accept4(lsock, NULL, NULL, SOCK_CLOEXEC);
            if (s < 0) continue;
            /* One lord-server at a time: while the one that has the session is alive, nobody else gets the
             * master. A second lord-server of the same user (a test, a headless session) used to take it as
             * well: two readers of one pty, each taking part of the program's output (an ESC lost, half a
             * frame missing), and each setting the pty's size for its own window (Claude Code drawing in 80
             * columns of a 260-column window). The one that has it closes its end when it exits or crashes. */
            if (ctrl >= 0 && !peer_gone(ctrl)) { close(s); continue; }
            struct ucred cr;
            socklen_t len = sizeof cr;
            if (getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &len) || cr.uid != getuid() || send_fd(s, master, msg)) { close(s); continue; }
            if (ctrl >= 0) close(ctrl);
            ctrl = s;
        }
    }
    unlink(addr.sun_path);
    char state[300];
    hold_state_path(argv[2], state, sizeof state);
    unlink(state);
    return 0;
}

/* ---- server ---- */

/* Launches the holder for a newly created session. Returns the control fd (or -1) and the key. */
int hold_spawn(int master, pid_t pid, char *key, size_t keylen) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(key, keylen, "%x-%lx", (unsigned)pid, (unsigned long)(ts.tv_nsec ^ ts.tv_sec << 20) & 0xffffffffffUL);
    struct sockaddr_un addr;
    if (sock_path(key, &addr)) { key[0] = 0; return -1; }
    unlink(addr.sun_path);
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int pair[2] = { -1, -1 };
    if (ls < 0 || bind(ls, (struct sockaddr *)&addr, sizeof addr) || listen(ls, 2) ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair)) {
        if (ls >= 0) close(ls);
        unlink(addr.sun_path);
        key[0] = 0;
        return -1;
    }
    chmod(addr.sun_path, 0600);
    pid_t h = fork();
    if (h == 0) {
        /* fds 3, 4 and 5 for the holder; the rest (DRM, input, other PTYs) are closed */
        int fds[3] = { fcntl(master, F_DUPFD, 10), fcntl(ls, F_DUPFD, 10), fcntl(pair[1], F_DUPFD, 10) };
        for (int i = 0; i < 3; i++) { dup2(fds[i], 3 + i); }
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
        syscall(SYS_close_range, 6, ~0u, 0);
        char spid[16];
        snprintf(spid, sizeof spid, "%d", (int)pid);
        char *args[] = { "lord-term-holder", "--term-holder", key, spid, NULL };
        execv("/proc/self/exe", args);
        _exit(127);
    }
    close(ls);
    close(pair[1]);
    if (h < 0) { close(pair[0]); unlink(addr.sun_path); key[0] = 0; return -1; }
    return pair[0];
}

/* On startup: connects to each live holder and recovers its session. Sockets without a
 * holder (machine rebooted, dead process) are deleted. */
void hold_restore(void (*fn)(int fd, int ctrl, pid_t pid, const char *key)) {
    const char *d = hold_dir();
    DIR *dd = d ? opendir(d) : NULL;
    if (!dd) return;
    struct dirent *e;
    while ((e = readdir(dd))) {
        char key[64];
        size_t n = strlen(e->d_name);
        if (n < 6 || n - 5 >= sizeof key || strcmp(e->d_name + n - 5, ".sock")) continue;
        memcpy(key, e->d_name, n - 5);
        key[n - 5] = 0;
        if (!valid_key(key)) continue;
        struct sockaddr_un addr;
        if (sock_path(key, &addr)) continue;
        int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s < 0) continue;
        if (connect(s, (struct sockaddr *)&addr, sizeof addr)) {
            close(s);
            unlink(addr.sun_path);
            char st[300];
            hold_state_path(key, st, sizeof st);
            unlink(st);
            continue;
        }
        char msg[64] = "", cbuf[CMSG_SPACE(sizeof(int))];
        struct iovec iov = { msg, sizeof msg - 1 };
        struct msghdr m = { 0 };
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        m.msg_control = cbuf;
        m.msg_controllen = sizeof cbuf;
        struct timeval tv = { 2, 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ssize_t r = recvmsg(s, &m, MSG_CMSG_CLOEXEC);
        struct cmsghdr *c = r > 0 ? CMSG_FIRSTHDR(&m) : NULL;
        int fd = -1, pid = 0;
        if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(c), sizeof fd);
        if (fd < 0 || sscanf(msg, "LORDTERM %d", &pid) != 1 || pid <= 0) { if (fd >= 0) close(fd); close(s); continue; }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fn(fd, s, (pid_t)pid, key);
    }
    closedir(dd);
}
