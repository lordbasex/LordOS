/* fpspawn.c — org.freedesktop.portal.Flatpak served by lord-server (see fpspawn.h; docs/FLATPAK.md, "Sub-sandboxes";
 * AGENTS.md E9).
 *
 * Who asks: the bus gives the caller's pid (GetConnectionUnixProcessID): a Flatpak app reaches the session bus only
 * through its xdg-dbus-proxy, which runs in the app's own root-owned scope (app-lordos-fp-<id>.scope). Only such a
 * caller is served, and only as that app: the new process is `flatpak run` of the same app id, placed in the same
 * scope, entering the namespaces of the same LordOS view (its user, mount, IPC and PID namespaces, taken from the
 * view's first process), with Landlock's scoping applied again and LordOS's never-granted options and current
 * restrictions after whatever the app asked for (fpak_spawn_argv). The app's command and arguments come after the app
 * id, so flatpak never reads them as its own options. Nothing reaches the host: no command outside Flatpak's sandbox,
 * no other app id, no path outside the app's own ~/.var/app/<id>/sandbox, and no custom /app or /usr (usr-fd, app-fd
 * and the expose-fd options are refused).
 *
 * The process tree: lord-server → a waiter (in the scope; it entered the view's namespaces, so its child is born in
 * the view's PID namespace) → `flatpak run` (whose pid is the one the caller gets). The waiter ends the way its child
 * did, and lord-server reports that as SpawnExited to the caller alone. */
#include "fpspawn.h"
#include "fpak.h"
#include "lordlog.h"
#include "sec/appid.h"
#include "sec/cgroup.h"
#include "sec/restrict.h"
#include "sec/sandbox.h"
#include "fw/rules.h"
#include "vault/dbus.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif

extern char **environ;

#define BUS_NAME "org.freedesktop.portal.Flatpak"
#define OBJ      "/org/freedesktop/portal/Flatpak"
#define VERSION  6u
#define SUPPORTS 1u                     /* FLATPAK_SPAWN_SUPPORT_FLAGS_EXPOSE_PIDS */
#define MAX_PENDING 16
#define PENDING_MS 3000
#define MAX_KIDS 128
#define MAX_PER_APP 32

static DConn conn;
static int up;

static struct { DMsg call; uint32_t serial; double since; } pending[MAX_PENDING];
static int npending;

typedef struct {
    int live;
    pid_t waiter, pid;                  /* the waiter (our child) and `flatpak run` (both in our PID namespace) */
    char sender[64], id[FPAK_ID_MAX + 1];
    uint32_t flags;
    int inst_fd;                        /* NOTIFY_START: the read end of --instance-id-fd, or -1 */
    pid_t root;                         /* the view's first process (for the relative pid) */
    int parent_pid;                     /* the caller's instance, in the view's PID namespace (EXPOSE/SHARE_PIDS) */
    char inst[24];
    size_t inst_len;
} Kid;
static Kid kids[MAX_KIDS];

static double mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* ---- reading the caller's processes ---- */

/* A small file through /proc (never following a last link), NUL-terminated. Bytes read, or -1. */
static ssize_t read_small(const char *path, char *out, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t got = 0;
    for (ssize_t r; got < cap - 1 && (r = read(fd, out + got, cap - 1 - got)) > 0;) got += (size_t)r;
    close(fd);
    out[got] = 0;
    return (ssize_t)got;
}

static int cgroup_of(pid_t pid, char *out, size_t cap) {
    char p[64], buf[1024];
    snprintf(p, sizeof p, "/proc/%d/cgroup", (int)pid);
    if (read_small(p, buf, sizeof buf) <= 0) return -1;
    char *l = strstr(buf, "0::");
    if (!l) return -1;
    l += 3;
    char *nl = strchr(l, '\n');
    if (nl) *nl = 0;
    return snprintf(out, cap, "%s", l) < (int)cap ? 0 : -1;
}

static pid_t ppid_of(pid_t pid) {
    char p[64], buf[2048];
    snprintf(p, sizeof p, "/proc/%d/status", (int)pid);
    if (read_small(p, buf, sizeof buf) <= 0) return -1;
    char *l = strstr(buf, "\nPPid:");
    return l ? (pid_t)atoi(l + 6) : -1;
}

/* The NSpid fields of pid (outermost first). How many, or -1. */
static int nspids(pid_t pid, int *out, int max) {
    char p[64], buf[2048];
    snprintf(p, sizeof p, "/proc/%d/status", (int)pid);
    if (read_small(p, buf, sizeof buf) <= 0) return -1;
    char *l = strstr(buf, "\nNSpid:");
    if (!l) return -1;
    l += 7;
    int n = 0;
    for (char *end; n < max; l = end) {
        long v = strtol(l, &end, 10);
        if (end == l) break;
        out[n++] = (int)v;
    }
    return n;
}

/* The first process of the caller's view: its topmost ancestor still in the same scope. */
static pid_t view_root(pid_t pid) {
    char cg[512], pcg[512];
    if (cgroup_of(pid, cg, sizeof cg)) return -1;
    pid_t cur = pid;
    for (int i = 0; i < 64; i++) {
        pid_t pp = ppid_of(cur);
        if (pp <= 1 || cgroup_of(pp, pcg, sizeof pcg) || strcmp(cg, pcg)) return cur;
        cur = pp;
    }
    return -1;
}

/* key=value from a [group] of a small key file (the caller's /.flatpak-info) */
static int keyfile_get(const char *text, const char *group, const char *key, char *out, size_t cap) {
    char hdr[64];
    snprintf(hdr, sizeof hdr, "[%s]", group);
    const char *g = strstr(text, hdr);
    if (!g || (g != text && g[-1] != '\n')) return -1;
    size_t kl = strlen(key);
    for (const char *l = strchr(g, '\n'); l && l[1] && l[1] != '['; l = strchr(l + 1, '\n')) {
        if (strncmp(l + 1, key, kl) || l[1 + kl] != '=') continue;
        const char *v = l + 2 + kl, *e = strchr(v, '\n');
        size_t n = e ? (size_t)(e - v) : strlen(v);
        if (n >= cap) return -1;
        memcpy(out, v, n);
        out[n] = 0;
        return 0;
    }
    return -1;
}

static int digits_ok(const char *s) {
    if (!s || !s[0] || strlen(s) > 20) return 0;
    for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}

/* "child-pid": N in an instance's bwrapinfo.json, read inside the view */
static int instance_child_pid(pid_t root, const char *instance) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char path[PATH_MAX], buf[4096];
    if (!rt || rt[0] != '/' || !digits_ok(instance)) return -1;
    snprintf(path, sizeof path, "/proc/%d/root%s/.flatpak/%s/bwrapinfo.json", (int)root, rt, instance);
    if (read_small(path, buf, sizeof buf) <= 0) return -1;
    char *k = strstr(buf, "\"child-pid\"");
    if (!k) return -1;
    k = strchr(k, ':');
    int v = k ? atoi(k + 1) : 0;
    return v > 0 ? v : -1;
}

/* ---- replies ---- */

static void reply_error(const DMsg *m, const char *name, const char *text) { dbus_error(&conn, m, name, text); }

static void send_to(const char *dest, const char *member, const char *sig, const DBuf *b) {
    DHdr h = { .type = DBUS_SIGNAL, .path = OBJ, .iface = BUS_NAME, .member = member, .dest = dest, .sig = sig };
    dbus_send(&conn, &h, b);
}

static void match(const char *sender, int add) {
    char rule[256];
    snprintf(rule, sizeof rule, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
             "member='NameOwnerChanged',arg0='%s'", sender);
    DBuf b;
    dw_init(&b);
    dw_str(&b, rule);
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus",
               .member = add ? "AddMatch" : "RemoveMatch", .dest = "org.freedesktop.DBus", .sig = "s" };
    dbus_send(&conn, &h, &b);
    dw_free(&b);
}

/* ---- Spawn ---- */

typedef struct {
    char cwd[FPAK_SPAWN_STR_MAX];
    const char *args[FPAK_SPAWN_ARGS_MAX + 1], *env[FPAK_SPAWN_ENV_MAX + 1], *unset[FPAK_SPAWN_ENV_MAX + 1];
    const char *expose[FPAK_SPAWN_EXPOSE_MAX + 1], *expose_ro[FPAK_SPAWN_EXPOSE_MAX + 1];
    char *store;                        /* the argument strings, NUL-separated */
    size_t used, cap;
    uint32_t target[DBUS_MAX_FDS], handle[DBUS_MAX_FDS];
    int nfd;
    uint32_t flags, sandbox_flags;
    int has_cwd;
} Req;

static const char *keep(Req *q, const uint8_t *p, size_t n) {
    if (memchr(p, 0, n) || q->used + n + 1 > q->cap) return NULL;
    char *s = q->store + q->used;
    memcpy(s, p, n);
    s[n] = 0;
    q->used += n + 1;
    return s;
}

/* a bytestring argument (ay, with its trailing NUL) */
static const char *keep_bytes(Req *q, DRd *r) {
    uint32_t n = 0;
    const uint8_t *p = dr_bytes(r, &n);
    if (r->err || !p || !n || p[n - 1]) return NULL;
    return keep(q, p, n - 1);
}

static int read_strv(Req *q, DRd *r, const char **out, int max) {
    size_t end = dr_arr(r, 4);
    int n = 0;
    while (!r->err && dr_more(r, end)) {
        uint32_t l = 0;
        const char *s = dr_str(r, &l);
        if (r->err || !s || n >= max || !(out[n++] = keep(q, (const uint8_t *)s, l))) return -1;
    }
    out[n] = NULL;
    return r->err ? -1 : 0;
}

/* Spawn(ay cwd, aay argv, a{uh} fds, a{ss} envs, u flags, a{sv} options). 0 read, -1 bad, -2 unsupported option. */
static int read_spawn(const DMsg *m, Req *q) {
    DRd r;
    dr_init(&r, m);
    const char *cwd = keep_bytes(q, &r);
    if (!cwd) return -1;
    if (cwd[0]) { snprintf(q->cwd, sizeof q->cwd, "%s", cwd); q->has_cwd = 1; }
    size_t end = dr_arr(&r, 4);
    int na = 0;
    while (!r.err && dr_more(&r, end)) {
        const char *a = keep_bytes(q, &r);
        if (!a || na >= FPAK_SPAWN_ARGS_MAX) return -1;
        q->args[na++] = a;
    }
    q->args[na] = NULL;
    end = dr_arr(&r, 8);
    while (!r.err && dr_more(&r, end)) {
        dr_struct(&r);
        uint32_t t = dr_u32(&r), h = dr_u32(&r);
        if (r.err || q->nfd >= DBUS_MAX_FDS || t > 1023 || (int)h >= m->n_fds) return -1;
        for (int i = 0; i < q->nfd; i++) if (q->target[i] == t) return -1;
        q->target[q->nfd] = t;
        q->handle[q->nfd++] = h;
    }
    end = dr_arr(&r, 8);
    int ne = 0;
    while (!r.err && dr_more(&r, end)) {
        dr_struct(&r);
        uint32_t kl = 0, vl = 0;
        const char *k = dr_str(&r, &kl), *v = dr_str(&r, &vl);
        if (r.err || !k || !v || ne >= FPAK_SPAWN_ENV_MAX || kl + vl + 2 > FPAK_SPAWN_STR_MAX) return -1;
        char kv[FPAK_SPAWN_STR_MAX];
        snprintf(kv, sizeof kv, "%s=%s", k, v);
        if (!(q->env[ne++] = keep(q, (const uint8_t *)kv, strlen(kv)))) return -1;
    }
    q->env[ne] = NULL;
    q->flags = dr_u32(&r);
    q->unset[0] = q->expose[0] = q->expose_ro[0] = NULL;
    end = dr_arr(&r, 8);
    while (!r.err && dr_more(&r, end)) {
        dr_struct(&r);
        const char *k = dr_str(&r, NULL), *sig = dr_variant(&r);
        if (r.err || !k || !sig) return -1;
        if (!strcmp(k, "sandbox-expose") && !strcmp(sig, "as")) { if (read_strv(q, &r, q->expose, FPAK_SPAWN_EXPOSE_MAX)) return -1; }
        else if (!strcmp(k, "sandbox-expose-ro") && !strcmp(sig, "as")) { if (read_strv(q, &r, q->expose_ro, FPAK_SPAWN_EXPOSE_MAX)) return -1; }
        else if (!strcmp(k, "unset-env") && !strcmp(sig, "as")) { if (read_strv(q, &r, q->unset, FPAK_SPAWN_ENV_MAX)) return -1; }
        else if (!strcmp(k, "sandbox-flags") && !strcmp(sig, "u")) q->sandbox_flags = dr_u32(&r);
        else return -2;                 /* usr-fd, app-fd, sandbox-expose-fd(-ro) and anything else: not served */
    }
    return r.err ? -1 : 0;
}

static int count_app(const char *id) {
    int n = 0;
    for (int i = 0; i < MAX_KIDS; i++) n += kids[i].live && !strcmp(kids[i].id, id);
    return n;
}

/* The waiter: in the app's scope and view, starts `flatpak run` and ends as it did. Never returns. */
static void waiter(const char *scope, pid_t root, char *const argv[], char *const envp[], const Req *q, const DMsg *m,
                   int inst_w, int report_w, int logfd) {
    setsid();
    prctl(PR_SET_NAME, "lordos-spawn");          /* in the Monitor, under the app: not "lord-server" */
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    for (int s = 1; s < 32; s++) signal(s, SIG_DFL);
    if (app_scope_enter(scope) && getenv("LORDOS_SESSIOND")) _exit(121);
    /* the view's namespaces, opened before anything else changes */
    static const char *const ns[] = { "user", "ipc", "mnt", "pid" };
    static const int nst[] = { CLONE_NEWUSER, CLONE_NEWIPC, CLONE_NEWNS, CLONE_NEWPID };
    int nsfd[4];
    char p[64];
    /* every fd this needs goes above 1100 first (close-on-exec), so placing the app's fds at the numbers it asked
     * for (any of 0-1023) can't overwrite one of them; everything inherited closes at exec */
    syscall(SYS_close_range, 3, ~0u, CLOSE_RANGE_CLOEXEC);
    /* room above 1100 (the usual soft limit is 1024); put back before flatpak starts */
    struct rlimit rl0, rl;
    if (getrlimit(RLIMIT_NOFILE, &rl0)) _exit(124);
    rl = rl0;
    if (rl.rlim_cur < 1200) {
        if (rl.rlim_max < 1200) _exit(124);
        rl.rlim_cur = rl.rlim_max < 4096 ? rl.rlim_max : 4096;
        if (setrlimit(RLIMIT_NOFILE, &rl)) _exit(124);
    }
#define HIGH(fd) ((fd) >= 0 ? fcntl((fd), F_DUPFD_CLOEXEC, 1100) : -1)
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof p, "/proc/%d/ns/%s", (int)root, ns[i]);
        int o = open(p, O_RDONLY | O_CLOEXEC);
        if ((nsfd[i] = HIGH(o)) < 0) _exit(122);
        close(o);
    }
    int src[DBUS_MAX_FDS];
    for (int i = 0; i < q->nfd; i++) if ((src[i] = HIGH(m->fds[q->handle[i]])) < 0) _exit(126);
    int inst_hi = HIGH(inst_w), log_hi = HIGH(logfd), report = HIGH(report_w);
    int o = open("/dev/null", O_RDWR | O_CLOEXEC), null = HIGH(o);
    if (o >= 0) close(o);
#undef HIGH
    if (report < 0) _exit(126);
    int has[3] = { 0, 0, 0 };
    for (int i = 0; i < q->nfd; i++) {
        if (dup2(src[i], (int)q->target[i]) < 0) _exit(126);
        if (q->target[i] < 3) has[q->target[i]] = 1;
    }
    if (!has[0] && null >= 0) dup2(null, 0);
    for (int k = 1; k < 3; k++) if (!has[k]) dup2(log_hi >= 0 ? log_hi : null, k);
    if (inst_hi >= 0) {
        /* --instance-id-fd's number (fpak_spawn_argv wrote it): the lowest one the app didn't ask for */
        int n = 3;
        for (int again = 1; again;) { again = 0; for (int i = 0; i < q->nfd; i++) if ((int)q->target[i] == n) { n++; again = 1; } }
        if (dup2(inst_hi, n) < 0) _exit(126);
    }
    for (int i = 0; i < 4; i++) if (setns(nsfd[i], nst[i])) _exit(110 + i);   /* user, ipc, mnt, pid */
    if (chdir("/")) {}
    pid_t kid = fork();                  /* born in the view's PID namespace */
    if (kid < 0) _exit(123);
    if (kid == 0) {
        setpgid(0, 0);
        if (setrlimit(RLIMIT_NOFILE, &rl0)) _exit(126);
        if (sandbox_apply_scope()) _exit(126);      /* Landlock's scoping again, and no_new_privs */
        execve(FPAK_FLATPAK, argv, envp);
        _exit(127);
    }
    if (write(report, &kid, sizeof kid) != (ssize_t)sizeof kid) {}
    int st = 0;
    while (waitpid(kid, &st, 0) < 0 && errno == EINTR) {}
    if (WIFSIGNALED(st)) { signal(WTERMSIG(st), SIG_DFL); kill(getpid(), WTERMSIG(st)); }
    _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 126);
}

/* the environment of `flatpak run`: lord-server's, as the launcher gives it to a Flatpak app */
static int make_env(char **out, int max, int bus, int x11) {
    int n = 0;
    for (char **e = environ; *e && n < max - 1; e++) {
        if (!strncmp(*e, "XDG_ACTIVATION_TOKEN=", 21) || !strncmp(*e, "LORDOS_", 7)) continue;
        if (!bus && !strncmp(*e, "DBUS_SESSION_BUS_ADDRESS=", 25)) continue;
        if (!x11 && (!strncmp(*e, "DISPLAY=", 8) || !strncmp(*e, "XAUTHORITY=", 11))) continue;
        out[n++] = *e;
    }
    out[n] = NULL;
    return n;
}

static void do_spawn(DMsg *m, uint32_t caller) {
    char app[FW_APP_MAX], id[FPAK_ID_MAX + 1];
    if (!caller || appid_of_pid(caller, app, sizeof app) != 0 || strncmp(app, "flatpak:", 8) || !fpak_id_ok(app + 8)) {
        reply_error(m, "org.freedesktop.DBus.Error.AccessDenied", "only a Flatpak app LordOS started may spawn");
        return;
    }
    memcpy(id, app + 8, strlen(app + 8) + 1);             /* fpak_id_ok: at most FPAK_ID_MAX bytes */
    if (count_app(id) >= MAX_PER_APP) { reply_error(m, "org.freedesktop.DBus.Error.LimitsExceeded", "too many processes"); return; }
    /* who it says it is, from its own /.flatpak-info, must be the app its scope says */
    char path[96], info[8192], name[FPAK_ID_MAX + 2], iid[24] = "";
    snprintf(path, sizeof path, "/proc/%u/root/.flatpak-info", caller);
    if (read_small(path, info, sizeof info) <= 0 || keyfile_get(info, "Application", "name", name, sizeof name) || strcmp(name, id) ||
        keyfile_get(info, "Instance", "instance-id", iid, sizeof iid) || !digits_ok(iid)) {
        reply_error(m, "org.freedesktop.DBus.Error.AccessDenied", "not a Flatpak instance");
        return;
    }
    pid_t root = view_root((pid_t)caller);
    if (root <= 0) { reply_error(m, "org.freedesktop.DBus.Error.Failed", "the app's view is gone"); return; }
    Req *q = calloc(1, sizeof *q);
    if (!q || !(q->store = malloc(q->cap = 256 * 1024))) { free(q); reply_error(m, "org.freedesktop.DBus.Error.NoMemory", "no memory"); return; }
    int rr = read_spawn(m, q);
    FpakSpawn sp = { .flags = q->flags, .sandbox_flags = q->sandbox_flags, .cwd = q->has_cwd ? q->cwd : NULL, .args = q->args,
                     .env = q->env, .unset_env = q->unset, .expose = q->expose, .expose_ro = q->expose_ro, .instance_fd = -1 };
    if (!rr && (q->flags & (FPAK_SPAWN_EXPOSE_PIDS | FPAK_SPAWN_SHARE_PIDS))) sp.parent_pid = instance_child_pid(root, iid);
    if (!rr && (q->flags & FPAK_SPAWN_NOTIFY_START)) {
        int n = 3;
        for (int again = 1; again;) { again = 0; for (int i = 0; i < q->nfd; i++) if ((int)q->target[i] == n) { n++; again = 1; } }
        sp.instance_fd = n;
    }
    /* the app's installation and LordOS's restrictions now */
    char arch[32], branch[FPAK_BRANCH_MAX], root_dir[PATH_MAX];
    const char *home = getenv("HOME");
    int found = -1;
    if (home && home[0] == '/' && snprintf(root_dir, sizeof root_dir, "%s/.local/share/flatpak", home) < (int)sizeof root_dir)
        found = fpak_installed(root_dir, id, arch, sizeof arch, branch, sizeof branch, NULL, 0);
    if (found) found = fpak_installed("/var/lib/flatpak", id, arch, sizeof arch, branch, sizeof branch, NULL, 0);
    static const uint32_t flags[] = { FW_R_NO_NETWORK, FW_R_NO_USB, FW_R_NO_BLUETOOTH, FW_R_NO_FILES, FW_R_NO_MIC, FW_R_NO_CAMERA };
    uint32_t restricted = 0;
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) if (restrict_blocks(app, flags[i])) restricted |= flags[i];
    int dev = (restricted & (FW_R_NO_FILES | FW_R_NO_MIC | FW_R_NO_CAMERA | FW_R_NO_USB | FW_R_NO_BLUETOOTH)) != 0;
    char xs[PATH_MAX];
    const char *d = getenv("DISPLAY");
    struct stat st;
    int x11 = !dev && d && d[0] == ':' && snprintf(xs, sizeof xs, "/proc/%d/root/tmp/.X11-unix/X%s", (int)root, d + 1) < (int)sizeof xs &&
              !lstat(xs, &st) && S_ISSOCK(st.st_mode);
    FpakRun run = { .id = id, .arch = arch, .branch = branch, .restricted = restricted, .home = home, .x11 = x11 };
    static char *argv[FPAK_SPAWN_ARGV_MAX], *envp[512];
    char *abuf = malloc(256 * 1024);
    int argc = rr || found || !abuf ? -1 : fpak_spawn_argv(&run, &sp, argv, abuf, 256 * 1024);
    if (argc < 0 || ((q->flags & (FPAK_SPAWN_EXPOSE_PIDS | FPAK_SPAWN_SHARE_PIDS)) && sp.parent_pid <= 0)) {
        reply_error(m, rr == -2 ? "org.freedesktop.DBus.Error.NotSupported" : "org.freedesktop.DBus.Error.InvalidArgs",
                    rr == -2 ? "option not supported" : "request refused");
        fprintf(stderr, "fpspawn: %s: a Spawn was refused (%s)\n", id, rr == -2 ? "unsupported option" : found ? "not installed" : "invalid request");
        free(abuf); free(q->store); free(q);
        return;
    }
    make_env(envp, 512, !dev, x11);
    int slot = -1;
    for (int i = 0; i < MAX_KIDS && slot < 0; i++) if (!kids[i].live) slot = i;
    int inst[2] = { -1, -1 }, rep[2] = { -1, -1 };
    char scope[96], logp[PATH_MAX];
    snprintf(scope, sizeof scope, "fp-%s", id);
    int logfd = lordlog_path("apps", id, logp, sizeof logp) ? -1 : lordlog_open(logp);
    if (slot < 0 || pipe2(rep, O_CLOEXEC) || (sp.instance_fd >= 0 && pipe2(inst, O_CLOEXEC))) {
        reply_error(m, "org.freedesktop.DBus.Error.LimitsExceeded", "too many processes");
        goto out;
    }
    pid_t w = fork();
    if (w == 0) waiter(scope, root, argv, envp, q, m, inst[1], rep[1], logfd);
    close(rep[1]); rep[1] = -1;
    if (inst[1] >= 0) { close(inst[1]); inst[1] = -1; }
    pid_t kid = -1;
    struct pollfd pf = { rep[0], POLLIN, 0 };
    if (w > 0 && poll(&pf, 1, 3000) == 1 && read(rep[0], &kid, sizeof kid) == (ssize_t)sizeof kid && kid > 0) {
        Kid *k = &kids[slot];
        memset(k, 0, sizeof *k);
        k->live = 1;
        k->waiter = w;
        k->pid = kid;
        k->flags = q->flags;
        k->root = root;
        k->parent_pid = sp.parent_pid;
        k->inst_fd = inst[0];
        inst[0] = -1;
        if (k->inst_fd >= 0) fcntl(k->inst_fd, F_SETFL, O_NONBLOCK);
        snprintf(k->sender, sizeof k->sender, "%s", m->sender ? m->sender : "");
        snprintf(k->id, sizeof k->id, "%s", id);
        if ((q->flags & FPAK_SPAWN_WATCH_BUS) && k->sender[0]) match(k->sender, 1);
        DBuf b;
        dw_init(&b);
        dw_u32(&b, (uint32_t)kid);
        dbus_reply(&conn, m, "u", &b);
        dw_free(&b);
        if (logfd >= 0) lordlog_write(logfd, "INFO", "apps", "spawned a process of %s%s", id, q->flags & FPAK_SPAWN_SANDBOX ? " (sub-sandbox)" : "");
    } else {
        int wst = 0;
        if (w > 0) { kill(w, SIGKILL); waitpid(w, &wst, 0); }
        reply_error(m, "org.freedesktop.DBus.Error.Failed", "could not start it in the app's view");
        fprintf(stderr, "fpspawn: %s: could not start a spawned process (step %d)\n", id, WIFEXITED(wst) ? WEXITSTATUS(wst) : -1);
    }
out:
    for (int i = 0; i < 2; i++) { if (rep[i] >= 0) close(rep[i]); if (inst[i] >= 0) close(inst[i]); }
    if (logfd >= 0) close(logfd);
    free(abuf); free(q->store); free(q);
}

static void spawn_signal(const DMsg *m) {
    DRd r;
    dr_init(&r, m);
    uint32_t pid = dr_u32(&r), sig = dr_u32(&r);
    int pg = dr_bool(&r);
    if (r.err || sig < 1 || sig > 64) { reply_error(m, "org.freedesktop.DBus.Error.InvalidArgs", "bad signal"); return; }
    for (int i = 0; i < MAX_KIDS; i++) {
        Kid *k = &kids[i];
        if (!k->live || (uint32_t)k->pid != pid || !m->sender || strcmp(k->sender, m->sender)) continue;
        if (pg) killpg(k->pid, (int)sig); else kill(k->pid, (int)sig);
        break;
    }
    dbus_reply(&conn, m, NULL, NULL);           /* as flatpak-portal: also for a pid that isn't the caller's */
}

static void reply_props(const DMsg *m, int all, const char *prop) {
    DBuf b;
    dw_init(&b);
    if (all) {
        size_t a = dw_arr_begin(&b, 8);
        dw_struct(&b); dw_str(&b, "version"); dw_variant(&b, "u"); dw_u32(&b, VERSION);
        dw_struct(&b); dw_str(&b, "supports"); dw_variant(&b, "u"); dw_u32(&b, SUPPORTS);
        dw_arr_end(&b, a);
        dbus_reply(&conn, m, "a{sv}", &b);
    } else if (!strcmp(prop, "version") || !strcmp(prop, "supports")) {
        dw_variant(&b, "u");
        dw_u32(&b, !strcmp(prop, "version") ? VERSION : SUPPORTS);
        dbus_reply(&conn, m, "v", &b);
    } else reply_error(m, "org.freedesktop.DBus.Error.UnknownProperty", "no such property");
    dw_free(&b);
}

static int ask_sender(DMsg *m) {
    if (npending >= MAX_PENDING || !m->sender) { reply_error(m, "org.freedesktop.DBus.Error.LimitsExceeded", "busy"); return 0; }
    DBuf b;
    dw_init(&b);
    dw_str(&b, m->sender);
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus",
               .member = "GetConnectionUnixProcessID", .dest = "org.freedesktop.DBus", .sig = "s" };
    uint32_t s = dbus_send(&conn, &h, &b);
    dw_free(&b);
    if (!s) { reply_error(m, "org.freedesktop.DBus.Error.Failed", "bus"); return 0; }
    pending[npending].call = *m;
    pending[npending].serial = s;
    pending[npending].since = mono_ms();
    npending++;
    return 1;
}

static void on_reply(const DMsg *m) {
    if (!m->sender || strcmp(m->sender, "org.freedesktop.DBus")) return;
    for (int i = 0; i < npending; i++) {
        if (pending[i].serial != m->reply_serial) continue;
        DMsg call = pending[i].call;
        pending[i] = pending[--npending];
        uint32_t pid = 0;
        if (m->type == DBUS_RETURN && m->sig && !strcmp(m->sig, "u")) { DRd r; dr_init(&r, m); pid = dr_u32(&r); if (r.err) pid = 0; }
        do_spawn(&call, pid);
        dbus_msg_free(&call);
        return;
    }
}

static void on_owner_changed(const DMsg *m) {
    DRd r;
    dr_init(&r, m);
    const char *name = dr_str(&r, NULL), *old = dr_str(&r, NULL), *now = dr_str(&r, NULL);
    (void)old;
    if (r.err || !name || !now || now[0]) return;
    for (int i = 0; i < MAX_KIDS; i++) {
        Kid *k = &kids[i];
        if (!k->live || !(k->flags & FPAK_SPAWN_WATCH_BUS) || strcmp(k->sender, name)) continue;
        killpg(k->pid, SIGKILL);        /* the caller left the bus: what it spawned goes too (--watch-bus) */
        kill(k->pid, SIGKILL);
    }
}

/* 1 if the message was kept, 0 if the caller frees it */
static int handle(DMsg *m) {
    if ((m->type == DBUS_RETURN || m->type == DBUS_ERROR) && m->reply_serial) { on_reply(m); return 0; }
    if (m->type == DBUS_SIGNAL) {
        if (m->sender && !strcmp(m->sender, "org.freedesktop.DBus") && m->member && !strcmp(m->member, "NameOwnerChanged") &&
            m->sig && !strcmp(m->sig, "sss"))
            on_owner_changed(m);
        return 0;
    }
    if (m->type != DBUS_CALL || !m->member) return 0;
    const char *sig = m->sig ? m->sig : "";
    if (m->iface && !strcmp(m->iface, "org.freedesktop.DBus.Introspectable")) {
        DBuf b;
        dw_init(&b);
        dw_str(&b, "<node><interface name=\"org.freedesktop.portal.Flatpak\">"
                   "<property name=\"version\" type=\"u\" access=\"read\"/><property name=\"supports\" type=\"u\" access=\"read\"/>"
                   "<method name=\"Spawn\"><arg type=\"ay\" direction=\"in\"/><arg type=\"aay\" direction=\"in\"/>"
                   "<arg type=\"a{uh}\" direction=\"in\"/><arg type=\"a{ss}\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/>"
                   "<arg type=\"a{sv}\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/></method>"
                   "<method name=\"SpawnSignal\"><arg type=\"u\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/>"
                   "<arg type=\"b\" direction=\"in\"/></method>"
                   "<signal name=\"SpawnStarted\"><arg type=\"u\"/><arg type=\"u\"/></signal>"
                   "<signal name=\"SpawnExited\"><arg type=\"u\"/><arg type=\"u\"/></signal>"
                   "</interface></node>");
        dbus_reply(&conn, m, "s", &b);
        dw_free(&b);
        return 0;
    }
    if (!m->path || strcmp(m->path, OBJ)) { reply_error(m, "org.freedesktop.DBus.Error.UnknownObject", "no such object"); return 0; }
    if (m->iface && !strcmp(m->iface, "org.freedesktop.DBus.Properties")) {
        DRd r;
        dr_init(&r, m);
        const char *ifc = dr_str(&r, NULL);
        if (!strcmp(m->member, "GetAll") && !strcmp(sig, "s") && ifc && !strcmp(ifc, BUS_NAME)) reply_props(m, 1, NULL);
        else if (!strcmp(m->member, "Get") && !strcmp(sig, "ss") && ifc && !strcmp(ifc, BUS_NAME)) {
            const char *prop = dr_str(&r, NULL);
            if (r.err || !prop) reply_error(m, "org.freedesktop.DBus.Error.InvalidArgs", "bad property");
            else reply_props(m, 0, prop);
        } else reply_error(m, "org.freedesktop.DBus.Error.PropertyReadOnly", "read-only");
        return 0;
    }
    if (m->iface && strcmp(m->iface, BUS_NAME)) { reply_error(m, "org.freedesktop.DBus.Error.UnknownInterface", "no such interface"); return 0; }
    if (!strcmp(m->member, "Spawn") && !strcmp(sig, "ayaaya{uh}a{ss}ua{sv}")) return ask_sender(m);
    if (!strcmp(m->member, "SpawnSignal") && !strcmp(sig, "uub")) { spawn_signal(m); return 0; }
    if (!strcmp(m->member, "CreateUpdateMonitor")) { reply_error(m, "org.freedesktop.DBus.Error.NotSupported", "updates come from the Store"); return 0; }
    reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
    return 0;
}

int fpspawn_start(void) {
    int fd = dbus_connect_session();
    if (fd < 0 || dbus_open_fds(&conn, fd, 2000)) { if (fd >= 0) dbus_close(&conn); return -1; }
    DBuf b;
    dw_init(&b);
    dw_str(&b, BUS_NAME);
    dw_u32(&b, 4);                          /* DBUS_NAME_FLAG_DO_NOT_QUEUE */
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus", .member = "RequestName",
               .dest = "org.freedesktop.DBus", .sig = "su" };
    uint32_t s = dbus_send(&conn, &h, &b);
    dw_free(&b);
    DMsg m;
    int ok = s && dbus_wait_reply(&conn, s, &m, 2000) == 1;
    uint32_t res = 0;
    if (ok) { DRd r; dr_init(&r, &m); res = dr_u32(&r); }
    if (s) dbus_msg_free(&m);
    if (!ok || (res != 1 && res != 4)) {
        fprintf(stderr, "fpspawn: another program already serves %s\n", BUS_NAME);
        dbus_close(&conn);
        return -1;
    }
    up = 1;
    return 0;
}

int fpspawn_fd(void) { return up ? conn.fd : -1; }

void fpspawn_stop(void) {
    if (!up) return;
    for (int i = 0; i < npending; i++) { reply_error(&pending[i].call, "org.freedesktop.DBus.Error.ServiceUnknown", "Flatpak was removed"); dbus_msg_free(&pending[i].call); }
    npending = 0;
    dbus_flush(&conn);
    dbus_close(&conn);                      /* the bus releases the name with the connection */
    up = 0;
}
int fpspawn_wants_write(void) { return up && dbus_pending_out(&conn); }

/* NOTIFY_START: flatpak wrote the new instance's id; its process as the caller's PID namespace sees it */
static void started(Kid *k) {
    char buf[24];
    ssize_t r = read(k->inst_fd, buf, sizeof buf - 1 - k->inst_len);
    if (r > 0) { memcpy(k->inst + k->inst_len, buf, (size_t)r); k->inst_len += (size_t)r; k->inst[k->inst_len] = 0; }
    if (r < 0 && errno == EAGAIN) return;
    close(k->inst_fd);
    k->inst_fd = -1;
    uint32_t rel = 0;
    int child = digits_ok(k->inst) ? instance_child_pid(k->root, k->inst) : -1;
    if (child > 0 && k->parent_pid > 0) {
        /* the caller's depth from its process in the view, then the new process's pid at that depth */
        char cg[512], path[160];
        if (!cgroup_of(k->pid, cg, sizeof cg) && snprintf(path, sizeof path, "/sys/fs/cgroup%s/cgroup.procs", cg) < (int)sizeof path) {
            FILE *f = fopen(path, "re");
            int pid, depth = -1, v[16], n, base = -1;
            int rootns[16], rn = nspids(k->root, rootns, 16);
            base = rn > 0 ? rn - 1 : -1;            /* the view's level */
            for (int pass = 0; f && base >= 0 && pass < 2; pass++) {
                rewind(f);
                while (fscanf(f, "%d", &pid) == 1) {
                    if ((n = nspids(pid, v, 16)) <= base) continue;
                    if (!pass && v[base] == k->parent_pid) { depth = n - 1; break; }
                    if (pass && v[base] == child && depth >= 0 && n > depth) { rel = (uint32_t)v[depth]; break; }
                }
                if (depth < 0) break;
            }
            if (f) fclose(f);
        }
    }
    DBuf b;
    dw_init(&b);
    dw_u32(&b, (uint32_t)k->pid);
    dw_u32(&b, rel);
    send_to(k->sender, "SpawnStarted", "uu", &b);
    dw_free(&b);
}

void fpspawn_tick(void) {
    if (!up) return;
    double now = mono_ms();
    for (int i = 0; i < npending;) {                /* the bus never said who asked: refused */
        if (now - pending[i].since < PENDING_MS) { i++; continue; }
        DMsg call = pending[i].call;
        pending[i] = pending[--npending];
        reply_error(&call, "org.freedesktop.DBus.Error.Timeout", "no answer from the bus");
        dbus_msg_free(&call);
    }
    for (int i = 0; i < MAX_KIDS; i++) {
        Kid *k = &kids[i];
        if (!k->live) continue;
        if (k->inst_fd >= 0) started(k);
        int st = 0;
        pid_t r = waitpid(k->waiter, &st, WNOHANG);
        if (r == 0) continue;
        if (k->inst_fd >= 0) { close(k->inst_fd); k->inst_fd = -1; }
        DBuf b;
        dw_init(&b);
        dw_u32(&b, (uint32_t)k->pid);
        dw_u32(&b, r > 0 ? (uint32_t)st : 0);           /* the wait status, as flatpak-portal sends it */
        send_to(k->sender, "SpawnExited", "uu", &b);
        dw_free(&b);
        if ((k->flags & FPAK_SPAWN_WATCH_BUS) && k->sender[0]) {
            int others = 0;
            for (int j = 0; j < MAX_KIDS; j++) others += j != i && kids[j].live && !strcmp(kids[j].sender, k->sender);
            if (!others) match(k->sender, 0);
        }
        k->live = 0;
    }
    dbus_flush(&conn);
}

void fpspawn_dispatch(void) {
    if (!up) return;
    DMsg m;
    int r;
    while ((r = dbus_next(&conn, &m)) == 1) { if (!handle(&m)) dbus_msg_free(&m); }
    if (r < 0 || dbus_flush(&conn) < 0) {
        fprintf(stderr, "fpspawn: lost the session bus\n");
        for (int i = 0; i < npending; i++) dbus_msg_free(&pending[i].call);
        npending = 0;
        dbus_close(&conn);
        up = 0;
    }
}
