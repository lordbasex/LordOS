/* webmgr.c — see webmgr.h. */
#include "webmgr.h"
#include "../sec/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PROBE_KEEP_MS   (15 * 60 * 1000)      /* a finished probe waits this long for its Install */
#define LOG_MAX         (1024 * 1024)

static char assets[PATH_MAX];
static char helper[PATH_MAX];

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

void webmgr_init(const char *dir) { snprintf(assets, sizeof assets, "%s", dir ? dir : ""); }

/* ---- paths ---- */

static const char *home(void) {
    const char *h = getenv("HOME");
    return h && h[0] == '/' && strlen(h) < PATH_MAX - 128 ? h : NULL;
}

static char cfg_dir[PATH_MAX];
const char *webmgr_config_dir(void) {
    const char *h = home();
    snprintf(cfg_dir, sizeof cfg_dir, "%s/.config/lordos/web", h ? h : "/nonexistent");
    return cfg_dir;
}

static int app_file(const char *app, char *out, size_t n) {
    if (!webreg_id_ok(app)) return -1;
    int w = snprintf(out, n, "%s/apps/%s.json", webmgr_config_dir(), app);
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

int webmgr_data_dir(const char *app, const char *profile, char *out, size_t n) {
    const char *h = home();
    if (!h || !webreg_id_ok(app) || (profile && profile[0] && !webreg_id_ok(profile))) return -1;
    int w = profile && profile[0] ? snprintf(out, n, "%s/.local/share/lordos/web/%s/%s", h, app, profile)
                                  : snprintf(out, n, "%s/.local/share/lordos/web/%s", h, app);
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

/* every folder of p, 0700; a link on the way (or p not a folder at the end) refuses */
static int mkdirs(const char *p) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s", p) >= (int)sizeof tmp) return -1;
    for (char *s = tmp + 1; ; s++) {
        if (*s != '/' && *s) continue;
        char c = *s;
        *s = 0;
        if (mkdir(tmp, 0700) && errno != EEXIST) return -1;
        *s = c;
        if (!c) break;
    }
    struct stat st;
    return lstat(p, &st) || !S_ISDIR(st.st_mode) ? -1 : 0;
}

static int sub_dir(const char *name, char *out, size_t n) {
    int w = snprintf(out, n, "%s/%s", webmgr_config_dir(), name);
    return w < 0 || (size_t)w >= n || mkdirs(out) ? -1 : 0;
}

/* ---- lord-web ---- */

void webmgr_set_helper(const char *path) { snprintf(helper, sizeof helper, "%s", path ? path : ""); }

int webmgr_available(void) {
    if (!helper[0]) {
        char self[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n <= 0) return 0;
        self[n] = 0;
        char *slash = strrchr(self, '/');
        if (!slash) return 0;
        *slash = 0;
        if (snprintf(helper, sizeof helper, "%s/lord-web", self) >= (int)sizeof helper) { helper[0] = 0; return 0; }
    }
    struct stat st, me;
    if (stat(helper, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) return 0;
    /* owned by root or by lord-server's owner, and not writable by anyone else */
    if (stat("/proc/self/exe", &me) || (st.st_uid != 0 && st.st_uid != me.st_uid) || (st.st_mode & 022)) return 0;
    return 1;
}

/* The session's environment for lord-web (docs/WEBAPPS.md, "Starting an app"), built before fork. */
typedef struct { char buf[5][PATH_MAX + 32]; char *envp[9]; char log[PATH_MAX]; char home[PATH_MAX]; } Env;

static void env_build(Env *e) {
    static const char *const keep[5] = { "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS", "HOME", "LANG" };
    int n = 0;
    for (int i = 0; i < 5; i++) {
        const char *v = getenv(keep[i]);
        if (!v || strlen(v) >= PATH_MAX) continue;
        snprintf(e->buf[n], sizeof e->buf[n], "%s=%s", keep[i], v);
        e->envp[n] = e->buf[n];
        n++;
    }
    /* under lord-sessiond, lord-web is strict: no scope or no Landlock means it does not start (webmain.c) */
    if (getenv("LORDOS_SESSIOND")) e->envp[n++] = "LORDOS_SESSIOND=1";
    e->envp[n++] = "LORDOS_WEB=1";
    e->envp[n] = NULL;
    const char *h = home();
    e->log[0] = e->home[0] = 0;
    if (!h) return;
    snprintf(e->home, sizeof e->home, "%s", h);
    char d[PATH_MAX];
    snprintf(d, sizeof d, "%s/.cache/lordos", h);
    if (mkdirs(d)) return;
    if (snprintf(e->log, sizeof e->log, "%s/web.log", d) >= (int)sizeof e->log) { e->log[0] = 0; return; }
    struct stat st;
    if (!lstat(e->log, &st) && S_ISREG(st.st_mode) && st.st_size > LOG_MAX && truncate(e->log, 0)) e->log[0] = 0;
}

/* Starts lord-web with argv; chan (or -1) becomes its fd 3. Its own session (so its WebKit processes
 * can be ended with it), no signal mask, nothing else of lord-server's open. */
static pid_t spawn(char *const argv[], int chan) {
    Env e;
    env_build(&e);
    pid_t pid = fork();
    if (pid != 0) return pid;
    setsid();
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    for (int s = 1; s < 32; s++) signal(s, SIG_DFL);
    if (chan >= 0) { if (chan == 3) fcntl(3, F_SETFD, 0); else if (dup2(chan, 3) < 0) _exit(127); }
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    int lg = e.log[0] ? open(e.log, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600) : -1;
    if (null >= 0) dup2(null, 0);
    if (lg < 0) lg = null;
    if (lg >= 0) { dup2(lg, 1); dup2(lg, 2); }
    syscall(SYS_close_range, chan >= 0 ? 4u : 3u, ~0u, 0);
    if (e.home[0] && chdir(e.home)) { if (chdir("/")) {} }
    execve(helper, argv, e.envp);
    _exit(127);
}

/* ---- the processes ---- */

typedef struct { int used; pid_t pid; char app[WEBREG_ID_MAX + 1], profile[WEBREG_ID_MAX + 1]; } Kid;
static Kid kids[WEB_MAX_RUNNING];

static void reap(void) {
    for (int i = 0; i < WEB_MAX_RUNNING; i++) {
        if (!kids[i].used) continue;
        int st;
        pid_t r = waitpid(kids[i].pid, &st, WNOHANG);
        if (r == kids[i].pid || (r < 0 && errno == ECHILD)) kids[i].used = 0;
    }
}

static Kid *kid_of(const char *app, const char *profile) {
    for (int i = 0; i < WEB_MAX_RUNNING; i++)
        if (kids[i].used && !strcmp(kids[i].app, app) && !strcmp(kids[i].profile, profile)) return &kids[i];
    return NULL;
}

/* Ends a process and its WebKit processes (its own session): TERM, up to 2 s, then KILL. */
static void end_kid(Kid *k) {
    kill(-k->pid, SIGTERM);
    double t0 = now_ms();
    int st;
    while (waitpid(k->pid, &st, WNOHANG) == 0) {
        if (now_ms() - t0 > 2000) { kill(-k->pid, SIGKILL); waitpid(k->pid, &st, 0); break; }
        struct timespec ts = { 0, 20 * 1000000 };
        nanosleep(&ts, NULL);
    }
    k->used = 0;
}

static void end_app(const char *app, const char *profile) {
    reap();
    for (int i = 0; i < WEB_MAX_RUNNING; i++)
        if (kids[i].used && !strcmp(kids[i].app, app) && (!profile || !strcmp(kids[i].profile, profile))) end_kid(&kids[i]);
}

int webmgr_running(WebRunning *out, int max) {
    reap();
    int n = 0;
    for (int i = 0; i < WEB_MAX_RUNNING && n < max; i++) {
        if (!kids[i].used) continue;
        snprintf(out[n].app, sizeof out[n].app, "%s", kids[i].app);
        snprintf(out[n].profile, sizeof out[n].profile, "%s", kids[i].profile);
        out[n].pid = kids[i].pid;
        n++;
    }
    return n;
}

static pid_t parent_of(pid_t pid) {
    char p[64], b[512];
    snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t r = read(fd, b, sizeof b - 1);
    close(fd);
    if (r <= 0) return 0;
    b[r] = 0;
    char *e = strrchr(b, ')');                      /* the name may hold spaces and parentheses */
    int ppid = 0;
    if (!e || sscanf(e + 1, " %*c %d", &ppid) != 1) return 0;
    return (pid_t)ppid;
}

int webmgr_owner(pid_t pid, char *app, char *profile) {
    reap();
    for (int depth = 0; pid > 1 && depth < 8; depth++) {
        for (int i = 0; i < WEB_MAX_RUNNING; i++)
            if (kids[i].used && kids[i].pid == pid) {
                snprintf(app, WEBREG_ID_MAX + 1, "%s", kids[i].app);
                snprintf(profile, WEBREG_ID_MAX + 1, "%s", kids[i].profile);
                return 0;
            }
        pid = parent_of(pid);
    }
    return -1;
}

/* ---- the registry ---- */

const char *webmgr_load(const char *app, WebApp *a) {
    char path[PATH_MAX], why[96];
    if (app_file(app, path, sizeof path)) return "webapps.badId";
    size_t len;
    char *t = webreg_read_file(path, WEBREG_FILE_MAX, &len);
    if (!t) return "webapps.notInstalled";
    int r = webreg_parse(t, len, a, why, sizeof why);
    if (r) fprintf(stderr, "webapps: %s refused: %s\n", path, why);
    else if (strcmp(a->id, app)) r = -1;                /* a file names its own app */
    free(t);
    return r ? "webapps.badRegistry" : NULL;
}

static const char *save(const WebApp *a) {
    char dir[PATH_MAX], path[PATH_MAX];
    if (sub_dir("apps", dir, sizeof dir) || app_file(a->id, path, sizeof path)) return "webapps.writeFailed";
    char *buf = malloc(WEBREG_FILE_MAX + 1);
    size_t n = buf ? webreg_write(a, buf, WEBREG_FILE_MAX + 1) : 0;
    int r = n ? webreg_write_file(path, buf, n) : -1;
    free(buf);
    if (!n) return "webapps.badEntry";
    return r ? "webapps.writeFailed" : NULL;
}

static int cmp_names(const void *x, const void *y) { return strcmp(*(char *const *)x, *(char *const *)y); }

void webmgr_each(void (*fn)(const WebApp *a, const char *text, size_t len, const char *icon, void *ctx), void *ctx) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/apps", webmgr_config_dir());
    DIR *d = opendir(dir);
    if (!d) return;
    char *names[256];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 256) {
        size_t l = strlen(e->d_name);
        if (l < 6 || l > WEBREG_ID_MAX + 5 || strcmp(e->d_name + l - 5, ".json")) continue;
        char id[WEBREG_ID_MAX + 1];
        memcpy(id, e->d_name, l - 5);
        id[l - 5] = 0;
        if (webreg_id_ok(id) && (names[n] = strdup(id))) n++;
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], cmp_names);
    WebApp *a = malloc(sizeof *a);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX], icon[PATH_MAX] = "", why[96];
        size_t len;
        char *t = a && !app_file(names[i], path, sizeof path) ? webreg_read_file(path, WEBREG_FILE_MAX, &len) : NULL;
        if (t && !webreg_parse(t, len, a, why, sizeof why) && !strcmp(a->id, names[i])) {
            if (a->icon[0]) snprintf(icon, sizeof icon, "%s/%s", webmgr_config_dir(), a->icon);
            fn(a, t, len, icon, ctx);
        }
        free(t);
        free(names[i]);
    }
    free(a);
}

/* ---- the catalog (LordOS's own list, root-owned with the rest of its assets) ---- */

static int icon_file_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 5 || n > WEBREG_ID_MAX + 4 || strcmp(s + n - 4, ".png")) return 0;
    for (size_t i = 0; i < n - 4; i++) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) return 0;
    return 1;
}

/* The catalog entry id into a (name, URLs, color, background) and its icon's path. 0 found. */
static int catalog_entry(const char *id, WebApp *a, char *icon, size_t n) {
    char path[PATH_MAX];
    if (!assets[0] || snprintf(path, sizeof path, "%s/webapps/catalog.json", assets) >= (int)sizeof path) return -1;
    size_t len;
    char *t = webreg_read_file(path, 1u << 20, &len);
    if (!t) return -1;
    JDoc d;
    int rc = -1;
    if (json_parse(t, len, &d, NULL, 0)) { free(t); return -1; }
    const JVal *apps = d.root->type == J_OBJ ? json_get(d.root, "apps") : NULL;
    for (const JVal *e = apps && apps->type == J_ARR ? apps->kid : NULL; e; e = e->next) {
        const char *eid = e->type == J_OBJ ? json_str(e, "id") : NULL;
        if (!eid || strcmp(eid, id)) continue;
        const char *name = json_str(e, "name"), *start = json_str(e, "startUrl"), *scope = json_str(e, "scope"),
                   *origin = json_str(e, "origin"), *color = json_str(e, "themeColor"), *ic = json_str(e, "icon");
        if (!name || !start || !scope || !origin || strlen(name) > WEBREG_NAME_MAX || strlen(start) > WEBREG_URL_MAX ||
            strlen(scope) > WEBREG_URL_MAX || strlen(origin) > WEBREG_URL_MAX) break;
        snprintf(a->id, sizeof a->id, "%s", id);
        snprintf(a->name, sizeof a->name, "%s", name);
        snprintf(a->start_url, sizeof a->start_url, "%s", start);
        snprintf(a->scope, sizeof a->scope, "%s", scope);
        snprintf(a->origin, sizeof a->origin, "%s", origin);
        if (color && strlen(color) == 7) snprintf(a->theme_color, sizeof a->theme_color, "%s", color);
        a->background = json_bool(e, "background", 0);
        a->source = WEBREG_SRC_CATALOG;
        icon[0] = 0;
        if (ic && icon_file_ok(ic) && snprintf(icon, n, "%s/webapps/icons/%s", assets, ic) >= (int)n) icon[0] = 0;
        rc = 0;
        break;
    }
    json_free(&d);
    free(t);
    return rc;
}

/* ---- probes ---- */

typedef struct {
    int used, id, done;
    double started, finished;
    pid_t pid;
    int fd;
    char *json;                     /* the answer for JS */
    WebProbe p;
    unsigned char *icon;
    size_t icon_len;
    int icon_kind;                  /* 1 PNG, 2 SVG */
    char icon_path[PATH_MAX];
} Probe;

static pthread_mutex_t pmu = PTHREAD_MUTEX_INITIALIZER;
static Probe probes[WEB_PROBE_SLOTS];
static int next_probe = 1;

typedef struct { int fd; double deadline; char b[WEBREG_FILE_MAX + 4096]; size_t n; } Rd;

/* more bytes (0), or -1 at the end, an error or the deadline */
static int rd_fill(Rd *r) {
    if (r->n >= sizeof r->b) return -1;
    double left = r->deadline - now_ms();
    if (left <= 0) return -1;
    struct pollfd pf = { r->fd, POLLIN, 0 };
    int p = poll(&pf, 1, (int)left + 1);
    if (p <= 0) return -1;
    ssize_t got = read(r->fd, r->b + r->n, sizeof r->b - r->n);
    if (got <= 0) return -1;
    r->n += (size_t)got;
    return 0;
}

/* one line of at most max bytes (without its \n), moved out of the buffer into out (NUL-terminated) */
static int rd_line(Rd *r, char *out, size_t max) {
    for (;;) {
        char *nl = memchr(r->b, '\n', r->n);
        if (nl) {
            size_t l = (size_t)(nl - r->b);
            if (l > max) return -1;
            memcpy(out, r->b, l);
            out[l] = 0;
            memmove(r->b, nl + 1, r->n - l - 1);
            r->n -= l + 1;
            return (int)l;
        }
        if (r->n > max || rd_fill(r)) return -1;
    }
}

static int rd_bytes(Rd *r, unsigned char *out, size_t want) {
    size_t have = r->n < want ? r->n : want;
    memcpy(out, r->b, have);
    memmove(r->b, r->b + have, r->n - have);
    r->n -= have;
    while (have < want) {
        double left = r->deadline - now_ms();
        struct pollfd pf = { r->fd, POLLIN, 0 };
        if (left <= 0 || poll(&pf, 1, (int)left + 1) <= 0) return -1;
        ssize_t got = read(r->fd, out + have, want - have);
        if (got <= 0) return -1;
        have += (size_t)got;
    }
    return 0;
}

static void put_str(char *out, size_t cap, size_t *o, const char *key, const char *val) {
    char q[WEBREG_URL_MAX * 2 + 64];
    json_quote(q, sizeof q, val);
    int w = snprintf(out + *o, cap - *o, ",\"%s\":%s", key, q);
    if (w > 0 && *o + (size_t)w < cap) *o += (size_t)w;
}

static char *answer_json(const Probe *pr, const char *err) {
    size_t cap = 4 * WEBREG_URL_MAX + 4096, o = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    if (err) { snprintf(out, cap, "{\"done\":true,\"ok\":false,\"error\":\"%s\"}", err); return out; }
    o = (size_t)snprintf(out, cap, "{\"done\":true,\"ok\":true,\"manifest\":%s,\"iconKind\":\"%s\"", pr->p.manifest ? "true" : "false",
                         pr->icon_kind == 1 ? "png" : pr->icon_kind == 2 ? "svg" : "");
    put_str(out, cap, &o, "name", pr->p.name);
    put_str(out, cap, &o, "startUrl", pr->p.start_url);
    put_str(out, cap, &o, "scope", pr->p.scope);
    put_str(out, cap, &o, "origin", pr->p.origin);
    put_str(out, cap, &o, "themeColor", pr->p.theme_color);
    put_str(out, cap, &o, "iconPath", pr->icon_path);
    if (o + 2 < cap) { out[o++] = '}'; out[o] = 0; }
    return out;
}

/* the icon where the Store's sheet can show it before anything is installed: $XDG_RUNTIME_DIR/lordos-web-probe */
static void probe_icon_file(Probe *pr) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char dir[PATH_MAX], path[PATH_MAX];
    if (!rt || rt[0] != '/' || snprintf(dir, sizeof dir, "%s/lordos-web-probe", rt) >= (int)sizeof dir) return;
    mkdir(dir, 0700);
    struct stat st;
    if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) return;
    if (snprintf(path, sizeof path, "%s/%d.%s", dir, pr->id, pr->icon_kind == 1 ? "png" : "svg") >= (int)sizeof path) return;
    if (!webreg_write_file(path, pr->icon, pr->icon_len)) snprintf(pr->icon_path, sizeof pr->icon_path, "%s", path);
}

/* One step of the probe: lord-web <mode> with url as one line on its fd 3 (docs/WEBAPPS.md: at most 2 KB, nothing
 * after the newline). Its pid, with our end of the channel in *fd; -1 if it could not start. */
static pid_t start_step(const char *mode, const char *url, int *fd) {
    int sv[2];
    char line[WEBREG_URL_MAX + 2];
    int n = snprintf(line, sizeof line, "%s\n", url);
    if (n <= 1 || n >= (int)sizeof line || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) return -1;
    char m[16];
    snprintf(m, sizeof m, "%s", mode);
    char *const argv[] = { "lord-web", m, NULL };
    pid_t pid = spawn(argv, sv[1]);
    close(sv[1]);
    if (pid < 0) { close(sv[0]); return -1; }
    /* small: it fits the socket's buffer, the write never waits */
    if (write(sv[0], line, (size_t)n) != n) { kill(-pid, SIGKILL); waitpid(pid, NULL, 0); close(sv[0]); return -1; }
    shutdown(sv[0], SHUT_WR);
    *fd = sv[0];
    return pid;
}

/* It has said everything (or failed): a moment to exit by itself, then it and its WebKit processes are ended. */
static void end_step(pid_t pid, int fd, int grace_ms) {
    if (fd >= 0) close(fd);
    int st;
    double t0 = now_ms();
    while (waitpid(pid, &st, WNOHANG) == 0) {
        if (now_ms() - t0 > grace_ms) { kill(-pid, SIGKILL); waitpid(pid, &st, 0); break; }
        struct timespec ts = { 0, 20 * 1000000 };
        nanosleep(&ts, NULL);
    }
}

/* The second step: lord-web --probe-icon with the icon the first one chose; its header line
 * {"ok":true,"type","size"} and then exactly size bytes. A bad or missing icon only means no icon. */
static void probe_icon(Probe *pr, Rd *r, char *line) {
    int fd = -1;
    pid_t pid = start_step("--probe-icon", pr->p.icon_url, &fd);
    if (pid < 0) return;
    r->fd = fd;
    r->n = 0;
    r->deadline = now_ms() + WEB_PROBE_MS;
    size_t size = 0;
    int svg = 0, ok = 0;
    char why[48];
    int l = rd_line(r, line, 4096);
    int h = l >= 0 ? webreg_icon_header(line, (size_t)l, &size, &svg, why, sizeof why) : -1;
    if (h == 1 && (pr->icon = malloc(size))) {
        if (!rd_bytes(r, pr->icon, size) && webreg_icon_kind(pr->icon, size) == (svg ? 2 : 1)) {
            pr->icon_kind = svg ? 2 : 1;
            pr->icon_len = size;
            probe_icon_file(pr);
            ok = 1;
        } else { free(pr->icon); pr->icon = NULL; }
    } else if (h == 0) fprintf(stderr, "webapps: no icon from %s: %s\n", pr->p.icon_url, why[0] ? why : "refused");
    end_step(pid, fd, ok ? 1000 : 0);
}

static void *probe_thread(void *arg) {
    Probe *slot = arg;
    pthread_mutex_lock(&pmu);
    Probe pr = *slot;                   /* our copy: the slot is only written back at the end */
    pthread_mutex_unlock(&pmu);
    Rd *r = malloc(sizeof *r);
    char *line = malloc(WEBREG_FILE_MAX + 1);
    const char *err = "webapps.probeFailed";
    if (r && line) {
        r->fd = pr.fd;
        r->n = 0;
        r->deadline = pr.started + WEB_PROBE_MS;
        int l = rd_line(r, line, WEBREG_FILE_MAX);
        if (l < 0) err = now_ms() >= r->deadline ? "webapps.probeTimeout" : "webapps.probeFailed";
        else if (webreg_probe_parse(line, (size_t)l, &pr.p)) err = pr.p.error[0] ? pr.p.error : "webapps.probeFailed";
        else err = NULL;
    }
    end_step(pr.pid, pr.fd, err ? 0 : 1000);
    pr.fd = -1;
    if (!err && r && line && pr.p.icon_url[0]) probe_icon(&pr, r, line);
    free(line);
    free(r);
    char *json = answer_json(&pr, err);
    pthread_mutex_lock(&pmu);
    *slot = pr;
    slot->fd = -1;
    slot->json = json;
    slot->done = 1;
    slot->finished = now_ms();
    pthread_mutex_unlock(&pmu);
    return NULL;
}

/* with pmu held */
static void probe_free(Probe *p) {
    if (p->icon_path[0]) unlink(p->icon_path);
    free(p->json);
    free(p->icon);
    memset(p, 0, sizeof *p);
}

int webmgr_probe(const char *url, const char **err) {
    *err = NULL;
    if (!url || strlen(url) >= 2048 || strncmp(url, "https://", 8) || webreg_url_origin(url, NULL, 0)) { *err = "webapps.badAddress"; return 0; }
    if (!webmgr_available()) { *err = "webapps.noRuntime"; return 0; }
    pthread_mutex_lock(&pmu);
    Probe *slot = NULL;
    for (int i = 0; i < WEB_PROBE_SLOTS && !slot; i++) if (!probes[i].used) slot = &probes[i];
    for (int i = 0; i < WEB_PROBE_SLOTS && !slot; i++)
        if (probes[i].done && now_ms() - probes[i].finished > PROBE_KEEP_MS) { probe_free(&probes[i]); slot = &probes[i]; }
    if (!slot) { pthread_mutex_unlock(&pmu); *err = "webapps.tooMany"; return 0; }
    slot->used = 1;
    slot->id = next_probe++;
    pthread_mutex_unlock(&pmu);

    int fd = -1;
    pid_t pid = start_step("--probe", url, &fd);
    if (pid < 0) goto fail;
    pthread_mutex_lock(&pmu);
    slot->pid = pid;
    slot->fd = fd;
    slot->started = now_ms();
    int id = slot->id;
    pthread_mutex_unlock(&pmu);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&th, &at, probe_thread, slot);
    pthread_attr_destroy(&at);
    if (r) { end_step(pid, fd, 0); goto fail; }
    return id;
fail:
    pthread_mutex_lock(&pmu);
    probe_free(slot);
    pthread_mutex_unlock(&pmu);
    *err = "webapps.probeFailed";
    return 0;
}

char *webmgr_probe_result(int id, int *gone) {
    *gone = 0;
    char *out = NULL;
    pthread_mutex_lock(&pmu);
    Probe *p = NULL;
    for (int i = 0; i < WEB_PROBE_SLOTS; i++) if (probes[i].used && probes[i].id == id) p = &probes[i];
    if (!p) *gone = 1;
    else if (p->done && p->json) out = strdup(p->json);
    pthread_mutex_unlock(&pmu);
    return out;
}

/* ---- installing ---- */

static int file_taken(const char *id, void *ctx) {
    (void)ctx;
    char path[PATH_MAX];
    struct stat st;
    return app_file(id, path, sizeof path) || lstat(path, &st) == 0;
}

/* writes the icon bytes as icons/<app>.<ext> and names it in a */
static const char *write_icon(WebApp *a, const unsigned char *b, size_t n) {
    int kind = webreg_icon_kind(b, n);
    if (!kind) return "webapps.badIcon";
    char dir[PATH_MAX], path[PATH_MAX + 64];
    if (sub_dir("icons", dir, sizeof dir)) return "webapps.writeFailed";
    snprintf(a->icon, sizeof a->icon, "icons/%s.%s", a->id, kind == 1 ? "png" : "svg");
    snprintf(path, sizeof path, "%s/%s", webmgr_config_dir(), a->icon);
    if (webreg_write_file(path, b, n)) { a->icon[0] = 0; return "webapps.writeFailed"; }
    return NULL;
}

static void remove_icons(const char *app) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/icons/%s.png", webmgr_config_dir(), app);
    unlink(path);
    snprintf(path, sizeof path, "%s/icons/%s.svg", webmgr_config_dir(), app);
    unlink(path);
}

static const char *first_profile(WebApp *a, const char *name) {
    if (!webreg_name_ok(name)) return "webapps.badName";
    a->nprofiles = 0;
    if (webreg_profile_new_id(a, name, a->profiles[0].id, sizeof a->profiles[0].id)) return "webapps.badName";
    snprintf(a->profiles[0].name, sizeof a->profiles[0].name, "%s", name);
    a->nprofiles = 1;
    return NULL;
}

const char *webmgr_install(const char *entry, size_t len, char *app_out, size_t n) {
    JDoc d;
    if (!entry || len > 8192 || json_parse(entry, len, &d, NULL, 0)) return "webapps.badEntry";
    const char *err = "webapps.badEntry";
    WebApp *a = malloc(sizeof *a);
    unsigned char *icon = NULL;
    size_t icon_len = 0;
    const JVal *r = d.root;
    if (!a || r->type != J_OBJ) goto out;
    for (const JVal *m = r->kid; m; m = m->next)              /* only these keys: nothing else reaches the file */
        if (strcmp(m->key, "source") && strcmp(m->key, "id") && strcmp(m->key, "probe") && strcmp(m->key, "name") && strcmp(m->key, "profile")) goto out;
    const char *source = json_str(r, "source"), *profile = json_str(r, "profile");
    if (!source || !profile) goto out;
    webreg_init(a);
    if (!strcmp(source, "catalog")) {
        const char *id = json_str(r, "id");
        char ipath[PATH_MAX];
        if (!id || !webreg_id_ok(id) || json_get(r, "probe") || json_get(r, "name")) goto out;
        if (catalog_entry(id, a, ipath, sizeof ipath)) { err = "webapps.notInCatalog"; goto out; }
        if (file_taken(id, NULL)) { err = "webapps.exists"; goto out; }
        if (ipath[0]) icon = (unsigned char *)webreg_read_file(ipath, WEBREG_ICON_BYTES, &icon_len);
    } else if (!strcmp(source, "url")) {
        const JVal *pv = json_get(r, "probe");
        const char *name = json_str(r, "name");
        if (!pv || pv->type != J_NUM || json_get(r, "id")) goto out;
        int pid = (int)pv->num;
        pthread_mutex_lock(&pmu);
        Probe *p = NULL;
        for (int i = 0; i < WEB_PROBE_SLOTS; i++) if (probes[i].used && probes[i].done && probes[i].id == pid && probes[i].p.ok) p = &probes[i];
        if (p) {
            snprintf(a->name, sizeof a->name, "%s", name ? name : p->p.name);
            snprintf(a->start_url, sizeof a->start_url, "%s", p->p.start_url);
            snprintf(a->scope, sizeof a->scope, "%s", p->p.scope);
            snprintf(a->origin, sizeof a->origin, "%s", p->p.origin);
            snprintf(a->theme_color, sizeof a->theme_color, "%s", p->p.theme_color);
            if (p->icon && (icon = malloc(p->icon_len))) { memcpy(icon, p->icon, p->icon_len); icon_len = p->icon_len; }
        }
        pthread_mutex_unlock(&pmu);
        if (!p) { err = "webapps.probeGone"; goto out; }
        if (!webreg_name_ok(a->name)) { err = "webapps.badName"; goto out; }
        if (webreg_slug(a->name, a->id, sizeof a->id, file_taken, NULL)) { err = "webapps.badName"; goto out; }
        a->source = WEBREG_SRC_URL;
    } else goto out;
    if ((err = first_profile(a, profile))) goto out;
    if (webreg_check(a, NULL, 0)) { err = "webapps.badEntry"; goto out; }
    if (icon && write_icon(a, icon, icon_len)) a->icon[0] = 0;     /* without its icon it still installs (the initial is drawn) */
    if ((err = save(a))) { remove_icons(a->id); goto out; }
    snprintf(app_out, n, "%s", a->id);
    err = NULL;
    if (!strcmp(source, "url")) {                               /* used: its icon file goes */
        int pid = (int)json_num(r, "probe", 0);
        pthread_mutex_lock(&pmu);
        for (int i = 0; i < WEB_PROBE_SLOTS; i++) if (probes[i].used && probes[i].done && probes[i].id == pid) probe_free(&probes[i]);
        pthread_mutex_unlock(&pmu);
    }
out:
    free(icon);
    free(a);
    json_free(&d);
    return err;
}

/* ---- changing an installed app ---- */

static const char *with_app(const char *app, WebApp **a) {
    if (!webreg_id_ok(app)) return "webapps.badId";
    *a = malloc(sizeof **a);
    if (!*a) return "webapps.writeFailed";
    const char *e = webmgr_load(app, *a);
    if (e) { free(*a); *a = NULL; }
    return e;
}

const char *webmgr_add_profile(const char *app, const char *name, char *id, size_t n) {
    WebApp *a;
    const char *e = with_app(app, &a);
    if (e) return e;
    if (!webreg_name_ok(name)) e = "webapps.badName";
    else if (a->nprofiles >= WEBREG_PROFILES_MAX) e = "webapps.tooManyProfiles";
    else if (webreg_profile_new_id(a, name, a->profiles[a->nprofiles].id, sizeof a->profiles[0].id)) e = "webapps.badName";
    else {
        WebProfile *p = &a->profiles[a->nprofiles++];
        snprintf(p->name, sizeof p->name, "%s", name);
        snprintf(id, n, "%s", p->id);
        e = save(a);
    }
    free(a);
    return e;
}

const char *webmgr_rename_profile(const char *app, const char *profile, const char *name) {
    WebApp *a;
    const char *e = with_app(app, &a);
    if (e) return e;
    int i = webreg_profile_index(a, profile);
    if (i < 0) e = "webapps.noProfile";
    else if (!webreg_name_ok(name)) e = "webapps.badName";
    else { snprintf(a->profiles[i].name, sizeof a->profiles[i].name, "%s", name); e = save(a); }
    free(a);
    return e;
}

/* Removes a folder and everything in it without following links (FTW_PHYS) or leaving its file system. */
static int rm_cb(const char *p, const struct stat *st, int type, struct FTW *f) {
    (void)st; (void)f;
    return type == FTW_DP ? rmdir(p) : unlink(p);
}
static int rm_tree(const char *dir) {
    struct stat st;
    if (lstat(dir, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(dir);              /* a link put there: only the link goes */
    return nftw(dir, rm_cb, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
}

const char *webmgr_remove_profile(const char *app, const char *profile, int delete_data) {
    WebApp *a;
    const char *e = with_app(app, &a);
    if (e) return e;
    int i = webreg_profile_index(a, profile);
    char dir[PATH_MAX];
    if (i < 0) e = "webapps.noProfile";
    else if (a->nprofiles < 2) e = "webapps.lastProfile";
    else {
        end_app(app, profile);
        memmove(&a->profiles[i], &a->profiles[i + 1], (size_t)(a->nprofiles - i - 1) * sizeof a->profiles[0]);
        a->nprofiles--;
        e = save(a);
        if (!e && delete_data && (webmgr_data_dir(app, profile, dir, sizeof dir) || rm_tree(dir))) e = "webapps.deleteFailed";
    }
    free(a);
    return e;
}

const char *webmgr_set_permission(const char *app, const char *kind, const char *value) {
    int k = webreg_perm_kind(kind), v = webreg_perm_value(value);
    if (k < 0 || v < 0) return "webapps.badPermission";
    WebApp *a;
    const char *e = with_app(app, &a);
    if (e) return e;
    if (a->perms[k] != v) { a->perms[k] = v; e = save(a); }
    free(a);
    return e;
}

const char *webmgr_set_background(const char *app, int on) {
    WebApp *a;
    const char *e = with_app(app, &a);
    if (e) return e;
    if (a->background != !!on) { a->background = !!on; e = save(a); }
    free(a);
    return e;
}

const char *webmgr_uninstall(const char *app, int delete_data) {
    WebApp *a;
    const char *e = with_app(app, &a);
    if (e && strcmp(e, "webapps.badRegistry")) return e;       /* a broken file can still be removed */
    free(a);
    end_app(app, NULL);
    char path[PATH_MAX];
    if (app_file(app, path, sizeof path) || unlink(path)) return "webapps.deleteFailed";
    remove_icons(app);
    if (delete_data && (webmgr_data_dir(app, NULL, path, sizeof path) || rm_tree(path))) return "webapps.deleteFailed";
    return NULL;
}

/* ---- starting ---- */

const char *webmgr_launch(const char *app, const char *profile, int *raised) {
    *raised = 0;
    if (!webreg_id_ok(app) || !webreg_id_ok(profile)) return "webapps.badId";
    WebApp *a;
    const char *e = with_app(app, &a);
    if (e) return e;
    int known = webreg_profile_index(a, profile) >= 0;
    free(a);
    if (!known) return "webapps.noProfile";
    reap();
    if (kid_of(app, profile)) { *raised = 1; return NULL; }
    if (!webmgr_available()) return "webapps.noRuntime";
    Kid *k = NULL;
    for (int i = 0; i < WEB_MAX_RUNNING && !k; i++) if (!kids[i].used) k = &kids[i];
    if (!k) return "webapps.tooMany";
    char dir[PATH_MAX];
    if (webmgr_data_dir(app, profile, dir, sizeof dir) || mkdirs(dir)) return "webapps.writeFailed";
    char a_arg[WEBREG_ID_MAX + 1], p_arg[WEBREG_ID_MAX + 1];
    snprintf(a_arg, sizeof a_arg, "%s", app);
    snprintf(p_arg, sizeof p_arg, "%s", profile);
    char *const argv[] = { "lord-web", "--app", a_arg, "--profile", p_arg, NULL };
    pid_t pid = spawn(argv, -1);
    if (pid < 0) return "webapps.startFailed";
    k->used = 1;
    k->pid = pid;
    snprintf(k->app, sizeof k->app, "%s", app);
    snprintf(k->profile, sizeof k->profile, "%s", profile);
    return NULL;
}
