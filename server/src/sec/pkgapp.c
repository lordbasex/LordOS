/* pkgapp.c — the third app root and its check at load (see pkgapp.h, docs/SIGNING.md). */
#include "pkgapp.h"
#include "json.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void seterr(char *e, size_t n, const char *m) { if (e && n) snprintf(e, n, "%s", m); }

#ifdef LORD_SIGN_TEST
static const char *test_root;
void pkgapp_test_root(const char *path) { test_root = path; }     /* only tests/pkgapp_test.c */
#endif

const char *pkgapp_root(void) {
#ifdef LORD_SIGN_TEST
    if (test_root) return test_root;
#endif
#ifdef LORD_PKGAPP_TEST
    /* only build/lord-server-pkgtest: a root-owned folder under the test's own */
    const char *t = getenv("LORD_TEST_APPS_ROOT");
    if (t && t[0] == '/') return t;
#endif
    return PKGAPP_ROOT;
}

static int safe_dir(const char *path) {
    struct stat st;
    return !lstat(path, &st) && S_ISDIR(st.st_mode) && !(st.st_mode & (S_IWGRP | S_IWOTH)) &&
#ifdef LORD_SIGN_TEST
           (st.st_uid == 0 || st.st_uid == getuid());
#else
           st.st_uid == 0;
#endif
}

int pkgapp_dir(const char *id, char *out, size_t n) {
    char m[PATH_MAX + 32];
    if (!lpk_valid_id(id) || !safe_dir(pkgapp_root())) return -1;
    if ((size_t)snprintf(out, n, "%s/%s", pkgapp_root(), id) >= n || !safe_dir(out)) return -1;
    struct stat st;
    if ((size_t)snprintf(m, sizeof m, "%s/manifest.json", out) >= sizeof m || lstat(m, &st) || !S_ISREG(st.st_mode)) return -1;
    return 0;
}

int pkgapp_manifest(const char *dir, const char *id, PkgManifest *m) {
    memset(m, 0, sizeof *m);
    char path[PATH_MAX + 32];
    if ((size_t)snprintf(path, sizeof path, "%s/manifest.json", dir) >= sizeof path) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    char *b = malloc(PKGAPP_MANIFEST_MAX + 1);
    ssize_t n = b ? read(fd, b, PKGAPP_MANIFEST_MAX + 1) : -1;
    close(fd);
    if (n <= 0 || n > PKGAPP_MANIFEST_MAX) { free(b); return -1; }
    b[n] = 0;
    JDoc d;
    int r = -1;
    if (!json_parse(b, (size_t)n, &d, NULL, 0)) {
        const char *mid = json_str(d.root, "id"), *ver = json_str(d.root, "version"), *comp = json_str(d.root, "companion");
        const JVal *cv = json_get(d.root, "companion"), *perms = json_get(d.root, "permissions");
        if (mid && !strcmp(mid, id) && ver && strlen(ver) < sizeof m->version && (!cv || (comp && lpk_valid_path(comp)))) {
            snprintf(m->version, sizeof m->version, "%s", ver);
            snprintf(m->companion, sizeof m->companion, "%s", comp ? comp : "");
            for (const JVal *p = perms && perms->type == J_ARR ? perms->kid : NULL; p; p = p->next) {
                if (p->type != J_STR) continue;
                if (!strcmp(p->str, "network")) m->network = 1;
                else if (!strcmp(p->str, "screen-share")) m->screen = 1;
                else if (!strcmp(p->str, "remote-input")) m->input = 1;
            }
            r = 0;
        }
        json_free(&d);
    }
    free(b);
    return r;
}

int pkgapp_verify(const char *id, PkgManifest *m, LpkSigInfo *si, int *bin_fd, char *err, size_t n) {
    char dir[PATH_MAX];
    if (bin_fd) *bin_fd = -1;
    if (pkgapp_dir(id, dir, sizeof dir)) { seterr(err, n, "companion.notInstalled"); return -1; }
    if (pkgapp_manifest(dir, id, m)) { seterr(err, n, "companion.badManifest"); return -1; }
    if (bin_fd && !m->companion[0]) { seterr(err, n, "companion.none"); return -1; }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) { seterr(err, n, "companion.notInstalled"); return -1; }
    int r = sig_verify_tree(dfd, id, m->version, bin_fd ? m->companion : NULL, bin_fd, si, err, n);
    close(dfd);
    if (r) return -1;
    if (si->revoked) seterr(err, n, si->revoked_why);
    else if (si->trust != LPK_VERIFIED) seterr(err, n, "companion.notVerified");
    else if (bin_fd && *bin_fd < 0) seterr(err, n, "companion.notListed");    /* named by the manifest, not by the signature */
    else return 0;
    if (bin_fd && *bin_fd >= 0) { close(*bin_fd); *bin_fd = -1; }
    return -1;
}
