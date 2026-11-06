/* brframe.c — the browsers' own title bar preference in the user's profiles (see brframe.h). */
#include "brframe.h"
#include "jbuf.h"
#include "../sec/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PREFS_MAX   (32u * 1024 * 1024)     /* Chromium's Preferences */
#define USERJS_MAX  (1024u * 1024)          /* Firefox's user.js */
#define FFPREFS_MAX (16u * 1024 * 1024)     /* Firefox's prefs.js */
#define INI_MAX     (64u * 1024)            /* profiles.ini */
#define MAX_PROFILES 64

#define FF_PREF "browser.tabs.inTitlebar"
#define FF_MARK "// lordos: Settings > Appearance"     /* i18n-ok: a marker in a config file */

/* ---- JSON: just enough scanning to find a member's value span (the text was validated with
 * json_parse first, and the result is validated again) ---- */

static size_t sk_ws(const char *s, size_t n, size_t i) {
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    return i;
}

/* s[i] == '"': the index after the closing quote, 0 if unterminated */
static size_t sk_str(const char *s, size_t n, size_t i) {
    for (i++; i < n; i++) {
        if (s[i] == '\\') { i++; continue; }
        if (s[i] == '"') return i + 1;
    }
    return 0;
}

/* the index after the value at s[i], 0 if it isn't one */
static size_t sk_val(const char *s, size_t n, size_t i, int depth) {
    if (i >= n || depth > 64) return 0;
    if (s[i] == '"') return sk_str(s, n, i);
    if (s[i] == '{' || s[i] == '[') {
        int obj = s[i] == '{';
        char close = obj ? '}' : ']';
        i = sk_ws(s, n, i + 1);
        if (i < n && s[i] == close) return i + 1;
        for (;;) {
            if (obj) {
                if (i >= n || s[i] != '"' || !(i = sk_str(s, n, i))) return 0;
                i = sk_ws(s, n, i);
                if (i >= n || s[i] != ':') return 0;
                i = sk_ws(s, n, i + 1);
            }
            if (!(i = sk_val(s, n, i, depth + 1))) return 0;
            i = sk_ws(s, n, i);
            if (i < n && s[i] == ',') { i = sk_ws(s, n, i + 1); continue; }
            if (i < n && s[i] == close) return i + 1;
            return 0;
        }
    }
    size_t st = i;
    while (i < n && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '+' ||
                     s[i] == '.' || s[i] == 'E')) i++;
    return i > st ? i : 0;
}

/* The member `key` (compared byte for byte) of the object at s[o] == '{'. 1 found (value in
 * [*vs, *ve)), 0 missing, -1 malformed. *empty: the object has no members. */
static int find_member(const char *s, size_t n, size_t o, const char *key, size_t *vs, size_t *ve, int *empty) {
    size_t kl = strlen(key);
    size_t i = sk_ws(s, n, o + 1);
    *empty = i < n && s[i] == '}';
    if (*empty) return 0;
    for (;;) {
        if (i >= n || s[i] != '"') return -1;
        size_t ke = sk_str(s, n, i);
        if (!ke) return -1;
        int match = ke - i - 2 == kl && !memcmp(s + i + 1, key, kl);
        i = sk_ws(s, n, ke);
        if (i >= n || s[i] != ':') return -1;
        i = sk_ws(s, n, i + 1);
        size_t e = sk_val(s, n, i, 1);
        if (!e) return -1;
        if (match) { *vs = i; *ve = e; return 1; }
        i = sk_ws(s, n, e);
        if (i < n && s[i] == ',') { i = sk_ws(s, n, i + 1); continue; }
        if (i < n && s[i] == '}') return 0;
        return -1;
    }
}

/* *b[from, to) replaced with text. */
static int text_splice(char **b, size_t *bn, size_t from, size_t to, const char *text) {
    size_t tl = strlen(text), nn = *bn - (to - from) + tl;
    char *p = malloc(nn + 1);
    if (!p) return BRF_BAD;
    memcpy(p, *b, from);
    memcpy(p + from, text, tl);
    memcpy(p + from + tl, *b + to, *bn - to);
    p[nn] = 0;
    free(*b);
    *b = p;
    *bn = nn;
    return BRF_CHANGED;
}

/* keys[0].keys[1]… = lit (a JSON literal): replaced if present, otherwise inserted as the first
 * member of the deepest object that exists. */
static int set_path(char **b, size_t *bn, const char *const *keys, int nk, const char *lit) {
    const char *s = *b;
    size_t n = *bn, o = sk_ws(s, n, 0);
    if (o >= n || s[o] != '{') return BRF_BAD;
    for (int k = 0; k < nk; k++) {
        size_t vs = 0, ve = 0;
        int empty, f = find_member(s, n, o, keys[k], &vs, &ve, &empty);
        if (f < 0) return BRF_BAD;
        if (f == 1) {
            if (k == nk - 1) {
                size_t ll = strlen(lit);
                if (ve - vs == ll && !memcmp(s + vs, lit, ll)) return BRF_SAME;
                return text_splice(b, bn, vs, ve, lit);
            }
            if (s[vs] != '{') return BRF_BAD;      /* not an object: don't guess */
            o = vs;
            continue;
        }
        HBuf h = {0};
        for (int j = k; j < nk; j++) hb_printf(&h, "\"%s\":%s", keys[j], j == nk - 1 ? lit : "{");
        for (int j = k; j < nk - 1; j++) hb_printf(&h, "}");
        if (!empty) hb_printf(&h, ",");
        char *ins = hb_take(&h);
        if (!ins) return BRF_BAD;
        int r = text_splice(b, bn, o + 1, o + 1, ins);
        free(ins);
        return r;
    }
    return BRF_BAD;
}

int brf_chrome_edit(const char *in, size_t n, int on, char **out, size_t *outn) {
    *out = NULL;
    *outn = 0;
    on = !!on;
    if (!in || !n || n > PREFS_MAX) return BRF_BAD;
    JDoc d;
    if (json_parse(in, n, &d, NULL, 0)) return BRF_BAD;
    int ok = d.root && d.root->type == J_OBJ;
    const JVal *ext = ok ? json_get(d.root, "extensions") : NULL, *th = ext ? json_get(ext, "theme") : NULL;
    const char *tid = th ? json_str(th, "id") : NULL;
    int theme_ext = tid && tid[0];             /* a theme extension the user installed: kept */
    json_free(&d);
    if (!ok) return BRF_BAD;

    char *b = malloc(n + 1);
    if (!b) return BRF_BAD;
    memcpy(b, in, n);
    b[n] = 0;
    size_t bn = n;
    static const char *const frame[] = { "browser", "custom_chrome_frame" };
    static const char *const theme[] = { "extensions", "theme", "system_theme" };
    int r = set_path(&b, &bn, frame, 2, on ? "true" : "false"), changed = r == BRF_CHANGED;
    if (r != BRF_BAD && on && !theme_ext) {
        r = set_path(&b, &bn, theme, 3, "1");
        changed |= r == BRF_CHANGED;
    }
    if (r == BRF_BAD || !changed) { free(b); return r == BRF_BAD ? BRF_BAD : BRF_SAME; }

    /* the result must parse and say what was asked */
    if (json_parse(b, bn, &d, NULL, 0)) { free(b); return BRF_BAD; }
    const JVal *br = json_get(d.root, "browser"), *ccf = br ? json_get(br, "custom_chrome_frame") : NULL;
    ok = ccf && ccf->type == J_BOOL && (int)ccf->num == on;
    json_free(&d);
    if (!ok) { free(b); return BRF_BAD; }
    *out = b;
    *outn = bn;
    return BRF_CHANGED;
}

/* ---- Firefox's user.js / prefs.js lines ---- */

/* Is the line l[0..n) a user_pref of FF_PREF? *val its value (0/1; true/false too), *ours if marked. */
static int ff_line(const char *l, size_t n, int *val, int *ours) {
    size_t i = 0, kl = strlen(FF_PREF);
    while (i < n && (l[i] == ' ' || l[i] == '\t')) i++;
    static const char up[] = "user_pref(";
    if (n - i < sizeof up - 1 || memcmp(l + i, up, sizeof up - 1)) return 0;
    i += sizeof up - 1;
    while (i < n && (l[i] == ' ' || l[i] == '\t')) i++;
    if (i >= n || (l[i] != '"' && l[i] != '\'')) return 0;
    char q = l[i++];
    if (n - i < kl + 1 || memcmp(l + i, FF_PREF, kl) || l[i + kl] != q) return 0;
    i += kl + 1;
    while (i < n && (l[i] == ' ' || l[i] == '\t')) i++;
    if (i >= n || l[i] != ',') return 0;
    i++;
    while (i < n && (l[i] == ' ' || l[i] == '\t')) i++;
    *val = -1;
    if (i < n && l[i] >= '0' && l[i] <= '9') *val = l[i] != '0';
    else if (n - i >= 4 && !memcmp(l + i, "true", 4)) *val = 1;
    else if (n - i >= 5 && !memcmp(l + i, "false", 5)) *val = 0;
    *ours = memmem(l, n, FF_MARK, strlen(FF_MARK)) != NULL;
    return 1;
}

int brf_prefs_value(const char *in, size_t n) {
    int v = -1;
    for (size_t i = 0; i < n;) {
        const char *e = memchr(in + i, '\n', n - i);
        size_t len = e ? (size_t)(e - (in + i)) : n - i;
        int val, ours;
        if (ff_line(in + i, len, &val, &ours) && val >= 0) v = val;
        i += len + 1;
    }
    return v;
}

int brf_userjs_ours(const char *in, size_t n) {
    int v = -1;
    for (size_t i = 0; i < n;) {
        const char *e = memchr(in + i, '\n', n - i);
        size_t len = e ? (size_t)(e - (in + i)) : n - i;
        int val, ours;
        if (ff_line(in + i, len, &val, &ours) && ours) v = val;
        i += len + 1;
    }
    return v;
}

int brf_userjs_edit(const char *in, size_t n, int value, char **out, size_t *outn, int *own) {
    *out = NULL;
    *outn = 0;
    *own = 0;
    if (n > USERJS_MAX || (n && !in) || memchr(in ? in : "", 0, n)) return BRF_BAD;
    HBuf h = {0};
    for (size_t i = 0; i < n;) {
        const char *e = memchr(in + i, '\n', n - i);
        size_t len = e ? (size_t)(e - (in + i)) : n - i;
        int val, ours;
        if (ff_line(in + i, len, &val, &ours)) {
            if (!ours) { *own = 1; free(h.p); return BRF_SAME; }    /* the user's own line: theirs */
        } else {
            hb_printf(&h, "%.*s%s", (int)len, in + i, e ? "\n" : "");
        }
        i += len + 1;
    }
    if (value >= 0) {
        if (h.n && h.p[h.n - 1] != '\n') hb_printf(&h, "\n");
        hb_printf(&h, "user_pref(\"%s\", %d); %s, removed once Firefox has read it\n", FF_PREF, !!value, FF_MARK);
    }
    if (h.oom) { free(h.p); return BRF_BAD; }
    size_t hn = h.n;
    char *res = hb_take(&h);
    if (!res && hn) return BRF_BAD;
    if (hn == n && (!n || !memcmp(res, in, n))) { free(res); return BRF_SAME; }
    if (!res) { res = strdup(""); if (!res) return BRF_BAD; }
    *out = res;
    *outn = hn;
    return BRF_CHANGED;
}

/* ---- files ---- */

/* name in dfd: a regular file owned by the user, at most max bytes, opened without following
 * links. NULL (errno ENOENT if it doesn't exist). */
static char *read_at(int dfd, const char *name, size_t max, size_t *len, struct stat *st) {
    *len = 0;
    int fd = openat(dfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat s;
    if (fstat(fd, &s) || !S_ISREG(s.st_mode) || s.st_uid != getuid() || (size_t)s.st_size > max) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }
    char *b = malloc((size_t)s.st_size + 1);
    size_t got = 0;
    while (b && got < (size_t)s.st_size) {
        ssize_t r = read(fd, b + got, (size_t)s.st_size - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!b || got != (size_t)s.st_size) { free(b); errno = EIO; return NULL; }
    b[got] = 0;
    *len = got;
    if (st) *st = s;
    return b;
}

/* Atomic replace of name in dfd: a new file next to it (O_EXCL, never through a link), synced,
 * then renamed over it. */
static int write_at(int dfd, const char *name, const char *data, size_t len, mode_t mode) {
    char tmp[256];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.lordos-tmp", name) >= sizeof tmp) return -1;
    unlinkat(dfd, tmp, 0);
    int fd = openat(dfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int ok = fchmod(fd, mode & 0777) == 0;
    for (size_t put = 0; ok && put < len;) {
        ssize_t w = write(fd, data + put, len - put);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) ok = 0;
        else put += (size_t)w;
    }
    ok = ok && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok || renameat(dfd, tmp, dfd, name)) { unlinkat(dfd, tmp, 0); return -1; }
    fsync(dfd);
    return 0;
}

/* base/sub as a folder owned by the user (base itself may be a link the user made). */
static int open_root(const char *base, const char *sub) {
    char p[4096];
    if (!base || base[0] != '/' || (size_t)snprintf(p, sizeof p, "%s/%s", base, sub) >= sizeof p) return -1;
    int fd = open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || st.st_uid != getuid())) { close(fd); fd = -1; }
    return fd;
}

/* Is pid one of the user's processes whose name starts with one of names? */
static int pid_is(long pid, const char *const *names) {
    char p[64], comm[64];
    struct stat st;
    if (pid <= 1) return 0;
    snprintf(p, sizeof p, "/proc/%ld", pid);
    if (stat(p, &st) || st.st_uid != getuid()) return 0;
    snprintf(p, sizeof p, "/proc/%ld/comm", pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t k = read(fd, comm, sizeof comm - 1);
    close(fd);
    if (k <= 0) return 0;
    comm[k] = 0;
    comm[strcspn(comm, "\n")] = 0;
    for (; *names; names++) if (!strncmp(comm, *names, strlen(*names))) return 1;
    return 0;
}

static const char *const CHROMIUM_NAMES[] = { "chromium", "chrome", NULL };
static const char *const BRAVE_NAMES[] = { "brave", NULL };
static const char *const FIREFOX_NAMES[] = { "firefox", "GeckoMain", NULL };

/* The Chromium-based browsers whose profiles the switch writes (0.8.8.4, #39: Brave too): the data
 * folder under ~/.config and the names of the process that holds its SingletonLock. They share
 * Chromium's Preferences format and its two keys. */
typedef struct { const char *dir; const char *const *procs; } ChromeKind;
static const ChromeKind CHROME_KINDS[] = {
    { "chromium", CHROMIUM_NAMES },
    { "BraveSoftware/Brave-Browser", BRAVE_NAMES },
};
#define N_CHROME_KINDS (int)(sizeof CHROME_KINDS / sizeof CHROME_KINDS[0])

/* Chromium's SingletonLock is a link to "<host>-<pid>". Anything unexpected counts as running. */
static int chromium_locked(int dfd, const char *const *procs) {
    char t[300], host[256];
    ssize_t k = readlinkat(dfd, "SingletonLock", t, sizeof t - 1);
    if (k < 0) return errno == ENOENT ? 0 : 1;
    t[k] = 0;
    char *dash = strrchr(t, '-'), *e;
    if (!dash || gethostname(host, sizeof host)) return 1;
    host[sizeof host - 1] = 0;
    if ((size_t)(dash - t) != strlen(host) || memcmp(t, host, (size_t)(dash - t))) return 1;   /* another computer */
    long pid = strtol(dash + 1, &e, 10);
    if (*e || pid <= 0) return 1;
    return pid_is(pid, procs);                   /* a dead or unrelated pid: a stale lock */
}

int brf_chromium_running(const char *cfg) {
    int r = 0;
    for (int k = 0; k < N_CHROME_KINDS && !r; k++) {
        int dfd = open_root(cfg, CHROME_KINDS[k].dir);
        if (dfd < 0) continue;
        r = chromium_locked(dfd, CHROME_KINDS[k].procs);
        close(dfd);
    }
    return r;
}

/* A Firefox profile is open: a lock on .parentlock, or the "lock" link "<ip>:+<pid>" of a live Firefox. */
static int ff_locked(int pfd) {
    int fd = openat(pfd, ".parentlock", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd >= 0) {
        struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
        int r = fcntl(fd, F_GETLK, &fl);
        close(fd);
        if (!r && fl.l_type != F_UNLCK) return 1;
    }
    char t[128], *e;
    ssize_t k = readlinkat(pfd, "lock", t, sizeof t - 1);
    if (k > 0) {
        t[k] = 0;
        char *plus = strrchr(t, '+');
        if (plus) {
            long pid = strtol(plus + 1, &e, 10);
            if (!*e && pid_is(pid, FIREFOX_NAMES)) return 1;
        }
    }
    return 0;
}

static int chromium_profile_name(const char *n) {
    if (!strcmp(n, "Default")) return 1;
    if (strncmp(n, "Profile ", 8) || !n[8] || strlen(n) > 12) return 0;
    for (const char *p = n + 8; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* The profile folder names under Chromium's folder (Default, Profile N), sorted; malloc'd. */
static int chromium_profiles(int dfd, char **names) {
    int dup_fd = dup(dfd), nn = 0;
    DIR *d = dup_fd >= 0 ? fdopendir(dup_fd) : NULL;
    if (!d) { if (dup_fd >= 0) close(dup_fd); return -1; }
    for (struct dirent *de; nn < MAX_PROFILES && (de = readdir(d));)
        if (chromium_profile_name(de->d_name) && (names[nn] = strdup(de->d_name))) nn++;
    closedir(d);
    qsort(names, (size_t)nn, sizeof *names, cmp_str);
    return nn;
}

/* One browser's profiles (its folder dfd): counts added to r. Nothing is written while it runs. */
static void chrome_apply_one(int dfd, const char *const *procs, int on, BrfResult *r) {
    r->found = 1;
    if (chromium_locked(dfd, procs)) { r->running = 1; return; }
    char *names[MAX_PROFILES];
    int nn = chromium_profiles(dfd, names), running = 0;
    if (nn < 0) { r->failed++; return; }
    for (int i = 0; i < nn; i++) {
        int pfd = running ? -1 : openat(dfd, names[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        free(names[i]);
        if (pfd < 0) continue;
        struct stat st;
        size_t n;
        char *data = read_at(pfd, "Preferences", PREFS_MAX, &n, &st), *out = NULL;
        if (!data) {
            if (errno != ENOENT) { r->profiles++; r->failed++; }      /* no Preferences yet: not a profile in use */
            close(pfd);
            continue;
        }
        r->profiles++;
        size_t on_n;
        int e = brf_chrome_edit(data, n, on, &out, &on_n);
        if (e == BRF_BAD) r->failed++;
        else if (e == BRF_SAME) r->unchanged++;
        else if (chromium_locked(dfd, procs)) running = r->running = 1;   /* it started meanwhile: later */
        else if (write_at(pfd, "Preferences.lordos-backup", data, n, st.st_mode) ||
                 write_at(pfd, "Preferences", out, on_n, st.st_mode)) r->failed++;
        else r->changed++;
        free(out);
        free(data);
        close(pfd);
    }
}

void brf_chromium_apply(const char *cfg, int on, BrfResult *r) {
    memset(r, 0, sizeof *r);
    for (int k = 0; k < N_CHROME_KINDS; k++) {
        int dfd = open_root(cfg, CHROME_KINDS[k].dir);
        if (dfd < 0) continue;
        chrome_apply_one(dfd, CHROME_KINDS[k].procs, on, r);
        close(dfd);
    }
}

/* Profiles that switching on would change (*off) and that switching off would change (*on): a
 * profile with the frame or the theme unset or different counts, a malformed one doesn't. */
void brf_chromium_state(const char *cfg, int *on, int *off) {
    *on = *off = 0;
    for (int k = 0; k < N_CHROME_KINDS; k++) {
        int dfd = open_root(cfg, CHROME_KINDS[k].dir);
        if (dfd < 0) continue;
        char *names[MAX_PROFILES];
        int nn = chromium_profiles(dfd, names);
        for (int i = 0; i < nn; i++) {
            int pfd = openat(dfd, names[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            free(names[i]);
            if (pfd < 0) continue;
            size_t n;
            char *data = read_at(pfd, "Preferences", PREFS_MAX, &n, NULL), *out = NULL;
            close(pfd);
            if (!data) continue;
            size_t outn;
            if (brf_chrome_edit(data, n, 1, &out, &outn) == BRF_CHANGED) (*off)++;
            free(out);
            out = NULL;
            if (brf_chrome_edit(data, n, 0, &out, &outn) == BRF_CHANGED) (*on)++;
            free(out);
            free(data);
        }
        close(dfd);
    }
}

/* ---- Firefox profiles ---- */

typedef void (*FfFn)(int pfd, void *ctx);

static int ff_path_ok(const char *p) {
    size_t n = strlen(p);
    if (!n || n > 200 || !strcmp(p, ".") || !strcmp(p, "..")) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)p[i] < 0x20 || p[i] == '/' || p[i] == 0x7f) return 0;
    return 1;
}

/* Every relative profile listed in each root's profiles.ini, once. Returns how many roots exist. */
static int ff_each(const char *cfg, const char *home, FfFn fn, void *ctx) {
    char roots[2][64];
    const char *base[2] = { cfg, home };
    snprintf(roots[0], sizeof roots[0], "mozilla/firefox");
    snprintf(roots[1], sizeof roots[1], ".mozilla/firefox");
    dev_t devs[MAX_PROFILES];
    ino_t inos[MAX_PROFILES];
    int seen = 0, found = 0;
    for (int ri = 0; ri < 2; ri++) {
        int rfd = open_root(base[ri], roots[ri]);
        if (rfd < 0) continue;
        found++;
        size_t n;
        char *ini = read_at(rfd, "profiles.ini", INI_MAX, &n, NULL);
        char path[256] = "";
        int in_profile = 0, relative = 0;
        for (size_t i = 0; ini && i <= n;) {
            const char *e = i < n ? memchr(ini + i, '\n', n - i) : NULL;
            size_t len = i >= n ? 0 : e ? (size_t)(e - (ini + i)) : n - i;
            char line[300];
            int end = i >= n;                       /* one more pass to flush the last section */
            if (!end) {
                size_t l = len < sizeof line - 1 ? len : sizeof line - 1;
                memcpy(line, ini + i, l);
                line[l] = 0;
                line[strcspn(line, "\r")] = 0;
            }
            if (end || line[0] == '[') {
                if (in_profile && relative && ff_path_ok(path) && seen < MAX_PROFILES) {
                    int pfd = openat(rfd, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                    struct stat st;
                    if (pfd >= 0 && !fstat(pfd, &st) && st.st_uid == getuid()) {
                        int dup_seen = 0;
                        for (int k = 0; k < seen; k++) if (devs[k] == st.st_dev && inos[k] == st.st_ino) dup_seen = 1;
                        if (!dup_seen) { devs[seen] = st.st_dev; inos[seen] = st.st_ino; seen++; fn(pfd, ctx); }
                    }
                    if (pfd >= 0) close(pfd);
                }
                if (end) break;
                in_profile = !strncmp(line, "[Profile", 8);
                relative = 0;
                path[0] = 0;
            } else if (in_profile && !strncmp(line, "Path=", 5) && strlen(line + 5) < sizeof path) {
                snprintf(path, sizeof path, "%s", line + 5);
            } else if (in_profile && !strncmp(line, "IsRelative=", 11)) {
                relative = !strcmp(line + 11, "1");
            }
            i = e ? i + len + 1 : n;
        }
        free(ini);
        close(rfd);
    }
    return found;
}

typedef struct { BrfResult *r; int value; } FfApply;

static void ff_apply_one(int pfd, void *ctx) {
    FfApply *a = ctx;
    BrfResult *r = a->r;
    r->profiles++;
    int open_now = ff_locked(pfd);
    struct stat st;
    size_t n = 0, outn;
    char *u = read_at(pfd, "user.js", USERJS_MAX, &n, &st), *out = NULL;
    if (!u && errno != ENOENT) { r->failed++; return; }
    int existed = u != NULL, own;
    int e = brf_userjs_edit(u ? u : "", n, a->value, &out, &outn, &own);
    if (own) r->kept++;
    else if (e == BRF_BAD) r->failed++;
    else if (e == BRF_SAME) r->unchanged++;
    else {
        /* no line needed when prefs.js already holds the value and there was none of ours */
        size_t pn;
        char *p = brf_userjs_ours(u ? u : "", n) < 0 ? read_at(pfd, "prefs.js", FFPREFS_MAX, &pn, NULL) : NULL;
        if (p && brf_prefs_value(p, pn) == a->value) r->unchanged++;
        else if ((existed && n && write_at(pfd, "user.js.lordos-backup", u, n, st.st_mode)) ||
                 write_at(pfd, "user.js", out, outn, existed ? st.st_mode : 0600)) r->failed++;
        else { r->changed++; if (open_now) r->running++; }
        free(p);
    }
    free(out);
    free(u);
}

void brf_firefox_apply(const char *cfg, const char *home, int on, BrfResult *r) {
    memset(r, 0, sizeof *r);
    FfApply a = { r, !!on };
    r->found = ff_each(cfg, home, ff_apply_one, &a) > 0;
}

static int newer(const struct timespec *a, const struct timespec *b) {
    return a->tv_sec > b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

/* Has Firefox started this profile since t? Firefox makes the "lock" link and .parentlock when it
 * opens a profile (and leaves them when it quits), so their times are its last start. prefs.js is
 * no proof: Firefox also writes it when it quits, after a line written while it ran. */
static int ff_started_after(int pfd, const struct timespec *t) {
    struct stat st;
    if (!fstatat(pfd, "lock", &st, AT_SYMLINK_NOFOLLOW) && newer(&st.st_mtim, t)) return 1;
    return !fstatat(pfd, ".parentlock", &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) && newer(&st.st_mtim, t);
}

static void ff_tidy_one(int pfd, void *ctx) {
    int *removed = ctx;
    struct stat us;
    size_t n, outn;
    char *u = read_at(pfd, "user.js", USERJS_MAX, &n, &us), *out = NULL;
    if (!u) return;
    int own;
    if (brf_userjs_ours(u, n) >= 0 && !ff_locked(pfd) && ff_started_after(pfd, &us.st_mtim) &&
        brf_userjs_edit(u, n, -1, &out, &outn, &own) == BRF_CHANGED) {
        /* Firefox has started with the line since it was written: the value is its own now */
        if (outn ? !write_at(pfd, "user.js", out, outn, us.st_mode) : !unlinkat(pfd, "user.js", 0)) (*removed)++;
    }
    free(out);
    free(u);
}

int brf_firefox_tidy(const char *cfg, const char *home) {
    int removed = 0;
    ff_each(cfg, home, ff_tidy_one, &removed);
    return removed;
}

static void put_result(HBuf *h, const char *name, const BrfResult *r) {
    hb_printf(h, "\"%s\":{\"found\":%s,\"profiles\":%d,\"changed\":%d,\"unchanged\":%d,\"running\":%d,\"kept\":%d,\"failed\":%d}",
              name, r->found ? "true" : "false", r->profiles, r->changed, r->unchanged, r->running, r->kept, r->failed);
}

char *brf_result_json(const BrfResult *chromium, const BrfResult *firefox) {
    HBuf h = {0};
    hb_printf(&h, "{");
    put_result(&h, "chromium", chromium);
    hb_printf(&h, ",");
    put_result(&h, "firefox", firefox);
    hb_printf(&h, "}");
    return hb_take(&h);
}
