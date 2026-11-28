/* themestore.c — the per-user folder of community themes (themestore.h). */
#include "themestore.h"
#include "../sec/json.h"
#include "../pkg/crypto.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *const CAPS[] = { "wifi", "bluetooth", "sound", "battery", "brightness", "media", "notifications" };
#define NCAPS (sizeof CAPS / sizeof CAPS[0])

unsigned ts_cap_bit(const char *name) {
    for (unsigned i = 0; name && i < NCAPS; i++) if (!strcmp(name, CAPS[i])) return 1u << i;
    return 0;
}
const char *ts_cap_name(unsigned bit) {
    for (unsigned i = 0; i < NCAPS; i++) if (bit == 1u << i) return CAPS[i];
    return NULL;
}

int ts_valid_id(const char *id) {
    size_t n = id ? strlen(id) : 0;
    if (!n || n > TS_ID_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

static int seg_ok(const char *s, size_t n) {
    if (!n || n > 64) return 0;
    char c0 = s[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || (c0 >= '0' && c0 <= '9') || c0 == '_')) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    if (n >= 2 && s[0] == '.' && s[1] == '.') return 0;
    return 1;
}

static int ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a > b && !strcmp(s + a - b, suf);
}

int ts_valid_name(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (!n || n > TS_PATH_MAX || name[0] == '/' || strstr(name, "..") || strstr(name, "//")) return 0;
    int levels = 0;
    for (const char *p = name; *p;) {
        size_t l = strcspn(p, "/");
        if (!seg_ok(p, l)) return 0;
        levels++;
        p += l;
        if (*p == '/') { p++; if (!*p) return 0; }
    }
    if (levels > 6) return 0;
    if (!strcmp(name, "theme.json") || !strcmp(name, "theme.js") || !strcmp(name, "LICENSE") || !strcmp(name, "NOTICE") ||
        !strcmp(name, "THIRD_PARTY.md") || !strcmp(name, "PROVENANCE.md") || !strcmp(name, "README.md")) return 1;
    if (!strncmp(name, "assets/", 7)) {
        char low[TS_PATH_MAX + 1];
        for (size_t i = 0; i <= n; i++) low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
        return ends_with(low, ".png") || ends_with(low, ".jpg") || ends_with(low, ".jpeg");
    }
    if (!strncmp(name, "locales/", 8)) {
        const char *l = name + 8;
        if (strchr(l, '/') || !ends_with(l, ".json")) return 0;
        size_t ll = strlen(l) - 5;
        if (ll < 2 || ll > 8) return 0;
        for (size_t i = 0; i < ll; i++) {
            char c = l[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-')) return 0;
        }
        return 1;
    }
    return 0;
}

/* ---- the record ---- */

static int plain_text(const char *s, size_t max) {
    size_t n = strlen(s);
    if (n > max) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) return 0;
        /* bidi overrides and isolates (U+202A–202E, U+2066–2069) as UTF-8 */
        if (c == 0xe2 && i + 2 < n) {
            unsigned char b = (unsigned char)s[i + 1], d = (unsigned char)s[i + 2];
            if (b == 0x80 && d >= 0xaa && d <= 0xae) return 0;
            if (b == 0x81 && d >= 0xa6 && d <= 0xa9) return 0;
        }
    }
    return 1;
}

int ts_record_parse(const char *text, size_t len, TsRecord *r) {
    memset(r, 0, sizeof *r);
    JDoc doc;
    if (json_parse(text, len, &doc, NULL, 0)) return -1;
    int ok = doc.root && doc.root->type == J_OBJ && json_num(doc.root, "v", 0) == 1;
    const char *id = ok ? json_str(doc.root, "id") : NULL, *sha = ok ? json_str(doc.root, "sha256") : NULL;
    const char *author = ok ? json_str(doc.root, "author") : NULL, *ver = ok ? json_str(doc.root, "version") : NULL;
    const char *failed = ok ? json_str(doc.root, "failed") : NULL;
    ok = ok && ts_valid_id(id) && sha && strlen(sha) == 64 && strspn(sha, "0123456789abcdef") == 64;
    if (ok) {
        snprintf(r->id, sizeof r->id, "%s", id);
        memcpy(r->sha256, sha, 65);
        if (author && plain_text(author, sizeof r->author - 1)) snprintf(r->author, sizeof r->author, "%s", author);
        if (ver && plain_text(ver, sizeof r->version - 1)) snprintf(r->version, sizeof r->version, "%s", ver);
        if (failed && plain_text(failed, sizeof r->failed - 1)) snprintf(r->failed, sizeof r->failed, "%s", failed);
        r->trusted = json_bool(doc.root, "trusted", 0) == 1;
        double cr = json_num(doc.root, "crashes", 0);
        r->crashes = cr >= 0 && cr < 1000 ? (int)cr : 0;
        double t = json_num(doc.root, "installed", 0);
        r->installed = t > 0 && t < 1e11 ? (long long)t : 0;
        const JVal *caps = json_get(doc.root, "caps");
        if (caps && caps->type == J_ARR)
            for (const JVal *c = caps->kid; c; c = c->next) if (c->type == J_STR) r->caps |= ts_cap_bit(c->str);
    }
    json_free(&doc);
    return ok ? 0 : -1;
}

size_t ts_record_format(const TsRecord *r, char *out, size_t n) {
    char a[sizeof r->author * 6 + 3], v[sizeof r->version * 6 + 3], f[sizeof r->failed * 6 + 3], caps[160] = "";
    json_quote(a, sizeof a, r->author);
    json_quote(v, sizeof v, r->version);
    json_quote(f, sizeof f, r->failed);
    size_t cl = 0;
    for (unsigned i = 0; i < NCAPS; i++)
        if (r->caps & (1u << i)) cl += (size_t)snprintf(caps + cl, sizeof caps - cl, "%s\"%s\"", cl ? "," : "", CAPS[i]);
    int w = snprintf(out, n, "{\"v\":1,\"id\":\"%s\",\"sha256\":\"%s\",\"author\":%s,\"version\":%s,\"caps\":[%s],"
                     "\"trusted\":%s,\"crashes\":%d,\"failed\":%s,\"installed\":%lld}\n",
                     r->id, r->sha256, a, v, caps, r->trusted ? "true" : "false", r->crashes, f, r->installed);
    return w > 0 && (size_t)w < n ? (size_t)w : 0;
}

/* ---- the helper's stream ---- */

static int read_line(const uint8_t *buf, size_t len, size_t *pos, char *line, size_t ln) {
    size_t s = *pos, i = s;
    while (i < len && buf[i] != '\n' && i - s < ln - 1) i++;
    if (i >= len || buf[i] != '\n') return -1;
    memcpy(line, buf + s, i - s);
    line[i - s] = 0;
    *pos = i + 1;
    return 0;
}

static int parse_size(const char *s, size_t max, size_t *out) {
    if (!*s || strlen(s) > 9 || strspn(s, "0123456789") != strlen(s) || (s[0] == '0' && s[1])) return -1;
    unsigned long long v = strtoull(s, NULL, 10);
    if (v > max) return -1;
    *out = (size_t)v;
    return 0;
}

#define WHY(...) do { if (why && why_n) snprintf(why, why_n, __VA_ARGS__); } while (0)

int ts_stream_parse(const uint8_t *buf, size_t len, char *report, size_t report_n, TsFileFn file, void *ud,
                    char *why, size_t why_n) {
    size_t pos = 0, n = 0, total = 0;
    char line[TS_PATH_MAX + 32];
    if (report && report_n) report[0] = 0;
    if (len > TS_TOTAL_MAX + 65536) { WHY("answer too large"); return -1; }
    if (read_line(buf, len, &pos, line, sizeof line) || strcmp(line, "LORDTHEME 1")) { WHY("not a theme stream"); return -1; }
    if (read_line(buf, len, &pos, line, sizeof line) || strncmp(line, "R ", 2) || parse_size(line + 2, 65536, &n) ||
        len - pos < n + 1 || buf[pos + n] != '\n') { WHY("bad report"); return -1; }
    if (memchr(buf + pos, 0, n)) { WHY("bad report"); return -1; }
    if (report && report_n) {
        size_t k = n < report_n - 1 ? n : report_n - 1;
        memcpy(report, buf + pos, k);
        report[k] = 0;
    }
    pos += n + 1;
    int files = 0, have_code = 0, have_meta = 0;
    for (;;) {
        if (read_line(buf, len, &pos, line, sizeof line)) { WHY("truncated"); return -1; }
        if (!strcmp(line, "END")) break;
        if (strncmp(line, "F ", 2)) { WHY("bad entry"); return -1; }
        char *name = line + 2, *sp = strrchr(name, ' ');
        if (!sp) { WHY("bad entry"); return -1; }
        *sp = 0;
        size_t fl;
        int code = !strcmp(name, "theme.js");
        if (!ts_valid_name(name) || parse_size(sp + 1, code ? TS_CODE_MAX : TS_FILE_MAX, &fl) || len - pos < fl) {
            WHY("bad file entry"); return -1;
        }
        total += fl;
        if (++files > TS_FILES_MAX || total > TS_TOTAL_MAX) { WHY("too many files or bytes"); return -1; }
        have_code |= code;
        have_meta |= !strcmp(name, "theme.json");
        if (file && file(ud, name, buf + pos, fl)) { WHY("file %s not written", name); return -1; }
        pos += fl;
    }
    if (pos != len) { WHY("trailing bytes"); return -1; }
    if (files && (!have_code || !have_meta)) { WHY("theme.js or theme.json missing"); return -1; }
    return 0;
}

/* ---- writing ---- */

typedef struct {
    int dir;                            /* the staging folder */
    Sha256 code;
    int seen_code;
    char names[TS_FILES_MAX][TS_PATH_MAX + 1];
    int count;
} Stage;

static int mkdirs_at(int dir, const char *name) {
    char p[TS_PATH_MAX + 1];
    snprintf(p, sizeof p, "%s", name);
    for (char *s = strchr(p, '/'); s; s = strchr(s + 1, '/')) {
        *s = 0;
        if (mkdirat(dir, p, 0700) && errno != EEXIST) return -1;
        struct stat st;
        if (fstatat(dir, p, &st, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(st.st_mode)) return -1;
        *s = '/';
    }
    return 0;
}

static int write_all(int fd, const uint8_t *b, size_t n) {
    for (size_t o = 0; o < n;) {
        ssize_t w = write(fd, b + o, n - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        o += (size_t)w;
    }
    return 0;
}

static int stage_file(void *ud, const char *name, const uint8_t *data, size_t len) {
    Stage *s = ud;
    if (!strcmp(name, "install.json")) return -1;
    for (int i = 0; i < s->count; i++) if (!strcmp(s->names[i], name)) return -1;     /* twice */
    if (s->count >= TS_FILES_MAX) return -1;
    snprintf(s->names[s->count++], TS_PATH_MAX + 1, "%s", name);
    if (mkdirs_at(s->dir, name)) return -1;
    int fd = openat(s->dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int r = write_all(fd, data, len);
    if (!r && fsync(fd)) r = -1;
    close(fd);
    if (!strcmp(name, "theme.js")) { sha256_update(&s->code, data, len); s->seen_code = 1; }
    return r;
}

static int rm_tree_at(int dir, const char *name, int depth) {
    if (depth > 8) return -1;
    struct stat st;
    if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlinkat(dir, name, 0);
    int fd = openat(dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    DIR *d = fdopendir(fd);
    if (!d) { close(fd); return -1; }
    struct dirent *e;
    int r = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (rm_tree_at(dirfd(d), e->d_name, depth + 1)) r = -1;
    }
    closedir(d);
    if (unlinkat(dir, name, AT_REMOVEDIR)) r = -1;
    return r;
}

int ts_remove(int root_fd, const char *id) {
    if (!ts_valid_id(id)) return -1;
    return rm_tree_at(root_fd, id, 0);
}

static void hex(const uint8_t *in, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = d[in[i] >> 4]; out[2 * i + 1] = d[in[i] & 15]; }
    out[2 * n] = 0;
}

static int write_record_at(int dir, const TsRecord *r) {
    char text[1024];
    size_t n = ts_record_format(r, text, sizeof text);
    if (!n) return -1;
    unlinkat(dir, "install.json.tmp", 0);
    int fd = openat(dir, "install.json.tmp", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int w = write_all(fd, (const uint8_t *)text, n) || fsync(fd);
    close(fd);
    if (w || renameat(dir, "install.json.tmp", dir, "install.json")) { unlinkat(dir, "install.json.tmp", 0); return -1; }
    return 0;
}

int ts_install(int root_fd, const uint8_t *stream, size_t len, TsRecord *rec, char *why, size_t why_n) {
    if (!ts_valid_id(rec->id)) { WHY("bad id"); return -1; }
    uint8_t rnd[8];
    char stage_name[40], old_name[40], h[17];
    if (getrandom(rnd, sizeof rnd, 0) != (ssize_t)sizeof rnd) { WHY("no randomness"); return -1; }
    hex(rnd, sizeof rnd, h);
    snprintf(stage_name, sizeof stage_name, ".staging-%s", h);
    snprintf(old_name, sizeof old_name, ".old-%s", h);
    if (mkdirat(root_fd, stage_name, 0700)) { WHY("staging folder: %s", strerror(errno)); return -1; }
    Stage *s = calloc(1, sizeof *s);
    if (!s) { unlinkat(root_fd, stage_name, AT_REMOVEDIR); WHY("out of memory"); return -1; }
    s->dir = openat(root_fd, stage_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    sha256_init(&s->code);
    int r = s->dir < 0 ? -1 : ts_stream_parse(stream, len, NULL, 0, stage_file, s, why, why_n);
    if (!r && !s->seen_code) { WHY("no theme.js"); r = -1; }
    if (!r) {
        uint8_t d[32];
        sha256_final(&s->code, d);
        hex(d, 32, rec->sha256);
        rec->installed = (long long)time(NULL);
        rec->crashes = 0;
        rec->failed[0] = 0;
        if (write_record_at(s->dir, rec)) { WHY("install.json not written"); r = -1; }
    }
    if (s->dir >= 0) { fsync(s->dir); close(s->dir); }
    free(s);
    if (r) { rm_tree_at(root_fd, stage_name, 0); return -1; }
    struct stat st;
    int had = !fstatat(root_fd, rec->id, &st, AT_SYMLINK_NOFOLLOW);
    if (had && renameat(root_fd, rec->id, root_fd, old_name)) { rm_tree_at(root_fd, stage_name, 0); WHY("old version kept"); return -1; }
    if (renameat(root_fd, stage_name, root_fd, rec->id)) {
        if (had) renameat(root_fd, old_name, root_fd, rec->id);
        rm_tree_at(root_fd, stage_name, 0);
        WHY("not moved into place: %s", strerror(errno));
        return -1;
    }
    if (had) rm_tree_at(root_fd, old_name, 0);
    fsync(root_fd);
    return 0;
}

/* a regular file of the theme, whole (O_NOFOLLOW at every level: the theme's folder too) */
static char *read_in(int root_fd, const char *id, const char *name, size_t max, size_t *len) {
    if (!ts_valid_id(id)) return NULL;
    int d = openat(root_fd, id, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (d < 0) return NULL;
    int fd = openat(d, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    close(d);
    if (fd < 0) return NULL;
    struct stat st;
    char *b = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 0 && (size_t)st.st_size <= max &&
        (b = malloc((size_t)st.st_size + 1))) {
        size_t n = 0;
        while (n < (size_t)st.st_size) {
            ssize_t r = read(fd, b + n, (size_t)st.st_size - n);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            n += (size_t)r;
        }
        if (n != (size_t)st.st_size) { free(b); b = NULL; }
        else { b[n] = 0; *len = n; }
    }
    close(fd);
    return b;
}

int ts_record_read(int root_fd, const char *id, TsRecord *r) {
    size_t len = 0;
    char *t = read_in(root_fd, id, "install.json", 8192, &len);
    if (!t) return -1;
    int ok = !ts_record_parse(t, len, r) && !strcmp(r->id, id);
    free(t);
    return ok ? 0 : -1;
}

int ts_record_write(int root_fd, const TsRecord *r) {
    if (!ts_valid_id(r->id)) return -1;
    int d = openat(root_fd, r->id, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (d < 0) return -1;
    int ret = write_record_at(d, r);
    close(d);
    return ret;
}

char *ts_code_read(int root_fd, const TsRecord *r, size_t *len) {
    char *b = read_in(root_fd, r->id, "theme.js", TS_CODE_MAX, len);
    if (!b) return NULL;
    uint8_t d[32];
    char h[65];
    sha256(b, *len, d);
    hex(d, 32, h);
    if (strcmp(h, r->sha256)) { free(b); return NULL; }
    return b;
}

/* the person's wallpaper as the SDK's findWallpaper gives it (QA of 0.8.9.6.2, BUG-H): the copy of their own picture,
 * <home>/.local/share/lordos/wallpapers/<16 hex>[.thumb].jpg, or a catalog picture LordOS downloaded,
 * /var/lib/lordos/wallpapers/<64 hex>.jpg. Those exact names only: never another file of the home or the system */
static int hex_run(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}
int jst_wallpaper_file(const char *s, const char *home) {
    static const char store[] = "/var/lib/lordos/wallpapers/", mine[] = "/.local/share/lordos/wallpapers/";
    if (!s) return 0;
    size_t n = strlen(s);
    if (!strncmp(s, store, sizeof store - 1)) return n == sizeof store - 1 + 68 && hex_run(s + sizeof store - 1, 64) && !strcmp(s + n - 4, ".jpg");
    size_t h = home ? strlen(home) : 0;
    if (h < 2 || home[0] != '/' || strncmp(s, home, h) || strncmp(s + h, mine, sizeof mine - 1)) return 0;
    const char *f = s + h + sizeof mine - 1;
    return hex_run(f, 16) && (!strcmp(f + 16, ".jpg") || !strcmp(f + 16, ".thumb.jpg"));
}
