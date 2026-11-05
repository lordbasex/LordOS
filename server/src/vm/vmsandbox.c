/* vmsandbox.c — Landlock and seccomp around QEMU and lord-vm (see vmsandbox.h). */
#include "vmsandbox.h"
#include "vmproto.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__x86_64__)
#define VM_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define VM_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#define VM_AUDIT_ARCH 0
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

/* ---- seccomp: QEMU's denylist ---- */

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

static int qemu_filter(struct sock_filter *f, int cap) {
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
    /* clone: no new namespaces. [0]jeq [1]ld flags [2]and [3]jeq 0 → ok [4]EPERM [5]ALLOW */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 0, 5));
    EMIT(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])));
    EMIT(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, (unsigned)CLONE_NS_FLAGS));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0));
    RET_ERR(EPERM);
    RET_OK();
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
            char self[64];
            snprintf(self, sizeof self, "/proc/%d", (int)getpid());
            int ok = !rule(rs, "/usr", rx, h) && !rule(rs, j->disks_dir, rw, h) && !rule(rs, self, ro, h);
            rule(rs, "/etc/ld.so.cache", LANDLOCK_ACCESS_FS_READ_FILE, h);
            rule(rs, "/dev/null", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE, h);
            if (j->kvm) {
                uint64_t dev = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE;
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
                dev |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
                rule(rs, "/dev/kvm", dev, h);
            }
            if (j->iso && j->iso[0] && rule(rs, j->iso, LANDLOCK_ACCESS_FS_READ_FILE, h)) ok = 0;
            if (ok && !syscall(SYS_landlock_restrict_self, rs, 0U)) {
                mask |= VMS_LANDLOCK_FS;
                if (abi >= 4) mask |= VMS_LANDLOCK_NET;
                if (abi >= 6) mask |= VMS_LANDLOCK_SCOPE;
            }
            close(rs);
        }
    }

    if (VM_AUDIT_ARCH) {
        static struct sock_filter filt[256];
        int n = qemu_filter(filt, (int)(sizeof filt / sizeof filt[0]));
        if (n <= (int)(sizeof filt / sizeof filt[0])) {
            struct sock_fprog prog = { .len = (unsigned short)n, .filter = filt };
            if (!syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog)) mask |= VMS_SECCOMP_OURS;
        }
    }
    return mask;
}

static int apply_seccomp(void) {
    if (!VM_AUDIT_ARCH) return 0;
    static struct sock_filter filt[256];
    int n = qemu_filter(filt, (int)(sizeof filt / sizeof filt[0]));
    if (n > (int)(sizeof filt / sizeof filt[0])) return 0;
    struct sock_fprog prog = { .len = (unsigned short)n, .filter = filt };
    return !syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) ? VMS_SECCOMP_OURS : 0;
}

int vm_jail_tool(const char *read_file, const char *rw_dir) {
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
            int ok = !rule(rs, "/usr", ro | LANDLOCK_ACCESS_FS_EXECUTE, h) && read_file && !rule(rs, read_file, LANDLOCK_ACCESS_FS_READ_FILE, h);
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

int vm_jail_self(const char *machine_dir, const char *runtime_dir) {
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
    rule(rs, "/dev/null", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE, h);
    int r = ok && !prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) && !syscall(SYS_landlock_restrict_self, rs, 0U);
    close(rs);
    return r ? VMS_SUPERVISOR_LANDLOCK : 0;
}
