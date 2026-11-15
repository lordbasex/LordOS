/* browserprefs.c — LordOS's initial preferences for the Chromium-based browsers from the Store
 * (see browserprefs.h for the rules). */
#include "browserprefs.h"
#include "../sec/json.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The closed list: Debian package → the directory its browser binary lives in (where Chromium looks
 * for initial_preferences). Debian's own chromium is not here: it reads /etc/chromium, written by the
 * image (tools/iso/browser-defaults.sh) and migration 005. Vivaldi isn't either: its interface is its
 * own, not Chromium's, and neither key changes it. */
static const struct { const char *pkg, *dir; } BROWSERS[] = {
    { "google-chrome-stable",   "/opt/google/chrome" },
    { "google-chrome-beta",     "/opt/google/chrome-beta" },
    { "google-chrome-unstable", "/opt/google/chrome-unstable" },
    { "brave-browser",          "/opt/brave.com/brave" },
    { "brave-browser-beta",     "/opt/brave.com/brave-beta" },
    { "brave-browser-nightly",  "/opt/brave.com/brave-nightly" },
    { "microsoft-edge-stable",  "/opt/microsoft/msedge" },
    { "microsoft-edge-beta",    "/opt/microsoft/msedge-beta" },
    { "microsoft-edge-dev",     "/opt/microsoft/msedge-dev" },
};
#define NBROWSERS (sizeof BROWSERS / sizeof BROWSERS[0])

#define PREFS_MAX (1u << 20)

/* What a new file holds: the same two keys as /etc/chromium/master_preferences. */
static const char FRESH[] =
    "{\n"
    "  \"browser\": {\n"
    "    \"custom_chrome_frame\": true\n"
    "  },\n"
    "  \"extensions\": {\n"
    "    \"theme\": {\n"
    "      \"system_theme\": 1\n"
    "    }\n"
    "  }\n"
    "}\n";

/* LordOS's keys: a path of object members and the leaf's JSON value. */
static const struct { const char *path[3]; int n; const char *value; } WANTS[] = {
    { { "browser", "custom_chrome_frame" }, 2, "true" },
    { { "extensions", "theme", "system_theme" }, 3, "1" },   /* 1 = GTK */
};

const char *browser_prefs_dir(const char *pkg) {
    if (!pkg) return NULL;
    for (size_t i = 0; i < NBROWSERS; i++) if (!strcmp(BROWSERS[i].pkg, pkg)) return BROWSERS[i].dir;
    return NULL;
}

/* ---- a scanner over text json_parse already accepted (only finds positions) ---- */

static size_t skip_ws(const char *t, size_t n, size_t p) {
    while (p < n && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r')) p++;
    return p;
}

static size_t skip_string(const char *t, size_t n, size_t p) {     /* p at the opening quote */
    for (p++; p < n && t[p] != '"'; p++) if (t[p] == '\\') p++;
    return p < n ? p + 1 : n;
}

static size_t skip_value(const char *t, size_t n, size_t p) {
    if (p >= n) return n;
    if (t[p] == '"') return skip_string(t, n, p);
    if (t[p] == '{' || t[p] == '[') {
        int depth = 0;
        while (p < n) {
            char c = t[p];
            if (c == '"') { p = skip_string(t, n, p); continue; }
            if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') { if (--depth == 0) return p + 1; }
            p++;
        }
        return n;
    }
    while (p < n && t[p] != ',' && t[p] != '}' && t[p] != ']' && t[p] != ' ' && t[p] != '\t' && t[p] != '\n' && t[p] != '\r') p++;
    return p;
}

/* Position of the value of member key in the object whose '{' is at obj, or (size_t)-1. A key written
 * with escapes isn't matched (the caller then leaves the file alone). */
static size_t find_member(const char *t, size_t n, size_t obj, const char *key) {
    size_t kl = strlen(key), p = skip_ws(t, n, obj + 1);
    while (p < n && t[p] == '"') {
        size_t ks = p + 1, ke = skip_string(t, n, p);
        int match = ke - 1 - ks == kl && !memcmp(t + ks, key, kl);
        p = skip_ws(t, n, ke);
        if (p >= n || t[p] != ':') return (size_t)-1;
        p = skip_ws(t, n, p + 1);
        if (match) return p;
        p = skip_ws(t, n, skip_value(t, n, p));
        if (p < n && t[p] == ',') p = skip_ws(t, n, p + 1);
        else break;
    }
    return (size_t)-1;
}

/* Adds WANTS[w] to text if it's absent. 1 inserted (new *text), 0 nothing to do, -1 leave the file alone. */
static int add_want(char **text, size_t *len, int w) {
    JDoc d;
    if (json_parse(*text, *len, &d, NULL, 0)) return -1;
    const JVal *node = d.root;
    if (!node || node->type != J_OBJ) { json_free(&d); return -1; }
    int have = 0;
    while (have < WANTS[w].n) {
        const JVal *c = json_get(node, WANTS[w].path[have]);
        if (!c) break;
        if (have == WANTS[w].n - 1 || c->type != J_OBJ) { json_free(&d); return 0; }   /* set, or the vendor's own shape */
        node = c;
        have++;
    }
    json_free(&d);
    size_t obj = skip_ws(*text, *len, 0);
    for (int k = 0; k < have; k++) {
        obj = find_member(*text, *len, obj, WANTS[w].path[k]);
        if (obj == (size_t)-1 || (*text)[obj] != '{') return -1;
    }
    char ins[256];
    size_t il = 0;
    for (int k = have; k < WANTS[w].n; k++)
        il += (size_t)snprintf(ins + il, sizeof ins - il, k + 1 < WANTS[w].n ? "\"%s\": {" : "\"%s\": ", WANTS[w].path[k]);
    il += (size_t)snprintf(ins + il, sizeof ins - il, "%s", WANTS[w].value);
    for (int k = have; k + 1 < WANTS[w].n; k++) ins[il++] = '}';
    size_t after = skip_ws(*text, *len, obj + 1);
    int empty = after < *len && (*text)[after] == '}';
    if (!empty) ins[il++] = ',';
    char *out = malloc(*len + il + 1);
    if (!out) return -1;
    memcpy(out, *text, obj + 1);
    memcpy(out + obj + 1, ins, il);
    memcpy(out + obj + 1 + il, *text + obj + 1, *len - obj - 1);
    out[*len + il] = 0;
    free(*text);
    *text = out;
    *len += il;
    return 1;
}

int browser_prefs_merge(const char *text, size_t len, char **out, size_t *outlen) {
    *out = NULL;
    *outlen = 0;
    if (!text) {
        *out = strdup(FRESH);
        if (!*out) return -1;
        *outlen = sizeof FRESH - 1;
        return 1;
    }
    if (len > PREFS_MAX) return -1;
    char *cur = malloc(len + 1);
    if (!cur) return -1;
    memcpy(cur, text, len);
    cur[len] = 0;
    size_t cl = len;
    int changed = 0;
    for (size_t w = 0; w < sizeof WANTS / sizeof WANTS[0]; w++) {
        int r = add_want(&cur, &cl, (int)w);
        if (r < 0) { free(cur); return -1; }
        changed |= r;
    }
    JDoc d;                                             /* what we write must still be JSON */
    if (changed && json_parse(cur, cl, &d, NULL, 0)) { free(cur); return -1; }
    if (changed) json_free(&d);
    if (!changed) { free(cur); return 0; }
    *out = cur;
    *outlen = cl;
    return 1;
}

/* ---- files ---- */

/* The file's text (NUL-terminated), or NULL with *state: 0 absent, -1 not a plain readable file. */
static char *read_prefs(int dfd, const char *name, size_t *len, int *state) {
    struct stat st;
    *state = 0;
    if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW)) { if (errno != ENOENT) *state = -1; return NULL; }
    *state = -1;
    if (!S_ISREG(st.st_mode) || st.st_size > (off_t)PREFS_MAX) return NULL;
    int fd = openat(dfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    char *b = malloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (b && got < (size_t)st.st_size) {
        ssize_t r = read(fd, b + got, (size_t)st.st_size - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!b || got != (size_t)st.st_size) { free(b); return NULL; }
    b[got] = 0;
    *len = got;
    *state = 1;
    return b;
}

static int write_prefs(int dfd, const char *name, const char *text, size_t len) {
    char tmp[64];
    snprintf(tmp, sizeof tmp, ".%s.lordos-tmp", name);
    unlinkat(dfd, tmp, 0);
    int fd = openat(dfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, text + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        off += (size_t)w;
    }
    int ok = off == len && !fchmod(fd, 0644) && !fsync(fd);
    if (close(fd)) ok = 0;
    if (!ok || renameat(dfd, tmp, dfd, name)) { unlinkat(dfd, tmp, 0); return -1; }
    return 0;
}

int browser_prefs_apply_dir(const char *root, const char *dir) {
    char path[PATH_MAX];
    if (!root || !strcmp(root, "/")) root = "";
    if ((size_t)snprintf(path, sizeof path, "%s%s", root, dir) >= sizeof path) return -1;
    int dfd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) return errno == ENOENT ? 0 : -1;        /* not installed */
    /* the file the browser reads: initial_preferences, else master_preferences, else a new initial_preferences */
    const char *name = "initial_preferences";
    size_t len = 0;
    int state;
    char *text = read_prefs(dfd, name, &len, &state);
    if (state == 0) {
        text = read_prefs(dfd, "master_preferences", &len, &state);
        if (state != 0) name = "master_preferences";
        else name = "initial_preferences";
    }
    if (state < 0) { close(dfd); return -1; }
    char *out;
    size_t ol;
    int r = browser_prefs_merge(text, len, &out, &ol);
    free(text);
    if (r == 1) {
        r = write_prefs(dfd, name, out, ol) ? -1 : 1;
        free(out);
    }
    close(dfd);
    return r;
}

int browser_prefs_apply_pkg(const char *root, const char *pkg) {
    const char *dir = browser_prefs_dir(pkg);
    return dir ? browser_prefs_apply_dir(root, dir) : 0;
}

int browser_prefs_apply_all(const char *root) {
    int n = 0, bad = 0;
    for (size_t i = 0; i < NBROWSERS; i++) {
        int r = browser_prefs_apply_dir(root, BROWSERS[i].dir);
        if (r > 0) { n++; printf("LordOS: initial preferences for %s written in %s\n", BROWSERS[i].pkg, BROWSERS[i].dir); }
        else if (r < 0) { bad = 1; printf("LordOS: initial preferences for %s in %s left as they are\n", BROWSERS[i].pkg, BROWSERS[i].dir); }
    }
    return bad ? -1 : n;
}
