/* netconn.c — sockets for LordOS's own components, created in their own scopes (see netconn.h). */
#include "netconn.h"
#include "cgroup.h"
#include "../fw/rules.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *const NAMES[NC_COUNT] = { "mail", "print", "lists", "lan" };

const char *netconn_name(NcComponent c) { return (unsigned)c < NC_COUNT ? NAMES[c] : NULL; }

/* ---- the request ---- */

int netconn_req_ok(const NcReq *r) {
    if (!r || r->magic != NC_MAGIC || r->pad || r->pad2) return -1;
    if (r->fam != 4 && r->fam != 6) return -1;
    if (r->type != 1 && r->type != 2) return -1;
    if (r->connect > 1) return -1;
    if (r->connect && !r->port) return -1;
    if (!r->connect && (r->port || r->scope_id)) return -1;
    if (r->fam == 4) for (int i = 4; i < 16; i++) if (r->addr[i]) return -1;
    if (r->fam == 4 && r->scope_id) return -1;
    return 0;
}

int netconn_req_make(NcReq *r, int fam, int type, const struct sockaddr *to, socklen_t tolen) {
    memset(r, 0, sizeof *r);
    r->magic = NC_MAGIC;
    if (fam == AF_INET) r->fam = 4;
    else if (fam == AF_INET6) r->fam = 6;
    else return -1;
    if (type == SOCK_STREAM) r->type = 1;
    else if (type == SOCK_DGRAM) r->type = 2;
    else return -1;
    if (to) {
        if (to->sa_family != fam) return -1;
        r->connect = 1;
        if (fam == AF_INET) {
            if (tolen < (socklen_t)sizeof(struct sockaddr_in)) return -1;
            const struct sockaddr_in *a = (const struct sockaddr_in *)(const void *)to;
            memcpy(r->addr, &a->sin_addr, 4);
            r->port = ntohs(a->sin_port);
        } else {
            if (tolen < (socklen_t)sizeof(struct sockaddr_in6)) return -1;
            const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)(const void *)to;
            memcpy(r->addr, &a->sin6_addr, 16);
            r->port = ntohs(a->sin6_port);
            r->scope_id = a->sin6_scope_id;
        }
    }
    return netconn_req_ok(r);
}

int netconn_serve_one(const NcReq *r) {
    if (netconn_req_ok(r)) return -EINVAL;
    int fam = r->fam == 4 ? AF_INET : AF_INET6;
    int fd = socket(fam, (r->type == 1 ? SOCK_STREAM : SOCK_DGRAM) | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    if (r->connect) {
        int rc;
        if (fam == AF_INET) {
            struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(r->port) };
            memcpy(&a.sin_addr, r->addr, 4);
            rc = connect(fd, (struct sockaddr *)&a, sizeof a);
        } else {
            struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_port = htons(r->port), .sin6_scope_id = r->scope_id };
            memcpy(&a.sin6_addr, r->addr, 16);
            rc = connect(fd, (struct sockaddr *)&a, sizeof a);
        }
        if (rc && errno != EINPROGRESS) { int e = errno; close(fd); return -e; }
    }
    return fd;
}

/* ---- axia-server's side ---- */

static struct { int fd; pid_t pid; } conn[NC_COUNT] = { { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 } };
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static void drop(NcComponent c) {
    if (conn[c].fd >= 0) close(conn[c].fd);
    conn[c].fd = -1;
    if (conn[c].pid > 0) { kill(conn[c].pid, SIGKILL); waitpid(conn[c].pid, NULL, 0); }
    conn[c].pid = 0;
}

static int spawn(NcComponent c) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    char argv0[32];
    snprintf(argv0, sizeof argv0, "lord-net-%s", NAMES[c]);
    char envs[2][64];
    char *envp[3];
    int ne = 0;
    if (getenv("LORDOS_SESSIOND")) { snprintf(envs[ne], sizeof envs[ne], "LORDOS_SESSIOND=1"); envp[ne] = envs[ne]; ne++; }
    snprintf(envs[ne], sizeof envs[ne], "LANG=C.UTF-8");
    envp[ne] = envs[ne];
    ne++;
    envp[ne] = NULL;
    char *args[] = { argv0, "--net-connector", (char *)NAMES[c], NULL };
    pid_t p = fork();
    if (p == 0) {
        /* only the channel (fd 3) and stderr (the session log); no DRM, input or other fds */
        int ch = fcntl(sv[1], F_DUPFD, 10);
        dup2(ch, 3);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); }
        syscall(SYS_close_range, 4, ~0u, 0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        execve("/proc/self/exe", args, envp);
        _exit(127);
    }
    close(sv[1]);
    if (p < 0) { close(sv[0]); return -1; }
    conn[c].fd = sv[0];
    conn[c].pid = p;
    return 0;
}

/* One round trip; 0 with *out the fd, or -errno. */
static int ask(NcComponent c, const NcReq *r, int *out) {
    if (send(conn[c].fd, r, sizeof *r, MSG_NOSIGNAL) != (ssize_t)sizeof *r) return -EPIPE;
    struct pollfd p = { conn[c].fd, POLLIN, 0 };
    if (poll(&p, 1, 5000) != 1) return -ETIMEDOUT;      /* a first request also waits for its placement */
    int err = 0;
    struct iovec iov = { &err, sizeof err };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } cm;
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cm.b, .msg_controllen = sizeof cm.b };
    ssize_t n = recvmsg(conn[c].fd, &m, MSG_CMSG_CLOEXEC);
    if (n != (ssize_t)sizeof err) return -EPIPE;
    int fd = -1;
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h))
        if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS && h->cmsg_len == CMSG_LEN(sizeof(int))) memcpy(&fd, CMSG_DATA(h), sizeof fd);
    if (err) { if (fd >= 0) close(fd); return err < 0 ? err : -EIO; }
    if (fd < 0) return -EIO;
    /* what came back must be the socket that was asked for */
    int type = 0, dom = 0;
    socklen_t tl = sizeof type, dl = sizeof dom;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) || getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &dom, &dl) ||
        type != (r->type == 1 ? SOCK_STREAM : SOCK_DGRAM) || dom != (r->fam == 4 ? AF_INET : AF_INET6)) {
        close(fd);
        return -EIO;
    }
    *out = fd;
    return 0;
}

int netconn_socket(NcComponent comp, int fam, int type, const struct sockaddr *to, socklen_t tolen) {
    NcReq r;
    if ((unsigned)comp >= NC_COUNT || netconn_req_make(&r, fam, type, to, tolen)) { errno = EINVAL; return -1; }
    /* outside a LordOS session there are no scopes to put the sockets in (development, the
     * tests): the socket is made here, as before */
    if (!getenv("LORDOS_SESSIOND")) {
        int fd = netconn_serve_one(&r);
        if (fd < 0) { errno = -fd; return -1; }
        return fd;
    }
    pthread_mutex_lock(&mu);
    int fd = -1, rc = -EPIPE;
    for (int attempt = 0; attempt < 2 && (rc == -EPIPE || rc == -ETIMEDOUT); attempt++) {
        if (conn[comp].fd < 0 && spawn(comp)) { rc = -EAGAIN; break; }
        rc = ask(comp, &r, &fd);
        if (rc == -EPIPE || rc == -ETIMEDOUT) {
            fprintf(stderr, "netconn: the %s connector did not answer (%s)%s\n", NAMES[comp], strerror(-rc), attempt ? "" : "; starting it again");
            drop(comp);
        }
    }
    pthread_mutex_unlock(&mu);
    if (rc) { errno = -rc; return -1; }
    return fd;
}

/* ---- the connector ---- */

int netconn_main(int argc, char **argv) {
    if (argc != 3 || !fw_component_ok(argv[2])) { fprintf(stderr, "axia-server --net-connector: unknown component\n"); return 2; }
    int known = 0;
    for (int i = 0; i < NC_COUNT; i++) known |= !strcmp(argv[2], NAMES[i]);
    if (!known) return 2;
    /* no PR_SET_PDEATHSIG: it fires when the *thread* that forked us ends (a worker thread of
     * axia-server), not axia-server; the channel closing (recv → 0) is what ends the connector */
    prctl(PR_SET_DUMPABLE, 0);
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    signal(SIGPIPE, SIG_IGN);
    struct rlimit nf = { 64, 64 };
    setrlimit(RLIMIT_NOFILE, &nf);
    int type = 0;
    socklen_t tl = sizeof type;
    if (getsockopt(3, SOL_SOCKET, SO_TYPE, &type, &tl) || type != SOCK_SEQPACKET) { fprintf(stderr, "axia-server --net-connector: started by axia-server only\n"); return 2; }
    char scope[64];
    snprintf(scope, sizeof scope, "lordos-net-%s.scope", argv[2]);
    CgroupLimits lim = { .mem_high_mb = 0, .mem_max_mb = 64, .cpu_percent = 0 };
    if (cgroup_enter(scope, &lim) && getenv("LORDOS_SESSIOND")) {
        fprintf(stderr, "netconn: could not place the %s connector in %s: its connections are refused\n", argv[2], scope);
        return 3;                           /* fail closed: axia-server's requests get EPIPE */
    }
    if (getenv("LORDOS_SESSIOND")) cgroup_settle();   /* axia-netperm sees the scope before the first connect */
    for (;;) {
        NcReq r;
        ssize_t n = recv(3, &r, sizeof r, 0);
        if (n == 0) return 0;
        if (n < 0) { if (errno == EINTR) continue; return 1; }
        int fd = n == (ssize_t)sizeof r ? netconn_serve_one(&r) : -EINVAL;
        int err = fd < 0 ? fd : 0;
        struct iovec iov = { &err, sizeof err };
        union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } cm;
        struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
        if (fd >= 0) {
            memset(&cm, 0, sizeof cm);
            m.msg_control = cm.b;
            m.msg_controllen = sizeof cm.b;
            struct cmsghdr *h = CMSG_FIRSTHDR(&m);
            h->cmsg_level = SOL_SOCKET;
            h->cmsg_type = SCM_RIGHTS;
            h->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(h), &fd, sizeof fd);
        }
        if (sendmsg(3, &m, MSG_NOSIGNAL) < 0 && errno == EPIPE) return 0;
        if (fd >= 0) close(fd);
    }
}
