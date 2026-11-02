/* pkg.c — package actions of the privileged helper (see pkg.h and docs/PACKAGES.md).
 *
 *   pkg-update                      reads apt's sources, downloads the InRelease files, verifies the
 *                                   signatures, downloads Packages/Translation/DEP-11 verified by
 *                                   SHA-256 and builds /var/lib/lordos/pkg/index.bin
 *   pkg-install a b [--recommends] [--allow-remove]
 *   pkg-remove a b [--cascade] [--purge] [--keep-orphans]
 *   pkg-autoremove                  removes the dependencies left unused
 *   pkg-upgrade [--allow-remove]
 *   pkg-install-file <path.deb in the user's folder> [--allow-remove]
 *
 * Install = resolve (resolve.c), download the .debs (http.c, as nobody) to /var/cache/lordos/pkg,
 * verify their SHA-256 against the signed index and only then hand the files to dpkg (the only
 * one that touches /var/lib/dpkg). Progress for the interface: LOG lines "progress <phase> <done> <total>"
 * (phases: lists, index, download, unpack, configure, remove) and "plan <action> <package>
 * <version> <bytes>" before starting; the rest of the LOGs are text to display. */
#include "pkg.h"
#include "../sec/json.h"
#include "crypto.h"
#include "deb.h"
#include "http.h"
#include "idx.h"
#include "pgp.h"
#include "resolve.h"
#include "status.h"
#include "xz.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LISTS PKG_DIR "/lists"
#define ICONS PKG_DIR "/icons/64"
#define MAX_SOURCES 32
#define MAX_INDEX (512u << 20)          /* a decompressed Packages can't exceed this */

/* ---- utilities ---- */

static int mkdirs(const char *path, mode_t mode) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; if (mkdir(p, 0755) && errno != EEXIST) return -1; *s = '/'; }
    if (mkdir(p, mode) && errno != EEXIST) return -1;
    struct stat st;
    return lstat(p, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

static uint8_t *read_file(const char *path, size_t *len, size_t max) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size > max) { close(fd); return NULL; }
    uint8_t *b = malloc((size_t)st.st_size + 1);
    size_t n = 0;
    while (b && n < (size_t)st.st_size) {
        ssize_t r = read(fd, b + n, (size_t)st.st_size - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    if (!b || n != (size_t)st.st_size) { free(b); return NULL; }
    b[n] = 0;
    *len = n;
    return b;
}

static int lock_pkg(void) {
    if (mkdirs(PKG_DIR, 0755)) return -1;
    int fd = open(PKG_DIR "/lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return -2; }
    return fd;
}

static void progress(const char *phase, uint64_t done, uint64_t total) {
    admin_say("LOG", "progress %s %llu %llu", phase, (unsigned long long)done, (unsigned long long)total);   /* i18n-ok: protocol line parsed by apps/store */
}

static void fetch_progress(uint64_t done, uint64_t total, void *ud) { progress((const char *)ud, done, total); }

static void sanitize(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (; *in && o + 1 < n; in++) out[o++] = isalnum((unsigned char)*in) || strchr(".+~-", *in) ? *in : '_';
    out[o] = 0;
}

/* ---- sources (deb822 .sources and one-line sources.list) ---- */

typedef struct {
    char uri[512], suite[96];
    char comps[16][48];
    int ncomp;
    char keys[4][512];
    int nkeys;
    char *inline_key;
} Source;

static void mirror_uri(const char *in, char *out, size_t n) {
    /* mirror+file:/etc/apt/mirrors/debian.list: the first mirror in the list */
    out[0] = 0;
    if (!strncmp(in, "mirror+file:", 12)) {
        FILE *f = fopen(in + 12, "r");
        char line[600];
        while (f && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\t \r\n")] = 0;
            if (!strncmp(line, "http://", 7) || !strncmp(line, "https://", 8)) { mirror_uri(line, out, n); break; }
        }
        if (f) fclose(f);
        return;
    }
    if (!strncmp(in, "mirror+", 7)) in += 7;
    /* (0.8.5) https:// stays https (http.c → lord-tls); the signature is still what is trusted */
    if (!strncmp(in, "https://", 8) || !strncmp(in, "http://", 7)) snprintf(out, n, "%s", in);
    else if (!strncmp(in, "file:/", 6)) snprintf(out, n, "%s", in);       /* a local repository (http.c) */
    size_t l = strlen(out);
    while (l && out[l - 1] == '/') out[--l] = 0;
}

/* comma: also split on ',' (apt accepts "Architectures: amd64,arm64", which Cursor's package writes) */
static int words_sep(const char *v, size_t n, char out[][96], int max, int comma) {
    int k = 0;
    size_t i = 0;
#define WSEP(c) (isspace((unsigned char)(c)) || (comma && (c) == ','))
    while (i < n && k < max) {
        while (i < n && WSEP(v[i])) i++;
        size_t s = i;
        while (i < n && !WSEP(v[i])) i++;
        if (i > s) { snprintf(out[k], 96, "%.*s", (int)(i - s), v + s); k++; }
    }
    return k;
#undef WSEP
}
static int words(const char *v, size_t n, char out[][96], int max) { return words_sep(v, n, out, max, 0); }

static void add_source(Source *src, int *ns, const char *uri, const char *suite, char comps[][96], int nc,
                       const char *signed_by, size_t sblen) {
    if (*ns >= MAX_SOURCES) return;
    Source *s = &src[*ns];
    memset(s, 0, sizeof *s);
    mirror_uri(uri, s->uri, sizeof s->uri);
    if (!s->uri[0]) { admin_log("pkg.sourceSkipped", "uri", uri, NULL); return; }
    if (strchr(suite, '/') || !suite[0]) { admin_log("pkg.flatRepo", "uri", uri, "suite", suite, NULL); return; }
    snprintf(s->suite, sizeof s->suite, "%s", suite);
    for (int i = 0; i < nc && i < 16; i++) snprintf(s->comps[s->ncomp++], sizeof s->comps[0], "%s", comps[i]);
    if (sblen) {
        if (memchr(signed_by, '\n', sblen) || (sblen > 5 && !strncmp(signed_by, "-----", 5))) {
            /* inline key (deb822): strip each line's leading space and " ." = empty line */
            s->inline_key = malloc(sblen + 2);
            size_t o = 0;
            if (s->inline_key) {
                const char *p = signed_by, *e = signed_by + sblen;
                while (p < e) {
                    const char *nl = memchr(p, '\n', (size_t)(e - p));
                    if (!nl) nl = e;
                    const char *l = p;
                    while (l < nl && (*l == ' ' || *l == '\t')) l++;
                    if (nl - l == 1 && *l == '.') l = nl;
                    memcpy(s->inline_key + o, l, (size_t)(nl - l));
                    o += (size_t)(nl - l);
                    s->inline_key[o++] = '\n';
                    p = nl + 1;
                }
                s->inline_key[o] = 0;
            }
        } else {
            char w[4][96];
            int k = words(signed_by, sblen, w, 4);
            for (int i = 0; i < k; i++) if (w[i][0] == '/') snprintf(s->keys[s->nkeys++], sizeof s->keys[0], "%s", w[i]);
        }
    }
    (*ns)++;
}

static void parse_deb822(const char *buf, size_t len, Source *src, int *ns, const char *arch) {
    const char *pos = buf, *end = buf + len, *st, *se;
    while (d822_next_stanza(&pos, end, &st, &se)) {
        D822 it = { st, se };
        D822Field f;
        char types[8][96], uris[8][96], suites[8][96], comps[16][96], archs[8][96];
        int nt = 0, nu = 0, nsu = 0, nc = 0, na = 0, enabled = 1;
        const char *sb = NULL;
        size_t sbl = 0;
        while (d822_next_field(&it, &f)) {
            if (d822_is(&f, "Types")) nt = words(f.value, f.vlen, types, 8);
            else if (d822_is(&f, "URIs")) nu = words(f.value, f.vlen, uris, 8);
            else if (d822_is(&f, "Suites")) nsu = words(f.value, f.vlen, suites, 8);
            else if (d822_is(&f, "Components")) nc = words(f.value, f.vlen, comps, 16);
            else if (d822_is(&f, "Architectures")) na = words_sep(f.value, f.vlen, archs, 8, 1);
            else if (d822_is(&f, "Enabled") && f.vlen >= 2 && !strncasecmp(f.value, "no", 2)) enabled = 0;
            else if (d822_is(&f, "Signed-By")) { sb = f.value; sbl = f.vlen; }
        }
        int deb = 0;
        for (int i = 0; i < nt; i++) if (!strcmp(types[i], "deb")) deb = 1;
        int arch_ok = !na;
        for (int i = 0; i < na; i++) if (!strcmp(archs[i], arch)) arch_ok = 1;
        if (!deb || !enabled || !arch_ok) continue;
        for (int u = 0; u < nu; u++)
            for (int s = 0; s < nsu; s++) add_source(src, ns, uris[u], suites[s], comps, nc, sb, sbl);
    }
}

static void parse_list(const char *buf, size_t len, Source *src, int *ns, const char *arch) {
    const char *p = buf, *end = buf + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) nl = end;
        char line[1024];
        snprintf(line, sizeof line, "%.*s", (int)(nl - p), p);
        p = nl + 1;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        if (strncmp(s, "deb", 3) || !isspace((unsigned char)s[3])) continue;
        s += 4;
        char signed_by[512] = "";
        int arch_ok = 1;
        while (isspace((unsigned char)*s)) s++;
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close) continue;
            *close = 0;
            char *tok = strtok(s + 1, " \t");
            while (tok) {
                if (!strncmp(tok, "signed-by=", 10)) snprintf(signed_by, sizeof signed_by, "%s", tok + 10);
                else if (!strncmp(tok, "arch=", 5)) {
                    arch_ok = 0;
                    for (char *a = strtok(tok + 5, ","); a; a = strtok(NULL, ",")) if (!strcmp(a, arch)) arch_ok = 1;
                }
                tok = strtok(NULL, " \t");
            }
            s = close + 1;
        }
        char w[20][96];
        int k = words(s, strlen(s), w, 20);
        if (k < 2 || !arch_ok) continue;
        add_source(src, ns, w[0], w[1], w + 2, k - 2, signed_by, strlen(signed_by));
    }
}

static int load_sources(Source *src, const char *arch) {
    int ns = 0;
    size_t len;
    uint8_t *b = read_file("/etc/apt/sources.list", &len, 1 << 20);
    if (b) { parse_list((char *)b, len, src, &ns, arch); free(b); }
    glob_t g;
    if (!glob("/etc/apt/sources.list.d/*.list", 0, NULL, &g)) {
        for (size_t i = 0; i < g.gl_pathc; i++) if ((b = read_file(g.gl_pathv[i], &len, 1 << 20))) { parse_list((char *)b, len, src, &ns, arch); free(b); }
        globfree(&g);
    }
    if (!glob("/etc/apt/sources.list.d/*.sources", 0, NULL, &g)) {
        for (size_t i = 0; i < g.gl_pathc; i++) if ((b = read_file(g.gl_pathv[i], &len, 1 << 20))) { parse_deb822((char *)b, len, src, &ns, arch); free(b); }
        globfree(&g);
    }
    return ns;
}

static int load_keyring(const Source *s, PgpKeyring *kr) {
    memset(kr, 0, sizeof *kr);
    if (s->inline_key) return pgp_keyring_add_mem(kr, s->inline_key, strlen(s->inline_key));
    if (s->nkeys) {
        for (int i = 0; i < s->nkeys; i++) if (pgp_keyring_add_file(kr, s->keys[i])) admin_log("pkg.keyUnreadable", "path", s->keys[i], NULL);
        return kr->n ? 0 : -1;
    }
    /* no Signed-By: apt's trusted keys */
    glob_t g;
    if (!glob("/etc/apt/trusted.gpg.d/*.gpg", 0, NULL, &g)) { for (size_t i = 0; i < g.gl_pathc; i++) pgp_keyring_add_file(kr, g.gl_pathv[i]); globfree(&g); }
    if (!glob("/etc/apt/trusted.gpg.d/*.asc", 0, NULL, &g)) { for (size_t i = 0; i < g.gl_pathc; i++) pgp_keyring_add_file(kr, g.gl_pathv[i]); globfree(&g); }
    pgp_keyring_add_file(kr, "/etc/apt/trusted.gpg");
    if (!kr->n) pgp_keyring_add_file(kr, "/usr/share/keyrings/debian-archive-keyring.gpg");
    return kr->n ? 0 : -1;
}

/* ---- Release ---- */

typedef struct { char path[160]; uint8_t sha[32]; uint64_t size; } RelFile;

typedef struct {
    char suite[96], codename[96], origin[96], label[96];
    char comps[256];
    int64_t date, valid_until;
    int not_auto, but_auto, by_hash, no_all;
    RelFile *files;
    int nfiles;
} Release;

static int parse_release(const char *body, size_t len, Release *r) {
    memset(r, 0, sizeof *r);
    D822 it = { body, body + len };
    D822Field f;
    while (d822_next_field(&it, &f)) {
        char v[256];
        snprintf(v, sizeof v, "%.*s", (int)(f.vlen < 255 ? f.vlen : 255), f.value);
        if (d822_is(&f, "Suite")) snprintf(r->suite, sizeof r->suite, "%.95s", v);
        else if (d822_is(&f, "Codename")) snprintf(r->codename, sizeof r->codename, "%.95s", v);
        else if (d822_is(&f, "Origin")) snprintf(r->origin, sizeof r->origin, "%.95s", v);
        else if (d822_is(&f, "Label")) snprintf(r->label, sizeof r->label, "%.95s", v);
        else if (d822_is(&f, "Components")) snprintf(r->comps, sizeof r->comps, "%s", v);
        else if (d822_is(&f, "Date")) r->date = http_parse_date(v);
        else if (d822_is(&f, "Valid-Until")) r->valid_until = http_parse_date(v);
        else if (d822_is(&f, "NotAutomatic")) r->not_auto = !strcmp(v, "yes");
        else if (d822_is(&f, "ButAutomaticUpgrades")) r->but_auto = !strcmp(v, "yes");
        else if (d822_is(&f, "Acquire-By-Hash")) r->by_hash = !strcmp(v, "yes");
        else if (d822_is(&f, "No-Support-for-Architecture-all")) r->no_all = !!strstr(v, "Packages");
        else if (d822_is(&f, "SHA256")) {
            const char *p = f.value, *e = f.value + f.vlen;
            while (p < e) {
                const char *nl = memchr(p, '\n', (size_t)(e - p));
                if (!nl) nl = e;
                char hex[80], path[200];
                unsigned long long size;
                char line[400];
                snprintf(line, sizeof line, "%.*s", (int)(nl - p), p);
                if (sscanf(line, " %79s %llu %199s", hex, &size, path) == 3 && strlen(hex) == 64 && strlen(path) < sizeof r->files[0].path &&
                    !strstr(path, "..") && path[0] != '/') {
                    RelFile *nf = realloc(r->files, sizeof *nf * (size_t)(r->nfiles + 1));
                    if (!nf) return -1;
                    r->files = nf;
                    RelFile *x = &r->files[r->nfiles];
                    if (!hex_decode(hex, x->sha, 32)) {
                        snprintf(x->path, sizeof x->path, "%s", path);
                        x->size = size;
                        r->nfiles++;
                    }
                }
                p = nl + 1;
            }
        }
    }
    return 0;
}

static const RelFile *rel_find(const Release *r, const char *path) {
    for (int i = 0; i < r->nfiles; i++) if (!strcmp(r->files[i].path, path)) return &r->files[i];
    return NULL;
}

/* Picks the best available compression: .xz, .gz or uncompressed. */
static const RelFile *rel_pick(const Release *r, const char *base, const char **ext) {
    static const char *const exts[] = { ".xz", ".gz", "" };
    for (int i = 0; i < 3; i++) {
        char p[200];
        snprintf(p, sizeof p, "%s%s", base, exts[i]);
        const RelFile *f = rel_find(r, p);
        if (f) { *ext = exts[i]; return f; }
    }
    return NULL;
}

/* ---- decompression by extension ---- */

static uint8_t *load_index_file(const char *path, const char *ext, size_t *out_len) {
    size_t len;
    uint8_t *raw = read_file(path, &len, MAX_INDEX);
    if (!raw) return NULL;
    if (!*ext) { *out_len = len; return raw; }
    uint8_t *out = NULL;
    int r = !strcmp(ext, ".xz") ? xz_decode(raw, len, &out, out_len, MAX_INDEX) : gz_decode(raw, len, &out, out_len, MAX_INDEX);
    free(raw);
    return r ? NULL : out;
}

/* ---- tar (icons) ---- */

typedef int (*TarFn)(const char *name, const uint8_t *data, size_t len, void *ud);

static int tar_walk(const uint8_t *t, size_t len, TarFn fn, void *ud) {
    size_t p = 0;
    char longname[512] = "";
    while (p + 512 <= len) {
        const uint8_t *h = t + p;
        if (!h[0]) break;
        char name[256 + 2];
        size_t size = 0;
        for (int i = 124; i < 136 && h[i]; i++) if (h[i] >= '0' && h[i] <= '7') size = size * 8 + (size_t)(h[i] - '0');
        if (p + 512 + size > len) return -1;
        char type = (char)h[156];
        if (longname[0]) { snprintf(name, sizeof name, "%s", longname); longname[0] = 0; }
        else {
            char prefix[156];
            snprintf(prefix, sizeof prefix, "%.155s", (const char *)h + 345);
            if (!memcmp(h + 257, "ustar", 5) && prefix[0]) snprintf(name, sizeof name, "%s/%.100s", prefix, (const char *)h);
            else snprintf(name, sizeof name, "%.100s", (const char *)h);
        }
        if (type == 'L') snprintf(longname, sizeof longname, "%.*s", (int)(size < 511 ? size : 511), (const char *)h + 512);
        else if ((type == '0' || type == 0) && fn(name, h + 512, size, ud)) return -1;
        p += 512 + ((size + 511) & ~(size_t)511);
    }
    return 0;
}

typedef struct { IdxBuilder *b; int written; char **names; int nnames, cap; } IconCtx;

static int icon_entry(const char *name, const uint8_t *data, size_t len, void *ud) {
    IconCtx *c = ud;
    while (name[0] == '.' && name[1] == '/') name += 2;
    if (strchr(name, '/') || !name[0] || name[0] == '.' || strlen(name) > 200) return 0;
    for (const char *p = name; *p; p++) if (!isalnum((unsigned char)*p) && !strchr("._+-", *p)) return 0;
    if (!idxb_icon_wanted(c->b, name) || len > (1 << 20)) return 0;
    char path[PATH_MAX], tmp[PATH_MAX];
    snprintf(path, sizeof path, ICONS "/%s", name);
    snprintf(tmp, sizeof tmp, ICONS "/.%s.new", name);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return 0;
    int ok = write(fd, data, len) == (ssize_t)len;
    close(fd);
    if (!ok || rename(tmp, path)) { unlink(tmp); return 0; }
    c->written++;
    return 0;
}

static void icons_cleanup(IdxBuilder *b) {
    DIR *d = opendir(ICONS);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || !strcmp(e->d_name, ".."))) continue;
        if (e->d_name[0] == '.' || !idxb_icon_wanted(b, e->d_name)) unlinkat(dirfd(d), e->d_name, 0);
    }
    if (d) closedir(d);
}

/* ---- pkg-update ---- */

#define NOTES_MAX (4u << 20)

/* PKG_DIR/notes: the verified ReleaseNotes of every source (deb822: Package, Version, Notes), read
 * by lord.pkg.releaseNotes() without root. Replaced whole; removed when no source has any. */
static void write_notes(const char *notes, size_t len) {
    if (!len) { unlink(PKG_DIR "/notes"); return; }
    int fd = open(PKG_DIR "/.notes.new", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return;
    int ok = write(fd, notes, len) == (ssize_t)len;
    if (close(fd)) ok = 0;
    if (!ok || rename(PKG_DIR "/.notes.new", PKG_DIR "/notes")) unlink(PKG_DIR "/.notes.new");
}

/* The Store's banner (Store → Discover, apt/featured/ in the repository): a small JSON object and a
 * PNG, both listed in LordOS's signed InRelease. The JSON is checked to parse here and again by the
 * Store; the PNG only by its signature and size (it is drawn like the DEP-11 icons). Without them the
 * Store shows its own banner. */
#define FEATURED_MAX (64u << 10)
#define BANNER_MAX (2u << 20)
static int featured_valid(const uint8_t *t, size_t n) {
    if (!n || n > FEATURED_MAX) return 0;
    JDoc d;
    if (json_parse((const char *)t, n, &d, NULL, 0)) return 0;
    int ok = d.root && d.root->type == J_OBJ;
    json_free(&d);
    return ok;
}
static int banner_valid(const uint8_t *t, size_t n) {
    static const uint8_t png[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    return n > 8 && n <= BANNER_MAX && !memcmp(t, png, 8);
}
static void write_one(const char *path, const char *tmp, const uint8_t *d, size_t n) {
    if (!d || !n) { unlink(path); return; }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return;
    int ok = write(fd, d, n) == (ssize_t)n;
    if (close(fd)) ok = 0;
    if (!ok || rename(tmp, path)) unlink(tmp);
}
static void write_featured(const uint8_t *feat, size_t fl, const uint8_t *img, size_t il) {
    write_one(PKG_DIR "/featured.json", PKG_DIR "/.featured.json.new", feat, fl);
    write_one(PKG_DIR "/featured.png", PKG_DIR "/.featured.png.new", feat ? img : NULL, il);
}

typedef struct { char path[PATH_MAX]; char ext[4]; int kind, src; char comp[48]; } Wanted;   /* kind 0 Packages, 1 Translation, 2 DEP-11, 3 icons, 4 release notes, 5 featured, 6 banner */

static int file_ok(const char *path, uint64_t size) {
    struct stat st;
    return !lstat(path, &st) && S_ISREG(st.st_mode) && (uint64_t)st.st_size == size;
}

/* What the index was built from: the list files it read (named by their own SHA-256) and their
 * component. Stored in PKG_DIR/sources after each build: a component or source added or removed
 * without a new InRelease upstream (enabling non-free-firmware, say) still rebuilds the index. */
#define SOURCES_SUM PKG_DIR "/sources"
static void wanted_sum(const Wanted *w, int nw, char hex[65]) {
    Sha256 s;
    uint8_t d[32];
    sha256_init(&s);
    for (int k = 0; k < nw; k++) {
        sha256_update(&s, w[k].path, strlen(w[k].path));
        sha256_update(&s, " ", 1);
        sha256_update(&s, w[k].comp, strlen(w[k].comp));
        sha256_update(&s, "\n", 1);
    }
    sha256_final(&s, d);
    hex_encode(d, 32, hex);
}

static int sources_changed(const char *sum) {
    char old[80] = "";
    FILE *f = fopen(SOURCES_SUM, "re");
    if (f) { if (!fgets(old, sizeof old, f)) old[0] = 0; fclose(f); }
    old[strcspn(old, "\n")] = 0;
    return strcmp(old, sum) != 0;
}

static void sources_save(const char *sum) {
    FILE *f = fopen(SOURCES_SUM ".new", "we");
    if (!f) return;
    int ok = fprintf(f, "%s\n", sum) > 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(SOURCES_SUM ".new", SOURCES_SUM)) unlink(SOURCES_SUM ".new");
}

static void gc_lists(Wanted *w, int nw, Source *src, int ns) {
    DIR *d = opendir(LISTS);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[PATH_MAX];
        snprintf(full, sizeof full, LISTS "/%s", e->d_name);
        int keep = 0;
        for (int i = 0; i < nw && !keep; i++) if (!strcmp(w[i].path, full)) keep = 1;
        for (int i = 0; i < ns && !keep; i++) {
            char name[700];
            snprintf(name, sizeof name, "%s_%s", src[i].uri, src[i].suite);
            char san[700];
            sanitize(name, san, sizeof san);
            char p[PATH_MAX];
            snprintf(p, sizeof p, LISTS "/%.300s_InRelease", san);
            if (!strcmp(p, full)) keep = 1;
        }
        if (!keep) unlinkat(dirfd(d), e->d_name, 0);
    }
    if (d) closedir(d);
}

void pkg_act_update(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg; (void)len;
    int lk = lock_pkg();
    if (lk == -2) { admin_err("pkg.busy", NULL); return; }
    if (lk < 0 || mkdirs(LISTS, 0755) || mkdirs(LISTS "/partial", 0755) || mkdirs(ICONS, 0755)) { admin_err("pkg.prepareFailed", "path", PKG_DIR, NULL); return; }
    const char *arch = pkg_native_arch();
    Source *src = calloc(MAX_SOURCES, sizeof *src);
    if (!src) { admin_err("sys.outOfMemory", NULL); return; }
    int ns = load_sources(src, arch);
    if (!ns) { admin_err("pkg.noSources", NULL); free(src); return; }

    /* 1. InRelease of each source */
    FetchItem *rel = calloc((size_t)ns, sizeof *rel);
    char (*final)[PATH_MAX] = calloc((size_t)ns, PATH_MAX);
    if (!rel || !final) { admin_err("sys.outOfMemory", NULL); return; }
    for (int i = 0; i < ns; i++) {
        char name[700], san[700];
        snprintf(name, sizeof name, "%s_%s", src[i].uri, src[i].suite);
        sanitize(name, san, sizeof san);
        snprintf(final[i], PATH_MAX, LISTS "/%.300s_InRelease", san);
        snprintf(rel[i].url, sizeof rel[i].url, "%s/dists/%s/InRelease", src[i].uri, src[i].suite);
        snprintf(rel[i].dest, sizeof rel[i].dest, LISTS "/partial/%.300s_InRelease", san);
        rel[i].max = 64u << 20;
        struct stat st;
        if (!stat(final[i], &st)) rel[i].ims = (int64_t)st.st_mtime;
        admin_log("pkg.source", "uri", src[i].uri, "suite", src[i].suite, "component", src[i].comps[0], NULL);
    }
    progress("lists", 0, (uint64_t)ns);
    fetch_batch(rel, ns, NULL, NULL);

    Release *R = calloc((size_t)ns, sizeof *R);
    int *good = calloc((size_t)ns, sizeof *good);
    Wanted *w = NULL;
    int nw = 0, failures = 0;
    int64_t now = (int64_t)time(NULL);
    for (int i = 0; R && good && i < ns; i++) {
        const char *path = rel[i].status == 200 ? rel[i].dest : rel[i].status == 304 ? final[i] : NULL;
        if (!path) { admin_log("pkg.sourceFailed", "uri", src[i].uri, "suite", src[i].suite, "reason", rel[i].err, NULL); failures++; continue; }
        size_t blen;
        uint8_t *b = read_file(path, &blen, 64u << 20);
        PgpKeyring kr;
        char *body = NULL, err[256];
        size_t bodylen = 0;
        int ok = b && !load_keyring(&src[i], &kr);
        if (!ok) lerr_set(err, sizeof err, b ? "pkg.release.noKeys" : "pkg.release.unreadable", NULL);
        else {
            int g = pgp_verify_clearsigned(&kr, (char *)b, blen, now, &body, &bodylen, err, sizeof err);
            pgp_keyring_free(&kr);
            ok = g > 0;
        }
        free(b);
        if (ok && parse_release(body, bodylen, &R[i])) { ok = 0; lerr_set(err, sizeof err, "pkg.release.malformed", NULL); }
        if (ok && R[i].valid_until && R[i].valid_until < now) { ok = 0; lerr_set(err, sizeof err, "pkg.release.expired", NULL); }
        if (ok && R[i].date > now + 86400) { ok = 0; lerr_set(err, sizeof err, "pkg.release.future", NULL); }
        if (ok && strcmp(R[i].suite, src[i].suite) && strcmp(R[i].codename, src[i].suite)) { ok = 0; lerr_set(err, sizeof err, "pkg.release.wrongSuite", "got", R[i].codename, "want", src[i].suite, NULL); }
        free(body);
        if (ok && rel[i].status == 200) {
            /* don't accept a list older than the one we already had (replay attack) */
            size_t ol;
            uint8_t *old = read_file(final[i], &ol, 64u << 20);
            if (old) {
                const char *d = strstr((char *)old, "\nDate: ");
                if (d && http_parse_date(d + 7) > R[i].date) { ok = 0; lerr_set(err, sizeof err, "pkg.release.older", NULL); }
                free(old);
            }
            if (ok) {
                if (rename(rel[i].dest, final[i])) { ok = 0; lerr_set(err, sizeof err, "pkg.release.saveFailed", NULL); }
                else if (rel[i].last_modified) {
                    struct timespec ts[2] = { { (time_t)rel[i].last_modified, 0 }, { (time_t)rel[i].last_modified, 0 } };
                    utimensat(AT_FDCWD, final[i], ts, 0);
                }
            }
        }
        if (!ok) { admin_log("pkg.sourceFailed", "uri", src[i].uri, "suite", src[i].suite, "reason", err, NULL); unlink(rel[i].dest); failures++; continue; }
        good[i] = 1;
        admin_log("pkg.sourceVerified", "uri", src[i].uri, "suite", src[i].suite, "label", R[i].label[0] ? R[i].label : R[i].origin, NULL);
        /* which files to download */
        for (int c = 0; c < src[i].ncomp; c++) {
            char base[200];
            const char *ext;
            for (int k = 0; k < 7; k++) {
                if (k >= 5 && strcmp(R[i].origin, "LordOS")) continue;   /* the Store's banner: LordOS's repository only */
                if (k == 0) snprintf(base, sizeof base, "%s/binary-%s/Packages", src[i].comps[c], arch);
                else if (k == 1) snprintf(base, sizeof base, "%s/i18n/Translation-en", src[i].comps[c]);
                else if (k == 2) snprintf(base, sizeof base, "%s/dep11/Components-%s.yml", src[i].comps[c], arch);
                else if (k == 3) snprintf(base, sizeof base, "%s/dep11/icons-64x64.tar", src[i].comps[c]);
                else if (k == 4) snprintf(base, sizeof base, "%s/notes/ReleaseNotes", src[i].comps[c]);   /* LordOS's release notes */
                else if (k == 5) snprintf(base, sizeof base, "%s/featured/Featured", src[i].comps[c]);    /* the Store's banner (JSON) */
                else snprintf(base, sizeof base, "%s/featured/Banner", src[i].comps[c]);                  /* and its image (PNG) */
                const RelFile *f = rel_pick(&R[i], base, &ext);
                if (k == 2 && f && !strcmp(ext, "")) f = NULL;     /* the uncompressed YAML is huge */
                if (!f) { if (k == 0) admin_log("pkg.sourceMissingFile", "uri", src[i].uri, "suite", src[i].suite, "file", base, NULL); continue; }
                Wanted *nwp = realloc(w, sizeof *w * (size_t)(nw + 2));
                if (!nwp) continue;
                w = nwp;
                Wanted *x = &w[nw++];
                memset(x, 0, sizeof *x);
                char hex[65];
                hex_encode(f->sha, 32, hex);
                snprintf(x->path, sizeof x->path, LISTS "/%s%s", hex, ext);
                snprintf(x->ext, sizeof x->ext, "%s", ext);
                x->kind = k;
                x->src = i;
                snprintf(x->comp, sizeof x->comp, "%s", src[i].comps[c]);
                if (k == 0 && !R[i].no_all) {           /* "all" Packages separately */
                    snprintf(base, sizeof base, "%s/binary-all/Packages", src[i].comps[c]);
                    const RelFile *fa = rel_pick(&R[i], base, &ext);
                    if (fa) {
                        Wanted *y = &w[nw++];
                        memset(y, 0, sizeof *y);
                        hex_encode(fa->sha, 32, hex);
                        snprintf(y->path, sizeof y->path, LISTS "/%s%s", hex, ext);
                        snprintf(y->ext, sizeof y->ext, "%s", ext);
                        y->kind = 0;
                        y->src = i;
                        snprintf(y->comp, sizeof y->comp, "%s", src[i].comps[c]);
                    }
                }
            }
        }
    }
    int changed = 0;
    for (int i = 0; i < ns; i++) if (rel[i].status != 304) changed = 1;
    free(rel);
    progress("lists", (uint64_t)ns, (uint64_t)ns);
    /* release notes listed but not stored yet (an index built before LordOS read them): rebuild */
    for (int k = 0; k < nw && !changed; k++) if (w[k].kind == 4 && access(PKG_DIR "/notes", F_OK)) changed = 1;
    for (int k = 0; k < nw && !changed; k++) if (w[k].kind == 5 && access(PKG_DIR "/featured.json", F_OK)) changed = 1;
    /* a component or source added or removed: the same InRelease files, other lists */
    char sum[65];
    wanted_sum(w, nw, sum);
    if (!changed && sources_changed(sum)) changed = 1;
    Idx cur;
    if (!changed && !failures && !idx_open(&cur, PKG_DIR)) {
        /* no list changed since last time: the index is already up to date */
        uint32_t n = cur.h->npkg;
        idx_close(&cur);
        for (int i = 0; i < ns; i++) { free(R[i].files); free(src[i].inline_key); }
        free(R); free(good); free(w); free(src); free(final);
        utimensat(AT_FDCWD, PKG_DIR "/index.bin", NULL, 0);       /* "last update": now */
        close(lk);
        admin_ok("pkg.upToDate", "count", lerr_int(n), NULL);
        return;
    }
    int ngood = 0;
    for (int i = 0; good && i < ns; i++) ngood += good[i];
    if (!ngood) { admin_err("pkg.noVerifiedSource", NULL); return; }

    /* 2. missing indexes (by their SHA-256: what's already there isn't downloaded again) */
    FetchItem *fi = calloc((size_t)(nw ? nw : 1), sizeof *fi);
    int nf = 0;
    uint64_t total = 0;
    for (int k = 0; fi && k < nw; k++) {
        Wanted *x = &w[k];
        const char *hexname = strrchr(x->path, '/') + 1;
        /* look up the RelFile again for the size and the URL */
        const RelFile *f = NULL;
        for (int j = 0; j < R[x->src].nfiles && !f; j++) {
            char hex[65];
            hex_encode(R[x->src].files[j].sha, 32, hex);
            if (!strncmp(hexname, hex, 64) && !strcmp(hexname + 64, x->ext)) f = &R[x->src].files[j];
        }
        if (!f || file_ok(x->path, f->size)) continue;
        int dup = 0;
        for (int j = 0; j < nf; j++) if (!strcmp(fi[j].dest, x->path)) dup = 1;
        if (dup) continue;
        FetchItem *it = &fi[nf++];
        if (R[x->src].by_hash) {
            char dir[200], hex[65];
            snprintf(dir, sizeof dir, "%s", f->path);
            char *sl = strrchr(dir, '/');
            if (sl) *sl = 0;
            hex_encode(f->sha, 32, hex);
            snprintf(it->url, sizeof it->url, "%s/dists/%s/%s/by-hash/SHA256/%s", src[x->src].uri, src[x->src].suite, dir, hex);
        } else snprintf(it->url, sizeof it->url, "%s/dists/%s/%s", src[x->src].uri, src[x->src].suite, f->path);
        snprintf(it->dest, sizeof it->dest, "%s", x->path);
        it->max = f->size;
        it->expect = f->size;
        total += f->size;
        memcpy(it->sha256, f->sha, 32);            /* the expected value; fetch_batch overwrites it with the computed one */
    }
    uint8_t (*expect)[32] = calloc((size_t)(nf ? nf : 1), 32);
    for (int k = 0; expect && k < nf; k++) memcpy(expect[k], fi[k].sha256, 32);
    if (nf) {
        admin_log("pkg.downloadingIndex", "count", lerr_int(nf), "kb", lerr_int((long long)(total / 1024)), NULL);
        fetch_batch(fi, nf, fetch_progress, "download");
    }
    for (int k = 0; fi && expect && k < nf; k++) {
        if (fi[k].status != 200 || fi[k].size != fi[k].expect || memcmp(fi[k].sha256, expect[k], 32)) {
            if (fi[k].status == 200) lerr_set(fi[k].err, sizeof fi[k].err, "pkg.hashMismatchRelease", NULL);
            admin_log("pkg.fileFailed", "url", fi[k].url, "reason", fi[k].err, NULL);
            unlink(fi[k].dest);
        }
    }
    free(fi);
    free(expect);

    /* 3. index: Packages, Translation, DEP-11, icons */
    IdxBuilder *b = idxb_new();
    int *srcid = calloc((size_t)ns, sizeof *srcid);
    if (!b || !srcid) { admin_err("sys.outOfMemory", NULL); return; }
    for (int i = 0; i < ns; i++) srcid[i] = -1;
    int pk_files = 0, pk_missing = 0;
    char *notes = NULL;                 /* every source's ReleaseNotes, one after the other */
    size_t notes_len = 0;
    uint8_t *feat = NULL, *banner = NULL;   /* the Store's banner: its JSON and its PNG */
    size_t feat_len = 0, banner_len = 0;
    for (int pass = 0; pass < 7; pass++)
        for (int k = 0; k < nw; k++) {
            Wanted *x = &w[k];
            if (x->kind != pass || !good[x->src]) continue;
            progress("index", (uint64_t)(pass * nw + k), (uint64_t)(7 * nw));
            if (srcid[x->src] < 0) {
                Release *rr = &R[x->src];
                int prio = rr->not_auto ? (rr->but_auto ? 100 : 1) : 500;
                srcid[x->src] = idxb_source(b, src[x->src].uri, src[x->src].suite, x->comp, rr->codename, rr->origin, rr->label, rr->date, rr->valid_until, prio);
            }
            if (pass >= 5) {                    /* the banner: one of each, checked here; drawn by the Store */
                size_t tl;
                uint8_t *t = load_index_file(x->path, x->ext, &tl);
                int okf = t && (pass == 5 ? featured_valid(t, tl) : banner_valid(t, tl));
                if (okf && pass == 5 && !feat) { feat = t; feat_len = tl; t = NULL; }
                else if (okf && pass == 6 && !banner) { banner = t; banner_len = tl; t = NULL; }
                free(t);
                continue;
            }
            if (pass == 4) {
                size_t tl;
                uint8_t *t = load_index_file(x->path, x->ext, &tl);
                char *nn = t && notes_len + tl + 2 <= NOTES_MAX ? realloc(notes, notes_len + tl + 2) : NULL;
                if (nn) {
                    notes = nn;
                    memcpy(notes + notes_len, t, tl);
                    notes_len += tl;
                    notes[notes_len++] = '\n';
                    notes[notes_len++] = '\n';
                }
                free(t);
                continue;
            }
            if (pass == 3) {
                size_t tl;
                uint8_t *t = load_index_file(x->path, x->ext, &tl);
                if (!t) continue;
                IconCtx ic = { b, 0, NULL, 0, 0 };
                tar_walk(t, tl, icon_entry, &ic);
                free(t);
                continue;
            }
            size_t dl;
            uint8_t *d = load_index_file(x->path, x->ext, &dl);
            if (!d) {
                if (pass == 0) { pk_missing++; admin_log("pkg.componentMissing", "component", x->comp, "suite", src[x->src].suite, NULL); }
                continue;
            }
            if (pass == 0) { idxb_packages(b, srcid[x->src], (char *)d, dl, arch); pk_files++; }
            else if (pass == 1) idxb_translation(b, (char *)d, dl);
            else idxb_appstream(b, (char *)d, dl);
            free(d);
        }
    progress("index", 1, 1);
    char err[256];
    int wrote = pk_files && !idxb_write(b, PKG_DIR, arch, err, sizeof err);
    if (wrote) icons_cleanup(b);
    if (wrote) write_notes(notes, notes_len);
    if (wrote) write_featured(feat, feat_len, banner, banner_len);
    free(feat);
    free(banner);
    if (wrote && !failures) sources_save(sum);
    free(notes);
    uint32_t count = idxb_count(b);
    idxb_free(b);
    if (wrote) gc_lists(w, nw, src, ns);
    for (int i = 0; i < ns; i++) { free(R[i].files); free(src[i].inline_key); }
    free(R); free(good); free(w); free(src); free(final); free(srcid);
    close(lk);
    if (!wrote) { admin_err("pkg.indexFailed", "reason", pk_files ? err : "pkg.noPackagesFile", NULL); return; }
    admin_event("pkg index");
    if (failures || pk_missing) admin_ok("pkg.updatedWithProblems", "count", lerr_int(count), "sources", lerr_int(ngood), "problems", lerr_int(failures + pk_missing), NULL);
    else admin_ok("pkg.updated", "count", lerr_int(count), "sources", lerr_int(ngood), NULL);
}

/* ---- dpkg ---- */

typedef struct { int unpack, configure, remove, total; } DpkgProgress;

/* Runs dpkg (fixed path) with the frontend lock held by us; forwards its output. */
static int run_dpkg(const char *const argv[], DpkgProgress *pr) {
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) dup2(null, 0);
        dup2(p[1], 1);
        dup2(p[1], 2);
        syscall(SYS_close_range, 3, ~0u, 0);
        signal(SIGPIPE, SIG_DFL);
        char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8", "LC_ALL=C.UTF-8", "HOME=/root",
                              "DEBIAN_FRONTEND=noninteractive", "DEBCONF_NONINTERACTIVE_SEEN=true", "DPKG_FRONTEND_LOCKED=1", "TERM=dumb", NULL };
        execve(argv[0], (char *const *)argv, env);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) { close(p[0]); return -1; }
    FILE *f = fdopen(p[0], "r");
    char line[600];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!line[0]) continue;
        admin_say("LOG", "%s", line);
        if (pr) {
            if (!strncmp(line, "Unpacking ", 10)) progress("unpack", (uint64_t)++pr->unpack, (uint64_t)pr->total);
            else if (!strncmp(line, "Setting up ", 11)) progress("configure", (uint64_t)++pr->configure, (uint64_t)pr->total);
            else if (!strncmp(line, "Removing ", 9) || !strncmp(line, "Purging ", 8)) progress("remove", (uint64_t)++pr->remove, (uint64_t)pr->total);
        }
    }
    if (f) fclose(f); else close(p[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

static int lock_dpkg(void) {
    int fd = open("/var/lib/dpkg/lock-frontend", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0640);
    if (fd < 0) return -1;
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (fcntl(fd, F_SETLK, &fl)) { close(fd); return -2; }
    return fd;
}

/* If a previous installation was cut short, dpkg accepts nothing new until it's finished. */
static void dpkg_pending(void) {
    DIR *d = opendir("/var/lib/dpkg/updates");
    struct dirent *e;
    int pending = 0;
    while (d && (e = readdir(d))) if (e->d_name[0] != '.') pending = 1;
    if (d) closedir(d);
    if (!pending) return;
    admin_log("pkg.dpkgInterrupted", NULL);
    const char *const argv[] = { "/usr/bin/dpkg", "--force-confdef", "--force-confold", "--configure", "-a", NULL };
    run_dpkg(argv, NULL);
}

/* ---- list of what's installed as a dependency ---- */

static void auto_update(const Plan *p) {
    size_t len = 0;
    uint8_t *old = read_file(PKG_DIR "/auto", &len, 4 << 20);
    FILE *f = fopen(PKG_DIR "/auto.new", "w");
    if (!f) { free(old); return; }
    char *s = old ? (char *)old : NULL;
    while (s && *s) {
        char *nl = strchr(s, '\n');
        if (nl) *nl = 0;
        int drop = 0;
        for (int i = 0; i < p->n; i++)
            if (!strcmp(p->items[i].name, s) && (p->items[i].action == PA_REMOVE || p->items[i].requested)) drop = 1;
        if (*s && !drop) fprintf(f, "%s\n", s);
        s = nl ? nl + 1 : NULL;
    }
    for (int i = 0; i < p->n; i++) if (p->items[i].action == PA_INSTALL && !p->items[i].requested) fprintf(f, "%s\n", p->items[i].name);
    int ok = fflush(f) == 0;
    fclose(f);
    free(old);
    if (ok) rename(PKG_DIR "/auto.new", PKG_DIR "/auto"); else unlink(PKG_DIR "/auto.new");
}

/* ---- applying a plan ---- */

static int parse_names(const char *arg, size_t len, char names[][129], int max, int *nn, int *flags) {
    *nn = 0;
    *flags = 0;
    if (len > 8192) return -1;
    char buf[8193];
    memcpy(buf, arg, len);
    buf[len] = 0;
    for (char *tok = strtok(buf, " \t\n"); tok; tok = strtok(NULL, " \t\n")) {
        if (!strcmp(tok, "--recommends")) *flags |= 1;
        else if (!strcmp(tok, "--allow-remove")) *flags |= 2;
        else if (!strcmp(tok, "--cascade")) *flags |= 4;
        else if (!strcmp(tok, "--purge")) *flags |= 8;
        else if (!strcmp(tok, "--keep-orphans")) *flags |= 16;
        else if (deb_valid_name(tok, strlen(tok)) && *nn < max) snprintf(names[(*nn)++], 129, "%s", tok);
        else { { char nm[64]; snprintf(nm, sizeof nm, "%.60s", tok); admin_err("pkg.invalidName", "name", nm, NULL); } return -1; }
    }
    return 0;
}

static void say_plan(const Plan *p, const Idx *x) {
    for (int i = 0; i < p->n; i++) {
        const PlanItem *it = &p->items[i];
        const char *a = it->action == PA_INSTALL ? "install" : it->action == PA_UPGRADE ? "upgrade" : "remove";
        admin_say("LOG", "plan %s %s %s %u", a, it->name, it->version ? it->version : "", it->rec >= 0 ? x->pkg[it->rec].size : 0);   /* i18n-ok: protocol line parsed by apps/store */
    }
    for (int i = 0; i < p->nheld; i++) admin_say("LOG", "plan held %s", p->held[i]);   /* i18n-ok: protocol line parsed by apps/store */
}

static void deb_cache_name(const Idx *x, long rec, char *out, size_t n) {
    char nm[160], ver[160], ar[32];
    sanitize(idx_s(x, x->pkg[rec].name), nm, sizeof nm);
    const char *v = idx_s(x, x->pkg[rec].version);
    size_t o = 0;
    for (; *v && o + 4 < sizeof ver; v++) {
        if (*v == ':') { memcpy(ver + o, "%3a", 3); o += 3; }
        else ver[o++] = isalnum((unsigned char)*v) || strchr(".+~-", *v) ? *v : '_';
    }
    ver[o] = 0;
    sanitize(idx_s(x, x->pkg[rec].arch), ar, sizeof ar);
    snprintf(out, n, PKG_CACHE "/%s_%s_%s.deb", nm, ver, ar);
}

static int file_sha_ok(const char *path, const uint8_t *sha, uint64_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != size) { close(fd); return 0; }
    Sha256 s;
    sha256_init(&s);
    uint8_t buf[65536];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) sha256_update(&s, buf, (size_t)r);
    close(fd);
    uint8_t out[32];
    sha256_final(&s, out);
    return r == 0 && !memcmp(out, sha, 32);
}

/* Downloads, verifies and installs the plan's contents; extra: local .deb to add at the end (or NULL). */
static int apply_plan(const Idx *x, const Plan *p, int purge, const char *extra_deb) {
    int ninst = 0, nrem = 0;
    for (int i = 0; i < p->n; i++) { if (p->items[i].rec >= 0) ninst++; else if (p->items[i].action == PA_REMOVE) nrem++; }
    if (!ninst && !nrem && !extra_deb) { admin_ok("pkg.nothingToDo", NULL); return 0; }
    if (mkdirs(PKG_CACHE, 0755)) { admin_err("pkg.prepareFailed", "path", PKG_CACHE, NULL); return -1; }

    /* download */
    char (*files)[PATH_MAX] = calloc((size_t)(p->n ? p->n : 1), PATH_MAX);
    FetchItem *fi = calloc((size_t)(ninst ? ninst : 1), sizeof *fi);
    int *fitem = calloc((size_t)(ninst ? ninst : 1), sizeof *fitem);
    if (!files || !fi || !fitem) { admin_err("sys.outOfMemory", NULL); return -1; }
    int nf = 0;
    uint64_t total = 0;
    for (int i = 0; i < p->n; i++) {
        long rec = p->items[i].rec;
        if (rec < 0) continue;
        deb_cache_name(x, rec, files[i], PATH_MAX);
        const IdxPkg *k = &x->pkg[rec];
        if (file_sha_ok(files[i], k->sha256, k->size)) continue;
        FetchItem *it = &fi[nf];
        fitem[nf++] = i;
        snprintf(it->url, sizeof it->url, "%s/%s", idx_s(x, x->src[k->src].uri), idx_s(x, k->filename));
        snprintf(it->dest, sizeof it->dest, "%s", files[i]);
        it->max = k->size;
        it->expect = k->size;
        total += k->size;
    }
    if (nf) {
        admin_log("pkg.downloadingPackages", "count", lerr_int(nf), "kb", lerr_int((long long)(total / 1024)), NULL);
        progress("download", 0, total);
        fetch_batch(fi, nf, fetch_progress, "download");
        for (int k = 0; k < nf; k++) {
            const IdxPkg *pk = &x->pkg[p->items[fitem[k]].rec];
            if (fi[k].status != 200 || fi[k].size != pk->size || memcmp(fi[k].sha256, pk->sha256, 32)) {
                admin_err("pkg.downloadFailed", "name", idx_s(x, pk->name), "reason", fi[k].status == 200 ? "pkg.hashMismatchIndex" : fi[k].err, NULL);
                unlink(fi[k].dest);
                free(files); free(fi); free(fitem);
                return -1;
            }
        }
    }
    free(fi);
    free(fitem);

    int lk = lock_dpkg();
    if (lk < 0) { admin_err(lk == -2 ? "pkg.dpkgBusy" : "pkg.dpkgLockFailed", NULL); free(files); return -1; }
    dpkg_pending();
    DpkgProgress pr = { 0, 0, 0, ninst + (extra_deb ? 1 : 0) };
    int rc = 0;
    if (nrem) {
        const char **argv = calloc((size_t)nrem + 4, sizeof *argv);
        int a = 0;
        argv[a++] = "/usr/bin/dpkg";
        argv[a++] = purge ? "--purge" : "--remove";
        for (int i = 0; i < p->n; i++) if (p->items[i].action == PA_REMOVE) argv[a++] = p->items[i].name;
        argv[a] = NULL;
        DpkgProgress rp = { 0, 0, 0, nrem };
        rc = run_dpkg(argv, &rp);
        free(argv);
        if (rc) admin_log("pkg.dpkgRemoveExit", "code", lerr_int(rc), NULL);
    }
    if (!rc && (ninst || extra_deb)) {
        int *order = calloc((size_t)(p->n ? p->n : 1), sizeof *order), *group = calloc((size_t)(p->n ? p->n : 1), sizeof *group);
        int no = order && group ? plan_order(x, p, order, group) : -1;
        if (no < 0) rc = -1;
        for (int start = 0; !rc && start < no;) {
            int end = start;
            while (end < no && group[end] == group[start]) end++;
            const char **argv = calloc((size_t)(end - start) + 8, sizeof *argv);
            int a = 0;
            argv[a++] = "/usr/bin/dpkg";
            argv[a++] = "--force-confdef";
            argv[a++] = "--force-confold";
            argv[a++] = "--install";
            for (int k = start; k < end; k++) argv[a++] = files[order[k]];
            argv[a] = NULL;
            rc = run_dpkg(argv, &pr);
            free(argv);
            start = end;
        }
        if (!rc && extra_deb) {
            const char *const argv[] = { "/usr/bin/dpkg", "--force-confdef", "--force-confold", "--install", extra_deb, NULL };
            rc = run_dpkg(argv, &pr);
        }
        free(order);
        free(group);
    }
    close(lk);
    if (rc) {
        admin_err("pkg.dpkgFailed", "code", lerr_int(rc), NULL);
        free(files);
        return -1;
    }
    auto_update(p);
    for (int i = 0; i < p->n; i++) if (p->items[i].rec >= 0) unlink(files[i]);   /* the cache isn't kept */
    free(files);
    int ni = 0, nu = 0, nr = 0;
    for (int i = 0; i < p->n; i++) {
        if (p->items[i].action == PA_INSTALL) ni++;
        else if (p->items[i].action == PA_UPGRADE) nu++;
        else nr++;
    }
    admin_event("pkg changed");
    if (extra_deb) ni++;
    admin_ok("pkg.done", "installed", lerr_int(ni), "upgraded", lerr_int(nu), "removed", lerr_int(nr), NULL);
    return 0;
}

static int open_db(Idx *x, Status *st, int need_index) {
    memset(st, 0, sizeof *st);
    if (idx_open(x, PKG_DIR) && need_index) { admin_err("pkg.noIndex", NULL); return -1; }
    if (status_load(st, DPKG_STATUS, pkg_native_arch())) { admin_err("pkg.dpkgStatusUnreadable", NULL); idx_close(x); return -1; }
    return 0;
}

/* List of what's installed as a dependency (for orphans). */
typedef struct { char *buf; const char **v; int n; } AutoList;

static void auto_load(AutoList *a) {
    memset(a, 0, sizeof *a);
    size_t len;
    a->buf = (char *)read_file(PKG_DIR "/auto", &len, 4 << 20);
    if (!a->buf) return;
    int cap = 1;
    for (size_t i = 0; i < len; i++) if (a->buf[i] == '\n') cap++;
    a->v = calloc((size_t)cap, sizeof *a->v);
    for (char *p = a->buf; a->v && p && *p;) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        if (*p && deb_valid_name(p, strlen(p)) && a->n < cap) a->v[a->n++] = p;
        p = nl ? nl + 1 : NULL;
    }
}

static void auto_free(AutoList *a) { free(a->buf); free(a->v); }

static void run_plan_action(const char *const *ins, int ni, const char *const *rem, int nr, const PlanOpts *o, int purge) {
    int lk = lock_pkg();
    if (lk == -2) { admin_err("pkg.busy", NULL); return; }
    if (lk < 0) { admin_err("pkg.prepareFailed", "path", PKG_DIR, NULL); return; }
    Idx x;
    Status st;
    if (open_db(&x, &st, ni > 0 || o->upgrade_all)) { close(lk); return; }
    AutoList al;
    auto_load(&al);
    PlanOpts po = *o;
    po.autos = al.v;
    po.nautos = al.n;
    Plan p;
    plan_make(&x, &st, ins, ni, rem, nr, &po, &p);
    auto_free(&al);
    say_plan(&p, &x);
    if (!p.ok) admin_say("ERR", "%s", p.err);   /* i18n-ok: already a code line */
    else if (!p.n && o->orphans && !ni && !nr) admin_ok("pkg.nothingToRemove", NULL);
    else {
        admin_say("LOG", "plan total %llu %lld", (unsigned long long)p.download, (long long)p.isize_delta);   /* i18n-ok: protocol line parsed by apps/store */
        apply_plan(&x, &p, purge, NULL);
    }
    plan_free(&p);
    status_free(&st);
    idx_close(&x);
    close(lk);
}

void pkg_act_install(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char names[64][129];
    int n, flags;
    if (parse_names(arg, len, names, 64, &n, &flags)) return;
    if (!n) { admin_err("pkg.missingName", NULL); return; }
    const char *ins[64];
    for (int i = 0; i < n; i++) ins[i] = names[i];
    PlanOpts o = { .recommends = flags & 1, .allow_remove = (flags & 2) != 0 };
    run_plan_action(ins, n, NULL, 0, &o, 0);
}

void pkg_act_remove(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char names[64][129];
    int n, flags;
    if (parse_names(arg, len, names, 64, &n, &flags)) return;
    if (!n) { admin_err("pkg.missingName", NULL); return; }
    const char *rem[64];
    for (int i = 0; i < n; i++) rem[i] = names[i];
    PlanOpts o = { .cascade = (flags & 4) != 0, .allow_remove = (flags & 6) != 0, .orphans = !(flags & 16) };
    run_plan_action(NULL, 0, rem, n, &o, (flags & 8) != 0);
}

/* Removes what was installed as a dependency that nobody uses anymore (apt autoremove). */
void pkg_act_autoremove(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    if (len) { admin_err("admin.invalidArgument", NULL); (void)arg; return; }
    PlanOpts o = { .orphans = 1 };
    run_plan_action(NULL, 0, NULL, 0, &o, 0);
}

void pkg_act_upgrade(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char names[1][129];
    int n, flags;
    if (parse_names(arg, len, names, 0, &n, &flags)) return;
    PlanOpts o = { .upgrade_all = 1, .allow_remove = (flags & 2) != 0 };
    run_plan_action(NULL, 0, NULL, 0, &o, 0);
}

/* ---- local .deb ---- */

typedef struct { char name[129], version[129], arch[32]; char *depends, *predepends, *conflicts, *breaks; } DebControl;

/* ar: "!<arch>\n" + members with 60-byte headers */
static int ar_member(const uint8_t *d, size_t len, const char *prefix, const uint8_t **out, size_t *olen, char *name, size_t nn) {
    if (len < 8 || memcmp(d, "!<arch>\n", 8)) return -1;
    size_t p = 8;
    while (p + 60 <= len) {
        char nm[17], sz[11];
        memcpy(nm, d + p, 16); nm[16] = 0;
        memcpy(sz, d + p + 48, 10); sz[10] = 0;
        size_t s = strtoul(sz, NULL, 10);
        if (p + 60 + s > len) return -1;
        char *e = nm + 15;
        while (e >= nm && (*e == ' ' || *e == '/')) *e-- = 0;
        if (!strncmp(nm, prefix, strlen(prefix))) {
            *out = d + p + 60;
            *olen = s;
            snprintf(name, nn, "%s", nm);
            return 0;
        }
        p += 60 + s + (s & 1);
    }
    return -1;
}

typedef struct { char *control; size_t len; } CtlCtx;

static int control_entry(const char *name, const uint8_t *data, size_t len, void *ud) {
    CtlCtx *c = ud;
    while (name[0] == '.' && name[1] == '/') name += 2;
    if (strcmp(name, "control") || len > (1 << 20) || c->control) return 0;
    c->control = malloc(len + 1);
    if (c->control) { memcpy(c->control, data, len); c->control[len] = 0; c->len = len; }
    return 0;
}

static int read_control(const uint8_t *deb, size_t len, DebControl *dc, char *err, size_t errlen) {
    memset(dc, 0, sizeof *dc);
    const uint8_t *m;
    size_t ml;
    char name[32];
    if (ar_member(deb, len, "debian-binary", &m, &ml, name, sizeof name) || ml < 3 || m[0] != '2') { lerr_set(err, errlen, "pkg.deb.notDeb", NULL); return -1; }
    if (ar_member(deb, len, "control.tar", &m, &ml, name, sizeof name)) { lerr_set(err, errlen, "pkg.deb.noControlTar", NULL); return -1; }
    uint8_t *tar = NULL;
    size_t tl = 0;
    if (!strcmp(name, "control.tar.xz")) { if (xz_decode(m, ml, &tar, &tl, 64u << 20)) tar = NULL; }
    else if (!strcmp(name, "control.tar.gz")) { if (gz_decode(m, ml, &tar, &tl, 64u << 20)) tar = NULL; }
    else if (!strcmp(name, "control.tar")) { tar = malloc(ml); if (tar) { memcpy(tar, m, ml); tl = ml; } }
    else { lerr_set(err, errlen, "pkg.deb.compression", "name", name, NULL); return -1; }
    if (!tar) { lerr_set(err, errlen, "pkg.deb.controlDamaged", NULL); return -1; }
    CtlCtx c = { NULL, 0 };
    tar_walk(tar, tl, control_entry, &c);
    free(tar);
    if (!c.control) { lerr_set(err, errlen, "pkg.deb.noControl", NULL); return -1; }
    D822 it = { c.control, c.control + c.len };
    D822Field f;
    while (d822_next_field(&it, &f)) {
        char **dst = NULL;
        if (d822_is(&f, "Package")) snprintf(dc->name, sizeof dc->name, "%.*s", (int)(f.vlen < 128 ? f.vlen : 128), f.value);
        else if (d822_is(&f, "Version")) snprintf(dc->version, sizeof dc->version, "%.*s", (int)(f.vlen < 128 ? f.vlen : 128), f.value);
        else if (d822_is(&f, "Architecture")) snprintf(dc->arch, sizeof dc->arch, "%.*s", (int)(f.vlen < 31 ? f.vlen : 31), f.value);
        else if (d822_is(&f, "Depends")) dst = &dc->depends;
        else if (d822_is(&f, "Pre-Depends")) dst = &dc->predepends;
        else if (d822_is(&f, "Conflicts")) dst = &dc->conflicts;
        else if (d822_is(&f, "Breaks")) dst = &dc->breaks;
        if (dst && !*dst && (*dst = malloc(f.vlen + 1))) { memcpy(*dst, f.value, f.vlen); (*dst)[f.vlen] = 0; }
    }
    free(c.control);
    if (!deb_valid_name(dc->name, strlen(dc->name)) || !deb_valid_version(dc->version, strlen(dc->version))) { lerr_set(err, errlen, "pkg.deb.badControl", NULL); return -1; }
    return 0;
}

static void control_free(DebControl *dc) { free(dc->depends); free(dc->predepends); free(dc->conflicts); free(dc->breaks); }

/* Copies the user's file without following links and with their permissions (setfsuid), into a
 * new root-owned file. That way there's no race: what gets verified and installed is the copy. */
static int copy_user_file(const AdminReq *r, const char *path, const char *dest, char *err, size_t errlen) {
    char home[PATH_MAX];
    if (!realpath(r->home, home)) { lerr_set(err, errlen, "pkg.file.badHome", NULL); return -1; }
    size_t hl = strlen(home);
    if (strncmp(path, home, hl) || path[hl] != '/' || strstr(path, "/../") || strstr(path, "/./") || strlen(path) > 1000) {
        lerr_set(err, errlen, "pkg.file.outsideHome", NULL);
        return -1;
    }
    setfsgid(r->gid);
    setfsuid(r->uid);
    int dfd = open(home, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char rel[1024];
    snprintf(rel, sizeof rel, "%s", path + hl + 1);
    char *save = NULL, *comp = strtok_r(rel, "/", &save);
    int fd = -1;
    while (dfd >= 0 && comp) {
        char *next = strtok_r(NULL, "/", &save);
        int nfd = openat(dfd, comp, (next ? O_DIRECTORY : O_NONBLOCK) | O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        close(dfd);
        dfd = -1;
        if (!next) fd = nfd; else dfd = nfd;
        comp = next;
    }
    if (dfd >= 0) close(dfd);
    setfsuid(0);
    setfsgid(0);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > (off_t)(2048u << 20)) {
        if (fd >= 0) close(fd);
        lerr_set(err, errlen, "pkg.file.openFailed", NULL);
        return -1;
    }
    unlink(dest);
    int out = open(dest, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (out < 0) { close(fd); lerr_set(err, errlen, "pkg.file.copyFailed", NULL); return -1; }
    uint8_t buf[65536];
    ssize_t n;
    int ok = 1;
    while (ok && (n = read(fd, buf, sizeof buf)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; ok = 0; break; }
        if (write(out, buf, (size_t)n) != n) ok = 0;
    }
    close(fd);
    if (close(out)) ok = 0;
    if (!ok) { unlink(dest); lerr_set(err, errlen, "pkg.file.copyFailed", NULL); return -1; }
    return 0;
}

void pkg_act_install_file(const AdminReq *r, const char *arg, size_t len) {
    char buf[1100];
    if (len >= sizeof buf) { admin_err("pkg.file.pathTooLong", NULL); return; }
    memcpy(buf, arg, len);
    buf[len] = 0;
    int allow_remove = 0;
    char *flag = strstr(buf, " --allow-remove");
    if (flag && !flag[15]) { *flag = 0; allow_remove = 1; }
    size_t pl = strlen(buf);
    if (buf[0] != '/' || pl < 5 || strcmp(buf + pl - 4, ".deb") || strchr(buf, '\n')) { admin_err("pkg.file.needDebPath", NULL); return; }
    int lk = lock_pkg();
    if (lk == -2) { admin_err("pkg.busy", NULL); return; }
    if (lk < 0 || mkdirs(PKG_CACHE, 0755)) { admin_err("pkg.prepareFailed", "path", PKG_CACHE, NULL); return; }
    char dest[PATH_MAX], err[256];
    snprintf(dest, sizeof dest, PKG_CACHE "/local-%d.deb", (int)getpid());
    if (copy_user_file(r, buf, dest, err, sizeof err)) { admin_say("ERR", "%s", err); close(lk); return; }   /* i18n-ok: already a code line */
    size_t dl;
    uint8_t *deb = read_file(dest, &dl, 2048u << 20);
    DebControl dc;
    if (!deb || read_control(deb, dl, &dc, err, sizeof err)) {
        free(deb);
        unlink(dest);
        admin_say("ERR", "%s", deb ? err : "pkg.file.unreadable");   /* i18n-ok: already a code line */
        close(lk);
        return;
    }
    free(deb);
    const char *arch = pkg_native_arch();
    if (strcmp(dc.arch, arch) && strcmp(dc.arch, "all")) {
        admin_err("pkg.deb.wrongArch", "name", dc.name, "arch", dc.arch, "system", arch, NULL);
        control_free(&dc); unlink(dest); close(lk);
        return;
    }
    admin_log("pkg.localPackage", "name", dc.name, "version", dc.version, "arch", dc.arch, NULL);
    Idx x;
    Status st;
    if (open_db(&x, &st, 0)) { control_free(&dc); unlink(dest); close(lk); return; }
    /* the .deb's dependencies: for each unmet group, the first installable alternative */
    const char *ins[64];
    char names[64][129];
    int ni = 0, missing = 0;
    for (int f = 0; f < 2; f++) {
        const char *s = f ? dc.depends : dc.predepends;
        if (!s) continue;
        const char *e = s + strlen(s);
        DebRel a[32];
        int k;
        while ((k = deb_next_group(&s, e, a, 32)) > 0) {
            int ok = 0;
            for (int j = 0; j < k && !ok; j++) {
                const StPkg *ip = status_find(&st, a[j].name, a[j].nlen);
                if (ip && ver_satisfies(ip->version, a[j].op, a[j].ver, a[j].vlen)) ok = 1;
                for (int q = 0; q < st.n && !ok; q++) {          /* provided by something installed */
                    if (!st.p[q].native || !st.p[q].provides || a[j].op != REL_NONE) continue;
                    const char *pp = st.p[q].provides, *pe = pp + strlen(pp);
                    DebRel pr[16];
                    int pk;
                    while (!ok && (pk = deb_next_group(&pp, pe, pr, 16)) > 0)
                        for (int z = 0; z < pk; z++) if (pr[z].nlen == a[j].nlen && !memcmp(pr[z].name, a[j].name, a[j].nlen)) ok = 1;
                }
            }
            for (int j = 0; j < k && !ok && ni < 64; j++) {
                long c = pkg_candidate(&x, a[j].name, a[j].nlen, status_find(&st, a[j].name, a[j].nlen));
                long lo, hi;
                idx_providers(&x, a[j].name, a[j].nlen, &lo, &hi);
                if ((c >= 0 && ver_satisfies(idx_s(&x, x.pkg[c].version), a[j].op, a[j].ver, a[j].vlen)) || (a[j].op == REL_NONE && hi > lo)) {
                    snprintf(names[ni], 129, "%.*s", (int)a[j].nlen, a[j].name);
                    ins[ni] = names[ni];
                    ni++;
                    ok = 1;
                }
            }
            if (!ok) { missing++; { char dn[130]; snprintf(dn, sizeof dn, "%.*s", (int)a[0].nlen, a[0].name); admin_log("pkg.unresolvedDep", "name", dn, NULL); } }
        }
    }
    if (missing) {
        admin_err("pkg.deb.missingDeps", "name", dc.name, NULL);
    } else {
        PlanOpts o = { .allow_remove = allow_remove };
        Plan p;
        plan_make(&x, &st, ins, ni, NULL, 0, &o, &p);
        say_plan(&p, &x);
        admin_say("LOG", "plan install %s %s %llu", dc.name, dc.version, (unsigned long long)dl);   /* i18n-ok: protocol line parsed by apps/store */
        if (!p.ok) admin_say("ERR", "%s", p.err);   /* i18n-ok: already a code line */
        else apply_plan(&x, &p, 0, dest);
        plan_free(&p);
    }
    unlink(dest);
    control_free(&dc);
    status_free(&st);
    idx_close(&x);
    close(lk);
}
