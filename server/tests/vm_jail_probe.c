/* vm_jail_probe.c — QEMU's jail as a compromised QEMU would find it (docs/VIRTUALIZATION.md 3.3, vmsandbox.h).
 *
 * The probe goes through lord-vm's own steps for QEMU: vm_userns_open, fork, vm_ns_enter, vm_ns_setup, vm_jail_qemu (its
 * Landlock and seccomp), then execve, so the kernel decides its dumpability as it does QEMU's. What runs after exec
 * is this program again (a copy in the jail's disks folder, started through the dynamic loader under /usr): code that
 * already runs inside QEMU, the case the jail is for. It tries what such code would, and reports each try.
 * The outside half is the person's other programs (this process is even the probe's ancestor, which Yama alone
 * would let in): it may not read the probe's memory, environment, descriptors or maps, nor ptrace it.
 *
 *   build/vm-jail-probe          PASS/FAIL lines, exit 1 on a FAIL; "skipped: needs ..." and exit 0 without user
 *                                namespaces or Landlock. GAP lines: what the jail still allows, left to the seccomp
 *                                allowlist (QEMU's own -sandbox, which this probe doesn't load, refuses setpriority)
 *   build/vm-jail-probe --before the jail as it was before the namespaces, to compare: its FAILs are what they close
 * No QEMU, KVM or root. */
#include "../src/vm/vmsandbox.h"
#include "../src/vm/vmproto.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static int inside(char **argv) {
    const char *w = argv[2];
    pid_t sib = (pid_t)atoi(argv[3]), outside = (pid_t)atoi(argv[4]);
    uid_t uid = (uid_t)atoi(argv[5]);
    char p[PATH_MAX + 64];
    int e;
    check(getuid() == uid, "inside: its uid is the person's (mapped)", 0);
    check(getgid() == 65534, "inside: its group isn't mapped (overflow gid)", 0);
    check(prctl(PR_GET_DUMPABLE) == 0, "inside: non-dumpable after exec", 0);
    REFUSED(kill(sib, 0), 0, "inside: can't signal a sibling process (Landlock's signal scope)");
    REFUSED(kill(outside, SIGCONT), 0, "inside: can't signal the process that started it");
    /* no pid namespace (vmsandbox.h): a sibling can still be named; what that allows is left to the seccomp allowlist */
    struct rlimit rl;
    if (!prlimit(sib, RLIMIT_CORE, NULL, &rl)) printf("GAP   inside: can read a sibling's limits (prlimit): left to the seccomp allowlist\n");
    if (!setpriority(PRIO_PROCESS, (id_t)sib, 0)) printf("GAP   inside: can renice a sibling (QEMU's own -sandbox resourcecontrol=deny refuses it)\n");
    REFUSED(ptrace(PTRACE_ATTACH, sib, 0, 0), 0, "inside: can't ptrace a sibling");
    REFUSED(ptrace(PTRACE_ATTACH, 1, 0, 0), 0, "inside: can't ptrace init (pid 1)");
    char b[64];
    struct iovec l = { b, sizeof b }, r = { (void *)0x400000, sizeof b };
    REFUSED(process_vm_readv(sib, &l, 1, &r, 1, 0), 0, "inside: can't read a sibling's memory (process_vm_readv)");
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
    REFUSED(socket(AF_UNIX, SOCK_STREAM, 0), 0, "inside: can't make a socket");
    REFUSED(unshare(CLONE_NEWUSER), 0, "inside: can't make a user namespace");
    REFUSED(fork(), 0, "inside: can't make a process");
    /* alive until the outside half is done with it */
    char c;
    while (read(0, &c, 1) > 0) {}
    return fails ? 1 : 0;
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

int main(int argc, char **argv) {
    if (argc == 6 && !strcmp(argv[1], "--inside")) return inside(argv);
    if (vm_landlock_abi() < 4) { printf("skipped: needs Landlock ABI 4 or later\n"); return 0; }
    int userns = vm_userns_open();
    if (userns < 0) { printf("skipped: needs unprivileged user namespaces (%s)\n", strerror(errno)); return 0; }
    /* lord-vm is non-dumpable from here on; so is its child until exec */
    prctl(PR_SET_DUMPABLE, 0);
    char w[] = "/tmp/vm-jail-probe.XXXXXX", p[PATH_MAX + 64], probe[PATH_MAX + 64], disks[PATH_MAX + 16];
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
    char loader[PATH_MAX];
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
    int before = argc > 1 && !strcmp(argv[1], "--before");
    pid_t q = fork();
    if (q < 0) { perror("fork"); return 1; }
    if (!q) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        dup2(out[1], 1);
        dup2(out[1], 2);
        dup2(hold[0], 0);
        const char *rt = getenv("XDG_RUNTIME_DIR");
        if (!before && (!vm_ns_enter(userns) || vm_ns_setup(rt && rt[0] ? rt : "/dev/shm") != VMNS_LOADER)) { printf("FAIL  namespaces or the private loader\n"); _exit(1); }
        VmQemuJail j = { .disks_dir = disks };
        int mask = vm_jail_qemu(&j);
        if (!(mask & VMS_SECCOMP_OURS) || !(mask & VMS_NO_NEW_PRIVS) || !(mask & VMS_LANDLOCK_FS)) { printf("FAIL  jail: mask %d\n", mask); _exit(1); }
        char sibs[16], outs[16], uids[16];
        snprintf(sibs, sizeof sibs, "%d", (int)sib);
        snprintf(outs, sizeof outs, "%d", (int)me);
        snprintf(uids, sizeof uids, "%u", (unsigned)getuid());
        char *args[] = { loader, probe, "--inside", w, sibs, outs, uids, NULL };
        /* the program is the loader under /usr (Landlock lets only /usr run); in this mount namespace it is the
         * unreadable copy, so this exec makes the probe non-dumpable exactly as QEMU's does */
        char henv[PATH_MAX + 16];
        const char *h = getenv("HOME");
        snprintf(henv, sizeof henv, "PROBE_HOME=%s", h ? h : "");
        char *env[] = { henv, NULL };
        execve(loader, args, env);
        printf("FAIL  exec: %s\n", strerror(errno));
        _exit(127);
    }
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
        else if (!strncmp(line, "GAP", 3)) lines--;
        else if (!strncmp(line, "PASS", 4)) passes++;
        if (strstr(line, "can't make a process")) break;
    }
    check(lines >= 18, "inside: the probe ran and reported", 0);
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
