/* vm_jail_probe.c — QEMU's jail as a compromised QEMU would find it (docs/VIRTUALIZATION.md 3.3, vmsandbox.h).
 *
 * The probe goes through lord-vm's own steps for QEMU: vm_userns_open, fork, vm_ns_enter, vm_ns_setup, vm_jail_qemu (its
 * Landlock and the seccomp allowlist), then the exec by descriptor, so the kernel decides its dumpability as it does
 * QEMU's. What runs after exec is this program again (a copy in the jail's disks folder, started through the dynamic
 * loader under /usr): code that already runs inside QEMU, the case the jail is for. It tries what such code would, and
 * reports each try. The outside half is the person's other programs (this process is even the probe's ancestor, which
 * Yama alone would let in): it may not read the probe's memory, environment, descriptors or maps, nor ptrace it.
 *
 * Then the seccomp allowlist (vmseccomp.h) as lord-vm runs it: the filter's listener comes back to this process over a
 * socket (the jailed side never keeps it), and each call that isn't QEMU's is tried by a fresh jailed probe. The call
 * must arrive here, named, before it runs; this process then kills the probe, which never goes on. Without a listener
 * the same call kills the probe with SIGSYS. What must keep working is tried too (threads, TCG's writable and executable
 * memory on a TCG machine).
 *
 *   build/vm-jail-probe          PASS/FAIL lines, exit 1 on a FAIL; "skipped: needs ..." and exit 0 without user
 *                                namespaces or Landlock
 *   build/vm-jail-probe --before the jail as it was before the namespaces, to compare: its FAILs are what they close
 * No QEMU, KVM or root. */
#include "../src/vm/vmsandbox.h"
#include "../src/vm/vmproto.h"
#include "../src/vm/vmseccomp.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails, passes;
static void check(int ok, const char *what, int err) {
    if (ok) { passes++; printf("PASS  %s\n", what); }
    else { fails++; printf("FAIL  %s (%s)\n", what, err ? strerror(err) : "allowed"); }
    fflush(stdout);
}
/* the call must fail (with `want`, or any error when 0): the error is taken before anything else runs */
#define REFUSED(expr, want, what) do { errno = 0; int r_ = (expr) < 0, e_ = errno; \
        if (r_ && (want) && e_ != (want)) { fails++; printf("FAIL  %s (refused, but %s)\n", what, strerror(e_)); fflush(stdout); } \
        else check(r_, what, 0); } while (0)

/* ---- inside: code running as QEMU, after exec ---- */

static void *thread_main(void *a) { return a; }

static int inside(char **argv) {
    const char *w = argv[2];
    pid_t sib = (pid_t)atoi(argv[3]), outside = (pid_t)atoi(argv[4]);
    uid_t uid = (uid_t)atoi(argv[5]);
    char p[PATH_MAX + 64];
    int e;
    check(getuid() == uid, "inside: its uid is the person's (mapped)", 0);
    check(getgid() == 65534, "inside: its group isn't mapped (overflow gid)", 0);
    check(prctl(PR_GET_DUMPABLE) == 0, "inside: non-dumpable after exec", 0);
    /* no pid namespace (vmsandbox.h): a sibling can still be named; the allowlist stops kill, prlimit64, setpriority,
     * ptrace and process_vm_readv on it (the kill cases, one probe each) */
    (void)outside;
    snprintf(p, sizeof p, "/proc/%d/status", (int)sib);
    REFUSED(open(p, O_RDONLY), 0, "inside: can't read a sibling's /proc");
    REFUSED(open("/proc/1/environ", O_RDONLY), 0, "inside: can't read init's /proc/1");
    errno = 0;
    DIR *d = opendir("/proc");
    e = errno;
    check(!d, "inside: can't list /proc", e);
    if (d) closedir(d);
    errno = 0;
    check(open("/proc/self/status", O_RDONLY) >= 0, "inside: reads its own /proc/self (control)", errno);
    const char *home = getenv("PROBE_HOME");
    if (home && home[0]) {
        snprintf(p, sizeof p, "%s/.ssh", home);
        REFUSED(open(p, O_RDONLY | O_DIRECTORY), 0, "inside: can't open ~/.ssh");
        REFUSED(open(home, O_RDONLY | O_DIRECTORY), 0, "inside: can't list the home folder");
    }
    snprintf(p, sizeof p, "%s/secret", w);
    REFUSED(open(p, O_RDONLY), 0, "inside: can't read a file beside its disks folder");
    snprintf(p, sizeof p, "%s/written", w);
    REFUSED(open(p, O_WRONLY | O_CREAT, 0600), 0, "inside: can't write outside its disks folder");
    REFUSED(open("/tmp/vm-jail-probe-escape", O_WRONLY | O_CREAT, 0600), 0, "inside: can't write in /tmp");
    snprintf(p, sizeof p, "%s/disks/disk.img", w);
    int fd = open(p, O_RDWR);
    e = errno;
    check(fd >= 0 && write(fd, "ok", 2) == 2, "inside: writes its own disk (control)", e);
    if (fd >= 0) close(fd);
    REFUSED(open("/etc/hostname", O_RDONLY), 0, "inside: can't read /etc");
    REFUSED(socket(AF_UNIX, SOCK_STREAM, 0), EACCES, "inside: can't make a socket (EACCES)");
    REFUSED(syscall(SYS_io_uring_setup, 8, NULL), EPERM, "inside: no io_uring (EPERM)");
    REFUSED(prctl(PR_SET_DUMPABLE, 1), EPERM, "inside: can't make itself dumpable again (EPERM)");
    check(prctl(PR_GET_DUMPABLE) == 0, "inside: still non-dumpable", 0);
    /* the allowlist's listener is lord-vm's alone: none of this process's descriptors is one */
    int held = 0;
    for (int k = 0; k < 1024; k++) {
        char lp[64], lt[128];
        snprintf(lp, sizeof lp, "/proc/self/fd/%d", k);
        ssize_t ln = readlink(lp, lt, sizeof lt - 1);
        if (ln > 0) { lt[ln] = 0; if (strstr(lt, "seccomp")) held = 1; }
    }
    check(!held, "inside: holds no seccomp listener", 0);
    /* what QEMU does keeps working (controls) */
    pthread_t th;
    int pe = pthread_create(&th, NULL, thread_main, NULL);
    check(!pe && !pthread_join(th, NULL), "inside: makes a thread (clone3 ENOSYS, then clone) (control)", pe);
    errno = 0;
    void *rx = mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(rx != MAP_FAILED, "inside: maps executable memory that isn't writable (control)", errno);
    errno = 0;
    check(eventfd(0, EFD_CLOEXEC) >= 0, "inside: eventfd (control)", errno);
    errno = 0;
    char tio[64];
    check(ioctl(1, 0x5401, tio) < 0 && errno == ENOTTY, "inside: isatty on a pipe answers ENOTTY (control)", errno);
    REFUSED(fork(), EPERM, "inside: can't make a process (EPERM)");
    /* alive until the outside half is done with it */
    char c;
    while (read(0, &c, 1) > 0) {}
    return fails ? 1 : 0;
}

/* one call the allowlist must stop: it never returns here (the outside kills this process at the notification) */
static int kill_case(const char *what, pid_t sib) {
    struct rlimit rl;
    char b[64];
    struct iovec l = { b, sizeof b }, r = { (void *)0x400000, sizeof b };
    long ret = -1;
    if (!strcmp(what, "unshare")) ret = unshare(CLONE_NEWUSER);
    else if (!strcmp(what, "setns")) ret = setns(0, 0);
    else if (!strcmp(what, "ptrace")) ret = ptrace(PTRACE_ATTACH, sib, 0, 0);
    else if (!strcmp(what, "process_vm_readv")) ret = process_vm_readv(sib, &l, 1, &r, 1, 0);
    else if (!strcmp(what, "prlimit64")) ret = prlimit(sib, RLIMIT_CORE, NULL, &rl);
    else if (!strcmp(what, "setpriority")) ret = setpriority(PRIO_PROCESS, (id_t)sib, 0);
    else if (!strcmp(what, "kill")) ret = kill(sib, 0);
    else if (!strcmp(what, "ioctl-tiocsti")) ret = ioctl(0, 0x5412, "x");
    else if (!strcmp(what, "ioctl-kvm")) ret = ioctl(0, 0xAE00, 0);
    else if (!strcmp(what, "mmap-wx")) ret = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED ? -1 : 0;
    else if (!strcmp(what, "mprotect-wx")) {
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ret = mprotect(p, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
    }
    else if (!strcmp(what, "execve")) { char *a[] = { "/usr/bin/true", NULL }; ret = execve(a[0], a, a + 1); }
    else if (!strcmp(what, "bpf")) ret = syscall(SYS_bpf, 0, NULL, 0);
    else if (!strcmp(what, "perf_event_open")) ret = syscall(SYS_perf_event_open, NULL, 0, -1, -1, 0);
    else if (!strcmp(what, "userfaultfd")) ret = syscall(SYS_userfaultfd, 0);
    else if (!strcmp(what, "keyctl")) ret = syscall(SYS_keyctl, 0, 0);
    else if (!strcmp(what, "mount")) ret = syscall(SYS_mount, "none", "/tmp", "tmpfs", 0, NULL);
    else if (!strcmp(what, "connect")) ret = syscall(SYS_connect, 3, NULL, 0);
    else if (!strcmp(what, "personality")) ret = syscall(SYS_personality, 0x0400000);
    else if (!strcmp(what, "prctl-ptracer")) ret = prctl(0x59616d61, -1);
    else if (!strcmp(what, "seccomp-listener")) {
        /* refused with EPERM, never a kill: a listener QEMU held could answer for its own calls */
        struct sock_filter f[] = { BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW) };
        struct sock_fprog pr = { 1, f };
        ret = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &pr);
        printf("%s %ld\n", ret < 0 && errno == EPERM ? "REFUSED" : "SURVIVED", ret);
        return 0;
    }
    else if (!strcmp(what, "mmap-wx-tcg")) {
        /* TCG's code buffer: allowed on a TCG machine (no kill) */
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        printf("%s\n", p != MAP_FAILED ? "ALLOWED" : "REFUSED");
        return 0;
    }
    else { printf("UNKNOWN %s\n", what); return 2; }
    printf("SURVIVED %ld %d\n", ret, errno);
    return 1;
}

/* ---- outside: lord-vm's steps, then the person's other programs ---- */

static int copy_file(const char *from, const char *to, mode_t mode) {
    int s = open(from, O_RDONLY | O_CLOEXEC), d = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    struct stat st;
    int ok = s >= 0 && d >= 0 && !fstat(s, &st);
    off_t off = 0;
    while (ok && off < st.st_size) { ssize_t n = sendfile(d, s, &off, (size_t)(st.st_size - off)); if (n <= 0) ok = 0; }
    if (s >= 0) close(s);
    if (d >= 0) close(d);
    return ok ? 0 : -1;
}

static int userns = -1, before;
static char loader[PATH_MAX], probe[PATH_MAX + 64], disks[PATH_MAX + 16];

/* lord-vm's child: namespaces, the private loader, the jail (with a listener when `notify`), the listener sent on `st`
 * (lord-vm's way: SCM_RIGHTS, then closed), the exec of the loader by descriptor with `args` */
static pid_t jailed(char **args, int out, int hold, int st, int notify, int tcg) {
    pid_t q = fork();
    if (q) return q;
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    dup2(out, 1);
    dup2(out, 2);
    dup2(hold, 0);
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!before && (!vm_ns_enter(userns) || vm_ns_setup(rt && rt[0] ? rt : "/dev/shm") != VMNS_LOADER)) { printf("FAIL  namespaces or the private loader\n"); _exit(1); }
    /* the program is the loader under /usr (Landlock lets only /usr run); in this mount namespace it is the
     * unreadable copy, so this exec makes the probe non-dumpable exactly as QEMU's does */
    int xfd = open(loader, O_PATH | O_CLOEXEC);
    int lfd = -1;
    VmQemuJail j = { .disks_dir = disks, .exec_fd = xfd, .tcg = tcg, .notify_out = notify ? &lfd : NULL };
    int mask = xfd >= 0 ? vm_jail_qemu(&j) : 0;
    if (!(mask & VMS_SECCOMP_OURS) || !(mask & VMS_SECCOMP_ALLOWLIST) || !(mask & VMS_NO_NEW_PRIVS) || !(mask & VMS_LANDLOCK_FS) ||
        (notify && !(mask & VMS_SECCOMP_NAMED))) { printf("FAIL  jail: mask %d\n", mask); _exit(1); }
    if (st >= 0) {
        union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } cm;
        memset(&cm, 0, sizeof cm);
        struct iovec io = { &mask, sizeof mask };
        struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cm.b, .msg_controllen = sizeof cm.b };
        struct cmsghdr *ch = CMSG_FIRSTHDR(&mh);
        ch->cmsg_level = SOL_SOCKET; ch->cmsg_type = SCM_RIGHTS; ch->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(ch), &lfd, sizeof(int));
        if (sendmsg(st, &mh, 0) != (ssize_t)sizeof mask) _exit(1);
        close(lfd);
        close(st);
    }
    char henv[PATH_MAX + 16];
    const char *h = getenv("HOME");
    snprintf(henv, sizeof henv, "PROBE_HOME=%s", h ? h : "");
    char *env[] = { henv, NULL };
    syscall(SYS_execveat, xfd, "", args, env, AT_EMPTY_PATH);
    printf("FAIL  exec: %s\n", strerror(errno));
    _exit(127);
}

/* one kill case: the call must reach the listener named `want`, before it runs; the probe is killed then */
static void kill_one(const char *what, const char *want, pid_t sib, int notify) {
    int out[2], hold[2], st[2];
    if (pipe2(out, O_CLOEXEC) || pipe2(hold, O_CLOEXEC) || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, st)) { check(0, what, errno); return; }
    char sibs[16];
    snprintf(sibs, sizeof sibs, "%d", (int)sib);
    char *args[] = { loader, probe, "--kill", (char *)what, sibs, NULL };
    pid_t q = jailed(args, out[1], hold[0], notify ? st[1] : -1, notify, 0);
    close(out[1]); close(hold[0]); close(st[1]);
    int lfd = -1;
    if (notify) {
        int mask = 0;
        union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } cm;
        struct iovec io = { &mask, sizeof mask };
        struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cm.b, .msg_controllen = sizeof cm.b };
        if (recvmsg(st[0], &mh, MSG_CMSG_CLOEXEC | MSG_WAITALL) == (ssize_t)sizeof mask) {
            struct cmsghdr *ch = CMSG_FIRSTHDR(&mh);
            if (ch && ch->cmsg_type == SCM_RIGHTS) memcpy(&lfd, CMSG_DATA(ch), sizeof(int));
        }
    }
    close(st[0]);
    char msg[256];
    int named = 0, ran_on = 0;
    if (lfd >= 0) {
        struct seccomp_notif_sizes sz;
        static char req_b[512], resp_b[256];
        struct pollfd pf = { lfd, POLLIN, 0 };
        if (!syscall(SYS_seccomp, SECCOMP_GET_NOTIF_SIZES, 0, &sz) && sz.seccomp_notif <= sizeof req_b && poll(&pf, 1, 10000) == 1) {
            struct seccomp_notif *req = (struct seccomp_notif *)req_b;
            memset(req_b, 0, sizeof req_b);
            if (!ioctl(lfd, SECCOMP_IOCTL_NOTIF_RECV, req)) {
                char d[160];
                vm_sc_describe(&req->data, d, sizeof d);
                const char *nm = vm_sc_name(req->data.nr);
                named = nm && !strcmp(nm, want);
                snprintf(msg, sizeof msg, "%s", d);
                /* lord-vm's order: the process first, then the answer (the call never runs) */
                kill(q, SIGKILL);
                struct seccomp_notif_resp *resp = (struct seccomp_notif_resp *)resp_b;
                memset(resp_b, 0, sizeof resp_b);
                resp->id = req->id;
                resp->error = -EPERM;
                ioctl(lfd, SECCOMP_IOCTL_NOTIF_SEND, resp);
            } else snprintf(msg, sizeof msg, "no notification: %s", strerror(errno));
        } else snprintf(msg, sizeof msg, "no notification in 10 s");
        close(lfd);
    } else if (notify) snprintf(msg, sizeof msg, "no listener came back");
    else msg[0] = 0;
    char line[256];
    FILE *in = fdopen(out[0], "r");
    while (in && fgets(line, sizeof line, in)) if (!strncmp(line, "SURVIVED", 8) || !strncmp(line, "FAIL", 4)) { ran_on = 1; fputs(line, stdout); }
    if (in) fclose(in);
    close(hold[1]);
    int ws = 0;
    waitpid(q, &ws, 0);
    char what_[200];
    if (notify) {
        snprintf(what_, sizeof what_, "allowlist: %s stops the process before it runs, named \"%s\" (%s)", what, want, msg);
        check(named && !ran_on && WIFSIGNALED(ws) && WTERMSIG(ws) == SIGKILL, what_, 0);
    } else {
        snprintf(what_, sizeof what_, "allowlist without a listener: %s kills the process (SIGSYS)", what);
        check(!ran_on && WIFSIGNALED(ws) && WTERMSIG(ws) == SIGSYS, what_, 0);
    }
}

/* a call that must be answered, not stopped: the probe prints `want` */
static void answered(const char *what, const char *want, int tcg, const char *label) {
    int out[2], hold[2];
    if (pipe2(out, O_CLOEXEC) || pipe2(hold, O_CLOEXEC)) { check(0, label, errno); return; }
    char *args[] = { loader, probe, "--kill", (char *)what, "1", NULL };
    pid_t q = jailed(args, out[1], hold[0], -1, 0, tcg);
    close(out[1]); close(hold[0]);
    char line[256] = "";
    FILE *in = fdopen(out[0], "r");
    int got = 0;
    while (in && fgets(line, sizeof line, in)) if (!strncmp(line, want, strlen(want))) got = 1;
    if (in) fclose(in);
    close(hold[1]);
    int ws = 0;
    waitpid(q, &ws, 0);
    check(got && WIFEXITED(ws), label, 0);
}

int main(int argc, char **argv) {
    if (argc == 6 && !strcmp(argv[1], "--inside")) return inside(argv);
    if (argc == 4 && !strcmp(argv[1], "--kill")) return kill_case(argv[2], (pid_t)atoi(argv[3]));
    if (vm_landlock_abi() < 4) { printf("skipped: needs Landlock ABI 4 or later\n"); return 0; }
    userns = vm_userns_open();
    if (userns < 0) { printf("skipped: needs unprivileged user namespaces (%s)\n", strerror(errno)); return 0; }
    /* lord-vm is non-dumpable from here on; so is its child until exec */
    prctl(PR_SET_DUMPABLE, 0);
    char w[] = "/tmp/vm-jail-probe.XXXXXX", p[PATH_MAX + 64];
    if (!mkdtemp(w)) { perror("mkdtemp"); return 1; }
    snprintf(disks, sizeof disks, "%s/disks", w);
    mkdir(disks, 0700);
    snprintf(p, sizeof p, "%s/secret", w);
    FILE *f = fopen(p, "w");
    if (f) { fputs("not for QEMU\n", f); fclose(f); }
    snprintf(p, sizeof p, "%s/disk.img", disks);
    f = fopen(p, "w");
    if (f) fclose(f);
    snprintf(probe, sizeof probe, "%s/probe", disks);
    if (copy_file("/proc/self/exe", probe, 0700)) { perror("copy"); return 1; }
#if defined(__x86_64__)
    const char *ld = "/lib64/ld-linux-x86-64.so.2";
#else
    const char *ld = "/lib/ld-linux-aarch64.so.1";
#endif
    if (!realpath(ld, loader)) { printf("skipped: no loader at %s\n", ld); return 0; }
    pid_t sib = fork();
    if (!sib) { prctl(PR_SET_PDEATHSIG, SIGKILL); for (;;) pause(); }
    int out[2], hold[2];
    if (pipe2(out, O_CLOEXEC) || pipe2(hold, O_CLOEXEC)) return 1;
    pid_t me = getpid();
    /* --before: the jail as it was until 0.8.9.5 (no namespaces, the loader readable), to compare; its FAILs are what
     * the namespaces and the private loader close */
    before = argc > 1 && !strcmp(argv[1], "--before");
    char sibs[16], outs[16], uids[16];
    snprintf(sibs, sizeof sibs, "%d", (int)sib);
    snprintf(outs, sizeof outs, "%d", (int)me);
    snprintf(uids, sizeof uids, "%u", (unsigned)getuid());
    char *args[] = { loader, probe, "--inside", w, sibs, outs, uids, NULL };
    pid_t q = jailed(args, out[1], hold[0], -1, 0, 0);
    if (q < 0) { perror("fork"); return 1; }
    close(out[1]);
    close(hold[0]);
    /* the inside report first: once it's in, the probe has exec'd and its dumpability is set */
    FILE *in = fdopen(out[0], "r");
    char line[512];
    int lines = 0;
    while (in && fgets(line, sizeof line, in)) {
        fputs(line, stdout);
        lines++;
        if (!strncmp(line, "FAIL", 4)) fails++;
        else if (!strncmp(line, "PASS", 4)) passes++;
        if (strstr(line, "can't make a process")) break;
    }
    check(lines >= 24, "inside: the probe ran and reported", 0);
    /* the person's other programs (this one is even its ancestor) */
    snprintf(p, sizeof p, "/proc/%d/environ", (int)q);
    REFUSED(open(p, O_RDONLY), 0, "outside: can't read QEMU's environment");
    snprintf(p, sizeof p, "/proc/%d/mem", (int)q);
    REFUSED(open(p, O_RDONLY), 0, "outside: can't open QEMU's memory (/proc/<pid>/mem)");
    snprintf(p, sizeof p, "/proc/%d/maps", (int)q);
    REFUSED(open(p, O_RDONLY), 0, "outside: can't read QEMU's maps");
    snprintf(p, sizeof p, "/proc/%d/fd", (int)q);
    errno = 0;
    DIR *d = opendir(p);
    int e = errno;
    check(!d, "outside: can't list QEMU's descriptors", e);
    if (d) closedir(d);
    snprintf(p, sizeof p, "/proc/%d/root/etc/hostname", (int)q);
    REFUSED(open(p, O_RDONLY), 0, "outside: can't enter QEMU's mount namespace through /proc/<pid>/root");
    char b[64];
    struct iovec l = { b, sizeof b }, r = { (void *)0x400000, sizeof b };
    REFUSED(process_vm_readv(q, &l, 1, &r, 1, 0), 0, "outside: can't read QEMU's memory (process_vm_readv)");
    REFUSED(ptrace(PTRACE_ATTACH, q, 0, 0), 0, "outside: can't ptrace QEMU, even as its ancestor");
    check(vm_nondumpable(q) == 1, "outside: vm_nondumpable agrees", 0);
    snprintf(p, sizeof p, "/proc/%d/status", (int)q);
    f = fopen(p, "r");
    int uidok = 0;
    while (f && fgets(line, sizeof line, f)) {
        unsigned u;
        if (sscanf(line, "Uid:\t%u", &u) == 1 && u == getuid()) uidok = 1;
    }
    if (f) fclose(f);
    check(uidok, "outside: QEMU's status still names the person (status stays readable)", 0);
    close(hold[1]);
    if (in) fclose(in);
    int st;
    waitpid(q, &st, 0);

    /* the allowlist: each call that isn't QEMU's, in a probe of its own */
    static const char *const CASES[][2] = {
        { "unshare", "unshare" }, { "setns", "setns" }, { "ptrace", "ptrace" }, { "process_vm_readv", "process_vm_readv" },
        { "prlimit64", "prlimit64" }, { "setpriority", "setpriority" }, { "kill", "kill" }, { "ioctl-tiocsti", "ioctl" },
        { "ioctl-kvm", "ioctl" }, { "mmap-wx", "mmap" }, { "mprotect-wx", "mprotect" }, { "execve", "execve" },
        { "bpf", "bpf" }, { "perf_event_open", "perf_event_open" }, { "userfaultfd", "userfaultfd" }, { "keyctl", "keyctl" },
        { "mount", "mount" }, { "connect", "connect" }, { "personality", "personality" }, { "prctl-ptracer", "prctl" },
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) kill_one(CASES[i][0], CASES[i][1], sib, 1);
    kill_one("unshare", "unshare", sib, 0);
    kill_one("mmap-wx", "mmap", sib, 0);
    answered("seccomp-listener", "REFUSED", 0, "allowlist: a filter with a listener is refused (EPERM), not run");
    answered("mmap-wx-tcg", "ALLOWED", 1, "allowlist: a TCG machine may map memory writable and executable (control)");

    kill(sib, SIGKILL);
    waitpid(sib, NULL, 0);
    snprintf(p, sizeof p, "%s/secret", w); unlink(p);
    snprintf(p, sizeof p, "%s/disk.img", disks); unlink(p);
    unlink(probe);
    rmdir(disks);
    rmdir(w);
    printf("vm-jail-probe: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
