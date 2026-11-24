/* vmseccomp.c — QEMU's seccomp allowlist as a BPF program (see vmseccomp.h, vmsyscalls.h). */
#include "vmseccomp.h"
#include "vmsyscalls.h"

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
#include <sys/auxv.h>
#include <sys/syscall.h>

#define ROW(n, r) { #n, __NR_##n, VSR_##r },
static const VmScRow ROWS[] = {
    VM_SC_COMMON(ROW)
#if defined(__x86_64__)
    VM_SC_AMD64(ROW)
#elif defined(__aarch64__)
    VM_SC_ARM64(ROW)
#endif
};
#undef ROW

const VmScRow *vm_sc_rows(int *n) {
    *n = (int)(sizeof ROWS / sizeof ROWS[0]);
    return ROWS;
}

/* every call's name on this computer (build/gen/vmsysnames.h, made from the kernel's <asm/unistd.h> by mk/vm.mk) */
static const struct { const char *name; int nr; } NAMES[] = {
#include "vmsysnames.h"
};

const char *vm_sc_name(int nr) {
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) if (NAMES[i].nr == nr) return NAMES[i].name;
    return NULL;
}

/* ---- the ioctl requests ---- */

/* fixed requests every machine may make: QEMU's file layer asks a disk image for its sector size (BLKSSZGET) and XFS's
 * direct I/O alignment (XFS_IOC_DIOINFO), measured; GLib asks stderr whether it is a terminal before a warning
 * (TCGETS, TIOCGWINSZ) and a socket how much is waiting (FIONREAD), kept (answers about its own descriptors) */
static const uint32_t IOCTL_ANY[] = {
    0x1268,         /* BLKSSZGET */
    0x800c581e,     /* XFS_IOC_DIOINFO */
    0x5401,         /* TCGETS */
    0x5413,         /* TIOCGWINSZ */
    0x541B,         /* FIONREAD */
};
#define IOC_TYPE_KVM    0xAE
#define IOC_TYPE_DRM    0x64    /* 'd': the render node's (Mesa's EGL and virglrenderer; measured on amdgpu and i915) */
#define IOC_TYPE_SYNC   0x3E    /* '>': sync_file (Mesa merges fences: SYNC_IOC_MERGE measured on i915, 2026-11-23) */
#define IOC_TYPE_DMABUF 0x62    /* 'b': dma-buf (Mesa's fence export and import on the dmabufs it shares; read in Mesa) */
#define IOC_TYPE_VFIO   0x3B    /* ';': VFIO's (hw/vfio, read: an Intel vGPU can't run here) */
#define IOC_TYPE_USB    0x55    /* 'U': usbfs's USBDEVFS_* (libusb in QEMU's usb-host, on a host USB device's descriptor) */
#define IOC_TYPE_TUN    0x54    /* 'T': TUNSETIFF is 202; below 200 are the terminal's, TIOCSTI among them */
#define IOC_TUN_FIRST   200

/* prctl options QEMU and libseccomp were measured to use; PR_SET_DUMPABLE is refused (QEMU stays non-dumpable) */
static const uint32_t PRCTL_OK[] = {
    PR_SET_NAME,            /* its threads' names */
    PR_CAPBSET_READ,        /* libseccomp's probe */
    PR_SET_TIMERSLACK,      /* QEMU's main loop */
    PR_MCE_KILL,            /* KVM: memory errors reach the guest early */
    PR_SET_NO_NEW_PRIVS,    /* libseccomp, before QEMU's own filter */
    PR_GET_DUMPABLE,        /* (kept) a read of its own flag; the jail probe checks it is 0 */
};

/* threads only (QEMU's own spawn=deny rule): these must all be set, and none of the forbidden ones */
#define CLONE_NEED (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM)
#define CLONE_FORBID (CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET | \
                      CLONE_NEWCGROUP | CLONE_VFORK | CLONE_PARENT | CLONE_PTRACE | CLONE_UNTRACED | CLONE_IO | \
                      0x00001000 /* CLONE_PIDFD */)

/* ---- the program ---- */

typedef struct {
    struct sock_filter *f;
    int cap, n;
    uint32_t deflt;
} Emit;

static void put(Emit *e, struct sock_filter s) {
    if (e->n < e->cap) e->f[e->n] = s;
    e->n++;
}
#define STMT(c, k) put(e, (struct sock_filter)BPF_STMT((c), (k)))
#define JUMP(c, k, t, f_) put(e, (struct sock_filter)BPF_JUMP((c), (k), (t), (f_)))
#define LD_NR() STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr))
/* an argument's low 32 bits: every argument checked here is an int or unsigned int to the kernel (vmsyscalls.h) */
#define LD_ARG(i) STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[(i)]))
#define RET(v) STMT(BPF_RET | BPF_K, (v))
#define ALLOW() RET(SECCOMP_RET_ALLOW)
#define DEFAULT() RET(e->deflt)
#define ERR(x) RET(SECCOMP_RET_ERRNO | ((x) & SECCOMP_RET_DATA))

/* A holds argument `arg`: allowed when it equals one of `v` (jumps of one to the ALLOW at the end) */
static void any_of(Emit *e, const uint32_t *v, int nv, uint32_t miss) {
    for (int i = 0; i < nv; i++) JUMP(BPF_JMP | BPF_JEQ | BPF_K, v[i], (unsigned char)(nv - i), 0);
    RET(miss);
    ALLOW();
}

/* the body of one row; A holds the call's number, the body always returns */
static int body(Emit *e, const VmScRow *r, const VmScOpts *o) {
    int learn_all = o->learn == 2;
    /* learn-all: every row with an argument rule but ioctl goes to lord-vm too, which logs its arguments */
    if (learn_all && r->rule != VSR_IOCTL && r->rule != VSR_HOT && r->rule != VSR_EACCES && r->rule != VSR_EPERM && r->rule != VSR_ENOSYS) {
        DEFAULT();
        return 0;
    }
    switch (r->rule) {
    case VSR_HOT: ALLOW(); return 0;
    case VSR_OK: if (learn_all) DEFAULT(); else ALLOW(); return 0;
    /* libusb in QEMU's usb-host (6.6): only a machine that may get a host USB device (VmScOpts.usb) */
    case VSR_USB: if (o->usb && !learn_all) ALLOW(); else DEFAULT(); return 0;
    case VSR_EACCES: ERR(EACCES); return 0;
    case VSR_EPERM: ERR(EPERM); return 0;
    case VSR_ENOSYS: ERR(ENOSYS); return 0;
    case VSR_CLONE:
        LD_ARG(0);
        if (o->venus) {
            /* the render server: also a plain fork for a context worker (glibc's: SIGCHLD with the child's tid set and
             * cleared, nothing shared) */
            JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)(CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | SIGCHLD), 4, 0);
            JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)SIGCHLD, 3, 0);
        }
        STMT(BPF_ALU | BPF_AND | BPF_K, (uint32_t)(CLONE_NEED | CLONE_FORBID));
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)CLONE_NEED, 1, 0);
        ERR(EPERM);
        ALLOW();
        return 0;
    case VSR_VENUS:
        if (o->venus && !learn_all) ALLOW(); else DEFAULT();
        return 0;
    case VSR_VENUSEPERM:
        if (o->venus) ERR(EPERM); else DEFAULT();
        return 0;
    case VSR_VENUSKILL:
        /* kill: the render server's own processes, by a pid above 0 (never 0, -1 or a group) */
        if (!o->venus || learn_all) { DEFAULT(); return 0; }
        LD_ARG(0);
        JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x80000000u, 1, 0);
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1);
        DEFAULT();
        ALLOW();
        return 0;
    case VSR_IOCTL: {
        LD_ARG(1);
        uint32_t types[6];
        int nt = 0;
        if (o->kvm) types[nt++] = IOC_TYPE_KVM;
        /* learn-all: only KVM's (hot) stay allowed; the others are logged */
        if (o->render && !learn_all) { types[nt++] = IOC_TYPE_DRM; types[nt++] = IOC_TYPE_SYNC; types[nt++] = IOC_TYPE_DMABUF; }
        if (o->vgpu && !learn_all) types[nt++] = IOC_TYPE_VFIO;
        if (o->usb && !learn_all) types[nt++] = IOC_TYPE_USB;
        /* [exact requests] → ALLOW at the end; then the type byte; then a tap's TUN range */
        int nx = learn_all ? 0 : (int)(sizeof IOCTL_ANY / sizeof IOCTL_ANY[0]);   /* learn-all: logged */
        /* each exact request jumps over what follows it to the final ALLOW: the distance is counted below */
        int tail = 2 + nt + (o->tap ? 5 : 1);   /* rsh, and, the type jumps, the TUN check or the default */
        for (int i = 0; i < nx; i++) JUMP(BPF_JMP | BPF_JEQ | BPF_K, IOCTL_ANY[i], (unsigned char)(nx - 1 - i + tail), 0);
        STMT(BPF_ALU | BPF_RSH | BPF_K, 8);
        STMT(BPF_ALU | BPF_AND | BPF_K, 0xff);
        for (int i = 0; i < nt; i++) JUMP(BPF_JMP | BPF_JEQ | BPF_K, types[i], (unsigned char)(nt - 1 - i + (o->tap ? 5 : 1)), 0);
        if (o->tap) {
            /* 'T' with a number of 200 or more: the tap's TUN requests (net/tap-linux.c), never the terminal's */
            JUMP(BPF_JMP | BPF_JEQ | BPF_K, IOC_TYPE_TUN, 0, 3);
            LD_ARG(1);
            STMT(BPF_ALU | BPF_AND | BPF_K, 0xff);
            JUMP(BPF_JMP | BPF_JGE | BPF_K, IOC_TUN_FIRST, 1, 0);
            DEFAULT();
        } else DEFAULT();
        ALLOW();
        return 0;
    }
    case VSR_PRCTL:
        LD_ARG(0);
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, PR_SET_DUMPABLE, 0, 1);
        ERR(EPERM);
        any_of(e, PRCTL_OK, (int)(sizeof PRCTL_OK / sizeof PRCTL_OK[0]), e->deflt);
        return 0;
    case VSR_SOCKETPAIR: {
        uint32_t unix_[1] = { AF_UNIX };
        LD_ARG(0);
        any_of(e, unix_, 1, SECCOMP_RET_ERRNO | EACCES);
        return 0;
    }
    case VSR_MMAP:
    case VSR_MPROTECT:
        /* without TCG (whose code buffer is writable and executable): with a listener every PROT_EXEC goes to lord-vm,
         * which lets through only what the loader does (vm_sc_startup_exec: a private read-only file mapping, arm64's
         * PROT_EXEC|PROT_BTI mprotect) and only until QEMU's start is over (its QMP handshake: libraries, QEMU's modules
         * and Mesa's drivers are loaded by then); after it, executable memory of any kind stops QEMU, whatever the file
         * (a memfd or a file of its disks folder written to be run). Without a listener (the fallback) only the loader's
         * kind passes, at any time; the kernel still refuses it on a file of the disks folder, noexec in QEMU's mount
         * namespace (vm_ns_setup) */
        if (o->tcg) { ALLOW(); return 0; }
        LD_ARG(2);
        if (o->notify) {
            JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 0, 1);
            DEFAULT();
            ALLOW();
        } else if (r->rule == VSR_MMAP) {
            JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 0, 4);
            JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_WRITE, 2, 0);
            LD_ARG(3);
            JUMP(BPF_JMP | BPF_JSET | BPF_K, MAP_SHARED | MAP_ANONYMOUS, 0, 1);   /* MAP_SHARED_VALIDATE has MAP_SHARED's bit */
            DEFAULT();
            ALLOW();
        } else if (o->bti) {
            JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 0, 3);
            JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_WRITE, 1, 0);
            JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x10 /* PROT_BTI */, 1, 0);
            DEFAULT();
            ALLOW();
        } else {
            JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 0, 1);
            DEFAULT();
            ALLOW();
        }
        return 0;
    case VSR_MEMFD:
        /* never MFD_EXEC (an executable memfd) */
        LD_ARG(1);
        JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x0010 /* MFD_EXEC */, 0, 1);
        DEFAULT();
        ALLOW();
        return 0;
    case VSR_PRLIMIT:
    case VSR_TGKILL: {
        /* prlimit64: 0 or itself; tgkill: a thread of its own */
        uint32_t v[2] = { (uint32_t)o->self, 0 };
        LD_ARG(0);
        if (o->venus && r->rule == VSR_TGKILL) {
            /* the render server and its workers: their own threads, by a pid above 0 (Landlock's signal scope keeps a
             * signal inside its jail); prlimit stays on 0 or the server itself, as below */
            JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x80000000u, 0, 1);
            DEFAULT();
            JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1);
            DEFAULT();
            ALLOW();
            return 0;
        }
        any_of(e, v, r->rule == VSR_PRLIMIT ? 2 : 1, e->deflt);
        return 0;
    }
    case VSR_SECCOMP:
        /* anything but a filter with a listener (QEMU could then answer for its own calls); libseccomp's probes
         * (SECCOMP_SET_MODE_STRICT with flags: EINVAL from the kernel, which it expects) pass */
        LD_ARG(0);
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, SECCOMP_SET_MODE_FILTER, 0, 3);
        LD_ARG(1);
        JUMP(BPF_JMP | BPF_JSET | BPF_K, SECCOMP_FILTER_FLAG_NEW_LISTENER, 0, 1);
        ERR(EPERM);
        ALLOW();
        return 0;
    case VSR_EXECVEAT:
        /* the one exec. With a listener every execveat goes to lord-vm, which lets only the first through (vm_sc_exec_once:
         * the start's own, from the child before QEMU ran) and stops QEMU at any later one: a descriptor number proves
         * nothing once QEMU runs (it can close and reuse it). Without a listener (the fallback) only lord-vm's descriptor
         * number with AT_EMPTY_PATH passes, and then Landlock lets nothing but QEMU's binary and its loader run, and
         * QEMU's own spawn=deny refuses exec once its sandbox is up */
        if (o->exec_fd < 0 || o->notify) { DEFAULT(); return 0; }
        LD_ARG(0);
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)o->exec_fd, 0, 3);
        LD_ARG(4);
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, AT_EMPTY_PATH, 0, 1);
        ALLOW();
        DEFAULT();
        return 0;
    }
    return -1;
}

static int by_nr(const void *a, const void *b) {
    const VmScRow *x = *(const VmScRow *const *)a, *y = *(const VmScRow *const *)b;
    return (x->nr > y->nr) - (x->nr < y->nr);
}

/* rows[lo, hi) sorted by number; A holds the call's number. Up to four rows: one comparison each (their bodies are
 * short, under 255 instructions); more: split at the middle, numbers below it first */
static int tree(Emit *e, const VmScRow *const *s, int lo, int hi, const VmScOpts *o) {
    if (hi - lo <= 4) {
        for (int i = lo; i < hi; i++) {
            int at = e->n;
            JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)s[i]->nr, 0, 0);
            int b0 = e->n;
            if (body(e, s[i], o)) return -1;
            int len = e->n - b0;
            if (len > 255) return -1;
            if (at < e->cap) e->f[at].jf = (unsigned char)len;
            /* the body may have loaded an argument: the number again for the next comparison */
            if (i + 1 < hi) LD_NR();
        }
        DEFAULT();
        return 0;
    }
    int mid = lo + (hi - lo) / 2;
    /* [at] jge mid → [at+1] ja right; else [at+2] the lower half */
    JUMP(BPF_JMP | BPF_JGE | BPF_K, (uint32_t)s[mid]->nr, 0, 1);
    int ja = e->n;
    STMT(BPF_JMP | BPF_JA, 0);
    if (tree(e, s, lo, mid, o)) return -1;
    if (ja < e->cap) e->f[ja].k = (uint32_t)(e->n - ja - 1);
    return tree(e, s, mid, hi, o);
}

int vm_sc_build_for(const VmScRow *rows, int n, uint32_t audit_arch, const VmScOpts *o, struct sock_filter *f, int cap) {
    if (n <= 0 || n > 1024 || o->self <= 0) return -1;     /* QEMU's pid is in the rules */
    const VmScRow **s = malloc(sizeof *s * (size_t)n);
    if (!s) return -1;
    int k = 0;
    for (int i = 0; i < n; i++) if (!o->drop || strcmp(rows[i].name, o->drop)) s[k++] = &rows[i];
    n = k;
    qsort(s, (size_t)n, sizeof *s, by_nr);
    for (int i = 1; i < n; i++) if (s[i]->nr == s[i - 1]->nr) { free(s); return -1; }
    Emit em = { f, cap, 0, o->notify ? SECCOMP_RET_USER_NOTIF : SECCOMP_RET_KILL_PROCESS }, *e = &em;
    STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
    JUMP(BPF_JMP | BPF_JEQ | BPF_K, audit_arch, 1, 0);
    RET(SECCOMP_RET_KILL_PROCESS);
    LD_NR();
    /* amd64's x32 numbers (bit 30) are never QEMU's */
    if (audit_arch == AUDIT_ARCH_X86_64) {
        JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x40000000u, 0, 1);
        RET(SECCOMP_RET_KILL_PROCESS);
    }
    int r = tree(e, s, 0, n, o);
    free(s);
    return r ? -1 : em.n;
}

int vm_sc_exec_once(const struct seccomp_notif *req, pid_t child, int *done) {
    if (req->data.nr != __NR_execveat || *done || child <= 0 || (pid_t)req->pid != child) return 0;
    *done = 1;
    return 1;
}

int vm_sc_startup_exec(const struct seccomp_data *d, int bti) {
    uint32_t prot = (uint32_t)d->args[2];
    if (!(prot & PROT_EXEC) || (prot & PROT_WRITE)) return 0;
    if (d->nr == __NR_mmap) return !((uint32_t)d->args[3] & (MAP_SHARED | MAP_ANONYMOUS));
    if (d->nr == __NR_mprotect) return bti && (prot & 0x10 /* PROT_BTI */);
    return 0;
}

int vm_sc_cpu_bti(void) {
#if defined(__aarch64__)
    return !!(getauxval(AT_HWCAP2) & (1UL << 17));     /* HWCAP2_BTI */
#else
    return 0;
#endif
}

int vm_sc_build(const VmScOpts *o, struct sock_filter *f, int cap) {
#if defined(__x86_64__)
    return vm_sc_build_for(ROWS, (int)(sizeof ROWS / sizeof ROWS[0]), AUDIT_ARCH_X86_64, o, f, cap);
#elif defined(__aarch64__)
    return vm_sc_build_for(ROWS, (int)(sizeof ROWS / sizeof ROWS[0]), AUDIT_ARCH_AARCH64, o, f, cap);
#else
    (void)o; (void)f; (void)cap;
    return 0;
#endif
}

void vm_sc_describe(const struct seccomp_data *d, char *out, size_t n) {
    const char *name = vm_sc_name(d->nr);
    char nb[32];
    if (!name) { snprintf(nb, sizeof nb, "system call %d", d->nr); name = nb; }
    if (d->nr == __NR_ioctl) snprintf(out, n, "ioctl 0x%08x on descriptor %d", (unsigned)d->args[1], (int)d->args[0]);
    else if (d->nr == __NR_prctl) snprintf(out, n, "prctl option %d", (int)d->args[0]);
    else if (d->nr == __NR_mmap) snprintf(out, n, "mmap with rights 0x%x, flags 0x%x", (unsigned)d->args[2], (unsigned)d->args[3]);
    else if (d->nr == __NR_mprotect) snprintf(out, n, "mprotect with rights 0x%x", (unsigned)d->args[2]);
    else if (d->nr == __NR_memfd_create) snprintf(out, n, "memfd_create with flags 0x%x", (unsigned)d->args[1]);
    else if (d->nr == __NR_clone) snprintf(out, n, "clone with flags 0x%x", (unsigned)d->args[0]);
    else snprintf(out, n, "%s (0x%llx, 0x%llx, 0x%llx)", name, (unsigned long long)d->args[0], (unsigned long long)d->args[1],
                  (unsigned long long)d->args[2]);
}

uint64_t vm_sc_learn_key(const struct seccomp_data *d) {
    uint64_t k = (uint64_t)(uint32_t)d->nr << 32;
    if (d->nr == __NR_ioctl) k |= (uint32_t)d->args[1];
    else if (d->nr == __NR_prctl || d->nr == __NR_socketpair || d->nr == __NR_seccomp) k |= (uint32_t)d->args[0];
    else if (d->nr == __NR_mmap || d->nr == __NR_mprotect) k |= (uint32_t)d->args[2] & (PROT_WRITE | PROT_EXEC);
    else if (d->nr == __NR_clone) k |= (uint32_t)d->args[0];
    return k;
}
