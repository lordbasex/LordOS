/* sandbox_test.c — applies the sandbox and checks that what is forbidden fails and normal things work.
 * Needs the kernel's Landlock and seccomp (6.12); run in the VM, not in `make test`.
 *
 *   build/sandbox-test          app profile, without network permission
 *   build/sandbox-test net      app profile, with network permission
 *   build/sandbox-test helper   helper profile (axia-media, axia-doc): no new sockets, no
 *                               connect/bind, no exec, threads only, clone3 → ENOSYS, TSYNC,
 *                               folders by fd (rw_dir_fds) and kcmp only against itself */
#include "sec/sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#ifndef KCMP_FILE
#define KCMP_FILE 0
#endif

static int fails, checks;
#define OK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); putchar('\n'); } else { printf("ok: "); printf(__VA_ARGS__); putchar('\n'); } } while (0)

static const char *home;

static int app_mode(int network) {
    char pkg[512], data[512], ssh[512], settings[512], parent[512];
    snprintf(pkg, sizeof pkg, "%s/.local/share/axia/apps/evil", home);
    snprintf(parent, sizeof parent, "%s/.local/share/axia/appdata", home);
    snprintf(data, sizeof data, "%s/.local/share/axia/appdata/evil", home);
    snprintf(ssh, sizeof ssh, "%s/.ssh", home);
    snprintf(settings, sizeof settings, "%s/.config/axia/settings.json", home);
    mkdir(parent, 0755);
    mkdir(data, 0755);
    int pre = socket(AF_UNIX, SOCK_STREAM, 0);          /* like the Wayland one: opened BEFORE */

    const char *ro[] = { "/usr/share", NULL };
    SandboxConfig cfg = { .id = "evil", .pkg_dir = pkg, .data_dir = data, .runtime_dir = getenv("XDG_RUNTIME_DIR"), .ro_extra = ro, .network = network };
    printf("mask=0x%x\n", sandbox_apply(&cfg));

    char f[600];
    snprintf(f, sizeof f, "%s/prueba.txt", data);
    int fd = open(f, O_CREAT | O_WRONLY, 0644);
    OK(fd >= 0 && write(fd, "hola", 4) == 4, "write to appdata/<id> (fd=%d)", fd);
    if (fd >= 0) close(fd);
    OK(open(ssh, O_RDONLY | O_DIRECTORY) < 0, "reading ~/.ssh fails (%s)", strerror(errno));
    OK(open(settings, O_RDONLY) < 0, "reading settings.json fails (%s)", strerror(errno));
    snprintf(f, sizeof f, "%s/fuera.txt", home);
    OK(open(f, O_CREAT | O_WRONLY, 0644) < 0, "writing to $HOME fails (%s)", strerror(errno));

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (network) OK(s >= 0, "with permission: AF_INET socket opens");
    else OK(s < 0, "without permission: AF_INET socket fails (%s)", strerror(errno));
    if (s >= 0) close(s);
    OK(socket(AF_UNIX, SOCK_STREAM, 0) < 0, "new AF_UNIX socket fails (%s): cannot reach the session bus", strerror(errno));
    int sv[2];
    OK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0, "socketpair fails (%s)", strerror(errno));
    OK(pre >= 0, "the socket opened before the sandbox still works (fd=%d)", pre);
    OK(ptrace(PTRACE_TRACEME, 0, 0, 0) < 0, "ptrace fails (%s)", strerror(errno));
    OK(ptrace(PTRACE_ATTACH, getppid(), 0, 0) < 0, "ptrace on another process (the parent) fails (%s)", strerror(errno));
    char *const av[] = { "/bin/sh", "-c", "true", NULL };
    OK(execve("/bin/sh", av, NULL) < 0, "exec of /bin/sh fails (%s)", strerror(errno));
    OK(open("/dev/dri/card0", O_RDWR) < 0, "opening a device (/dev/dri/card0) fails (%s)", strerror(errno));
    OK(open("/dev/video0", O_RDWR) < 0, "opening /dev/video0 fails (%s)", strerror(errno));
    char doc[600];
    snprintf(doc, sizeof doc, "%s/Documentos", home);
    OK(open(doc, O_RDONLY | O_DIRECTORY) < 0, "opening ~/Documentos directly fails (the powerbox passes only the fd) (%s)", strerror(errno));
    return 0;
}

/* ---- ayudante ---- */

static volatile int thread_done;
static void *thread_fn(void *a) { (void)a; thread_done = 1; return NULL; }

/* Thread created BEFORE the sandbox: tests TSYNC (the filter reaches it too). */
static volatile int early_go, early_fork_failed, early_exec_failed, early_done;
static void *early_fn(void *a) {
    (void)a;
    while (!early_go) usleep(1000);
    char *const av[] = { "/bin/true", NULL };
    early_exec_failed = execve("/bin/true", av, NULL) < 0;
    long r = syscall(SYS_clone, (unsigned long)SIGCHLD, NULL, NULL, NULL, 0UL);   /* fork() a mano */
    if (r == 0) _exit(0);
    early_fork_failed = r < 0;
    early_done = 1;
    return NULL;
}

static int helper_mode(void) {
    /* work folder passed as an fd (as axia-server does with axia-doc) */
    char work[512], outside[512];
    snprintf(work, sizeof work, "%s/.cache/lordos-helper-test", home);
    snprintf(outside, sizeof outside, "%s/.cache", home);
    mkdir(outside, 0700);
    mkdir(work, 0700);
    int wfd = open(work, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int pre = socket(AF_UNIX, SOCK_STREAM, 0);            /* unconnected, opened before */
    pthread_t early;
    pthread_create(&early, NULL, early_fn, NULL);

    int extra[] = {
#ifdef SYS_kcmp
        SYS_kcmp,
#endif
        -1 };
    int rw[] = { wfd, -1 };
    const char *ro[] = { "/usr/share/fonts", NULL };
    SandboxHelperCfg cfg = { .name = "lordos-helper-test", .ro_paths = ro, .extra_syscalls = extra,
                             .allow_ioctl_classes = 0, .mem_max_mb = 256, .ro_dir_fds = NULL, .rw_dir_fds = rw };
    printf("mask=0x%x\n", sandbox_apply_helper(&cfg));

    /* sockets */
    OK(socket(AF_UNIX, SOCK_STREAM, 0) < 0, "new socket fails (%s)", strerror(errno));
    struct sockaddr_un su = { .sun_family = AF_UNIX };
    snprintf(su.sun_path, sizeof su.sun_path, "%s/wayland-0", getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/run/user/1000");
    OK(pre >= 0 && connect(pre, (struct sockaddr *)&su, sizeof su) < 0, "connect to a named unix socket (%s) fails (%s)", su.sun_path, strerror(errno));
    OK(bind(pre, (struct sockaddr *)&su, sizeof su) < 0, "bind fails (%s)", strerror(errno));
    int sv[2];
    OK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0, "socketpair fails (%s)", strerror(errno));

    /* exec y procesos */
    char *const av[] = { "/bin/true", NULL };
    OK(execve("/bin/true", av, NULL) < 0, "execve fails (%s)", strerror(errno));
    pid_t p = fork();
    if (p == 0) _exit(0);
    OK(p < 0, "fork (clone without CLONE_THREAD) fails (%s)", strerror(errno));
    OK(syscall(SYS_clone3, NULL, 0) < 0 && errno == ENOSYS, "clone3 → ENOSYS (%s)", strerror(errno));
    pthread_t t;
    int tr = pthread_create(&t, NULL, thread_fn, NULL);
    if (!tr) pthread_join(t, NULL);
    OK(tr == 0 && thread_done, "creating a thread works (glibc falls back from clone3 to clone with CLONE_THREAD)");

    /* TSYNC: the thread that already existed was filtered too */
    early_go = 1;
    pthread_join(early, NULL);
    OK(early_done && early_exec_failed && early_fork_failed, "TSYNC: the earlier thread cannot exec or fork either");

    /* carpetas */
    int f = openat(wfd, "salida.txt", O_CREAT | O_WRONLY, 0644);
    OK(f >= 0 && write(f, "x", 1) == 1, "writing inside rw_dir_fds works (fd=%d)", f);
    if (f >= 0) close(f);
    OK(openat(wfd, "salida.txt", O_WRONLY | O_TRUNC) < 0, "truncating inside rw_dir_fds fails (%s)", strerror(errno));
    char out[600];
    snprintf(out, sizeof out, "%s/fuera.txt", outside);
    OK(open(out, O_CREAT | O_WRONLY, 0644) < 0, "writing outside rw_dir_fds fails (%s)", strerror(errno));
    OK(open("/etc/passwd", O_RDONLY) < 0, "reading /etc fails (%s)", strerror(errno));
    OK(open("/usr/share/fonts", O_RDONLY | O_DIRECTORY) >= 0, "ro_paths (fonts) are readable");

    /* kcmp: against itself yes, against another process no */
#ifdef SYS_kcmp
    long k1 = syscall(SYS_kcmp, getpid(), getpid(), KCMP_FILE, wfd, wfd);
    OK(k1 == 0, "kcmp against itself works (%ld, %s)", k1, k1 < 0 ? strerror(errno) : "equal");
    long k2 = syscall(SYS_kcmp, getpid(), getppid(), KCMP_FILE, 0, 0);
    OK(k2 < 0 && errno == EPERM, "kcmp against another process fails (%s)", strerror(errno));
#endif
    OK(ptrace(PTRACE_TRACEME, 0, 0, 0) < 0, "ptrace fails (%s)", strerror(errno));
    return 0;
}

int main(int argc, char **argv) {
    home = getenv("HOME");
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "helper")) helper_mode();
    else app_mode(!strcmp(mode, "net"));
    printf("sandbox-test %s: %d/%d passed\n", mode[0] ? mode : "app", checks - fails, checks);
    return fails ? 1 : 0;
}
