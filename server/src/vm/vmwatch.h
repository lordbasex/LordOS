/* vmwatch.h — the pure parts of what Sentinela checks of a running machine (js_vm.c stats, docs/SENTINELA.md
 * "Machines"): where its scope must be, whether a cgroup path is exactly that scope or below it, and whether a process's
 * program is the installed one. Tested by build/vmwatch-test. */
#ifndef LORD_VMWATCH_H
#define LORD_VMWATCH_H

#include "vmproto.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
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

/* lord-vm's own cgroup, from /proc/<pid>/cgroup: exactly the machine's scope, or its "supervisor" leaf when lord-vm
 * divided the scope (3.7). Never a leaf below it (QEMU's, passt's: processes the guest may reach) and never another
 * cgroup that merely starts the same way. */
static inline int vmwatch_cg_lordvm(const char *proc_cgroup, const char *scope) {
    if (!vmwatch_cg_under(proc_cgroup, scope)) return 0;
    const char *rest = proc_cgroup + 3 + strlen(scope);
    if (*rest == 0 || *rest == '\n') return 1;
    return !strncmp(rest, "/supervisor", 11) && (rest[11] == 0 || rest[11] == '\n');
}

/* ---- the lord-vm processes this lord-server vouches for (docs/SENTINELA.md "Machines") ----
 * lord-vm is non-dumpable (QEMU's user namespace is made while it still is), so /proc/<lord-vm>/exe can't be read by the
 * person's other processes, lord-server included. What attests a machine's lord-vm instead: lord-server holds a pidfd of
 * every lord-vm it starts (the child it forked and exec'd from the installed program, or, for a detached start, the pidfd
 * the installed lord-vm sends of itself over a socketpair only it was given), with the machine's id and the process's
 * start time. A peer of the machine's socket is that lord-vm only when its SO_PEERCRED pid is one held for that machine
 * and the pidfd still names a live process. A lord-vm this lord-server didn't start (it restarted) is adopted only with
 * the kernel's pidfd of the socket's peer (SO_PEERPIDFD) when that process is in the machine's own scope or its
 * supervisor leaf, where lord-sessiond places only the installed LordOS programs of the session's server leaf; it stays
 * unverified otherwise. */
#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif
#define VMWATCH_PROCS 32
typedef struct { long pid; int pidfd; unsigned long long start; char id[37]; int adopted; } VmProcRec;

/* the pidfd still names a live process (pidfd_send_signal with signal 0) */
static inline int vmwatch_pidfd_alive(int pidfd) {
    return pidfd >= 0 && syscall(SYS_pidfd_send_signal, pidfd, 0, NULL, 0) == 0;
}

/* the pid a pidfd names, from the kernel (/proc/self/fdinfo's "Pid:"), -1 when fd isn't a pidfd of a live process */
static inline long vmwatch_pidfd_pid(int pidfd) {
    char p[64], buf[1024];
    if (pidfd < 0) return -1;
    snprintf(p, sizeof p, "/proc/self/fdinfo/%d", pidfd);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    const char *s = strstr(buf, "\nPid:");
    if (!s) return -1;
    long v = strtol(s + 5, NULL, 10);
    return v > 1 ? v : -1;
}

/* the process's start time (clock ticks after boot, /proc/<pid>/stat field 22), 0 unknown */
static inline unsigned long long vmwatch_start_time(long pid) {
    char p[64], buf[1024];
    snprintf(p, sizeof p, "/proc/%ld/stat", pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    const char *s = strrchr(buf, ')');          /* comm may hold spaces and ')': fields count from the last one */
    if (!s) return 0;
    int field = 2;
    for (s++; *s && field < 22; s++) if (*s == ' ') field++;
    return field == 22 ? strtoull(s, NULL, 10) : 0;
}

/* the dead ones' pidfds closed and their places freed */
static inline void vmwatch_procs_prune(VmProcRec *t, int n) {
    for (int i = 0; i < n; i++)
        if (t[i].pid && !vmwatch_pidfd_alive(t[i].pidfd)) { if (t[i].pidfd >= 0) close(t[i].pidfd); memset(&t[i], 0, sizeof t[i]); t[i].pidfd = -1; }
}

/* id's lord-vm with pidfd (taken: the table closes it); its pid from the pidfd itself and its start time read while the
 * pidfd shows it alive before and after. 0 kept, -1 not (bad id, not a live pidfd, no room: pidfd closed) */
static inline int vmwatch_procs_add(VmProcRec *t, int n, const char *id, int pidfd, int adopted) {
    size_t il = id ? strlen(id) : 0;
    int ok = il > 0 && il <= 36;
    for (size_t i = 0; ok && i < il; i++) if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f') || id[i] == '-')) ok = 0;
    long pid = ok ? vmwatch_pidfd_pid(pidfd) : -1;
    unsigned long long st = pid > 1 ? vmwatch_start_time(pid) : 0;
    if (pid <= 1 || !st || !vmwatch_pidfd_alive(pidfd)) { if (pidfd >= 0) close(pidfd); return -1; }
    vmwatch_procs_prune(t, n);
    int slot = -1;
    for (int i = 0; i < n; i++) {
        if (t[i].pid == pid && !strcmp(t[i].id, id)) { close(pidfd); return 0; }   /* the same one again */
        if (!t[i].pid && slot < 0) slot = i;
    }
    if (slot < 0) { close(pidfd); return -1; }
    t[slot].pid = pid;
    t[slot].pidfd = pidfd;
    t[slot].start = st;
    t[slot].adopted = adopted;
    memcpy(t[slot].id, id, il + 1);
    return 0;
}

/* the record of id's lord-vm whose pid is pid, still alive and still the same process (its start time unchanged); NULL
 * otherwise: a mismatch is never verified */
static inline const VmProcRec *vmwatch_procs_match(VmProcRec *t, int n, const char *id, long pid) {
    if (!id || !id[0] || pid <= 1) return NULL;
    for (int i = 0; i < n; i++) {
        if (t[i].pid != pid || strcmp(t[i].id, id)) continue;
        if (!vmwatch_pidfd_alive(t[i].pidfd) || vmwatch_start_time(pid) != t[i].start) return NULL;
        return &t[i];
    }
    return NULL;
}

/* A running machine's own lord-vm, asked over its socket as `lord-vm --status` asks: the peer's pid and uid from the
 * kernel (SO_PEERCRED, never from a file), its pidfd (SO_PEERPIDFD, -1 on a kernel without it; the caller closes it),
 * its state and QEMU's pid (VM_STATE), the VMS_* sandbox bits and VMC_* cgroup mode (VM_INFO). ms at most; 0 with both
 * answers, -1 otherwise. The caller still checks that the peer is a lord-vm it vouches for (vmwatch_procs_match) and
 * that the QEMU is its child. */
typedef struct { long pid, qemu; unsigned uid; int state, sandbox, cg, pidfd; } VmLive;
static inline int vmwatch_live(const char *sockpath, VmLive *lv, int ms) {
    memset(lv, 0, sizeof *lv);
    lv->state = -1;
    lv->pidfd = -1;
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
    {
        int pfd = -1;
        socklen_t pl = sizeof pfd;
        if (!getsockopt(fd, SOL_SOCKET, SO_PEERPIDFD, &pfd, &pl) && pfd >= 0) {
            fcntl(pfd, F_SETFD, FD_CLOEXEC);
            if (vmwatch_pidfd_pid(pfd) == cr.pid) lv->pidfd = pfd; else close(pfd);
        }
    }
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
    if (!(info && state) && lv->pidfd >= 0) { close(lv->pidfd); lv->pidfd = -1; }
    return info && state ? 0 : -1;
}

#endif
