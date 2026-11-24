/* vmwatch.h — the pure parts of what Sentinela checks of a running machine (js_vm.c stats, docs/SENTINELA.md
 * "Machines"): where its scope must be, whether a cgroup path is exactly that scope or below it, and whether a process's
 * program is the installed one. Tested by build/vmwatch-test. */
#ifndef LORD_VMWATCH_H
#define LORD_VMWATCH_H

#include "vmproto.h"

#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* The machine's scope, in the session's machines' slice (docs/VIRTUALIZATION.md 3.7): self ".../lordos-session-<uid>/server" →
 * ".../lordos-session-<uid>/lordos-machines.slice/app-lordos-lordos-vm-<id>.scope" (tor: a group's Tor gateway,
 * "app-lordos-lordos-vm-tor-<id>.scope", lordvm.c setup_cgroups); slice 0: where a machine started before the slice
 * existed is, beside lord-server's own leaf. "" (and -1) when self isn't a LordOS session's server leaf or id isn't a
 * plain machine id (hex digits and '-'). */
static inline int vmwatch_scope_path(const char *self, const char *id, int tor, int slice, char *out, size_t n) {
    if (n) out[0] = 0;
    if (!self || !id || !id[0] || strlen(id) > 36) return -1;
    for (const char *p = id; *p; p++) if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || *p == '-')) return -1;
    const char *sl = strrchr(self, '/');
    if (!sl || sl == self || strcmp(sl, "/server") || self[0] != '/') return -1;
    const char *sess = sl;
    while (sess > self && sess[-1] != '/') sess--;
    if (strncmp(sess, "lordos-session-", 15)) return -1;
    if ((size_t)snprintf(out, n, "%.*s/%sapp-lordos-lordos-vm-%s%s.scope", (int)(sl - self), self, slice ? "lordos-machines.slice/" : "",
                         tor ? "tor-" : "", id) >= n) { out[0] = 0; return -1; }
    return 0;
}

/* a /proc/<pid>/cgroup text ("0::<path>\n") names exactly `scope` or a cgroup below it: never a path that merely
 * contains it (another root, a lookalike name, a longer name with the same start) */
static inline int vmwatch_cg_under(const char *proc_cgroup, const char *scope) {
    if (!proc_cgroup || !scope || scope[0] != '/' || strncmp(proc_cgroup, "0::", 3)) return 0;
    const char *p = proc_cgroup + 3;
    size_t l = strlen(scope);
    if (strncmp(p, scope, l)) return 0;
    if (!(p[l] == 0 || p[l] == '\n' || p[l] == '/')) return 0;
    for (const char *q = p; *q && *q != '\n'; q++) if (q[0] == '/' && q[1] == '.' && q[2] == '.' && (q[3] == '/' || q[3] == 0 || q[3] == '\n')) return 0;
    return 1;
}

/* /proc/<pid>/exe's text is the installed program `want` (its real path); an upgraded one reads "<path> (deleted)" and
 * still counts */
static inline int vmwatch_exe_same(const char *exe, const char *want) {
    if (!exe || !want || want[0] != '/') return 0;
    size_t l = strlen(exe), w = strlen(want);
    if (l == w) return !strcmp(exe, want);
    return l == w + 10 && !strncmp(exe, want, w) && !strcmp(exe + w, " (deleted)");
}

/* A running machine's own lord-vm, asked over its socket as `lord-vm --status` asks: the peer's pid and uid from the
 * kernel (SO_PEERCRED, never from a file), its state and QEMU's pid (VM_STATE), the VMS_* sandbox bits and VMC_* cgroup
 * mode (VM_INFO). ms at most; 0 with both answers, -1 otherwise. The caller still checks that the peer runs the
 * installed lord-vm and that the QEMU is its child. */
typedef struct { long pid, qemu; unsigned uid; int state, sandbox, cg; } VmLive;
static inline int vmwatch_live(const char *sockpath, VmLive *lv, int ms) {
    memset(lv, 0, sizeof *lv);
    lv->state = -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (!sockpath || strlen(sockpath) >= sizeof a.sun_path) return -1;
    memcpy(a.sun_path, sockpath, strlen(sockpath) + 1);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) { if (fd >= 0) close(fd); return -1; }
    struct ucred cr;
    socklen_t cl = sizeof cr;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) || cr.pid <= 1) { close(fd); return -1; }
    lv->pid = cr.pid;
    lv->uid = cr.uid;
    VmMsg q = vm_msg(VM_STATUS, 9, 0, 0, 0, 0, "");
    if (vm_send(fd, &q, -1)) { close(fd); return -1; }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int info = 0, state = 0;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        int left = ms - (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        struct pollfd pf = { fd, POLLIN, 0 };
        if (left <= 0 || poll(&pf, 1, left) <= 0) break;
        VmMsg r;
        int rfd = -1;
        if (vm_recv(fd, &r, &rfd) <= 0) break;
        if (rfd >= 0) close(rfd);
        if (r.type == VM_INFO) { lv->sandbox = r.c; lv->cg = r.d; info = 1; }
        else if (r.type == VM_STATE) { lv->state = r.a; lv->qemu = r.d; state = 1; }
        if (info && state) break;
    }
    close(fd);
    return info && state ? 0 : -1;
}

#endif
