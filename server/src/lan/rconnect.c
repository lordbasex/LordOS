/* rconnect.c — see rconnect.h. */
#include "rconnect.h"
#include "lanaddr.h"
#include "../doc/wire.h"
#include "../fsops.h"
#include "../sec/cgroup.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define RCLONE "/usr/bin/rclone"
#define FUSERMOUNT "/usr/bin/fusermount3"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static const char *state = "idle", *package = "";
static char where[PATH_MAX];
static int busy;
static struct { char kind[8], host[254], user[65], domain[65], password[256], path[201]; int port; } q;

static void finish(const char *st, const char *pkg, const char *path) {
    pthread_mutex_lock(&mu);
    state = st;
    package = pkg ? pkg : "";
    snprintf(where, sizeof where, "%s", path ? path : "");
    busy = 0;
    explicit_bzero(&q, sizeof q);
    pthread_mutex_unlock(&mu);
}

/* in the child: only 0, 1, 2 and maybe 3 stay */
static void child_fds(int in, int fd3) {
    int null = open("/dev/null", O_RDWR);
    if (fd3 >= 0) { int f = fcntl(fd3, F_DUPFD, 10); dup2(f, 3); }
    dup2(in >= 0 ? in : null, 0);
    dup2(null, 1);
    syscall(SYS_close_range, fd3 >= 0 ? 4 : 3, ~0u, 0);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
}

/* runs prog with `input` on stdin and returns its stdout; no scope: nothing here uses the network */
static int filter(char *const argv[], const char *input, size_t ilen, char *out, size_t cap) {
    int a[2], b[2];
    if (pipe2(a, O_CLOEXEC)) return -1;
    if (pipe2(b, O_CLOEXEC)) { close(a[0]); close(a[1]); return -1; }
    pid_t p = fork();
    if (!p) {
        dup2(a[0], 0); dup2(b[1], 1);
        syscall(SYS_close_range, 3, ~0u, 0);
        sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, NULL);
        execve(argv[0], argv, environ);
        _exit(127);
    }
    close(a[0]); close(b[1]);
    if (p < 0) { close(a[1]); close(b[0]); return -1; }
    int w = write(a[1], input, ilen) == (ssize_t)ilen ? 0 : -1;
    close(a[1]);
    size_t o = 0;
    for (;;) { ssize_t r = read(b[0], out + o, cap - 1 - o); if (r <= 0) break; o += (size_t)r; if (o >= cap - 1) break; }
    close(b[0]);
    int st = 0;
    waitpid(p, &st, 0);
    if (w || !WIFEXITED(st) || WEXITSTATUS(st)) return -1;
    return (int)o;
}

static int memfd_with(const void *data, size_t n) {
    int fd = (int)syscall(SYS_memfd_create, "lordos-connect", 1u /* MFD_CLOEXEC */);
    if (fd < 0) return -1;
    if (write(fd, data, n) != (ssize_t)n) { close(fd); return -1; }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

static int mounted_at(const char *dir) {
    FILE *f = fopen("/proc/self/mounts", "re");
    if (!f) return 0;
    char line[4096], mnt[2048];
    int yes = 0;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "%*s %2047s", mnt) == 1 && !strcmp(mnt, dir)) yes = 1;
    fclose(f);
    return yes;
}

static const char *remote_root(char *out, size_t n) {
    snprintf(out, n, "%s/.local/share/lordos/remote", fsops_home());
    return out;
}

static void mkparents(const char *dir) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", dir);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0700); *s = '/'; }
    mkdir(p, 0700);
}

static void mount_job(void) {
    if (access(RCLONE, X_OK)) { finish("missing", "rclone", NULL); return; }
    if (access(FUSERMOUNT, X_OK)) { finish("missing", "fuse3", NULL); return; }
    char *ob_argv[] = { RCLONE, "obscure", "-", NULL };
    char ob[512] = "", cfg[1024], root[PATH_MAX], dir[PATH_MAX];
    if (q.password[0]) {
        int n = filter(ob_argv, q.password, strlen(q.password), ob, sizeof ob);
        if (n <= 0) { finish("failed", NULL, NULL); return; }
        while (n && (ob[n - 1] == '\n' || ob[n - 1] == '\r')) ob[--n] = 0;
    }
    int cl = rconnect_config(cfg, sizeof cfg, q.kind, q.host, q.port, q.user, ob, q.path);
    explicit_bzero(ob, sizeof ob);
    if (cl < 0) { finish("refused", NULL, NULL); return; }
    int fd = memfd_with(cfg, (size_t)cl);
    explicit_bzero(cfg, sizeof cfg);
    if (fd < 0) { finish("failed", NULL, NULL); return; }
    snprintf(dir, sizeof dir, "%s/%s on %s", remote_root(root, sizeof root), q.kind, q.host);
    mkparents(dir);
    if (mounted_at(dir)) { close(fd); finish("done", NULL, dir); return; }
    char src[240] = "r:";
    if (strcmp(q.kind, "webdav") && strcmp(q.kind, "davs")) snprintf(src, sizeof src, "r:%s", q.path);
    char *argv[] = { "rclone", "mount", src, dir, "--config", "/proc/self/fd/3", "--vfs-cache-mode", "writes", "--contimeout", "8s", "--low-level-retries", "2", "--retries", "1", NULL };
    pid_t p = fork();
    if (!p) {
        child_fds(-1, fd);
        if (app_scope_enter("rclone") && getenv("LORDOS_SESSIOND")) _exit(126);
        execve(RCLONE, argv, environ);
        _exit(127);
    }
    close(fd);
    if (p < 0) { finish("failed", NULL, NULL); return; }
    for (int i = 0; i < 200; i++) {
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (mounted_at(dir)) { finish("done", NULL, dir); waitpid(p, NULL, 0); rmdir(dir); return; }
        int st;
        if (waitpid(p, &st, WNOHANG) == p) { rmdir(dir); finish("failed", NULL, NULL); return; }
    }
    kill(p, SIGTERM);
    waitpid(p, NULL, 0);
    rmdir(dir);
    finish("failed", NULL, NULL);
}

static void *job(void *arg) {
    (void)arg;
    mount_job();
    return NULL;
}

int rconnect_start(const char *kind, const char *host, int port, const char *user, const char *domain, const char *password, const char *path) {
    static const char *const kinds[] = { "sftp", "ftp", "ftps", "webdav", "davs", NULL };
    int k = 0;
    for (int i = 0; kinds[i]; i++) k |= !strcmp(kinds[i], kind);
    if (!k || !rconnect_host_ok(host) || !rconnect_user_ok(user) || !rconnect_user_ok(domain) || !rconnect_path_ok(path) ||
        port < 0 || port > 65535 || strlen(password) >= sizeof q.password || strpbrk(password, "\n\r")) return 0;
    pthread_mutex_lock(&mu);
    if (busy) { pthread_mutex_unlock(&mu); return 0; }
    snprintf(q.kind, sizeof q.kind, "%s", kind);
    snprintf(q.host, sizeof q.host, "%s", host);
    snprintf(q.user, sizeof q.user, "%s", user);
    snprintf(q.domain, sizeof q.domain, "%s", domain);
    snprintf(q.password, sizeof q.password, "%s", password);
    snprintf(q.path, sizeof q.path, "%s", path);
    q.port = port;
    busy = 1;
    state = "running";
    package = "";
    where[0] = 0;
    pthread_mutex_unlock(&mu);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 1024 * 1024);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    int r = pthread_create(&th, &at, job, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&at);
    if (r) { finish("failed", NULL, NULL); return 0; }
    return 1;
}

char *rconnect_poll(void) {
    JBuf b;
    jb_init(&b);
    pthread_mutex_lock(&mu);
    jb_printf(&b, "{\"state\":\"%s\",\"package\":\"%s\",\"path\":", state, package);
    jb_str(&b, where);
    pthread_mutex_unlock(&mu);
    jb_raw(&b, "}");
    if (b.oom) { jb_free(&b); return NULL; }
    return b.p;
}

int rconnect_unmount(const char *path) {
    char root[PATH_MAX];
    size_t n = strlen(remote_root(root, sizeof root));
    if (strncmp(path, root, n) || path[n] != '/' || strchr(path + n + 1, '/') || strstr(path, "..")) return -1;
    pid_t p = fork();
    if (!p) {
        child_fds(-1, -1);
        char *argv[] = { "fusermount3", "-u", (char *)path, NULL };
        execve(FUSERMOUNT, argv, environ);
        _exit(127);
    }
    if (p < 0) return -1;
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) && !WEXITSTATUS(st) ? 0 : -1;
}
