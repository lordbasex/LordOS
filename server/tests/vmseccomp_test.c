/* vmseccomp_test.c — QEMU's seccomp allowlist (src/vm/vmseccomp.c, vmsyscalls.h; docs/VIRTUALIZATION.md 3.3).
 *
 * For this computer's table and the other architecture's (tests/vmseccomp_other.c, built against that kernel's header:
 * a row it doesn't have doesn't compile):
 *   - every row is a real call of that architecture, once;
 *   - none of the dangerous calls is allowed (absent, or refused with an error and never run);
 *   - the BPF program, run here by a small interpreter for every combination of the machine's options: the
 *     architecture check, amd64's x32 numbers, every row reaching its own rule, every other number reaching the
 *     default action, and each argument rule (ioctl per device, clone, mmap and mprotect without TCG, prctl, the pids
 *     of prlimit64, kill and tgkill, seccomp's listener, execveat's descriptor), learn-all;
 *   - the program's size stays far under the kernel's 4096 instructions.
 * No QEMU, no kernel filter loaded (tests/vm_jail_probe.c loads it). */
#include "../src/vm/vmseccomp.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>

const VmScRow *vmsc_other_rows(int *n, uint32_t *arch, const char **name);

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL  "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- a classic BPF interpreter: the instructions vmseccomp.c emits, every jump checked to stay inside ---- */
static uint32_t run(const struct sock_filter *f, int n, const struct seccomp_data *d, int *bad) {
    uint32_t a = 0;
    for (int pc = 0, steps = 0; pc >= 0 && pc < n && steps < 100000; steps++) {
        const struct sock_filter *i = &f[pc];
        switch (i->code) {
        case BPF_LD | BPF_W | BPF_ABS:
            if (i->k > sizeof *d - 4 || i->k % 4) { *bad = 1; return 0; }
            memcpy(&a, (const char *)d + i->k, 4);
            pc++;
            break;
        case BPF_ALU | BPF_AND | BPF_K: a &= i->k; pc++; break;
        case BPF_ALU | BPF_RSH | BPF_K: a >>= i->k; pc++; break;
        case BPF_JMP | BPF_JA: pc += 1 + (int)i->k; break;
        case BPF_JMP | BPF_JEQ | BPF_K: pc += 1 + (a == i->k ? i->jt : i->jf); break;
        case BPF_JMP | BPF_JGE | BPF_K: pc += 1 + (a >= i->k ? i->jt : i->jf); break;
        case BPF_JMP | BPF_JSET | BPF_K: pc += 1 + ((a & i->k) ? i->jt : i->jf); break;
        case BPF_RET | BPF_K: return i->k;
        default: *bad = 1; return 0;
        }
    }
    *bad = 1;
    return 0;
}

typedef struct {
    const VmScRow *rows;
    int n;
    uint32_t arch;
    const char *name;
} Table;

static struct sock_filter prog[BPF_MAXINSNS];
static int progn;
static VmScOpts cur;
static Table *tab;

static int build(const VmScOpts *o) {
    cur = *o;
    progn = vm_sc_build_for(tab->rows, tab->n, tab->arch, o, prog, BPF_MAXINSNS);
    return progn;
}

static uint32_t verdict6(int nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    struct seccomp_data d;
    memset(&d, 0, sizeof d);
    d.nr = nr;
    d.arch = tab->arch;
    d.args[0] = a0; d.args[1] = a1; d.args[2] = a2; d.args[3] = a3; d.args[4] = a4;
    int bad = 0;
    uint32_t r = run(prog, progn, &d, &bad);
    CHECK(!bad, "%s: the program ran off its end or used an unknown instruction (call %d)", tab->name, nr);
    return r;
}
static uint32_t verdict(int nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a4) { return verdict6(nr, a0, a1, a2, 0, a4); }

static const VmScRow *row(const char *name) {
    for (int i = 0; i < tab->n; i++) if (!strcmp(tab->rows[i].name, name)) return &tab->rows[i];
    return NULL;
}
static int nr_of(const char *name) { const VmScRow *r = row(name); return r ? r->nr : -1; }

/* calls QEMU must never make: absent, or refused with an error (they never run) */
static const char *const DANGEROUS[] = {
    "ptrace", "process_vm_readv", "process_vm_writev", "kcmp", "pidfd_getfd", "pidfd_open", "pidfd_send_signal",
    "process_madvise", "process_mrelease", "kexec_load", "kexec_file_load", "bpf", "perf_event_open", "userfaultfd",
    "mount", "umount2", "pivot_root", "chroot", "setns", "unshare", "fsopen", "fsmount", "fsconfig", "fspick",
    "move_mount", "open_tree", "mount_setattr", "keyctl", "add_key", "request_key", "init_module", "finit_module",
    "delete_module", "io_uring_setup", "io_uring_enter", "io_uring_register", "execve", "fork", "vfork", "connect",
    "bind", "listen", "accept", "accept4", "open_by_handle_at", "name_to_handle_at", "fanotify_init", "mknodat", "mknod",
    "swapon", "swapoff", "reboot", "acct", "syslog", "quotactl", "setuid", "setgid", "setreuid", "setregid", "setresuid",
    "setresgid", "setfsuid", "setfsgid", "setgroups", "capset", "setpriority", "sched_setaffinity", "sched_setscheduler",
    "sched_setparam", "sched_setattr", "setpgid", "setsid", "personality", "iopl", "ioperm", "settimeofday",
    "clock_settime", "clock_adjtime", "adjtimex", "sethostname", "setdomainname", "memfd_secret", "move_pages",
    "migrate_pages", "set_mempolicy", "vhangup", "uselib", "lookup_dcookie", "tkill", "kill", "rt_sigqueueinfo",
    "rt_tgsigqueueinfo", "pkey_mprotect", "remap_file_pages", "modify_ldt", "landlock_restrict_self", "ustat", "sysfs",
    "_sysctl", "nfsservctl", "vserver", "kexec", "seccomp_unotify", NULL,
};

static void table_checks(void) {
    for (int i = 0; i < tab->n; i++) {
        const VmScRow *r = &tab->rows[i];
        CHECK(r->nr >= 0 && r->nr < 1024, "%s: %s has number %d", tab->name, r->name, r->nr);
        CHECK(r->rule >= VSR_OK && r->rule <= VSR_VENUSEPERM, "%s: %s has no rule", tab->name, r->name);
        for (int j = 0; j < i; j++) {
            CHECK(strcmp(r->name, tab->rows[j].name), "%s: %s is twice", tab->name, r->name);
            CHECK(r->nr != tab->rows[j].nr, "%s: %s and %s are the same call", tab->name, r->name, tab->rows[j].name);
        }
    }
    for (int i = 0; DANGEROUS[i]; i++) {
        const VmScRow *r = row(DANGEROUS[i]);
        /* VENUS rows run only under the render server's own filter (VmScOpts.venus); QEMU's gets its default (below) */
        CHECK(!r || r->rule == VSR_EPERM || r->rule == VSR_EACCES || r->rule == VSR_ENOSYS || r->rule == VSR_VENUS ||
              r->rule == VSR_VENUSKILL || r->rule == VSR_VENUSEPERM,
              "%s: %s may run (it must be absent or refused)", tab->name, DANGEROUS[i]);
    }
    CHECK(!row("execve"), "%s: execve is a row (QEMU's one exec is execveat on lord-vm's descriptor)", tab->name);
}

#define ALLOW SECCOMP_RET_ALLOW
#define KILL SECCOMP_RET_KILL_PROCESS
#define NOTIF SECCOMP_RET_USER_NOTIF
#define ERRNO(e) (SECCOMP_RET_ERRNO | (e))

static void program_checks(void) {
    const pid_t self = 4242;
    int maxlen = 0;
    /* every combination of the machine's options, with and without a listener */
    for (int bits = 0; bits < 256; bits++) {
        VmScOpts o = { .kvm = bits & 1, .tcg = !!(bits & 2), .render = !!(bits & 4), .vgpu = !!(bits & 8),
                       .tap = !!(bits & 16), .notify = !!(bits & 32), .bti = !!(bits & 64), .usb = !!(bits & 128), .self = self, .exec_fd = 9, .learn = 0 };
        int n = build(&o);
        CHECK(n > 0 && n <= BPF_MAXINSNS, "%s: options %d: the program has %d instructions", tab->name, bits, n);
        if (n <= 0 || n > BPF_MAXINSNS) continue;
        if (n > maxlen) maxlen = n;
        uint32_t deflt = o.notify ? NOTIF : KILL;
        /* the architecture first */
        {
            struct seccomp_data d = { .nr = nr_of("read"), .arch = tab->arch == AUDIT_ARCH_X86_64 ? AUDIT_ARCH_I386 : AUDIT_ARCH_ARM };
            int bad = 0;
            CHECK(run(prog, progn, &d, &bad) == KILL && !bad, "%s: another architecture's call isn't killed", tab->name);
        }
        if (tab->arch == AUDIT_ARCH_X86_64) CHECK(verdict(nr_of("read") | 0x40000000, 0, 0, 0, 0) == KILL, "amd64: an x32 call isn't killed");
        /* every row reaches its rule; every other number reaches the default */
        for (int nr = 0; nr < 1024; nr++) {
            const VmScRow *r = NULL;
            for (int i = 0; i < tab->n; i++) if (tab->rows[i].nr == nr) r = &tab->rows[i];
            uint32_t got = verdict(nr, 0, 0, 0, 0);
            if (!r) { CHECK(got == deflt, "%s: call %d, not a row, gets 0x%x", tab->name, nr, got); continue; }
            switch (r->rule) {
            case VSR_OK: case VSR_HOT: CHECK(got == ALLOW, "%s: %s isn't allowed (0x%x)", tab->name, r->name, got); break;
            case VSR_EACCES: CHECK(got == ERRNO(EACCES), "%s: %s isn't EACCES", tab->name, r->name); break;
            case VSR_EPERM: CHECK(got == ERRNO(EPERM), "%s: %s isn't EPERM", tab->name, r->name); break;
            case VSR_ENOSYS: CHECK(got == ERRNO(ENOSYS), "%s: %s isn't ENOSYS", tab->name, r->name); break;
            /* libusb's rows (6.6): only for a machine that may get a host USB device */
            case VSR_USB: CHECK(got == (o.usb ? ALLOW : deflt), "%s: %s with usb=%d", tab->name, r->name, o.usb); break;
            /* QEMU's filter: the render server's rows are its default */
            case VSR_VENUS: case VSR_VENUSKILL: case VSR_VENUSEPERM: CHECK(got == deflt, "%s: %s (the render server's) runs for QEMU (0x%x)", tab->name, r->name, got); break;
            default: break;     /* the argument rules, below */
            }
        }
        /* ioctl: per device; TIOCSTI never */
        int io = nr_of("ioctl");
        CHECK(verdict(io, 3, 0x5401, 0, 0) == ALLOW, "%s: TCGETS refused", tab->name);
        CHECK(verdict(io, 9, 0x1268, 0, 0) == ALLOW, "%s: BLKSSZGET refused", tab->name);
        CHECK(verdict(io, 9, 0x800c581e, 0, 0) == ALLOW, "%s: XFS_IOC_DIOINFO refused", tab->name);
        CHECK(verdict(io, 9, 0x5421, 0, 0) == deflt, "%s: FIONBIO (never measured) allowed", tab->name);
        CHECK(verdict(io, 3, 0x5412, 0, 0) == deflt, "%s: TIOCSTI not stopped", tab->name);
        CHECK(verdict(io, 3, 0x541C, 0, 0) == deflt, "%s: TIOCLINUX not stopped", tab->name);
        CHECK(verdict(io, 3, 0xAE80, 0, 0) == (o.kvm ? ALLOW : deflt), "%s: KVM_RUN with kvm=%d", tab->name, o.kvm);
        CHECK(verdict(io, 3, 0xC00464B4u, 0, 0) == (o.render ? ALLOW : deflt), "%s: a DRM ioctl with render=%d", tab->name, o.render);
        CHECK(verdict(io, 3, 0x3B6E, 0, 0) == (o.vgpu ? ALLOW : deflt), "%s: a VFIO ioctl with vgpu=%d", tab->name, o.vgpu);
        CHECK(verdict(io, 3, 0x400454CA, 0, 0) == (o.tap ? ALLOW : deflt), "%s: TUNSETIFF with tap=%d", tab->name, o.tap);
        /* usbfs (USBDEVFS_SUBMITURB 0x8038550a, USBDEVFS_CLAIMINTERFACE 0x8004550f) only with usb; never a terminal's */
        CHECK(verdict(io, 3, 0x8038550au, 0, 0) == (o.usb ? ALLOW : deflt), "%s: USBDEVFS_SUBMITURB with usb=%d", tab->name, o.usb);
        CHECK(verdict(io, 3, 0x8004550fu, 0, 0) == (o.usb ? ALLOW : deflt), "%s: USBDEVFS_CLAIMINTERFACE with usb=%d", tab->name, o.usb);
        CHECK(verdict(io, 3, 0x5412, 0, 0) == deflt, "%s: TIOCSTI with usb=%d", tab->name, o.usb);
        CHECK(verdict(io, 3, 0x5412, 0, 0) == deflt, "%s: TIOCSTI with tap=%d", tab->name, o.tap);
        CHECK(verdict(io, 3, 0xC0186201u, 0, 0) == (o.render ? ALLOW : deflt), "%s: a dma-buf ioctl with render=%d", tab->name, o.render);
        CHECK(verdict(io, 3, 0xC0303E03u, 0, 0) == (o.render ? ALLOW : deflt), "%s: SYNC_IOC_MERGE with render=%d", tab->name, o.render);
        CHECK(verdict(io, 3, 0x0000AE00 | (1ull << 32), 0, 0) == (o.kvm ? ALLOW : deflt), "%s: high bits of the request", tab->name);
        CHECK(verdict(io, 3, 0xC0502100u, 0, 0) == deflt, "%s: SECCOMP_IOCTL_NOTIF_RECV not stopped", tab->name);
        /* clone: threads only */
        int cl = nr_of("clone");
        uint64_t thr = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
                       CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;
        CHECK(verdict(cl, thr, 0, 0, 0) == ALLOW, "%s: pthread_create's clone refused", tab->name);
        CHECK(verdict(cl, SIGCHLD | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID, 0, 0, 0) == ERRNO(EPERM), "%s: fork's clone allowed", tab->name);
        CHECK(verdict(cl, thr | CLONE_NEWUSER, 0, 0, 0) == ERRNO(EPERM), "%s: a thread in a new user namespace", tab->name);
        CHECK(verdict(cl, thr | CLONE_VFORK, 0, 0, 0) == ERRNO(EPERM), "%s: CLONE_VFORK", tab->name);
        CHECK(verdict(cl, thr | 0x00001000, 0, 0, 0) == ERRNO(EPERM), "%s: CLONE_PIDFD", tab->name);
        CHECK(verdict(cl, thr & ~(uint64_t)CLONE_THREAD, 0, 0, 0) == ERRNO(EPERM), "%s: a process sharing memory", tab->name);
        if (nr_of("clone3") >= 0) CHECK(verdict(nr_of("clone3"), 0, 0, 0, 0) == ERRNO(ENOSYS), "%s: clone3 isn't ENOSYS", tab->name);
        /* executable memory without TCG: only a private, read-only mapping of a file (the loader's) */
        int mm = nr_of("mmap"), mp = nr_of("mprotect"), mf = nr_of("memfd_create");
        uint32_t wx = o.tcg ? ALLOW : deflt;
        /* the loader's kind: allowed without a listener; with one, lord-vm decides (vm_sc_startup_exec, below) */
        uint32_t lib = o.tcg || !o.notify ? ALLOW : NOTIF;
        CHECK(verdict6(mm, 0, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, 5) == lib, "%s: a library's r-x mapping (notify=%d)", tab->name, o.notify);
        CHECK(verdict6(mm, 0, 4096, PROT_READ | PROT_EXEC | 0x10, MAP_PRIVATE | MAP_DENYWRITE, 5) == lib, "%s: an r-x BTI mapping", tab->name);
        CHECK(verdict6(mm, 0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, 5) == ALLOW, "%s: a shared rw- mapping refused", tab->name);
        CHECK(verdict6(mm, 0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, (uint32_t)-1) == ALLOW, "%s: anonymous rw- refused", tab->name);
        CHECK(verdict6(mm, 0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, (uint32_t)-1) == wx, "%s: anonymous rwx with tcg=%d", tab->name, o.tcg);
        CHECK(verdict6(mm, 0, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, (uint32_t)-1) == wx, "%s: anonymous r-x with tcg=%d", tab->name, o.tcg);
        CHECK(verdict6(mm, 0, 4096, PROT_READ | PROT_EXEC, MAP_SHARED, 5) == wx, "%s: a shared r-x mapping with tcg=%d", tab->name, o.tcg);
        CHECK(verdict6(mm, 0, 4096, PROT_READ | PROT_EXEC, 0x03 /* MAP_SHARED_VALIDATE */, 5) == wx, "%s: MAP_SHARED_VALIDATE r-x", tab->name);
        CHECK(verdict6(mm, 0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE, 5) == wx, "%s: a file's rwx mapping with tcg=%d", tab->name, o.tcg);
        CHECK(verdict(mp, 0, 4096, PROT_READ, 0) == ALLOW, "%s: mprotect r-- refused", tab->name);
        CHECK(verdict(mp, 0, 4096, PROT_READ | PROT_WRITE, 0) == ALLOW, "%s: mprotect rw- refused", tab->name);
        CHECK(verdict(mp, 0, 4096, PROT_READ | PROT_EXEC, 0) == wx, "%s: mprotect r-x with tcg=%d", tab->name, o.tcg);
        CHECK(verdict(mp, 0, 4096, PROT_WRITE | PROT_EXEC, 0) == wx, "%s: mprotect -wx with tcg=%d", tab->name, o.tcg);
        CHECK(verdict(mp, 0, 4096, PROT_READ | PROT_EXEC | 0x10, 0) == (o.tcg || (o.bti && !o.notify) ? ALLOW : deflt), "%s: mprotect r-x BTI with bti=%d", tab->name, o.bti);
        CHECK(verdict(mp, 0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC | 0x10, 0) == wx, "%s: mprotect rwx BTI", tab->name);
        CHECK(verdict(mf, 0, 0x3, 0, 0) == ALLOW, "%s: QEMU's memfd refused", tab->name);
        CHECK(verdict(mf, 0, 0x3 | 0x0008, 0, 0) == ALLOW, "%s: a memfd with MFD_NOEXEC_SEAL refused", tab->name);
        CHECK(verdict(mf, 0, 0x3 | 0x0010, 0, 0) == deflt, "%s: a memfd with MFD_EXEC allowed", tab->name);
        /* pids */
        int pl = nr_of("prlimit64"), tk = nr_of("tgkill");
        CHECK(!row("kill") || (row("kill")->rule == VSR_VENUSKILL && verdict(nr_of("kill"), 77, 9, 0, 0) == deflt), "%s: kill runs for QEMU (QEMU signals its threads with tgkill only; kill is the render server's row)", tab->name);
        CHECK(verdict(pl, 0, 7, 0, 0) == ALLOW, "%s: prlimit64 on itself (0) refused", tab->name);
        CHECK(verdict(pl, (uint64_t)self, 7, 0, 0) == ALLOW, "%s: prlimit64 on its pid refused", tab->name);
        CHECK(verdict(pl, 1, 7, 0, 0) == deflt, "%s: prlimit64 on pid 1 allowed", tab->name);
        CHECK(verdict(pl, (uint64_t)self + 1, 7, 0, 0) == deflt, "%s: prlimit64 on another pid allowed", tab->name);
        CHECK(verdict(tk, (uint64_t)self, 77, 10, 0) == ALLOW, "%s: tgkill on its thread refused", tab->name);
        CHECK(verdict(tk, (uint64_t)self + 5, 77, 10, 0) == deflt, "%s: tgkill elsewhere allowed", tab->name);
        /* prctl */
        int pc = nr_of("prctl");
        CHECK(verdict(pc, PR_SET_NAME, 0, 0, 0) == ALLOW, "%s: PR_SET_NAME refused", tab->name);
        CHECK(verdict(pc, PR_SET_DUMPABLE, 1, 0, 0) == ERRNO(EPERM), "%s: PR_SET_DUMPABLE not EPERM", tab->name);
        CHECK(verdict(pc, PR_SET_MM, 0, 0, 0) == deflt, "%s: PR_SET_MM not stopped", tab->name);
        CHECK(verdict(pc, 0x59616d61, 0, 0, 0) == deflt, "%s: PR_SET_PTRACER not stopped", tab->name);
        CHECK(verdict(pc, PR_SET_SECCOMP, 2, 0, 0) == deflt, "%s: PR_SET_SECCOMP not stopped", tab->name);
        /* seccomp: no listener */
        int sc = nr_of("seccomp");
        CHECK(verdict(sc, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, 0, 0) == ALLOW, "%s: QEMU's own filter refused", tab->name);
        CHECK(verdict(sc, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, 0, 0) == ERRNO(EPERM), "%s: a listener allowed", tab->name);
        CHECK(verdict(sc, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER | SECCOMP_FILTER_FLAG_TSYNC, 0, 0) == ERRNO(EPERM), "%s: a listener with TSYNC", tab->name);
        CHECK(verdict(sc, SECCOMP_GET_ACTION_AVAIL, 0, 0, 0) == ALLOW, "%s: SECCOMP_GET_ACTION_AVAIL refused", tab->name);
        /* socketpair: AF_UNIX only; socket never */
        CHECK(verdict(nr_of("socketpair"), AF_UNIX, SOCK_STREAM, 0, 0) == ALLOW, "%s: an AF_UNIX socketpair refused", tab->name);
        CHECK(verdict(nr_of("socketpair"), AF_INET, SOCK_STREAM, 0, 0) == ERRNO(EACCES), "%s: an AF_INET socketpair", tab->name);
        CHECK(verdict(nr_of("socket"), AF_UNIX, SOCK_STREAM, 0, 0) == ERRNO(EACCES), "%s: socket allowed", tab->name);
        /* execveat: with a listener always lord-vm's call (vm_sc_exec_once lets one through); without, lord-vm's
         * descriptor number and AT_EMPTY_PATH only */
        int ex = nr_of("execveat");
        CHECK(verdict(ex, 9, 0, 0, AT_EMPTY_PATH) == (o.notify ? NOTIF : ALLOW), "%s: the exec of QEMU with notify=%d", tab->name, o.notify);
        CHECK(verdict(ex, 10, 0, 0, AT_EMPTY_PATH) == deflt, "%s: execveat of another descriptor", tab->name);
        CHECK(verdict(ex, 9, 0, 0, 0) == deflt, "%s: execveat of a path under the descriptor", tab->name);
        CHECK(verdict(ex, (uint64_t)AT_FDCWD, 0, 0, AT_EMPTY_PATH) == deflt, "%s: execveat AT_FDCWD", tab->name);
    }
    /* no exec at all once the descriptor is gone; learn-all: rows that aren't HOT go to lord-vm */
    VmScOpts o = { .kvm = 1, .notify = 1, .self = self, .exec_fd = -1 };
    build(&o);
    CHECK(verdict(nr_of("execveat"), 9, 0, 0, AT_EMPTY_PATH) == NOTIF, "%s: execveat without a descriptor", tab->name);
    o.learn = 2;
    build(&o);
    CHECK(verdict(nr_of("read"), 0, 0, 0, 0) == ALLOW, "%s: learn-all: read (HOT) isn't allowed", tab->name);
    CHECK(verdict(nr_of("openat"), 0, 0, 0, 0) == NOTIF, "%s: learn-all: openat goes to lord-vm", tab->name);
    CHECK(verdict(nr_of("socket"), 1, 1, 0, 0) == ERRNO(EACCES), "%s: learn-all: socket is still EACCES", tab->name);
    o.self = 0;
    CHECK(vm_sc_build_for(tab->rows, tab->n, tab->arch, &o, prog, BPF_MAXINSNS) < 0, "%s: a program without QEMU's pid", tab->name);
    int ok = 0, er = 0, ar = 0;
    for (int i = 0; i < tab->n; i++) {
        int r = tab->rows[i].rule;
        if (r == VSR_OK || r == VSR_HOT) ok++;
        else if (r == VSR_EACCES || r == VSR_EPERM || r == VSR_ENOSYS) er++;
        else ar++;
    }
    printf("%s: %d rows (%d allowed, %d with argument rules, %d refused with an error), at most %d instructions\n",
           tab->name, tab->n, ok, ar, er, maxlen);
}

/* venus's render server's filter (vm_jail_venus): its workers' forks, kill and tgkill only to a pid above 0, prlimit
 * on itself, thread priorities EPERM; no exec but the one */
static void venus_checks(void) {
    VmScOpts o = { .render = 1, .venus = 1, .notify = 1, .self = 4242, .exec_fd = 9 };
    if (build(&o) <= 0) { CHECK(0, "%s: the render server's program", tab->name); return; }
    int cl = nr_of("clone");
    CHECK(verdict(cl, 0x01200011 /* CHILD_SETTID|CHILD_CLEARTID|SIGCHLD */, 0, 0, 0) == ALLOW, "%s: venus: glibc's fork refused", tab->name);
    CHECK(verdict(cl, 17 /* SIGCHLD */, 0, 0, 0) == ALLOW, "%s: venus: a plain fork refused", tab->name);
    CHECK(verdict(cl, 0x10000000 | 17 /* CLONE_NEWUSER */, 0, 0, 0) != ALLOW, "%s: venus: a clone into a new namespace allowed", tab->name);
    int k = nr_of("kill");
    CHECK(verdict(k, 77, 9, 0, 0) == ALLOW, "%s: venus: kill of its worker refused", tab->name);
    CHECK(verdict(k, 0, 9, 0, 0) == NOTIF, "%s: venus: kill 0 (its group) not stopped", tab->name);
    CHECK(verdict(k, 0xffffffffu, 9, 0, 0) == NOTIF, "%s: venus: kill -1 not stopped", tab->name);
    CHECK(verdict(k, 0xfffffff0u, 9, 0, 0) == NOTIF, "%s: venus: kill of a group not stopped", tab->name);
    int tg = nr_of("tgkill");
    CHECK(verdict(tg, 77, 78, 0, 0) == ALLOW && verdict(tg, 0, 78, 0, 0) == NOTIF && verdict(tg, 0xffffffffu, 1, 0, 0) == NOTIF,
          "%s: venus: tgkill not limited to a pid above 0", tab->name);
    int pl = nr_of("prlimit64");
    CHECK(verdict(pl, 0, 0, 0, 0) == ALLOW && verdict(pl, 4242, 0, 0, 0) == ALLOW && verdict(pl, 77, 0, 0, 0) == NOTIF,
          "%s: venus: prlimit not limited to itself", tab->name);
    CHECK(verdict(nr_of("setpriority"), 0, 0, 19, 0) == ERRNO(EPERM) && verdict(nr_of("sched_setscheduler"), 0, 5, 0, 0) == ERRNO(EPERM),
          "%s: venus: thread priorities not refused with EPERM", tab->name);
    CHECK(verdict(nr_of("wait4"), 77, 0, 0, 0) == ALLOW, "%s: venus: wait4 refused", tab->name);
    CHECK(verdict(nr_of("execveat"), 9, 0, 0, 0x1000) == NOTIF, "%s: venus: execveat not sent to lord-vm", tab->name);
    if (nr_of("fork") >= 0) CHECK(verdict(nr_of("fork"), 0, 0, 0, 0) == ALLOW, "%s: venus: fork refused", tab->name);
}

int main(void) {
    Table native = { 0 }, other = { 0 };
    native.rows = vm_sc_rows(&native.n);
#if defined(__x86_64__)
    native.arch = AUDIT_ARCH_X86_64; native.name = "amd64";
#elif defined(__aarch64__)
    native.arch = AUDIT_ARCH_AARCH64; native.name = "arm64";
#endif
    other.rows = vmsc_other_rows(&other.n, &other.arch, &other.name);
    if (!native.arch) { printf("skipped: needs amd64 or arm64\n"); return 0; }
    tab = &native;
    table_checks();
    program_checks();
    venus_checks();
    /* names this computer gives the rows match the kernel's */
    for (int i = 0; i < native.n; i++) {
        const char *k = vm_sc_name(native.rows[i].nr);
        CHECK(k && !strcmp(k, native.rows[i].name), "%s: row %s is call %d, which the kernel calls %s", native.name,
              native.rows[i].name, native.rows[i].nr, k ? k : "(nothing)");
    }
    /* what lord-vm lets through while QEMU starts, and only then: the loader's kind of executable mapping */
    {
        struct seccomp_data d;
        memset(&d, 0, sizeof d);
        d.nr = SYS_mmap; d.args[2] = PROT_READ | PROT_EXEC; d.args[3] = MAP_PRIVATE; d.args[4] = 5;
        CHECK(vm_sc_startup_exec(&d, 0), "startup: a library's mapping refused");
        d.args[3] = MAP_SHARED;
        CHECK(!vm_sc_startup_exec(&d, 0), "startup: a shared executable mapping let through");
        d.args[3] = MAP_PRIVATE | MAP_ANONYMOUS;
        CHECK(!vm_sc_startup_exec(&d, 0), "startup: anonymous executable memory let through");
        d.args[3] = MAP_PRIVATE; d.args[2] = PROT_READ | PROT_WRITE | PROT_EXEC;
        CHECK(!vm_sc_startup_exec(&d, 0), "startup: a writable executable mapping let through");
        d.nr = SYS_mprotect; d.args[2] = PROT_READ | PROT_EXEC | 0x10;
        CHECK(vm_sc_startup_exec(&d, 1) && !vm_sc_startup_exec(&d, 0), "startup: BTI's mprotect");
        d.args[2] = PROT_READ | PROT_EXEC;
        CHECK(!vm_sc_startup_exec(&d, 1), "startup: mprotect to executable without BTI let through");
        d.nr = SYS_openat;
        CHECK(!vm_sc_startup_exec(&d, 1), "startup: another call let through");
    }
    /* the one exec lord-vm lets through: the first execveat of the child, nothing after it, nothing from elsewhere */
    {
        struct seccomp_notif r;
        memset(&r, 0, sizeof r);
        int done = 0;
        r.pid = 77; r.data.nr = SYS_openat;
        CHECK(!vm_sc_exec_once(&r, 77, &done) && !done, "exec once: another call let through");
        r.data.nr = SYS_execveat; r.pid = 78;
        CHECK(!vm_sc_exec_once(&r, 77, &done) && !done, "exec once: another process's exec let through");
        r.pid = 77;
        CHECK(vm_sc_exec_once(&r, 77, &done) && done, "exec once: the start's own exec refused");
        CHECK(!vm_sc_exec_once(&r, 77, &done), "exec once: a second exec let through");
        done = 0;
        CHECK(!vm_sc_exec_once(&r, 0, &done), "exec once: let through without a child");
    }
    if (other.n) {
        tab = &other;
        table_checks();
        program_checks();
    } else printf("skipped: the other architecture's table (needs its <asm/unistd_64.h>)\n");
    printf("vmseccomp-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
