/* sandbox.c — sandbox for .lpk apps (see sandbox.h and docs/PRIVACY.md).
 *
 * Network decision: lord-server runs without root, so it can't create a network namespace
 * (unshare(CLONE_NEWNET) requires CAP_SYS_ADMIN). Instead, an app without the `network` permission
 * stays in the machine's netns but can't open network sockets: seccomp rejects
 * socket(AF_INET/AF_INET6/AF_PACKET) and Landlock (ABI ≥ 4) denies it TCP bind/connect. The
 * monitoring agent also sets per-cgroup nftables rules (defense in depth). AF_UNIX
 * sockets remain allowed: Wayland and the broker need them (the broker arrives as an already
 * connected fd, no connect). */
#include "sandbox.h"
#include "cgroup.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/capability.h>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/socket.h>

#ifdef __has_include
#  if __has_include(<linux/openat2.h>)
#    include <linux/openat2.h>
#    define HAVE_OPENAT2 1
#  endif
#  if __has_include(<linux/landlock.h>)
#    include <linux/landlock.h>
#    define HAVE_LANDLOCK 1
#  endif
#endif

void sandbox_scope_name(const char *id, char *out, size_t n) { snprintf(out, n, "app-lordos-lpk-%s.scope", id); }

/* ---- no_new_privs and capabilities ---- */

static int apply_nopriv(void) { return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 ? 0 : -1; }

static int drop_caps(void) {
    struct __user_cap_header_struct hdr = { 0x20080522 /* _LINUX_CAPABILITY_VERSION_3 */, 0 };
    struct __user_cap_data_struct data[2] = { { 0, 0, 0 }, { 0, 0, 0 } };
    /* also drop the bounding set and the ambient ones (best effort) */
    for (int c = 0; c <= 40; c++) prctl(PR_CAPBSET_DROP, c, 0, 0, 0);
    prctl(PR_SET_SECUREBITS, 0, 0, 0, 0);
    return syscall(SYS_capset, &hdr, data) == 0 ? 0 : -1;
}

/* ---- Landlock ---- */

#ifdef HAVE_LANDLOCK
static int ll_create(struct landlock_ruleset_attr *a, size_t n) { return (int)syscall(SYS_landlock_create_ruleset, a, n, 0U); }
static int ll_add(int fd, enum landlock_rule_type t, const void *attr) { return (int)syscall(SYS_landlock_add_rule, fd, t, attr, 0U); }
static int ll_enforce(int fd) { return (int)syscall(SYS_landlock_restrict_self, fd, 0U); }
/* The kernel's Landlock ABI. LORD_SANDBOX_MAX_ABI (tests only) can lower it, never raise it: the
 * only effect is that restricted apps refuse to start. */
static int ll_abi(void) {
    int abi = (int)syscall(SYS_landlock_create_ruleset, NULL, 0U, LANDLOCK_CREATE_RULESET_VERSION);
    const char *cap = getenv("LORD_SANDBOX_MAX_ABI");
    if (cap && *cap >= '0' && *cap <= '9' && atoi(cap) < abi) abi = atoi(cap);
    return abi;
}

static int ll_path(int rs, const char *path, uint64_t allow) {
    struct landlock_path_beneath_attr pb = { .allowed_access = allow };
    pb.parent_fd = open(path, O_PATH | O_CLOEXEC);
    if (pb.parent_fd < 0) return -1;
    int r = ll_add(rs, LANDLOCK_RULE_PATH_BENEATH, &pb);
    close(pb.parent_fd);
    return r;
}

/* Rule on a folder that's already open (an fd from lord-server). */
static int ll_fd(int rs, int fd, uint64_t allow) {
    struct landlock_path_beneath_attr pb = { .allowed_access = allow, .parent_fd = fd };
    return ll_add(rs, LANDLOCK_RULE_PATH_BENEATH, &pb);
}
#endif


/* ro_fds / rw_fds: already open folders (helpers); NULL for apps. */
static int apply_landlock(const SandboxConfig *cfg, const int *ro_fds, const int *rw_fds, int *fs_ok, int *net_ok) {
    *fs_ok = *net_ok = 0;
#ifdef HAVE_LANDLOCK
    int abi = ll_abi();
    if (abi < 1) { fprintf(stderr, "sandbox: no Landlock (ABI %d); seccomp only\n", abi); return -1; }

    uint64_t ro = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE;
    uint64_t rw = ro | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
                  LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_MAKE_SYM |
                  LANDLOCK_ACCESS_FS_TRUNCATE;
    uint64_t fs_handled = ro | rw;
    /* accesses an old ABI doesn't know: removed from "handled" so creating the set doesn't fail */
    if (abi < 2) fs_handled &= ~(uint64_t)LANDLOCK_ACCESS_FS_REFER;
    if (abi < 3) fs_handled &= ~(uint64_t)LANDLOCK_ACCESS_FS_TRUNCATE;
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
    if (abi >= 5) fs_handled |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
    rw &= fs_handled; ro &= fs_handled;
    /* helpers' rw folders: write, create and delete files, but NOT truncate */
    uint64_t rwfd = (ro | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
                     LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_REMOVE_FILE) & fs_handled;

    struct landlock_ruleset_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.handled_access_fs = fs_handled;
#ifdef LANDLOCK_ACCESS_NET_CONNECT_TCP
    uint64_t net_handled = (abi >= 4) ? (LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP) : 0;
    attr.handled_access_net = net_handled;
#endif
#ifdef LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET
    if (abi >= 6) attr.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
#endif
    int rs = ll_create(&attr, sizeof attr);
    if (rs < 0) { fprintf(stderr, "sandbox: could not create Landlock ruleset: %s\n", strerror(errno)); return -1; }

    /* read-only: runtime (fonts, assets) and the package folder */
    if (cfg->pkg_dir) ll_path(rs, cfg->pkg_dir, ro);
    for (const char *const *p = cfg->ro_extra; p && *p; p++) ll_path(rs, *p, ro);
    uint64_t dev = (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE) & fs_handled;
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
    if (abi >= 5) dev |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
    for (const char *const *p = cfg->dev_files; p && *p; p++) ll_path(rs, *p, dev);
    /* read and write: only the app's data folder */
    if (cfg->data_dir) ll_path(rs, cfg->data_dir, rw);
    /* folders passed as already open fds (helpers) */
    for (const int *p = ro_fds; p && *p >= 0; p++) ll_fd(rs, *p, ro);
    for (const int *p = rw_fds; p && *p >= 0; p++) ll_fd(rs, *p, rwfd);
    /* the Wayland socket and the broker's live in XDG_RUNTIME_DIR: read (connection via an already open fd) */
    if (cfg->runtime_dir) ll_path(rs, cfg->runtime_dir, LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR);
    /* /dev/null, /dev/urandom and the system libraries: read and execute only */
    ll_path(rs, "/dev/null", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE);
    ll_path(rs, "/dev/urandom", LANDLOCK_ACCESS_FS_READ_FILE);
    ll_path(rs, "/usr/lib", ro);
    ll_path(rs, "/lib", ro);

    *fs_ok = 1;
    /* network: if the app does NOT have the permission, no network rule is added → all TCP stays
     * forbidden. If it has it, bind and connect to any port are allowed. */
#ifdef LANDLOCK_ACCESS_NET_CONNECT_TCP
    if (net_handled) {
        if (cfg->network) {
            struct landlock_net_port_attr np = { .allowed_access = LANDLOCK_ACCESS_NET_CONNECT_TCP | LANDLOCK_ACCESS_NET_BIND_TCP };
            for (int port = 0; port <= 65535; port++) { np.port = (uint64_t)port; ll_add(rs, LANDLOCK_RULE_NET_PORT, &np); }
        }
        *net_ok = 1;
    }
#endif
    if (apply_nopriv()) { close(rs); return -1; }     /* Landlock requires no_new_privs */
    int r = ll_enforce(rs);
    close(rs);
    if (r) { fprintf(stderr, "sandbox: could not apply Landlock: %s\n", strerror(errno)); *fs_ok = *net_ok = 0; return -1; }
    return 0;
#else
    (void)cfg; (void)ro_fds; (void)rw_fds;
    fprintf(stderr, "sandbox: built without Landlock\n");
    return -1;
#endif
}

/* ---- "no files" for Linux apps (sandbox.h) ---- */

/* The "Open with" grant, opened again without following any symlink, and only if it is still the
 * inode the launcher checked: a running instance of the app may have swapped a component of the
 * path for a symlink to somewhere else since. -1: the grant is dropped (fail closed). */
static int open_opened(const SandboxFilesCfg *cfg) {
#if defined(HAVE_OPENAT2) && defined(SYS_openat2)
    struct open_how how;
    memset(&how, 0, sizeof how);
    how.flags = O_PATH | O_CLOEXEC;
    how.resolve = RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
    int fd = (int)syscall(SYS_openat2, AT_FDCWD, cfg->opened, &how, sizeof how);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || (unsigned long long)st.st_dev != cfg->opened_dev || (unsigned long long)st.st_ino != cfg->opened_ino ||
        !(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode))) { close(fd); return -1; }
    return fd;
#else
    (void)cfg;
    return -1;
#endif
}

static int is_opened(const SandboxFilesCfg *cfg, const char *path) { return cfg->opened && !strcmp(cfg->opened, path); }

int sandbox_landlock_abi(void) {
#ifdef HAVE_LANDLOCK
    int abi = ll_abi();
    return abi > 0 ? abi : 0;
#else
    return 0;
#endif
}

int sandbox_apply_files(const SandboxFilesCfg *cfg) {
#if defined(HAVE_LANDLOCK) && defined(LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET)
    int abi = ll_abi();
    if (abi < 6) return -1;             /* fail closed: abstract unix sockets must be scoped too */
    uint64_t ro = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE;
    uint64_t rw = ro | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
                  LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_MAKE_SYM |
                  LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_CHAR |
                  LANDLOCK_ACCESS_FS_MAKE_BLOCK;
    uint64_t handled = rw;
    /* REFER (ABI 2): handled and granted in the writable folders. Leaving it unhandled would make
     * every rename between folders fail, which breaks browsers and editors. */
    if (abi >= 2) { handled |= LANDLOCK_ACCESS_FS_REFER; rw |= LANDLOCK_ACCESS_FS_REFER; }
    if (abi >= 3) { handled |= LANDLOCK_ACCESS_FS_TRUNCATE; rw |= LANDLOCK_ACCESS_FS_TRUNCATE; }
    /* IOCTL_DEV (ABI 5) is not handled: GPU and sound ioctls stay as they are */
    uint64_t file_rw = (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE |
                        (abi >= 3 ? LANDLOCK_ACCESS_FS_TRUNCATE : 0)) & handled;
    struct landlock_ruleset_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.handled_access_fs = handled;
    /* abstract unix sockets of programs outside the app (ABI 6): refused too */
    attr.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET;
    int rs = ll_create(&attr, sizeof attr);
    if (rs < 0) return -1;
    /* not "/run" or "/var" as a whole: /run/media/<user> and /var/mail are the user's */
    static const char *const sys_ro[] = { "/usr", "/bin", "/sbin", "/lib", "/lib32", "/lib64", "/libx32", "/etc", "/opt",
                                          "/var/lib", "/var/cache", "/sys", "/run/dbus", "/run/systemd", "/run/udev",
                                          "/run/resolvconf", "/run/NetworkManager", "/run/cups", "/run/avahi-daemon",
                                          "/run/lock", NULL };
    static const char *const sys_rw[] = { "/tmp", "/var/tmp", "/dev", "/proc", NULL };
    for (int i = 0; sys_ro[i]; i++) ll_path(rs, sys_ro[i], ro);
    for (int i = 0; sys_rw[i]; i++) ll_path(rs, sys_rw[i], rw);
    if (cfg->runtime_dir) ll_path(rs, cfg->runtime_dir, rw);
    for (int pass = 0; pass < 2; pass++) {
        const char *const *list = pass ? cfg->rw : cfg->ro;
        for (int i = 0; i < SANDBOX_FILES_MAX && list[i]; i++) {
            struct stat st;
            if (is_opened(cfg, list[i])) {
                int fd = open_opened(cfg);
                if (fd < 0) continue;
                if (!fstat(fd, &st))
                    ll_fd(rs, fd, S_ISDIR(st.st_mode) ? (pass ? rw : ro) : (pass ? file_rw : (LANDLOCK_ACCESS_FS_READ_FILE & handled)));
                close(fd);
                continue;
            }
            if (stat(list[i], &st)) continue;
            ll_path(rs, list[i], S_ISDIR(st.st_mode) ? (pass ? rw : ro) : (pass ? file_rw : (LANDLOCK_ACCESS_FS_READ_FILE & handled)));
        }
    }
    if (apply_nopriv()) { close(rs); return -1; }
    int r = ll_enforce(rs);
    close(rs);
    return r ? -1 : 0;
#else
    (void)cfg;
    return -1;
#endif
}

int sandbox_apply_scope(void) {
#if defined(HAVE_LANDLOCK) && defined(LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET) && defined(LANDLOCK_SCOPE_SIGNAL)
    int abi = ll_abi();
    if (abi < 6) {                      /* fail closed: the session's abstract sockets would stay reachable */
        static const char m[] = "sandbox: Landlock ABI < 6 (abstract unix sockets can't be scoped): not launched\n";
        if (write(2, m, sizeof m - 1) < 0) {}
        return -1;
    }
    if (apply_nopriv()) return -1;
    struct landlock_ruleset_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
    int rs = ll_create(&attr, sizeof attr);
    if (rs < 0) return -1;
    int r = ll_enforce(rs);
    close(rs);
    return r ? -1 : 0;
#else
    static const char m[] = "sandbox: built without Landlock scoping: not launched\n";
    if (write(2, m, sizeof m - 1) < 0) {}
    return -1;
#endif
}

/* ---- the app's own view: user + mount + PID namespace (sandbox.h) ---- */

#ifndef OPEN_TREE_CLONE
#define OPEN_TREE_CLONE 1
#endif
#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#endif

static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = strlen(text);
    int r = write(fd, text, n) == (ssize_t)n ? 0 : -1;
    close(fd);
    return r;
}

/* mkdir -p of the parents of path (inside a tmpfs we just mounted) */
static void mk_parents(const char *path) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    for (char *q = p + 1; *q; q++) {
        if (*q != '/') continue;
        *q = 0;
        mkdir(p, 0700);
        *q = '/';
    }
}

/* Binds an open_tree clone on path: creates an empty dir or file of the same kind first. */
static int bind_clone(int tree, const char *path, int is_dir) {
    mk_parents(path);
    if (is_dir) mkdir(path, 0700);
    else { int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600); if (fd >= 0) close(fd); }
    return (int)syscall(SYS_move_mount, tree, "", AT_FDCWD, path, MOVE_MOUNT_F_EMPTY_PATH);
}

static int is_under(const char *path, const char *dir) {
    size_t n = strlen(dir);
    return !strncmp(path, dir, n) && (path[n] == '/' || !path[n]);
}

int sandbox_files_isolate(const SandboxFilesCfg *cfg) {
    if (!cfg->home || cfg->home[0] != '/' || !strcmp(cfg->home, "/")) return -1;
    char map[64];
    unsigned uid = (unsigned)getuid(), gid = (unsigned)getgid();
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC)) return -1;
    if (write_file("/proc/self/setgroups", "deny")) return -1;
    snprintf(map, sizeof map, "%u %u 1", uid, uid);
    if (write_file("/proc/self/uid_map", map)) return -1;
    snprintf(map, sizeof map, "%u %u 1", gid, gid);
    if (write_file("/proc/self/gid_map", map)) return -1;
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)) return -1;

    /* the PID namespace starts with the next child: it is PID 1 */
    int sync[2];
    if (pipe2(sync, O_CLOEXEC)) return -1;
    pid_t init = fork();
    if (init < 0) return -1;
    if (init > 0) {
        /* tell the caller (through the exit status of nothing: we're an orphan) — wait until PID 1
         * says whether the view could be built, so a failure is logged before we go */
        char ok = 0;
        close(sync[1]);
        if (read(sync[0], &ok, 1) != 1) ok = 0;
        if (!ok) { static const char m[] = "sandbox: could not build the app's view (namespaces); not launched\n"; if (write(2, m, sizeof m - 1) < 0) {} }
        _exit(0);
    }
    close(sync[0]);

    /* PID 1: clones of everything that stays visible, taken before the tmpfs hide the originals */
    struct { int fd, dir; const char *path; int ro; } keep[2 * SANDBOX_FILES_MAX + 10];
    int nk = 0;
    for (int pass = 0; pass < 2; pass++) {
        const char *const *list = pass ? cfg->ro : cfg->rw;
        for (int i = 0; !cfg->keep_home && i < SANDBOX_FILES_MAX && list[i]; i++) {
            if (!is_under(list[i], cfg->home) || !strcmp(list[i], cfg->home)) continue;   /* outside home: Landlock alone */
            struct stat st;
            int t;
            if (is_opened(cfg, list[i])) {
                int fd = open_opened(cfg);
                if (fd < 0 || fstat(fd, &st)) { if (fd >= 0) close(fd); continue; }
                t = (int)syscall(SYS_open_tree, fd, "", OPEN_TREE_CLONE | O_CLOEXEC | AT_EMPTY_PATH);
                close(fd);
            } else {
                if (lstat(list[i], &st) || !(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode))) continue;
                t = (int)syscall(SYS_open_tree, AT_FDCWD, list[i], OPEN_TREE_CLONE | O_CLOEXEC);
            }
            if (t < 0) continue;
            keep[nk].fd = t; keep[nk].dir = S_ISDIR(st.st_mode); keep[nk].path = list[i]; keep[nk].ro = pass; nk++;
        }
    }
    char sockpath[8][PATH_MAX];
    int nsock = 0;
    for (int i = 0; cfg->runtime_dir && i < 8 && cfg->sockets[i]; i++) {
        snprintf(sockpath[nsock], PATH_MAX, "%s/%s", cfg->runtime_dir, cfg->sockets[i]);
        struct stat st;
        if (lstat(sockpath[nsock], &st) || !S_ISSOCK(st.st_mode)) continue;
        int t = (int)syscall(SYS_open_tree, AT_FDCWD, sockpath[nsock], OPEN_TREE_CLONE | O_CLOEXEC);
        if (t < 0) continue;
        keep[nk].fd = t; keep[nk].dir = 0; keep[nk].path = sockpath[nsock]; keep[nk].ro = 0; nk++;
        nsock++;
    }
    int bad = 0;
    const char *opts = "mode=0700,size=64m";
    if (!cfg->keep_home) bad |= mount("tmpfs", cfg->home, "tmpfs", MS_NOSUID | MS_NODEV, opts);
    if (cfg->runtime_dir) bad |= mount("tmpfs", cfg->runtime_dir, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0700,size=8m");
    bad |= mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
    struct stat vt;
    if (!stat("/var/tmp", &vt)) bad |= mount("tmpfs", "/var/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
    bad |= mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    /* /dev stays (GPU, sound, camera: lord-netperm decides those), but not the session's shared
     * memory nor its terminals (keystrokes of a sudo password, TIOCSTI) */
    if (!stat("/dev/shm", &vt)) bad |= mount("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
    bad |= mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "newinstance,ptmxmode=0666,mode=0620");
    if (!bad) bad |= mount("/dev/pts/ptmx", "/dev/ptmx", NULL, MS_BIND, NULL);
    for (int i = 0; i < nk && !bad; i++) {
        if (bind_clone(keep[i].fd, keep[i].path, keep[i].dir)) { bad = 1; break; }
        if (keep[i].ro && mount(NULL, keep[i].path, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY | MS_NOSUID | MS_NODEV, NULL)) { bad = 1; break; }
    }
    for (int i = 0; i < nk; i++) close(keep[i].fd);
    /* the working directory again, inside the view: the one inherited still points into the real
     * folders under the tmpfs (relative connect() to the session bus, sockets in the home) */
    if (!bad && (!cfg->cwd || chdir(cfg->cwd)) && chdir(cfg->home)) bad = 1;
    char ok = !bad;
    if (write(sync[1], &ok, 1) < 0) {}
    close(sync[1]);
    if (bad) _exit(1);

    pid_t app = fork();
    if (app < 0) _exit(1);
    if (app == 0) { prctl(PR_SET_PDEATHSIG, SIGKILL); return 0; }   /* the app: Landlock, then execve */
    /* PID 1 only reaps; it leaves with the app's status. SIGTERM/SIGINT/SIGHUP go to the app. */
    sigset_t all;
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, NULL);
    for (;;) {
        struct timespec tick = { 1, 0 };
        siginfo_t si;
        int sig = sigtimedwait(&all, &si, &tick);
        if (sig == SIGTERM || sig == SIGINT || sig == SIGHUP) kill(app, sig);
        int st;
        pid_t w;
        while ((w = waitpid(-1, &st, WNOHANG)) > 0)
            if (w == app) _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0));
        if (w < 0 && errno == ECHILD) _exit(0);
    }
}

/* ---- seccomp-bpf: allowlist ---- */

#if defined(__x86_64__)
#  define SB_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#  define SB_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#  define SB_AUDIT_ARCH 0
#endif

#define NR(x) (int)(x)

/* Syscalls common to apps and helpers. Everything not here (nor in the lists
 * below) returns EPERM. The dangerous ones (ptrace, mount, kexec, bpf, perf_event_open, keyctl,
 * add_key, init_module, execve…) are left out by definition. */
static const int allow[] = {
    NR(SYS_read), NR(SYS_write), NR(SYS_readv), NR(SYS_writev), NR(SYS_pread64), NR(SYS_pwrite64),
    NR(SYS_close), NR(SYS_lseek), NR(SYS_fstat),
#ifdef SYS_stat
    NR(SYS_stat), NR(SYS_lstat),
#endif
    NR(SYS_newfstatat), NR(SYS_statx), NR(SYS_fstatfs), NR(SYS_statfs),
    NR(SYS_openat), NR(SYS_openat2), NR(SYS_getdents64), NR(SYS_readlinkat), NR(SYS_faccessat),
#ifdef SYS_access
    NR(SYS_access), NR(SYS_readlink), NR(SYS_open),          /* x86-64 only */
#endif
    NR(SYS_faccessat2), NR(SYS_fcntl), NR(SYS_dup), NR(SYS_dup3),
#ifdef SYS_dup2
    NR(SYS_dup2), NR(SYS_pipe), NR(SYS_poll), NR(SYS_epoll_create), NR(SYS_eventfd),    /* x86-64 only */
#endif
    NR(SYS_pipe2), NR(SYS_ppoll), NR(SYS_pselect6), NR(SYS_epoll_create1), NR(SYS_epoll_ctl), NR(SYS_epoll_pwait), NR(SYS_eventfd2),
    NR(SYS_mmap), NR(SYS_munmap), NR(SYS_mremap), NR(SYS_mprotect), NR(SYS_madvise), NR(SYS_brk),
    NR(SYS_rt_sigaction), NR(SYS_rt_sigprocmask), NR(SYS_rt_sigreturn), NR(SYS_sigaltstack),
    NR(SYS_nanosleep), NR(SYS_clock_nanosleep), NR(SYS_clock_gettime), NR(SYS_clock_getres), NR(SYS_gettimeofday),
    NR(SYS_getpid), NR(SYS_gettid), NR(SYS_getppid), NR(SYS_getuid), NR(SYS_geteuid), NR(SYS_getgid), NR(SYS_getegid),
    NR(SYS_getrandom), NR(SYS_getrusage), NR(SYS_sysinfo), NR(SYS_uname), NR(SYS_sched_getaffinity), NR(SYS_sched_yield),
    NR(SYS_futex),
#ifdef SYS_futex_time64
    NR(SYS_futex_time64),
#endif
    NR(SYS_set_robust_list), NR(SYS_get_robust_list), NR(SYS_set_tid_address), NR(SYS_rseq),
    NR(SYS_exit), NR(SYS_exit_group), NR(SYS_tgkill), NR(SYS_restart_syscall),
    NR(SYS_memfd_create), NR(SYS_ftruncate), NR(SYS_fsync), NR(SYS_fdatasync),
    NR(SYS_mkdirat), NR(SYS_unlinkat), NR(SYS_renameat2), NR(SYS_renameat), NR(SYS_fchmod), NR(SYS_fchmodat), NR(SYS_utimensat),
    NR(SYS_symlinkat), NR(SYS_linkat), NR(SYS_getcwd), NR(SYS_umask),
#ifdef SYS_fadvise64
    NR(SYS_fadvise64),
#endif
    NR(SYS_recvmsg), NR(SYS_sendmsg), NR(SYS_recvfrom), NR(SYS_sendto), NR(SYS_shutdown),
    NR(SYS_getsockopt), NR(SYS_setsockopt), NR(SYS_getsockname), NR(SYS_getpeername),
    NR(SYS_prctl), NR(SYS_prlimit64), NR(SYS_getrlimit),
    NR(SYS_rt_sigtimedwait), NR(SYS_wait4), NR(SYS_setpriority), NR(SYS_getpriority),
#ifdef SYS_epoll_wait
    NR(SYS_epoll_wait),      /* x86-64: exists; aarch64 only has epoll_pwait (already above) */
#endif
};

/* Only for apps (not for helpers): connect/bind (network sockets with the permission),
 * and unrestricted clone/clone3. Never socketpair. */
static const int allow_app[] = { NR(SYS_bind), NR(SYS_connect), NR(SYS_clone), NR(SYS_clone3) };

/* helper = 1: hardened helper profile. self = getpid() (for kcmp). */
static int build_filter(struct sock_filter *f, int cap, int net, const int *extra, int ioctl_classes, int helper, unsigned self) {
    int n = 0;
#define EMIT(stmt) do { if (n < cap) f[n] = (struct sock_filter)stmt; n++; } while (0)
#define RELOAD_NR() EMIT(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)))
    /* correct architecture or the process is killed */
    EMIT(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SB_AUDIT_ARCH, 1, 0));
    EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    RELOAD_NR();
    /* ioctl(2): only for decoding helpers, and only by device class
     * (type byte _IOC_TYPE = (request >> 8) & 0xff). Never a generic ioctl nor TIOCSTI. */
    if (ioctl_classes) {
        int types[4], nt = 0;
        if (ioctl_classes & SANDBOX_IOCTL_DRM) types[nt++] = 'd';      /* DRM / VA-API */
        if (ioctl_classes & SANDBOX_IOCTL_V4L2) types[nt++] = 'V';     /* V4L2 (Pi 5) */
        if (ioctl_classes & SANDBOX_IOCTL_DMABUF) types[nt++] = 'b';   /* DMA-BUF sync */
        if (ioctl_classes & SANDBOX_IOCTL_MEDIA) types[nt++] = '|';    /* media controller requests (Pi 5) */
        EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR(SYS_ioctl), 0, (unsigned char)(nt + 4)));
        EMIT(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1])));   /* request (low 32 bits) */
        EMIT(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0x0000ff00u));                             /* _IOC_TYPE << 8 */
        for (int i = 0; i < nt; i++)
            EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)(types[i] << 8), (unsigned char)(nt - i), 0));
        EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)));    /* type not allowed */
        EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));                                 /* type allowed */
        RELOAD_NR();
    }
    if (helper) {
        /* clone: threads only ([0]ld flags [1]and [2]jeq [3]EPERM [4]ALLOW) */
        EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR(SYS_clone), 0, 5));
        EMIT(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])));
        EMIT(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, (unsigned)CLONE_THREAD));
        EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)CLONE_THREAD, 1, 0));
        EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)));
        EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
        RELOAD_NR();
        /* clone3: ENOSYS, so glibc falls back to clone (which can be filtered by flags) */
        EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR(SYS_clone3), 0, 1));
        EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA)));
#ifdef SYS_kcmp
        /* kcmp: only if the helper asks for it, and only on itself (pid1 == pid2 == getpid()).
         * Mesa uses it (os_same_file_description) to tell whether two DRM fds are the same. */
        int want_kcmp = 0;
        for (const int *e = extra; e && *e >= 0; e++) if (*e == NR(SYS_kcmp)) want_kcmp = 1;
        if (want_kcmp) {
            EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR(SYS_kcmp), 0, 6));
            EMIT(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])));
            EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, self, 0, 2));
            EMIT(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1])));
            EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, self, 1, 0));
            EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)));
            EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
            RELOAD_NR();
        }
#endif
        /* socket, socketpair, connect, bind, listen, accept, execve and execveat: none is in
         * the lists, so they fall into EPERM */
    } else {
        /* apps' socket(2): network only (with the permission). Not AF_UNIX either: Wayland and the broker
         * are already connected before the sandbox, and a new unix socket could reach the
         * session bus or other named sockets (Landlock ≤ 6 doesn't filter unix connect).
         * [0]ld domain [1]jeq inet [2]jeq inet6 [3]EACCES [4]ALLOW */
        EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR(SYS_socket), 0, 5));
        EMIT(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])));
        EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET, net ? 2 : 1, 0));
        EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET6, net ? 1 : 0, 0));
        EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EACCES & SECCOMP_RET_DATA)));
        EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
        RELOAD_NR();
    }
    /* allowlist */
    for (size_t i = 0; i < sizeof allow / sizeof allow[0]; i++) {
        EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)allow[i], 0, 1));
        EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    }
    if (!helper) for (size_t i = 0; i < sizeof allow_app / sizeof allow_app[0]; i++) {
        EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)allow_app[i], 0, 1));
        EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    }
    for (const int *e = extra; e && *e >= 0; e++) {
#ifdef SYS_kcmp
        if (helper && *e == NR(SYS_kcmp)) continue;      /* already filtered above */
#endif
        /* the ones the profile forbids are never added this way */
        if (helper && (*e == NR(SYS_clone) || *e == NR(SYS_clone3) || *e == NR(SYS_socket) || *e == NR(SYS_connect) ||
                       *e == NR(SYS_bind) || *e == NR(SYS_execve) || *e == NR(SYS_execveat) || *e == NR(SYS_ioctl))) continue;
        EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)*e, 0, 1));
        EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    }
    /* the rest: EPERM (doesn't kill, so a libc probing a new syscall doesn't die) */
    EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)));
#undef RELOAD_NR
#undef EMIT
    return n;
}

/* Applies the filter to all threads (TSYNC). Returns 0 on success. */
static int apply_seccomp_extra(int net, const int *extra, int ioctl_classes, int helper) {
    if (!SB_AUDIT_ARCH) { fprintf(stderr, "sandbox: seccomp not supported on this architecture\n"); return -1; }
    static struct sock_filter filt[1024];
    int n = build_filter(filt, (int)(sizeof filt / sizeof filt[0]), net, extra, ioctl_classes, helper, (unsigned)getpid());
    if (n > (int)(sizeof filt / sizeof filt[0])) { fprintf(stderr, "sandbox: seccomp filter too large (%d)\n", n); return -1; }
    struct sock_fprog prog = { .len = (unsigned short)n, .filter = filt };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return -1;
    long r = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &prog);
    if (r == 0) return 0;
    fprintf(stderr, "sandbox: seccomp with TSYNC failed (%s)\n", r > 0 ? "a thread could not be synchronized" : strerror(errno));
    if (helper) return -1;                     /* the helper fails closed (see sandbox_apply_helper) */
    /* app: when applied it still has a single thread; retried without TSYNC */
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog)) { fprintf(stderr, "sandbox: could not apply seccomp: %s\n", strerror(errno)); return -1; }
    return 0;
}

int sandbox_apply(const SandboxConfig *cfg) {
    int mask = 0;
    char scope[80];
    sandbox_scope_name(cfg->id, scope, sizeof scope);
    CgroupLimits lim = { .mem_high_mb = 192, .mem_max_mb = 512, .cpu_percent = 100 };
    if (!cgroup_enter(scope, &lim)) mask |= SB_CGROUP;

    /* no_new_privs and dropping capabilities before seccomp (capset is not on the allowlist) */
    if (!apply_nopriv()) mask |= SB_NOPRIV;
    if (!drop_caps()) mask |= SB_NOCAPS;

    int fs_ok = 0, net_ok = 0;
    if (!apply_landlock(cfg, NULL, NULL, &fs_ok, &net_ok)) { if (fs_ok) mask |= SB_LANDLOCK_FS; if (net_ok) mask |= SB_LANDLOCK_NET; }

    /* seccomp last: once in place, no more folders are opened for Landlock */
    if (!apply_seccomp_extra(cfg->network, NULL, 0, 0)) mask |= SB_SECCOMP;

    fprintf(stderr, "sandbox[%s]: cgroup=%d landlock-fs=%d landlock-net=%d seccomp=%d nopriv=%d nocaps=%d net=%s\n",
            cfg->id, !!(mask & SB_CGROUP), !!(mask & SB_LANDLOCK_FS), !!(mask & SB_LANDLOCK_NET),
            !!(mask & SB_SECCOMP), !!(mask & SB_NOPRIV), !!(mask & SB_NOCAPS), cfg->network ? "yes" : "no");
    return mask;
}

int sandbox_apply_helper(const SandboxHelperCfg *cfg) {
    int mask = 0;
    if (cfg->name) {
        char scope[96];
        snprintf(scope, sizeof scope, "%.80s.scope", cfg->name);
        CgroupLimits lim = { .mem_high_mb = cfg->mem_max_mb ? cfg->mem_max_mb * 3 / 4 : 0, .mem_max_mb = cfg->mem_max_mb, .cpu_percent = 0 };
        if (!cgroup_enter(scope, &lim)) mask |= SB_CGROUP;
    }
    if (!apply_nopriv()) mask |= SB_NOPRIV;
    if (!drop_caps()) mask |= SB_NOCAPS;
    /* no network; read-only access to the declared folders, plus the folders that arrive as fds */
    SandboxConfig ro = { .id = cfg->name ? cfg->name : "helper", .pkg_dir = NULL, .data_dir = NULL,
                         .runtime_dir = NULL, .ro_extra = cfg->ro_paths, .dev_files = cfg->dev_files, .network = 0 };
    int fs_ok = 0, net_ok = 0;
    if (!apply_landlock(&ro, cfg->ro_dir_fds, cfg->rw_dir_fds, &fs_ok, &net_ok)) { if (fs_ok) mask |= SB_LANDLOCK_FS; if (net_ok) mask |= SB_LANDLOCK_NET; }
    if (apply_seccomp_extra(0, cfg->extra_syscalls, cfg->allow_ioctl_classes, 1)) {
        /* fails closed: a helper decoding foreign data doesn't run without seccomp */
        fprintf(stderr, "sandbox-helper[%s]: no seccomp; the helper exits\n", ro.id);
        _exit(70);
    }
    mask |= SB_SECCOMP;
    fprintf(stderr, "sandbox-helper[%s]: cgroup=%d landlock-fs=%d seccomp=1(tsync) nopriv=%d nocaps=%d (no network or new sockets, no exec, threads only)\n",
            ro.id, !!(mask & SB_CGROUP), !!(mask & SB_LANDLOCK_FS), !!(mask & SB_NOPRIV), !!(mask & SB_NOCAPS));
    return mask;
}
