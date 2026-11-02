/* appspawn.c — lord-server launches an .lpk app as its own isolated process
 * (`lord-server --app <id>`), the third narrow exception to "the server does not run commands"
 * (see AGENTS.md). The only argument is a VALIDATED id of an installed package, never caller
 * arguments. The broker socket (socketpair) is created here and handed to the child; the server
 * end is registered in brokersrv.c together with the app process's real pid. */
#include "app.h"
#include "../lpk.h"
#include "../sec/broker.h"
#include "../sec/pkgapp.h"
#include "../ui/js_sys.h"
#include "../vault/vclient.h"
#include "apphost_bc.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int app_package_dir(const char *id, const char *assets, char *out, size_t n) {
    if (!lpk_valid_id(id)) return -1;
    struct stat st;
    if (assets && assets[0]) {
        char d[PATH_MAX], real[PATH_MAX], m[PATH_MAX + 16];
        if ((size_t)snprintf(d, sizeof d, "%s/sysapps/%s", assets, id) < sizeof d && realpath(d, real) &&
            !stat(real, &st) && S_ISDIR(st.st_mode) && !(st.st_mode & S_IWOTH) && (st.st_uid == 0 || st.st_uid == getuid()) &&
            (size_t)snprintf(m, sizeof m, "%s/manifest.json", real) < sizeof m && !stat(m, &st) && S_ISREG(st.st_mode) &&
            (size_t)snprintf(out, n, "%s", real) < n)
            return 1;
    }
    if (!pkgapp_dir(id, out, n)) return 2;
    const char *root = lpk_root();
    if (root && (size_t)snprintf(out, n, "%s/%s", root, id) < n && !stat(out, &st) && S_ISDIR(st.st_mode)) return 0;
    return -1;
}

/* System apps that get their own channel to lord-vault (vc_attach_app): only Passwords. */
static int wants_vault(const char *id, int system) { return system == 1 && !strcmp(id, "passwords"); }

/* 0 if the process was started (and its broker registered). The id must be an installed package. */
int app_spawn(const char *id) {
    if (!lpk_valid_id(id)) return -1;
    char dir[PATH_MAX];
    int sys = app_package_dir(id, js_sys_assets(), dir, sizeof dir);
    if (sys < 0) return -1;
    if (sys == 2 && js_sys_reserved_id(id)) return -1;    /* LordOS's own string namespaces, as for a home .lpk */
    /* one process per app: if it is already running, it opens another window instead */
    if (!brokersrv_new_window(id)) { fprintf(stderr, "lord-server: app '%s' already running: new window\n", id); return 0; }

    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return -1;
    self[n] = 0;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    /* the child's end is NOT close-on-exec: the app process inherits it */
    fcntl(sv[1], F_SETFD, 0);
    /* the intermediate child reports the app's real pid here (so the compositor can tell which
     * Wayland client is this app: wl.c routes its ❖ shortcuts to it) */
    int pp[2];
    if (pipe2(pp, O_CLOEXEC)) { close(sv[0]); close(sv[1]); return -1; }
    /* Passwords talks to lord-vault on its own channel: decrypted data never passes through here */
    int vfd = wants_vault(id, sys) ? vc_attach_app() : -1;
    int bcfd = apphost_bc_dup_ro();

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        pid_t g = fork();                    /* orphaned grandchild, like the Linux app launcher */
        if (g != 0) { if (g > 0 && write(pp[1], &g, sizeof g) != (ssize_t)sizeof g) _exit(1); _exit(0); }
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        sigset_t none;                       /* lord-server blocks SIGINT/SIGTERM (signalfd): not the app */
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        char fdstr[16];
        snprintf(fdstr, sizeof fdstr, "%d", sv[1]);
        setenv(BROKER_FD_ENV, fdstr, 1);
        if (vfd >= 0) {
            char vstr[16];
            fcntl(vfd, F_SETFD, 0);          /* inherited by the app process (LORD_VAULT_FD) */
            snprintf(vstr, sizeof vstr, "%d", vfd);
            setenv("LORD_VAULT_FD", vstr, 1);
        } else unsetenv("LORD_VAULT_FD");
        if (bcfd >= 0) {                     /* the app host as bytecode (apphost_bc.c), read-only */
            char bstr[16];
            fcntl(bcfd, F_SETFD, 0);
            snprintf(bstr, sizeof bstr, "%d", bcfd);
            setenv(APPHOST_BC_ENV, bstr, 1);
        } else unsetenv(APPHOST_BC_ENV);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); if (null > 2) close(null); }
        /* lord-server passes the id, already validated; there are no caller arguments */
        char *const argv[] = { self, (char *)"--app", (char *)id, NULL };
        unsetenv("DISPLAY");                    /* no X11 for isolated apps (their seccomp has no unix socket() either) */
        execve("/proc/self/exe", argv, environ);   /* not `self`: after a package upgrade it is "<path> (deleted)" */
        _exit(127);
    }
    close(sv[1]);
    close(pp[1]);
    if (vfd >= 0) close(vfd);
    if (bcfd >= 0) close(bcfd);
    if (pid < 0) { close(sv[0]); close(pp[0]); return -1; }
    waitpid(pid, NULL, 0);                   /* the intermediate child exits right away */
    pid_t app = -1;
    if (read(pp[0], &app, sizeof app) != (ssize_t)sizeof app || app <= 0) app = -1;
    close(pp[0]);
    if (brokersrv_add(sv[0], id, (int)app)) { close(sv[0]); return -1; }
    if (vfd >= 0) brokersrv_set_vault(sv[0]);   /* this app may also ask for the FIDO2 device */
    if (sys == 2) brokersrv_set_packaged(sv[0]);  /* may ask for its companion; the app process itself proves nothing */
    if (sys == 1) brokersrv_set_system(sv[0]);  /* a system package: its keyring owner, lord-maild (brokermail.c) */
    fprintf(stderr, "lord-server: app '%s' started as an isolated process (pid %d)\n", id, (int)app);
    return 0;
}

int app_companion_spawn(const char *id, int network, int share_fd, int *app_fd) {
    *app_fd = -1;
    int sv[2];
    if (!lpk_valid_id(id) || socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { if (share_fd >= 0) close(share_fd); return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        prctl(PR_SET_PDEATHSIG, SIGKILL);       /* it doesn't outlive lord-server */
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        int a = fcntl(sv[1], F_DUPFD, 16), b = share_fd >= 0 ? fcntl(share_fd, F_DUPFD, 16) : -1;
        if (a < 0 || dup2(a, 3) != 3) _exit(127);
        if (b >= 0) { if (dup2(b, 4) != 4) _exit(127); } else close(4);
        syscall(SYS_close_range, 5U, ~0U, 0U);  /* nothing of the compositor's follows */
        char *const argv[] = { (char *)"lord-companion", (char *)"--companion", (char *)id, (char *)(network ? "1" : "0"), NULL };
        execve("/proc/self/exe", argv, environ);
        _exit(127);
    }
    close(sv[1]);
    if (share_fd >= 0) close(share_fd);
    if (pid < 0) { close(sv[0]); return -1; }
    *app_fd = sv[0];
    return (int)pid;
}
