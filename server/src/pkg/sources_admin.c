/* sources_admin.c — vendor repositories from LordOS's catalog (0.8.5; Store → Artificial Intelligence,
 * docs/UPDATES.md "Mirrored apps (catalog)"). Administrators only:
 *
 *   pkg-source-add <id>      the catalog entry <id> (source type "apt", delivery "direct") becomes
 *                            /etc/apt/sources.list.d/lordos-<id>.sources and its vendor key
 *                            /usr/share/keyrings/lordos-<id>.asc
 *   pkg-source-remove <id>   removes both
 *
 * A vendor whose package adds a one-line .list unless one already names its repository (Spotify)
 * gets "optout": "<name>.list" in the catalog: an empty file of that name is created only when it
 * doesn't exist, and removed with the source only while it is still empty.
 *
 * A vendor whose package always writes its own source (Cursor: cursor.sources, Signed-By
 * /usr/share/keyrings/anysphere.gpg) would leave two entries for one repository with different
 * Signed-By, which makes apt refuse every source. Its catalog entry names those files
 * ("files": {"sources": "cursor", "keyring": "anysphere.gpg"}) and the same text is written there
 * instead, the key dearmored when the name ends in .gpg, so the package's own write changes nothing.
 *
 * Nothing comes from the caller but the id: the repository, suite, components, architectures and key
 * come from the catalog the lordos package ships (<LordOS dir>/apt/catalog.json and apt/keys/, root's,
 * not writable by others), every field is checked against a strict character set, the key file must
 * hold a primary key with exactly the catalog's fingerprint, and the files are fixed text written
 * whole and renamed. The engine then verifies that repository's InRelease with that key only (its
 * Signed-By), over HTTPS (lord-tls). */
#include "pkg.h"
#include "pgp.h"
#include "../sec/json.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SOURCES_DIR "/etc/apt/sources.list.d"
#define KEYRINGS    "/usr/share/keyrings"
#define CATALOG_MAX (512 * 1024)

/* <LordOS dir> (the folder that holds bin/lord-server) */
static int lordos_dir(char *out, size_t cap) {
    char exe[PATH_MAX];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k <= 0) return -1;
    exe[k] = 0;
    for (int i = 0; i < 2; i++) { char *s = strrchr(exe, '/'); if (!s) return -1; *s = 0; }
    return (size_t)snprintf(out, cap, "%s", exe) < cap ? 0 : -1;
}

/* a root-owned regular file not writable by others, read whole */
static char *read_root_file(const char *path, size_t max, size_t *len) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022) || (size_t)st.st_size > max) { close(fd); return NULL; }
    char *b = malloc((size_t)st.st_size + 1);
    ssize_t r = b ? read(fd, b, (size_t)st.st_size) : -1;
    close(fd);
    if (r != st.st_size) { free(b); return NULL; }
    b[r] = 0;
    *len = (size_t)r;
    return b;
}

static int good_id(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > 40) return 0;
    for (size_t i = 0; i < n; i++) if (!islower((unsigned char)s[i]) && !isdigit((unsigned char)s[i]) && s[i] != '-') return 0;
    return s[0] != '-';
}
/* a word of a deb822 field: letters, digits and ._-+~ */
static int good_word(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > 64) return 0;
    for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)s[i]) && !strchr("._-+~", s[i])) return 0;
    return 1;
}
/* https://host[/path], host and path in a strict set, no "..", no spaces */
static int good_url(const char *s) {
    if (!s || strncmp(s, "https://", 8) || strlen(s) > 200 || strstr(s, "..")) return 0;
    for (const char *p = s + 8; *p; p++) if (!isalnum((unsigned char)*p) && !strchr("-./_", *p)) return 0;
    return s[8] && s[8] != '/';
}

/* the field as one line of space-separated words (a string or an array of strings) */
static int words(const JVal *v, char *out, size_t cap) {
    out[0] = 0;
    if (!v) return -1;
    size_t o = 0;
    if (v->type == J_STR) {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", v->str);
        for (char *t = strtok(tmp, " "); t; t = strtok(NULL, " ")) {
            if (!good_word(t) || o + strlen(t) + 2 > cap) return -1;
            o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? " " : "", t);
        }
    } else if (v->type == J_ARR) {
        for (const JVal *k = v->kid; k; k = k->next) {
            if (k->type != J_STR || !good_word(k->str) || o + strlen(k->str) + 2 > cap) return -1;
            o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? " " : "", k->str);
        }
    } else return -1;
    return o ? 0 : -1;
}

static int hexval(int c) { return isdigit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1; }
/* "31DD DE24…" or "31DDDE24…" → 20 bytes */
static int parse_fpr(const char *s, uint8_t out[20]) {
    int n = 0, hi = -1;
    for (; s && *s; s++) {
        if (*s == ' ') continue;
        int v = hexval((unsigned char)*s);
        if (v < 0 || n >= 20) return -1;
        if (hi < 0) hi = v; else { out[n++] = (uint8_t)(hi << 4 | v); hi = -1; }
    }
    return n == 20 && hi < 0 ? 0 : -1;
}

static int write_atomic(const char *path, const char *data, size_t n) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.lordos", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    int ok = write(fd, data, n) == (ssize_t)n && !fchmod(fd, 0644);
    if (fsync(fd)) ok = 0;
    if (close(fd)) ok = 0;
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

/* the catalog entry for id, as the fields of its .sources and its key; 0 or an error already sent */
typedef struct { char name[80], url[201], suites[128], comps[128], arches[64], keypath[PATH_MAX], sname[48], kname[64], optout[48]; uint8_t fpr[20]; } Repo;

/* a keyring file name: lowercase letters, digits and ._-, ending in .asc or .gpg */
static int good_keyname(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 5 || n > 63 || s[0] == '.' || s[0] == '-' || (strcmp(s + n - 4, ".asc") && strcmp(s + n - 4, ".gpg"))) return 0;
    for (size_t i = 0; i < n; i++) if (!islower((unsigned char)s[i]) && !isdigit((unsigned char)s[i]) && !strchr(".-_", s[i])) return 0;
    return 1;
}

/* the paths of id's .sources and keyring: the vendor's names from the catalog, else lordos-<id> */
static void repo_paths(const char *id, const Repo *r, char *spath, size_t scap, char *kpath, size_t kcap) {
    if (r && r->sname[0]) {
        snprintf(spath, scap, SOURCES_DIR "/%s.sources", r->sname);
        snprintf(kpath, kcap, KEYRINGS "/%s", r->kname);
    } else {
        snprintf(spath, scap, SOURCES_DIR "/lordos-%s.sources", id);
        snprintf(kpath, kcap, KEYRINGS "/lordos-%s.asc", id);
    }
}
static int find_repo(const char *id, Repo *r) {
    char dir[PATH_MAX], path[PATH_MAX];
    if (lordos_dir(dir, sizeof dir)) { admin_err("pkg.catalogMissing", NULL); return -1; }
    size_t len;
    char *text = (size_t)snprintf(path, sizeof path, "%s/apt/catalog.json", dir) < sizeof path ? read_root_file(path, CATALOG_MAX, &len) : NULL;
    if (!text) { admin_err("pkg.catalogMissing", NULL); return -1; }
    JDoc doc;
    int rc = -1;
    if (json_parse(text, len, &doc, NULL, 0)) { free(text); admin_err("pkg.catalogBad", NULL); return -1; }
    const JVal *apps = json_get(doc.root, "apps");
    const JVal *a = NULL;
    for (const JVal *k = apps && apps->type == J_ARR ? apps->kid : NULL; k; k = k->next) {
        const char *kid = json_str(k, "id");
        if (kid && !strcmp(kid, id)) { a = k; break; }
    }
    const JVal *src = a ? json_get(a, "source") : NULL;
    const char *type = src ? json_str(src, "type") : NULL, *deliv = src ? json_str(src, "delivery") : NULL;
    if (!a || !src || !type || strcmp(type, "apt") || !deliv || strcmp(deliv, "direct")) { admin_err("pkg.notInCatalog", "id", id, NULL); goto out; }
    const char *url = json_str(src, "url"), *key = json_str(src, "key"), *fpr = json_str(src, "fingerprint"), *name = json_str(a, "name");
    const JVal *suite = json_get(src, "suite") ? json_get(src, "suite") : json_get(src, "codename");
    const JVal *comps = json_get(src, "components") ? json_get(src, "components") : json_get(src, "component");
    const JVal *arches = json_get(a, "arches") ? json_get(a, "arches") : json_get(src, "arches");
    const char *kb = key ? strrchr(key, '/') : NULL;
    kb = kb ? kb + 1 : key;
    int kok = kb && strlen(kb) < 64 && strlen(kb) > 4 && !strcmp(kb + strlen(kb) - 4, ".asc");
    for (const char *p = kb; kok && *p; p++) if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p) && !strchr(".-_", *p)) kok = 0;
    if (!good_url(url) || !kok || parse_fpr(fpr, r->fpr) || words(suite, r->suites, sizeof r->suites) ||
        words(comps, r->comps, sizeof r->comps) || words(arches, r->arches, sizeof r->arches)) { admin_err("pkg.catalogBad", NULL); goto out; }
    snprintf(r->url, sizeof r->url, "%s", url);
    snprintf(r->name, sizeof r->name, "%s", name ? name : id);
    for (char *p = r->name; *p; p++) if ((unsigned char)*p < 0x20 || *p == '#') *p = ' ';
    if ((size_t)snprintf(r->keypath, sizeof r->keypath, "%s/apt/keys/%s", dir, kb) >= sizeof r->keypath) { admin_err("pkg.catalogBad", NULL); goto out; }
    r->sname[0] = r->kname[0] = r->optout[0] = 0;
    /* "optout": the empty one-line list file whose presence tells the vendor's package not to add its
     * own source (Spotify: spotify.list); created only when missing, removed only when still empty */
    const char *oo = json_str(src, "optout");
    if (oo) {
        size_t on = strlen(oo);
        int ok = on > 5 && on < sizeof r->optout && !strcmp(oo + on - 5, ".list") && strncmp(oo, "lordos", 6) && islower((unsigned char)oo[0]);
        for (size_t i = 0; ok && i < on; i++) if (!islower((unsigned char)oo[i]) && !isdigit((unsigned char)oo[i]) && !strchr(".-_", oo[i])) ok = 0;
        if (!ok) { admin_err("pkg.catalogBad", NULL); goto out; }
        snprintf(r->optout, sizeof r->optout, "%s", oo);
    }
    const JVal *files = json_get(src, "files");
    if (files) {
        const char *sn = json_str(files, "sources"), *kn = json_str(files, "keyring");
        if (!good_id(sn) || !good_keyname(kn) || !strncmp(sn, "lordos", 6)) { admin_err("pkg.catalogBad", NULL); goto out; }
        snprintf(r->sname, sizeof r->sname, "%s", sn);
        snprintf(r->kname, sizeof r->kname, "%s", kn);
    }
    rc = 0;
out:
    json_free(&doc);
    free(text);
    return rc;
}

void pkg_act_source_add(const AdminReq *req, const char *arg, size_t len) {
    (void)req;
    char id[48];
    while (len && (arg[len - 1] == '\n' || arg[len - 1] == ' ')) len--;
    if (!len || len >= sizeof id) { admin_err("admin.invalidArgument", NULL); return; }
    memcpy(id, arg, len);
    id[len] = 0;
    if (!good_id(id)) { admin_err("admin.invalidArgument", NULL); return; }
    Repo r;
    if (find_repo(id, &r)) return;
    /* the key: a primary key with exactly the catalog's fingerprint */
    size_t klen;
    char *key = read_root_file(r.keypath, 64 * 1024, &klen);
    PgpKeyring kr = { 0 };
    int match = 0;
    if (key && !pgp_keyring_add_mem(&kr, key, klen))
        for (int i = 0; i < kr.n; i++) if (kr.keys[i].primary && !memcmp(kr.keys[i].fpr, r.fpr, 20)) match = 1;
    pgp_keyring_free(&kr);
    if (!match) { free(key); admin_err("pkg.keyMismatch", "id", id, NULL); return; }
    char kpath[PATH_MAX], spath[PATH_MAX], text[1200];
    repo_paths(id, &r, spath, sizeof spath, kpath, sizeof kpath);
    mkdir(KEYRINGS, 0755);
    mkdir(SOURCES_DIR, 0755);
    int bad;
    if (!strcmp(kpath + strlen(kpath) - 4, ".gpg")) {
        uint8_t *bin = NULL;
        long blen = pgp_dearmor(key, klen, &bin);
        bad = blen <= 0 || write_atomic(kpath, (const char *)bin, (size_t)blen);
        free(bin);
    } else bad = write_atomic(kpath, key, klen);
    free(key);
    if (bad) { admin_err("sys.writeFailed", "path", kpath, NULL); return; }
    int n = snprintf(text, sizeof text,
                     "# Added by LordOS (Store): %s. Removing it in the Store removes this file.\n"
                     "Types: deb\nURIs: %s\nSuites: %s\nComponents: %s\nArchitectures: %s\nSigned-By: %s\n",
                     r.name, r.url, r.suites, r.comps, r.arches, kpath);
    if (n <= 0 || n >= (int)sizeof text || write_atomic(spath, text, (size_t)n)) { unlink(kpath); admin_err("sys.writeFailed", "path", spath, NULL); return; }
    if (r.optout[0]) {
        char opath[PATH_MAX];
        snprintf(opath, sizeof opath, SOURCES_DIR "/%s", r.optout);
        int ofd = open(opath, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (ofd >= 0) close(ofd);                    /* an existing file (the user's own) stays as it is */
    }
    admin_ok("pkg.sourceAdded", "id", id, NULL);
}

void pkg_act_source_remove(const AdminReq *req, const char *arg, size_t len) {
    (void)req;
    char id[48];
    while (len && (arg[len - 1] == '\n' || arg[len - 1] == ' ')) len--;
    if (!len || len >= sizeof id) { admin_err("admin.invalidArgument", NULL); return; }
    memcpy(id, arg, len);
    id[len] = 0;
    if (!good_id(id)) { admin_err("admin.invalidArgument", NULL); return; }
    Repo r;
    if (find_repo(id, &r)) return;
    char kpath[PATH_MAX], spath[PATH_MAX];
    repo_paths(id, &r, spath, sizeof spath, kpath, sizeof kpath);
    if ((unlink(spath) && errno != ENOENT) || (unlink(kpath) && errno != ENOENT)) { admin_err("sys.writeFailed", "path", spath, NULL); return; }
    if (r.optout[0]) {
        char opath[PATH_MAX];
        struct stat ost;
        snprintf(opath, sizeof opath, SOURCES_DIR "/%s", r.optout);
        if (!lstat(opath, &ost) && S_ISREG(ost.st_mode) && ost.st_size == 0) unlink(opath);
    }
    admin_ok("pkg.sourceRemoved", "id", id, NULL);
}
