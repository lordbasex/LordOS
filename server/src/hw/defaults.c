/* defaults.c — default applications on disk (see defaults.h). */
#include "defaults.h"
#include "mimeapps.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIST_MAX (256 * 1024)

static char *slurp(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > LIST_MAX) { close(fd); return NULL; }
    char *b = malloc((size_t)st.st_size + 1);
    ssize_t r = b ? read(fd, b, (size_t)st.st_size) : -1;
    close(fd);
    if (r < 0) { free(b); return NULL; }
    b[r] = 0;
    return b;
}

static const char *home(void) { const char *h = getenv("HOME"); return h && h[0] == '/' ? h : NULL; }

static int write_atomic(const char *path, const char *text) {
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    int ok = fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    if (fd >= 0) { ok = fsync(fd) == 0 && ok; close(fd); }
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

/* For tests only: a folder that stands for "/" when the system files are read (the fake XDG trees of
 * tests/defaults_test.c). Empty in lord-server: nothing else sets it. */
static char sysroot[256];
void defaults_set_root(const char *root) { snprintf(sysroot, sizeof sysroot, "%s", root && root[0] == '/' ? root : ""); }
const char *defaults_root(void) { return sysroot; }

char *defaults_read(const char *path) { return slurp(path); }

/* The mimeapps.list files in the specification's order ("MIME Applications Associations", "File
 * name and location"): the user's ($XDG_CONFIG_HOME, or ~/.config), the system configuration
 * (/etc/xdg), the user's data folder (~/.local/share/applications), then the system data folders
 * (/usr/local/share, /usr/share) and the deprecated /usr/share/applications/defaults.list. */
void defaults_load(DefaultsFiles *f) {
    memset(f, 0, sizeof *f);
    char paths[DEFAULTS_NFILES][PATH_MAX];
    int user[DEFAULTS_NFILES] = { 0 }, np = 0;
    const char *h = home(), *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/') { user[np] = 1; snprintf(paths[np++], PATH_MAX, "%s/mimeapps.list", xdg); }
    else if (h) { user[np] = 1; snprintf(paths[np++], PATH_MAX, "%s/.config/mimeapps.list", h); }
    snprintf(paths[np++], PATH_MAX, "%s/etc/xdg/mimeapps.list", sysroot);
    if (h) { user[np] = 1; snprintf(paths[np++], PATH_MAX, "%s/.local/share/applications/mimeapps.list", h); }
    snprintf(paths[np++], PATH_MAX, "%s/usr/local/share/applications/mimeapps.list", sysroot);
    snprintf(paths[np++], PATH_MAX, "%s/usr/share/applications/mimeapps.list", sysroot);
    snprintf(paths[np++], PATH_MAX, "%s/usr/share/applications/defaults.list", sysroot);
    for (int i = 0; i < np; i++) {
        f->text[i] = slurp(paths[i]);
        f->user[i] = user[i];
    }
    f->n = np;
}

void defaults_free(DefaultsFiles *f) {
    for (int i = 0; i < f->n; i++) free(f->text[i]);
    memset(f, 0, sizeof *f);
}

int defaults_removed(const DefaultsFiles *f, int upto, const char *mime, const char *id) {
    for (int i = 0; i <= upto && i < f->n; i++) if (mimeapps_removed(f->text[i], mime, id)) return 1;
    return 0;
}

int defaults_lookup(const char *mime, char *id, size_t n) {
    if (!mime_valid(mime)) return -1;
    DefaultsFiles f;
    defaults_load(&f);
    int r = -1;
    /* a file's Removed Associations apply to it and to the files after it */
    for (int i = 0; i < f.n && r; i++)
        if (mimeapps_default(f.text[i], mime, id, n) == 0 && !defaults_removed(&f, i, mime, id)) r = 0;
    defaults_free(&f);
    return r;
}

int defaults_unset(const char *mime) {
    const char *h = home();
    if (!h || !mime_valid(mime)) return -1;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/.config/mimeapps.list", h);
    char *old = slurp(path);
    if (!old) return 0;                         /* nothing chosen: already automatic */
    char *now = mimeapps_unset_default(old, mime);
    int r = !now ? -1 : !strcmp(now, old) ? 0 : write_atomic(path, now);
    free(old);
    free(now);
    return r;
}

int defaults_set(const char *mime, const char *id) {
    const char *h = home();
    if (!h || !mime_valid(mime) || !desktop_id_valid(id)) return -1;
    char dir[PATH_MAX], path[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/.config", h);
    mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/.config/mimeapps.list", h);
    char *old = slurp(path);
    char *now = mimeapps_set_default(old, mime, id);
    free(old);
    if (!now) return -1;
    int r = write_atomic(path, now);
    free(now);
    return r;
}

int defaults_guess(const char *path, char *mime, size_t n) {
    static char *globs;
    static int tried;
    if (!tried) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/usr/share/mime/globs2", sysroot);
        tried = 1;
        globs = slurp(p);
    }
    return globs ? mime_guess(globs, path, mime, n) : -1;
}

int defaults_terminal(char *id, size_t n) {
    const char *h = home();
    if (!h) return -1;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/.config/xdg-terminals.list", h);
    char *t = slurp(path);
    if (!t) return -1;
    int r = -1;
    for (char *line = strtok(t, "\n"); line && r; line = strtok(NULL, "\n")) {
        while (*line == ' ' || *line == '+') line++;     /* "+" marks a preferred entry in the spec */
        if (*line == '#' || !*line) continue;
        char *sp = strpbrk(line, " :\t");
        if (sp) *sp = 0;
        if (desktop_id_valid(line) && strlen(line) < n) { snprintf(id, n, "%s", line); r = 0; }
    }
    free(t);
    return r;
}

int defaults_set_terminal(const char *id) {
    const char *h = home();
    if (!h || !desktop_id_valid(id)) return -1;
    char path[PATH_MAX], text[DESKTOP_ID_MAX + 96];
    snprintf(path, sizeof path, "%s/.config", h);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.config/xdg-terminals.list", h);
    snprintf(text, sizeof text, "# Written by LordOS (Settings → Default Apps)\n%s\n", id);
    return write_atomic(path, text);
}
