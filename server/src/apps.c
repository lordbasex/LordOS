/* apps.c — installed Linux apps: reads the .desktop files from /usr/share/applications and
 * ~/.local/share/applications (Desktop Entry Specification 1.5), finds their icon (iconfind.c) and launches them.
 *
 * It is the second exception to "the server does not run commands" (approved for M7; the first
 * is the Terminal's shell). Limits:
 *   - an app is only launched by its id, and the id must belong to a .desktop file in those two
 *     folders that passes the apps_list() filters;
 *   - the command is that file's Exec line; the caller adds no arguments or
 *     files (the %f %u… codes are removed) and it does not go through a shell: argv is built per the
 *     specification and run with execv;
 *   - terminal apps (Terminal=true, approved 2026-11-10) are listed with terminal = 1 when their
 *     program is found in the fixed PATH, but never run here: apps_launch_terminal parses their Exec
 *     line the same way (no caller arguments, no file) and keeps that argv in a one-shot slot
 *     (term/termapp.c: random 128-bit token, 10 s, single use, at most 4 pending). The Terminal passes
 *     only that token to lord.term.create({ app }), whose tab starts the user's login shell as always
 *     and has lord-server type the line, each argument single-quoted in C, into it once. An app with
 *     restrictions (Settings → Privacy) is refused: in a Terminal tab it would run without them;
 *   - JS cannot create .desktop files: lord.fs is read-only and LordOS app packages are installed
 *     in another folder (~/.local/share/lordos/apps), so a command cannot be
 *     invented from the UI.
 * App output goes to apps/<id>.log in the user's log folder (lordlog.h: /var/log/lordos/users/<user>,
 * or ~/.cache/lordos), after a stamped "started" line; capped there and rotated by logrotate.
 *
 * An app the owner restricted with "no access to your files" (Settings → Privacy, firewall id
 * desktop:<id>, sec/restrict.h) starts under Landlock (sandbox_apply_files): only its own config,
 * cache and data folders, the system read-only, /tmp and the runtime dir, plus the one file or
 * folder it was opened with. A restriction set while the app runs applies at its next launch.
 *
 * Every launch carries an xdg-activation token (XDG_ACTIVATION_TOKEN, wl/xdgact.c): the new window, or
 * the window of the copy already running that the new process hands the link or file to (a browser),
 * comes to the front, because the person asked for it. */
#include "apps.h"
#include "lordlog.h"
#include "iconfind.h"
#include "hw/defaults.h"
#include "hw/mimeapps.h"
#include "fsops.h"
#include "sec/cgroup.h"
#include "sec/restrict.h"
#include "sec/sandbox.h"
#include "fw/rules.h"
#include "wl/xauth.h"
#include "wl/xdgact.h"
#include "term/termapp.h"
#include "fpak.h"
#include "fpspawner.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
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
        char *dot = strchr(*loc, '.'); if (dot) *dot = 0;
        /* "en@shaw", "sr@latin": a modifier matches only a language that has it, and LordOS's never do (GNOME's
         * .desktop files carry Name[en@shaw], in the Shavian alphabet, which used to win over the plain Name) */
        static char never[] = "@";
        if (strchr(*loc, '@')) *loc = never;
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

/* MimeType into its field; a list longer than the field keeps only its whole types, so a cut type
 * ("video/mp" of "video/mp4") never stands for another. */
static void set_mime(char *dst, size_t n, const char *v) {
    size_t len = strlen(v);
    if (len < n) { memcpy(dst, v, len + 1); return; }
    len = n - 1;
    while (len && v[len] != ';') len--;
    memcpy(dst, v, len);
    dst[len] = 0;
}

static int parse_desktop(const char *file, const char *lang, Entry *e) {
    FILE *f = fopen(file, "r");
    if (!f) return -1;
    memset(e, 0, sizeof *e);
    /* a whole line however long: a fixed buffer would cut VLC's MimeType (2.4 KB) and read the rest
     * as a line of its own (GitHub #123) */
    char *line = NULL;
    size_t cap = 0;
    int in_main = 0;
    Action *act = NULL;                 /* inside a [Desktop Action …] group that is kept */
    while (getline(&line, &cap, f) >= 0) {
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
        else if (!strcmp(k, "MimeType")) set_mime(e->a.mime, sizeof e->a.mime, v);
        else if (!strcmp(k, "StartupWMClass")) snprintf(e->a.wmclass, sizeof e->a.wmclass, "%s", v);
        else if (!strcmp(k, "OnlyShowIn")) snprintf(e->only, sizeof e->only, "%s", v);
        else if (!strcmp(k, "NotShowIn")) snprintf(e->not_, sizeof e->not_, "%s", v);
        else if (!strcmp(k, "Hidden")) e->hidden = !strcmp(v, "true");
        else if (!strcmp(k, "NoDisplay")) e->nodisplay = !strcmp(v, "true");
        else if (!strcmp(k, "Terminal")) e->terminal = !strcmp(v, "true");
        else if (!strcmp(k, "Actions")) snprintf(e->actions, sizeof e->actions, "%s", v);
    }
    free(line);
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

/* ---- icons: the installed themes' PNGs, then SVG (iconfind.c) ---- */

static void find_icon(const char *name, char *out, size_t n) {
    IconRoots r;
    iconfind_roots(&r, getenv("HOME"), "");
    iconfind_app(&r, name, out, n);
}

/* A tray icon's IconName (ui/sni.c): a plain name, never a path, looked up in the same places at the bar's sizes,
 * in the status folders first. */
void apps_status_icon(const char *name, char *out, size_t n) {
    IconRoots r;
    iconfind_roots(&r, getenv("HOME"), "");
    iconfind_status(&r, name ? name : "", out, n);
}

/* ---- list ---- */

/* A desktop id the launcher can run: it must also make a valid app scope name
 * ("app-lordos-<id>.scope", cgroup_scope_name_ok: no '+', at most 62 characters, no "..") and
 * never one that reads as a .lpk package's scope ("lpk-...": that would be lpk:<rest>'s identity,
 * rules and restrictions) or a Flatpak app's ("fp-...": flatpak:<rest>, docs/FLATPAK.md E2). */
#define APP_ID_MAX 62
static int valid_id(const char *id) {
    size_t n = strlen(id);
    if (!n || n >= sizeof apps[0].id || n > APP_ID_MAX) return 0;
    for (const char *p = id; *p; p++) if (!isalnum((unsigned char)*p) && !strchr("._-", *p)) return 0;
    return id[0] != '.' && id[0] != '-' && !strstr(id, "..") && strncmp(id, "lpk-", 4) && strncmp(id, "fp-", 3);
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

static int terminal_program_ok(const Entry *e, int user);

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
            if (!e->is_app || e->hidden || e->nodisplay || !e->has_name || !e->exec[0]) continue;
            if (e->only[0] && !list_has(e->only, "LordOS")) continue;
            if (e->not_[0] && list_has(e->not_, "LordOS")) continue;
            char prog[PATH_MAX];
            if (e->tryexec[0] && find_program(e->tryexec, d == 0, prog, sizeof prog)) continue;
            /* a terminal app is typed into a shell: its program must be one the launcher would find */
            if (e->terminal && !terminal_program_ok(e, d == 0)) continue;
            DesktopApp *a = &list[n++];
            *a = e->a;
            snprintf(a->id, sizeof a->id, "%s", id);
            a->user = d == 0;
            a->terminal = e->terminal;
            find_icon(e->icon_name, a->icon, sizeof a->icon);
            exec_bin(e->exec, a->bin, sizeof a->bin);
        }
        closedir(dir);
    }
    free(ids);
    free(e);
    if (!list) return 0;
    qsort(list, (size_t)n, sizeof *list, cmp_name);
    /* the table was sized for MAX_APPS (18.6 MB); kept at that size, the pages the scan touched stayed
     * resident as 2 MB huge pages (0.8.9.1: ~10 MB for about a hundred apps) */
    DesktopApp *fit = realloc(list, (size_t)(n ? n : 1) * sizeof *list);
    if (fit) list = fit;
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
                    if ((c == 'u' || c == 'U') && file[0] == '/') { file_uri(file, uri, sizeof uri); v = uri; }
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

/* A terminal app's Exec line as argv (no file, field codes removed) whose program is found in the
 * fixed PATH (plus ~/.local/bin for the user's own .desktop files). */
static int terminal_program_ok(const Entry *e, int user) {
    char *buf = malloc(TERMAPP_ARGV_MAX);
    char *argv[MAX_ARGS], prog[PATH_MAX];
    int used = 0;
    int ok = buf && split_exec(e->exec, e, argv, buf, TERMAPP_ARGV_MAX, NULL, &used) > 0 && !find_program(argv[0], user, prog, sizeof prog);
    free(buf);
    return ok;
}

/* The app's own log, apps/<id>.log, opened (and its "started" line written) here, before fork: the
 * child only dup2s it. -1 if there is none. */
static int open_log(const char *id, const char *action, const char *file) {
    char path[PATH_MAX];
    if (lordlog_path("apps", id, path, sizeof path)) return -1;
    int fd = lordlog_open(path);
    lordlog_write(fd, "INFO", "apps", "starting %s%s%s%s", id, action ? " (action " : "", action ? action : "", action ? ")" : "");
    (void)file;                                  /* never the file's name: it may say what the person opened */
    return fd;
}

/* ---- "no access to your files": the Landlock plan (built before fork) ---- */

typedef struct {
    SandboxFilesCfg cfg;
    char home[PATH_MAX], runtime[PATH_MAX], wayland[64], opened[PATH_MAX], cwd[PATH_MAX], x11[32], xauth[64];
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
                                         "/.config/lordos", "/.local/share/lordos", "/.config/fish", "/.bin",
                                         "/.local/share/flatpak", "/.config/dbus-1", NULL };

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

int apps_system_file(const char *id, char *out, size_t n) {
    if (!id || !out || !n) return -1;
    const DesktopApp *a = find_app(id);
    if (!a || a->user) return -1;
    const char *root = defaults_root();
    size_t rl = strlen(root);
    if (rl && strncmp(a->file, root, rl)) return -1;
    const char *p = a->file + rl;
    if (strncmp(p, "/usr/share/applications/", 24) || strchr(p + 24, '/') || strlen(p) >= n) return -1;
    snprintf(out, n, "%s", p);
    return 0;
}

int apps_actions(const char *id, const char *lang, AppAction *out, int max) {
    const DesktopApp *a = find_app(id);
    if (!a || a->terminal || max <= 0) return 0;        /* a terminal app's actions would need a shell too */
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

/* A terminal app: its Exec line, parsed as launch() parses it (the file is read again: what counts is
 * what it says now), goes to a one-shot slot; nothing runs here. */
int apps_is_terminal(const char *id) {
    const DesktopApp *a = find_app(id);
    return a && a->terminal;
}

int apps_launch_terminal(const char *id, char *token, size_t n) {
    last_error[0] = 0;
    if (!token || n < TERMAPP_TOKEN_HEX + 1) return -1;
    token[0] = 0;
    const DesktopApp *a = find_app(id);
    if (!a || !a->terminal) return -1;
    char fwid[FW_APP_MAX];
    snprintf(fwid, sizeof fwid, "desktop:%s", id);
    if (restrict_blocks(fwid, FW_R_ALL)) {
        /* fail closed: in the Terminal's shell it would have the Terminal's scope, firewall rules and access */
        snprintf(last_error, sizeof last_error, "apps.restrictTerminal");
        return -1;
    }
    Entry *e = malloc(sizeof *e);
    char *buf = malloc(TERMAPP_ARGV_MAX);
    char *argv[MAX_ARGS], prog[PATH_MAX];
    int used = 0, argc = -1;
    int ok = e && buf && !parse_desktop(a->file, cached_lang, e) && e->is_app && e->terminal && !e->hidden &&
             (argc = split_exec(e->exec, e, argv, buf, TERMAPP_ARGV_MAX, NULL, &used)) > 0 &&
             !find_program(argv[0], a->user, prog, sizeof prog);
    char tk[TERMAPP_TOKEN_HEX + 1];
    if (ok) ok = !termapp_put(argv, argc, termapp_now(), tk);
    free(e);
    free(buf);
    if (!ok) return -1;
    snprintf(token, n, "%s", tk);
    return 0;
}

int apps_launch_action(const char *id, const char *action) {
    last_error[0] = 0;
    if (!action || !valid_action(action)) return -1;
    return launch(id, NULL, action);
}

/* ---- which entry runs a program (a name to show, never a decision) ---- */

/* Does the entry's Exec line run exe: its program (past "env" and VAR=value words), found as the launcher finds
 * it and resolved, is exe? With flatpak_id: is it "flatpak run [--options] <flatpak_id>[/arch/branch]"? */
static int entry_runs(const DesktopApp *a, const char *exe, const char *flatpak_id, Entry *e, char *buf) {
    char *argv[MAX_ARGS];
    int used = 0;
    if (parse_desktop(a->file, cached_lang[0] ? cached_lang : "en", e) || !e->exec[0]) return 0;
    int argc = split_exec(e->exec, e, argv, buf, 4096, NULL, &used), i = 0;
    if (argc > 0 && (!strcmp(argv[0], "env") || !strcmp(argv[0], "/usr/bin/env")))
        for (i = 1; i < argc && argv[i][0] != '=' && argv[i][0] != '-' && strchr(argv[i], '='); i++) {}
    if (i >= argc) return 0;
    char prog[PATH_MAX], real[PATH_MAX];
    if (find_program(argv[i], a->user, prog, sizeof prog) || !realpath(prog, real)) return 0;
    if (!flatpak_id) return !strcmp(real, exe);
    const char *base = strrchr(real, '/');
    if (strcmp(base ? base + 1 : real, "flatpak") || i + 1 >= argc || strcmp(argv[i + 1], "run")) return 0;
    size_t fl = strlen(flatpak_id);
    for (int j = i + 2; j < argc; j++) {
        if (argv[j][0] == '-') continue;            /* flatpak run's options are --name=value */
        return !strncmp(argv[j], flatpak_id, fl) && (argv[j][fl] == 0 || argv[j][fl] == '/');
    }
    return 0;
}

/* Could this entry be the one meant? The window's app_id (only a hint), the Flatpak id, or its program's name. */
static int entry_hinted(const DesktopApp *a, const char *hint) {
    if (!hint || !hint[0]) return 0;
    size_t il = strlen(a->id), hl = strlen(hint);
    return !strcasecmp(a->id, hint) || !strcasecmp(a->wmclass, hint) || (il > hl && a->id[il - hl - 1] == '.' && !strcasecmp(a->id + il - hl, hint));
}

void apps_name_of_program(const char *exe, const char *flatpak_id, const char *hint, char *out, size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    if (flatpak_id && !flatpak_id[0]) flatpak_id = NULL;
    if (!flatpak_id && (!exe || exe[0] != '/')) return;
    const DesktopApp *list;
    int cnt = apps_list(cached_lang[0] ? cached_lang : "en", &list);
    const char *base = exe ? strrchr(exe, '/') : NULL;
    base = base ? base + 1 : "";
    Entry *e = malloc(sizeof *e);
    char *buf = malloc(4096);
    /* first the entries the hint or the Flatpak id points at, then those whose program has the same name; at most
     * a few files read */
    for (int pass = 0, tried = 0; e && buf && pass < 2 && !out[0]; pass++) {
        for (int i = 0; i < cnt && tried < 32; i++) {
            const DesktopApp *a = &list[i];
            int want = pass == 0 ? (entry_hinted(a, hint) || (flatpak_id && !strcmp(a->id, flatpak_id)))
                                 : !entry_hinted(a, hint) && !flatpak_id && base[0] && !strcmp(a->bin, base);
            if (!want) continue;
            tried++;
            if (entry_runs(a, exe, flatpak_id, e, buf)) { snprintf(out, n, "%s", a->name); break; }
        }
    }
    free(e);
    free(buf);
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

/* ---- Flatpak apps (docs/FLATPAK.md; AGENTS.md "Flatpak as an app source", E1 and E2) ----
 * Only an id: the installation is read here, the command line is built by fpak_argv from fixed tables, never an
 * export's Exec line. Every Flatpak app runs in its root-owned app-lordos-fp-<id>.scope and in the launcher's view
 * (never systemd/ in it: otherwise `flatpak run` has the user manager move it out of the scope). Phase 1: x86-64. */

/* /usr/bin/flatpak root's and not writable by anyone else */
__attribute__((unused))   /* unused off x86-64: phase 1 launches Flatpak apps only there */
static int flatpak_binary_ok(void) {
    struct stat st;
    return !lstat(FPAK_FLATPAK, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022) && (st.st_mode & 0111);
}

/* the user's installation first, then the system's (read only: LordOS never installs there) */
static int flatpak_find(const char *id, char *arch, size_t an, char *branch, size_t bn, char *md, size_t mn) {
    const char *h = getenv("HOME");
    char root[PATH_MAX];
    if (h && h[0] == '/' && snprintf(root, sizeof root, "%s/.local/share/flatpak", h) < (int)sizeof root &&
        !fpak_installed(root, id, arch, an, branch, bn, md, mn))
        return 0;
    return fpak_installed("/var/lib/flatpak", id, arch, an, branch, bn, md, mn);
}

int apps_flatpak_installed(const char *id) {
    char arch[32], branch[FPAK_BRANCH_MAX];
    return fpak_id_ok(id) && !flatpak_find(id, arch, sizeof arch, branch, sizeof branch, NULL, 0);
}

/* The user's Flatpak installation and Flatpak's own cache, writable in the view: `flatpak run` opens its OSTree
 * repository for writing (locks) and keeps its ld.so caches in ~/.cache/flatpak, created on first use ("Read-only file
 * system" on VM 127 without them). It is `flatpak run`'s alone: inside the sandbox both are hidden by fpak_argv's
 * --nofilesystem (a tmpfs over them; checked: an app given the whole home sees them empty). Only real folders, no link
 * on the way. */
static void flatpak_installation(FilesPlan *p) {
    char path[PATH_MAX + 64];
    char inst[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/.local/share/flatpak", p->home) < (int)sizeof path && realpath(path, inst) &&
        !strcmp(inst, path) && is_dir(inst)) {
        plan_add(p, 1, "%s%s", inst, "");
        /* its permission overrides and its exports stay read-only, kernel-enforced (ro binds go on after the rw ones):
         * `flatpak run` only reads them, and nothing in the view may widen an app's permissions or plant an export
         * (overrides is created first, so it can't be made later) */
        static const char *const ro[] = { "/overrides", "/exports", NULL };
        for (int k = 0; ro[k]; k++) {
            char sub[PATH_MAX + 32], r[PATH_MAX];
            snprintf(sub, sizeof sub, "%s%s", inst, ro[k]);
            if (k == 0) mkdir(sub, 0755);
            if (realpath(sub, r) && !strcmp(r, sub) && is_dir(r)) plan_add(p, 0, "%s%s", r, "");
        }
    }
    if (snprintf(path, sizeof path, "%s/.cache", p->home) >= (int)sizeof path) return;
    mkdir(path, 0700);
    strcat(path, "/flatpak");
    mkdir(path, 0755);
    /* only that very folder: ~/.cache or it a link would put the grant somewhere else */
    char real[PATH_MAX];
    struct stat st;
    if (realpath(path, real) && !strcmp(real, path) && !lstat(real, &st) && S_ISDIR(st.st_mode)) plan_add(p, 1, "%s%s", real, "");
}

/* The writable folders of a Flatpak app's read-only home: ~/.var/app/<id> (created) and the person's folders, each a
 * real folder (not a link) directly or deeper in the home and not a protected one; and the opened path when grant_ok
 * allows writing it. A line of user-dirs.dirs it doesn't understand is skipped: fewer writable folders, never more. */
static void flatpak_writable(FilesPlan *p, const char *id, const char *opened) {
    char path[PATH_MAX + 128];
    static const char *const dirs[] = { "%s/.var%s", "%s/.var/app%s", "%s/.var/app/%s" };
    for (int k = 0; k < 3; k++) {
        if (snprintf(path, sizeof path, dirs[k], p->home, k == 2 ? id : "") >= (int)sizeof path) return;
        mkdir(path, 0700);
    }
    if (is_dir(path)) plan_add(p, 1, "%s%s", path, "");
    flatpak_installation(p);
    snprintf(path, sizeof path, "%s/.config/user-dirs.dirs", p->home);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    FILE *f = fd >= 0 ? fdopen(fd, "re") : NULL;
    if (!f && fd >= 0) close(fd);
    static const char *const keys[] = { "XDG_DESKTOP_DIR", "XDG_DOCUMENTS_DIR", "XDG_DOWNLOAD_DIR", "XDG_MUSIC_DIR",
                                        "XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "XDG_PUBLICSHARE_DIR", "XDG_TEMPLATES_DIR", NULL };
    char line[PATH_MAX];
    while (f && fgets(line, sizeof line, f)) {
        int known = 0;
        for (int k = 0; keys[k]; k++) {
            size_t kl = strlen(keys[k]);
            if (!strncmp(line, keys[k], kl) && line[kl] == '=') { known = (int)kl + 1; break; }
        }
        if (!known || strncmp(line + known, "\"$HOME/", 7)) continue;
        char *rel = line + known + 7, *q = strchr(rel, '"');
        if (!q || q == rel) continue;
        *q = 0;
        if (strstr(rel, "..") || strchr(rel, '\\') || rel[0] == '/') continue;
        /* resolved, and only if it is exactly that folder: no link and no "." or empty part anywhere on its way, strictly
         * below the home (never the home itself: "Documents/." is the home), a folder, and not a protected one */
        char real[PATH_MAX];
        struct stat st;
        if (snprintf(path, sizeof path, "%s/%s", p->home, rel) >= (int)sizeof path || !realpath(path, real) || strcmp(real, path) ||
            !path_under(real, p->home) || !strcmp(real, p->home) || lstat(real, &st) || !S_ISDIR(st.st_mode))
            continue;
        if (!fsops_protected_resolved(real) && grant_ok(real)) plan_add(p, 1, "%s%s", real, "");
    }
    if (f) fclose(f);
    if (opened) {
        struct stat st;
        if (stat(opened, &st)) return;
        if (grant_ok(opened)) plan_add(p, 1, "%s%s", opened, "");
        snprintf(p->opened, sizeof p->opened, "%s", opened);
        p->cfg.opened = p->opened;
        p->cfg.opened_dev = (unsigned long long)st.st_dev;
        p->cfg.opened_ino = (unsigned long long)st.st_ino;
    }
}

/* The view of a Flatpak app. Unrestricted: the home as it is (Flatpak exposes only what its arguments allow) and the
 * session bus, which the app reaches only through Flatpak's filtering proxy (--nosocket=session-bus). Device-restricted:
 * no bus, as for Linux apps. No access to files: an empty home with only its ~/.var/app/<id> (rw), the installation
 * (ro, `flatpak run` reads it) and the opened file. Never systemd/ or any other socket of the session. */
/* An unrestricted Flatpak app keeps the session's X display, as an unrestricted Linux app does: the X socket of
 * $DISPLAY (":<n>", the local Xwayland) and the cookie file of $XAUTHORITY when it is in the runtime folder.
 * `flatpak run` then gives it to the app only if the app declares x11 or fallback-x11. A restricted one never
 * gets it: X11 apps can watch each other's windows, input and selections (docs/PRIVACY.md, "X11 apps"). */
static void flatpak_x11(FilesPlan *p) {
    const char *d = getenv("DISPLAY"), *xa = getenv("XAUTHORITY");
    if (!d || d[0] != ':') return;
    char path[32];
    if (snprintf(path, sizeof path, "/tmp/.X11-unix/X%s", d + 1) >= (int)sizeof path || !sandbox_x11_path_ok(path)) return;
    snprintf(p->x11, sizeof p->x11, "%s", path);
    p->cfg.x11 = p->x11;
    size_t rl = strlen(p->runtime);
    if (xa && rl && !strncmp(xa, p->runtime, rl) && xa[rl] == '/' && xa[rl + 1] && !strchr(xa + rl + 1, '/') &&
        strlen(xa + rl + 1) < sizeof p->xauth) {
        snprintf(p->xauth, sizeof p->xauth, "%s", xa + rl + 1);
        p->cfg.xauth = p->xauth;
    }
}

__attribute__((unused))   /* unused off x86-64: phase 1 launches Flatpak apps only there */
static FilesPlan *flatpak_plan(const char *id, uint32_t restricted, const char *opened) {
    FilesPlan *p = view_plan(NULL);
    if (!p) return NULL;
    int dev = (restricted & (FW_R_NO_FILES | FW_R_NO_MIC | FW_R_NO_CAMERA | FW_R_NO_USB | FW_R_NO_BLUETOOTH)) != 0;
    if (!dev) {
        int n = 0;
        while (p->cfg.sockets[n]) n++;
        if (n < 7) { p->cfg.sockets[n++] = "bus"; p->cfg.sockets[n] = NULL; }
    }
    if (!dev) flatpak_x11(p);
    if (!(restricted & FW_R_NO_FILES)) {
        /* the real home, read-only: writable only its own ~/.var/app/<id> and the person's folders (Desktop,
         * Documents, Downloads, Music, Pictures, Videos, Public, Templates of ~/.config/user-dirs.dirs) and the opened
         * path. Kernel-enforced, whatever Flatpak grants: no startup file, key or autostart entry can be written,
         * created or renamed into place (decided by the maintainer on 2026-11-19, after a security review). */
        p->cfg.home_ro = 1;
        flatpak_writable(p, id, opened);
    }
    if (restricted & FW_R_NO_FILES) {
        p->cfg.keep_home = 0;
        char path[PATH_MAX + 128];
        static const char *const dirs[] = { "%s/.var%s", "%s/.var/app%s", "%s/.var/app/%s" };
        for (int k = 0; k < 3; k++) {
            if (snprintf(path, sizeof path, dirs[k], p->home, k == 2 ? id : "") >= (int)sizeof path) { free(p); return NULL; }
            mkdir(path, 0700);
        }
        if (is_dir(path)) plan_add(p, 1, "%s%s", path, "");
        flatpak_installation(p);
        if (opened) {
            struct stat st;
            if (stat(opened, &st)) { free(p); return NULL; }
            plan_add(p, grant_ok(opened), "%s%s", opened, "");
            snprintf(p->opened, sizeof p->opened, "%s", opened);
            p->cfg.opened = p->opened;
            p->cfg.opened_dev = (unsigned long long)st.st_dev;
            p->cfg.opened_ino = (unsigned long long)st.st_ino;
        }
    }
    return p;
}

static void apps_view_exec(const SandboxFilesCfg *cfg, int file_rules, const char *prog, char *const argv[], int spawner);

static void (*spawn_hook)(int fd, const char *id);
void apps_set_spawn_hook(void (*hook)(int fd, const char *id)) { spawn_hook = hook; }

static int launch_flatpak(const char *id, const char *opened) {
    last_error[0] = 0;
#if !defined(__x86_64__)
    (void)id; (void)opened;
    return -1;                                  /* phase 1: x86-64 PCs only (apps_flatpak_list lists none elsewhere) */
#else
    char arch[32], branch[FPAK_BRANCH_MAX];
    char *md = malloc(16384);
    if (!md || !fpak_id_ok(id) || flatpak_find(id, arch, sizeof arch, branch, sizeof branch, md, 16384) ||
        strcmp(arch, "x86_64") || !flatpak_binary_ok()) {
        free(md);
        return -1;
    }
    free(md);
    char fwid[FW_APP_MAX], scope[96];
    snprintf(fwid, sizeof fwid, "flatpak:%s", id);
    snprintf(scope, sizeof scope, "fp-%s", id);
    static const uint32_t flags[] = { FW_R_NO_NETWORK, FW_R_NO_USB, FW_R_NO_BLUETOOTH, FW_R_NO_FILES, FW_R_NO_MIC, FW_R_NO_CAMERA };
    uint32_t restricted = 0;
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) if (restrict_blocks(fwid, flags[i])) restricted |= flags[i];
    /* every Flatpak app runs in the view, whose abstract sockets only Landlock's scoping (ABI 6) keeps apart */
    if (sandbox_landlock_abi() < 6) {
        fprintf(stderr, "apps: %s: this kernel's Landlock is too old (ABI < 6) for a Flatpak app: not launched\n", id);
        snprintf(last_error, sizeof last_error, "apps.restrictKernel");
        return -1;
    }
    FilesPlan *plan = flatpak_plan(id, restricted, opened);
    char *argv[FPAK_ARGV_MAX];
    char *buf = malloc(8192);
    FpakRun r = { .id = id, .arch = arch, .branch = branch, .restricted = restricted, .opened_ro = opened && !grant_ok(opened),
                  .home = plan ? plan->home : NULL, .opened = opened, .x11 = plan && plan->cfg.x11 };   /* flatpak_x11 */
    if (!plan || !buf || fpak_argv(&r, argv, buf, 8192) < 0) {
        fprintf(stderr, "apps: %s: the Flatpak app's view or command line can't be built: not launched\n", id);
        free(plan);
        free(buf);
        return -1;
    }
    char token[XACT_TOKEN_LEN + 1] = "";
    const char *tk = xact_issue(xact_now(), XACT_LAUNCH_MS, 1, id);
    if (tk) snprintf(token, sizeof token, "%s", tk);
    int logfd = open_log(id, NULL, opened);
    /* the app's Spawn helper (fpspawner.h): lord-server keeps one end, the view's helper the other */
    int sw[2] = { -1, -1 };
    /* (a device-restricted app has no session bus, so no Spawn to serve) */
    if (spawn_hook && !(restricted & (FW_R_NO_FILES | FW_R_NO_MIC | FW_R_NO_CAMERA | FW_R_NO_USB | FW_R_NO_BLUETOOTH)) &&
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sw))
        sw[0] = sw[1] = -1;
    pid_t pid = fork();
    if (pid == 0) {
        if (sw[0] >= 0) close(sw[0]);
        if (logfd == FPSW_FD) logfd = fcntl(logfd, F_DUPFD_CLOEXEC, FPSW_FD + 1);
        /* the helper's end at FPSW_FD (kept through the exec of --launch-view, the only fd besides LV_FD) */
        if (sw[1] >= 0 && (sw[1] == FPSW_FD ? fcntl(sw[1], F_SETFD, 0) : dup2(sw[1], FPSW_FD)) < 0) _exit(126);
        if (sw[1] >= 0 && sw[1] != FPSW_FD) close(sw[1]);
        setsid();                                   /* its own session: never signals for lord-server's group */
        if (fork() != 0) _exit(0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        for (int s = 1; s < 32; s++) signal(s, SIG_DFL);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
        if (logfd >= 0) { dup2(logfd, 1); dup2(logfd, 2); }
        if (sw[1] >= 0) { syscall(SYS_close_range, 3, FPSW_FD - 1, 0); syscall(SYS_close_range, FPSW_FD + 1, ~0u, 0); }
        else syscall(SYS_close_range, 3, ~0u, 0);
        const char *home = getenv("HOME");
        if (home && chdir(home)) {}
        /* its root-owned scope (not app_scope_reserved: "fp-" is reserved for this path alone) */
        if (app_scope_enter(scope) && getenv("LORDOS_SESSIOND")) {
            static const char msg[] = "apps: could not place the Flatpak app in its own scope: not launched\n";
            if (write(2, msg, sizeof msg - 1) < 0) {}
            _exit(126);
        }
        if (!plan->cfg.x11) xauth_env_drop();          /* kept only with the X socket in its view (flatpak_x11) */
        /* a device-restricted app has no session bus at all: `flatpak run` then starts no proxy */
        if (restricted & (FW_R_NO_FILES | FW_R_NO_MIC | FW_R_NO_CAMERA | FW_R_NO_USB | FW_R_NO_BLUETOOTH)) unsetenv("DBUS_SESSION_BUS_ADDRESS");
        if (token[0]) setenv("XDG_ACTIVATION_TOKEN", token, 1); else unsetenv("XDG_ACTIVATION_TOKEN");
        /* the view, then Landlock's scoping only (its file rules would forbid the mounts bubblewrap makes), in a clean
         * process image (apps_launch_view_main): never returns on success */
        apps_view_exec(&plan->cfg, 0, FPAK_FLATPAK, argv, sw[1] >= 0);
        static const char msg[] = "apps: could not build the Flatpak app's view: not launched\n";
        if (write(2, msg, sizeof msg - 1) < 0) {}
        _exit(126);
    }
    if (logfd >= 0) close(logfd);
    free(plan);
    free(buf);
    if (sw[1] >= 0) close(sw[1]);
    if (pid < 0) { if (sw[0] >= 0) close(sw[0]); return -1; }
    if (sw[0] >= 0) spawn_hook(sw[0], id);          /* it owns the fd now */
    waitpid(pid, NULL, 0);
    return 0;
#endif
}

int apps_launch_flatpak(const char *id) { return launch_flatpak(id, NULL); }

/* ---- the installed Flatpak apps, for the launcher, the Dock, Privacy and the Monitor (docs/FLATPAK.md) ----
 * From the exported .desktop files, whose name must be an installed app's id (fpak_id_ok, flatpak_find): only the
 * name, comment, categories, MIME types, StartupWMClass and icon are read, never Exec (apps_launch_flatpak builds the
 * command line). The user's installation first, then the system's. */
#define FLATPAK_LIST_MAX 128
static DesktopApp flatpaks[FLATPAK_LIST_MAX];

int apps_flatpak_list(const char *lang, const DesktopApp **out) {
    int n = 0;
    *out = flatpaks;
#if !defined(__x86_64__)
    (void)lang;
    return 0;                                   /* phase 1: x86-64 PCs only */
#endif
    const char *h = getenv("HOME");
    char roots[2][PATH_MAX];
    snprintf(roots[0], sizeof roots[0], "%s/.local/share/flatpak/exports/share", h && h[0] == '/' ? h : "/nonexistent");
    snprintf(roots[1], sizeof roots[1], "/var/lib/flatpak/exports/share");
    IconRoots ir;
    memset(&ir, 0, sizeof ir);
    for (int k = 0; k < 2; k++)
        if (snprintf(ir.themes[k], sizeof ir.themes[k], "%.4000s/icons/hicolor", roots[k]) >= (int)sizeof ir.themes[k]) ir.themes[k][0] = 0;
    snprintf(ir.themes[2], sizeof ir.themes[2], "/usr/share/icons/hicolor");
    ir.nthemes = ir.npng = 3;
    Entry *e = malloc(sizeof *e);
    for (int k = 0; e && k < 2; k++) {
        char dir[PATH_MAX + 32];
        snprintf(dir, sizeof dir, "%s/applications", roots[k]);
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) && n < FLATPAK_LIST_MAX) {
            size_t l = strlen(de->d_name);
            if (l <= 8 || strcmp(de->d_name + l - 8, ".desktop") || l - 8 > FPAK_ID_MAX) continue;
            char id[FPAK_ID_MAX + 1];
            memcpy(id, de->d_name, l - 8);
            id[l - 8] = 0;
            if (!apps_flatpak_installed(id)) continue;
            int dup = 0;
            for (int i = 0; i < n; i++) if (!strcmp(flatpaks[i].id, id)) dup = 1;
            char file[PATH_MAX * 2];
            snprintf(file, sizeof file, "%s/%s", dir, de->d_name);
            if (dup || parse_desktop(file, lang, e) || !e->is_app || e->hidden || e->nodisplay || !e->a.name[0]) continue;
            DesktopApp *a = &flatpaks[n++];
            *a = e->a;
            snprintf(a->id, sizeof a->id, "%s", id);
            if (snprintf(a->file, sizeof a->file, "%.4000s", file) >= (int)sizeof a->file) a->file[0] = 0;
            a->bin[0] = 0;
            a->terminal = 0;
            a->user = k == 0;
            if (!a->wmclass[0]) snprintf(a->wmclass, sizeof a->wmclass, "%s", id);   /* a Flatpak window's app_id is its id */
            a->icon[0] = 0;
            iconfind_app(&ir, e->icon_name, a->icon, sizeof a->icon);
        }
        closedir(d);
    }
    free(e);
    *out = flatpaks;
    return n;
}


/* "Open with" for a Flatpak app: launchWith's rules (only an app whose export declares MimeType; a real path in the home
 * folder or the user's mounts, a regular file or folder); it is granted with --filesystem=<path> and passed after the id. */
int apps_launch_flatpak_with(const char *id, const char *path) {
    char real[PATH_MAX];
    struct stat st;
    const DesktopApp *l;
    int n = apps_flatpak_list(cached_lang[0] ? cached_lang : "en", &l), opens = 0;
    for (int i = 0; i < n && id; i++) if (!strcmp(l[i].id, id)) opens = l[i].mime[0] != 0;
    if (!opens) return -1;
    if (!path || !realpath(path, real) || !fsops_allowed(real) || stat(real, &st) || !(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))) return -1;
    /* never the home folder itself, nor a folder holding a protected one: the grant would reach it whole */
    const char *h = getenv("HOME");
    char home[PATH_MAX];
    if (fsops_protected(real) || fsops_protected_below(real) || (h && realpath(h, home) && !strcmp(real, home))) return -1;
    return launch_flatpak(id, real);
}

/* A remote desktop or web address for the app that declares that scheme: vnc:// or rdp:// and a strict set of characters.
 * It goes as ONE argument, never through a shell. A web address may be longer (APPS_URI_WEB_MAX): Cloud's "Tell Us on
 * GitHub" prefills an issue form with the check's report, about 1.5 KB (#79); the launcher's argument buffer is 4 KB. */
int apps_launch_uri(const char *id, const char *uri) {
    static const char *const schemes[] = { "vnc", "rdp", "http", "https" };
    size_t n = uri ? strlen(uri) : 0;
    int web = n >= 4 && !strncmp(uri, "http", 4);
    if (n < 7 || n > (web ? APPS_URI_WEB_MAX : 300)) return -1;
    /* a web address also has a query: it still goes as one argument, and quotes, spaces and backslashes stay out */
    const char *chars = web ? ":/.-_@[]?=&%#+~,;" : ":/.-_@[]";
    for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)uri[i]) && !strchr(chars, uri[i])) return -1;
    for (size_t i = 0; i < 4; i++) {
        size_t l = strlen(schemes[i]);
        if (strncmp(uri, schemes[i], l) || strncmp(uri + l, "://", 3)) continue;
        if (!uri[l + 3] || uri[l + 3] == '-') return -1;
        const DesktopApp *a = find_app(id);
        char mime[32];
        snprintf(mime, sizeof mime, "x-scheme-handler/%s", schemes[i]);
        if (!a || !list_has(a->mime, mime)) return -1;
        return launch(id, uri, NULL);
    }
    return -1;
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
/* ---- the view, built in a clean process (lord-server --launch-view) ----
 * lord-server drops from root to the user when a session starts, so it isn't dumpable: its /proc/self files stay
 * root's, and a child of it can't write its new user namespace's uid_map. Every restricted app failed to start from
 * the desktop ("not launched"; found on VM 127, 2026-11-19), while the tests, which run the launcher from build/launch-cli,
 * passed. Making the child dumpable would let the user's other processes read the copy of lord-server's memory it holds
 * until exec (unless Yama stops them). Instead the child execs /proc/self/exe --launch-view: a fresh process image,
 * dumpable by execve and holding nothing of lord-server's, builds the view and execs the app. The plan goes on fd 3,
 * a sealed memfd of "key\0value\0" pairs, written here and read back by apps_launch_view_main. */
#define LV_FD 3
#define LV_MAX (1 << 20)

static int lv_put(int fd, const char *k, const char *v) {
    if (!v) return 0;
    size_t kl = strlen(k) + 1, vl = strlen(v) + 1;
    return write(fd, k, kl) == (ssize_t)kl && write(fd, v, vl) == (ssize_t)vl ? 0 : -1;
}

/* In the launcher's child, after its scope and environment: never returns on success. */
static void apps_view_exec(const SandboxFilesCfg *cfg, int file_rules, const char *prog, char *const argv[], int spawner) {
    int fd = memfd_create("lordos-launch-view", MFD_ALLOW_SEALING);
    if (fd < 0) return;
    char num[32];
    int bad = lv_put(fd, "home", cfg->home) || lv_put(fd, "runtime", cfg->runtime_dir) || lv_put(fd, "cwd", cfg->cwd) ||
              lv_put(fd, "opened", cfg->opened) || lv_put(fd, "prog", prog) || lv_put(fd, "x11", cfg->x11) || lv_put(fd, "xauth", cfg->xauth);
    for (int i = 0; !bad && i < SANDBOX_FILES_MAX && cfg->rw[i]; i++) bad = lv_put(fd, "rw", cfg->rw[i]);
    for (int i = 0; !bad && i < SANDBOX_FILES_MAX && cfg->ro[i]; i++) bad = lv_put(fd, "ro", cfg->ro[i]);
    for (int i = 0; !bad && i < 8 && cfg->sockets[i]; i++) bad = lv_put(fd, "sock", cfg->sockets[i]);
    snprintf(num, sizeof num, "%llu", cfg->opened_dev);
    bad = bad || lv_put(fd, "odev", num);
    snprintf(num, sizeof num, "%llu", cfg->opened_ino);
    bad = bad || lv_put(fd, "oino", num) || lv_put(fd, "keephome", cfg->keep_home ? "1" : "0") ||
          lv_put(fd, "homero", cfg->home_ro ? "1" : "0") || lv_put(fd, "rules", file_rules ? "files" : "scope") ||
          (spawner && lv_put(fd, "spawner", "1"));
    for (int i = 0; !bad && argv[i]; i++) bad = lv_put(fd, "arg", argv[i]);
    if (bad || fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) || lseek(fd, 0, SEEK_SET)) { close(fd); return; }
    if (fd != LV_FD) { if (dup2(fd, LV_FD) < 0) { close(fd); return; } close(fd); }
    static char *const self[] = { "lord-server", "--launch-view", NULL };
    execve("/proc/self/exe", self, environ);
}

int apps_launch_view_main(void) {
    static const char fail[] = "apps: the app's view could not be built: not launched\n";
    prctl(PR_SET_NAME, "lordos-view");          /* not "exe" (/proc/self/exe) in the Monitor; the app it execs gets its own */
    char *buf = malloc(LV_MAX + 2);
    size_t got = 0;
    for (ssize_t r; buf && got < LV_MAX && (r = read(LV_FD, buf + got, LV_MAX - got)) != 0;) {
        if (r < 0) { if (errno == EINTR) continue; break; }
        got += (size_t)r;
    }
    close(LV_FD);
    if (!buf || !got || got >= LV_MAX || buf[got - 1]) { if (write(2, fail, sizeof fail - 1) < 0) {} return 126; }
    buf[got] = 0;
    SandboxFilesCfg cfg;
    memset(&cfg, 0, sizeof cfg);
    const char *prog = NULL, *rules = NULL;
    int spawner = 0;
    char *args[MAX_ARGS + 1];
    int na = 0, nrw = 0, nro = 0, ns = 0, bad = 0;
    for (char *p = buf; p < buf + got && !bad;) {
        char *k = p, *v = p + strlen(p) + 1;
        if (v >= buf + got) { bad = 1; break; }
        p = v + strlen(v) + 1;
        if (!strcmp(k, "home")) cfg.home = v;
        else if (!strcmp(k, "runtime")) cfg.runtime_dir = v;
        else if (!strcmp(k, "cwd")) cfg.cwd = v;
        else if (!strcmp(k, "opened")) cfg.opened = v;
        else if (!strcmp(k, "prog")) prog = v;
        else if (!strcmp(k, "x11")) cfg.x11 = sandbox_x11_path_ok(v) ? v : NULL;
        else if (!strcmp(k, "xauth")) cfg.xauth = v[0] && v[0] != '.' && !strchr(v, '/') ? v : NULL;
        else if (!strcmp(k, "rules")) rules = v;
        else if (!strcmp(k, "keephome")) cfg.keep_home = !strcmp(v, "1");
        else if (!strcmp(k, "homero")) cfg.home_ro = !strcmp(v, "1");
        else if (!strcmp(k, "odev")) cfg.opened_dev = strtoull(v, NULL, 10);
        else if (!strcmp(k, "oino")) cfg.opened_ino = strtoull(v, NULL, 10);
        else if (!strcmp(k, "rw")) { if (nrw < SANDBOX_FILES_MAX) cfg.rw[nrw++] = v; else bad = 1; }
        else if (!strcmp(k, "ro")) { if (nro < SANDBOX_FILES_MAX) cfg.ro[nro++] = v; else bad = 1; }
        else if (!strcmp(k, "sock")) { if (ns < 7) cfg.sockets[ns++] = v; else bad = 1; }
        else if (!strcmp(k, "arg")) { if (na < MAX_ARGS) args[na++] = v; else bad = 1; }
        else if (!strcmp(k, "spawner")) spawner = !strcmp(v, "1");
        else bad = 1;
    }
    args[na] = NULL;
    /* a Flatpak app's Spawn helper: only on the scope rules (never with file rules: bubblewrap couldn't mount), and only
     * a socket at FPSW_FD; anything else there is closed */
    struct stat sst;
    if (spawner && (!rules || strcmp(rules, "scope") || fstat(FPSW_FD, &sst) || !S_ISSOCK(sst.st_mode))) bad = 1;
    if (!spawner) close(FPSW_FD);
    else fcntl(FPSW_FD, F_SETFD, FD_CLOEXEC);
    if (bad || !prog || prog[0] != '/' || !na || !rules || !cfg.home || cfg.home[0] != '/' ||
        sandbox_files_isolate(&cfg) || (!strcmp(rules, "files") ? sandbox_apply_files(&cfg) : sandbox_apply_scope())) {
        if (write(2, fail, sizeof fail - 1) < 0) {}
        return 126;
    }
    /* the helper: in the app's scope, view and Landlock domain (the one just made), so a sub-sandbox may join the app's
     * namespaces as Flatpak's own portal would let it, and never with more than the app has */
    if (spawner) {
        pid_t h = fork();
        if (h < 0) { if (write(2, fail, sizeof fail - 1) < 0) {} return 126; }
        if (h == 0) fpsw_main(FPSW_FD);
        close(FPSW_FD);
    }
    execve(prog, args, environ);
    return 127;
}

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

    char token[XACT_TOKEN_LEN + 1] = "";
    const char *tk = xact_issue(xact_now(), XACT_LAUNCH_MS, 1, id);
    if (tk) snprintf(token, sizeof token, "%s", tk);
    int logfd = open_log(id, action, file);
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
        if (logfd >= 0) { dup2(logfd, 1); dup2(logfd, 2); }
        syscall(SYS_close_range, 3, ~0u, 0);
        const char *home = getenv("HOME");
        if (!cwd[0] || chdir(cwd)) { if (home && chdir(home)) {} }
        /* its own app-lordos-<id>.scope cgroup: the firewall, the Monitor, the PipeWire guard and
         * the notification server know the app only by it. In a LordOS session a launch that can't
         * be placed there never runs (it would run as lord-server itself, with none of its rules). */
        if (app_scope_reserved(id) || (app_scope_enter(id) && getenv("LORDOS_SESSIOND"))) {
            static const char msg[] = "apps: could not place the app in its own scope: not launched\n";
            if (write(2, msg, sizeof msg - 1) < 0) {}
            _exit(126);
        }
        /* its own view (namespaces: no session sockets, only its folders), then Landlock */
        /* a restricted app gets no X display: X11 apps share one X server and can watch each
         * other's windows, input and selections (docs/PRIVACY.md, "X11 apps"); its private /tmp has no
         * X socket either, nor its own runtime folder the X cookie (wl/xauth.h) */
        if (plan) xauth_env_drop();
        if (token[0]) setenv("XDG_ACTIVATION_TOKEN", token, 1); else unsetenv("XDG_ACTIVATION_TOKEN");
        if (plan) {
            /* the view and Landlock in a clean process image (apps_launch_view_main): never returns on success */
            apps_view_exec(&plan->cfg, !plan->cfg.keep_home, prog, argv, 0);
            static const char msg[] = "apps: could not start the restricted app's view: not launched\n";
            if (write(2, msg, sizeof msg - 1) < 0) {}
            _exit(126);
        }
        execve(prog, argv, environ);
        _exit(127);
    }
    if (logfd >= 0) close(logfd);
    free(plan);
    free(buf);
    if (pid < 0) return -1;
    waitpid(pid, NULL, 0);
    return 0;
}
