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
 * (0.8.8.9) A vendor package that ships its own source as one of its files (Valve's steam-launcher owns the
 * conffile /etc/apt/sources.list.d/steam-stable.list and /usr/share/keyrings/steam.gpg) can't be given those
 * names in advance: dpkg would find a file it doesn't own. Its entry says so
 * ("files": {"list": "steam-stable.list", "keyring": "steam.gpg", "ownedBy": "steam-launcher"}):
 * pkg-source-add writes the usual lordos-<id> bootstrap source and key for the first installation, and
 * once that package is installed with its own source naming the same repository and its own keyring
 * holding the pinned key, pkg_sources_handover() (run by the engine after dpkg and before pkg-update)
 * removes LordOS's two files, so apt never sees two entries for one repository. pkg-source-remove then
 * removes the bootstrap files if they are still there, and the vendor's list only once its package is
 * gone (removed, its configuration left) and the list names nothing but that repository.
 *
 * Nothing comes from the caller but the id: the repository, suite, components, architectures and key
 * come from the catalog the lordos package ships (<LordOS dir>/apt/catalog.json and apt/keys/, root's,
 * not writable by others), every field is checked against a strict character set, the key file must
 * hold a primary key with exactly the catalog's fingerprint, and the files are fixed text written
 * whole and renamed. The engine then verifies that repository's InRelease with that key only (its
 * Signed-By), over HTTPS (lord-tls). */
#include "pkg.h"
#include "deb.h"
#include "pgp.h"
#include "status.h"
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

/* the owner the catalog and the vendor files must have: root (pkg_test.c sets its own user for its fixture tree) */
uid_t pkg_sources_owner = 0;

/* a root-owned regular file not writable by others, read whole */
static char *read_root_file(const char *path, size_t max, size_t *len) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != pkg_sources_owner || (st.st_mode & 022) || (size_t)st.st_size > max) { close(fd); return NULL; }
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
typedef struct {
    char id[48], name[80], url[201], suites[128], comps[128], arches[64], keypath[PATH_MAX], sname[48], kname[64], optout[48];
    char lname[48], owner[129];         /* (0.8.8.9) the vendor package that owns its own source: its list file and name */
    uint8_t fpr[20];
} Repo;

/* a keyring file name: lowercase letters, digits and ._-, ending in .asc or .gpg */
static int good_keyname(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 5 || n > 63 || s[0] == '.' || s[0] == '-' || (strcmp(s + n - 4, ".asc") && strcmp(s + n - 4, ".gpg"))) return 0;
    for (size_t i = 0; i < n; i++) if (!islower((unsigned char)s[i]) && !isdigit((unsigned char)s[i]) && !strchr(".-_", s[i])) return 0;
    return 1;
}

/* a one-line list file name of the vendor's: lowercase letters, digits and ._-, ending in .list, never lordos* */
static int good_listname(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 6 || n > 47 || strcmp(s + n - 5, ".list") || !strncmp(s, "lordos", 6) || !islower((unsigned char)s[0])) return 0;
    for (size_t i = 0; i < n; i++) if (!islower((unsigned char)s[i]) && !isdigit((unsigned char)s[i]) && !strchr(".-_", s[i])) return 0;
    return 1;
}

/* the paths of id's .sources and keyring: the vendor's names from the catalog, else lordos-<id> (also the
 * bootstrap files of a vendor package that owns its own source) */
static void repo_paths(const char *id, const Repo *r, char *spath, size_t scap, char *kpath, size_t kcap) {
    if (r && r->sname[0]) {
        snprintf(spath, scap, SOURCES_DIR "/%s.sources", r->sname);
        snprintf(kpath, kcap, KEYRINGS "/%s", r->kname);
    } else {
        snprintf(spath, scap, SOURCES_DIR "/lordos-%s.sources", id);
        snprintf(kpath, kcap, KEYRINGS "/lordos-%s.asc", id);
    }
}
/* One catalog entry (app a, already known to be "apt"/"direct") as a Repo. NULL, or the error code. */
static const char *repo_from(const JVal *a, const char *dir, const char *id, Repo *r) {
    memset(r, 0, sizeof *r);
    snprintf(r->id, sizeof r->id, "%s", id);
    const JVal *src = json_get(a, "source");
    const char *url = json_str(src, "url"), *key = json_str(src, "key"), *fpr = json_str(src, "fingerprint"), *name = json_str(a, "name");
    const JVal *suite = json_get(src, "suite") ? json_get(src, "suite") : json_get(src, "codename");
    const JVal *comps = json_get(src, "components") ? json_get(src, "components") : json_get(src, "component");
    const JVal *arches = json_get(src, "arches") ? json_get(src, "arches") : json_get(a, "arches");
    const char *kb = key ? strrchr(key, '/') : NULL;
    kb = kb ? kb + 1 : key;
    int kok = kb && strlen(kb) < 64 && strlen(kb) > 4 && !strcmp(kb + strlen(kb) - 4, ".asc");
    for (const char *p = kb; kok && *p; p++) if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p) && !strchr(".-_", *p)) kok = 0;
    if (!good_url(url) || !kok || parse_fpr(fpr, r->fpr) || words(suite, r->suites, sizeof r->suites) ||
        words(comps, r->comps, sizeof r->comps) || words(arches, r->arches, sizeof r->arches)) return "pkg.catalogBad";
    snprintf(r->url, sizeof r->url, "%s", url);
    snprintf(r->name, sizeof r->name, "%s", name ? name : id);
    for (char *p = r->name; *p; p++) if ((unsigned char)*p < 0x20 || *p == '#') *p = ' ';
    if ((size_t)snprintf(r->keypath, sizeof r->keypath, "%s/apt/keys/%s", dir, kb) >= sizeof r->keypath) return "pkg.catalogBad";
    /* (0.8.8.9) "multiarch": ["i386"]: the repository also serves the 32-bit libraries an amd64 app needs
     * (Valve's steam-libs-i386); i386 is the only value, and only next to amd64 */
    const JVal *ma = json_get(src, "multiarch");
    if (ma) {
        if (ma->type != J_ARR || !ma->kid || ma->kid->next || ma->kid->type != J_STR || strcmp(ma->kid->str, "i386") ||
            strcmp(r->arches, "amd64")) return "pkg.catalogBad";
        snprintf(r->arches, sizeof r->arches, "amd64 i386");
    }
    /* "optout": the empty one-line list file whose presence tells the vendor's package not to add its
     * own source (Spotify: spotify.list); created only when missing, removed only when still empty */
    const char *oo = json_str(src, "optout");
    if (oo) {
        size_t on = strlen(oo);
        int ok = on > 5 && on < sizeof r->optout && !strcmp(oo + on - 5, ".list") && strncmp(oo, "lordos", 6) && islower((unsigned char)oo[0]);
        for (size_t i = 0; ok && i < on; i++) if (!islower((unsigned char)oo[i]) && !isdigit((unsigned char)oo[i]) && !strchr(".-_", oo[i])) ok = 0;
        if (!ok) return "pkg.catalogBad";
        snprintf(r->optout, sizeof r->optout, "%s", oo);
    }
    const JVal *files = json_get(src, "files");
    if (files) {
        const char *sn = json_str(files, "sources"), *kn = json_str(files, "keyring");
        const char *ln = json_str(files, "list"), *ob = json_str(files, "ownedBy");
        if (ob || ln) {                 /* the vendor package owns its source: bootstrap, then hand over */
            if (sn || r->optout[0] || !good_listname(ln) || !good_keyname(kn) || !strncmp(kn, "lordos", 6) ||
                !ob || strlen(ob) >= sizeof r->owner || !deb_valid_name(ob, strlen(ob))) return "pkg.catalogBad";
            snprintf(r->lname, sizeof r->lname, "%s", ln);
            snprintf(r->kname, sizeof r->kname, "%s", kn);
            snprintf(r->owner, sizeof r->owner, "%s", ob);
        } else {
            if (!good_id(sn) || !good_keyname(kn) || !strncmp(sn, "lordos", 6)) return "pkg.catalogBad";
            snprintf(r->sname, sizeof r->sname, "%s", sn);
            snprintf(r->kname, sizeof r->kname, "%s", kn);
        }
    }
    return NULL;
}

/* The catalog (<dir>/apt/catalog.json, root's), parsed; NULL with the error code in *err. */
static char *catalog_read(const char *dir, JDoc *doc, const char **err) {
    char path[PATH_MAX];
    size_t len;
    char *text = (size_t)snprintf(path, sizeof path, "%s/apt/catalog.json", dir) < sizeof path ? read_root_file(path, CATALOG_MAX, &len) : NULL;
    if (!text) { *err = "pkg.catalogMissing"; return NULL; }
    if (json_parse(text, len, doc, NULL, 0)) { free(text); *err = "pkg.catalogBad"; return NULL; }
    return text;
}

static int direct_apt(const JVal *a) {
    const JVal *src = json_get(a, "source");
    const char *type = src ? json_str(src, "type") : NULL, *deliv = src ? json_str(src, "delivery") : NULL;
    return src && type && !strcmp(type, "apt") && deliv && !strcmp(deliv, "direct");
}

static int find_repo(const char *id, Repo *r) {
    char dir[PATH_MAX];
    if (lordos_dir(dir, sizeof dir)) { admin_err("pkg.catalogMissing", NULL); return -1; }
    JDoc doc;
    const char *err = NULL;
    char *text = catalog_read(dir, &doc, &err);
    if (!text) { admin_err(err, NULL); return -1; }
    const JVal *apps = json_get(doc.root, "apps");
    const JVal *a = NULL;
    for (const JVal *k = apps && apps->type == J_ARR ? apps->kid : NULL; k; k = k->next) {
        const char *kid = json_str(k, "id");
        if (kid && !strcmp(kid, id)) { a = k; break; }
    }
    int rc = -1;
    if (!a || !direct_apt(a)) admin_err("pkg.notInCatalog", "id", id, NULL);
    else if ((err = repo_from(a, dir, id, r))) admin_err(err, NULL);
    else rc = 0;
    json_free(&doc);
    free(text);
    return rc;
}

/* ---- a vendor package's own source (0.8.8.9) ---- */

/* A root-owned regular file not writable by others, not a link: read whole (at most max), or NULL. */
static char *read_owned(const char *path, size_t max, size_t *len) { return read_root_file(path, max, len); }

/* Does a one-line list name the catalog's repository (an active "deb" line with its URL and suite)? only: and
 * nothing else (every active line, deb or deb-src, is that repository). */
static int list_names_repo(const char *text, const Repo *r, int only) {
    char url[204];
    snprintf(url, sizeof url, "%s", r->url);
    size_t ul = strlen(url);
    while (ul && url[ul - 1] == '/') url[--ul] = 0;
    char suite[128];
    snprintf(suite, sizeof suite, "%s", r->suites);
    suite[strcspn(suite, " ")] = 0;
    int named = 0, other = 0;
    for (const char *p = text; *p;) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        char line[1024];
        snprintf(line, sizeof line, "%.*s", (int)(ll < sizeof line - 1 ? ll : sizeof line - 1), p);
        p += ll + (nl ? 1 : 0);
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) continue;
        int src = !strncmp(s, "deb-src", 7) && (s[7] == ' ' || s[7] == '\t');
        if (!src && (strncmp(s, "deb", 3) || (s[3] != ' ' && s[3] != '\t'))) { other = 1; continue; }
        s += src ? 7 : 3;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') { char *c = strchr(s, ']'); if (!c) { other = 1; continue; } s = c + 1; }
        char u[400] = "", su[100] = "";
        if (sscanf(s, " %399s %99s", u, su) != 2) { other = 1; continue; }
        size_t l = strlen(u);
        while (l && u[l - 1] == '/') u[--l] = 0;
        if (strcmp(u, url) || strcmp(su, suite)) { other = 1; continue; }
        if (!src) named = 1;
    }
    return named && (!only || !other);
}

/* Does the keyring file hold a primary key with the catalog's fingerprint? */
static int keyring_has(const char *path, const Repo *r) {
    size_t kl;
    char *k = read_owned(path, 256 * 1024, &kl);
    PgpKeyring kr = { 0 };
    int ok = 0;
    if (k && !pgp_keyring_add_mem(&kr, k, kl))
        for (int i = 0; i < kr.n; i++) if (kr.keys[i].primary && !memcmp(kr.keys[i].fpr, r->fpr, 20)) ok = 1;
    pgp_keyring_free(&kr);
    free(k);
    return ok;
}

/* Is the vendor package installed (dpkg's status under root) with its own source and keyring in place? */
static int vendor_ready(const char *root, const Repo *r) {
    char p[PATH_MAX];
    Status st = { 0 };
    snprintf(p, sizeof p, "%s" DPKG_STATUS, root);
    int inst = !status_load(&st, p, pkg_native_arch()) && status_find(&st, r->owner, strlen(r->owner));
    status_free(&st);
    if (!inst) return 0;
    size_t ll;
    snprintf(p, sizeof p, "%s" SOURCES_DIR "/%s", root, r->lname);
    char *list = read_owned(p, 64 * 1024, &ll);
    int ok = list && list_names_repo(list, r, 0);
    free(list);
    snprintf(p, sizeof p, "%s" KEYRINGS "/%s", root, r->kname);
    return ok && keyring_has(p, r);
}

static int vendor_present(const char *root, const char *owner) {
    char p[PATH_MAX];
    Status st = { 0 };
    snprintf(p, sizeof p, "%s" DPKG_STATUS, root);
    int inst = !status_load(&st, p, pkg_native_arch()) && status_find(&st, owner, strlen(owner));
    status_free(&st);
    return inst;
}

/* removes LordOS's bootstrap files of r under root; 1 when something was removed */
static int bootstrap_remove(const char *root, const Repo *r) {
    char sp[PATH_MAX], kp[PATH_MAX], a[PATH_MAX], b[PATH_MAX];
    repo_paths(r->id, NULL, sp, sizeof sp, kp, sizeof kp);
    snprintf(a, sizeof a, "%s%s", root, sp);
    snprintf(b, sizeof b, "%s%s", root, kp);
    int n = !unlink(a);
    n += !unlink(b);
    return n > 0;
}

static int handover_in(const char *root, const char *dir) {
    JDoc doc;
    const char *err = NULL;
    char *text = catalog_read(dir, &doc, &err);
    if (!text) return 0;
    int done = 0;
    const JVal *apps = json_get(doc.root, "apps");
    for (const JVal *a = apps && apps->type == J_ARR ? apps->kid : NULL; a; a = a->next) {
        const char *id = json_str(a, "id");
        Repo r;
        if (!id || !good_id(id) || !direct_apt(a) || repo_from(a, dir, id, &r) || !r.owner[0]) continue;
        char sp[PATH_MAX], kp[PATH_MAX], full[PATH_MAX];
        repo_paths(r.id, NULL, sp, sizeof sp, kp, sizeof kp);
        snprintf(full, sizeof full, "%s%s", root, sp);
        if (access(full, F_OK) || !vendor_ready(root, &r)) continue;
        if (bootstrap_remove(root, &r)) {
            fprintf(stderr, "pkg: %s: %s installed its own source (%s); LordOS's bootstrap source removed\n", r.id, r.owner, r.lname);
            done++;
        }
    }
    json_free(&doc);
    free(text);
    return done;
}

void pkg_sources_handover(const char *root) {
    char dir[PATH_MAX];
    if (!lordos_dir(dir, sizeof dir)) handover_in(root ? root : "", dir);
}

/* For tests: the same with the catalog in <dir>/apt (pkg_test.c). */
int pkg_sources_handover_in(const char *root, const char *dir) { return handover_in(root, dir); }

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
    /* a vendor package with its own source already in place: nothing to add (no second entry for it) */
    if (r.owner[0] && vendor_ready("", &r)) { admin_ok("pkg.sourceAdded", "id", id, NULL); return; }
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
    if (r.owner[0] && !vendor_present("", r.owner)) {
        /* the vendor's package is gone but dpkg keeps its list (a conffile) while its keyring went with it:
         * that list is removed when it names nothing but this repository */
        char lp[PATH_MAX];
        size_t ll;
        snprintf(lp, sizeof lp, SOURCES_DIR "/%s", r.lname);
        char *list = read_owned(lp, 64 * 1024, &ll);
        if (list && list_names_repo(list, &r, 1)) unlink(lp);
        free(list);
    }
    if (r.optout[0]) {
        char opath[PATH_MAX];
        struct stat ost;
        snprintf(opath, sizeof opath, SOURCES_DIR "/%s", r.optout);
        if (!lstat(opath, &ost) && S_ISREG(ost.st_mode) && ost.st_size == 0) unlink(opath);
    }
    admin_ok("pkg.sourceRemoved", "id", id, NULL);
}
