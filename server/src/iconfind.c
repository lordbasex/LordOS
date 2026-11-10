/* iconfind.c — icon names to files in the installed themes (see iconfind.h). */
#include "iconfind.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), t = strlen(tail);
    return n > t && !strcmp(s + n - t, tail);
}

/* a regular file with that extension */
static int is_file(const char *path, const char *ext) {
    struct stat st;
    return ends_with(path, ext) && !stat(path, &st) && S_ISREG(st.st_mode);
}

/* out = the formatted path if it fits and is a regular file with that extension */
static int try_path(char *out, size_t n, const char *ext, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int try_path(char *out, size_t n, const char *ext, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    return w > 0 && (size_t)w < n && is_file(out, ext);
}

void iconfind_roots(IconRoots *r, const char *home, const char *prefix) {
    memset(r, 0, sizeof *r);
    if (home && *home) snprintf(r->themes[r->nthemes++], PATH_MAX, "%s/.local/share/icons/hicolor", home);
    snprintf(r->themes[r->nthemes++], PATH_MAX, "%s/usr/share/icons/hicolor", prefix);
    r->npng = r->nthemes;
    snprintf(r->themes[r->nthemes++], PATH_MAX, "%s/usr/share/icons/Adwaita", prefix);
    snprintf(r->pixmaps, PATH_MAX, "%s/usr/share/pixmaps", prefix);
    snprintf(r->appstream, PATH_MAX, "%s/var/lib/lordos/pkg/icons/64", prefix);
}

/* the SVGs of a name in these kinds of folder: every theme's scalable/ first, then /usr/share/pixmaps */
static int find_svg(const IconRoots *r, const char *const *kinds, const char *name, char *out, size_t n) {
    for (int k = 0; kinds[k]; k++)
        for (int t = 0; t < r->nthemes; t++)
            if (try_path(out, n, ".svg", "%s/scalable/%s/%s.svg", r->themes[t], kinds[k], name)) return 1;
    return try_path(out, n, ".svg", "%s/%s.svg", r->pixmaps, name);
}

/* the symbolic ones, last: "<name>-symbolic.svg" in symbolic/ (where the themes put them now) or scalable/ */
static int find_symbolic(const IconRoots *r, const char *const *kinds, const char *name, char *out, size_t n) {
    const char *suffix = ends_with(name, "-symbolic") ? "" : "-symbolic";
    for (int k = 0; kinds[k]; k++)
        for (int t = 0; t < r->nthemes; t++) {
            if (try_path(out, n, ".svg", "%s/symbolic/%s/%s%s.svg", r->themes[t], kinds[k], name, suffix)) return 1;
            if (*suffix && try_path(out, n, ".svg", "%s/scalable/%s/%s%s.svg", r->themes[t], kinds[k], name, suffix)) return 1;
        }
    return 0;
}

/* the 64 px AppStream PNG downloaded by the package engine (<package>_<icon>.png) */
static int find_appstream(const IconRoots *r, const char *name, char *out, size_t n) {
    DIR *d = opendir(r->appstream);
    if (!d) return 0;
    char want[300];
    snprintf(want, sizeof want, "_%s.png", name);
    size_t wl = strlen(want);
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l > wl && !strcmp(e->d_name + l - wl, want)) found = try_path(out, n, ".png", "%s/%s", r->appstream, e->d_name);
    }
    closedir(d);
    return found;
}

int iconfind_app(const IconRoots *r, const char *name, char *out, size_t n) {
    static const int sizes[] = { 64, 48, 128, 96, 256, 72, 32, 512 };
    static const char *const kinds[] = { "apps", NULL };
    out[0] = 0;
    if (!name || !name[0] || strstr(name, "..") || strlen(name) >= 256) return 0;
    if (name[0] == '/') {
        if ((is_file(name, ".png") || is_file(name, ".svg")) && strlen(name) < n) { snprintf(out, n, "%s", name); return 1; }
        return 0;
    }
    for (int t = 0; t < r->npng; t++)
        for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++)
            if (try_path(out, n, ".png", "%s/%dx%d/apps/%s.png", r->themes[t], sizes[s], sizes[s], name)) return 1;
    if (try_path(out, n, ".png", "%s/%s.png", r->pixmaps, name)) return 1;
    if (find_svg(r, kinds, name, out, n) || find_appstream(r, name, out, n) || find_symbolic(r, kinds, name, out, n)) return 1;
    out[0] = 0;
    return 0;
}

int iconfind_status(const IconRoots *r, const char *name, char *out, size_t n) {
    static const int sizes[] = { 32, 24, 22, 48, 64, 16, 128, 256 };
    static const char *const kinds[] = { "status", "apps", NULL };
    out[0] = 0;
    size_t len = name ? strlen(name) : 0;
    if (!len || len > 128 || name[0] == '.' || strstr(name, "..")) return 0;
    for (const char *p = name; *p; p++) if (!isalnum((unsigned char)*p) && !strchr("._-+", *p)) return 0;
    for (int k = 0; kinds[k]; k++)
        for (int t = 0; t < r->npng; t++)
            for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++)
                if (try_path(out, n, ".png", "%s/%dx%d/%s/%s.png", r->themes[t], sizes[s], sizes[s], kinds[k], name)) return 1;
    if (try_path(out, n, ".png", "%s/%s.png", r->pixmaps, name)) return 1;
    if (find_svg(r, kinds, name, out, n) || find_symbolic(r, kinds, name, out, n)) return 1;
    out[0] = 0;
    return 0;
}

int iconfind_svg_allowed(const char *real, const char *home) {
    static const char *const sys[] = { "/usr/share/", "/usr/local/share/", "/opt/", "/var/lib/flatpak/exports/share/", NULL };
    static const char *const own[] = { "/.local/share/icons/", "/.icons/", "/.local/share/flatpak/exports/share/", NULL };
    if (!real || real[0] != '/' || !ends_with(real, ".svg") || strstr(real, "/../")) return 0;
    for (int i = 0; sys[i]; i++) if (!strncmp(real, sys[i], strlen(sys[i]))) return 1;
    size_t hl = home ? strlen(home) : 0;
    if (hl < 2 || strncmp(real, home, hl)) return 0;
    for (int i = 0; own[i]; i++) if (!strncmp(real + hl, own[i], strlen(own[i]))) return 1;
    return 0;
}
