/* apps.c — installed Linux apps: reads the .desktop files from /usr/share/applications and
 * ~/.local/share/applications (Desktop Entry Specification 1.5), finds their PNG icon and launches them.
 *
 * It is the second exception to "the server does not run commands" (approved for M7; the first
 * is the Terminal's shell). Limits:
 *   - an app is only launched by its id, and the id must belong to a .desktop file in those two
 *     folders that passes the apps_list() filters;
 *   - the command is that file's Exec line; the caller adds no arguments or
 *     files (the %f %u… codes are removed) and it does not go through a shell: argv is built per the
 *     specification and run with execv;
 *   - terminal apps (Terminal=true) are neither listed nor launched;
 *   - JS cannot create .desktop files: lord.fs is read-only and LordOS app packages are installed
 *     in another folder (~/.local/share/lordos/apps), so a command cannot be
 *     invented from the UI.
 * App output goes to ~/.cache/lordos/apps.log (trimmed when it exceeds 1 MB).
 *
 * An app the owner restricted with "no access to your files" (Settings → Privacy, firewall id
 * desktop:<id>, sec/restrict.h) starts under Landlock (sandbox_apply_files): only its own config,
 * cache and data folders, the system read-only, /tmp and the runtime dir, plus the one file or
 * folder it was opened with. A restriction set while the app runs applies at its next launch. */
#include "apps.h"
#include "hw/defaults.h"
#include "hw/mimeapps.h"
#include "fsops.h"
#include "sec/cgroup.h"
#include "sec/restrict.h"
#include "sec/sandbox.h"
#include "fw/rules.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define MAX_APPS 1024

static DesktopApp *apps;
static int napps;
static char cached_lang[32];
static long long cached_mtime[2];      /* the folders' mtimes in ns: a file added in the same second still shows */

/* ---- reading a .desktop file ---- */

/* Removes the escapes of a string-type value (\s \n \t \r \\). */
static void unescape(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 's' ? ' ' : *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'r' ? '\r' : *s;
        } else *o++ = *s;
    }
    *o = 0;
}

/* Priority of a translated key for the language: Name[es_AR] > Name[es] > Name. */
static int locale_rank(const char *loc, const char *lang) {
    if (!loc) return 1;
    char ll[8] = "", cc[8] = "";
    sscanf(lang, "%7[a-z]%*[-_]%7[A-Za-z]", ll, cc);
    char want[20];
    snprintf(want, sizeof want, "%s_%s", ll, cc);
    if (cc[0] && !strcmp(loc, want)) return 3;
    if (!strcmp(loc, ll)) return 2;
    return 0;
}

/* A desktop action ("[Desktop Action <name>]", Desktop Entry Specification, "Additional
 * applications actions"): its name, translated label and Exec line. Only the actions listed in
 * the main group's Actions= key count. */
typedef struct {
    char name[APPS_ACTION_NAME_MAX], label[160], exec[1024];
    int rank, has_label;
} Action;

typedef struct {
    DesktopApp a;
    char exec[1024], tryexec[256], icon_name[256], path[512];
    char only[256], not_[256], actions[512];
    int ranks[5];                       /* name generic comment keywords: language priority */
    int hidden, nodisplay, terminal, is_app, has_name;
    Action act[APPS_MAX_ACTIONS];       /* the [Desktop Action …] groups seen (valid names only) */
    int nact;
} Entry;

/* An action identifier the launcher accepts: [A-Za-z0-9-] (the specification's charset), at most
 * 63 characters. */
static int valid_action(const char *s) {
    size_t n = strlen(s);
    if (!n || n >= APPS_ACTION_NAME_MAX) return 0;
    for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-') return 0;
    return 1;
}

/* "Key[loc]=value" → key, loc (or NULL) and value, in place. 0 if the line has no '='. */
static int split_kv(char *line, char **k, char **loc, char **v) {
    char *eq = strchr(line, '=');
    if (!eq) return 0;
    *k = line; *v = eq + 1;
    char *ke = eq;
    while (ke > *k && ke[-1] == ' ') ke--;
    *ke = 0;
    while (**v == ' ') (*v)++;
    *loc = NULL;
    char *br = strchr(*k, '[');
    if (br) {
        *br = 0; *loc = br + 1;
        char *cb = strchr(*loc, ']'); if (cb) *cb = 0;
        char *at = strchr(*loc, '@'); if (at) *at = 0;
        char *dot = strchr(*loc, '.'); if (dot) *dot = 0;
    }
    return 1;
}

static void set_tr(Entry *e, int slot, char *dst, size_t n, const char *val, const char *loc, const char *lang) {
    int r = locale_rank(loc, lang);
    if (!r && loc) return;
    if (r < e->ranks[slot]) return;
    e->ranks[slot] = r;
    snprintf(dst, n, "%s", val);
}

static int parse_desktop(const char *file, const char *lang, Entry *e) {
    FILE *f = fopen(file, "r");
    if (!f) return -1;
    memset(e, 0, sizeof *e);
    char line[2048];
    int in_main = 0;
    Action *act = NULL;                 /* inside a [Desktop Action …] group that is kept */
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') {
            static const char pre[] = "[Desktop Action ";
            size_t len = strlen(line);
            in_main = !strcmp(line, "[Desktop Entry]");
            act = NULL;
            if (!strncmp(line, pre, sizeof pre - 1) && len > sizeof pre && line[len - 1] == ']') {
                line[len - 1] = 0;
                const char *name = line + sizeof pre - 1;
                int dup = 0;
                for (int i = 0; i < e->nact; i++) if (!strcmp(e->act[i].name, name)) dup = 1;
                if (valid_action(name) && !dup && e->nact < APPS_MAX_ACTIONS) {
                    act = &e->act[e->nact++];
                    /* bounded copy (valid_action already keeps it shorter than the field); the
                     * precision keeps -Wformat-truncation quiet */
                    snprintf(act->name, sizeof act->name, "%.*s", (int)sizeof act->name - 1, name);
                }
            }
            continue;
        }
        if ((!in_main && !act) || line[0] == '#' || !line[0]) continue;
        char *k, *loc, *v;
        if (!split_kv(line, &k, &loc, &v)) continue;
        if (act) {
            if (!strcmp(k, "Name")) {
                int r = locale_rank(loc, lang);
                if ((r || !loc) && r >= act->rank) { act->rank = r; snprintf(act->label, sizeof act->label, "%s", v); act->has_label = 1; }
            } else if (!loc && !strcmp(k, "Exec")) snprintf(act->exec, sizeof act->exec, "%s", v);
            continue;
        }
        if (!strcmp(k, "Name")) {
            set_tr(e, 0, e->a.name, sizeof e->a.name, v, loc, lang); e->has_name = 1;
            if (!loc) snprintf(e->a.name_c, sizeof e->a.name_c, "%s", v);
        }
        else if (!strcmp(k, "GenericName")) set_tr(e, 1, e->a.generic, sizeof e->a.generic, v, loc, lang);
        else if (!strcmp(k, "Comment")) set_tr(e, 2, e->a.comment, sizeof e->a.comment, v, loc, lang);
        else if (!strcmp(k, "Keywords")) set_tr(e, 3, e->a.keywords, sizeof e->a.keywords, v, loc, lang);
        else if (loc) continue;
        else if (!strcmp(k, "Type")) e->is_app = !strcmp(v, "Application");
        else if (!strcmp(k, "Exec")) snprintf(e->exec, sizeof e->exec, "%s", v);
        else if (!strcmp(k, "TryExec")) snprintf(e->tryexec, sizeof e->tryexec, "%s", v);
        else if (!strcmp(k, "Icon")) snprintf(e->icon_name, sizeof e->icon_name, "%s", v);
        else if (!strcmp(k, "Path")) snprintf(e->path, sizeof e->path, "%s", v);
        else if (!strcmp(k, "Categories")) snprintf(e->a.categories, sizeof e->a.categories, "%s", v);
        else if (!strcmp(k, "MimeType")) snprintf(e->a.mime, sizeof e->a.mime, "%s", v);
        else if (!strcmp(k, "StartupWMClass")) snprintf(e->a.wmclass, sizeof e->a.wmclass, "%s", v);
        else if (!strcmp(k, "OnlyShowIn")) snprintf(e->only, sizeof e->only, "%s", v);
        else if (!strcmp(k, "NotShowIn")) snprintf(e->not_, sizeof e->not_, "%s", v);
        else if (!strcmp(k, "Hidden")) e->hidden = !strcmp(v, "true");
        else if (!strcmp(k, "NoDisplay")) e->nodisplay = !strcmp(v, "true");
        else if (!strcmp(k, "Terminal")) e->terminal = !strcmp(v, "true");
        else if (!strcmp(k, "Actions")) snprintf(e->actions, sizeof e->actions, "%s", v);
    }
    fclose(f);
    unescape(e->a.name); unescape(e->a.name_c); unescape(e->a.generic); unescape(e->a.comment);
    for (int i = 0; i < e->nact; i++) unescape(e->act[i].label);
    snprintf(e->a.file, sizeof e->a.file, "%s", file);
    return 0;
}

/* Program name of an Exec line ("env VAR=1 /usr/bin/foo %u" → "foo"), only to
 * recognize its processes in the Monitor; never used to launch anything. */
static void exec_bin(const char *exec, char *out, size_t n) {
    out[0] = 0;
    const char *p = exec;
    for (;;) {
        while (*p == ' ') p++;
        if (!*p) return;
        const char *q = p;
        char quote = *p == '"' ? '"' : 0;
        if (quote) q = ++p;
        while (*q && (quote ? *q != '"' : *q != ' ')) q++;
        size_t len = (size_t)(q - p);
        int is_env = len == 3 && !strncmp(p, "env", 3), is_var = memchr(p, '=', len) != NULL;
        if (!is_env && !is_var) {
            const char *base = p;
            for (const char *c = p; c < q; c++) if (*c == '/') base = c + 1;
            snprintf(out, n, "%.*s", (int)(q - base), base);
            return;
        }
        p = *q ? q + 1 : q;
    }
}

static int list_has(const char *list, const char *item) {
    size_t n = strlen(item);
    for (const char *p = list; *p;) {
        const char *q = strchr(p, ';');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (len == n && !strncmp(p, item, n)) return 1;
        if (!q) break;
        p = q + 1;
    }
    return 0;
}

/* Program of the Exec line in the fixed PATH (plus ~/.local/bin for the user's .desktop files). */
static int find_program(const char *prog, int user, char *out, size_t n) {
    if (!prog[0]) return -1;
    if (strchr(prog, '/')) {
        if (prog[0] != '/' || access(prog, X_OK)) return -1;
        snprintf(out, n, "%s", prog);
        return 0;
    }
    const char *home = getenv("HOME");
    const char *dirs[] = { "/usr/local/bin", "/usr/bin", "/bin", "/usr/local/games", "/usr/games", NULL, NULL };
    char local[PATH_MAX];
    if (user && home) { snprintf(local, sizeof local, "%s/.local/bin", home); dirs[5] = local; }
    for (int i = 0; dirs[i]; i++) {
        snprintf(out, n, "%s/%s", dirs[i], prog);
        struct stat st;
        if (!stat(out, &st) && S_ISREG(st.st_mode) && !access(out, X_OK)) return 0;
    }
    return -1;
}

/* ---- icons: hicolor PNGs (and from the user's theme), or /usr/share/pixmaps ---- */

static int is_png(const char *path) {
    size_t n = strlen(path);
    struct stat st;
    return n > 4 && !strcmp(path + n - 4, ".png") && !stat(path, &st) && S_ISREG(st.st_mode);
}

static void find_icon(const char *name, char *out, size_t n) {
    out[0] = 0;
    if (!name[0] || strstr(name, "..")) return;
    if (name[0] == '/') { if (is_png(name)) snprintf(out, n, "%s", name); return; }
    static const int sizes[] = { 64, 48, 128, 96, 256, 72, 32, 512 };
    const char *home = getenv("HOME");
    char roots[3][PATH_MAX];
    int nr = 0;
    if (home) snprintf(roots[nr++], PATH_MAX, "%s/.local/share/icons/hicolor", home);
    snprintf(roots[nr++], PATH_MAX, "/usr/share/icons/hicolor");
    for (int r = 0; r < nr; r++)
        for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
            int w = snprintf(out, n, "%s/%dx%d/apps/%s.png", roots[r], sizes[s], sizes[s], name);
            if (w > 0 && (size_t)w < n && is_png(out)) return;
        }
    int w = snprintf(out, n, "/usr/share/pixmaps/%s.png", name);
    if (w > 0 && (size_t)w < n && is_png(out)) return;
    /* last resort: the 64 px AppStream PNG downloaded by the package engine
     * (/var/lib/lordos/pkg/icons/64/<package>_<icon>.png): useful for apps that only ship SVG */
    out[0] = 0;
    DIR *d = opendir("/var/lib/lordos/pkg/icons/64");
    if (!d) return;
    char want[300];
    snprintf(want, sizeof want, "_%s.png", name);
    size_t wl = strlen(want);
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l > wl && !strcmp(e->d_name + l - wl, want)) {
            w = snprintf(out, n, "/var/lib/lordos/pkg/icons/64/%s", e->d_name);
            if (w > 0 && (size_t)w < n && is_png(out)) break;
            out[0] = 0;
        }
    }
    closedir(d);
}

/* ---- list ---- */

/* A desktop id the launcher can run: it must also make a valid app scope name
 * ("app-lordos-<id>.scope", cgroup_scope_name_ok: no '+', at most 62 characters, no "..") and
 * never one that reads as a .lpk package's scope ("lpk-...": that would be lpk:<rest>'s identity,
 * rules and restrictions). */
#define APP_ID_MAX 62
static int valid_id(const char *id) {
    size_t n = strlen(id);
    if (!n || n >= sizeof apps[0].id || n > APP_ID_MAX) return 0;
    for (const char *p = id; *p; p++) if (!isalnum((unsigned char)*p) && !strchr("._-", *p)) return 0;
    return id[0] != '.' && id[0] != '-' && !strstr(id, "..") && strncmp(id, "lpk-", 4);
}

static void app_dirs(char dirs[2][PATH_MAX]) {
    const char *home = getenv("HOME");
    snprintf(dirs[0], PATH_MAX, "%s/.local/share/applications", home ? home : "/nonexistent");
    snprintf(dirs[1], PATH_MAX, "%s/usr/share/applications", defaults_root());   /* root: "" outside tests */
}

static int cmp_name(const void *a, const void *b) {
    return strcasecmp(((const DesktopApp *)a)->name, ((const DesktopApp *)b)->name);
}

/* Is the id already there? (the user's come first and shadow the system's) */
static int seen(const char *id, char (*ids)[160], int n) {
    for (int i = 0; i < n; i++) if (!strcmp(ids[i], id)) return 1;
    return 0;
}

int apps_list(const char *lang, const DesktopApp **out) {
    char dirs[2][PATH_MAX];
    app_dirs(dirs);
    long long mt[2];
    for (int d = 0; d < 2; d++) { struct stat st; mt[d] = stat(dirs[d], &st) ? 0 : st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec; }
    if (apps && !strcmp(cached_lang, lang) && mt[0] == cached_mtime[0] && mt[1] == cached_mtime[1]) { *out = apps; return napps; }

    DesktopApp *list = calloc(MAX_APPS, sizeof *list);
    char (*ids)[160] = calloc(MAX_APPS * 2, sizeof *ids);
    Entry *e = malloc(sizeof *e);
    int n = 0, nids = 0;
    for (int d = 0; d < 2 && list && ids && e; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *de;
        while ((de = readdir(dir)) && n < MAX_APPS) {
            size_t len = strlen(de->d_name);
            if (len < 9 || strcmp(de->d_name + len - 8, ".desktop")) continue;
            /* the id is the file name without .desktop, with any character outside [A-Za-z0-9._-] made
             * '-' ("LordOS Imager.desktop" → "LordOS-Imager"): packages name their files freely, but the
             * id also names the app's scope (app-lordos-<id>.scope); the real file is kept in a->file */
            char id[160];
            snprintf(id, sizeof id, "%.*s", (int)(len - 8), de->d_name);
            for (char *q = id; *q; q++) if (!isalnum((unsigned char)*q) && !strchr("._-", *q)) *q = '-';
            if (!valid_id(id) || seen(id, ids, nids)) continue;
            if (nids < MAX_APPS * 2) snprintf(ids[nids++], sizeof ids[0], "%s", id);   /* it also shadows if hidden */
            char file[PATH_MAX * 2];
            int flen = snprintf(file, sizeof file, "%s/%s", dirs[d], de->d_name);
            if (flen < 0 || flen >= PATH_MAX || parse_desktop(file, lang, e)) continue;
            if (!e->is_app || e->hidden || e->nodisplay || e->terminal || !e->has_name || !e->exec[0]) continue;
            if (e->only[0] && !list_has(e->only, "LordOS")) continue;
            if (e->not_[0] && list_has(e->not_, "LordOS")) continue;
            char prog[PATH_MAX];
            if (e->tryexec[0] && find_program(e->tryexec, d == 0, prog, sizeof prog)) continue;
            DesktopApp *a = &list[n++];
            *a = e->a;
            snprintf(a->id, sizeof a->id, "%s", id);
            a->user = d == 0;
            find_icon(e->icon_name, a->icon, sizeof a->icon);
            exec_bin(e->exec, a->bin, sizeof a->bin);
        }
        closedir(dir);
    }
    free(ids);
    free(e);
    if (!list) return 0;
    qsort(list, (size_t)n, sizeof *list, cmp_name);
    free(apps);
    apps = list;
    napps = n;
    snprintf(cached_lang, sizeof cached_lang, "%s", lang);
    cached_mtime[0] = mt[0];
    cached_mtime[1] = mt[1];
    *out = apps;
    return napps;
}

/* ---- launch ---- */

/* Builds argv from the Exec line (specification, "The Exec key"): arguments separated by
 * spaces, double quotes with \" \` \$ \\, field codes. With file (apps_launch_with), the
 * first %f/%F goes as the path and the first %u/%U as file://…; the other file codes
 * are removed. %i → --icon <icon>; %c → name; %k → the .desktop file; %% → %. *used tells whether file was used. */
#define MAX_ARGS 64
static void file_uri(const char *path, char *out, size_t n) {
    size_t o = (size_t)snprintf(out, n, "file://");
    for (const unsigned char *p = (const unsigned char *)path; *p && o + 4 < n; p++) {
        if (isalnum(*p) || strchr("/-_.~", *p)) out[o++] = (char)*p;
        else o += (size_t)snprintf(out + o, n - o, "%%%02X", *p);
    }
    out[o] = 0;
}

static int split_exec(const char *exec, const Entry *e, char **argv, char *buf, size_t cap, const char *file, int *used) {
    int argc = 0;
    size_t o = 0;
    const char *p = exec;
    while (*p && argc < MAX_ARGS - 3) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        size_t start = o;
        int quoted = 0, field_only = 0, empty_ok = 0;
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
            if (o + 4 >= cap) return -1;
            if (*p == '"') { quoted = !quoted; empty_ok = 1; p++; continue; }
            if (quoted && *p == '\\' && p[1] && strchr("\"`$\\", p[1])) { buf[o++] = p[1]; p += 2; continue; }
            if (*p == '%' && p[1]) {
                char c = p[1];
                p += 2;
                if (c == '%') buf[o++] = '%';
                else if (c == 'c') { size_t l = strlen(e->a.name); if (o + l >= cap) return -1; memcpy(buf + o, e->a.name, l); o += l; }
                else if (c == 'k') { size_t l = strlen(e->a.file); if (o + l >= cap) return -1; memcpy(buf + o, e->a.file, l); o += l; }
                else if (c == 'i') field_only = e->icon_name[0] ? 2 : 1;
                else if (file && !*used && strchr("fFuU", c)) {
                    char uri[PATH_MAX * 3];
                    const char *v = file;
                    if (c == 'u' || c == 'U') { file_uri(file, uri, sizeof uri); v = uri; }
                    size_t l = strlen(v);
                    if (o + l + 1 >= cap) return -1;
                    memcpy(buf + o, v, l);
                    o += l;
                    *used = 1;
                    empty_ok = 1;
                }
                else field_only = 1;            /* the other file codes and the deprecated ones */
                continue;
            }
            buf[o++] = *p++;
        }
        if (quoted) return -1;                  /* unclosed quotes */
        if (field_only == 2 && o == start) {
            argv[argc++] = "--icon";
            size_t l = strlen(e->icon_name);
            if (o + l + 1 >= cap) return -1;
            memcpy(buf + o, e->icon_name, l + 1);
            argv[argc++] = buf + start;
            o += l + 1;
            continue;
        }
        if (o == start && !empty_ok) continue;  /* it was only a field code */
        buf[o++] = 0;
        argv[argc++] = buf + start;
    }
    argv[argc] = NULL;
    return argc;
}

static void open_log(void) {
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    if (!home) return;
    snprintf(path, sizeof path, "%s/.cache", home);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.cache/lordos", home);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.cache/lordos/apps.log", home);
    struct stat st;
    int flags = O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW;
    if (!lstat(path, &st) && st.st_size > 1024 * 1024) flags |= O_TRUNC;
    int fd = open(path, flags, 0600);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
}

/* ---- "no access to your files": the Landlock plan (built before fork) ---- */

typedef struct {
    SandboxFilesCfg cfg;
    char home[PATH_MAX], runtime[PATH_MAX], wayland[64], opened[PATH_MAX], cwd[PATH_MAX];
    char buf[2 * SANDBOX_FILES_MAX][PATH_MAX];
    int nbuf, nrw, nro;
} FilesPlan;

static const char *plan_str(FilesPlan *p, const char *fmt, const char *a, const char *b) {
    if (p->nbuf >= 2 * SANDBOX_FILES_MAX) return NULL;
    int n = snprintf(p->buf[p->nbuf], PATH_MAX, fmt, a, b);
    if (n < 0 || n >= PATH_MAX) return NULL;
    return p->buf[p->nbuf++];
}

static void plan_add(FilesPlan *p, int rw, const char *fmt, const char *a, const char *b) {
    int *n = rw ? &p->nrw : &p->nro;
    const char **list = rw ? p->cfg.rw : p->cfg.ro;
    if (*n >= SANDBOX_FILES_MAX) return;
    const char *s = plan_str(p, fmt, a, b);
    if (!s) return;
    for (int i = 0; i < *n; i++) if (!strcmp(list[i], s)) { p->nbuf--; return; }
    list[(*n)++] = s;
    list[*n] = NULL;
}

/* The canonical desktop id as a folder name under ~/.config, ~/.cache and ~/.local/share: only
 * if it can't name a folder that belongs to someone else's startup, keys or settings. The id is
 * the only name used: Exec, StartupWMClass and Name are free text of the .desktop file. */
static int own_dir_ok(const char *id) {
    /* folders another program reads to run something, or that hold someone else's keys or data */
    static const char *const deny[] = { "lordos", "ssh", "gnupg", "autostart", "applications", "systemd", "environment.d",
                                        "keyrings", "dconf", "git", "trash", "mime", "fontconfig", "pulse", "pipewire",
                                        "wireplumber", "user-dirs.dirs", "mimeapps.list", "gtk-3.0", "gtk-4.0", "fish",
                                        "bash", "zsh", "nvim", "vim", "emacs", "nano", "mc", "flatpak", "containers",
                                        "thumbnails", "recently-used.xbel", "icons", "themes", "fonts", "dbus-1",
                                        "user-tmpfiles.d", "xdg-desktop-portal", "menus", "kio", "plasma-workspace",
                                        "tmux", "kitty", "alacritty", "foot", "wezterm", "mozilla", "chromium",
                                        "google-chrome", "BraveSoftware", "Code", "keepassxc", "evolution", "Electron",
                                        "gstreamer-1.0", "mesa_shader_cache", "mesa_shader_cache_db", "radv_builtin_shaders",
                                        "bin", "applications-merged", "desktop-directories", "kservices5", "kservices6",
                                        "speech-dispatcher", "environment", "profile.d", NULL };
    size_t n = strlen(id);
    if (!n || n > 100 || id[0] == '.' || id[0] == '-') return 0;
    for (const char *q = id; *q; q++) if (!isalnum((unsigned char)*q) && !strchr("._-+", *q)) return 0;
    if (!strncasecmp(id, "lordos", 6)) return 0;
    for (int i = 0; deny[i]; i++) if (!strcasecmp(id, deny[i])) return 0;
    return 1;
}

/* A writable grant must never land on a folder the system reads to start or install something,
 * or that holds a security decision (fsops_protected), nor on the shell's startup files. */
/* Folders of the home that make the session run something or hold keys: no writable grant inside
 * them, and none on a folder that contains one (opening ~/.config with an app must not hand it
 * ~/.config/autostart). fsops_protected's list counts the same way. */
static const char *const sensitive[] = { "/.ssh", "/.gnupg", "/.config/systemd", "/.local/share/systemd",
                                         "/.local/share/dbus-1", "/.config/environment.d", "/.config/user-tmpfiles.d",
                                         "/.local/bin", "/bin", "/.config/autostart", "/.local/share/applications",
                                         "/.config/lordos", "/.local/share/lordos", NULL };

static int path_under(const char *path, const char *dir) {
    size_t n = strlen(dir);
    return n && !strncmp(path, dir, n) && (path[n] == '/' || !path[n]);
}

static int grant_ok(const char *path) {
    if (fsops_protected(path)) return 0;
    const char *h = getenv("HOME");
    char home[PATH_MAX], full[PATH_MAX + 64];
    if (!h || !realpath(h, home)) return 0;
    if (!strcmp(path, home)) return 0;                  /* the whole home: never writable */
    for (int i = 0; sensitive[i]; i++) {
        snprintf(full, sizeof full, "%s%s", home, sensitive[i]);
        if (path_under(path, full) || path_under(full, path)) return 0;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    static const char *const startup[] = { ".bashrc", ".bash_profile", ".bash_login", ".bash_logout", ".bash_aliases",
                                           ".profile", ".zshrc", ".zprofile", ".zshenv", ".zlogin", ".zlogout",
                                           ".xprofile", ".xsession", ".xsessionrc", ".xinitrc", ".Xresources",
                                           ".pam_environment", ".gitconfig", ".ssh", ".gnupg", NULL };
    for (int i = 0; startup[i]; i++) if (!strcmp(base, startup[i])) return 0;
    struct stat st;
    if (!lstat(path, &st) && S_ISREG(st.st_mode) && st.st_nlink > 1) return 0;   /* maybe another name of one of those */
    return 1;
}

static int is_dir(const char *p) { struct stat st; return !lstat(p, &st) && S_ISDIR(st.st_mode); }

/* What a restricted app may touch (see sandbox.h). file: the realpath it was opened with, or NULL. */
static FilesPlan *files_plan(const DesktopApp *a, const char *file, const char *cwd) {
    const char *h = getenv("HOME");
    FilesPlan *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    if (!h || !realpath(h, p->home) || p->home[0] != '/' || !strcmp(p->home, "/")) { free(p); return NULL; }
    const char *home = p->home;
    p->cfg.home = home;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char rtdef[64];
    snprintf(rtdef, sizeof rtdef, "/run/user/%u", (unsigned)getuid());
    if (!(rt && rt[0] == '/' && realpath(rt, p->runtime)) && !realpath(rtdef, p->runtime)) {
        free(p);                        /* no runtime dir to hide: its sockets (the bus) would stay reachable */
        return NULL;
    }
    p->cfg.runtime_dir = p->runtime;
    /* the only sockets of the session it keeps: the display and the sound server's normal ones
     * (never pipewire-0-manager, the bus, systemd's, agents' or LordOS's own) */
    const char *wd = getenv("WAYLAND_DISPLAY");
    int ns = 0;
    if (wd && wd[0] && !strchr(wd, '/') && strlen(wd) < sizeof p->wayland) { snprintf(p->wayland, sizeof p->wayland, "%s", wd); p->cfg.sockets[ns++] = p->wayland; }
    else p->cfg.sockets[ns++] = "wayland-0";
    p->cfg.sockets[ns++] = "pipewire-0";
    p->cfg.sockets[ns++] = "pulse/native";
    p->cfg.sockets[ns] = NULL;
    char path[PATH_MAX];
    /* its own folders, named exactly after its canonical id, created if missing */
    if (own_dir_ok(a->id)) {
        static const char *const own[] = { "%s/.config/%s", "%s/.cache/%s", "%s/.local/share/%s", "%s/.local/state/%s", NULL };
        static const char *const parents[] = { "%s/.config", "%s/.cache", "%s/.local", "%s/.local/share", "%s/.local/state", NULL };
        for (int k = 0; parents[k]; k++) { snprintf(path, sizeof path, parents[k], home); mkdir(path, 0700); }
        for (int k = 0; own[k]; k++) {
            snprintf(path, sizeof path, own[k], home, a->id);
            mkdir(path, 0700);
            if (is_dir(path) && grant_ok(path)) plan_add(p, 1, "%s%s", path, "");
        }
    }
    /* apps whose data folder isn't named after their id: a fixed table (canonical id → folder in
     * the home and in ~/.cache), never names taken from the .desktop file */
    static const struct { const char *id, *dir; } alias[] = { { "firefox-esr", "mozilla" }, { "firefox", "mozilla" }, { NULL, NULL } };
    for (int k = 0; alias[k].id; k++) {
        if (strcmp(a->id, alias[k].id)) continue;
        static const char *const where[] = { "%s/.%s", "%s/.cache/%s", NULL };
        for (int w = 0; where[w]; w++) {
            snprintf(path, sizeof path, where[w], home, alias[k].dir);
            mkdir(path, 0700);
            if (is_dir(path) && grant_ok(path)) plan_add(p, 1, "%s%s", path, "");
        }
    }
    /* caches shared by every app that hold nothing of the user's (not all of ~/.cache: its
     * thumbnails are pictures of the user's documents) */
    static const char *const caches[] = { "fontconfig", "mesa_shader_cache", "mesa_shader_cache_db", "radv_builtin_shaders",
                                          "gstreamer-1.0", NULL };
    /* read only: they are shared with unrestricted apps, which load them as trusted (a crafted
     * font or shader cache would run in them) */
    for (int k = 0; caches[k]; k++) plan_add(p, 0, "%s/.cache/%s", home, caches[k]);
    /* the desktop's look and settings, read only */
    static const char *const look[] = { ".config/gtk-3.0", ".config/gtk-4.0", ".config/fontconfig", ".config/dconf",
                                        ".config/mimeapps.list", ".config/user-dirs.dirs", ".config/user-dirs.locale",
                                        ".config/kdeglobals", ".config/Trolltech.conf", ".config/qt5ct", ".config/qt6ct",
                                        ".config/pulse", ".local/share/fonts", ".local/share/icons", ".local/share/themes",
                                        ".local/share/mime", ".local/share/applications", ".fonts", ".icons", ".themes", NULL };
    for (int k = 0; look[k]; k++) plan_add(p, 0, "%s/%s", home, look[k]);
    /* "Open with": that file or folder only (read only if it is one of the protected ones) */
    if (file) {
        struct stat st;
        if (stat(file, &st)) { free(p); return NULL; }
        plan_add(p, grant_ok(file), "%s%s", file, "");
        snprintf(p->opened, sizeof p->opened, "%s", file);
        p->cfg.opened = p->opened;
        p->cfg.opened_dev = (unsigned long long)st.st_dev;
        p->cfg.opened_ino = (unsigned long long)st.st_ino;
    }
    if (cwd && cwd[0] == '/') { snprintf(p->cwd, sizeof p->cwd, "%s", cwd); p->cfg.cwd = p->cwd; }
    return p;
}

/* No microphone, camera, USB or Bluetooth (without no-files): the same view minus the home
 * restriction. No session bus, no systemd user manager, no agents: such an app can't create or
 * change a unit, or start anything outside its scope (systemd-run --user, D-Bus activation), so the
 * device program and the PipeWire guard, which know it by its scope, always see it. */
static FilesPlan *view_plan(const char *cwd) {
    FilesPlan *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    const char *h = getenv("HOME");
    if (!h || !realpath(h, p->home) || p->home[0] != '/' || !strcmp(p->home, "/")) { free(p); return NULL; }
    p->cfg.home = p->home;
    p->cfg.keep_home = 1;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char rtdef[64];
    snprintf(rtdef, sizeof rtdef, "/run/user/%u", (unsigned)getuid());
    if (!(rt && rt[0] == '/' && realpath(rt, p->runtime)) && !realpath(rtdef, p->runtime)) { free(p); return NULL; }
    p->cfg.runtime_dir = p->runtime;
    const char *wd = getenv("WAYLAND_DISPLAY");
    int ns = 0;
    if (wd && wd[0] && !strchr(wd, '/') && strlen(wd) < sizeof p->wayland) { snprintf(p->wayland, sizeof p->wayland, "%s", wd); p->cfg.sockets[ns++] = p->wayland; }
    else p->cfg.sockets[ns++] = "wayland-0";
    p->cfg.sockets[ns++] = "pipewire-0";            /* playback; capture goes through the PipeWire guard */
    p->cfg.sockets[ns++] = "pulse/native";
    p->cfg.sockets[ns] = NULL;
    if (cwd && cwd[0] == '/') { snprintf(p->cwd, sizeof p->cwd, "%s", cwd); p->cfg.cwd = p->cwd; }
    return p;
}

static char last_error[64];
const char *apps_last_error(void) { return last_error; }

static int launch(const char *id, const char *file, const char *action);
int apps_launch(const char *id) { return launch(id, NULL, NULL); }

/* A desktop action of the entry that may run: listed in its Actions= key, with a valid name, a
 * label and an Exec line. NULL otherwise. */
static const Action *entry_action(const Entry *e, const char *name) {
    if (!name || !valid_action(name) || !list_has(e->actions, name)) return NULL;
    for (int i = 0; i < e->nact; i++)
        if (!strcmp(e->act[i].name, name)) return e->act[i].has_label && e->act[i].exec[0] ? &e->act[i] : NULL;
    return NULL;
}

/* The listed app with that id, or NULL (the same lookup as the launcher). */
static const DesktopApp *find_app(const char *id) {
    if (!valid_id(id)) return NULL;
    const DesktopApp *list;
    int n = apps_list(cached_lang[0] ? cached_lang : "en", &list);
    for (int i = 0; i < n; i++) if (!strcmp(list[i].id, id)) return &list[i];
    return NULL;
}

int apps_actions(const char *id, const char *lang, AppAction *out, int max) {
    const DesktopApp *a = find_app(id);
    if (!a || max <= 0) return 0;
    Entry *e = malloc(sizeof *e);
    if (!e) return 0;
    int n = 0;
    if (!parse_desktop(a->file, lang && lang[0] ? lang : "en", e)) {
        /* in the order of the Actions= key */
        for (const char *p = e->actions; *p && n < max;) {
            const char *q = strchr(p, ';');
            size_t len = q ? (size_t)(q - p) : strlen(p);
            char name[APPS_ACTION_NAME_MAX];
            if (len && len < sizeof name) {
                memcpy(name, p, len);
                name[len] = 0;
                const Action *act = entry_action(e, name);
                int dup = 0;
                for (int i = 0; i < n; i++) if (!strcmp(out[i].name, name)) dup = 1;
                if (act && !dup) {
                    snprintf(out[n].name, sizeof out[n].name, "%s", act->name);
                    snprintf(out[n].label, sizeof out[n].label, "%s", act->label);
                    n++;
                }
            }
            if (!q) break;
            p = q + 1;
        }
    }
    free(e);
    return n;
}

int apps_launch_action(const char *id, const char *action) {
    last_error[0] = 0;
    if (!action || !valid_action(action)) return -1;
    return launch(id, NULL, action);
}

/* Opening a file with an app: only an app that declares MimeType (accepts files), a real
 * path inside the home folder or /media/$USER (fsops_allowed), a regular file or folder,
 * and only if its Exec has a file code: the path goes as ONE argument. */
int apps_launch_with(const char *id, const char *path) {
    char real[PATH_MAX];
    struct stat st;
    if (!path || !realpath(path, real) || !fsops_allowed(real) || stat(real, &st) || !(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))) return -1;
    /* id "" = the default app for the file's type (apps_default_for: the user's choice in Settings →
     * Default Apps, else the system's default, else the first installed app that declares the type);
     * -1 if that is one of LordOS's own apps (the theme opens those) or there is none */
    char mime[MIME_MAX], def[DESKTOP_ID_MAX];
    if (id && !id[0]) {
        if (defaults_guess(real, mime, sizeof mime) || apps_default_for(mime, def, sizeof def) <= 0) return -1;
        if (!strncmp(def, "lordos-", 7)) return -1;     /* one of ours: the theme opens it */
        def[strlen(def) - 8] = 0;                       /* without ".desktop" */
        id = def;
    }
    return launch(id, real, NULL);
}

/* ---- the default app for a MIME type ---- */

/* Can the listed app with that desktop id ("x.desktop") open a file? The launcher's own rules: it
 * is listed (Type=Application, not Hidden/NoDisplay/terminal, TryExec found, OnlyShowIn/NotShowIn),
 * it declares MimeType (and this type when `declared`), and its Exec line takes a file (%f %F %u
 * %U) and names a program that exists. */
static int opens_files(const char *desktop_id, const char *mime, int declared) {
    size_t l = strlen(desktop_id);
    char id[160];
    if (!desktop_id_valid(desktop_id) || l - 8 >= sizeof id) return 0;
    memcpy(id, desktop_id, l - 8);
    id[l - 8] = 0;
    const DesktopApp *a = find_app(id);
    if (!a || !a->mime[0] || (declared && !list_has(a->mime, mime))) return 0;
    Entry *e = malloc(sizeof *e);
    char *buf = malloc(4096);
    char *argv[MAX_ARGS], prog[PATH_MAX];
    int used = 0;
    int ok = e && buf && !parse_desktop(a->file, cached_lang[0] ? cached_lang : "en", e) && e->is_app && !e->terminal &&
             !e->hidden && split_exec(e->exec, e, argv, buf, 4096, "/", &used) > 0 && used &&
             !find_program(argv[0], a->user, prog, sizeof prog);
    free(e);
    free(buf);
    return ok;
}

static int by_id(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int apps_default_for(const char *mime, char *out, size_t n) {
    if (!mime_valid(mime) || n < DESKTOP_ID_MAX) return -1;
    DefaultsFiles f;
    defaults_load(&f);
    char id[DESKTOP_ID_MAX];
    int r = 0;
    /* 1. [Default Applications], file by file: the first entry that is installed (the user's own
     *    files first; LordOS's own "lordos-<app>.desktop" counts only there, the theme opens it) */
    for (int i = 0; i < f.n && !r; i++)
        for (int k = 0; !r && mimeapps_entry(f.text[i], "Default Applications", mime, k, id, sizeof id) == 0; k++) {
            if (defaults_removed(&f, i, mime, id)) continue;
            if (f.user[i] && !strncmp(id, "lordos-", 7)) r = APPS_DEFAULT_USER;
            else if (opens_files(id, mime, 0)) r = f.user[i] ? APPS_DEFAULT_USER : APPS_DEFAULT_LISTED;
        }
    /* 2. [Added Associations], in the same file order */
    for (int i = 0; i < f.n && !r; i++)
        for (int k = 0; !r && mimeapps_entry(f.text[i], "Added Associations", mime, k, id, sizeof id) == 0; k++)
            if (!defaults_removed(&f, i, mime, id) && opens_files(id, mime, 0)) r = APPS_DEFAULT_LISTED;
    /* 3. the installed apps that declare the type: mimeinfo.cache order (the user's folder, then the
     *    system's), then the rest sorted by desktop id */
    const char *dirs[2] = { NULL, NULL };
    char cache[2][PATH_MAX];
    const char *h = getenv("HOME");
    if (h && h[0] == '/') { snprintf(cache[0], PATH_MAX, "%s/.local/share/applications/mimeinfo.cache", h); dirs[0] = cache[0]; }
    snprintf(cache[1], PATH_MAX, "%s/usr/share/applications/mimeinfo.cache", defaults_root());
    dirs[1] = cache[1];
    for (int d = 0; d < 2 && !r; d++) {
        char *t = dirs[d] ? defaults_read(dirs[d]) : NULL;
        for (int k = 0; t && !r && mimeapps_entry(t, "MIME Cache", mime, k, id, sizeof id) == 0; k++)
            if (!defaults_removed(&f, f.n - 1, mime, id) && opens_files(id, mime, 1)) r = APPS_DEFAULT_DECLARED;
        free(t);
    }
    if (!r) {
        const DesktopApp *list;
        int na = apps_list(cached_lang[0] ? cached_lang : "en", &list);
        /* copies: opens_files() may reread the list and free this one */
        char (*ids)[sizeof list[0].id] = na > 0 ? malloc((size_t)na * sizeof *ids) : NULL;
        int m = 0;
        for (int i = 0; ids && i < na; i++) if (list_has(list[i].mime, mime)) memcpy(ids[m++], list[i].id, sizeof ids[0]);
        if (m) qsort(ids, (size_t)m, sizeof *ids, by_id);
        for (int i = 0; i < m && !r; i++) {
            snprintf(id, sizeof id, "%s.desktop", ids[i]);
            if (!defaults_removed(&f, f.n - 1, mime, id) && opens_files(id, mime, 1)) r = APPS_DEFAULT_DECLARED;
        }
        free(ids);
    }
    defaults_free(&f);
    if (!r) return 0;
    snprintf(out, n, "%s", id);
    return r;
}

/* action: the name of one of the entry's desktop actions (apps_launch_action), whose Exec line
 * replaces the main one; never together with a file. Everything else (the id, the listing filters,
 * the scope, the restrictions) is the same as a plain launch. */
static int launch(const char *id, const char *file, const char *action) {
    last_error[0] = 0;
    if (file && action) return -1;
    const DesktopApp *a = find_app(id);
    if (!a) return -1;
    /* the file is read again: what counts is what it says now */
    Entry *e = malloc(sizeof *e);
    char *buf = malloc(4096);
    char *argv[MAX_ARGS];
    char prog[PATH_MAX];
    int used = 0;
    int ok = e && buf && !parse_desktop(a->file, cached_lang, e) && e->is_app && !e->terminal && !e->hidden &&
             (!file || e->a.mime[0]);
    const char *exec = e ? e->exec : NULL;
    if (ok && action) {
        const Action *act = entry_action(e, action);
        if (act) exec = act->exec; else ok = 0;
    }
    ok = ok && split_exec(exec, e, argv, buf, 4096, file, &used) > 0 && (!file || used) && !find_program(argv[0], a->user, prog, sizeof prog);
    char cwd[PATH_MAX] = "";
    if (ok && e->path[0] == '/') snprintf(cwd, sizeof cwd, "%s", e->path);
    free(e);
    if (!ok) { free(buf); return -1; }
    /* "no access to your files": Landlock before execve. Without Landlock in the kernel the app
     * doesn't start (the owner asked for it to be kept away from their files). */
    FilesPlan *plan = NULL;
    char fwid[FW_APP_MAX];
    snprintf(fwid, sizeof fwid, "desktop:%s", id);
    uint32_t viewed = FW_R_NO_FILES | FW_R_NO_MIC | FW_R_NO_CAMERA | FW_R_NO_USB | FW_R_NO_BLUETOOTH;
    if (restrict_blocks(fwid, viewed) && sandbox_landlock_abi() < 6) {
        /* fail closed: without Landlock ABI 6 the session's abstract sockets can't be kept away */
        fprintf(stderr, "apps: %s is restricted, but this kernel's Landlock is too old (ABI < 6): not launched\n", id);
        snprintf(last_error, sizeof last_error, "apps.restrictKernel");
        free(buf);
        return -1;
    }
    if (restrict_blocks(fwid, FW_R_NO_FILES)) {
        if (!sandbox_landlock_abi() || !(plan = files_plan(a, file, cwd))) {
            fprintf(stderr, "apps: %s is restricted to no files, but Landlock is not available: not launched\n", id);
            free(buf);
            return -1;
        }
    } else if (restrict_blocks(fwid, FW_R_NO_MIC | FW_R_NO_CAMERA | FW_R_NO_USB | FW_R_NO_BLUETOOTH)) {
        if (!(plan = view_plan(cwd))) {
            fprintf(stderr, "apps: %s is restricted, but its own view can't be built: not launched\n", id);
            free(buf);
            return -1;
        }
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* orphaned grandchild: it does not remain a child of lord-server */
        setsid();
        if (fork() != 0) _exit(0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        for (int s = 1; s < 32; s++) signal(s, SIG_DFL);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
        open_log();
        syscall(SYS_close_range, 3, ~0u, 0);
        const char *home = getenv("HOME");
        if (!cwd[0] || chdir(cwd)) { if (home && chdir(home)) {} }
        /* its own app-lordos-<id>.scope cgroup: the firewall, the Monitor, the PipeWire guard and
         * the notification server know the app only by it. In a LordOS session a launch that can't
         * be placed there never runs (it would run as lord-server itself, with none of its rules). */
        if (app_scope_enter(id) && getenv("LORDOS_SESSIOND")) {
            static const char msg[] = "apps: could not place the app in its own scope: not launched\n";
            if (write(2, msg, sizeof msg - 1) < 0) {}
            _exit(126);
        }
        /* its own view (namespaces: no session sockets, only its folders), then Landlock */
        /* a restricted app gets no X display: X11 apps share one X server and can watch each
         * other's windows, input and selections (docs/PRIVACY.md, "X11 apps"); its private /tmp has no
         * X socket either */
        if (plan) unsetenv("DISPLAY");
        if (plan && (sandbox_files_isolate(&plan->cfg) ||
                     (plan->cfg.keep_home ? sandbox_apply_scope() : sandbox_apply_files(&plan->cfg)))) {
            static const char msg[] = "apps: could not apply the no-files restriction: not launched\n";
            if (write(2, msg, sizeof msg - 1) < 0) {}
            _exit(126);
        }
        execve(prog, argv, environ);
        _exit(127);
    }
    free(plan);
    free(buf);
    if (pid < 0) return -1;
    waitpid(pid, NULL, 0);
    return 0;
}
