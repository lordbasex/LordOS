/* pkgstate.c — saved original, "Modified by you", restore, share and updates
 * (see pkgstate.h and docs/SIGNING.md). */
#include "pkgstate.h"
#include "json.h"
#include "../pkg/crypto.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void seterr(char *e, size_t n, const char *m) { if (e && n) snprintf(e, n, "%s", m); }

static int orig_dir(char *out, size_t n) {
    const char *root = lpk_root();
    if (!root) return -1;
    snprintf(out, n, "%s/.orig", root);
    if (mkdir(out, 0700) && errno != EEXIST) return -1;
    return 0;
}

int pkg_original_path(const char *id, char *out, size_t n) {
    char d[PATH_MAX];
    if (!lpk_valid_id(id) || orig_dir(d, sizeof d)) return -1;
    return (size_t)snprintf(out, n, "%s/%s.lpk", d, id) < n ? 0 : -1;
}

static int write_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t r = write(fd, c, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        c += r;
        n -= (size_t)r;
    }
    return 0;
}

static int copy_file(const char *src, const char *dst) {
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    int in = open(src, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (in < 0) return -1;
    unlink(tmp);
    int out = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    int ok = out >= 0;
    char buf[65536];
    ssize_t r;
    while (ok && (r = read(in, buf, sizeof buf)) != 0) {
        if (r < 0) { if (errno == EINTR) continue; ok = 0; break; }
        if (write_all(out, buf, (size_t)r)) ok = 0;
    }
    close(in);
    if (out >= 0 && (fsync(out) || close(out))) ok = 0;
    if (ok && rename(tmp, dst)) ok = 0;
    if (!ok) unlink(tmp);
    return ok ? 0 : -1;
}

int pkg_keep_original(const char *id, const char *lpk_path) {
    char dst[PATH_MAX];
    if (pkg_original_path(id, dst, sizeof dst)) return -1;
    return copy_file(lpk_path, dst);
}

static struct { char id[48]; unsigned long long fp; PkgStatus st; } cache[32];
static int ncache, next_slot;

void pkg_forget(const char *id) {
    char p[PATH_MAX];
    if (!pkg_original_path(id, p, sizeof p)) unlink(p);
    for (int i = 0; i < ncache; i++) if (!strcmp(cache[i].id, id)) cache[i].id[0] = 0;
}

/* ---- installed files ---- */

typedef struct { char path[224]; long long size, mtime; } DirFile;
static DirFile *walk_files;
static int walk_n, walk_cap, walk_bad;
static size_t walk_base;

static int walk_cb(const char *p, const struct stat *st, int flag, struct FTW *f) {
    (void)f;
    if (flag == FTW_D || flag == FTW_DP) return 0;
    if (strlen(p) <= walk_base) return 0;
    const char *rel = p + walk_base + 1;
    if (flag != FTW_F || !S_ISREG(st->st_mode) || !lpk_valid_path(rel)) { walk_bad = 1; }
    if (walk_n == walk_cap) {
        int cap = walk_cap ? walk_cap * 2 : 64;
        if (cap > LPK_MAX_FILES * 2) return 1;
        DirFile *n = realloc(walk_files, (size_t)cap * sizeof *n);
        if (!n) return 1;
        walk_files = n;
        walk_cap = cap;
    }
    DirFile *d = &walk_files[walk_n++];
    snprintf(d->path, sizeof d->path, "%s", rel);
    d->size = flag == FTW_F ? (long long)st->st_size : -1;
    d->mtime = (long long)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
    return 0;
}

static int list_dir(const char *dir) {
    walk_n = 0;
    walk_bad = 0;
    walk_base = strlen(dir);
    return nftw(dir, walk_cb, 16, FTW_PHYS);
}

static char *read_file(const char *path, size_t max, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0) return NULL;
    char *b = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && (size_t)st.st_size <= max && (b = malloc((size_t)st.st_size + 1))) {
        size_t got = 0;
        while (got < (size_t)st.st_size) {
            ssize_t r = read(fd, b + got, (size_t)st.st_size - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            got += (size_t)r;
        }
        if (got != (size_t)st.st_size) { free(b); b = NULL; } else { b[got] = 0; *len = got; }
    }
    close(fd);
    return b;
}

static void add_change(PkgStatus *st, const char *path, char kind) {
    if (st->nchanged < PKG_MAX_CHANGED) {
        snprintf(st->changed[st->nchanged], sizeof st->changed[0], "%s", path);
        st->change_kind[st->nchanged] = kind;
    }
    st->nchanged++;
    st->modified = 1;
}

static void manifest_version(const Lpk *pk, char *out, size_t n) {
    const LpkFile *m = lpk_find(pk, "manifest.json");
    out[0] = 0;
    JDoc d;
    if (m && !json_parse((const char *)m->data, m->size, &d, NULL, 0)) {
        const char *v = json_str(d.root, "version");
        snprintf(out, n, "%s", v ? v : "");
        json_free(&d);
    }
}

int pkg_status(const char *id, PkgStatus *st) {
    memset(st, 0, sizeof *st);
    const char *root = lpk_root();
    if (!root || !lpk_valid_id(id)) return -1;
    char dir[PATH_MAX], orig[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/%s", root, id);
    struct stat ds, os;
    if (stat(dir, &ds) || !S_ISDIR(ds.st_mode)) return -1;
    st->installed = 1;
    if (pkg_original_path(id, orig, sizeof orig) || stat(orig, &os)) return 0;      /* installed before M8 */
    st->has_original = 1;
    if (list_dir(dir)) { seterr(st->error, sizeof st->error, "pkgstate.readFailed"); return 0; }
    /* cheap fingerprint: sizes and dates of everything, plus the original */
    unsigned long long fp = 1469598103934665603ULL;
    for (int i = 0; i < walk_n; i++) {
        for (const char *p = walk_files[i].path; *p; p++) fp = (fp ^ (unsigned char)*p) * 1099511628211ULL;
        fp = (fp ^ (unsigned long long)walk_files[i].size) * 1099511628211ULL;
        fp = (fp ^ (unsigned long long)walk_files[i].mtime) * 1099511628211ULL;
    }
    fp = (fp ^ (unsigned long long)os.st_size) * 1099511628211ULL;
    fp = (fp ^ (unsigned long long)os.st_mtim.tv_sec) * 1099511628211ULL;
    for (int i = 0; i < ncache; i++)
        if (!strcmp(cache[i].id, id) && cache[i].fp == fp) { *st = cache[i].st; return 0; }

    Lpk pk;
    char err[160];
    if (lpk_open(orig, &pk, err, sizeof err)) { snprintf(st->error, sizeof st->error, "%s", err); return 0; }
    manifest_version(&pk, st->version, sizeof st->version);
    if (sig_verify_package(&pk, id, st->version, &st->sig, err, sizeof err)) snprintf(st->error, sizeof st->error, "%s", err);
    /* the installed files against the original, file by file */
    for (int i = 0; i < pk.nfiles; i++) {
        const LpkFile *f = &pk.files[i];
        int found = -1;
        for (int j = 0; j < walk_n; j++) if (!strcmp(walk_files[j].path, f->path)) { found = j; break; }
        if (found < 0) { add_change(st, f->path, 'r'); continue; }
        char p[PATH_MAX * 2];
        snprintf(p, sizeof p, "%s/%s", dir, f->path);
        size_t len = 0;
        char *data = walk_files[found].size == (long long)f->size ? read_file(p, LPK_MAX_TOTAL, &len) : NULL;
        uint8_t a[32], b[32];
        if (data) { sha256(data, len, a); sha256(f->data, f->size, b); }
        if (!data || memcmp(a, b, 32)) add_change(st, f->path, 'm');
        free(data);
    }
    for (int j = 0; j < walk_n; j++) if (!lpk_find(&pk, walk_files[j].path)) add_change(st, walk_files[j].path, 'a');
    lpk_free(&pk);
    int slot = -1;
    for (int i = 0; i < ncache; i++) if (!strcmp(cache[i].id, id) || !cache[i].id[0]) { slot = i; break; }
    if (slot < 0) { if (ncache < 32) slot = ncache++; else { slot = next_slot; next_slot = (next_slot + 1) % 32; } }
    snprintf(cache[slot].id, sizeof cache[slot].id, "%s", id);
    cache[slot].fp = fp;
    cache[slot].st = *st;
    return 0;
}

int pkg_check_update(const char *id, const LpkSigInfo *next, char *err, size_t n) {
    char orig[PATH_MAX];
    struct stat st;
    if (pkg_original_path(id, orig, sizeof orig) || stat(orig, &st)) return 0;     /* first install */
    Lpk pk;
    char e2[160], ver[40];
    if (lpk_open(orig, &pk, e2, sizeof e2)) return 0;
    manifest_version(&pk, ver, sizeof ver);
    LpkSigInfo old;
    int bad = sig_verify_package(&pk, id, ver, &old, e2, sizeof e2);
    lpk_free(&pk);
    if (bad || !old.has_signature) return 0;           /* the installed one has no valid signature: no key to require */
    if (!next->has_signature) { seterr(err, n, "pkgstate.updateUnsigned"); return -1; }
    if (strcmp(old.key, next->key)) { seterr(err, n, "pkgstate.updateOtherKey"); return -1; }
    return 0;
}

int pkg_restore(const char *id, char *err, size_t n) {
    char orig[PATH_MAX];
    if (pkg_original_path(id, orig, sizeof orig)) { seterr(err, n, "pkgstate.invalidApp"); return -1; }
    Lpk pk;
    if (lpk_open(orig, &pk, err, n)) return -1;
    char ver[40];
    manifest_version(&pk, ver, sizeof ver);
    LpkSigInfo si;
    int r = sig_verify_package(&pk, id, ver, &si, err, n);
    if (!r) r = lpk_install(&pk, id, err, n);
    lpk_free(&pk);
    for (int i = 0; i < ncache; i++) if (!strcmp(cache[i].id, id)) cache[i].id[0] = 0;
    return r;
}

/* ---- the user's key ---- */

int pkg_user_key(uint8_t seed[32], char pubhex[65], char name[128]) {
    const char *home = getenv("HOME");
    if (!home) return -1;
    char dir[PATH_MAX], path[PATH_MAX + 16];
    snprintf(dir, sizeof dir, "%s/.local/share/axia/keys", home);
    lpk_root();                                     /* creates ~/.local/share/axia */
    if (mkdir(dir, 0700) && errno != EEXIST) return -1;
    snprintf(path, sizeof path, "%s/user.json", dir);
    size_t len = 0;
    char *txt = read_file(path, 4096, &len);
    if (txt) {
        JDoc d;
        int ok = !json_parse(txt, len, &d, NULL, 0);
        const char *s = ok ? json_str(d.root, "secret") : NULL, *nm = ok ? json_str(d.root, "name") : NULL;
        int r = s && strlen(s) == 64 && !hex_decode(s, seed, 32) ? 0 : -1;
        if (!r) {
            uint8_t pub[32];
            ed25519_public(seed, pub);
            hex_encode(pub, 32, pubhex);
            snprintf(name, 128, "%s", nm ? nm : "");
        }
        if (ok) json_free(&d);
        explicit_bzero(txt, len);
        free(txt);
        return r;
    }
    if (getrandom(seed, 32, 0) != 32) return -1;
    uint8_t pub[32];
    ed25519_public(seed, pub);
    hex_encode(pub, 32, pubhex);
    struct passwd *pw = getpwuid(getuid());
    char full[128] = "";
    if (pw && pw->pw_gecos && pw->pw_gecos[0]) { snprintf(full, sizeof full, "%s", pw->pw_gecos); char *c = strchr(full, ','); if (c) *c = 0; }
    if (!full[0]) snprintf(full, sizeof full, "%s", pw ? pw->pw_name : "user");
    snprintf(name, 128, "%s", full);
    char sh[65], qn[300], out[600];
    hex_encode(seed, 32, sh);
    json_quote(qn, sizeof qn, full);
    int n = snprintf(out, sizeof out, "{\n  \"format\": 1,\n  \"alg\": \"ed25519\",\n  \"name\": %s,\n  \"public\": \"%s\",\n  \"secret\": \"%s\"\n}\n", qn, pubhex, sh);
    explicit_bzero(sh, sizeof sh);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    int r = fd >= 0 && n > 0 && !write_all(fd, out, (size_t)n) ? 0 : -1;
    if (fd >= 0) close(fd);
    explicit_bzero(out, sizeof out);
    if (!r) fprintf(stderr, "sign: own key created (%s)\n", pubhex);
    return r;
}

/* ---- share: a new .lpk (uncompressed tar) ---- */

typedef struct { unsigned char *b; size_t len, cap; int fail; } Buf;

static void put(Buf *o, const void *p, size_t n) {
    if (o->fail) return;
    if (o->len + n > o->cap) {
        size_t cap = o->cap ? o->cap : 65536;
        while (cap < o->len + n) cap *= 2;
        unsigned char *nb = realloc(o->b, cap);
        if (!nb) { o->fail = 1; return; }
        o->b = nb;
        o->cap = cap;
    }
    memcpy(o->b + o->len, p, n);
    o->len += n;
}

static void tar_add(Buf *o, const char *name, const void *data, size_t size) {
    unsigned char h[512];
    memset(h, 0, sizeof h);
    const char *base = name;
    size_t nl = strlen(name);
    if (nl > 100) {                                 /* ustar: prefix/name */
        const char *s = name + nl - 100;
        while (*s && *s != '/') s++;
        if (!*s || s - name > 155) { o->fail = 1; return; }
        memcpy(h + 345, name, (size_t)(s - name));
        base = s + 1;
    }
    memcpy(h, base, strlen(base));
    memcpy(h + 100, "0000644", 8);
    memcpy(h + 108, "0000000", 8);
    memcpy(h + 116, "0000000", 8);
    snprintf((char *)h + 124, 12, "%011lo", (unsigned long)size);
    snprintf((char *)h + 136, 12, "%011lo", (unsigned long)time(NULL));
    memset(h + 148, ' ', 8);
    h[156] = '0';
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    h[155] = ' ';
    put(o, h, 512);
    put(o, data, size);
    static const unsigned char zero[512];
    if (size % 512) put(o, zero, 512 - size % 512);
}

int pkg_share(const char *id, const char *manifest, size_t mlen, const char *out_path, char *err, size_t n) {
    const char *root = lpk_root();
    if (!root || !lpk_valid_id(id)) { seterr(err, n, "pkgstate.invalidApp"); return -1; }
    JDoc md;
    if (json_parse(manifest, mlen, &md, NULL, 0)) { seterr(err, n, "pkgstate.badManifest"); return -1; }
    const char *mid = json_str(md.root, "id"), *ver = json_str(md.root, "version"), *based = json_str(md.root, "basedOn");
    int bad = !mid || strcmp(mid, id) || !ver || !based;
    char version[40];
    snprintf(version, sizeof version, "%s", ver ? ver : "");
    json_free(&md);
    if (bad) { seterr(err, n, "pkgstate.manifestNotBasedOn"); return -1; }
    uint8_t seed[32];
    char pub[65], name[128];
    if (pkg_user_key(seed, pub, name)) { seterr(err, n, "pkgstate.keyFailed"); return -1; }
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/%s", root, id);
    if (list_dir(dir) || walk_bad) { explicit_bzero(seed, 32); seterr(err, n, "pkgstate.badFolder"); return -1; }
    Buf tar = { 0 }, contents = { 0 };
    char line[600], q[300];
    json_quote(q, sizeof q, name);
    int k = snprintf(line, sizeof line, "{\n  \"format\": 1,\n  \"id\": \"%s\",\n  \"version\": \"%s\",\n  \"signer\": { \"name\": %s, \"key\": \"%s\" },\n  \"files\": [\n", id, version, q, pub);
    put(&contents, line, (size_t)k);
    int first = 1;
    tar_add(&tar, "manifest.json", manifest, mlen);
    uint8_t h[32];
    char hx[65];
    sha256(manifest, mlen, h);
    hex_encode(h, 32, hx);
    k = snprintf(line, sizeof line, "    { \"path\": \"manifest.json\", \"size\": %zu, \"sha256\": \"%s\" }", mlen, hx);
    put(&contents, line, (size_t)k);
    first = 0;
    for (int i = 0; i < walk_n && !tar.fail; i++) {
        const char *rel = walk_files[i].path;
        if (!strcmp(rel, "manifest.json") || !strcmp(rel, SIG_CONTENTS) || !strcmp(rel, SIG_SIGNATURE)) continue;
        char p[PATH_MAX * 2];
        snprintf(p, sizeof p, "%s/%s", dir, rel);
        size_t len = 0;
        char *data = read_file(p, LPK_MAX_TOTAL, &len);
        if (!data) { tar.fail = 1; break; }
        tar_add(&tar, rel, data, len);
        sha256(data, len, h);
        hex_encode(h, 32, hx);
        k = snprintf(line, sizeof line, "%s    { \"path\": \"%s\", \"size\": %zu, \"sha256\": \"%s\" }", first ? "" : ",\n", rel, len, hx);
        put(&contents, line, (size_t)k);
        free(data);
    }
    put(&contents, "\n  ]\n}\n", 7);
    char sig[400];
    int r = -1;
    if (!tar.fail && !contents.fail && !sig_make_detached(seed, contents.b, contents.len, sig, sizeof sig)) {
        tar_add(&tar, SIG_CONTENTS, contents.b, contents.len);
        tar_add(&tar, SIG_SIGNATURE, sig, strlen(sig));
        static const unsigned char end[1024];
        put(&tar, end, sizeof end);
        int fd = tar.fail ? -1 : open(out_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd >= 0 && !write_all(fd, tar.b, tar.len) && !close(fd)) r = 0;
        else { if (fd >= 0) { close(fd); unlink(out_path); } seterr(err, n, errno == EEXIST ? "pkgstate.exists" : "pkgstate.writeFailed"); }
    } else seterr(err, n, "pkgstate.buildFailed");
    explicit_bzero(seed, sizeof seed);
    free(tar.b);
    free(contents.b);
    return r;
}
