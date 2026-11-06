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

static int fetch_one(const char *url, const char *dest, uint64_t max, FetchItem *it) {
    memset(it, 0, sizeof *it);
    if (snprintf(it->url, sizeof it->url, "%s", url) >= (int)sizeof it->url) return -1;
    snprintf(it->dest, sizeof it->dest, "%s", dest);
    it->max = max;
    fetch_batch_in(it, 1, vm_progress, NULL, VM_SCOPE);
    if (it->status != 200) { unlink(dest); return -1; }
    chmod(dest, 0644);
    return 0;
}

void vm_act_image_list(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char buf[256], *f[3];
    VmCatEntry e;
    int n = split(arg, len, buf, sizeof buf, f, 3);
    if (n < 2 || entry_of(f[0], f[1], &e)) { admin_err("admin.invalidArgument", NULL); return; }
    char url[VMCAT_URL + 160], dest[160];
    if (n == 2 && e.sums[0]) {                               /* the list has a fixed name */
        snprintf(url, sizeof url, "%s%s", e.dir, e.sums);
        snprintf(dest, sizeof dest, VM_LISTS "/%s-%s.sums", f[0], f[1]);
    } else if (n == 2) {                                     /* the folder's listing, to find the list's name in */
        snprintf(url, sizeof url, "%s", e.dir);
        snprintf(dest, sizeof dest, VM_LISTS "/%s-%s.index", f[0], f[1]);
    } else if (e.sums_re[0] && vmcat_match(e.sums_re, f[2])) {
        snprintf(url, sizeof url, "%s%s", e.dir, f[2]);
        snprintf(dest, sizeof dest, VM_LISTS "/%s-%s.sums", f[0], f[1]);
    } else { admin_err("admin.invalidArgument", NULL); return; }
    store_dirs();
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
    snprintf(lp, sizeof lp, VM_LISTS "/%s-%s.sums", f[0], f[1]);
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
    FetchItem it;
    int bad = fetch_one(url, dest, e.max_bytes, &it);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", it.sha256[i]);
    unlink(jp);
    if (!bad && strcmp(hex, sha)) { unlink(dest); bad = 2; }
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
    char p[200];
    snprintf(p, sizeof p, VM_IMAGES "/%s.iso.part", sha);
    unlink(p);
    snprintf(p, sizeof p, VM_IMAGES "/%s.lock", sha);
    unlink(p);
    admin_ok("vm.downloadCancelled", NULL);
}

/* an administrator's: the image is shared by every account's machines */
void vm_act_image_remove(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char s[65];
    if (len != 64 || strlen(arg) != 64 || !vmcat_sha_ok(arg)) { admin_err("admin.invalidArgument", NULL); return; }
    for (int i = 0; i < 64; i++) s[i] = (char)tolower((unsigned char)arg[i]);
    s[64] = 0;
    char p[200];
    snprintf(p, sizeof p, VM_IMAGES "/%s.iso", s);
    if (unlink(p) && errno != ENOENT) { admin_err("vm.removeFailed", NULL); return; }
    admin_ok("vm.imageRemoved", NULL);
}
