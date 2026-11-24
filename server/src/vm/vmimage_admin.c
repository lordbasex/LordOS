/* vmimage_admin.c — Machines' downloads of systems in the admin helper: vm-image-list, vm-image-get, vm-image-cancel and
 * vm-image-remove (vm_admin.h; docs/VIRTUALIZATION.md 3.9, "The Linux download cards"). Apart from vm_admin.c so that
 * tests/vm_image_cli.c can run them like the helper does. */
#include "vm_admin.h"
#include "vmcatalog.h"
#include "vmurl.h"
#include "../pkg/http.h"
#include "../pkg/crypto.h"
#include "../sec/codesig.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>

/* ---- the systems the assistant downloads (vmcatalog.h) ----
 *
 * Everything lands in /var/lib/lordos/machines, root's and readable by all: the makers' lists in lists/, the verified
 * images in images/<sha256>.iso (one copy for every account; a name is the content's hash, so an image there is the one
 * that was checked). Root writes the bytes and compares one hash; it never parses what arrives (the app reads the lists
 * and names the file; the helper only checks that one line of the list it fetched itself says exactly that). */
#ifndef VM_CATALOG                /* tests/vm_image_cli.c builds with its own */
#define VM_CATALOG "/usr/lib/lordos/assets/machines/systems.json"
#endif
#ifndef VM_KEYS                   /* the makers' keys the catalog pins (packaging/keys/machines) */
#define VM_KEYS    "/usr/lib/lordos/assets/machines/keys"
#endif
#ifndef VM_STORE
#define VM_STORE   "/var/lib/lordos/machines"
#endif
#define VM_LISTS   VM_STORE "/lists"
#define VM_IMAGES  VM_STORE "/images"
#define VM_SCOPE   "lordos-net-machines.scope"
#define VM_LIST_MAX (1u << 20)

static int word(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 32) return 0;
    for (size_t i = 0; i < n; i++) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) return 0;
    return 1;
}

/* up to max space-separated words of arg (no control characters) into buf */
static int split(const char *arg, size_t len, char *buf, size_t bn, char **f, int max) {
    if (!len || len >= bn || strlen(arg) != len) return -1;
    memcpy(buf, arg, len);
    buf[len] = 0;
    for (size_t i = 0; i < len; i++) if ((unsigned char)buf[i] < 0x21 && buf[i] != ' ') return -1;
    int n = 0;
    for (char *s = strtok(buf, " "); s; s = strtok(NULL, " ")) { if (n == max) return -1; f[n++] = s; }
    return n;
}

static char *read_root_file(const char *path, size_t max, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    char *buf = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && st.st_size > 0 && (size_t)st.st_size <= max &&
        (buf = malloc((size_t)st.st_size + 1))) {
        ssize_t r = read(fd, buf, (size_t)st.st_size);
        if (r != st.st_size) { free(buf); buf = NULL; } else { buf[r] = 0; *len = (size_t)r; }
    }
    close(fd);
    return buf;
}

static int entry_of(const char *id, const char *arch, VmCatEntry *e) {
    size_t n = 0;
    char *cat = word(id) ? read_root_file(VM_CATALOG, VM_LIST_MAX, &n) : NULL;
    int rc = cat ? vmcat_find(cat, n, id, arch, e) : -1;
    free(cat);
    return rc;
}

static void store_dirs(void) {
    char up[sizeof VM_STORE];
    memcpy(up, VM_STORE, sizeof up);
    *strrchr(up, '/') = 0;                                   /* /var/lib/lordos */
    mkdir(up, 0755);
    mkdir(VM_STORE, 0755);
    mkdir(VM_LISTS, 0755);
    mkdir(VM_IMAGES, 0755);
    chmod(VM_STORE, 0755); chmod(VM_LISTS, 0755); chmod(VM_IMAGES, 0755);
}

static void vm_progress(uint64_t done, uint64_t total, void *ud) {
    (void)ud;
    admin_say("LOG", "progress download %llu %llu", (unsigned long long)done, (unsigned long long)total);   /* i18n-ok: protocol line parsed by apps/machines */
}

static int fetch_it(const char *url, const char *dest, uint64_t max, int resume, FetchItem *it) {
    memset(it, 0, sizeof *it);
    if (snprintf(it->url, sizeof it->url, "%s", url) >= (int)sizeof it->url) return -1;
    snprintf(it->dest, sizeof it->dest, "%s", dest);
    it->max = max;
    it->resume = resume;
    fetch_batch_in(it, 1, vm_progress, NULL, VM_SCOPE);
    if (it->status != 200) { unlink(dest); return -1; }
    chmod(dest, 0644);
    return 0;
}
static int fetch_one(const char *url, const char *dest, uint64_t max, FetchItem *it) { return fetch_it(url, dest, max, 0, it); }

/* images/<sha256>.json, root's: what the image is, for the Storage window (the content is named by its hash only) */
static void write_image_info(const char *sha, const char *id, const char *arch, const char *file, const char *from, uint64_t size) {
    char p[240], t[248];
    snprintf(p, sizeof p, VM_IMAGES "/%s.json", sha);
    snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "we");
    if (!f) return;
    /* id, arch and file are words and plain names checked against the catalog: nothing to escape */
    fprintf(f, "{\"id\": \"%s\", \"arch\": \"%s\", \"file\": \"%s\", ", id, arch, file);
    /* a person's address, checked by vmurl_check and without its query (vmurl_redact): no '"' or '\\' in it */
    if (from) fprintf(f, "\"from\": \"%s\", ", from);
    fprintf(f, "\"size\": %llu, \"at\": %lld}\n", (unsigned long long)size, (long long)time(NULL));
    if (fclose(f) || chmod(t, 0644) || rename(t, p)) unlink(t);
}

/* one source's list (and, when the maker signs it apart, its signature) checked with the entry's pinned keys: LIST_OK
 * with *body (malloc) the text that was signed, or the error the action answers with (its argument in why) */
enum { LIST_OK = 1, LIST_FAILED = 0, LIST_UNSIGNED = -1, LIST_BADSIG = -2, LIST_NOKEYS = -3 };
static int fetch_signed_list(const char *id, const char *arch, const VmCatEntry *e, const char *lurl, const char *surl,
                             char **body, size_t *bl, char *why, size_t wn) {
    char raw[160], sigp[160];
    snprintf(raw, sizeof raw, VM_LISTS "/%s-%s.fetched", id, arch);
    snprintf(sigp, sizeof sigp, VM_LISTS "/%s-%s.sig", id, arch);
    *body = NULL;
    *bl = 0;
    why[0] = 0;
    FetchItem it;
    if (fetch_one(lurl, raw, VM_LIST_MAX, &it)) { snprintf(why, wn, "%s", it.err[0] ? it.err : "?"); return LIST_FAILED; }
    int have_sig = 0;
    if (surl[0]) {
        if (!fetch_one(surl, sigp, 64 * 1024, &it)) have_sig = 1;
        else if (strcmp(it.err, "pkg.http.status status=404") && strcmp(it.err, "pkg.http.status status=410")) {   /* none published: unsigned */
            unlink(raw);
            snprintf(why, wn, "%s", it.err[0] ? it.err : "?");
            return LIST_FAILED;
        }
    }
    size_t ln = 0, sn = 0, kn = 0;
    char *list = read_root_file(raw, VM_LIST_MAX, &ln), *sig = have_sig ? read_root_file(sigp, 64 * 1024, &sn) : NULL;
    char kp[200];
    snprintf(kp, sizeof kp, VM_KEYS "/%s", e->keys);
    /* LordOS's own releases (VMCAT_SIG_LORDOS): the release key compiled into LordOS, the one the Imager checks with */
    char *keys = NULL;
    if (e->sig_type == VMCAT_SIG_LORDOS) { if ((keys = malloc(32))) { memcpy(keys, codesig_release_key, 32); kn = 32; } }
    else keys = read_root_file(kp, 1u << 20, &kn);
    unlink(raw);
    unlink(sigp);
    int rc = !list ? -2 : !keys ? -3 : vmcat_verify_list(e, keys, kn, list, ln, sig, sn, (int64_t)time(NULL), body, bl, why, wn);
    free(list); free(sig); free(keys);
    if (rc == 1) return LIST_OK;
    free(*body);
    *body = NULL;
    if (rc == -2) { snprintf(why, wn, "pkg.http.notDownloaded"); return LIST_FAILED; }
    if (rc == -3) return LIST_NOKEYS;
    if (rc == 0) return LIST_UNSIGNED;
    if (!why[0]) snprintf(why, wn, "pkg.pgp.badSignature");
    return LIST_BADSIG;
}

/* the list into lists/, from the first of the entry's sources (vmcat_list_source: LordOS's copy on lordos.org, then the
 * maker's folder) whose list verifies; only the text that was signed is kept, as lists/<id>-<arch>.list, which is all
 * vm-image-get believes. When every source fails, the answer is the last one's error. */
static void get_signed_list(const char *id, const char *arch, const VmCatEntry *e, const char *name) {
    char dest[160], tmp[176];
    snprintf(dest, sizeof dest, VM_LISTS "/%s-%s.list", id, arch);
    snprintf(tmp, sizeof tmp, "%s.tmp", dest);
    unlink(dest);                                            /* a list is only there while its last check passed */
    char old[160];
    snprintf(old, sizeof old, VM_LISTS "/%s-%s.sums", id, arch);   /* 0.8.9's first build kept unchecked lists here */
    unlink(old);
    char lurl[VMCAT_URL + 160], surl[VMCAT_URL + 160], why[200] = "?";
    char *body = NULL;
    size_t bl = 0;
    int rc = LIST_FAILED;
    for (int i = 0; rc != LIST_OK && !vmcat_list_source(e, name, i, lurl, surl, sizeof lurl); i++)
        rc = fetch_signed_list(id, arch, e, lurl, surl, &body, &bl, why, sizeof why);
    if (rc == LIST_FAILED) { admin_err("vm.listFailed", "reason", why[0] ? why : "?", NULL); return; }
    if (rc == LIST_NOKEYS) { admin_err("vm.listBadSignature", "name", e->keys, "reason", "pkg.pgp.unknownKeys count=0", NULL); return; }
    if (rc == LIST_UNSIGNED) { admin_err("vm.listUnsigned", "name", name, NULL); return; }
    if (rc == LIST_BADSIG) { admin_err("vm.listBadSignature", "name", name, "reason", why, NULL); return; }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    int ok = fd >= 0;
    for (size_t o = 0; ok && o < bl;) {
        ssize_t w = write(fd, body + o, bl - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) ok = 0; else o += (size_t)w;
    }
    if (fd >= 0 && (fsync(fd) || close(fd))) ok = 0;
    free(body);
    if (!ok || fchmodat(AT_FDCWD, tmp, 0644, 0) || rename(tmp, dest)) { unlink(tmp); admin_err("vm.listFailed", "reason", "pkg.http.writeFailed", NULL); return; }
    admin_say("OK", "%s", dest);                             /* i18n-ok: a path, parsed by apps/machines */
}

/* lists/<id>-<arch>.release: the release folder the entry's last verified list came from (an entry with "releases",
 * vmcatalog.h), which vm-image-get downloads from; written only beside a list that verified */
static void note_write(const char *id, const char *arch, const char *what, const char *rel) {
    char p[160], t[176];
    snprintf(p, sizeof p, VM_LISTS "/%s-%s.%s", id, arch, what);
    if (!rel) { unlink(p); return; }
    snprintf(t, sizeof t, "%s.tmp", p);
    int fd = open(t, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    size_t l = strlen(rel);
    if (fd < 0) return;
    int ok = write(fd, rel, l) == (ssize_t)l;
    if (close(fd) || !ok || rename(t, p)) unlink(t);
}
static void release_note(const char *id, const char *arch, const char *rel) { note_write(id, arch, "release", rel); }

/* a root-side note (lists/<id>-<arch>.<what>, root's, in root's folder): a release, or "" */
static void note_read(const char *id, const char *arch, const char *what, char *out, size_t n) {
    char p[160];
    size_t l = 0;
    snprintf(p, sizeof p, VM_LISTS "/%s-%s.%s", id, arch, what);
    char *v = read_root_file(p, 31, &l);
    out[0] = 0;
    if (v && l < n) { memcpy(out, v, l); out[l] = 0; }
    for (size_t i = 0; out[i]; i++) if (!((out[i] >= '0' && out[i] <= '9') || out[i] == '.')) { out[0] = 0; break; }
    free(v);
}

/* the rollback floor of an entry with "releases" (vmcatalog.h): the catalog's floor, raised to the newest release whose
 * list this helper ever verified (lists/<id>-<arch>.newest, root's: no file of the person's can lower it). A release
 * below it is refused by vm-image-list and vm-image-get */
static int release_allowed(const VmCatEntry *e, const char *id, const char *arch, const char *rel) {
    char newest[32];
    if (!vmcat_release_ok(e, rel)) return 0;
    note_read(id, arch, "newest", newest, sizeof newest);
    return !newest[0] || vmcat_release_cmp(rel, newest) >= 0;
}
void vm_act_image_list(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char buf[256], *f[4];
    VmCatEntry e;
    int n = split(arg, len, buf, sizeof buf, f, 4);
    if (n < 2 || entry_of(f[0], f[1], &e) || e.npins) { admin_err("admin.invalidArgument", NULL); return; }   /* pinned: the catalog is the list */
    store_dirs();
    if (e.rel_dir[0]) {
        /* an entry with "releases" (Whonix, docs/VIRTUALIZATION.md 19.6): "<id> <arch>" fetches the folder of releases'
         * listing (only names are taken from it, each checked again here); "<id> <arch> <release>" that release's folder
         * listing; "<id> <arch> <release> <list>" that list and its signature. A release below the catalog's floor, or
         * not a release's name, is refused */
        if (n >= 3 && (!release_allowed(&e, f[0], f[1], f[2]) || vmcat_release_dir(&e, f[2]))) { admin_err("admin.invalidArgument", NULL); return; }
        if (n == 4) {
            /* the list must name the very release of its folder: no older signed list beside a newer folder */
            char lrel[32];
            if (!e.sums_re[0] || !vmcat_match(e.sums_re, f[3]) || vmcat_release_in(&e, f[3], lrel, sizeof lrel) || strcmp(lrel, f[2])) {
                admin_err("admin.invalidArgument", NULL);
                return;
            }
            release_note(f[0], f[1], NULL);
            get_signed_list(f[0], f[1], &e, f[3]);
            char lp[160], newest[32];
            struct stat st;
            snprintf(lp, sizeof lp, VM_LISTS "/%s-%s.list", f[0], f[1]);
            if (!lstat(lp, &st) && S_ISREG(st.st_mode) && st.st_uid == 0) {
                release_note(f[0], f[1], f[2]);
                note_read(f[0], f[1], "newest", newest, sizeof newest);
                if (!newest[0] || vmcat_release_cmp(f[2], newest) > 0) note_write(f[0], f[1], "newest", f[2]);
            }
            return;
        }
        char url[VMCAT_URL + 160], dest[160];
        snprintf(url, sizeof url, "%s", e.dir);
        snprintf(dest, sizeof dest, VM_LISTS "/%s-%s.%s", f[0], f[1], n == 3 ? "index" : "releases");
        FetchItem it;
        if (fetch_one(url, dest, VM_LIST_MAX, &it)) { admin_err("vm.listFailed", "reason", it.err[0] ? it.err : "?", NULL); return; }
        admin_say("OK", "%s", dest);                         /* i18n-ok: a path, parsed by apps/machines */
        return;
    }
    if (n == 4) { admin_err("admin.invalidArgument", NULL); return; }
    if (n == 2 && e.sums[0]) { get_signed_list(f[0], f[1], &e, e.sums); return; }   /* the list has a fixed name */
    if (n == 3) {                                            /* a name found in the folder's listing (Fedora) */
        if (!e.sums_re[0] || !vmcat_match(e.sums_re, f[2])) { admin_err("admin.invalidArgument", NULL); return; }
        get_signed_list(f[0], f[1], &e, f[2]);
        return;
    }
    /* the folder's listing, to find the list's name in: not trusted for anything but that name, which must match the
     * entry's pattern and whose list is then checked like any other */
    char url[VMCAT_URL + 160], dest[160];
    snprintf(url, sizeof url, "%s", e.dir);
    snprintf(dest, sizeof dest, VM_LISTS "/%s-%s.index", f[0], f[1]);
    FetchItem it;
    if (fetch_one(url, dest, VM_LIST_MAX, &it)) { admin_err("vm.listFailed", "reason", it.err[0] ? it.err : "?", NULL); return; }
    admin_say("OK", "%s", dest);                             /* i18n-ok: a path, parsed by apps/machines */
}

static void image_job(const AdminReq *r, char *p, size_t n) { snprintf(p, n, "/run/lordos/vm-image-%u.job", (unsigned)r->uid); }

/* downloads url into images/<sha>.iso (vm-image-get, vm-image-get-user): at most max bytes, kept only with that SHA-256.
 * A cut or stopped download stays as <sha>.iso.part and the next try continues it; the hash is over the whole file. */
static void image_fetch(const AdminReq *r, const char *sha, const char *url, uint64_t max, int public_only,
                        const char *id, const char *arch, const char *file, const char *from) {
    char dest[200], lock[200];
    snprintf(dest, sizeof dest, VM_IMAGES "/%s.iso", sha);
    struct stat st;
    if (!lstat(dest, &st) && S_ISREG(st.st_mode) && st.st_uid == 0) { admin_say("OK", "%s", dest); return; }   /* i18n-ok: a path */
    snprintf(lock, sizeof lock, VM_IMAGES "/%s.lock", sha);
    int lfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lfd < 0 || flock(lfd, LOCK_EX | LOCK_NB)) { if (lfd >= 0) close(lfd); admin_err("vm.imageBusy", NULL); return; }
    char jp[64];
    image_job(r, jp, sizeof jp);
    mkdir("/run/lordos", 0755);
    FILE *jf = fopen(jp, "we");
    if (jf) { fprintf(jf, "%d %u %s\n", (int)getpid(), (unsigned)r->uid, sha); fclose(jf); }
    write_image_info(sha, id, arch, file, from, 0);
    char pp[208];
    snprintf(pp, sizeof pp, "%s.part", dest);
    if (!lstat(pp, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && st.st_size > 0)
        admin_log("vm.downloadResumed", "from", lerr_int((long long)st.st_size), NULL);   /* parsed by apps/machines */
    FetchItem it;
    memset(&it, 0, sizeof it);
    int bad = -1;
    if (snprintf(it.url, sizeof it.url, "%s", url) < (int)sizeof it.url) {
        snprintf(it.dest, sizeof it.dest, "%s", dest);
        it.max = max;
        it.resume = 1;
        it.public_only = public_only;
        fetch_batch_in(&it, 1, vm_progress, NULL, VM_SCOPE);
        bad = it.status != 200;
        if (bad) unlink(dest); else chmod(dest, 0644);
    }
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", it.sha256[i]);
    unlink(jp);
    if (!bad && strcmp(hex, sha)) { unlink(dest); bad = 2; }
    if (!bad) write_image_info(sha, id, arch, file, from, it.size);
    unlink(lock);
    close(lfd);
    if (bad == 2) { admin_err("vm.hashMismatch", NULL); return; }
    if (bad) { admin_err("vm.downloadFailed", "reason", it.err[0] ? it.err : "?", NULL); return; }
    admin_say("OK", "%s", dest);                             /* i18n-ok: a path, parsed by apps/machines */
}

/* a machine group's archive (an entry with "hash": "sha512", vmcatalog.h; docs/VIRTUALIZATION.md 19.6): the download
 * goes to images/<sha512>.tar.xz, the name from the entry's kind, never from JS; kept only when the whole file's SHA-512
 * is the one the verified list gives for that name. Root hashes the bytes; it never parses the archive (lord-vmunpack,
 * sandboxed, does, in the person's lord-vm) */
static int sha512_file(const char *path, char out[129]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    Sha512 h;
    sha512_init(&h);
    static uint8_t b[1 << 20];
    for (;;) {
        ssize_t k = read(fd, b, sizeof b);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) { close(fd); return -1; }
        if (!k) break;
        sha512_update(&h, b, (size_t)k);
    }
    close(fd);
    uint8_t d[64];
    sha512_final(&h, d);
    for (int i = 0; i < 64; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
    return 0;
}

/* a verified archive in the store: images/<sha512>.tar.xz, root's, one link, 0644. Only archive_fetch makes that name,
 * by rename after the SHA-512 matched; the download itself never has it (it is <sha512>.tar.xz.dl until checked) */
static int archive_verified(const char *path) {
    struct stat st;
    return !lstat(path, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && st.st_nlink == 1 && (st.st_mode & 07777) == 0644;
}

static void archive_fetch(const AdminReq *r, const char *sha, const char *url, uint64_t max, const char *id, const char *arch, const char *file) {
    char dest[240], tmp[248], lock[240];
    snprintf(dest, sizeof dest, VM_IMAGES "/%s.tar.xz", sha);
    snprintf(tmp, sizeof tmp, "%s.dl", dest);
    snprintf(lock, sizeof lock, VM_IMAGES "/%s.lock", sha);
    /* the lock first: no answer about the name while another download of it may be renaming */
    int lfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lfd < 0 || flock(lfd, LOCK_EX | LOCK_NB)) { if (lfd >= 0) close(lfd); admin_err("vm.imageBusy", NULL); return; }
    if (archive_verified(dest)) { close(lfd); admin_say("OK", "%s", dest); return; }   /* i18n-ok: a path */
    unlink(dest);                                            /* anything else at the final name isn't trusted */
    char jp[64];
    image_job(r, jp, sizeof jp);
    mkdir("/run/lordos", 0755);
    FILE *jf = fopen(jp, "we");
    if (jf) { fprintf(jf, "%d %u %s\n", (int)getpid(), (unsigned)r->uid, sha); fclose(jf); }
    write_image_info(sha, id, arch, file, NULL, 0);
    char pp[256];
    struct stat st;
    snprintf(pp, sizeof pp, "%s.part", tmp);
    if (!lstat(pp, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && st.st_size > 0)
        admin_log("vm.downloadResumed", "from", lerr_int((long long)st.st_size), NULL);   /* parsed by apps/machines */
    FetchItem it;
    memset(&it, 0, sizeof it);
    int bad = -1;
    if (snprintf(it.url, sizeof it.url, "%s", url) < (int)sizeof it.url && strlen(tmp) < sizeof it.dest) {
        snprintf(it.dest, sizeof it.dest, "%s", tmp);       /* the fetcher writes <tmp>.part and renames it to <tmp> */
        it.max = max;
        it.resume = 1;
        fetch_batch_in(&it, 1, vm_progress, NULL, VM_SCOPE);
        bad = it.status != 200;
    }
    unlink(jp);
    char hex[129] = "";
    /* hashed under its temporary name, 0600; the final name only by rename after a match, and 0644 only then */
    if (!bad && (chmod(tmp, 0600) || sha512_file(tmp, hex) || strcmp(hex, sha))) bad = 2;
    if (!bad && rename(tmp, dest)) bad = 3;
    if (!bad && chmod(dest, 0644)) { unlink(dest); bad = 3; }
    if (bad) unlink(tmp);
    else write_image_info(sha, id, arch, file, NULL, it.size);
    unlink(lock);
    close(lfd);
    if (bad == 2) { admin_err("vm.hashMismatch", NULL); return; }
    if (bad == 3) { admin_err("vm.downloadFailed", "reason", "pkg.http.writeFailed", NULL); return; }
    if (bad) { admin_err("vm.downloadFailed", "reason", it.err[0] ? it.err : "?", NULL); return; }
    admin_say("OK", "%s", dest);                             /* i18n-ok: a path, parsed by apps/machines */
}

void vm_act_image_get(const AdminReq *r, const char *arg, size_t len) {
    char buf[400], *f[4];
    VmCatEntry e;
    int n = split(arg, len, buf, sizeof buf, f, 4);
    if (n != 4 || entry_of(f[0], f[1], &e) || !vmcat_hash_ok(&e, f[3])) { admin_err("admin.invalidArgument", NULL); return; }
    if (e.hash == VMCAT_SHA512) {
        /* the release folder the verified list came from (vm-image-list), the name a line of that list gives */
        char rp[160], rel[40] = "", lp[160], hex[129];
        snprintf(rp, sizeof rp, VM_LISTS "/%s-%s.release", f[0], f[1]);
        snprintf(lp, sizeof lp, VM_LISTS "/%s-%s.list", f[0], f[1]);
        size_t rl = 0, sl = 0;
        char *rv = e.rel_dir[0] ? read_root_file(rp, 39, &rl) : NULL;
        if (rv) { memcpy(rel, rv, rl); rel[rl] = 0; free(rv); }
        /* the archive must name the release its verified list came from, and that release must still be allowed */
        char frel[32];
        if (!vmcat_match(e.file_re, f[2]) ||
            (e.rel_dir[0] && (!release_allowed(&e, f[0], f[1], rel) || vmcat_release_dir(&e, rel) ||
                              vmcat_release_in(&e, f[2], frel, sizeof frel) || strcmp(frel, rel)))) { admin_err("vm.notListed", NULL); return; }
        for (int i = 0; i < 128; i++) hex[i] = (char)tolower((unsigned char)f[3][i]);
        hex[128] = 0;
        char *sums = read_root_file(lp, VM_LIST_MAX, &sl);
        int listed = sums && vmcat_sums_has(sums, sl, f[2], hex);
        free(sums);
        if (!listed) { admin_err("vm.notListed", NULL); return; }
        char url[VMCAT_URL + 160];
        snprintf(url, sizeof url, "%s%s", e.file_dir, f[2]);
        store_dirs();
        archive_fetch(r, hex, url, e.max_bytes, f[0], f[1], f[2]);
        return;
    }
    char sha[65];
    for (int i = 0; i < 64; i++) sha[i] = (char)tolower((unsigned char)f[3][i]);
    sha[64] = 0;
    int pin = -1;
    if (e.npins) {
        /* a pinned entry (vmcatalog.h): the name and hash must be one of the catalog's own files */
        pin = vmcat_pin_find(&e, f[2], sha);
        if (pin < 0) { admin_err("vm.notListed", NULL); return; }
        e.max_bytes = e.pin[pin].bytes;
    } else {
        if (!vmcat_match(e.file_re, f[2])) { admin_err("admin.invalidArgument", NULL); return; }
        /* the name and hash must be a line of the list this helper fetched for that entry */
        char lp[160];
        snprintf(lp, sizeof lp, VM_LISTS "/%s-%s.list", f[0], f[1]);   /* written only after its signature verified */
        size_t sl = 0;
        char *sums = read_root_file(lp, VM_LIST_MAX, &sl);
        int listed = sums && vmcat_sums_has(sums, sl, f[2], sha);
        free(sums);
        if (!listed) { admin_err("vm.notListed", NULL); return; }
    }
    char url[VMCAT_URL + 160];
    if (pin >= 0) snprintf(url, sizeof url, "%s", e.pin[pin].url);
    else snprintf(url, sizeof url, "%s%s", e.file_dir, f[2]);
    store_dirs();
    image_fetch(r, sha, url, e.max_bytes, 0, f[0], f[1], f[2], NULL);
}

/* a person's system (vm-image-get-user): a small file of its signature (the detached signature, the checksum file it
 * signs) from an address checked like the image's, into lists/u-<sha>.<kind>, root's; refused or not found: -1 */
static int fetch_user_small(const char *url, const char *sha, const char *kind, uint64_t max, FetchItem *it) {
    char dest[200];
    snprintf(dest, sizeof dest, VM_LISTS "/u-%s.%s", sha, kind);
    unlink(dest);                                            /* only there while the last download of it worked */
    memset(it, 0, sizeof *it);
    snprintf(it->url, sizeof it->url, "%s", url);
    snprintf(it->dest, sizeof it->dest, "%s", dest);
    it->max = max;
    it->public_only = 1;
    fetch_batch_in(it, 1, NULL, NULL, VM_SCOPE);
    if (it->status != 200) { unlink(dest); return -1; }
    chmod(dest, 0644);
    return 0;
}

/* vm-image-get-user "<https URL> <sha256> <size|-> [<signature URL> <checksum file URL>]" (vm_admin.h; approved by the
 * maintainer on 2026-11-22): a system the person added in Machines settings → Systems. Every address passes
 * vmurl_check (https, a public DNS name, port 443, no user name, a plain path), and the downloader keeps to public
 * addresses and those rules through every redirect (FetchItem.public_only). The image goes to images/<sha256>.iso like
 * a catalog one, at most 16 GiB (or its stated size) and only while the store keeps VMURL_RESERVE free, and is kept
 * only with that SHA-256. The signature files are only stored for lord-pgpv (lord-server checks them, never root). */
void vm_act_image_get_user(const AdminReq *r, const char *arg, size_t len) {
    char buf[3 * VMURL_MAX + 160], *f[5];
    int n = split(arg, len, buf, sizeof buf, f, 5);
    char host[256], file[128], h2[256], f2[128];
    uint64_t size = 0;
    if ((n != 3 && n != 5) || vmurl_check(f[0], host, sizeof host, file, sizeof file) || !vmcat_sha_ok(f[1]) ||
        vmurl_size(f[2], &size) ||
        (n == 5 && (vmurl_check(f[3], h2, sizeof h2, f2, sizeof f2) || vmurl_check(f[4], h2, sizeof h2, f2, sizeof f2)))) {
        admin_err("vm.urlRefused", NULL);
        return;
    }
    char sha[65];
    for (int i = 0; i < 64; i++) sha[i] = (char)tolower((unsigned char)f[1][i]);
    sha[64] = 0;
    store_dirs();
    if (n == 5) {
        FetchItem it;
        if (fetch_user_small(f[3], sha, "sig", VMURL_SIG_MAX, &it) || fetch_user_small(f[4], sha, "sums", VMURL_SUMS_MAX, &it)) {
            admin_err("vm.sigFetchFailed", "reason", it.err[0] ? it.err : "?", NULL);
            return;
        }
    }
    char dest[200], pp[208];
    snprintf(dest, sizeof dest, VM_IMAGES "/%s.iso", sha);
    snprintf(pp, sizeof pp, "%s.part", dest);
    struct stat st;
    if (!lstat(dest, &st) && S_ISREG(st.st_mode) && st.st_uid == 0) { admin_say("OK", "%s", dest); return; }   /* i18n-ok: a path */
    uint64_t part = !lstat(pp, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 ? (uint64_t)st.st_size : 0;
    struct statvfs vf;
    uint64_t free_bytes = !statvfs(VM_IMAGES, &vf) ? (uint64_t)vf.f_bavail * vf.f_frsize : 0, need = 0;
    uint64_t max = vmurl_room(size, part, free_bytes, &need);
    if (!max) {
        admin_err("vm.noSpace", "need", lerr_int((long long)need), "free", lerr_int((long long)free_bytes), NULL);
        return;
    }
    char from[VMURL_MAX + 16];
    vmurl_redact(f[0], from, sizeof from);
    image_fetch(r, sha, f[0], max, 1, "user", "", file, from);
}

void vm_act_image_cancel(const AdminReq *r, const char *arg, size_t len) {
    (void)arg;
    if (len) { admin_err("admin.invalidArgument", NULL); return; }
    char jp[64], sha[140] = "", cp[64], comm[32] = "";
    image_job(r, jp, sizeof jp);
    FILE *f = fopen(jp, "re");
    int pid = 0;
    unsigned uid = 0;
    int ok = f && fscanf(f, "%d %u %139s", &pid, &uid, sha) == 3;
    if (f) fclose(f);
    VmCatEntry e512 = { .hash = VMCAT_SHA512 };
    if (!ok || pid <= 1 || uid != r->uid || (!vmcat_sha_ok(sha) && !vmcat_hash_ok(&e512, sha))) { admin_err("vm.nothingRunning", NULL); return; }
    snprintf(cp, sizeof cp, "/proc/%d/comm", pid);
    FILE *c = fopen(cp, "re");
    if (c) { if (!fgets(comm, sizeof comm, c)) comm[0] = 0; fclose(c); }
    unlink(jp);
    if (strncmp(comm, "lord-admin", 10)) { admin_err("vm.nothingRunning", NULL); return; }
    kill(pid, SIGTERM);
    for (int i = 0; i < 40 && !kill(pid, 0); i++) usleep(50000);
    /* the part stays (<sha256>.iso.part): asking for the image again continues it; Storage removes it */
    char p[240];
    snprintf(p, sizeof p, VM_IMAGES "/%s.lock", sha);
    unlink(p);
    admin_ok("vm.downloadCancelled", NULL);
}

/* 1 if a process has path open (QEMU with it in a machine's CD drive, whoever's machine that is) */
static int open_by_someone(const char *path) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    int busy = 0;
    for (struct dirent *pe; !busy && (pe = readdir(d));) {
        if (pe->d_name[0] < '1' || pe->d_name[0] > '9') continue;
        char fdd[300];
        snprintf(fdd, sizeof fdd, "/proc/%.32s/fd", pe->d_name);
        DIR *fds = opendir(fdd);
        if (!fds) continue;
        for (struct dirent *fe; !busy && (fe = readdir(fds));) {
            char lp[400], to[256];
            if (fe->d_name[0] == '.') continue;
            snprintf(lp, sizeof lp, "%s/%.32s", fdd, fe->d_name);
            ssize_t n = readlink(lp, to, sizeof to - 1);
            if (n > 0) { to[n] = 0; busy = !strcmp(to, path); }
        }
        closedir(fds);
    }
    closedir(d);
    return busy;
}

/* an administrator's: the image is shared by every account's machines. Refused while a running machine has it in its
 * CD drive (any account's: the files QEMU holds open); a part of a stopped download goes with it. */
void vm_act_image_remove(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char s[129];
    VmCatEntry e512 = { .hash = VMCAT_SHA512 };
    int big = len == 128;                                    /* a machine group's archive, images/<sha512>.tar.xz */
    if ((len != 64 && len != 128) || strlen(arg) != len || (big ? !vmcat_hash_ok(&e512, arg) : !vmcat_sha_ok(arg))) { admin_err("admin.invalidArgument", NULL); return; }
    for (size_t i = 0; i < len; i++) s[i] = (char)tolower((unsigned char)arg[i]);
    s[len] = 0;
    char p[240], lock[240];
    snprintf(p, sizeof p, VM_IMAGES "/%s.%s", s, big ? "tar.xz" : "iso");
    snprintf(lock, sizeof lock, VM_IMAGES "/%s.lock", s);
    int lfd = open(lock, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (lfd >= 0 && flock(lfd, LOCK_EX | LOCK_NB)) { close(lfd); admin_err("vm.imageBusy", NULL); return; }   /* downloading */
    if (open_by_someone(p)) { if (lfd >= 0) close(lfd); admin_err("vm.imageInUse", NULL); return; }
    int rc = unlink(p) && errno != ENOENT;
    char q[248];
    snprintf(q, sizeof q, "%s.part", p);
    if (unlink(q) && errno != ENOENT) rc = 1;
    if (big) {
        /* an archive's download under its temporary name (archive_fetch) */
        snprintf(q, sizeof q, "%s.dl", p);
        if (unlink(q) && errno != ENOENT) rc = 1;
        snprintf(q, sizeof q, "%s.dl.part", p);
        if (unlink(q) && errno != ENOENT) rc = 1;
    }
    snprintf(q, sizeof q, VM_IMAGES "/%s.json", s);
    unlink(q);
    if (lfd >= 0) { unlink(lock); close(lfd); }
    if (rc) { admin_err("vm.removeFailed", NULL); return; }
    admin_ok("vm.imageRemoved", NULL);
}
