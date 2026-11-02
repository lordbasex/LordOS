/* xwayland.c — Xwayland on demand: X11 apps start it by connecting (see xwayland.h).
 *
 * axia-server reserves an X display for the session (the lock file /tmp/.X<n>-lock and the socket
 * /tmp/.X11-unix/X<n>, mode 0600: only this user's programs can connect; no abstract socket, no
 * TCP) and sets DISPLAY=:<n>. Nothing runs until an X11 app connects. Then, and only then, it
 * starts /usr/bin/Xwayland with a fixed argument list:
 *
 *   Xwayland :<n> -rootless -listenfd 3 -wm 5 -nolisten tcp -extension XTEST
 *
 * fd 3 is the listening socket (Xwayland accepts that first connection and the next ones), fd 4 is
 * its Wayland connection (WAYLAND_SOCKET, a socketpair: axia-server knows which client is Xwayland)
 * and fd 5 is the window manager's connection (xwm.c). XTEST (synthetic input) is off: an X11 app
 * can't type into another one. Xwayland runs in its own scope (lordos-xwayland.scope) and its own
 * network namespace (no network at all), with no_new_privs, a minimal environment and its log in
 * ~/.cache/axia/xwayland.log. 10 s after the last X11 app closes, the window manager stops it
 * (xwl_idle); the socket stays in axia-server, so the next X11 app starts it again. */
#include "xwayland.h"
#include "xwm.h"
#include "x11wire.h"
#include "wl.h"
#include "../sec/cgroup.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

#define XWAYLAND_BIN "/usr/bin/Xwayland"

struct wl_display *wl__display(void);

static struct {
    int disp, lfd;
    char display[16], sock[64], lock[64];
    struct wl_event_loop *loop;
    struct wl_event_source *lev;        /* the X socket, watched while Xwayland isn't running */
    pid_t pid;
    int pidfd;
    struct wl_event_source *pev;
    struct wl_client *client;
    struct wl_listener gone;
    time_t started, disabled_until;
    int quick_fails;
} XW = { .disp = -1, .lfd = -1, .pidfd = -1 };

static int on_connect(int fd, uint32_t mask, void *data);

/* Is display n in use? A lock file with a live process, or a socket nobody's lock explains. A
 * stale lock and socket of ours (a previous session that crashed) are removed. */
static int display_taken(int n, void *ctx) {
    (void)ctx;
    char lock[64], sock[64];
    snprintf(lock, sizeof lock, "/tmp/.X%d-lock", n);
    snprintf(sock, sizeof sock, "/tmp/.X11-unix/X%d", n);
    struct stat st;
    if (!lstat(lock, &st)) {
        char buf[16] = "";
        int fd = open(lock, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0) { if (read(fd, buf, sizeof buf - 1) < 0) buf[0] = 0; close(fd); }
        long pid = strtol(buf, NULL, 10);
        if (pid > 0 && (kill((pid_t)pid, 0) == 0 || errno == EPERM)) return 1;
        if (st.st_uid != getuid() || !S_ISREG(st.st_mode)) return 1;      /* someone else's: not ours to clean */
        unlink(lock);
        if (!lstat(sock, &st) && st.st_uid == getuid() && S_ISSOCK(st.st_mode)) unlink(sock);
    }
    return !lstat(sock, &st);
}

static void drain(int fd) {
    for (int i = 0; i < 64; i++) {
        int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (c < 0) break;
        close(c);
    }
}

static void watch_socket(int on) {
    if (on && !XW.lev && XW.lfd >= 0) XW.lev = wl_event_loop_add_fd(XW.loop, XW.lfd, WL_EVENT_READABLE, on_connect, NULL);
    else if (!on && XW.lev) { wl_event_source_remove(XW.lev); XW.lev = NULL; }
}

int xwl_init(void) {
    struct wl_display *dpy = wl__display();
    if (!dpy || XW.lfd >= 0) return -1;
    struct stat st;
    if (stat(XWAYLAND_BIN, &st) || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "xwayland: not installed (the xwayland package): X11 apps can't open\n");
        return -1;
    }
    XW.loop = wl_display_get_event_loop(dpy);
    if (lstat("/tmp/.X11-unix", &st)) {
        if (mkdir("/tmp/.X11-unix", 01777) == 0) chmod("/tmp/.X11-unix", 01777);
    } else if (!S_ISDIR(st.st_mode) || (st.st_uid != 0 && st.st_uid != getuid())) {
        fprintf(stderr, "xwayland: /tmp/.X11-unix isn't a directory of root's: no X11 apps\n");
        return -1;
    }
    for (int tries = 0; tries < 8; tries++) {
        int n = x11_pick_display(display_taken, NULL);
        if (n < 0) break;
        snprintf(XW.lock, sizeof XW.lock, "/tmp/.X%d-lock", n);
        snprintf(XW.sock, sizeof XW.sock, "/tmp/.X11-unix/X%d", n);
        int lf = open(XW.lock, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0444);
        if (lf < 0) continue;                               /* someone took it meanwhile */
        char pid[16];
        int len = snprintf(pid, sizeof pid, "%10d\n", (int)getpid());
        if (write(lf, pid, (size_t)len) != len) { close(lf); unlink(XW.lock); continue; }
        close(lf);
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        snprintf(a.sun_path, sizeof a.sun_path, "%s", XW.sock);
        mode_t old = umask(0177);                           /* 0600: only this user can connect */
        int ok = fd >= 0 && !bind(fd, (struct sockaddr *)&a, sizeof a);
        umask(old);
        if (ok) ok = !chmod(XW.sock, 0600) && !listen(fd, 64);
        if (!ok) { if (fd >= 0) close(fd); unlink(XW.lock); continue; }
        XW.lfd = fd;
        XW.disp = n;
        snprintf(XW.display, sizeof XW.display, ":%d", n);
        break;
    }
    if (XW.lfd < 0) { fprintf(stderr, "xwayland: no free X display\n"); return -1; }
    setenv("DISPLAY", XW.display, 1);               /* inherited by the apps LordOS starts */
    /* Java (AWT/Swing) guesses frame insets for a reparenting window manager unless told: ours
     * doesn't reparent (the theme draws the frame), so without this its content is shifted and blank */
    setenv("_JAVA_AWT_WM_NONREPARENTING", "1", 1);
    watch_socket(1);
    fprintf(stderr, "xwayland: DISPLAY=%s (%s), started on the first X11 connection\n", XW.display, XW.sock);
    return 0;
}

static void child_exited(void) {
    int st = 0;
    if (XW.pid > 0 && waitpid(XW.pid, &st, WNOHANG) <= 0) return;      /* not yet */
    if (XW.pev) { wl_event_source_remove(XW.pev); XW.pev = NULL; }
    if (XW.pidfd >= 0) { close(XW.pidfd); XW.pidfd = -1; }
    int quick = time(NULL) - XW.started < 4 && !(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    fprintf(stderr, "xwayland: exited (%s %d)\n", WIFSIGNALED(st) ? "signal" : "status", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
    XW.pid = 0;
    xwm_stop();
    if (XW.client) { struct wl_client *c = XW.client; XW.client = NULL; wlx_set_client(NULL); wl_list_remove(&XW.gone.link); wl_client_destroy(c); }
    XW.quick_fails = quick ? XW.quick_fails + 1 : 0;
    if (XW.quick_fails >= 3) {
        XW.disabled_until = time(NULL) + 60;
        XW.quick_fails = 0;
        fprintf(stderr, "xwayland: it failed to start three times; X11 apps are refused for a minute\n");
    }
    watch_socket(1);
}

static int on_pidfd(int fd, uint32_t mask, void *data) {
    (void)fd; (void)mask; (void)data;
    child_exited();
    return 0;
}

static void client_gone(struct wl_listener *l, void *data) {
    (void)l; (void)data;
    XW.client = NULL;
    wlx_set_client(NULL);
    wl_list_init(&XW.gone.link);
    xwm_stop();
    if (XW.pid > 0) kill(XW.pid, SIGTERM);                  /* its Wayland connection is gone: so is it */
}

void xwl_wm_lost(void) { if (XW.pid > 0) kill(XW.pid, SIGTERM); }

void xwl_idle(void) {
    if (XW.pid <= 0) return;
    fprintf(stderr, "xwayland: no X11 app for 10 s: stopping it (the next one starts it again)\n");
    XW.started = 0;                                         /* a normal stop, not a failed start */
    kill(XW.pid, SIGTERM);
}

/* The binary: a regular file owned by root and writable by nobody else. */
static int binary_ok(void) {
    struct stat st;
    return !stat(XWAYLAND_BIN, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022) && (st.st_mode & 0111);
}

static int open_log(void) {
    const char *home = getenv("HOME");
    if (!home) return -1;
    char p[512];
    snprintf(p, sizeof p, "%s/.cache/axia/xwayland.log", home);
    struct stat st;
    int trunc = !stat(p, &st) && st.st_size > (1 << 20);
    return open(p, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC | (trunc ? O_TRUNC : 0), 0600);
}

static void spawn(void) {
    if (!binary_ok()) {
        fprintf(stderr, "xwayland: %s is missing or not root's: not started\n", XWAYLAND_BIN);
        XW.disabled_until = time(NULL) + 60;
        drain(XW.lfd);
        return;
    }
    int wlp[2] = { -1, -1 }, wmp[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wlp) || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wmp)) {
        for (int i = 0; i < 2; i++) { if (wlp[i] >= 0) close(wlp[i]); if (wmp[i] >= 0) close(wmp[i]); }
        drain(XW.lfd);
        return;
    }
    int logfd = open_log();
    char disp[16];
    snprintf(disp, sizeof disp, ":%d", XW.disp);
    /* everything the child needs, prepared before fork (it only calls async-signal-safe functions
     * and the scope request) */
    char env_home[600], env_rt[600];
    snprintf(env_home, sizeof env_home, "HOME=%s", getenv("HOME") ? getenv("HOME") : "/");
    snprintf(env_rt, sizeof env_rt, "XDG_RUNTIME_DIR=%s", getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp");
    char *envp[] = { env_home, env_rt, "PATH=/usr/bin:/bin", "WAYLAND_SOCKET=4", "LANG=C.UTF-8", NULL };
    char *argv[] = { "Xwayland", disp, "-rootless", "-listenfd", "3", "-wm", "5",
                     "-nolisten", "tcp", "-extension", "XTEST", NULL };
    int managed = getenv("LORDOS_SESSIOND") != NULL;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        for (int s = 1; s < 32; s++) signal(s, SIG_DFL);
        /* fds 3, 4, 5 (moved high first so none overwrites another) */
        int a = fcntl(XW.lfd, F_DUPFD_CLOEXEC, 100), b = fcntl(wlp[1], F_DUPFD_CLOEXEC, 100), c = fcntl(wmp[1], F_DUPFD_CLOEXEC, 100);
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (a < 0 || b < 0 || c < 0 || null < 0) _exit(126);
        dup2(null, 0);
        dup2(logfd >= 0 ? logfd : null, 1);
        dup2(logfd >= 0 ? logfd : null, 2);
        if (dup2(a, 3) != 3 || dup2(b, 4) != 4 || dup2(c, 5) != 5) _exit(126);
        syscall(SYS_close_range, 6, ~0u, 0);
        /* its own scope: never axia-server's (the firewall and the Monitor know it apart) */
        CgroupLimits lim = { 0, 0, 0 };
        if (cgroup_enter("lordos-xwayland.scope", &lim) && managed) {
            static const char m[] = "xwayland: could not enter lordos-xwayland.scope: not started\n";
            if (write(2, m, sizeof m - 1) < 0) {}
            _exit(126);
        }
        /* no network at all: its own (empty) network namespace, inside a user namespace mapping
         * only this user (Xwayland never needs root or the network) */
        uid_t uid = getuid();
        gid_t gid = getgid();
        /* axia-server dropped root, so it isn't dumpable and /proc/self is root's: the maps below
         * couldn't be written (Xwayland would run as the overflow uid and fail to start xkbcomp) */
        prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
        if (!unshare(CLONE_NEWUSER | CLONE_NEWNET)) {
            char map[64];
            int f = open("/proc/self/setgroups", O_WRONLY | O_CLOEXEC);
            if (f >= 0) { if (write(f, "deny", 4) < 0) {} close(f); }
            int len = snprintf(map, sizeof map, "%u %u 1", (unsigned)uid, (unsigned)uid);
            f = open("/proc/self/uid_map", O_WRONLY | O_CLOEXEC);
            if (f >= 0) { if (write(f, map, (size_t)len) < 0) {} close(f); }
            len = snprintf(map, sizeof map, "%u %u 1", (unsigned)gid, (unsigned)gid);
            f = open("/proc/self/gid_map", O_WRONLY | O_CLOEXEC);
            if (f >= 0) { if (write(f, map, (size_t)len) < 0) {} close(f); }
        }
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        execve(XWAYLAND_BIN, argv, envp);
        _exit(127);
    }
    close(wlp[1]);
    close(wmp[1]);
    if (logfd >= 0) close(logfd);
    if (pid < 0) { close(wlp[0]); close(wmp[0]); drain(XW.lfd); return; }
    XW.pid = pid;
    XW.started = time(NULL);
    watch_socket(0);                                        /* Xwayland accepts from now on */
    XW.client = wl_client_create(wl__display(), wlp[0]);
    if (!XW.client) { close(wlp[0]); close(wmp[0]); kill(pid, SIGTERM); }
    else {
        XW.gone.notify = client_gone;
        wl_client_add_destroy_listener(XW.client, &XW.gone);
        wlx_set_client(XW.client);
        if (xwm_start(wmp[0], XW.loop)) kill(pid, SIGTERM);
    }
    XW.pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (XW.pidfd >= 0) XW.pev = wl_event_loop_add_fd(XW.loop, XW.pidfd, WL_EVENT_READABLE, on_pidfd, NULL);
    fprintf(stderr, "xwayland: started (pid %d) for an X11 app on %s\n", (int)pid, XW.display);
}

static int on_connect(int fd, uint32_t mask, void *data) {
    (void)mask; (void)data;
    if (XW.pid > 0) { watch_socket(0); return 0; }
    if (time(NULL) < XW.disabled_until) { drain(fd); return 0; }
    spawn();
    return 0;
}

void xwl_stop(void) {
    watch_socket(0);
    if (XW.pid > 0) {
        kill(XW.pid, SIGTERM);
        for (int i = 0; i < 20 && waitpid(XW.pid, NULL, WNOHANG) == 0; i++) usleep(10000);
        XW.pid = 0;
    }
    if (XW.pev) { wl_event_source_remove(XW.pev); XW.pev = NULL; }
    if (XW.pidfd >= 0) { close(XW.pidfd); XW.pidfd = -1; }
    xwm_stop();
    if (XW.lfd >= 0) {
        close(XW.lfd);
        XW.lfd = -1;
        unlink(XW.sock);
        unlink(XW.lock);
    }
    XW.display[0] = 0;
}

const char *xwl_display(void) { return XW.display; }
const char *xwl_socket_path(void) { return XW.lfd >= 0 ? XW.sock : ""; }
int xwl_running(void) { return XW.pid > 0; }
