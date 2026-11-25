/* nofile_test.c — lord-server's open files' limit (src/nofile.c, GitHub #251).
 *
 * In the reported session lord-server reached the usual soft limit of 1024 open files: the kernel dropped the files
 * Brave and MailCraft sent (libwayland: "file descriptor expected ... import_timeline"; both closed and would not open
 * again) and Xwayland's child could not open /dev/null (exit 126), until the next login. Checked here: a process at
 * its limit has the files sent to it dropped (the failure) while the raised one receives them; with more than 1024
 * open, a child lowered back at fork() could not open anything (why nofile_restore comes after the child's
 * close_range), and lowered after it, it has the usual 1024 and room under it; the count a report gets. */
#include "../src/nofile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static rlim_t soft_now(void) {
    struct rlimit rl;
    return getrlimit(RLIMIT_NOFILE, &rl) ? 0 : rl.rlim_cur;
}

/* sends one fd over s (an open /dev/null); 0 sent */
static int send_fd(int s) {
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char b = 'x';
    struct iovec io = { &b, 1 };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u;
    memset(&u, 0, sizeof u);
    struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = u.buf, .msg_controllen = sizeof u.buf };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    int r = sendmsg(s, &m, 0) == 1 ? 0 : -1;
    close(fd);
    return r;
}

/* receives one message: the fd that came with it (closed here), 0 if it came without one (dropped), -1 an error */
static int recv_fd(int s) {
    char b;
    struct iovec io = { &b, 1 };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u;
    struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = u.buf, .msg_controllen = sizeof u.buf };
    if (recvmsg(s, &m, MSG_CMSG_CLOEXEC) != 1) return -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len >= CMSG_LEN(sizeof(int))) {
            int fd;
            memcpy(&fd, CMSG_DATA(c), sizeof fd);
            close(fd);
            return 1;
        }
    return 0;
}

/* fills the table with /dev/null up to the limit; returns how many were opened (closed by the caller via fds) */
static int fill(int *fds, int max) {
    int n = 0;
    while (n < max) {
        int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (fd < 0) break;
        fds[n++] = fd;
    }
    return n;
}

static void test_target(void) {
    CHECK(nofile_target(1024, 524288, 65536) == 65536);
    CHECK(nofile_target(1024, 4096, 65536) == 4096);              /* never past the hard limit */
    CHECK(nofile_target(1024, RLIM_INFINITY, 65536) == 65536);
    CHECK(nofile_target(100000, 524288, 65536) == 100000);        /* never lowered */
    CHECK(nofile_target(RLIM_INFINITY, RLIM_INFINITY, 65536) == RLIM_INFINITY);
    CHECK(nofile_target(1024, 1024, 65536) == 1024);              /* nothing to raise */
}

int main(void) {
    test_target();

    /* the session's starting point: the usual soft limit of 1024 (or the hard one, if lower) */
    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0);
    rlim_t start = rl.rlim_max == RLIM_INFINITY || rl.rlim_max > 1024 ? 1024 : rl.rlim_max;
    struct rlimit st = { start, rl.rlim_max };
    CHECK(setrlimit(RLIMIT_NOFILE, &st) == 0);
    int canraise = nofile_target(start, rl.rlim_max, NOFILE_WANT) > start;

    /* the failure, before the fix: at the limit, a file an app sends is dropped (the message still arrives) */
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    int *fds = calloc(start + 16 + 64, sizeof *fds);
    CHECK(fds != NULL);
    int n = fill(fds, (int)start + 16);
    CHECK(errno == EMFILE);
    close(fds[--n]);                        /* one free: the sender's own open of /dev/null */
    CHECK(send_fd(sv[0]) == 0);
    fds[n] = open("/dev/null", O_RDONLY | O_CLOEXEC);    /* the sender's slot taken again: the receiver is full */
    CHECK(fds[n] >= 0);
    if (fds[n] >= 0) n++;
    CHECK(recv_fd(sv[1]) == 0);             /* "file descriptor expected" in libwayland */

    /* the fix: raised, the same message brings its file */
    rlim_t before = 0, after = 0;
    CHECK(nofile_raise(&before, &after) == 0);
    CHECK(before == start);
    if (canraise) {
        CHECK(after > start && after <= NOFILE_WANT);
        CHECK(soft_now() == after);
        int extra = open("/dev/null", O_RDONLY | O_CLOEXEC);   /* the sender's */
        CHECK(extra >= 0);
        if (extra >= 0) close(extra);
        CHECK(send_fd(sv[0]) == 0);
        CHECK(recv_fd(sv[1]) == 1);
    } else {
        fprintf(stderr, "nofile: the hard limit is %llu here: the raise isn't checked\n", (unsigned long long)rl.rlim_max);
    }

    /* a report's count: every file open now (the table is full of /dev/null) */
    rlim_t lim = 0;
    int cnt = nofile_count(&lim);
    CHECK(lim == soft_now());
    CHECK(cnt >= n && cnt <= n + 8);

    /* lord-server past 1024 files: every number under 1024 taken, more open above it */
    int more = 0;
    if (canraise) {
        more = fill(fds + n, 64);
        CHECK(more == 64);
        n += more;
        CHECK(n > (int)start);
    }

    /* a forked child (an app, Xwayland, a shell) keeps the raised limit until it closed lord-server's files: lowered
     * at fork(), it could not open /dev/null nor move a socket (Xwayland's "exit 126" in the report) */
    pid_t p = fork();
    if (p == 0) {
        struct rlimit c;
        if (getrlimit(RLIMIT_NOFILE, &c) || c.rlim_cur != after) _exit(1);
        if (canraise) {
            nofile_restore();               /* the wrong place: before the clean-up */
            int f = open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (f >= 0 || errno != EMFILE) _exit(2);
            struct rlimit up = { after, c.rlim_max };
            if (setrlimit(RLIMIT_NOFILE, &up)) _exit(3);
        }
        /* the child's own work (Xwayland's: /dev/null, its sockets moved high), then lord-server's files closed */
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        int hi = null >= 0 ? fcntl(null, F_DUPFD_CLOEXEC, 100) : -1;
        if (null < 0 || hi < 0) _exit(4);
        if (dup2(null, 0) != 0 || dup2(hi, 3) != 3) _exit(5);
        if (syscall(SYS_close_range, 4, ~0u, 0)) _exit(6);
        nofile_restore();                   /* the right place: just before execve */
        if (getrlimit(RLIMIT_NOFILE, &c) || c.rlim_cur != (after > FD_SETSIZE ? FD_SETSIZE : after) || c.rlim_max != rl.rlim_max) _exit(7);
        int f = open("/dev/null", O_RDONLY);  /* room under 1024: what the app opens first */
        if (f < 0 || f >= FD_SETSIZE) _exit(8);
        _exit(0);
    }
    int ws = 0;
    CHECK(p > 0 && waitpid(p, &ws, 0) == p && WIFEXITED(ws));
    if (WIFEXITED(ws) && WEXITSTATUS(ws)) fprintf(stderr, "nofile: the child failed at step %d\n", WEXITSTATUS(ws));
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0);
    CHECK(soft_now() == after);             /* the parent keeps its own */

    /* nofile_restore never raises: a limit already under 1024 stays */
    p = fork();
    if (p == 0) {
        struct rlimit low = { 256, rl.rlim_max }, c;
        if (setrlimit(RLIMIT_NOFILE, &low)) _exit(1);
        nofile_restore();
        _exit(getrlimit(RLIMIT_NOFILE, &c) == 0 && c.rlim_cur == 256 ? 0 : 2);
    }
    CHECK(p > 0 && waitpid(p, &ws, 0) == p && WIFEXITED(ws) && WEXITSTATUS(ws) == 0);

    /* once: a second call changes nothing */
    rlim_t b2 = 0, a2 = 0;
    CHECK(nofile_raise(&b2, &a2) == 0 && b2 == after && a2 == after);

    for (int i = 0; i < n; i++) close(fds[i]);
    free(fds);
    close(sv[0]);
    close(sv[1]);
    if (fails) { fprintf(stderr, "nofile: %d failed\n", fails); return 1; }
    printf("nofile: ok (soft limit %llu → %llu, %d files open; an app's child gets %d back after its clean-up)\n",
           (unsigned long long)start, (unsigned long long)after, n, FD_SETSIZE);
    return 0;
}
