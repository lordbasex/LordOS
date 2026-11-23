/* llmjail.c — the local model engine's sandbox (see llmjail.h and docs/SENTINELA.md). */
#include "llmjail.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__has_include)
#  if __has_include(<linux/landlock.h>)
#    include <linux/landlock.h>
#    define HAVE_LANDLOCK 1
#  endif
#endif

/* the engine's arguments: the model through its descriptor, the socket in the runtime folder (the working directory),
 * two threads, a 4096-token context, one request at a time, no web interface, no GPU, no prompt cache in RAM, no
 * network for downloads, no thinking (the answer is a short JSON) */
const char *const LLMJAIL_ARGV[] = {
    "llama-server", "-m", "/proc/self/fd/3", "--host", "engine.sock", "-t", "2", "-tb", "2", "--threads-http", "2",
    "-c", "4096", "-np", "1", "--no-webui", "--no-slots", "-ngl", "0", "-dev", "none", "-cram", "0", "--offline",
    "--reasoning", "off", "--no-warmup", NULL,
};

/* ---- 1. namespaces ---- */

static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

static int namespaces(int *mask) {
    uid_t uid = getuid();
    gid_t gid = getgid();
    if (unshare(CLONE_NEWUSER | CLONE_NEWNET)) return -1;
    *mask |= LJ_USERNS | LJ_NETNS;
    char m[64];
    /* the same uid and gid inside: the socket it makes belongs to the user, and nothing in it is root of anything */
    snprintf(m, sizeof m, "%u %u 1\n", (unsigned)uid, (unsigned)uid);
    if (write_file("/proc/self/uid_map", m)) return -1;
    if (write_file("/proc/self/setgroups", "deny\n")) return -1;
    snprintf(m, sizeof m, "%u %u 1\n", (unsigned)gid, (unsigned)gid);
    if (write_file("/proc/self/gid_map", m)) return -1;
    return 0;
}

/* ---- 2. Landlock ---- */

#ifdef HAVE_LANDLOCK
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif
#ifndef LANDLOCK_ACCESS_FS_IOCTL_DEV
#define LANDLOCK_ACCESS_FS_IOCTL_DEV (1ULL << 15)
#endif
#define LJ_NET_BIND_TCP (1ULL << 0)
#define LJ_NET_CONNECT_TCP (1ULL << 1)
#define LJ_SCOPE_ABSTRACT_UNIX (1ULL << 0)
#define LJ_SCOPE_SIGNAL (1ULL << 1)

/* struct landlock_ruleset_attr as of ABI 6 (an older header may not have the last two fields) */
struct lj_ruleset_attr { uint64_t handled_access_fs, handled_access_net, scoped; };

static int add_fd(int rs, int fd, uint64_t allow) {
    struct landlock_path_beneath_attr pb = { .allowed_access = allow, .parent_fd = fd };
    return (int)syscall(SYS_landlock_add_rule, rs, LANDLOCK_RULE_PATH_BENEATH, &pb, 0U);
}

static int add_path(int rs, const char *path, uint64_t allow, int file) {
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    /* a file takes only file rights */
    if (file || (!fstat(fd, &st) && !S_ISDIR(st.st_mode)))
        allow &= LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE;
    int r = add_fd(rs, fd, allow);
    close(fd);
    return r;
}

static int landlock(const LlmJailCfg *cfg, int *mask) {
    int abi = (int)syscall(SYS_landlock_create_ruleset, NULL, 0U, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) return -1;
    uint64_t fs = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
                  LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                  LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
                  LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                  LANDLOCK_ACCESS_FS_MAKE_SYM;
    if (abi >= 2) fs |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 3) fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
    if (abi >= 5) fs |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
    struct lj_ruleset_attr attr = { .handled_access_fs = fs };
    size_t size = sizeof attr.handled_access_fs;
    if (abi >= 4) { attr.handled_access_net = LJ_NET_BIND_TCP | LJ_NET_CONNECT_TCP; size = 2 * sizeof(uint64_t); }
    if (abi >= 6) { attr.scoped = LJ_SCOPE_ABSTRACT_UNIX | LJ_SCOPE_SIGNAL; size = sizeof attr; }
    int rs = (int)syscall(SYS_landlock_create_ruleset, &attr, size, 0U);
    if (rs < 0) return -1;
    const uint64_t RD = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
    int bad = 0;
    bad |= add_path(rs, "/usr", RD, 0);                           /* libraries, ggml's CPU backends; never executable */
    add_path(rs, "/etc/ld.so.cache", LANDLOCK_ACCESS_FS_READ_FILE, 1);
    add_path(rs, "/sys/devices/system/cpu", RD, 0);               /* CPU topology */
    add_path(rs, "/dev/null", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE, 1);
    add_path(rs, "/dev/urandom", LANDLOCK_ACCESS_FS_READ_FILE, 1);
    /* the dynamic loader is opened for execution with the engine: executable, nothing else in /usr is */
    static const char *const loaders[] = { "/lib64/ld-linux-x86-64.so.2", "/lib/ld-linux-aarch64.so.1", NULL };
    for (const char *const *l = loaders; *l; l++) add_path(rs, *l, LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE, 1);
    bad |= add_fd(rs, cfg->engine_fd, LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE);
    bad |= add_fd(rs, cfg->model_fd, LANDLOCK_ACCESS_FS_READ_FILE);
    bad |= add_fd(rs, cfg->dir_fd, LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_REMOVE_FILE);
    if (bad) { close(rs); return -1; }
    if (syscall(SYS_landlock_restrict_self, rs, 0U)) { close(rs); return -1; }
    close(rs);
    *mask |= LJ_LANDLOCK_FS;
    if (abi >= 4) *mask |= LJ_LANDLOCK_NET;
    if (abi >= 6) *mask |= LJ_LANDLOCK_SCOPE;
    return 0;
}
#else
static int landlock(const LlmJailCfg *cfg, int *mask) { (void)cfg; (void)mask; return -1; }
#endif

/* ---- 3. seccomp ---- */

#if defined(__x86_64__)
#define LJ_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define LJ_ARCH AUDIT_ARCH_AARCH64
#else
#define LJ_ARCH 0
#endif

#define EMIT(x) do { if (n < max) f[n] = (struct sock_filter)x; n++; } while (0)
#define RET_ERR(e) BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ((e) & SECCOMP_RET_DATA))
#define RET_ALLOW BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
#define LD_NR BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr))
#define LD_ARG(i) BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[i]))

static int filter(struct sock_filter *f, int max) {
    int n = 0;
    EMIT(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, LJ_ARCH, 1, 0));
    EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    EMIT(LD_NR);
#if defined(__x86_64__)
    EMIT(BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1));     /* x32 calls */
    EMIT(RET_ERR(EPERM));
#endif
    /* socket: AF_UNIX and SOCK_STREAM only. [0]jeq socket [1]ld a0 [2]jeq unix [3]EAFNOSUPPORT [4]ld a1 [5]and 0xf
     * [6]jeq stream [7]EPERM [8]ALLOW */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 8));
    EMIT(LD_ARG(0));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 1, 0));
    EMIT(RET_ERR(EAFNOSUPPORT));
    EMIT(LD_ARG(1));
    EMIT(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0xf));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SOCK_STREAM, 1, 0));
    EMIT(RET_ERR(EPERM));
    EMIT(RET_ALLOW);
    EMIT(LD_NR);
    /* socketpair: AF_UNIX only */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socketpair, 0, 4));
    EMIT(LD_ARG(0));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 1, 0));
    EMIT(RET_ERR(EAFNOSUPPORT));
    EMIT(RET_ALLOW);
    EMIT(LD_NR);
    /* execveat: only the engine's descriptor, empty path; it is close-on-exec, so once the engine runs that number
     * names nothing it was given, and Landlock lets nothing else in /usr be executed */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_execveat, 0, 6));
    EMIT(LD_ARG(0));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, LLMJAIL_ENGINE_FD, 0, 3));
    EMIT(LD_ARG(4));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AT_EMPTY_PATH, 0, 1));
    EMIT(RET_ALLOW);
    EMIT(RET_ERR(EPERM));
    EMIT(LD_NR);
    /* clone: no new namespace of any kind */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 0, 4));
    EMIT(LD_ARG(0));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, CLONE_NEWNS | CLONE_NEWUSER | CLONE_NEWNET | CLONE_NEWPID | CLONE_NEWIPC |
                  CLONE_NEWUTS | CLONE_NEWCGROUP, 0, 1));
    EMIT(RET_ERR(EPERM));
    EMIT(RET_ALLOW);
    EMIT(LD_NR);
    /* ioctl: never TIOCSTI or TIOCLINUX (typing into a terminal) */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_ioctl, 0, 5));
    EMIT(LD_ARG(1));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x5412 /* TIOCSTI */, 1, 0));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x541C /* TIOCLINUX */, 0, 1));
    EMIT(RET_ERR(EPERM));
    EMIT(RET_ALLOW);
    EMIT(LD_NR);
    /* clone3 and io_uring: ENOSYS (glibc falls back to clone; io_uring's own socket and connect would bypass this) */
    static const int nosys[] = { SYS_clone3, SYS_io_uring_setup, SYS_io_uring_enter, SYS_io_uring_register };
    for (size_t i = 0; i < sizeof nosys / sizeof nosys[0]; i++) {
        EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)nosys[i], 0, 1));
        EMIT(RET_ERR(ENOSYS));
    }
    static const int deny[] = {
        SYS_connect, SYS_execve, SYS_ptrace, SYS_process_vm_readv, SYS_process_vm_writev, SYS_pidfd_getfd,
        SYS_mount, SYS_umount2, SYS_pivot_root, SYS_chroot, SYS_unshare, SYS_setns, SYS_mount_setattr, SYS_open_tree,
        SYS_move_mount, SYS_fsopen, SYS_fsmount, SYS_fsconfig, SYS_fspick, SYS_bpf, SYS_perf_event_open, SYS_keyctl,
        SYS_add_key, SYS_request_key, SYS_userfaultfd, SYS_kexec_load, SYS_kexec_file_load, SYS_init_module,
        SYS_finit_module, SYS_delete_module, SYS_acct, SYS_swapon, SYS_swapoff, SYS_reboot, SYS_settimeofday,
        SYS_clock_settime, SYS_adjtimex, SYS_clock_adjtime, SYS_quotactl, SYS_name_to_handle_at, SYS_open_by_handle_at,
        SYS_syslog, SYS_vhangup, SYS_landlock_create_ruleset, SYS_seccomp,
#if defined(__x86_64__)
        SYS_iopl, SYS_ioperm, SYS_uselib, SYS_ustat, SYS_sysfs,
#endif
    };
    for (size_t i = 0; i < sizeof deny / sizeof deny[0]; i++) {
        EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)deny[i], 0, 1));
        EMIT(RET_ERR(EPERM));
    }
    EMIT(RET_ALLOW);
    return n;
}
#undef EMIT

static int seccomp(void) {
    if (!LJ_ARCH) return -1;
    static struct sock_filter f[256];
    int n = filter(f, (int)(sizeof f / sizeof f[0]));
    if (n > (int)(sizeof f / sizeof f[0])) return -1;
    struct sock_fprog prog = { .len = (unsigned short)n, .filter = f };
    return (int)syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &prog) ? -1 : 0;
}

int llmjail_apply(const LlmJailCfg *cfg) {
    int mask = 0;
    if (namespaces(&mask)) {
        if (cfg->need_netns) return -1;
        /* unprivileged user namespaces off: the seccomp filter and Landlock still keep it off the network */
        if (!(mask & LJ_NETNS) && !unshare(CLONE_NEWNET)) mask |= LJ_NETNS;
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return -1;
    mask |= LJ_NOPRIV;
    landlock(cfg, &mask);
    if (seccomp()) return -1;
    mask |= LJ_SECCOMP;
    return mask;
}
