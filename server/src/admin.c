/* admin.c — minimal privileged helper of LordOS (like polkit in GNOME, but with a closed
 * list of actions).
 *
 * lord-server starts as root (to open DRM and evdev) and drops privileges right away.
 * Just before that, it creates this helper: a child process that stays root, with no screen, no
 * input and no other descriptor, connected to the server by a socketpair. It only understands
 * the actions in the `actions` table (below) and each one validates its argument on its own,
 * without trusting anything the user can edit. Actions in this file:
 *
 *   locale-gen <name>         generates a region listed in /usr/share/i18n/SUPPORTED (UTF-8)
 *   system-locale <name>      system default region (/etc/default/locale), already generated
 *   timezone <Zone/City>      time zone that exists in /usr/share/zoneinfo (timedatectl)
 *   langpack <ll[_CC]>        Debian packages for the language: manpages, hunspell, aspell,
 *                             hyphen, mythes (ll-cc or ll, e.g. hunspell-pt-br), the fonts for its
 *                             script and the input method dictionary (skkdic, pinyin)
 *                             (list compiled in here, not read from files)
 *
 * The package (pkg/), network and VPN (net/), installer (install/), remote desktop and remote
 * login (remote/: remote_admin.c, ssh_admin.c)
 * and users (users/) actions live in their modules; the table says which ones exist.
 *
 * Protocol (socketpair): request "<id> <action> <length>\n" followed by <length> bytes of
 * argument (up to ADMIN_ARG_MAX; it may have several lines, like a VPN configuration).
 * Replies "<id> LOG text" (progress), and at the end "<id> OK text" or "<id> ERR text".
 * Unsolicited notices go with id 0: "0 EVENT text". Long actions (installing packages)
 * run in a child process, so several can be in progress at once. It only serves if the
 * session user is an administrator (sudo, admin or wheel group), except for the actions
 * marked A_ANYONE (connecting to a network, as in NetworkManager). It exits when lord-server
 * closes the connection, and with it the resident processes (wpa_supplicant, VPN, DHCP). */
#include "admin.h"
#include "pkg/pkg.h"
#include "net/net.h"
#include "install/install.h"
#include "remote/remote_admin.h"
#include "remote/ssh_admin.h"
#include "remote/ssh_root.h"
#include "users/users_admin.h"
#include "disk/disk.h"
#include "imager/imager_admin.h"
#include "logs/logs_admin.h"
#include "hw/hw_admin.h"
#include "clean/clean_admin.h"
#include "vm/vm_admin.h"
#include "lan/share_admin.h"
#include "remote/sgrant_admin.h"
#include "monitor/proc_admin.h"
#include "localename.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- helper side (root) ---- */

static int out_fd = -1;                 /* socket, or the pipe to the helper in a child */
static int cur_id;

/* lord-netd (the system network, see below): in its main process, replies go to the client that
 * asked (cur_client) and events (id 0) to everyone. It is 0 in child processes. */
static int netd_main;
static int cur_client = -1;
static void netd_route(int id, const char *line, size_t n);

static void write_fd_all(int fd, const char *p, size_t n) {
    for (ssize_t w, o = 0; o < (ssize_t)n; o += w) {
        w = write(fd, p + o, n - (size_t)o);
        if (w < 0 && errno == EINTR) { w = 0; continue; }
        if (w <= 0) return;
    }
}

static void out_write(int id, const char *line, size_t n) {
    if (netd_main) netd_route(id, line, n);
    else write_fd_all(out_fd, line, n);
}

static void put_line(int id, const char *kind, const char *msg) {
    char line[1200];
    int n = snprintf(line, sizeof line, "%d %s %s\n", id, kind, msg);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) { n = sizeof line - 1; line[n - 1] = '\n'; }
    out_write(id, line, (size_t)n);
}

static void vput(int id, const char *kind, const char *fmt, va_list ap) {
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, ap);
    for (char *p = msg; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    put_line(id, kind, msg);
}

void admin_say(const char *kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vput(cur_id, kind, fmt, ap);
    va_end(ap);
}

void admin_event(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vput(0, "EVENT", fmt, ap);
    va_end(ap);
}

static int is_admin(const char *user) {
    struct passwd *pw = getpwnam(user);
    if (!pw) return 0;
    gid_t groups[256];
    int n = 256;
    if (getgrouplist(user, pw->pw_gid, groups, &n) < 0) return 0;
    for (int i = 0; i < n; i++) {
        struct group *g = getgrgid(groups[i]);
        if (g && (!strcmp(g->gr_name, "sudo") || !strcmp(g->gr_name, "admin") || !strcmp(g->gr_name, "wheel"))) return 1;
    }
    return 0;
}

static char *const clean_env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8", "DEBIAN_FRONTEND=noninteractive", NULL };

/* Child with stdin on /dev/null, stdout (and stderr if err) to the pipe, no other descriptors. */
static pid_t spawn(const char *const argv[], char *const env[], int out, int err) {
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) dup2(null, 0);
        dup2(out >= 0 ? out : null, 1);
        dup2(err ? (out >= 0 ? out : null) : null, 2);
        syscall(SYS_close_range, 3, ~0u, 0);
        signal(SIGPIPE, SIG_DFL);
        /* if the helper dies (even from a signal), the program does too: no orphaned
         * wpa_supplicant or openvpn are left. It survives execve (the programs are not setuid). */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() == 1) _exit(127);
        execve(argv[0], (char *const *)argv, env ? env : clean_env);
        _exit(127);
    }
    return pid;
}

static int wait_code(pid_t pid) {
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

int admin_run(const char *const argv[]) {
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -1;
    pid_t pid = spawn(argv, NULL, p[1], 1);
    close(p[1]);
    if (pid < 0) { close(p[0]); return -1; }
    FILE *f = fdopen(p[0], "r");
    char line[512];
    int lines = 0;
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] && lines++ < 2000) admin_say("LOG", "%s", line);
    }
    if (f) fclose(f); else close(p[0]);
    return wait_code(pid);
}

int admin_run_input(const char *const argv[], const char *input) {
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC)) return -1;
    if (pipe2(out, O_CLOEXEC)) { close(in[0]); close(in[1]); return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        syscall(SYS_close_range, 3, ~0u, 0);
        signal(SIGPIPE, SIG_DFL);
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        execve(argv[0], (char *const *)argv, clean_env);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (pid < 0) { close(in[1]); close(out[0]); return -1; }
    size_t n = strlen(input);
    for (size_t o = 0; o < n;) {
        ssize_t w = write(in[1], input + o, n - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        o += (size_t)w;
    }
    close(in[1]);
    FILE *f = fdopen(out[0], "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (line[0]) admin_say("LOG", "%s", line);
    }
    if (f) fclose(f); else close(out[0]);
    return wait_code(pid);
}

int admin_run_capture(const char *const argv[], char *out, size_t cap) {
    int p[2];
    if (!cap || pipe2(p, O_CLOEXEC)) return -1;
    pid_t pid = spawn(argv, NULL, p[1], 0);
    close(p[1]);
    if (pid < 0) { close(p[0]); return -1; }
    size_t n = 0;
    for (ssize_t r; (r = read(p[0], out + n, cap - 1 - n)) != 0;) {
        if (r < 0) { if (errno == EINTR) continue; break; }
        n += (size_t)r;
        if (n == cap - 1) {                 /* full: the rest is discarded */
            char junk[4096];
            while (read(p[0], junk, sizeof junk) > 0) {}
            break;
        }
    }
    out[n] = 0;
    close(p[0]);
    return wait_code(pid);
}

/* ---- child processes: long and resident actions ---- */

#define MAX_KIDS 32

typedef struct {
    pid_t pid;
    int fd;                             /* its output */
    int id;                             /* action (0 if resident) */
    int done;                           /* already sent OK/ERR */
    int raw;                            /* external program: each line is wrapped in an EVENT */
    char name[32];                      /* resident */
    int client;                         /* lord-netd: client that requested the action (-1: none) */
    char buf[2048];
    size_t len;
} Kid;

static Kid kids[MAX_KIDS];

static Kid *kid_new(void) {
    for (int i = 0; i < MAX_KIDS; i++) if (!kids[i].pid) { memset(&kids[i], 0, sizeof kids[i]); return &kids[i]; }
    return NULL;
}

static Kid *daemon_find(const char *name) {
    for (int i = 0; i < MAX_KIDS; i++) if (kids[i].pid && !kids[i].id && !strcmp(kids[i].name, name)) return &kids[i];
    return NULL;
}

static int valid_name(const char *name) {
    size_t n = strlen(name);
    if (!n || n >= sizeof kids[0].name) return 0;
    for (const char *p = name; *p; p++) if (!isalnum((unsigned char)*p) && !strchr("-_.:", *p)) return 0;
    return 1;
}

int admin_daemon_running(const char *name) { return daemon_find(name) != NULL; }

int admin_daemon_exec(const char *name, const char *const argv[], const char *const env[]) {
    if (!valid_name(name) || daemon_find(name)) return -1;
    Kid *k = kid_new();
    int p[2];
    if (!k || pipe2(p, O_CLOEXEC)) return -1;
    pid_t pid = spawn(argv, (char *const *)env, p[1], 1);
    close(p[1]);
    if (pid < 0) { close(p[0]); return -1; }
    *k = (Kid){ .pid = pid, .fd = p[0], .raw = 1, .client = -1 };
    snprintf(k->name, sizeof k->name, "%s", name);
    return 0;
}

int admin_daemon_fork(const char *name, void (*fn)(void *), void *arg) {
    if (!valid_name(name) || daemon_find(name)) return -1;
    Kid *k = kid_new();
    int p[2];
    if (!k || pipe2(p, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        /* only the pipe: neither lord-server's socket nor, in lord-netd, its lock and socket
         * (otherwise a DHCP client outliving a dead lord-netd would keep the next one from starting) */
        int keep = fcntl(p[1], F_DUPFD, 3);
        for (int f = 3; f < 1024; f++) if (f != keep) close(f);
        out_fd = keep;
        cur_id = 0;
        netd_main = 0;
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        signal(SIGTERM, SIG_DFL);
        fn(arg);
        _exit(0);
    }
    close(p[1]);
    if (pid < 0) { close(p[0]); return -1; }
    *k = (Kid){ .pid = pid, .fd = p[0], .client = -1 };
    snprintf(k->name, sizeof k->name, "%s", name);
    return 0;
}

int admin_daemon_stop(const char *name) {
    Kid *k = daemon_find(name);
    if (!k) return -1;
    kill(k->pid, SIGTERM);
    return 0;
}

int admin_daemon_signal(const char *name, int sig) {
    Kid *k = daemon_find(name);
    return k && kill(k->pid, sig) == 0 ? 0 : -1;
}

static void kid_line(Kid *k, char *line) {
    /* "<id> KIND text": forwarded as is; noted if it already finished */
    char *sp = strchr(line, ' ');
    if (sp && (!strncmp(sp + 1, "OK", 2) || !strncmp(sp + 1, "ERR", 3))) k->done = 1;
    size_t n = strlen(line);
    line[n] = '\n';
    cur_client = k->client;
    out_write(atoi(line), line, n + 1);
    line[n] = 0;
}

static void kid_read(Kid *k) {
    ssize_t r = read(k->fd, k->buf + k->len, sizeof k->buf - 1 - k->len);
    if (r > 0) {
        k->len += (size_t)r;
        char *nl;
        while ((nl = memchr(k->buf, '\n', k->len))) {
            *nl = 0;
            if (k->raw) {
                char msg[1100];
                snprintf(msg, sizeof msg, "daemon %s %s", k->name, k->buf);
                for (char *p = msg; *p; p++) if (*p == '\r') *p = ' ';
                put_line(0, "EVENT", msg);
            } else kid_line(k, k->buf);
            size_t used = (size_t)(nl - k->buf) + 1;
            memmove(k->buf, nl + 1, k->len - used);
            k->len -= used;
        }
        if (k->len >= sizeof k->buf - 1) k->len = 0;       /* line too long */
        return;
    }
    if (r < 0 && (errno == EINTR || errno == EAGAIN)) return;
    /* finished */
    close(k->fd);
    int code = wait_code(k->pid);
    cur_client = k->client;
    if (k->id && !k->done) put_line(k->id, "ERR", code == 0 ? "admin.noReply" : "admin.actionFailed");
    if (!k->id) {
        char msg[96];
        snprintf(msg, sizeof msg, "daemon %s exit %d", k->name, code);
        put_line(0, "EVENT", msg);
    }
    k->pid = 0;
}

/* ---- action table ---- */

enum { A_FORK = 1, A_ANYONE = 2 };

typedef void (*ActionFn)(const AdminReq *r, const char *arg, size_t len);

#ifndef LORD_NETD
static void act_locale_gen(const char *name);
static void act_system_locale(const char *name);
static void act_timezone(const char *zone);
static void act_langpack(const char *arg);
static void a_locale_gen(const AdminReq *r, const char *a, size_t n) { (void)r; (void)n; act_locale_gen(a); }
static void a_system_locale(const AdminReq *r, const char *a, size_t n) { (void)r; (void)n; act_system_locale(a); }
static void a_timezone(const AdminReq *r, const char *a, size_t n) { (void)r; (void)n; act_timezone(a); }
static void a_langpack(const AdminReq *r, const char *a, size_t n) { (void)r; (void)n; act_langpack(a); }

#endif /* LORD_NETD */

static const struct { const char *name; ActionFn fn; int flags; } actions[] = {
#ifdef LORD_NETD
    /* lord-netd: networking only (docs/NETWORK.md, "The network belongs to the system") */
    NET_ADMIN_ACTIONS
#else
    { "locale-gen", a_locale_gen, A_FORK },
    { "system-locale", a_system_locale, 0 },
    { "timezone", a_timezone, 0 },
    { "langpack", a_langpack, A_FORK },
    PKG_ADMIN_ACTIONS
    NET_ADMIN_ACTIONS
    INSTALL_ADMIN_ACTIONS
    REMOTE_ADMIN_ACTIONS
    SSH_ADMIN_ACTIONS
    SSH_ROOT_ACTIONS
    USERS_ADMIN_ACTIONS
    DISK_ADMIN_ACTIONS
    IMAGER_ADMIN_ACTIONS
    LOGS_ADMIN_ACTIONS
    HW_ADMIN_ACTIONS
    CLEAN_ADMIN_ACTIONS
    VM_ADMIN_ACTIONS
    PROC_ADMIN_ACTIONS
    SHARE_ADMIN_ACTIONS
    SGRANT_ADMIN_ACTIONS
#endif /* LORD_NETD: other modules' actions go above this line */
};
#define N_ACTIONS (sizeof actions / sizeof actions[0])

int admin_known(const char *action) {
    for (size_t i = 0; i < N_ACTIONS; i++) if (!strcmp(actions[i].name, action)) return 1;
    return 0;
}

#ifndef LORD_NETD
/* Valid region name supported by glibc: "es_AR.UTF-8", "en_IN.UTF-8" (localename.h). */
static int supported_locale(const char *name) {
    size_t n = strlen(name);
    if (!n || n > 64) return 0;
    for (const char *p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("_.-@", *p)) return 0;
    return locale_supported_utf8(name);
}

/* Is it already generated? glibc maps the locale archive only once per process, so
 * a freshly generated region is not visible with newlocale() here: `locale -a` is asked, which
 * is a new process. Compares normalized ("es_AR.UTF-8" = "es_AR.utf8"). */
static void norm(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (; *in && o + 1 < n; in++) if (*in != '-') out[o++] = (char)tolower((unsigned char)*in);
    out[o] = 0;
}

static int locale_generated(const char *name) {
    int p[2];
    if (pipe(p)) return 0;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 2); }
        syscall(SYS_close_range, 3, ~0u, 0);
        char *const argv[] = { "/usr/bin/locale", "-a", NULL };
        char *const env[] = { "PATH=/usr/bin:/bin", "LANG=C", NULL };
        execve(argv[0], argv, env);
        _exit(127);
    }
    close(p[1]);
    char want[96], line[128], got[128];
    norm(name, want, sizeof want);
    int found = 0;
    FILE *f = pid > 0 ? fdopen(p[0], "r") : NULL;
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        norm(line, got, sizeof got);
        if (!strcmp(got, want)) found = 1;
    }
    if (f) fclose(f); else close(p[0]);
    if (pid > 0) waitpid(pid, NULL, 0);
    return found;
}

/* Enables the line in /etc/locale.gen (uncomments or adds it) and runs locale-gen. */
static void act_locale_gen(const char *name) {
    if (!supported_locale(name)) { admin_err("locale.unsupported", "name", name, NULL); return; }
    char want[160];
    snprintf(want, sizeof want, "%s UTF-8", name);
    FILE *f = fopen("/etc/locale.gen", "r");
    char *buf = NULL;
    size_t len = 0;
    FILE *mem = open_memstream(&buf, &len);
    int found = 0;
    char line[512];
    while (f && mem && fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == '#' || *s == ' ') s++;
        if (!strncmp(s, want, strlen(want)) && (s[strlen(want)] == '\n' || !s[strlen(want)])) {
            fprintf(mem, "%s\n", want);
            found = 1;
        } else fputs(line, mem);
    }
    if (f) fclose(f);
    if (!mem) { admin_err("sys.outOfMemory", NULL); return; }
    if (!found) fprintf(mem, "%s\n", want);
    fclose(mem);
    int fd = open("/etc/locale.gen.lordos", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int ok = fd >= 0 && write(fd, buf, len) == (ssize_t)len;
    if (fd >= 0) ok = close(fd) == 0 && ok;
    free(buf);
    if (!ok || rename("/etc/locale.gen.lordos", "/etc/locale.gen")) { admin_err("sys.writeFailed", "path", "/etc/locale.gen", NULL); return; }
    const char *const argv[] = { "/usr/sbin/locale-gen", NULL };
    int r = admin_run(argv);
    if (r == 0) admin_ok("locale.generated", "name", name, NULL);
    else admin_err("sys.programFailed", "program", "locale-gen", "code", lerr_int(r), NULL);
}

static void act_system_locale(const char *name) {
    if (!supported_locale(name) && strcmp(name, "C.UTF-8")) { admin_err("locale.unsupported", "name", name, NULL); return; }
    if (!locale_generated(name)) { admin_err("locale.notGenerated", "name", name, NULL); return; }
    int fd = open("/etc/default/locale.lordos", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { admin_err("sys.writeFailed", "path", "/etc/default/locale", NULL); return; }
    dprintf(fd, "LANG=%s\n", name);
    close(fd);
    if (rename("/etc/default/locale.lordos", "/etc/default/locale")) { admin_err("sys.writeFailed", "path", "/etc/default/locale", NULL); return; }
    admin_ok("locale.systemSet", "name", name, NULL);
}

static void act_timezone(const char *zone) {
    size_t n = strlen(zone);
    int ok = n > 0 && n < 64 && zone[0] != '/' && !strstr(zone, "..");
    for (const char *p = zone; ok && *p; p++) if (!isalnum((unsigned char)*p) && !strchr("/_+-", *p)) ok = 0;
    char path[320];
    struct stat st;
    snprintf(path, sizeof path, "/usr/share/zoneinfo/%s", zone);
    if (!ok || stat(path, &st) || !S_ISREG(st.st_mode)) { admin_err("tz.unknown", "zone", zone, NULL); return; }
    const char *const argv[] = { "/usr/bin/timedatectl", "set-timezone", zone, NULL };
    if (access(argv[0], X_OK) == 0 && admin_run(argv) == 0) { admin_ok("tz.set", "zone", zone, NULL); return; }
    /* without systemd: the /etc/localtime link and /etc/timezone, like dpkg-reconfigure */
    unlink("/etc/localtime.lordos");
    if (symlink(path, "/etc/localtime.lordos") || rename("/etc/localtime.lordos", "/etc/localtime")) { admin_err("sys.writeFailed", "path", "/etc/localtime", NULL); return; }
    FILE *f = fopen("/etc/timezone", "w");
    if (f) { fprintf(f, "%s\n", zone); fclose(f); }
    admin_ok("tz.set", "zone", zone, NULL);
}

/* Fonts for the language's script and dictionaries for the input methods (ime.c), from
 * Debian 13. Compiled in: not read from any file. */
static const struct { const char *lang, *pkgs; } script_fonts[] = {
    { "ja", "fonts-noto-cjk" }, { "zh", "fonts-noto-cjk" }, { "ko", "fonts-noto-cjk" },
    { "ja", "skkdic" }, { "zh", "rime-data-pinyin-simp" },
    { "ar", "fonts-noto-core" }, { "fa", "fonts-noto-core" }, { "ur", "fonts-noto-core" }, { "he", "fonts-noto-core" },
    { "hi", "fonts-noto-core" }, { "bn", "fonts-noto-core" }, { "ta", "fonts-noto-core" }, { "te", "fonts-noto-core" },
    { "th", "fonts-noto-core" }, { "el", "fonts-noto-core" }, { "ru", "fonts-noto-core" }, { "uk", "fonts-noto-core" },
    { "ka", "fonts-noto-core" }, { "hy", "fonts-noto-core" }, { "am", "fonts-noto-core" },
    { "th", "libthai-data" },           /* word-break dictionary (ui/thbrk.c) */
    { "lo", "fonts-noto-core" }, { "km", "fonts-noto-core" }, { "my", "fonts-noto-core" }, { "bo", "fonts-noto-core" },
    { "dz", "fonts-noto-core" }, { "si", "fonts-noto-core" }, { "mn", "fonts-noto-core" }, { "jv", "fonts-noto-core" },
    { "su", "fonts-noto-core" }, { "ml", "fonts-noto-core" }, { "kn", "fonts-noto-core" }, { "gu", "fonts-noto-core" },
    { "pa", "fonts-noto-core" }, { "or", "fonts-noto-core" }, { "mr", "fonts-noto-core" }, { "ne", "fonts-noto-core" },
};

static int package_exists(const char *pkg) {
    const char *const argv[] = { "/usr/bin/apt-cache", "show", "--no-all-versions", pkg, NULL };
    int p[2];
    if (pipe(p)) return 0;
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(null, 1); dup2(null, 2);
        syscall(SYS_close_range, 3, ~0u, 0);
        char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8", NULL };
        execve(argv[0], (char *const *)argv, env);
        _exit(127);
    }
    close(p[0]);
    close(p[1]);
    int st = 0;
    if (pid > 0) waitpid(pid, &st, 0);
    return pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* ll or ll_CC ("pt" or "pt_BR"): packages are tried with and without region (hunspell-pt-br). */
static void act_langpack(const char *arg) {
    size_t n = strlen(arg);
    int ok = n >= 2 && n <= 6;
    char ll[4] = "", cc[3] = "";
    size_t i = 0;
    for (; ok && i < n && arg[i] != '_'; i++) { if (arg[i] < 'a' || arg[i] > 'z' || i >= 3) ok = 0; else ll[i] = arg[i]; }
    if (ok && i < 2) ok = 0;
    if (ok && i < n) {
        if (n - i != 3 || arg[i + 1] < 'A' || arg[i + 1] > 'Z' || arg[i + 2] < 'A' || arg[i + 2] > 'Z') ok = 0;
        else { cc[0] = (char)(arg[i + 1] - 'A' + 'a'); cc[1] = (char)(arg[i + 2] - 'A' + 'a'); }
    }
    if (!ok) { admin_err("lang.invalidCode", "code", arg, NULL); return; }
    static const char *const patterns[] = { "manpages-%s", "hunspell-%s", "aspell-%s", "hyphen-%s", "mythes-%s" };
    char names[24][48];
    int count = 0;
    for (size_t k = 0; k < sizeof patterns / sizeof *patterns && count < 20; k++) {
        char full[16];
        if (cc[0]) {
            snprintf(full, sizeof full, "%s-%s", ll, cc);
            snprintf(names[count], sizeof names[count], patterns[k], full);
            if (package_exists(names[count])) { count++; continue; }
        }
        snprintf(names[count], sizeof names[count], patterns[k], ll);
        if (package_exists(names[count])) count++;
    }
    for (size_t i = 0; i < sizeof script_fonts / sizeof *script_fonts; i++)
        if (!strcmp(script_fonts[i].lang, ll) && count < 20) snprintf(names[count++], sizeof names[0], "%s", script_fonts[i].pkgs);
    if (!count) { admin_ok("lang.noDebianPackages", "lang", arg, NULL); return; }
    const char *argv[28] = { "/usr/bin/apt-get", "install", "-y", "--no-install-recommends" };
    int a = 4;
    for (int i = 0; i < count; i++) { argv[a++] = names[i]; admin_log("lang.package", "name", names[i], NULL); }
    argv[a] = NULL;
    int r = admin_run(argv);
    if (r == 0) admin_ok("lang.installed", "count", lerr_int(count), "lang", arg, NULL);
    else admin_err("sys.programFailed", "program", "apt-get", "code", lerr_int(r), NULL);
}

#endif /* LORD_NETD */

/* Takes the complete requests "<id> <action> <length>\n<argument>" out of the buffer and calls fn
 * for each (the argument in its own NUL-terminated memory; fn does not free it). */
typedef void (*ReqFn)(int id, const char *action, char *arg, size_t len, void *ctx);
static void parse_requests(char *in, size_t *inlen, ReqFn fn, void *ctx) {
    for (;;) {
        char *nl = memchr(in, '\n', *inlen < 160 ? *inlen : 160);
        if (!nl) { if (*inlen >= 160) *inlen = 0; break; }      /* invalid header: dropped */
        *nl = 0;
        int id = 0;
        char action[64] = "";
        unsigned long len = 0;
        int ok = sscanf(in, "%d %63s %lu", &id, action, &len) == 3 && id > 0 && len <= ADMIN_ARG_MAX;
        size_t head = (size_t)(nl - in) + 1;
        if (!ok) { memmove(in, in + head, *inlen - head); *inlen -= head; continue; }
        if (*inlen < head + len) { *nl = '\n'; break; }          /* argument not complete yet */
        char *arg = malloc(len + 1);
        if (arg) { memcpy(arg, in + head, len); arg[len] = 0; }
        memmove(in, in + head + len, *inlen - head - len);
        *inlen -= head + len;
        if (!arg) continue;
        fn(id, action, arg, len, ctx);
        explicit_bzero(arg, len);           /* may carry a password (user-add, user-passwd, Wi-Fi) */
        free(arg);
    }
}

/* Runs action a (already authorized) with req: in a child if A_FORK (which keeps only its output
 * pipe). client: in lord-netd, who gets its replies (-1 in a session's helper). */
static void run_action(size_t a, AdminReq *req, int id, const char *arg, size_t len, int client) {
    if (actions[a].flags & A_FORK) {
        Kid *k = kid_new();
        int p[2];
        if (!k || pipe2(p, O_CLOEXEC)) { admin_err("admin.tooMany", NULL); return; }
        pid_t pid = fork();
        if (pid == 0) {
            close(p[0]);
            /* none of lord-server's or lord-netd's client sockets: only the pipe */
            int keep = fcntl(p[1], F_DUPFD, 3);
            for (int f = 3; f < 1024; f++) if (f != keep) close(f);
            out_fd = keep;
            netd_main = 0;
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            actions[a].fn(req, arg, len);
            _exit(0);
        }
        close(p[1]);
        if (pid < 0) { close(p[0]); admin_err("admin.forkFailed", NULL); }
        else *k = (Kid){ .pid = pid, .fd = p[0], .id = id, .client = client };
    } else actions[a].fn(req, arg, len);
}

static size_t find_action(const char *name) {
    size_t a = 0;
    while (a < N_ACTIONS && strcmp(actions[a].name, name)) a++;
    return a;
}

/* ---- the system network: lord-netd (docs/NETWORK.md, "The network belongs to the system") ----
 *
 * The network doesn't belong to a session: lord-netd brings it up, a small root process that
 * lord-sessiond starts and supervises at boot (before anyone logs in). It serves only the net-*
 * actions of this same table, on /run/lordos/netd.sock (0600, root only: the sessions' helpers).
 * Each helper introduces itself with "HELLO <user>" and lord-netd works out again whether that
 * user is an administrator. Replies go to the helper that asked; events (id 0) go to all of them.
 * The resident processes (DHCP, wpa_supplicant, VPN) are lord-netd's children, so they outlive
 * sessions logging in and out. Without lord-netd (a standalone lord-server, in development) the
 * session's helper runs the network itself, as before. */
#define NETD_SOCK  "/run/lordos/netd.sock"
#define NETD_MARK  "/run/lordos/netd.enabled"     /* created by lord-sessiond when it starts lord-netd */
#define NETD_VPNS  "/run/lordos/netd-vpns"        /* VPNs that are up: restored if lord-netd restarts */

/* session helper side */
static int netd_fd = -1;
static char netd_user[64];
static char netd_buf[8192];
static size_t netd_len;
static int netd_pending[64];
static int netd_npending;

static int netd_connect(void) {
    if (netd_fd >= 0) return 0;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", NETD_SOCK);
    struct ucred cr;
    socklen_t cl = sizeof cr;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) || cr.uid != 0) {
        close(fd);
        return -1;
    }
    char hello[96];
    int n = snprintf(hello, sizeof hello, "HELLO %s\n", netd_user);
    write_fd_all(fd, hello, (size_t)n);
    netd_fd = fd;
    netd_len = 0;
    return 0;
}

static void netd_lost(void) {
    if (netd_fd >= 0) close(netd_fd);
    netd_fd = -1;
    for (int i = 0; i < netd_npending; i++) put_line(netd_pending[i], "ERR", "netd.restarted");
    netd_npending = 0;
}

/* What lord-netd sends goes to lord-server unchanged (the ids are this session's). */
static void netd_read(void) {
    ssize_t r = read(netd_fd, netd_buf + netd_len, sizeof netd_buf - 1 - netd_len);
    if (r < 0 && (errno == EINTR || errno == EAGAIN)) return;
    if (r <= 0) { netd_lost(); return; }
    netd_len += (size_t)r;
    char *nl;
    while ((nl = memchr(netd_buf, '\n', netd_len))) {
        size_t used = (size_t)(nl - netd_buf) + 1;
        *nl = 0;
        int id = atoi(netd_buf);
        char *sp = strchr(netd_buf, ' ');
        if (id > 0 && sp && (!strncmp(sp + 1, "OK", 2) || !strncmp(sp + 1, "ERR", 3)))
            for (int i = 0; i < netd_npending; i++)
                if (netd_pending[i] == id) { netd_pending[i] = netd_pending[--netd_npending]; break; }
        *nl = '\n';
        write_fd_all(out_fd, netd_buf, used);
        memmove(netd_buf, netd_buf + used, netd_len - used);
        netd_len -= used;
    }
    if (netd_len >= sizeof netd_buf - 1) netd_len = 0;
}

/* 1: no lord-netd (the session runs the network); 0: forwarded; -1: it should be there and isn't. */
static int netd_forward(int id, const char *action, const char *arg, size_t len) {
    if (netd_connect()) return access(NETD_MARK, F_OK) == 0 ? -1 : 1;
    char head[128];
    int n = snprintf(head, sizeof head, "%d %s %zu\n", id, action, len);
    write_fd_all(netd_fd, head, (size_t)n);
    write_fd_all(netd_fd, arg, len);
    if (netd_npending < (int)(sizeof netd_pending / sizeof *netd_pending)) netd_pending[netd_npending++] = id;
    return 0;
}

typedef struct { AdminReq *req; int allowed; const char *user; } HelperCtx;

static void helper_request(int id, const char *action, char *arg, size_t len, void *ctx) {
    HelperCtx *h = ctx;
    cur_id = id;
    h->req->id = id;
    size_t a = find_action(action);
    int fw = 1;
    if (a == N_ACTIONS) admin_err("admin.unknownAction", NULL);
    else if (strlen(arg) != len) admin_err("admin.invalidArgument", NULL);
    else if (!h->allowed && !(actions[a].flags & A_ANYONE)) admin_err("admin.notAdmin", "user", h->user, NULL);
    else if (!strncmp(action, "net-", 4) && (fw = netd_forward(id, action, arg, len)) <= 0) {
        if (fw < 0) admin_err("netd.unavailable", NULL);
    } else run_action(a, h->req, id, arg, len, -1);
    cur_id = 0;
}

static void helper(int fd, const char *user) {
    out_fd = fd;
    prctl(PR_SET_NAME, "lord-admin");
    prctl(PR_SET_PDEATHSIG, SIGTERM);       /* if lord-server dies, the helper does too */
    signal(SIGPIPE, SIG_IGN);
    umask(022);
    struct passwd *pw = getpwnam(user);
    if (!pw) _exit(1);
    char home[PATH_MAX], uname[64];
    snprintf(home, sizeof home, "%s", pw->pw_dir);
    snprintf(uname, sizeof uname, "%s", user);
    snprintf(netd_user, sizeof netd_user, "%s", user);
    int allowed = is_admin(user);
    AdminReq req = { .user = uname, .home = home, .uid = pw->pw_uid, .gid = pw->pw_gid, .admin = allowed };
    HelperCtx hc = { &req, allowed, uname };
    char *in = malloc(ADMIN_ARG_MAX + 512);
    size_t inlen = 0;
    if (!in) _exit(1);
    /* lord-netd: connect now (to receive network events) and reconnect when lord-sessiond restarts
     * it (the socket appears again: inotify, no polling) */
    netd_connect();
    int ino = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (ino >= 0 && inotify_add_watch(ino, "/run/lordos", IN_CREATE | IN_MOVED_TO) < 0) { close(ino); ino = -1; }

    for (;;) {
        struct pollfd pfd[MAX_KIDS + 3];
        Kid *who[MAX_KIDS + 3];
        int n = 0;
        pfd[n++] = (struct pollfd){ fd, POLLIN, 0 };
        pfd[n++] = (struct pollfd){ netd_fd, POLLIN, 0 };
        pfd[n++] = (struct pollfd){ ino, POLLIN, 0 };
        for (int i = 0; i < MAX_KIDS; i++)
            if (kids[i].pid) { who[n] = &kids[i]; pfd[n++] = (struct pollfd){ kids[i].fd, POLLIN, 0 }; }
        if (poll(pfd, (nfds_t)n, -1) < 0) { if (errno == EINTR) continue; break; }
        for (int i = 3; i < n; i++) if (pfd[i].revents) kid_read(who[i]);
        if (pfd[1].fd >= 0 && pfd[1].revents) netd_read();
        if (pfd[2].fd >= 0 && pfd[2].revents) {
            char ev[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
            int seen = 0;
            for (ssize_t r; (r = read(ino, ev, sizeof ev)) > 0;)
                for (char *p = ev; p < ev + r; p += sizeof(struct inotify_event) + ((struct inotify_event *)p)->len) {
                    struct inotify_event *e = (struct inotify_event *)p;
                    if (e->len && !strcmp(e->name, "netd.sock")) seen = 1;
                }
            if (seen && netd_fd < 0) netd_connect();
        }
        if (!pfd[0].revents) continue;

        ssize_t r = read(fd, in + inlen, ADMIN_ARG_MAX + 511 - inlen);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        inlen += (size_t)r;
        parse_requests(in, &inlen, helper_request, &hc);
    }
    for (int i = 0; i < MAX_KIDS; i++) if (kids[i].pid) kill(kids[i].pid, SIGTERM);
    _exit(0);
}

/* lord-netd side */
#define NETD_MAX_CLIENTS 32

typedef struct {
    int fd;                             /* -1: free */
    int hello;                          /* has said who it is */
    char user[64], home[PATH_MAX];
    AdminReq req;
    char *in;
    size_t inlen;
} NetdClient;

static NetdClient clients[NETD_MAX_CLIENTS];

static void netd_route(int id, const char *line, size_t n) {
    if (id == 0) {
        /* event: to every session, and to the log (the journal, through lord-sessiond) */
        const char *t = memchr(line, ' ', n);
        if (t) { t = memchr(t + 1, ' ', n - (size_t)(t + 1 - line)); }
        if (t) fprintf(stderr, "lord-netd: %.*s", (int)(n - (size_t)(t + 1 - line)), t + 1);
        for (int i = 0; i < NETD_MAX_CLIENTS; i++) if (clients[i].fd >= 0 && clients[i].hello) write_fd_all(clients[i].fd, line, n);
        return;
    }
    if (cur_client >= 0 && cur_client < NETD_MAX_CLIENTS && clients[cur_client].fd >= 0) write_fd_all(clients[cur_client].fd, line, n);
}

#ifdef LORD_NETD
static void netd_drop(int c) {
    close(clients[c].fd);
    clients[c].fd = -1;
    free(clients[c].in);
    clients[c].in = NULL;
    for (int i = 0; i < MAX_KIDS; i++) if (kids[i].client == c) kids[i].client = -1;     /* its reply has nowhere to go now */
}

/* List of VPNs that are up (one per line): to reconnect them if lord-netd restarts. */
static void netd_vpn_note(const char *name, int up) {
    char *t = NULL, line[64];
    size_t tl = 0;
    FILE *m = open_memstream(&t, &tl);
    FILE *f = fopen(NETD_VPNS, "r");
    while (m && f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] && strcmp(line, name)) fprintf(m, "%s\n", line);
    }
    if (f) fclose(f);
    if (!m) return;
    if (up) fprintf(m, "%s\n", name);
    fclose(m);
    FILE *o = fopen(NETD_VPNS ".new", "w");
    if (o) { fwrite(t, 1, tl, o); fclose(o); rename(NETD_VPNS ".new", NETD_VPNS); }
    free(t);
}

static void netd_request(int id, const char *action, char *arg, size_t len, void *ctx) {
    NetdClient *c = ctx;
    cur_client = (int)(c - clients);
    cur_id = id;
    c->req.id = id;
    size_t a = find_action(action);
    if (a == N_ACTIONS || strncmp(action, "net-", 4)) admin_err("admin.unknownAction", NULL);
    else if (strlen(arg) != len) admin_err("admin.invalidArgument", NULL);
    else if (!c->req.admin && !(actions[a].flags & A_ANYONE)) admin_err("admin.notAdmin", "user", c->user, NULL);
    else if (!strcmp(action, "net-restore")) admin_ok("net.restoreSystem", NULL);
    else {
        run_action(a, &c->req, id, arg, len, cur_client);
        if (!strcmp(action, "net-vpn-up")) netd_vpn_note(arg, 1);
        else if (!strcmp(action, "net-vpn-down") || !strcmp(action, "net-vpn-delete")) netd_vpn_note(arg, 0);
    }
    cur_id = 0;
    cur_client = -1;
}

static int valid_user(const char *u) {
    size_t n = strlen(u);
    if (!n || n > 32) return 0;
    for (const char *p = u; *p; p++) if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p) && !strchr("_-", *p)) return 0;
    return 1;
}

static void netd_hello(NetdClient *c) {
    char *nl = memchr(c->in, '\n', c->inlen < 80 ? c->inlen : 80);
    if (!nl) { if (c->inlen >= 80) c->inlen = 0; return; }
    *nl = 0;
    char user[40] = "";
    struct passwd *pw = NULL;
    if (sscanf(c->in, "HELLO %39s", user) == 1 && valid_user(user)) pw = getpwnam(user);
    size_t head = (size_t)(nl - c->in) + 1;
    memmove(c->in, c->in + head, c->inlen - head);
    c->inlen -= head;
    if (!pw) return;
    snprintf(c->user, sizeof c->user, "%s", user);
    snprintf(c->home, sizeof c->home, "%s", pw->pw_dir);
    c->req = (AdminReq){ .user = c->user, .home = c->home, .uid = pw->pw_uid, .gid = pw->pw_gid, .admin = is_admin(user) };
    c->hello = 1;
}

int admin_netd_serve(void) {
    signal(SIGPIPE, SIG_IGN);
    umask(022);
    prctl(PR_SET_NAME, "lord-netd");
    mkdir("/run/lordos", 0755);
    /* only one: if one is already running (it outlived a lord-sessiond restart), this one exits */
    int lk = open("/run/lordos/netd.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lk < 0 || flock(lk, LOCK_EX | LOCK_NB)) { fprintf(stderr, "lord-netd: another instance is already running\n"); return 3; }
    FILE *pf = fopen("/run/lordos/netd.pid", "w");
    if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
    for (int i = 0; i < NETD_MAX_CLIENTS; i++) clients[i].fd = -1;

    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s.new", NETD_SOCK);
    unlink(sa.sun_path);
    mode_t old = umask(077);            /* 0600: root only (the sessions' helpers) */
    int bad = lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof sa) || listen(lfd, 16);
    umask(old);
    if (bad || rename(sa.sun_path, NETD_SOCK)) { fprintf(stderr, "lord-netd: cannot open %s: %s\n", NETD_SOCK, strerror(errno)); return 1; }

    /* the network at boot: saved configs, DHCP, known Wi-Fi and DNS */
    netd_main = 1;
    static char sys_user[] = "root", sys_home[] = "/root";
    AdminReq sys = { .user = sys_user, .home = sys_home, .uid = 0, .gid = 0, .admin = 1 };
    cur_client = -1;
    net_act_restore(&sys, "system", 6);
    /* VPNs that were up before lord-netd restarted (not those from before a shutdown) */
    {
        FILE *f = fopen(NETD_VPNS, "r");
        char line[64];
        while (f && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            if (line[0]) net_act_vpn_up(&sys, line, strlen(line));
        }
        if (f) fclose(f);
    }
    fprintf(stderr, "lord-netd: ready (%s)\n", NETD_SOCK);
    int watch = net_watch_open();

    for (;;) {
        struct pollfd pfd[3 + NETD_MAX_CLIENTS + MAX_KIDS];
        int ref[3 + NETD_MAX_CLIENTS + MAX_KIDS];       /* >= 0: client; < 0: -(kid + 1) */
        int n = 0;
        short bus_ev = 0;
        int bus = net_sleep_poll(&bus_ev);              /* logind: sleep and wake (net/sleepbus.c) */
        pfd[n] = (struct pollfd){ lfd, POLLIN, 0 }; ref[n++] = 0;
        pfd[n] = (struct pollfd){ watch, POLLIN, 0 }; ref[n++] = 0;
        pfd[n] = (struct pollfd){ bus, bus_ev, 0 }; ref[n++] = 0;
        for (int i = 0; i < NETD_MAX_CLIENTS; i++)
            if (clients[i].fd >= 0) { pfd[n] = (struct pollfd){ clients[i].fd, POLLIN, 0 }; ref[n++] = i; }
        for (int i = 0; i < MAX_KIDS; i++)
            if (kids[i].pid) { pfd[n] = (struct pollfd){ kids[i].fd, POLLIN, 0 }; ref[n++] = -(i + 1); }
        int tmo = net_watch_timeout(), bt = net_sleep_timeout();
        if (bt >= 0 && (tmo < 0 || bt < tmo)) tmo = bt;
        if (poll(pfd, (nfds_t)n, tmo) < 0) { if (errno == EINTR) continue; return 1; }
        net_sleep_run(pfd[2].revents);
        for (int k = 3; k < n; k++) {
            if (!pfd[k].revents) continue;
            if (ref[k] < 0) { kid_read(&kids[-ref[k] - 1]); continue; }
            NetdClient *c = &clients[ref[k]];
            ssize_t r = read(c->fd, c->in + c->inlen, ADMIN_ARG_MAX + 511 - c->inlen);
            if (r < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (r <= 0) { netd_drop(ref[k]); continue; }
            c->inlen += (size_t)r;
            if (!c->hello) netd_hello(c);
            if (c->hello) parse_requests(c->in, &c->inlen, netd_request, c);
        }
        if (pfd[0].revents & POLLIN) {
            int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
            if (fd < 0) continue;
            struct ucred cr;
            socklen_t cl = sizeof cr;
            int slot = -1;
            for (int i = 0; i < NETD_MAX_CLIENTS && slot < 0; i++) if (clients[i].fd < 0) slot = i;
            char *in = slot >= 0 ? malloc(ADMIN_ARG_MAX + 512) : NULL;
            if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) || cr.uid != 0 || !in) { free(in); close(fd); continue; }
            struct timeval tv = { 2, 0 };           /* a stuck session doesn't block the network */
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            clients[slot] = (NetdClient){ .fd = fd, .in = in };
        }
        net_watch_run(pfd[1].revents != 0);
    }
}
#endif /* LORD_NETD */

/* ---- server side ---- */

static int sock = -1;
static int next_id = 1;
static char rbuf[8192];
static size_t rlen;

int admin_start(const char *user) {
    if (geteuid() != 0 || !user) return -1;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(sv[0]); close(sv[1]); return -1; }
    if (pid == 0) {
        int fd = fcntl(sv[1], F_DUPFD, 3);
        dup2(fd, 3);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); }
        syscall(SYS_close_range, 4, ~0u, 0);
        /* the real-time limit lord-sessiond gave the compositor (par.h) is not the helper's: the
         * processes it drops to another user (the Store's download child) must not inherit it */
        struct rlimit rtp = { 0, 0 };
        setrlimit(RLIMIT_RTPRIO, &rtp);
        helper(3, user);
    }
    close(sv[1]);
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    sock = sv[0];
    return 0;
}

int admin_fd(void) { return sock; }

static int write_all(const char *p, size_t n) {
    while (n) {
        ssize_t w = write(sock, p, n);
        if (w < 0 && (errno == EINTR || errno == EAGAIN)) {
            struct pollfd pf = { sock, POLLOUT, 0 };
            if (errno == EAGAIN && poll(&pf, 1, 2000) <= 0) return -1;
            continue;
        }
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int admin_send(const char *action, const char *arg, size_t len) {
    if (sock < 0 || !admin_known(action) || len > ADMIN_ARG_MAX || memchr(arg, 0, len)) return -1;
    char head[128];
    int id = next_id++;
    if (next_id > 1 << 30) next_id = 1;
    int n = snprintf(head, sizeof head, "%d %s %zu\n", id, action, len);
    if (write_all(head, (size_t)n) || write_all(arg, len)) return -1;
    return id;
}

void admin_dispatch(void (*fn)(int id, const char *kind, const char *text)) {
    for (;;) {
        ssize_t r = read(sock, rbuf + rlen, sizeof rbuf - 1 - rlen);
        if (r <= 0) { if (r == 0) { close(sock); sock = -1; } break; }
        rlen += (size_t)r;
        char *nl;
        while ((nl = memchr(rbuf, '\n', rlen))) {
            *nl = 0;
            int id = atoi(rbuf);
            char *kind = strchr(rbuf, ' ');
            if (kind) {
                kind++;
                char *sp = strchr(kind, ' ');
                if (sp) { *sp = 0; fn(id, kind, sp + 1); } else fn(id, kind, "");
            }
            size_t used = (size_t)(nl - rbuf) + 1;
            memmove(rbuf, nl + 1, rlen - used);
            rlen -= used;
        }
        if (rlen >= sizeof rbuf - 1) rlen = 0;
        if (sock < 0) break;
    }
}
