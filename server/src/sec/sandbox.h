/* sandbox.h — our own sandbox for native .lpk apps (see sandbox.c and docs/PRIVACY.md).
 *
 * Our own code: no bubblewrap, flatpak or firejail. Uses kernel 6.12 features directly:
 *   - Landlock (ABI up to 6): read-only access to the package folder, the runtime and the fonts;
 *     read and write of ~/.local/share/lordos/appdata/<id>; nothing else from $HOME. TCP
 *     rules and abstract unix socket scoping.
 *   - seccomp-bpf: a syscall allowlist; no ptrace, mount, kexec, bpf, perf, keyctl or
 *     module loading. Without the `network` permission, it also blocks network sockets.
 *   - no_new_privs and all capabilities dropped.
 *   - its own cgroup v2 (cgroup.c) with bounded memory and CPU.
 * If something is missing in the kernel, it warns in the log and goes on without that part (it doesn't keep the app from
 * starting). Applied in the app process (lord-server --app <id>), after dropping
 * privileges and before loading the app's code. */
#ifndef LORD_SEC_SANDBOX_H
#define LORD_SEC_SANDBOX_H

#include <stddef.h>

typedef struct {
    const char *id;             /* package id (already validated) */
    const char *pkg_dir;        /* ~/.local/share/lordos/apps/<id>  (read-only) */
    const char *data_dir;       /* ~/.local/share/lordos/appdata/<id> (read and write) */
    const char *runtime_dir;    /* XDG_RUNTIME_DIR (for the Wayland socket; just what's needed) */
    const char *const *ro_extra;/* extra read-only folders (fonts, assets); NULL-terminated */
    const char *const *dev_files;/* exact device files opened for read and write; NULL-terminated */
    int network;                /* 1 if the app declared and has the `network` permission */
} SandboxConfig;

/* Applies everything possible. Returns a mask of what ended up active (SB_*). Never fails in
 * a way that prevents startup: whatever couldn't be done is skipped with a warning. */
enum { SB_CGROUP = 1, SB_LANDLOCK_FS = 2, SB_LANDLOCK_NET = 4, SB_SECCOMP = 8, SB_NOPRIV = 16, SB_NOCAPS = 32 };
int sandbox_apply(const SandboxConfig *cfg);

/* cgroup scope name of a .lpk app: "app-lordos-lpk-<id>.scope". */
void sandbox_scope_name(const char *id, char *out, size_t n);

/* ---- variant for decoding helpers (lord-media, lord-doc) ----
 * A helper that decodes untrusted data (video, PDF, archives)
 * needs neither network nor filesystem access: it gets fds already open and writes to
 * another fd. This variant applies: no network, no files except the read-only folders
 * passed to it (fonts, the libraries the lib loads at startup), the syscall allowlist
 * plus the ones the helper declares, a memory cap, no_new_privs and no capabilities. Same
 * sandbox as .lpk apps, without forks. */
/* ioctl classes a helper may need for hardware decoding. Filtered by the
 * type byte of the ioctl number (_IOC_TYPE): only those types; never a generic ioctl nor
 * TIOCSTI. The helper can NOT open /dev (Landlock): it gets the device fds already
 * opened by lord-server. */
enum { SANDBOX_IOCTL_DRM = 1, SANDBOX_IOCTL_V4L2 = 2, SANDBOX_IOCTL_DMABUF = 4, SANDBOX_IOCTL_MEDIA = 8 };

typedef struct {
    const char *name;               /* cgroup scope name (e.g. "lord-media") */
    const char *const *ro_paths;    /* read-only folders (fonts, /usr/lib…); NULL-terminated */
    const int *extra_syscalls;      /* extra allowed syscalls; -1-terminated */
    int  allow_ioctl_classes;       /* OR of SANDBOX_IOCTL_* (hardware decoding) */
    long mem_max_mb;                /* memory cap (0 = no cap) */
    /* folders already opened by lord-server (directory fds), -1-terminated; NULL = none.
     * ro: read; rw: read + write/create files, folders and symlinks and delete
     * files (no truncation). That way the helper needs no path at all. */
    const int *ro_dir_fds;
    const int *rw_dir_fds;
    /* exact device files a decoder library opens by path itself: the Pi 5's HEVC decoder only */
    const char *const *dev_files;
} SandboxHelperCfg;

/* Callers can detect the ro_dir_fds/rw_dir_fds fields. */
#define SANDBOX_HELPER_DIR_FDS 1

/* Hardened compared to apps: no socket/socketpair/connect/bind/listen/accept (the channel
 * is inherited; sendmsg/recvmsg still work), clone only with CLONE_THREAD, clone3 → ENOSYS (glibc
 * falls back to clone), no execve/execveat, and the filter applies to ALL threads (TSYNC). If seccomp
 * can't be applied, the process exits (fails closed). kcmp, if requested in extra_syscalls,
 * only works for comparing fds of the process itself (pid1 == pid2 == getpid()). */
int sandbox_apply_helper(const SandboxHelperCfg *cfg);

/* In a child right before execve of a system program that reads a server's bytes or a secret: only the system's folders,
 * read-only, nothing to write but /dev/null; network 0 also means no TCP. Best effort on a kernel with no Landlock. */
void sandbox_exec_child(int network);

/* ---- a packaged app's companion (app/companion.c, docs/GO-LINK.md 3.3) ----
 * Right before execveat of the verified binary, in a process with one thread. Go can't apply this from inside, so it
 * goes on before exec and stays on. Files: read in the home folder, write in ~/Downloads, execute only that binary,
 * nothing else; with network, read the few system files that needs. Syscalls: its own allowlist. No new AF_UNIX
 * socket, ever: Landlock doesn't filter connect on named sockets, and with one it could reach the session bus. No
 * child processes, threads only. No ioctl, no chmod, chown or utimes (Landlock doesn't cover those, and they would
 * reach the whole home folder). The exec left in the filter names this descriptor, and Landlock lets nothing else
 * run; with no memfd_create there is no file outside Landlock's reach to run. Unlike every other sandbox here,
 * this one fails closed as a whole: -1 means don't exec. */
typedef struct {
    const char *home;
    int downloads_fd;       /* ~/Downloads, O_PATH, opened with no link followed */
    int app_fd;             /* the package's folder */
    int bin_fd;             /* the binary: the descriptor that was hashed */
    int network;            /* AF_INET, AF_INET6, AF_NETLINK's route family: the manifest's `network`, allowed */
} SandboxCompanionCfg;
int sandbox_exec_companion(const SandboxCompanionCfg *cfg);

/* ---- "no access to your files" for Linux apps (the launcher, apps.c) ----
 * Landlock only: no seccomp, no capability changes, no network or scoping rules, so a normal
 * GTK/Qt app or browser keeps working; device ioctls are not handled (the device restrictions are
 * lord-netperm's). What the app may touch:
 *   read and execute: the system (/usr, /etc, /opt, /var/lib, /var/cache, /sys, a few service
 *   folders of /run), plus ro[];
 *   read and write: /tmp, /var/tmp, /dev, /proc, the runtime dir, plus rw[] (its own config, cache and
 *   data dirs, and the file or folder the user opened it with);
 * everything else under the home folder and the user's mounts is denied. Applied in the
 * launcher's child, after it entered its scope and right before execve; it also sets
 * no_new_privs (Landlock requires it), so setuid programs started by the app don't gain
 * privileges. Doesn't allocate (safe after fork). 0 applied, -1 not (kernel without Landlock:
 * the caller decides). */
#define SANDBOX_FILES_MAX 48
typedef struct {
    const char *home;
    const char *runtime_dir;
    const char *rw[SANDBOX_FILES_MAX + 1];     /* NULL-terminated */
    const char *ro[SANDBOX_FILES_MAX + 1];
    const char *sockets[8];                    /* names inside runtime_dir kept visible (wayland-0...), NULL-terminated */
    const char *cwd;                           /* the .desktop's Path= (or NULL: the home folder), entered inside the view */
    /* the file or folder the app was opened with ("Open with"; also in rw[] or ro[]): opened
     * again without following any symlink and only if it is still the same inode the launcher checked */
    const char *opened;
    unsigned long long opened_dev, opened_ino;
    /* 1: the view without the home restriction (no-mic, no-camera, no-usb, no-bluetooth): the home
     * folder stays as it is and rw[]/ro[] are unused; everything else (runtime dir with only
     * sockets[], private /tmp, /var/tmp, /dev/shm, devpts, /proc, PID and IPC namespaces) is the
     * same. Then sandbox_apply_scope instead of sandbox_apply_files. */
    int keep_home;
} SandboxFilesCfg;

/* Before sandbox_apply_files: the app's own view of the system, so no unix socket of the session
 * can be reached (Landlock in kernel 6.12 doesn't cover connect() on unix sockets, and the session
 * bus or a terminal multiplexer would run anything outside the restriction). In a new user, mount
 * and PID namespace (unprivileged; uid and gid map to themselves):
 *   - the home folder is an empty tmpfs with only rw[] and ro[] bound into it (ro[] read-only);
 *   - the runtime dir is an empty tmpfs with only sockets[] bound into it: no session bus, no
 *     systemd user manager, no agents, no LordOS sockets;
 *   - /tmp, /var/tmp and /dev/shm are private tmpfs (no other program's sockets or shared memory),
 *     with its own IPC namespace, and /dev/pts is a new devpts instance (not the session's terminals);
 *   - the working directory is entered again inside the view (cwd, else the home folder): a
 *     directory opened before the tmpfs went on would still reach what they hide;
 *   - /proc is a new procfs of the new PID namespace (no /proc/<pid>/root of the session's
 *     processes back into the real one).
 * The calling process forks: the parent _exits (the caller already is an orphan), the child is
 * PID 1 of the new namespace and only reaps; its child returns 0 and goes on to Landlock and
 * execve. -1 (and nothing launched) if the kernel refuses any step: the caller doesn't start the
 * app. Doesn't allocate. */
int sandbox_files_isolate(const SandboxFilesCfg *cfg);
int sandbox_apply_files(const SandboxFilesCfg *cfg);
/* For keep_home views: Landlock with no filesystem rules, only its scoping (ABI 6): no abstract
 * unix socket and no signal outside the app (the view has no network namespace, so abstract
 * sockets are shared with the session). Sets no_new_privs. 0 applied (or a kernel without ABI 6:
 * logged, nothing else to do), -1 failed. */
int sandbox_apply_scope(void);
/* Landlock ABI of the running kernel (0 = none). */
int sandbox_landlock_abi(void);

#endif
