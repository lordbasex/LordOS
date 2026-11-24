/* vmlink.c — see vmlink.h. */
#include "vmlink.h"
#include "vmgroup.h"
#include "vmproto.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

int vm_link_fd_ok(int fd, char *why, size_t wn) {
    int dom = 0, type = 0, acc = 0;
    socklen_t l = sizeof dom;
    struct ucred cr;
    socklen_t cl = sizeof cr;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISSOCK(st.st_mode)) { snprintf(why, wn, "not a socket"); return 0; }
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &dom, &l) || dom != AF_UNIX) { snprintf(why, wn, "not an AF_UNIX socket"); return 0; }
    l = sizeof type;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &l) || type != SOCK_STREAM) { snprintf(why, wn, "not a stream socket"); return 0; }
    l = sizeof acc;
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &acc, &l) || acc) { snprintf(why, wn, "a listening socket"); return 0; }
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) || cl != sizeof cr || cr.pid <= 0 || cr.uid != getuid()) { snprintf(why, wn, "its peer isn't this account's"); return 0; }
    return 1;
}

void vm_link_runtime(char *out, size_t n) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0] == '/' && strlen(rt) < 200) snprintf(out, n, "%s", rt);
    else snprintf(out, n, "/run/user/%u", (unsigned)getuid());
}

static int connect_id(const char *rundir, const char *id) {
    char path[sizeof ((struct sockaddr_un *)0)->sun_path];
    if (!vm_uuid_valid(id) || (size_t)snprintf(path, sizeof path, "%s/lordos-vm/%s.sock", rundir, id) >= sizeof path) return -1;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path, path, strlen(path) + 1);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    return fd;
}

static long long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* the VM_REPLY to seq on fd, until the deadline: its code, or -1 */
static int await_reply(int fd, uint32_t seq, long long deadline, char *text, size_t tn) {
    for (;;) {
        long long left = deadline - mono_ms();
        if (left <= 0) return -1;
        struct pollfd pf = { fd, POLLIN, 0 };
        int pr = poll(&pf, 1, (int)left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) return -1;
        VmMsg r;
        int rfd = -1, got = vm_recv(fd, &r, &rfd);
        if (rfd >= 0) close(rfd);
        if (got < 0) return -1;
        if (got == 0) continue;
        if (r.type == VM_REPLY && r.seq == seq) { snprintf(text, tn, "%s", r.text); return r.a; }
    }
}

int vm_link_pair(const char *rundir, const char *a_id, const char *b_id, const char *group, int on, int timeout_ms, char *err, size_t en) {
    err[0] = 0;
    if (!vm_uuid_valid(group) || !strcmp(a_id, b_id)) { snprintf(err, en, "bad request"); return -1; }
    int fa = connect_id(rundir, a_id), fb = connect_id(rundir, b_id);
    if (fa < 0 || fb < 0) {
        snprintf(err, en, "%s isn't running", fa < 0 ? "the first member" : "the second member");
        if (fa >= 0) close(fa);
        if (fb >= 0) close(fb);
        return -1;
    }
    int sv[2] = { -1, -1 };
    if (on && socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) { snprintf(err, en, "socketpair: %s", strerror(errno)); close(fa); close(fb); return -1; }
    VmMsg qa = vm_msg(VM_LINK, 7, on, 0, 0, 0, group), qb = vm_msg(VM_LINK, 8, on, 0, 0, 0, group);
    int sa = vm_send(fa, &qa, sv[0]), sb = vm_send(fb, &qb, sv[1]);
    /* the ends are the lord-vms' now (or nobody's): none stays in this process */
    if (sv[0] >= 0) close(sv[0]);
    if (sv[1] >= 0) close(sv[1]);
    long long deadline = mono_ms() + (timeout_ms > 0 ? timeout_ms : 3000);
    char ta[VM_TEXT_MAX] = "", tb[VM_TEXT_MAX] = "";
    int ra = sa ? -1 : await_reply(fa, 7, deadline, ta, sizeof ta), rb = sb ? -1 : await_reply(fb, 8, deadline, tb, sizeof tb);
    close(fa);
    close(fb);
    if (ra == VME_OK && rb == VME_OK) return 0;
    snprintf(err, en, "%s: %s", ra != VME_OK ? "the first member" : "the second member",
             ra != VME_OK ? (ta[0] ? ta : "no answer") : (tb[0] ? tb : "no answer"));
    return -1;
}

int vm_link_group(const char *gdir, int on, int timeout_ms, char *err, size_t en) {
    err[0] = 0;
    VmGroup g;
    if (!vm_group_dir_ok(gdir) || vm_group_load(gdir, &g, err, en)) { if (!err[0]) snprintf(err, en, "not a group folder"); return -1; }
    int gw = vm_group_role(&g, VM_ROLE_GATEWAY), ws = vm_group_role(&g, VM_ROLE_WORKSTATION);
    if (gw < 0 || ws < 0) { snprintf(err, en, "the group has no gateway and workstation"); return -1; }
    char ids[2][37];
    const int idx[2] = { gw, ws };
    for (int k = 0; k < 2; k++) {
        char dir[VM_PATH_MAX + VMG_DIR_MAX + 2];
        VmMachine m;
        if ((size_t)snprintf(dir, sizeof dir, "%s/%s", gdir, g.m[idx[k]].dir) >= sizeof dir || vm_machine_load(dir, &m, err, en)) {
            if (!err[0]) snprintf(err, en, "a member can't be read");
            return -1;
        }
        if (strcmp(m.group, g.id) || m.role != g.m[idx[k]].role) { snprintf(err, en, "%s isn't this group's %s", g.m[idx[k]].dir, VMG_ROLE_NAMES[g.m[idx[k]].role]); return -1; }
        memcpy(ids[k], m.id, sizeof ids[k]);
    }
    char rundir[256];
    vm_link_runtime(rundir, sizeof rundir);
    return vm_link_pair(rundir, ids[0], ids[1], g.id, on, timeout_ms, err, en);
}
