/* tkrun.c — `lordos-testbench --toolkits`: runs the toolkit harnesses (tkrun.h, docs/TOOLKIT-AUDIT.md).
 *
 * Each harness is a small Python 3 program installed in /usr/lib/lordos/toolkit-audit/. It runs only when its
 * bindings are already installed (a quick import probe first); nothing is ever installed. It runs as this user,
 * in its own process group, with a clean environment: the session's display variables, the backend variable
 * (GDK_BACKEND or QT_QPA_PLATFORM), WAYLAND_DEBUG=1 and the C locale (so the stderr patterns are English). Its
 * stdout gives the JSON line, its stderr the globals it bound and any fatal error; at 8 s the whole group is killed. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "tkrun.h"
#include "../sec/json.h"
#include "../web/weblangid.h"
#include "../wl/toolkitenv.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static double mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static const char *env_get(char *const *env, const char *k) {
    size_t n = strlen(k);
    for (char *const *e = env; e && *e; e++)
        if (!strncmp(*e, k, n) && (*e)[n] == '=') return *e + n + 1;
    return NULL;
}

/* ---- the environment ---- */

static const char *const keep_vars[] = {
    "HOME", "XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_DATA_DIRS",
    "XDG_CONFIG_DIRS", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE", "DBUS_SESSION_BUS_ADDRESS",
};

typedef struct { char *buf; size_t len, cap; char **out; int n, max; int full; } EnvB;

static void env_put(EnvB *b, const char *k, const char *v) {
    size_t need = strlen(k) + 1 + strlen(v) + 1;
    if (b->n + 1 >= b->max || b->len + need > b->cap) { b->full = 1; return; }
    char *s = b->buf + b->len;
    snprintf(s, need, "%s=%s", k, v);
    b->len += need;
    b->out[b->n++] = s;
    b->out[b->n] = NULL;
}

/* the Wayland socket's name: WAYLAND_DISPLAY, or wayland-0 when it exists */
static const char *wayland_name(char *const *env) {
    const char *w = env_get(env, "WAYLAND_DISPLAY");
    if (w && *w) return w;
    const char *rt = env_get(env, "XDG_RUNTIME_DIR");
    if (!rt || rt[0] != '/') return NULL;
    char p[PATH_MAX];
    struct stat st;
    snprintf(p, sizeof p, "%s/wayland-0", rt);
    return !stat(p, &st) && S_ISSOCK(st.st_mode) ? "wayland-0" : NULL;
}

int tk_env(const TkRunOpts *o, int combo, char *buf, size_t blen, char **out, int max) {
    const TkCombo *c = tk_combo(combo);
    char *const *env = o->env ? o->env : environ;
    EnvB b = { buf, 0, blen, out, 0, max, 0 };
    if (!c || max < 1) return -1;
    out[0] = NULL;
    const char *wl = wayland_name(env), *dpy = env_get(env, "DISPLAY");
    if (c->wayland ? !wl : !(dpy && *dpy)) return -1;
    env_put(&b, "PATH", "/usr/local/bin:/usr/bin:/bin");
    env_put(&b, "LANG", "C.UTF-8");
    env_put(&b, "LC_ALL", "C.UTF-8");
    for (size_t i = 0; i < sizeof keep_vars / sizeof keep_vars[0]; i++) {
        const char *v = env_get(env, keep_vars[i]);
        if (v) env_put(&b, keep_vars[i], v);
    }
    /* GTK 3 reads its theme and dark mode from GSettings, and lord-server gives every app it starts
     * GSETTINGS_BACKEND=keyfile (ui/portal.c portal_start): the harness gets the same, or the caller's own when it is
     * one of GLib's backends. Without it GTK 3 read dconf's defaults and the theme check failed for nothing. */
    const char *gb = env_get(env, "GSETTINGS_BACKEND");
    env_put(&b, "GSETTINGS_BACKEND", gb && (!strcmp(gb, "keyfile") || !strcmp(gb, "dconf") || !strcmp(gb, "memory")) ? gb : "keyfile");
    /* Qt follows LordOS's dark mode through the platform theme lord-server names for every app it starts
     * (QT_QPA_PLATFORMTHEME, wl/toolkitenv.h): the harness gets the same, or the caller's own when it is a plain
     * plugin name (lower case letters and digits, as Qt's own are: gnome, gtk3, xdgdesktopportal, qt5ct). */
    const char *qt = env_get(env, "QT_QPA_PLATFORMTHEME");
    int qt_ok = qt && *qt && strlen(qt) < 32;
    for (const char *p = qt; qt_ok && *p; p++) qt_ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9');
    env_put(&b, "QT_QPA_PLATFORMTHEME", qt_ok ? qt : TOOLKITENV_QT_THEME);
    if (c->wayland) env_put(&b, "WAYLAND_DISPLAY", wl);
    else {
        env_put(&b, "DISPLAY", dpy);
        /* Xwayland's cookie: XAUTHORITY, or the one lord-server writes beside the socket */
        const char *xa = env_get(env, "XAUTHORITY"), *rt = env_get(env, "XDG_RUNTIME_DIR");
        char p[PATH_MAX];
        if (xa && *xa) env_put(&b, "XAUTHORITY", xa);
        else if (rt && rt[0] == '/' && dpy[0] == ':') {
            int k = 0;
            char num[16];
            for (const char *d = dpy + 1; *d >= '0' && *d <= '9' && k < 15; d++) num[k++] = *d;
            num[k] = 0;
            struct stat st;
            snprintf(p, sizeof p, "%s/lord-xauth.%s", rt, num);
            if (k && !stat(p, &st) && S_ISREG(st.st_mode)) env_put(&b, "XAUTHORITY", p);
        }
    }
    if (c->env_var) env_put(&b, c->env_var, c->backend);
    env_put(&b, "WAYLAND_DEBUG", "1");
    env_put(&b, "GTK_A11Y", "none");                /* no accessibility bus warnings in the log */
    env_put(&b, "NO_AT_BRIDGE", "1");
    env_put(&b, "PYTHONDONTWRITEBYTECODE", "1");
    return b.full ? -1 : b.n;
}

/* ---- processes ---- */

static volatile pid_t running_group;                /* killed if we are interrupted */

static void on_signal(int sig) {
    pid_t g = running_group;
    if (g > 0) kill(-g, SIGKILL);
    signal(sig, SIG_DFL);
    raise(sig);
}

void tk_signals(void) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);
}

/* spawns argv in its own process group; out/err: our ends of its stdout/stderr (-1: /dev/null) */
static pid_t spawn(char *const argv[], char *const envp[], int *out, int *err) {
    int po[2] = { -1, -1 }, pe[2] = { -1, -1 };
    if (out && pipe2(po, O_CLOEXEC)) return -1;
    if (err && pipe2(pe, O_CLOEXEC)) { if (out) { close(po[0]); close(po[1]); } return -1; }
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (out) posix_spawn_file_actions_adddup2(&fa, po[1], 1);
    else posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    if (err) posix_spawn_file_actions_adddup2(&fa, pe[1], 2);
    else posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    sigset_t none, def;
    sigemptyset(&none);
    sigemptyset(&def);
    sigaddset(&def, SIGPIPE);
    sigaddset(&def, SIGINT);
    sigaddset(&def, SIGTERM);
    sigaddset(&def, SIGHUP);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &def);
    posix_spawnattr_setpgroup(&at, 0);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid = -1;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (out) close(po[1]);
    if (err) close(pe[1]);
    if (rc) {
        if (out) close(po[0]);
        if (err) close(pe[0]);
        return -1;
    }
    running_group = pid;
    if (out) *out = po[0];
    if (err) *err = pe[0];
    return pid;
}

/* waits for pid until deadline (ms, monotonic), reading its pipes meanwhile. how: 1 exit, 2 signal, 3 killed at
 * the deadline. */
typedef struct {
    char *out; size_t olen, ocap; int truncated; TkLog *log;
    /* show mode: the progress lines, reported as they come */
    const TkRunOpts *o; int slot, combo; TkProgBuf prog;
} Sink;

static void on_prog(void *ctx, const TkProg *p) {
    Sink *s = ctx;
    TkEvent e = { p->kind == TK_PROG_STEP ? TK_EV_STEP : TK_EV_CHECK, s->slot, s->combo, p->check, p->result, NULL };
    s->o->on_event(s->o->ctx, &e);
}

static int aborted(const TkRunOpts *o) { return o && o->abort && *o->abort; }

static void collect(pid_t pid, int fo, int fe, double deadline, Sink *s, int *how, int *code) {
    int exited = 0, killed = 0, status = 0;
    double drain_until = 0;
    char buf[8192];
    for (;;) {
        double now = mono_ms();
        if (!exited) {
            siginfo_t si;
            memset(&si, 0, sizeof si);
            if (!waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG | WNOWAIT) && si.si_pid == pid) {
                exited = 1;
                kill(-pid, SIGKILL);                /* whatever it left behind in its group (the leader is a zombie) */
                drain_until = now + 500;
            } else if (now >= deadline || aborted(s->o)) {
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
                killed = 1;
                exited = 1;
                drain_until = now + 500;
            }
        }
        if ((fo < 0 && fe < 0) || (exited && now >= drain_until)) break;
        struct pollfd p[2];
        int np = 0;
        if (fo >= 0) p[np++] = (struct pollfd){ fo, POLLIN, 0 };
        if (fe >= 0) p[np++] = (struct pollfd){ fe, POLLIN, 0 };
        int r = poll(p, (nfds_t)np, 50);
        if (r < 0 && errno != EINTR) break;
        for (int i = 0; r > 0 && i < np; i++) {
            if (!p[i].revents) continue;
            ssize_t k = read(p[i].fd, buf, sizeof buf);
            if (k > 0 && p[i].fd == fo) {
                size_t room = s->ocap - s->olen;
                size_t take = (size_t)k < room ? (size_t)k : room;
                memcpy(s->out + s->olen, buf, take);
                s->olen += take;
                if (take < (size_t)k) s->truncated = 1;
                if (s->o && s->o->show && s->o->on_event) tk_progress_feed(&s->prog, buf, (size_t)k, on_prog, s);
            } else if (k > 0) {
                tk_log_feed(s->log, buf, (size_t)k);
            } else if (k == 0 || (k < 0 && errno != EINTR && errno != EAGAIN)) {
                if (p[i].fd == fo) { close(fo); fo = -1; }
                else { close(fe); fe = -1; }
            }
        }
    }
    if (fo >= 0) close(fo);
    if (fe >= 0) close(fe);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    running_group = 0;
    if (killed) { *how = 3; *code = SIGKILL; }
    else if (WIFSIGNALED(status)) { *how = 2; *code = WTERMSIG(status); }
    else { *how = 1; *code = WEXITSTATUS(status); }
    tk_log_end(s->log);
}

/* python3 -I -c probe: 0 when the bindings import */
static int probe(const TkRunOpts *o, const TkCombo *c, char *const envp[]) {
    char *argv[] = { (char *)o->python, "-I", "-c", (char *)c->probe, NULL };
    pid_t pid = spawn(argv, envp, NULL, NULL);
    if (pid < 0) return -1;
    double deadline = mono_ms() + o->probe_ms;
    int status = 0;
    int timed_out = 0;
    for (;;) {
        /* WNOWAIT: the exited leader stays a zombie, so its process group id can't be reused before the kill below */
        siginfo_t si;
        memset(&si, 0, sizeof si);
        int w = waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG | WNOWAIT);
        if (!w && si.si_pid == pid) break;
        if (w < 0 && errno != EINTR) { kill(-pid, SIGKILL); running_group = 0; return -1; }
        if (mono_ms() >= deadline) { timed_out = 1; break; }
        struct timespec t = { 0, 20 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    kill(-pid, SIGKILL);                            /* whatever it left in its group (or itself, at the deadline) */
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    running_group = 0;
    if (timed_out) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* A folder or file the harness may come from: no link, owned by root (the installed folder) or, for a development
 * folder (LORDOS_TOOLKIT_AUDIT_DIR), by root or this user, and writable by nobody else. A harness someone else could
 * have changed is never run: it would run as this user, with this session's displays and bus. */
static int trusted(const char *path, int want_dir, int installed) {
    struct stat st;
    if (lstat(path, &st) || (want_dir ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode))) return 0;
    if (st.st_uid != 0 && (installed || st.st_uid != geteuid())) return 0;
    return !(st.st_mode & (S_IWGRP | S_IWOTH));
}

int tk_harness_path(const char *dir, const char *toolkit, char *p, size_t n) {
    int installed = !strcmp(dir, TK_HARNESS_DIR);
    int k = snprintf(p, n, "%s/%s.py", dir, toolkit);
    if (k <= 0 || (size_t)k >= n || dir[0] != '/') return -1;
    return trusted(dir, 1, installed) && trusted(p, 0, installed) ? 0 : -1;
}

static int harness_path(const TkRunOpts *o, const TkCombo *c, char *p, size_t n) {
    return tk_harness_path(o->dir, c->toolkit, p, n);
}

void tk_report_free(TkReport *rep) {
    for (int i = 0; i < TK_COMBOS; i++) { free(rep->r[i].log); rep->r[i].log = NULL; }
}

/* one combination into r (its status set when it doesn't run); -1 when memory ran out */
static int run_one(const TkRunOpts *o, const int *which, int i, int slot, TkResult *r, int *probed, char *ob) {
    const TkCombo *c = tk_combo(which[i]);
    if (!c) { r->status = TK_ST_NO_HARNESS; return 0; }
    r->log = malloc(sizeof *r->log);
    if (!r->log) return -1;
    tk_log_init(r->log);
    char hp[PATH_MAX], ebuf[16384], *envp[48];
    if (harness_path(o, c, hp, sizeof hp)) { r->status = TK_ST_NO_HARNESS; return 0; }
    if (access(o->python, X_OK)) { r->status = TK_ST_NOT_INSTALLED; return 0; }
    int first = i;                                  /* the toolkit's probe, once */
    for (int j = 0; j < i; j++) if (!strcmp(tk_combo(which[j])->toolkit, c->toolkit)) { first = j; break; }
    char *penv[] = { "PATH=/usr/bin:/bin", "LANG=C.UTF-8", "PYTHONDONTWRITEBYTECODE=1", NULL };
    if (!probed[first]) probed[first] = probe(o, c, penv) ? -1 : 1;
    if (probed[first] < 0) { r->status = TK_ST_NOT_INSTALLED; return 0; }
    if (tk_env(o, which[i], ebuf, sizeof ebuf, envp, 48) < 0) { r->status = TK_ST_NO_DISPLAY; return 0; }
    if (aborted(o)) return 0;
    if (o->progress && o->T) {
        char line[256];
        const char *kv[] = { "combo", c->id, NULL };
        tk_fill(o->T("running"), kv, line, sizeof line);
        fprintf(stderr, "%s\n", line);
    }
    /* a fixed argument list: the harness's path (checked above) and, in show mode, the fixed word --show */
    char *argv[] = { (char *)o->python, "-I", hp, o->show ? "--show" : NULL, NULL };
    int fo = -1, fe = -1;
    double t0 = mono_ms();
    pid_t pid = spawn(argv, envp, &fo, &fe);
    if (pid < 0) { r->status = TK_ST_NOT_INSTALLED; return 0; }
    if (o->on_event) {
        TkEvent e = { TK_EV_START, slot, which[i], -1, 0, NULL };
        o->on_event(o->ctx, &e);
    }
    Sink s;
    memset(&s, 0, sizeof s);
    s.out = ob;
    s.ocap = TK_OUT_MAX;
    s.log = r->log;
    s.o = o;
    s.slot = slot;
    s.combo = which[i];
    int how = 1, code = 0;
    collect(pid, fo, fe, t0 + o->kill_ms, &s, &how, &code);
    r->elapsed_ms = (int)(mono_ms() - t0);
    tk_parse_output(r, s.out, s.olen, s.truncated);
    tk_finish(r, how, code);
    return 0;
}

int tk_audit(const TkRunOpts *o, const int *which, int n, TkReport *rep) {
    memset(rep, 0, sizeof *rep);
    rep->at_ms = (long long)time(NULL) * 1000;
    int probed[TK_COMBOS];                          /* per toolkit (the first combination's index): 1 ok, -1 not */
    memset(probed, 0, sizeof probed);
    char *ob = malloc(TK_OUT_MAX);
    if (!ob) return -1;
    for (int i = 0; i < n && i < TK_COMBOS && !aborted(o); i++) {
        int slot = rep->n;
        TkResult *r = &rep->r[rep->n++];
        tk_result_init(r, which[i]);
        if (run_one(o, which, i, slot, r, probed, ob)) { free(ob); return -1; }
        if (o->on_event) {
            TkEvent e = { TK_EV_DONE, slot, which[i], -1, 0, r };
            o->on_event(o->ctx, &e);
        }
    }
    free(ob);
    tk_report_global(rep);
    return 0;
}

/* ---- the cache ---- */

int tk_cache_path(char *out, size_t n, char *const *env) {
    const char *c = env_get(env ? env : environ, "XDG_CACHE_HOME"), *h = env_get(env ? env : environ, "HOME");
    int k;
    if (c && c[0] == '/') k = snprintf(out, n, "%s/lordos/toolkit-audit.json", c);
    else if (h && h[0] == '/') k = snprintf(out, n, "%s/.cache/lordos/toolkit-audit.json", h);
    else return -1;
    return k > 0 && (size_t)k < n ? 0 : -1;
}

int tk_save(const TkReport *rep, const char *path) {
    char dir[PATH_MAX], tmp[PATH_MAX + 16];
    if (snprintf(dir, sizeof dir, "%s", path) >= (int)sizeof dir) return -1;
    /* the folders, 0700 when made here */
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(dir, 0700) && errno != EEXIST) return -1;
        *p = '/';
    }
    char *slash = strrchr(dir, '/');
    if (!slash) return -1;
    *slash = 0;
    struct stat st;
    if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) return -1;
    snprintf(tmp, sizeof tmp, "%s.XXXXXX", path);
    int fd = mkstemp(tmp);                          /* O_EXCL, 0600 */
    if (fd < 0) return -1;
    fchmod(fd, 0600);
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return -1; }
    tk_report_json(f, rep);
    int bad = fflush(f) || ferror(f) || fsync(fd);
    if (fclose(f)) bad = 1;
    if (bad || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

/* ---- texts: assets/locales/<lang>/toolkitaudit.json ---- */

static JDoc cat_lang, cat_en;

static void load_json(const char *path, JDoc *d) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (1 << 20)) { close(fd); return; }
    char *b = malloc((size_t)st.st_size);
    ssize_t r = b ? read(fd, b, (size_t)st.st_size) : -1;
    close(fd);
    if (r != st.st_size || json_parse(b, (size_t)r, d, NULL, 0) || !d->root || d->root->type != J_OBJ) {
        json_free(d);
        memset(d, 0, sizeof *d);
    }
    free(b);
}

static void load_catalog(const char *lang, JDoc *d) {
    char self[PATH_MAX], p[PATH_MAX + 96];
    ssize_t k = readlink("/proc/self/exe", self, sizeof self - 1);
    self[k > 0 ? k : 0] = 0;
    char *sl = strrchr(self, '/');
    if (sl) *sl = 0;
    /* installed: /usr/lib/lordos/assets; built: server/build → assets */
    snprintf(p, sizeof p, "/usr/lib/lordos/assets/locales/%s/toolkitaudit.json", lang);
    load_json(p, d);
    if (d->root) return;
    snprintf(p, sizeof p, "%s/../../assets/locales/%s/toolkitaudit.json", self, lang);
    load_json(p, d);
}

static const char *T(const char *key) {
    const char *s = cat_lang.root ? json_str(cat_lang.root, key) : NULL;
    if (!s || !s[0]) s = cat_en.root ? json_str(cat_en.root, key) : NULL;
    return s && s[0] ? s : key;
}

static void texts_init(void) {
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LC_MESSAGES");
    if (!l || !*l) l = getenv("LANG");
    char lang[WEBLANG_ID_MAX];
    weblang_id(l, lang);
    load_catalog("en", &cat_en);
    if (strcmp(lang, "en")) load_catalog(lang, &cat_lang);
}

static void usage(void) {
    fprintf(stderr, "%s\n       lordos-testbench --toolkits --last\n", T("usage"));
}

/* the saved result, read as it is: a regular file of the user's, no link, at most TK_LAST_MAX bytes */
char *tk_read_saved(const char *path, size_t *len) {
    *len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    char *buf = NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_size <= 0 || st.st_size > TK_LAST_MAX) goto out;
    buf = malloc((size_t)st.st_size + 1);
    if (!buf) goto out;
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t k = read(fd, buf + got, (size_t)st.st_size - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        got += (size_t)k;
    }
    if (got != (size_t)st.st_size) { free(buf); buf = NULL; goto out; }
    buf[got] = 0;
    *len = got;
out:
    close(fd);
    return buf;
}

/* --toolkits --last: the saved result, summed up in fixed English lines (lordos-issue pastes them into a report) */
static int last_main(void) {
    char path[PATH_MAX];
    size_t len = 0;
    char *j = tk_cache_path(path, sizeof path, environ) ? NULL : tk_read_saved(path, &len);
    if (!j) { puts("no saved result (run lordos-testbench --toolkits in the LordOS Terminal)"); return 0; }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tk_last_summary(stdout, j, len, (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);   /* "unreadable" is an answer too */
    free(j);
    return ferror(stdout) ? 1 : 0;
}

int tk_parse_only(const char *only, int *which, char *bad, size_t blen) {
    int n = 0;
    if (bad && blen) bad[0] = 0;
    for (const char *p = only; *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len) {
            int k = tk_combo_find(p, len);
            if (k < 0) {
                if (bad && blen) {
                    snprintf(bad, blen, "%.*s", (int)(len < 40 ? len : 40), p);
                    for (char *q = bad; *q; q++) if ((unsigned char)*q < 0x20 || *q == 0x7f) *q = '?';
                }
                return -1;
            }
            int dup = 0;
            for (int j = 0; j < n; j++) dup |= which[j] == k;
            if (!dup && n < TK_COMBOS) which[n++] = k;
        }
        if (!e) break;
        p = e + 1;
    }
    return n;
}

const char *tk_harness_dir(void) {
    const char *dir = getenv("LORDOS_TOOLKIT_AUDIT_DIR");
    return dir && dir[0] == '/' ? dir : TK_HARNESS_DIR;
}

/* --show in a terminal: each check as the harness decides it */
static void term_event(void *ctx, const TkEvent *e) {
    (void)ctx;
    if (e->kind != TK_EV_CHECK) return;
    static const char *mark[3] = { "-", "\xe2\x9c\x93", "\xe2\x9c\x97" };     /* skip, pass, fail: - ✓ ✗ */
    fprintf(stderr, "  %s %s\n", mark[e->result >= 0 && e->result <= 2 ? e->result : 0], tk_check_id(e->check));
}

int toolkits_main(int argc, char **argv) {
    int json = 0, show = 0, wait = 0, which[TK_COMBOS], n = 0;
    const char *only = NULL;
    texts_init();
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--last")) return argc == 3 ? last_main() : (usage(), 2);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--toolkits")) continue;
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--show")) show = 1;
        else if (!strcmp(argv[i], "--wait")) wait = 1;
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strncmp(argv[i], "--only=", 7)) only = argv[i] + 7;
        else { usage(); return 2; }
    }
    if (only) {
        char id[64];
        n = tk_parse_only(only, which, id, sizeof id);
        if (n < 0) {
            char list[256] = "", line[512];
            for (int j = 0; j < TK_COMBOS; j++) {
                strncat(list, j ? ", " : "", sizeof list - strlen(list) - 1);
                strncat(list, tk_combo(j)->id, sizeof list - strlen(list) - 1);
            }
            const char *kv[] = { "id", id, "list", list, NULL };
            tk_fill(T("badOnly"), kv, line, sizeof line);
            fprintf(stderr, "%s\n", line);
            return 2;
        }
        if (!n) { usage(); return 2; }
    } else {
        for (int i = 0; i < TK_COMBOS; i++) which[n++] = i;
    }
    /* the harnesses are GUI clients of this session: never as root (sudo, pkexec), like any Wayland client here */
    if (geteuid() == 0) { fprintf(stderr, "%s\n", T("root")); return 2; }
    tk_signals();
    TkRunOpts o = { tk_harness_dir(), TK_PYTHON, show ? TK_SHOW_KILL_MS : TK_KILL_MS, TK_PROBE_MS, isatty(2), T, environ,
                    show, show && isatty(2) ? term_event : NULL, NULL, NULL };
    TkReport rep;
    if (tk_audit(&o, which, n, &rep)) { fprintf(stderr, "lordos-testbench: out of memory\n"); return 1; }
    if (json) tk_report_json(stdout, &rep);
    else tk_report_table(stdout, &rep, T);
    char path[PATH_MAX];
    if (!tk_cache_path(path, sizeof path, environ) && !tk_save(&rep, path)) {
        if (!json) {
            char line[PATH_MAX + 256];
            const char *kv[] = { "path", path, NULL };
            tk_fill(T("saved"), kv, line, sizeof line);
            printf("\n%s\n", line);
        }
    } else {
        fprintf(stderr, "%s\n", T("notSaved"));
    }
    tk_report_free(&rep);
    int rc = ferror(stdout) ? 1 : 0;
    /* --wait (the launcher's Toolkit Audit entry, Terminal=true): keep the window open until Enter, so the table can be
     * read; only with a terminal on stdin, so scripts and lordos-issue never block */
    if (wait && isatty(0)) {
        printf("\n%s", T("pressEnter"));
        fflush(stdout);
        int ch;
        while ((ch = getchar()) != EOF && ch != '\n') {}
    }
    return rc;
}
