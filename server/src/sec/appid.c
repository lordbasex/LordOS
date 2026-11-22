/* appid.c — app of a process by its cgroup (see appid.h). */
#include "appid.h"
#include "../fw/rules.h"

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Only the root-owned session subtree of lordos.service counts: a cgroup the user creates under
 * user@<uid>.service with the same names must never pose as an app. */
int appid_of_cgroup(const char *path, char *out, size_t n) {
    static const char base[] = "/system.slice/lordos.service/lordos-session-";
    if (strncmp(path, base, sizeof base - 1)) return -1;
    const char *p = path + sizeof base - 1;
    char *end;
    unsigned long uid = strtoul(p, &end, 10);
    if (end == p || *end != '/' || uid != (unsigned long)getuid()) return -1;
    p = end + 1;
    char scope[FW_APP_MAX + 32];
    size_t l = strcspn(p, "/\n");
    if (l < 18 || l >= sizeof scope || strncmp(p, "app-lordos-", 11) || strncmp(p + l - 6, ".scope", 6)) return -1;
    memcpy(scope, p, l);
    scope[l] = 0;
    return fw_app_canon(scope, out, n);
}

int appid_of_pid(unsigned pid, char *out, size_t n) {
    char f[64], line[1024];
    snprintf(f, sizeof f, "/proc/%u/cgroup", pid);
    FILE *fp = fopen(f, "re");
    if (!fp) return -1;
    int r = -1;
    while (fgets(line, sizeof line, fp))
        if (!strncmp(line, "0::", 3)) { r = appid_of_cgroup(line + 3, out, n) ? 1 : 0; break; }
    fclose(fp);
    return r;
}

int appid_running(int (*match)(const char *app, void *u), void *u) {
    char dir[96];
    snprintf(dir, sizeof dir, "/sys/fs/cgroup/system.slice/lordos.service/lordos-session-%u", (unsigned)getuid());
    DIR *d = opendir(dir);
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        char path[512], app[FW_APP_MAX];
        if (strncmp(e->d_name, "app-lordos-", 11)) continue;
        /* an app that has exited leaves its scope behind, empty, until the session ends: not running */
        char ev[512], line[64];
        snprintf(ev, sizeof ev, "%s/%s/cgroup.events", dir, e->d_name);
        FILE *f = fopen(ev, "re");
        int populated = 1;                          /* unreadable: counted, as before */
        while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "populated ", 10)) { populated = line[10] != '0'; break; }
        if (f) fclose(f);
        if (!populated) continue;
        snprintf(path, sizeof path, "/system.slice/lordos.service/lordos-session-%u/%s", (unsigned)getuid(), e->d_name);
        if (!appid_of_cgroup(path, app, sizeof app)) found = match(app, u);
    }
    closedir(d);
    return found;
}

/* ---- who a process belongs to, only to show it (see appid.h) ------------------------------------------------------ */

int appid_session_owner(const char *cg, char *out, size_t n) {
    char base[96];
    int bl = snprintf(base, sizeof base, "/system.slice/lordos.service/lordos-session-%u", (unsigned)getuid());
    if (!cg || !out || !n || bl < 0 || strncmp(cg, base, (size_t)bl) || (cg[bl] && cg[bl] != '/' && cg[bl] != '\n')) return -1;
    if (!appid_of_cgroup(cg, out, n)) return 0;
    if (cg[bl] == '/') {
        char seg[128];
        const char *p = cg + bl + 1;
        size_t l = strcspn(p, "/\n");
        if (l >= sizeof seg) return -1;
        memcpy(seg, p, l);
        seg[l] = 0;
        if (!fw_component_scope(seg, out, n)) return 0;
        /* an app-lordos-* name that isn't an app (the machines' prefix misused, a bad id): nobody's */
        if (!strncmp(seg, "app-lordos-", 11)) return -1;
    }
    if (n < sizeof "lordos") return -1;
    memcpy(out, "lordos", sizeof "lordos");
    return 2;
}

/* in the user's own service manager, but not the manager itself (its init.scope) */
static int in_user_manager_tree(const char *cg) {
    char pre[128];
    unsigned u = (unsigned)getuid();
    int l = snprintf(pre, sizeof pre, "/user.slice/user-%u.slice/user@%u.service/", u, u);
    if (l < 0 || strncmp(cg, pre, (size_t)l)) return 0;
    const char *rest = cg + l;
    return *rest && strncmp(rest, "init.scope", 10) != 0;
}

typedef struct { const AppidView *v; unsigned parent; char app[FW_APP_MAX]; int found, mixed; } ChildScan;

static void child_each(void *ctx, const char *cg, unsigned pid) {
    ChildScan *s = ctx;
    char app[FW_APP_MAX];
    if (s->mixed || s->v->parent(s->v->u, pid) != s->parent) return;
    if (appid_session_owner(cg, app, sizeof app) != 0) return;
    if (!s->found) { snprintf(s->app, sizeof s->app, "%s", app); s->found = 1; }
    else if (strcmp(s->app, app)) s->mixed = 1;
}

static int put(char *out, size_t n, const char *app) {
    size_t l = strlen(app);
    if (l + 1 > n) { out[0] = 0; return -1; }
    memcpy(out, app, l + 1);
    return 0;
}

int appid_lineage_in(const AppidView *v, unsigned pid, char *out, size_t n) {
    char cg[1024], app[FW_APP_MAX];
    if (!out || !n) return -1;
    out[0] = 0;
    if (!v || !pid || v->cgroup(v->u, pid, cg, sizeof cg)) return -1;
    if (appid_session_owner(cg, app, sizeof app) >= 0) return put(out, n, app);
    for (int depth = 0; depth < 16; depth++) {
        if (!in_user_manager_tree(cg) || !v->mine(v->u, pid)) return -1;
        /* a child it made while it was still in a scope */
        ChildScan s = { .v = v, .parent = pid };
        v->session_procs(v->u, child_each, &s);
        if (s.mixed) return -1;
        if (s.found) return put(out, n, s.app);
        unsigned pp = v->parent(v->u, pid);
        if (pp <= 1 || pp == pid || v->cgroup(v->u, pp, cg, sizeof cg)) return -1;
        /* the parent in an app's or a component's scope. Never "LordOS itself" from a parent: lord-server starts every
         * app, so one that left its scope at once would pass for LordOS */
        int r = appid_session_owner(cg, app, sizeof app);
        if (r == 0) return put(out, n, app);
        if (r > 0) return -1;
        pid = pp;
    }
    return -1;
}

static int real_cgroup(void *u, unsigned pid, char *out, size_t n) {
    (void)u;
    char f[64], line[1024];
    snprintf(f, sizeof f, "/proc/%u/cgroup", pid);
    FILE *fp = fopen(f, "re");
    if (!fp) return -1;
    int r = -1;
    while (fgets(line, sizeof line, fp))
        if (!strncmp(line, "0::", 3)) {
            line[strcspn(line, "\n")] = 0;
            r = put(out, n, line + 3);
            break;
        }
    fclose(fp);
    return r;
}

static unsigned real_parent(void *u, unsigned pid) {
    (void)u;
    char f[64], line[256];
    snprintf(f, sizeof f, "/proc/%u/status", pid);
    FILE *fp = fopen(f, "re");
    unsigned pp = 0;
    while (fp && fgets(line, sizeof line, fp))
        if (!strncmp(line, "PPid:", 5)) { pp = (unsigned)strtoul(line + 5, NULL, 10); break; }
    if (fp) fclose(fp);
    return pp;
}

static int real_mine(void *u, unsigned pid) {
    (void)u;
    char f[48];
    struct stat st;
    snprintf(f, sizeof f, "/proc/%u", pid);
    return !stat(f, &st) && st.st_uid == getuid();
}

static void procs_of(const char *dir, const char *cg, int depth, void (*each)(void *, const char *, unsigned), void *ctx) {
    char f[640], line[32];
    struct stat st;
    if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0) return;     /* only folders root owns */
    snprintf(f, sizeof f, "%s/cgroup.procs", dir);
    FILE *fp = fopen(f, "re");
    while (fp && fgets(line, sizeof line, fp)) {
        unsigned long p = strtoul(line, NULL, 10);
        if (p > 0 && p < 0x7fffffffUL) each(ctx, cg, (unsigned)p);
    }
    if (fp) fclose(fp);
    if (depth >= 2) return;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || e->d_type != DT_DIR) continue;
        char sub[512], subcg[512];
        if ((size_t)snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name) >= sizeof sub) continue;
        if ((size_t)snprintf(subcg, sizeof subcg, "%s/%s", cg, e->d_name) >= sizeof subcg) continue;
        procs_of(sub, subcg, depth + 1, each, ctx);
    }
    if (d) closedir(d);
}

void appid_session_procs(void (*each)(void *ctx, const char *cg, unsigned pid), void *ctx) {
    char cg[96], dir[128];
    snprintf(cg, sizeof cg, "/system.slice/lordos.service/lordos-session-%u", (unsigned)getuid());
    snprintf(dir, sizeof dir, "/sys/fs/cgroup%s", cg);
    procs_of(dir, cg, 0, each, ctx);
}

static void real_session_procs(void *u, void (*each)(void *, const char *, unsigned), void *ctx) { (void)u; appid_session_procs(each, ctx); }

int appid_of_lineage(unsigned pid, char *out, size_t n) {
    static const AppidView real = { real_cgroup, real_parent, real_mine, real_session_procs, NULL };
    return appid_lineage_in(&real, pid, out, n);
}

int appid_exe_base(const char *link, char *out, size_t n) {
    if (!out || n < 2) return -1;
    out[0] = 0;
    if (!link) return -1;
    char t[512];
    size_t k = strnlen(link, sizeof t - 1);
    memcpy(t, link, k);
    t[k] = 0;
    size_t dl = sizeof " (deleted)" - 1;
    if (k > dl && !strcmp(t + k - dl, " (deleted)")) t[k - dl] = 0;
    const char *b = strrchr(t, '/');
    b = b ? b + 1 : t;
    size_t w = 0;
    for (; *b && w + 1 < n; b++) {
        char c = *b;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' || c == '-') out[w++] = c;
    }
    out[w] = 0;
    return w ? 0 : -1;
}

int appid_exe_name(unsigned pid, char *out, size_t n) {
    char f[48], t[512];
    if (out && n) out[0] = 0;
    snprintf(f, sizeof f, "/proc/%u/exe", pid);
    ssize_t k = readlink(f, t, sizeof t - 1);
    if (k <= 0) return -1;
    t[k] = 0;
    return appid_exe_base(t, out, n);
}
