/* fplend.c — lending the chosen file to a running Flatpak app: lord-server's side (see fplend.h; the view's side is
 * lend_one in sec/sandbox.c). */
#include "fplend.h"
#include "fpak.h"
#include "fsops.h"
#include "sec/appid.h"
#include "sec/sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifdef __has_include
#  if __has_include(<linux/openat2.h>)
#    include <linux/openat2.h>
#    define HAVE_OPENAT2 1
#  endif
#endif

#define MAX_VIEWS 64
#define ANSWER_MS 2000

typedef struct { int fd; pid_t pid; char id[FPAK_ID_MAX + 1]; } LView;
static LView views[MAX_VIEWS];
static int init;
static uint32_t seq;

static void setup(void) {
    if (init) return;
    init = 1;
    for (int i = 0; i < MAX_VIEWS; i++) views[i].fd = -1;
}

static pid_t pid_of_pidfd(int fd) {
    char p[64], buf[1024];
    snprintf(p, sizeof p, "/proc/self/fdinfo/%d", fd);
    int f = open(p, O_RDONLY | O_CLOEXEC);
    if (f < 0) return -1;
    ssize_t n = read(f, buf, sizeof buf - 1);
    close(f);
    if (n <= 0) return -1;
    buf[n] = 0;
    char *l = strstr(buf, "\nPid:");
    long v = l ? strtol(l + 5, NULL, 10) : -1;
    return v > 0 && v < INT_MAX ? (pid_t)v : -1;
}

static void gone(int v) {
    if (views[v].fd >= 0) close(views[v].fd);
    views[v].fd = -1;
    views[v].pid = 0;
    views[v].id[0] = 0;
}

/* One message from a view, without waiting: 1 (m filled; *fd the pidfd a HELLO carried, or -1), 0 none, -1 gone */
static int recv_one(int v, SbLendMsg *m, int *fd) {
    *fd = -1;
    union { struct cmsghdr c; char b[CMSG_SPACE(sizeof(int) * 4)]; } u;
    struct iovec iov = { m, sizeof *m };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.b, .msg_controllen = sizeof u.b };
    ssize_t r = recvmsg(views[v].fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (r < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    if (r == 0) return -1;
    int bad = r != (ssize_t)sizeof *m || m->magic != SB_LEND_MAGIC || (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        for (int j = 0; j < k; j++) {
            int f;
            memcpy(&f, CMSG_DATA(c) + j * sizeof(int), sizeof f);
            if (*fd < 0 && !bad) *fd = f; else close(f);
        }
    }
    if (bad) { if (*fd >= 0) close(*fd); *fd = -1; return -1; }
    return 1;
}

/* hellos and closed sockets */
static void drain(void) {
    SbLendMsg *m = malloc(sizeof *m);
    if (!m) return;
    for (int v = 0; v < MAX_VIEWS; v++) {
        if (views[v].fd < 0) continue;
        int fd, r;
        while ((r = recv_one(v, m, &fd)) == 1) {
            if (m->type == SB_LEND_HELLO && !views[v].pid && fd >= 0) views[v].pid = pid_of_pidfd(fd);
            if (fd >= 0) close(fd);
        }
        if (r < 0) gone(v);
    }
    free(m);
}

void fplend_add_view(int fd, const char *id) {
    setup();
    drain();
    for (int v = 0; v < MAX_VIEWS; v++) {
        if (views[v].fd >= 0) continue;
        views[v].fd = fd;
        views[v].pid = 0;
        snprintf(views[v].id, sizeof views[v].id, "%s", id && fpak_id_ok(id) ? id : "");
        return;
    }
    close(fd);                                  /* no room: that view gets the real paths only */
}

/* ---- the caller's view ---- */

static ssize_t read_small(const char *path, char *out, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t got = 0;
    for (ssize_t r; got < cap - 1 && (r = read(fd, out + got, cap - 1 - got)) > 0;) got += (size_t)r;
    close(fd);
    out[got] = 0;
    return (ssize_t)got;
}

static int cgroup_of(pid_t pid, char *out, size_t cap) {
    char p[64], buf[1024];
    snprintf(p, sizeof p, "/proc/%d/cgroup", (int)pid);
    if (read_small(p, buf, sizeof buf) <= 0) return -1;
    char *l = strstr(buf, "0::");
    if (!l) return -1;
    l += 3;
    char *nl = strchr(l, '\n');
    if (nl) *nl = 0;
    return snprintf(out, cap, "%s", l) < (int)cap ? 0 : -1;
}

static pid_t ppid_of(pid_t pid) {
    char p[64], buf[2048];
    snprintf(p, sizeof p, "/proc/%d/status", (int)pid);
    if (read_small(p, buf, sizeof buf) <= 0) return -1;
    char *l = strstr(buf, "\nPPid:");
    return l ? (pid_t)atoi(l + 6) : -1;
}

/* the view's PID 1: the topmost ancestor still in the same scope (as fpspawn.c's view_root) */
static pid_t view_root(pid_t pid) {
    char cg[512], pcg[512];
    if (cgroup_of(pid, cg, sizeof cg)) return -1;
    pid_t cur = pid;
    for (int i = 0; i < 64; i++) {
        pid_t pp = ppid_of(cur);
        if (pp <= 1 || cgroup_of(pp, pcg, sizeof pcg) || strcmp(cg, pcg)) return cur;
        cur = pp;
    }
    return -1;
}

static double mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int open_nolink(int dirfd, const char *path, int flags, int dir) {
#if defined(HAVE_OPENAT2) && defined(SYS_openat2)
    struct open_how how;
    memset(&how, 0, sizeof how);
    how.flags = (uint64_t)(flags | O_CLOEXEC | (dir ? O_DIRECTORY : 0));
    how.mode = (flags & O_CREAT) ? 0644 : 0;
    how.resolve = RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | (dirfd != AT_FDCWD ? RESOLVE_BENEATH : 0);
    return (int)syscall(SYS_openat2, dirfd, path, &how, sizeof how);
#else
    (void)dirfd; (void)path; (void)flags; (void)dir;
    errno = ENOSYS;
    return -1;
#endif
}

/* asks the view and waits for its answer: 0 (grant set) or an errno */
static int ask(int v, SbLendMsg *q, char *grant) {
    q->magic = SB_LEND_MAGIC;
    q->type = SB_LEND_LEND;
    q->seq = ++seq;
    if (send(views[v].fd, q, sizeof *q, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)sizeof *q) { gone(v); return EPIPE; }
    uint32_t want = q->seq;
    double end = mono_ms() + ANSWER_MS;
    for (;;) {
        int fd, r = recv_one(v, q, &fd);
        if (fd >= 0) close(fd);
        if (r < 0) { gone(v); return EPIPE; }
        if (r == 1 && q->type == SB_LEND_DONE && q->seq == want) {
            if (q->status) return q->status > 0 ? q->status : EIO;
            q->grant[sizeof q->grant - 1] = 0;
            for (const char *c = q->grant; *c; c++) if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'))) return EPROTO;
            if (strlen(q->grant) != 32) return EPROTO;
            memcpy(grant, q->grant, 33);
            return 0;
        }
        double left = end - mono_ms();
        if (left <= 0) return ETIMEDOUT;
        struct pollfd pf = { views[v].fd, POLLIN, 0 };
        poll(&pf, 1, (int)left + 1);
    }
}

int fplend_lend(unsigned caller_pid, const char *path, int kind, char *out, size_t n) {
    char app[160];
    if (!caller_pid || appid_of_pid(caller_pid, app, sizeof app) != 0 || strncmp(app, "flatpak:", 8)) return 1;
    const char *id = app + 8;
    if (!fpak_id_ok(id) || !path || path[0] != '/' || kind < FPLEND_FILE || kind > FPLEND_SAVE) return -1;
    setup();
    drain();
    pid_t root = view_root((pid_t)caller_pid);
    int v = -1;
    for (int i = 0; root > 0 && i < MAX_VIEWS; i++)
        if (views[i].fd >= 0 && views[i].pid == root && !strcmp(views[i].id, id)) { v = i; break; }
    if (v < 0) { fprintf(stderr, "fplend: %s: no lending in the caller's view\n", id); return -1; }
    /* the name the app sees, and the folder for a save */
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    if (!sandbox_lend_name_ok(name)) return -1;
    SbLendMsg *q = calloc(1, sizeof *q);
    if (!q) return -1;
    int r = -1, fd = -1, pfd = -1;
    struct stat st, pst;
    q->kind = (uint32_t)kind;
    snprintf(q->name, sizeof q->name, "%s", name);
    if (kind == FPLEND_SAVE) {
        char parent[PATH_MAX];
        size_t pl = (size_t)(slash - path);
        if (!pl) pl = 1;                        /* "/name": the root folder */
        if (pl >= sizeof parent) goto out;
        memcpy(parent, path, pl);
        parent[pl] = 0;
        /* only a place the person may write, never a protected or sensitive one (the launcher's grant rules) */
        if (fsops_protected_resolved(path) || !apps_grant_ok(path) || !apps_grant_ok(parent)) {
            fprintf(stderr, "fplend: %s: a save there isn't lent\n", id);
            goto out;
        }
        pfd = open_nolink(AT_FDCWD, parent, O_PATH, 1);
        if (pfd < 0 || fstat(pfd, &pst) || !S_ISDIR(pst.st_mode)) goto out;
        /* the target, made empty if it isn't there (the app writes it through the lent name) */
        fd = open_nolink(pfd, name, O_WRONLY | O_CREAT | O_NOFOLLOW, 0);
        if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1) goto out;
        snprintf(q->path, sizeof q->path, "%s", parent);
        q->dev = (uint64_t)pst.st_dev; q->ino = (uint64_t)pst.st_ino;
        q->tdev = (uint64_t)st.st_dev; q->tino = (uint64_t)st.st_ino;
        q->rw = 1;
    } else {
        fd = open_nolink(AT_FDCWD, path, O_PATH, kind == FPLEND_DIR);
        if (fd < 0 || fstat(fd, &st) || (kind == FPLEND_DIR ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode))) goto out;
        /* never a protected folder or file (launchers, security decisions), nor a folder that is or holds the home's
         * keys and startup files: those the launcher's grant rules refuse even read */
        if (fsops_protected_resolved(path) || (kind == FPLEND_DIR && !apps_grant_ok(path))) {
            fprintf(stderr, "fplend: %s: that %s isn't lent\n", id, kind == FPLEND_DIR ? "folder" : "file");
            goto out;
        }
        snprintf(q->path, sizeof q->path, "%s", path);
        q->dev = (uint64_t)st.st_dev; q->ino = (uint64_t)st.st_ino;
        q->rw = (uint32_t)apps_grant_ok(path);
    }
    char grant[33];
    int e = ask(v, q, grant);
    /* a mount the kernel keeps read-only (a disk mounted so) is lent read-only */
    if (e == EPERM && q->rw && kind != FPLEND_SAVE) {
        q->rw = 0;
        e = ask(v, q, grant);
    }
    if (e) { fprintf(stderr, "fplend: %s: could not lend the chosen %s (%s)\n", id, kind == FPLEND_DIR ? "folder" : "file", strerror(e)); goto out; }
    /* Flatpak 1.16 binds $XDG_RUNTIME_DIR/app/<id> at /run/flatpak/app/<id> in every instance (and at the same path) */
    if (snprintf(out, n, "/run/flatpak/app/%s/doc/%s/%s", id, grant, name) >= (int)n) goto out;
    r = 0;
out:
    if (fd >= 0) close(fd);
    if (pfd >= 0) close(pfd);
    free(q);
    return r;
}
