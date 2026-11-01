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
#define KEYSCAN "/usr/bin/ssh-keyscan"
#define KEYGEN "/usr/bin/ssh-keygen"
#define OPENSSL "/usr/bin/openssl"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static const char *state = "idle", *package = "";
static char where[PATH_MAX], print_[256];
static int answer;                      /* 0 waiting, 1 yes, -1 no */
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

/* runs prog with `input` on stdin and returns its stdout. scope: the app scope it may reach the network from, or NULL */
static int filter_in(const char *scope, char *const argv[], const char *input, size_t ilen, char *out, size_t cap) {
    int a[2], b[2];
    if (pipe2(a, O_CLOEXEC)) return -1;
    if (pipe2(b, O_CLOEXEC)) { close(a[0]); close(a[1]); return -1; }
    pid_t p = fork();
    if (!p) {
        dup2(a[0], 0); dup2(b[1], 1);
        syscall(SYS_close_range, 3, ~0u, 0);
        sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, NULL);
        if (scope && app_scope_enter(scope) && getenv("LORDOS_SESSIOND")) _exit(126);
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
    out[o] = 0;
    int st = 0;
    waitpid(p, &st, 0);
    if (w || !WIFEXITED(st) || (WEXITSTATUS(st) && !scope)) return -1;
    return (int)o;
}
static int filter(char *const argv[], const char *input, size_t ilen, char *out, size_t cap) { return filter_in(NULL, argv, input, ilen, out, cap); }

static void mkparents(const char *dir) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", dir);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0700); *s = '/'; }
    mkdir(p, 0700);
}

/* asks the person about a fingerprint: 1 yes. The password stays in q meanwhile, at most two minutes. */
static int ask_trust(const char *fp) {
    pthread_mutex_lock(&mu);
    snprintf(print_, sizeof print_, "%s", fp);
    answer = 0;
    state = "confirm";
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < 1200; i++) {
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&mu);
        int a = answer;
        pthread_mutex_unlock(&mu);
        if (a) { pthread_mutex_lock(&mu); state = "running"; print_[0] = 0; pthread_mutex_unlock(&mu); return a > 0; }
    }
    return 0;
}

void rconnect_trust(int yes) { pthread_mutex_lock(&mu); answer = yes ? 1 : -1; pthread_mutex_unlock(&mu); }

static char *slurp(const char *path) {
    FILE *f = fopen(path, "re");
    if (!f) return NULL;
    static char b[256 * 1024];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return b;
}

/* SSH: the key must be the one the person accepted; the first time they see its fingerprint. rclone checks the file again
 * on the real connection. -1 finished here (state set), 0 go on. */
static int trust_ssh(char *known, size_t kn) {
    if (access(KEYSCAN, X_OK) || access(KEYGEN, X_OK)) { finish("missing", "openssh-client", NULL); return -1; }
    snprintf(known, kn, "%s/.config/lordos/remote_known_hosts", fsops_home());
    char tag[300], ps[8], *out = malloc(32768), fp[512] = "";
    if (!out) { finish("failed", NULL, NULL); return -1; }
    rconnect_host_tag(q.host, q.port, tag, sizeof tag);
    snprintf(ps, sizeof ps, "%d", q.port ? q.port : 22);
    char *scan[] = { KEYSCAN, "-T", "5", "-p", ps, "-t", "ed25519,ecdsa,rsa", "--", q.host, NULL };
    int n = filter_in("rclone", scan, "", 0, out, 32768);
    int have = 0, changed = 0, match = 0;
    char *file = slurp(known), good[32768] = "";
    size_t gl = 0;
    for (char *line = n > 0 ? strtok(out, "\n") : NULL; line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || !rconnect_keyline_ok(line, tag)) continue;
        have = 1;
        int k = file ? rconnect_known(file, tag, line) : -1;
        match |= k == 1;
        changed |= k == 0;
        gl += (size_t)snprintf(good + gl, sizeof good - gl, "%s\n", line);
    }
    free(out);
    if (!have) { finish("failed", NULL, NULL); return -1; }
    if (match) return 0;
    if (changed) { finish("changed", NULL, NULL); return -1; }           /* refused outright */
    char *kg[] = { KEYGEN, "-lf", "-", NULL };
    if (filter(kg, good, gl, fp, sizeof fp) <= 0) { finish("failed", NULL, NULL); return -1; }
    fp[strcspn(fp, "\n")] = 0;
    if (!ask_trust(fp)) { finish("refused", NULL, NULL); return -1; }
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/.config/lordos", fsops_home());
    mkparents(dir);
    int fd = open(known, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 || write(fd, good, gl) != (ssize_t)gl) { if (fd >= 0) close(fd); finish("failed", NULL, NULL); return -1; }
    close(fd);
    return 0;
}

/* TLS stays verified. A certificate the system doesn't trust needs the person to accept its fingerprint, and then only that
 * certificate is trusted for that host and port (--ca-cert); a different one is refused. ca: "" when the system trusts it. */
static int trust_tls(char *ca, size_t cn) {
    ca[0] = 0;
    if (access(OPENSSL, X_OK)) { finish("missing", "openssl", NULL); return -1; }
    int port = q.port ? q.port : q.kind[0] == 'f' ? 21 : 443;
    char conn[300], *out = malloc(65536), pem[16384], fp[256] = "", dir[PATH_MAX];
    if (!out) { finish("failed", NULL, NULL); return -1; }
    snprintf(conn, sizeof conn, "%s:%d", q.host, port);
    char *ftp[] = { OPENSSL, "s_client", "-connect", conn, "-servername", q.host, "-starttls", "ftp", "-verify_hostname", q.host, NULL };
    char *web[] = { OPENSSL, "s_client", "-connect", conn, "-servername", q.host, "-verify_hostname", q.host, NULL };
    int n = filter_in("rclone", q.kind[0] == 'f' && port != 990 ? ftp : web, "", 0, out, 65536);
    int v = n > 0 ? rconnect_tls_parse(out, pem, sizeof pem) : -1;
    free(out);
    if (v < 0) { finish("failed", NULL, NULL); return -1; }
    if (v == 1) return 0;
    char *x[] = { OPENSSL, "x509", "-noout", "-fingerprint", "-sha256", NULL };
    if (filter(x, pem, strlen(pem), fp, sizeof fp) <= 0) { finish("failed", NULL, NULL); return -1; }
    fp[strcspn(fp, "\n")] = 0;
    snprintf(dir, sizeof dir, "%s/.config/lordos/remote_certs", fsops_home());
    mkparents(dir);
    snprintf(ca, cn, "%s/%s_%d.pem", dir, q.host, port);
    char *file = slurp(ca);
    if (file) {
        if (strcmp(file, pem)) { finish("changed", NULL, NULL); return -1; }
        return 0;
    }
    if (!ask_trust(fp)) { finish("refused", NULL, NULL); return -1; }
    int fd = open(ca, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 || write(fd, pem, strlen(pem)) != (ssize_t)strlen(pem)) { if (fd >= 0) close(fd); finish("failed", NULL, NULL); return -1; }
    close(fd);
    return 0;
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

static void mount_job(void) {
    if (access(RCLONE, X_OK)) { finish("missing", "rclone", NULL); return; }
    if (access(FUSERMOUNT, X_OK)) { finish("missing", "fuse3", NULL); return; }
    char known[PATH_MAX] = "", ca[PATH_MAX] = "";
    if (!strcmp(q.kind, "sftp") && trust_ssh(known, sizeof known)) return;
    if ((!strcmp(q.kind, "ftps") || !strcmp(q.kind, "davs")) && trust_tls(ca, sizeof ca)) return;
    char *ob_argv[] = { RCLONE, "obscure", "-", NULL };
    char ob[512] = "", cfg[2048], root[PATH_MAX], dir[PATH_MAX];
    if (q.password[0]) {
        int n = filter(ob_argv, q.password, strlen(q.password), ob, sizeof ob);
        if (n <= 0) { finish("failed", NULL, NULL); return; }
        while (n && (ob[n - 1] == '\n' || ob[n - 1] == '\r')) ob[--n] = 0;
    }
    int cl = rconnect_config(cfg, sizeof cfg, q.kind, q.host, q.port, q.user, ob, q.path, known);
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
    char *argv[] = { "rclone", "mount", src, dir, "--config", "/proc/self/fd/3", "--vfs-cache-mode", "writes", "--contimeout", "8s", "--low-level-retries", "2", "--retries", "1", ca[0] ? "--ca-cert" : NULL, ca, NULL };
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
    jb_raw(&b, ",\"print\":");
    jb_str(&b, print_);
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
