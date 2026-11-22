/* studio_bg_admin.c — LordOS Studio's backgrounds on disk (docs/CAMERA-STUDIO-PLAN.md 6.2; AGENTS.md, E-S4). Closed
 * actions of the admin helper, the twins of wallpaper-get and wallpaper-remove (wallpaper_admin.c):
 *
 *   studio-bg-get "<id> <variant>"     anyone: downloads one file of the catalog to /var/lib/lordos/studio/<sha256>.jpg
 *                                      (a picture) or .mp4 (a video); answers its sha256
 *   studio-bg-remove "<id> <variant>"  an administrator: deletes it
 *
 * What is fetched comes only from the catalog in the package (assets/studio/backgrounds.json), read here as root: JS
 * names an entry, never a URL or a path. The base must be the project's GitHub owner on raw.githubusercontent.com, the
 * bytes go through fetch_batch (a child as nobody, in the Store's scope, so the firewall sees lordos:store), and a file
 * whose size or SHA-256 isn't the catalog's is never kept. lord-server then opens the file read-only (media/mstudio.c,
 * root-owned files of that folder only) and gives lord-studio the fd. */
#include "hw_admin.h"
#include "studio_bg_find.h"
#include "../admin.h"
#include "../pkg/crypto.h"
#include "../pkg/http.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef STUDIO_BG_CATALOG
#define STUDIO_BG_CATALOG "/usr/lib/lordos/assets/studio/backgrounds.json"
#endif
#ifndef STUDIO_BG_STORE
#define STUDIO_BG_STORE "/var/lib/lordos/studio"
#endif
#ifndef STUDIO_BG_OWNER_OK                              /* the tests run as a user: their catalog is theirs */
#define STUDIO_BG_OWNER_OK(st) ((st).st_uid == 0)
#endif

static int entry(const char *arg, size_t len, char *url, size_t un, char *sha, size_t sn, unsigned long long *size, const char **ext) {
    if (!len || len > 120 || strlen(arg) != len) return -1;
    int fd = open(STUDIO_BG_CATALOG, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    char *buf = NULL;
    int rc = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && STUDIO_BG_OWNER_OK(st) && st.st_size > 0 && st.st_size < 1 << 20 &&
        (buf = malloc((size_t)st.st_size + 1))) {
        ssize_t r = read(fd, buf, (size_t)st.st_size);
        if (r == st.st_size) rc = hw_studio_bg_find(buf, (size_t)r, arg, url, un, sha, sn, size, ext);
    }
    free(buf);
    close(fd);
    return rc;
}

/* the SHA-256 of a file already there (a regular file, not a link), or -1 */
static int file_sha(const char *path, unsigned long long size, char hex[65]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (unsigned long long)st.st_size != size) { close(fd); return -1; }
    Sha256 s;
    sha256_init(&s);
    static unsigned char buf[1 << 16];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) sha256_update(&s, buf, (size_t)r);
    close(fd);
    if (r < 0) return -1;
    uint8_t h[32];
    sha256_final(&s, h);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", h[i]);
    return 0;
}

static void progress(uint64_t done, uint64_t total, void *ud) {
    int *last = ud;
    int pct = total ? (int)(done * 100 / total) : 0;
    if (pct == *last) return;
    *last = pct;
    admin_say("PROGRESS", "%d", pct);                   /* i18n-ok: parsed by sdk/studiobg.js */
}

void hw_act_studio_bg_get(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char url[900], sha[65], hex[65];
    const char *ext = NULL;
    unsigned long long size = 0;
    if (entry(arg, len, url, sizeof url, sha, sizeof sha, &size, &ext)) { admin_err("admin.invalidArgument", NULL); return; }
    mkdir("/var/lib/lordos", 0755);
    mkdir(STUDIO_BG_STORE, 0755);
    chmod(STUDIO_BG_STORE, 0755);
    FetchItem it;
    memset(&it, 0, sizeof it);
    snprintf(it.url, sizeof it.url, "%s", url);
    snprintf(it.dest, sizeof it.dest, "%s/%s.%s", STUDIO_BG_STORE, sha, ext);
    if (!file_sha(it.dest, size, hex) && !strcmp(hex, sha)) { admin_say("OK", "%s", sha); return; }   /* i18n-ok */
    it.max = size;
    it.expect = size;
    int last = -1;
    fetch_batch(&it, 1, progress, &last);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", it.sha256[i]);
    if (it.status != 200 || it.size != size || strcmp(hex, sha)) {
        unlink(it.dest);
        admin_err("studio.downloadFailed", NULL);
        return;
    }
    chmod(it.dest, 0644);
    admin_say("OK", "%s", sha);                         /* i18n-ok: parsed by sdk/studiobg.js */
}

void hw_act_studio_bg_remove(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char url[900], sha[65], p[200];
    const char *ext = NULL;
    unsigned long long size = 0;
    if (entry(arg, len, url, sizeof url, sha, sizeof sha, &size, &ext)) { admin_err("admin.invalidArgument", NULL); return; }
    snprintf(p, sizeof p, "%s/%s.%s", STUDIO_BG_STORE, sha, ext);
    if (unlink(p) && errno != ENOENT) { admin_err("studio.removeFailed", NULL); return; }
    admin_say("OK", "%s", "removed");                   /* i18n-ok: parsed by sdk/studiobg.js */
}
