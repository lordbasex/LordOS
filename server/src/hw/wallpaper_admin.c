/* wallpaper_admin.c — moving wallpapers on disk (docs/WALLPAPERS.md). Closed actions of the admin helper:
 *
 *   wallpaper-get "<id> <variant>"     downloads one file of the catalog to /var/lib/lordos/wallpapers/<sha256>.mp4
 *   wallpaper-remove "<id> <variant>"  deletes it
 *   wallpaper-login "<id> <variant>"   the login screen plays it; "default" goes back to the built-in one
 *
 * What is fetched comes only from the catalog in the package, read here as root: JS names an entry, never a URL or
 * a path. The bytes go through fetch_batch (a child as nobody, in the Store's scope), and a file whose size or
 * SHA-256 isn't the catalog's is never kept. */
#include "hw_admin.h"
#include "wallpaper_find.h"
#include "../admin.h"
#include "../pkg/http.h"
#include "../sec/json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CATALOG "/usr/share/backgrounds/lordos/motion.json"
#define STORE   "/var/lib/lordos/wallpapers"

static int entry(const char *arg, size_t len, char *url, size_t un, char *sha, size_t sn, unsigned long long *size) {
    if (!len || len > 120 || strlen(arg) != len) return -1;
    int fd = open(CATALOG, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    char *buf = NULL;
    int rc = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && st.st_size > 0 && st.st_size < 1 << 20 && (buf = malloc((size_t)st.st_size + 1))) {
        ssize_t r = read(fd, buf, (size_t)st.st_size);
        if (r == st.st_size) rc = hw_wallpaper_find(buf, (size_t)r, arg, url, un, sha, sn, size);
    }
    free(buf);
    close(fd);
    return rc;
}

static void progress(uint64_t done, uint64_t total, void *ud) {
    int *last = ud;
    int pct = total ? (int)(done * 100 / total) : 0;
    if (pct == *last) return;
    *last = pct;
    char line[32];
    snprintf(line, sizeof line, "%d", pct);
    admin_say("PROGRESS", "%s", line);                  /* i18n-ok: parsed by sdk/motion.js */
}

void hw_act_wallpaper_get(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char url[900], sha[65];
    unsigned long long size = 0;
    if (entry(arg, len, url, sizeof url, sha, sizeof sha, &size)) { admin_err("admin.invalidArgument", NULL); return; }
    mkdir(STORE, 0755);
    chmod(STORE, 0755);
    FetchItem it;
    memset(&it, 0, sizeof it);
    snprintf(it.url, sizeof it.url, "%s", url);
    snprintf(it.dest, sizeof it.dest, "%s/%s.mp4", STORE, sha);
    struct stat st;
    if (!stat(it.dest, &st) && (unsigned long long)st.st_size == size) { admin_say("OK", "%s", sha); return; }   /* i18n-ok */
    it.max = size;
    it.expect = size;
    int last = -1;
    fetch_batch(&it, 1, progress, &last);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", it.sha256[i]);
    if (it.status != 200 || it.size != size || strcmp(hex, sha)) {
        unlink(it.dest);
        admin_err("wallpaper.downloadFailed", NULL);
        return;
    }
    chmod(it.dest, 0644);
    admin_say("OK", "%s", sha);                         /* i18n-ok: parsed by sdk/motion.js */
}

void hw_act_wallpaper_remove(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char url[900], sha[65], p[200];
    unsigned long long size = 0;
    if (entry(arg, len, url, sizeof url, sha, sizeof sha, &size)) { admin_err("admin.invalidArgument", NULL); return; }
    snprintf(p, sizeof p, "%s/%s.mp4", STORE, sha);
    if (unlink(p) && errno != ENOENT) { admin_err("wallpaper.removeFailed", NULL); return; }
    admin_say("OK", "%s", "removed");                   /* i18n-ok: parsed by sdk/motion.js */
}

void hw_act_wallpaper_login(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    const char *dst = STORE "/login.mp4", *tmp = STORE "/.login.part";
    if (len == 7 && !strncmp(arg, "default", 7)) {
        if (unlink(dst) && errno != ENOENT) { admin_err("wallpaper.removeFailed", NULL); return; }
        admin_say("OK", "%s", "default");               /* i18n-ok: parsed by sdk/motion.js */
        return;
    }
    char url[900], sha[65], p[200];
    unsigned long long size = 0;
    if (entry(arg, len, url, sizeof url, sha, sizeof sha, &size)) { admin_err("admin.invalidArgument", NULL); return; }
    snprintf(p, sizeof p, "%s/%s.mp4", STORE, sha);
    struct stat st;
    if (lstat(p, &st) || !S_ISREG(st.st_mode) || (unsigned long long)st.st_size != size) { admin_err("wallpaper.notDownloaded", NULL); return; }
    unlink(tmp);
    if (link(p, tmp) || rename(tmp, dst)) { unlink(tmp); admin_err("wallpaper.removeFailed", NULL); return; }
    admin_say("OK", "%s", sha);                         /* i18n-ok: parsed by sdk/motion.js */
}
