/* vmimage_admin.c — Machines' downloads of systems in the admin helper: vm-image-list, vm-image-get, vm-image-cancel and
 * vm-image-remove (vm_admin.h; docs/VIRTUALIZATION.md 3.9, "The Linux download cards"). Apart from vm_admin.c so that
 * tests/vm_image_cli.c can run them like the helper does. */
#include "vm_admin.h"
#include "vmcatalog.h"
#include "../pkg/http.h"

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
static void write_image_info(const char *sha, const char *id, const char *arch, const char *file, uint64_t size) {
    char p[200], t[208];
    snprintf(p, sizeof p, VM_IMAGES "/%s.json", sha);
    snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "we");
    if (!f) return;
    /* id, arch and file are words and plain names checked against the catalog: nothing to escape */
    fprintf(f, "{\"id\": \"%s\", \"arch\": \"%s\", \"file\": \"%s\", \"size\": %llu, \"at\": %lld}\n", id, arch, file,
            (unsigned long long)size, (long long)time(NULL));
    if (fclose(f) || chmod(t, 0644) || rename(t, p)) unlink(t);
}

/* the list (and, when the maker signs it apart, its signature) into lists/, checked with the entry's pinned keys; only
 * the text that was signed is kept, as lists/<id>-<arch>.list, which is all vm-image-get believes */
static void get_signed_list(const char *id, const char *arch, const VmCatEntry *e, const char *name) {
    char url[VMCAT_URL + 160], raw[160], sigp[160], dest[160], tmp[176];
    snprintf(raw, sizeof raw, VM_LISTS "/%s-%s.fetched", id, arch);
    snprintf(sigp, sizeof sigp, VM_LISTS "/%s-%s.sig", id, arch);
    snprintf(dest, sizeof dest, VM_LISTS "/%s-%s.list", id, arch);
    snprintf(tmp, sizeof tmp, "%s.tmp", dest);
    unlink(dest);                                            /* a list is only there while its last check passed */
    char old[160];
    snprintf(old, sizeof old, VM_LISTS "/%s-%s.sums", id, arch);   /* 0.8.9's first build kept unchecked lists here */
    unlink(old);
    FetchItem it;
    snprintf(url, sizeof url, "%s%s", e->dir, name);
    if (fetch_one(url, raw, VM_LIST_MAX, &it)) { admin_err("vm.listFailed", "reason", it.err[0] ? it.err : "?", NULL); return; }
    int have_sig = 0;
    if (e->sig_type == VMCAT_SIG_DETACHED) {
        snprintf(url, sizeof url, "%s%s", e->dir, e->sig_name);
        if (!fetch_one(url, sigp, 64 * 1024, &it)) have_sig = 1;
        else if (strcmp(it.err, "pkg.http.status status=404") && strcmp(it.err, "pkg.http.status status=410")) {   /* none published: unsigned */ unlink(raw); admin_err("vm.listFailed", "reason", it.err[0] ? it.err : "?", NULL); return; }
    }
    size_t ln = 0, sn = 0, kn = 0;
    char *list = read_root_file(raw, VM_LIST_MAX, &ln), *sig = have_sig ? read_root_file(sigp, 64 * 1024, &sn) : NULL;
    char kp[200];
    snprintf(kp, sizeof kp, VM_KEYS "/%s", e->keys);
    char *keys = read_root_file(kp, 1u << 20, &kn);
    unlink(raw);
    unlink(sigp);
    char *body = NULL, why[200] = "";
    size_t bl = 0;
    int rc = !list ? -2 : !keys ? -3 : vmcat_verify_list(e, keys, kn, list, ln, sig, sn, (int64_t)time(NULL), &body, &bl, why, sizeof why);
    free(list); free(sig); free(keys);
    if (rc == -2) { admin_err("vm.listFailed", "reason", "pkg.http.notDownloaded", NULL); return; }
    if (rc == -3) { admin_err("vm.listBadSignature", "name", e->keys, "reason", "pkg.pgp.unknownKeys count=0", NULL); return; }
    if (rc == 0) { free(body); admin_err("vm.listUnsigned", "name", name, NULL); return; }
    if (rc < 0) { free(body); admin_err("vm.listBadSignature", "name", name, "reason", why[0] ? why : "pkg.pgp.badSignature", NULL); return; }
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

void vm_act_image_list(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char buf[256], *f[3];
    VmCatEntry e;
    int n = split(arg, len, buf, sizeof buf, f, 3);
    if (n < 2 || entry_of(f[0], f[1], &e)) { admin_err("admin.invalidArgument", NULL); return; }
    store_dirs();
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

void vm_act_image_get(const AdminReq *r, const char *arg, size_t len) {
    char buf[320], *f[4];
    VmCatEntry e;
    int n = split(arg, len, buf, sizeof buf, f, 4);
    if (n != 4 || entry_of(f[0], f[1], &e) || !vmcat_match(e.file_re, f[2]) || !vmcat_sha_ok(f[3])) { admin_err("admin.invalidArgument", NULL); return; }
    char sha[65];
    for (int i = 0; i < 64; i++) sha[i] = (char)tolower((unsigned char)f[3][i]);
    sha[64] = 0;
    /* the name and hash must be a line of the list this helper fetched for that entry */
    char lp[160];
    snprintf(lp, sizeof lp, VM_LISTS "/%s-%s.list", f[0], f[1]);   /* written only after its signature verified */
    size_t sl = 0;
    char *sums = read_root_file(lp, VM_LIST_MAX, &sl);
    int listed = sums && vmcat_sums_has(sums, sl, f[2], sha);
    free(sums);
    if (!listed) { admin_err("vm.notListed", NULL); return; }
    store_dirs();
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
    char url[VMCAT_URL + 160];
    snprintf(url, sizeof url, "%s%s", e.file_dir, f[2]);
    write_image_info(sha, f[0], f[1], f[2], 0);
    /* a cut download is kept as <sha256>.iso.part (root's, next to its final name) and continued from there; the hash
     * is over the whole file all the same, and a part that turns out wrong is deleted with it */
    char pp[208];
    snprintf(pp, sizeof pp, "%s.part", dest);
    if (!lstat(pp, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && st.st_size > 0)
        admin_log("vm.downloadResumed", "from", lerr_int((long long)st.st_size), NULL);   /* parsed by apps/machines */
    FetchItem it;
    int bad = fetch_it(url, dest, e.max_bytes, 1, &it);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", it.sha256[i]);
    unlink(jp);
    if (!bad && strcmp(hex, sha)) { unlink(dest); bad = 2; }
    if (!bad) write_image_info(sha, f[0], f[1], f[2], it.size);
    unlink(lock);
    close(lfd);
    if (bad == 2) { admin_err("vm.hashMismatch", NULL); return; }
    if (bad) { admin_err("vm.downloadFailed", "reason", it.err[0] ? it.err : "?", NULL); return; }
    admin_say("OK", "%s", dest);                             /* i18n-ok: a path, parsed by apps/machines */
}

void vm_act_image_cancel(const AdminReq *r, const char *arg, size_t len) {
    (void)arg;
    if (len) { admin_err("admin.invalidArgument", NULL); return; }
    char jp[64], sha[80] = "", cp[64], comm[32] = "";
    image_job(r, jp, sizeof jp);
    FILE *f = fopen(jp, "re");
    int pid = 0;
    unsigned uid = 0;
    int ok = f && fscanf(f, "%d %u %79s", &pid, &uid, sha) == 3;
    if (f) fclose(f);
    if (!ok || pid <= 1 || uid != r->uid || !vmcat_sha_ok(sha)) { admin_err("vm.nothingRunning", NULL); return; }
    snprintf(cp, sizeof cp, "/proc/%d/comm", pid);
    FILE *c = fopen(cp, "re");
    if (c) { if (!fgets(comm, sizeof comm, c)) comm[0] = 0; fclose(c); }
    unlink(jp);
    if (strncmp(comm, "lord-admin", 10)) { admin_err("vm.nothingRunning", NULL); return; }
    kill(pid, SIGTERM);
    for (int i = 0; i < 40 && !kill(pid, 0); i++) usleep(50000);
    /* the part stays (<sha256>.iso.part): asking for the image again continues it; Storage removes it */
    char p[200];
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
    char s[65];
    if (len != 64 || strlen(arg) != 64 || !vmcat_sha_ok(arg)) { admin_err("admin.invalidArgument", NULL); return; }
    for (int i = 0; i < 64; i++) s[i] = (char)tolower((unsigned char)arg[i]);
    s[64] = 0;
    char p[200], lock[200];
    snprintf(p, sizeof p, VM_IMAGES "/%s.iso", s);
    snprintf(lock, sizeof lock, VM_IMAGES "/%s.lock", s);
    int lfd = open(lock, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (lfd >= 0 && flock(lfd, LOCK_EX | LOCK_NB)) { close(lfd); admin_err("vm.imageBusy", NULL); return; }   /* downloading */
    if (open_by_someone(p)) { if (lfd >= 0) close(lfd); admin_err("vm.imageInUse", NULL); return; }
    int rc = unlink(p) && errno != ENOENT;
    char q[208];
    snprintf(q, sizeof q, "%s.part", p);
    if (unlink(q) && errno != ENOENT) rc = 1;
    snprintf(q, sizeof q, VM_IMAGES "/%s.json", s);
    unlink(q);
    if (lfd >= 0) { unlink(lock); close(lfd); }
    if (rc) { admin_err("vm.removeFailed", NULL); return; }
    admin_ok("vm.imageRemoved", NULL);
}
