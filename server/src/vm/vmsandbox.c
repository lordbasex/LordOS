/* vmsandbox.c — Landlock and seccomp around QEMU and lord-vm (see vmsandbox.h). */
#include "vmsandbox.h"
#include "vmproto.h"
#include "vmseccomp.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__x86_64__)
#define VM_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define VM_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#define VM_AUDIT_ARCH 0
#endif

/* the dynamic loader QEMU's binary names (PT_INTERP) */
#if defined(__x86_64__)
#define VM_LOADER "/lib64/ld-linux-x86-64.so.2"
#elif defined(__aarch64__)
#define VM_LOADER "/lib/ld-linux-aarch64.so.1"
#else
#define VM_LOADER ""
#endif

int vm_landlock_abi(void) {
    int abi = (int)syscall(SYS_landlock_create_ruleset, NULL, 0U, LANDLOCK_CREATE_RULESET_VERSION);
    return abi > 0 ? abi : 0;
}

static uint64_t fs_handled(int abi) {
    uint64_t h = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
                 LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                 LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
                 LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                 LANDLOCK_ACCESS_FS_MAKE_SYM;
    if (abi >= 2) h |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 3) h |= LANDLOCK_ACCESS_FS_TRUNCATE;
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
    if (abi >= 5) h |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
    return h;
}

/* file rules may carry only these */
#define FILE_RIGHTS (LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_TRUNCATE)

static int rule(int rs, const char *path, uint64_t allow, uint64_t handled) {
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st)) { close(fd); return -1; }
    if (!S_ISDIR(st.st_mode)) allow &= FILE_RIGHTS
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
        | LANDLOCK_ACCESS_FS_IOCTL_DEV
#endif
        ;
    struct landlock_path_beneath_attr pb = { .allowed_access = allow & handled, .parent_fd = fd };
    int r = (int)syscall(SYS_landlock_add_rule, rs, LANDLOCK_RULE_PATH_BENEATH, &pb, 0U);
    close(fd);
    return r;
}

/* ---- seccomp: the image tools' denylist (qemu-img; QEMU itself has the allowlist, vmseccomp.c) ---- */

#define NR(x) (x)
static const int deny_eperm[] = {
    SYS_connect, SYS_bind, SYS_listen, SYS_accept, SYS_accept4,
    SYS_ptrace, SYS_process_vm_readv, SYS_process_vm_writev, SYS_kcmp,
    SYS_bpf, SYS_perf_event_open, SYS_keyctl, SYS_add_key, SYS_request_key,
    SYS_mount, SYS_umount2, SYS_pivot_root, SYS_chroot, SYS_unshare, SYS_setns,
    SYS_kexec_load, SYS_kexec_file_load, SYS_init_module, SYS_finit_module, SYS_delete_module,
    SYS_swapon, SYS_swapoff, SYS_reboot, SYS_acct, SYS_userfaultfd,
    SYS_io_uring_setup, SYS_io_uring_enter, SYS_io_uring_register,
    SYS_open_by_handle_at, SYS_name_to_handle_at, SYS_fanotify_init, SYS_quotactl, SYS_syslog, SYS_vhangup,
    SYS_mknodat, SYS_fsopen, SYS_fsmount, SYS_move_mount, SYS_open_tree, SYS_fsconfig, SYS_fspick,
#ifdef SYS_mknod
    SYS_mknod,
#endif
#ifdef SYS_iopl
    SYS_iopl, SYS_ioperm,
#endif
#ifdef SYS_uselib
    SYS_uselib,
#endif
};

#define CLONE_NS_FLAGS (CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWCGROUP)

static int tool_filter(struct sock_filter *f, int cap, int threads_only) {
    int n = 0;
#define EMIT(s) do { if (n < cap) f[n] = (struct sock_filter)s; n++; } while (0)
#define LDNR() EMIT(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)))
#define RET_ERR(e) EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ((e) & SECCOMP_RET_DATA)))
#define RET_OK() EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW))
    EMIT(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, VM_AUDIT_ARCH, 1, 0));
    EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    LDNR();
    /* socket: never (its sockets are the ones lord-vm passed) */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 1));
    RET_ERR(EACCES);
    /* clone: no new namespaces; for QEMU (threads_only) also no new process, only threads: QEMU never makes one (its own
     * -sandbox spawn=deny says the same), and a process it can't make can't inherit its limits (the locked memory root
     * grants a vGPU machine's QEMU, vmgpu_admin.c). [0]jeq [1]ld flags [2]and NS [3]jeq 0 → [5] [4]EPERM [5]ld flags
     * [6]and THREAD (or 0) [7]jeq mask → [9] [8]EPERM [9]ALLOW */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 0, 9));
    EMIT(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])));
    EMIT(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, (unsigned)CLONE_NS_FLAGS));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0));
    RET_ERR(EPERM);
    EMIT(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])));
    EMIT(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, threads_only ? (unsigned)CLONE_THREAD : 0u));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, threads_only ? (unsigned)CLONE_THREAD : 0u, 1, 0));
    RET_ERR(EPERM);
    RET_OK();
    LDNR();
    /* fork and vfork: QEMU none (threads_only) */
#ifdef SYS_fork
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_fork, 0, 1));
    if (threads_only) RET_ERR(EPERM); else RET_OK();
#endif
#ifdef SYS_vfork
    LDNR();
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_vfork, 0, 1));
    if (threads_only) RET_ERR(EPERM); else RET_OK();
#endif
    LDNR();
    /* clone3: its flags are in memory seccomp can't read: ENOSYS, glibc falls back to clone */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone3, 0, 1));
    RET_ERR(ENOSYS);
    /* ioctl: no TIOCSTI or TIOCLINUX on a terminal it may have inherited */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_ioctl, 0, 5));
    EMIT(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1])));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, TIOCSTI, 1, 0));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, TIOCLINUX, 0, 1));
    RET_ERR(EPERM);
    RET_OK();
    LDNR();
    for (size_t i = 0; i < sizeof deny_eperm / sizeof deny_eperm[0]; i++) {
        EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)deny_eperm[i], 0, 1));
        RET_ERR(EPERM);
    }
    RET_OK();
#undef EMIT
#undef LDNR
#undef RET_ERR
#undef RET_OK
    return n;
}

/* 3D graphics (vmsandbox.h): the render node read, write and ioctl; what Mesa reads to know the GPU behind it. A path
 * that isn't a DRM render node is refused (-1), and nothing is allowed then. */
static int render_rules(int rs, const char *node, uint64_t h) {
    struct stat st;
    if (strncmp(node, "/dev/dri/renderD", 16) || stat(node, &st) || !S_ISCHR(st.st_mode) || major(st.st_rdev) != 226 || minor(st.st_rdev) < 128) return -1;
    uint64_t dev = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE;
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
    dev |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
    if (rule(rs, node, dev, h)) return -1;
    /* libdrm's drmGetDevice2 lists /dev/dri and reads the GPU's PCI folder in sysfs; without either Mesa's EGL doesn't
     * start (measured 2026-11-20, amdgpu). Listing /dev/dri shows names only: opening card0 or another node is still
     * refused. The way through /sys/dev/char is a link and needs no rule. */
    if (rule(rs, "/dev/dri", LANDLOCK_ACCESS_FS_READ_DIR, h)) return -1;
    char sys[64], real[PATH_MAX];
    snprintf(sys, sizeof sys, "/sys/dev/char/%u:%u/device", major(st.st_rdev), minor(st.st_rdev));
    if (!realpath(sys, real) || strncmp(real, "/sys/devices/", 13)) return -1;
    if (rule(rs, real, LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR, h)) return -1;
    return 0;
}

/* an Intel vGPU (vmsandbox.h): the VFIO container and the vGPU's own group, nothing else of /dev/vfio; the mdev's sysfs
 * folder read. -1 when the paths aren't what they must be, and nothing is allowed then */
static int vgpu_rules(int rs, const char *sysdev, int group, uint64_t h) {
    if (group < 0 || strncmp(sysdev, "/sys/bus/mdev/devices/", 22) || strlen(sysdev) != 22 + 36 || strchr(sysdev + 22, '/')) return -1;
    uint64_t dev = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE;
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
    dev |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
    char g[32], real[PATH_MAX];
    snprintf(g, sizeof g, "/dev/vfio/%d", group);
    struct stat st;
    if (stat(g, &st) || !S_ISCHR(st.st_mode)) return -1;
    if (rule(rs, "/dev/vfio/vfio", dev, h) || rule(rs, g, dev, h)) return -1;
    if (!realpath(sysdev, real) || strncmp(real, "/sys/devices/", 13)) return -1;
    if (rule(rs, real, LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR, h)) return -1;
    return 0;
}

int vm_jail_qemu(const VmQemuJail *j) {
    int mask = 0;
    struct rlimit no = { 0, 0 };
    setrlimit(RLIMIT_CORE, &no);
    if (!prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) mask |= VMS_NO_NEW_PRIVS;

    int abi = vm_landlock_abi();
    if (abi >= 1) {
        struct landlock_ruleset_attr attr;
        memset(&attr, 0, sizeof attr);
        uint64_t h = fs_handled(abi);
        attr.handled_access_fs = h;
        if (abi >= 4) attr.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
#ifdef LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET
        if (abi >= 6) attr.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
#endif
        /* the whole struct: fields this kernel doesn't know are zero, which it accepts */
        int rs = (int)syscall(SYS_landlock_create_ruleset, &attr, sizeof attr, 0U);
        if (rs >= 0) {
            uint64_t ro = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
            uint64_t rx = ro | LANDLOCK_ACCESS_FS_EXECUTE;
            uint64_t rw = ro | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE;
            /* its own /proc/<pid>, through the link */
            /* /usr read only; execute only QEMU's binary and the loader it names (the exec opens both for execution;
             * libraries and Mesa's drivers are mapped, which needs no execute right). In QEMU's mount namespace the
             * loader's path is the private copy, mounted before this */
            char ldreal[PATH_MAX];
            int ok = j->exec_path && j->exec_path[0] == '/' && VM_LOADER[0] && realpath(VM_LOADER, ldreal) &&
                     !rule(rs, "/usr", ro, h) && !rule(rs, j->exec_path, rx, h) && !rule(rs, ldreal, rx, h) &&
                     !rule(rs, j->disks_dir, rw, h) && !rule(rs, "/proc/self", ro, h);
            rule(rs, "/etc/ld.so.cache", LANDLOCK_ACCESS_FS_READ_FILE, h);
            rule(rs, "/dev/null", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE, h);
            if (j->kvm) {
                uint64_t dev = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE;
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
                dev |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
                rule(rs, "/dev/kvm", dev, h);
            }
            /* 3D: a render node that doesn't check out gets no rule (QEMU's EGL then fails and it exits); it never costs
             * the rest of the sandbox */
            if (j->render && j->render[0]) render_rules(rs, j->render, h);
            /* a vGPU whose paths don't check out costs the whole sandbox: QEMU doesn't run (the caller refuses) */
            if (j->vgpu_dev && j->vgpu_dev[0] && vgpu_rules(rs, j->vgpu_dev, j->vgpu_group, h)) ok = 0;
            if (j->vgpu_dev && j->vgpu_rom && j->vgpu_rom[0] && rule(rs, j->vgpu_rom, LANDLOCK_ACCESS_FS_READ_FILE, h)) ok = 0;
            if (j->iso && j->iso[0] && rule(rs, j->iso, LANDLOCK_ACCESS_FS_READ_FILE, h)) ok = 0;
            if (j->drivers && j->drivers[0] && rule(rs, j->drivers, LANDLOCK_ACCESS_FS_READ_FILE, h)) ok = 0;
            if (j->base && j->base[0] && rule(rs, j->base, LANDLOCK_ACCESS_FS_READ_FILE, h)) ok = 0;
            if (ok && !syscall(SYS_landlock_restrict_self, rs, 0U)) {
                mask |= VMS_LANDLOCK_FS;
                if (abi >= 4) mask |= VMS_LANDLOCK_NET;
                if (abi >= 6) mask |= VMS_LANDLOCK_SCOPE;
            }
            close(rs);
        }
    }

    /* the allowlist (vmseccomp.h), last: from here on only its rows run, until exec and after. With a listener first
     * (lord-vm names what it stops); where the kernel refuses one, the same rows with kill as the default */
    if (j->notify_out) *j->notify_out = -1;
    if (VM_AUDIT_ARCH) {
        static struct sock_filter filt[BPF_MAXINSNS];
        VmScOpts o = { .kvm = j->kvm, .tcg = j->tcg, .render = j->render && j->render[0], .vgpu = j->vgpu_dev && j->vgpu_dev[0],
                       .tap = j->tap, .self = getpid(), .exec_fd = j->exec_fd, .notify = j->notify_out != NULL, .learn = j->learn, .bti = vm_sc_cpu_bti(),
                       .drop = j->sc_drop };
        for (int pass = 0; pass < 2 && !(mask & VMS_SECCOMP_OURS); pass++) {
            if (pass && (!o.notify || o.learn)) break;
            if (pass) o.notify = 0;
            int n = vm_sc_build(&o, filt, BPF_MAXINSNS);
            if (n <= 0 || n > BPF_MAXINSNS) break;
            struct sock_fprog prog = { .len = (unsigned short)n, .filter = filt };
            long r = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, o.notify ? SECCOMP_FILTER_FLAG_NEW_LISTENER : 0, &prog);
            if (r < 0) continue;
            mask |= VMS_SECCOMP_OURS | VMS_SECCOMP_ALLOWLIST;
            if (o.notify) { mask |= VMS_SECCOMP_NAMED; *j->notify_out = (int)r; }
        }
    }
    return mask;
}

static int apply_seccomp(void) {
    if (!VM_AUDIT_ARCH) return 0;
    static struct sock_filter filt[256];
    int n = tool_filter(filt, (int)(sizeof filt / sizeof filt[0]), 0);
    if (n > (int)(sizeof filt / sizeof filt[0])) return 0;
    struct sock_fprog prog = { .len = (unsigned short)n, .filter = filt };
    return !syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) ? VMS_SECCOMP_OURS : 0;
}

int vm_jail_tool(const char *read_file, const char *rw_dir) { return vm_jail_tool2(read_file, NULL, rw_dir); }
int vm_jail_tool2(const char *read_file, const char *read_file2, const char *rw_dir) {
    const char *r[2] = { read_file, read_file2 };
    return vm_jail_tool_n(r, read_file2 ? 2 : 1, rw_dir);
}
int vm_jail_tool_n(const char *const *reads, int nreads, const char *rw_dir) {
    int mask = 0;
    struct rlimit no = { 0, 0 };
    setrlimit(RLIMIT_CORE, &no);
    if (!prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) mask |= VMS_NO_NEW_PRIVS;
    int abi = vm_landlock_abi();
    if (abi >= 1) {
        struct landlock_ruleset_attr attr;
        memset(&attr, 0, sizeof attr);
        uint64_t h = fs_handled(abi);
        attr.handled_access_fs = h;
        if (abi >= 4) attr.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
#ifdef LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET
        if (abi >= 6) attr.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
#endif
        int rs = (int)syscall(SYS_landlock_create_ruleset, &attr, sizeof attr, 0U);
        if (rs >= 0) {
            uint64_t ro = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
            uint64_t rw = ro | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_REMOVE_FILE;
            int ok = !rule(rs, "/usr", ro | LANDLOCK_ACCESS_FS_EXECUTE, h) && nreads > 0 && reads[0];
            for (int i = 0; ok && i < nreads; i++) if (!reads[i] || rule(rs, reads[i], LANDLOCK_ACCESS_FS_READ_FILE, h)) ok = 0;
            if (rw_dir && rule(rs, rw_dir, rw, h)) ok = 0;
            rule(rs, "/etc/ld.so.cache", LANDLOCK_ACCESS_FS_READ_FILE, h);
            rule(rs, "/dev/null", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE, h);
            rule(rs, "/proc/self", ro, h);
            rule(rs, "/sys/devices/system/cpu", ro, h);
            if (ok && !syscall(SYS_landlock_restrict_self, rs, 0U)) mask |= VMS_LANDLOCK_FS | (abi >= 4 ? VMS_LANDLOCK_NET : 0);
            close(rs);
        }
    }
    mask |= apply_seccomp();
    return mask;
}

int vm_jail_self(const char *machine_dir, const char *runtime_dir, const char *xfer_dir) {
    int abi = vm_landlock_abi();
    if (abi < 1) return 0;
    struct landlock_ruleset_attr attr;
    memset(&attr, 0, sizeof attr);
    uint64_t h = fs_handled(abi);
    attr.handled_access_fs = h;
    int rs = (int)syscall(SYS_landlock_create_ruleset, &attr, sizeof attr.handled_access_fs, 0U);
    if (rs < 0) return 0;
    uint64_t rw = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_WRITE_FILE |
                  LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE;
    int ok = !rule(rs, machine_dir, rw, h);
    if (runtime_dir && runtime_dir[0]) rule(rs, runtime_dir, LANDLOCK_ACCESS_FS_REMOVE_FILE, h);
    /* without it the drops go the agent's way (lord-vm can't make the file there) */
    if (xfer_dir && xfer_dir[0]) rule(rs, xfer_dir, LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_MAKE_REG |
                                      LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE | LANDLOCK_ACCESS_FS_MAKE_DIR |
                                      LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_READ_DIR, h);
    rule(rs, "/dev/null", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE, h);
    int r = ok && !prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) && !syscall(SYS_landlock_restrict_self, rs, 0U);
    close(rs);
    return r ? VMS_SUPERVISOR_LANDLOCK : 0;
}

/* ---- QEMU's namespaces and its private loader (vmsandbox.h; docs/VIRTUALIZATION.md 3.3) ---- */

static int write_all_fd(int fd, const void *p, size_t n) {
    const char *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

static int read_full(int fd, void *p, size_t n) {
    char *b = p;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, b + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    return (int)got;
}

int vm_userns_open(void) {
    unsigned uid = (unsigned)getuid();
    int up[2], hold[2];
    if (pipe2(up, O_CLOEXEC)) return -1;
    if (pipe2(hold, O_CLOEXEC)) { int e = errno; close(up[0]); close(up[1]); errno = e; return -1; }
    pid_t h = fork();
    if (h < 0) { int e = errno; close(up[0]); close(up[1]); close(hold[0]); close(hold[1]); errno = e; return -1; }
    if (!h) {
        /* the holder: makes the namespace, maps only the person's uid in it (no group: see the header), says so, and
         * waits until the caller holds it */
        close(up[0]);
        close(hold[1]);
        int32_t e = 0;
        char map[48];
        int n = snprintf(map, sizeof map, "%u %u 1\n", uid, uid);
        int f = -1;
        if (unshare(CLONE_NEWUSER)) e = errno;
        else if ((f = open("/proc/self/uid_map", O_WRONLY | O_CLOEXEC)) < 0 || write(f, map, (size_t)n) != n) e = errno ? errno : EIO;
        if (f >= 0) close(f);
        if (write_all_fd(up[1], &e, sizeof e)) _exit(1);
        char c;
        while (read(hold[0], &c, 1) < 0 && errno == EINTR) {}
        _exit(0);
    }
    close(up[1]);
    close(hold[0]);
    int32_t e = EIO;
    int fd = -1;
    if (read_full(up[0], &e, sizeof e) == (int)sizeof e && !e) {
        char p[64];
        snprintf(p, sizeof p, "/proc/%d/ns/user", (int)h);
        fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0) e = errno;
    }
    close(up[0]);
    close(hold[1]);
    while (waitpid(h, NULL, 0) < 0 && errno == EINTR) {}
    if (fd < 0) errno = e ? e : EIO;
    return fd;
}

int vm_ns_enter(int userns) {
    if (userns < 0) return 0;
    if (setns(userns, CLONE_NEWUSER)) { dprintf(2, "lord-vm: QEMU's user namespace: %s\n", strerror(errno)); return 0; }
    if (unshare(CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWNET)) {
        dprintf(2, "lord-vm: QEMU's mount, ipc, uts and net namespaces: %s\n", strerror(errno));
        return 0;
    }
    return 1;
}


/* the copy holds exactly the loader's bytes */
static int same_bytes(int a, int b, off_t size) {
    static char x[65536], y[65536];
    for (off_t o = 0; o < size;) {
        size_t n = (size_t)(size - o) < sizeof x ? (size_t)(size - o) : sizeof x;
        if (pread(a, x, n, o) != (ssize_t)n || pread(b, y, n, o) != (ssize_t)n || memcmp(x, y, n)) return 0;
        o += (off_t)n;
    }
    return 1;
}

/* an unreadable copy of the loader over the loader, in this mount namespace only: 0 when it's there.
 * A file can be mounted only while it has a name (an O_TMPFILE's detached mount can't be moved: ENOENT), so the copy
 * has one for an instant, random, in the caller's private runtime folder; then it is unlinked, a write lease proves no
 * other descriptor is open on it (none can be opened later: no name, and our /proc is closed to the person's
 * programs), the mount is checked to be this very file, and its bytes the loader's. Anything else: no copy. */
static int private_loader(const char *tmpdir, char *why, size_t wn) {
    char real[PATH_MAX], name[PATH_MAX + 64];
    if (!VM_LOADER[0] || !realpath(VM_LOADER, real) || strncmp(real, "/usr/", 5)) { snprintf(why, wn, "no loader at %s under /usr", VM_LOADER); return -1; }
    int src = open(real, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (src < 0 || fstat(src, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (64 << 20)) {
        snprintf(why, wn, "%s: %s", real, strerror(errno ? errno : EINVAL));
        if (src >= 0) close(src);
        return -1;
    }
    unsigned long long rnd = 0;
    if (syscall(SYS_getrandom, &rnd, sizeof rnd, 0) != (long)sizeof rnd) rnd ^= (unsigned long long)st.st_ino * 2654435761u;
    snprintf(name, sizeof name, "%s/.lordvm-ld-%d-%016llx", tmpdir, (int)getpid(), rnd);
    int fd = open(name, O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0100);
    if (fd < 0) { snprintf(why, wn, "a private file in %s: %s", tmpdir, strerror(errno)); close(src); return -1; }
    off_t off = 0;
    while (off < st.st_size) {
        ssize_t w = sendfile(fd, src, &off, (size_t)(st.st_size - off));
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
    }
    int r = -1;
    struct stat mine, there;
    if (off != st.st_size || fchmod(fd, 0100)) snprintf(why, wn, "copy: %s", strerror(errno ? errno : EIO));
    else if (mount(name, real, NULL, MS_BIND, NULL)) snprintf(why, wn, "mount over %s: %s", real, strerror(errno));
    else {
        unlink(name);
        name[0] = 0;
        if (fcntl(fd, F_SETLEASE, F_WRLCK)) snprintf(why, wn, "the copy is open elsewhere (%s)", strerror(errno));
        else if (fstat(fd, &mine) || stat(real, &there) || mine.st_ino != there.st_ino || mine.st_dev != there.st_dev)
            snprintf(why, wn, "the file mounted isn't the copy");
        else if ((mine.st_mode & 07777) != 0100 || !same_bytes(fd, src, st.st_size)) snprintf(why, wn, "the copy changed");
        else r = 0;
        fcntl(fd, F_SETLEASE, F_UNLCK);
        if (r) umount2(real, MNT_DETACH);
    }
    if (name[0]) unlink(name);
    close(src);
    close(fd);           /* still open for writing, exec would refuse the file (ETXTBSY) */
    return r;
}

/* `dir` (and what is mounted under it) seen again at the same path, noexec, nosuid and nodev, in this mount namespace
 * only: a bind of the folder over itself, then a remount of that bind. A file QEMU wrote there can then be neither run
 * nor mapped executable (the kernel refuses PROT_EXEC on a noexec mount: EPERM), whatever seccomp lets through. What
 * the mount already had stays: read-only stays read-only, its atime flags are kept (a remount without any keeps them),
 * and in a user namespace the kernel refuses to clear a flag the outer mount locked. 0 when the folder is noexec now */
static int noexec_bind(const char *dir, char *why, size_t wn) {
    struct statvfs sv;
    if (statvfs(dir, &sv)) { snprintf(why, wn, "%s", strerror(errno)); return -1; }
    unsigned long keep = (sv.f_flag & ST_RDONLY) ? MS_RDONLY : 0;
    if (mount(dir, dir, NULL, MS_BIND | MS_REC, NULL)) { snprintf(why, wn, "bind: %s", strerror(errno)); return -1; }
    if (mount(NULL, dir, NULL, MS_REMOUNT | MS_BIND | MS_NOEXEC | MS_NOSUID | MS_NODEV | keep, NULL)) {
        snprintf(why, wn, "remount: %s", strerror(errno));
        umount2(dir, MNT_DETACH);
        return -1;
    }
    if (statvfs(dir, &sv) || !(sv.f_flag & ST_NOEXEC)) { snprintf(why, wn, "still executable after the remount"); return -1; }
    return 0;
}

int vm_ns_setup(const char *tmpdir, const char *disks) {
    int bits = 0;
    char why[PATH_MAX + 64];
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)) {
        dprintf(2, "lord-vm: QEMU's mount namespace: %s\n", strerror(errno));
        return 0;
    }
    const char *td = tmpdir && tmpdir[0] ? tmpdir : "/dev/shm";
    if (!private_loader(td, why, sizeof why)) bits |= VMNS_LOADER;
    else dprintf(2, "lord-vm: QEMU's private loader: %s (its memory stays readable by the person's programs)\n", why);
    /* after the loader: its bind is a mount of its own, made while the runtime folder still allowed exec, and stays so.
     * The disks folder is the only one QEMU's Landlock lets it write; the temporary and runtime folders are for a
     * kernel without Landlock. If a remount fails QEMU still runs: Landlock and seccomp still hold, and the log says so */
    if (disks && disks[0]) {
        if (!noexec_bind(disks, why, sizeof why)) bits |= VMNS_NOEXEC;
        else dprintf(2, "lord-vm: QEMU's disks folder couldn't be made noexec in its mount namespace (%s): a file QEMU writes "
                        "there could still be mapped executable where seccomp has no listener; Landlock and seccomp still hold\n", why);
    }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    const char *tmps[] = { "/dev/shm", "/tmp", "/var/tmp", rt && rt[0] == '/' ? rt : td };
    for (size_t i = 0; i < sizeof tmps / sizeof tmps[0]; i++) {
        struct stat st;
        if (stat(tmps[i], &st) || !S_ISDIR(st.st_mode)) continue;
        if (noexec_bind(tmps[i], why, sizeof why))
            dprintf(2, "lord-vm: %s couldn't be made noexec in QEMU's mount namespace (%s); Landlock and seccomp still hold\n", tmps[i], why);
    }
    return bits;
}

int vm_nondumpable(pid_t pid) {
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/environ", (int)pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { close(fd); return 0; }
    return errno == EACCES || errno == EPERM;
}
