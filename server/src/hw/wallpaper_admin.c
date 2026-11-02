/* wallpaper_admin.c — moving wallpapers on disk (docs/WALLPAPERS.md). Closed actions of the admin helper:
 *
 *   wallpaper-get "<id> <variant>"     downloads one file of the catalog to /var/lib/lordos/wallpapers/<sha256>.mp4
 *   wallpaper-remove "<id> <variant>"  deletes it
 *   wallpaper-login "still <id>"       the login screen shows that still wallpaper, with no video
 *   wallpaper-login "<id> <variant>"   the login screen plays it; "default" goes back to the built-in one
 *   wallpaper-image "<path>"           the session user's own copy of a picture they chose, made by a child running as
 *                                      them; answers its name, 16 hex digits. "remove <name>" deletes that copy
 *   wallpaper-login-image "<name>"     the login screen shows that copy: stored in /var/lib/lordos/wallpapers
 *
 * What is fetched comes only from the catalog in the package, read here as root: JS names an entry, never a URL or
 * a path. The bytes go through fetch_batch (a child as nobody, in the Store's scope), and a file whose size or
 * SHA-256 isn't the catalog's is never kept. */
#include "hw_admin.h"
#include "wallpaper_find.h"
#include "wallpaper_image.h"
#include "../admin.h"
#include "../pkg/crypto.h"
#include "../pkg/http.h"
#include "../sec/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CATALOG "/usr/share/backgrounds/lordos/motion.json"
#define STORE   "/var/lib/lordos/wallpapers"
#define MINE    "/.local/share/lordos/wallpapers"     /* under the user's home: their copies */
#define IMAGE_SECONDS 30                              /* a picture takes far less; a stuck disk doesn't wait forever */

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

/* What the login screen shows with no video: a still wallpaper's id, or "image:<name>". */
static int put_still(const char *what, size_t n) {
    const char *tmp = STORE "/.login.part";
    mkdir(STORE, 0755);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    int bad = write(fd, what, n) != (ssize_t)n;
    if (close(fd)) bad = 1;
    if (bad || rename(tmp, STORE "/login-still")) { unlink(tmp); return -1; }
    return 0;
}

/* ---- a picture of the user's own ----
 *
 * Root never opens the user's file and never decodes it. A child becomes the session user, with no way back and none of
 * the helper's open files, and reads and converts it there: a link to something only root can read just fails, and a
 * crafted picture reaches the decoder only with the rights the user already has. */

static int name_ok(const char *s, size_t n) {
    if (n != 16) return 0;
    for (size_t i = 0; i < n; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

/* Removes the stored login pictures, but for keep. */
static void login_images_clear(const char *keep) {
    DIR *d = opendir(STORE);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d)); ) {
        size_t l = strlen(e->d_name);
        if (l != 26 || strncmp(e->d_name, "login-", 6) || strcmp(e->d_name + 22, ".jpg") || !name_ok(e->d_name + 6, 16)) continue;
        if (keep && !strcmp(e->d_name, keep)) continue;
        unlinkat(dirfd(d), e->d_name, 0);
    }
    closedir(d);
}

static int write_all(int fd, const void *data, size_t n) {
    const char *p = data;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void image_hex(const unsigned char *d, size_t n, char hex[17]) {
    uint8_t h[32];
    sha256(d, n, h);
    for (int i = 0; i < 8; i++) snprintf(hex + i * 2, 3, "%02x", h[i]);
}

/* Becomes the session user for good. The rest of this process's open files are the helper's: all closed. */
static int become_user(const AdminReq *r, int out) {
    if (initgroups(r->user, r->gid) && setgroups(0, NULL)) return -1;
    if (setresgid(r->gid, r->gid, r->gid) || setresuid(r->uid, r->uid, r->uid)) return -1;
    if (!getuid() || !geteuid()) return -1;
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    struct rlimit cpu = { IMAGE_SECONDS, IMAGE_SECONDS };
    setrlimit(RLIMIT_CPU, &cpu);
    if (dup2(out, 3) < 0) return -1;
    close_range(4, ~0U, 0);
    int nul = open("/dev/null", O_RDWR);
    if (nul < 0) return -1;
    for (int fd = 0; fd < 3; fd++) if (nul != fd) dup2(nul, fd);
    if (nul > 3) close(nul);
    return chdir("/");
}

static int save_mine(const char *home, const char *name, const unsigned char *d, size_t n) {
    char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/.local/share/lordos/wallpapers", home) >= (int)sizeof dir) return WP_WRITE;
    for (char *p = dir + strlen(home) + 1; (p = strchr(p, '/')); *p++ = '/') { *p = 0; mkdir(dir, 0755); }
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%.4000s/%s", dir, name);
    snprintf(tmp, sizeof tmp, "%.4000s/.%s.%d", dir, name, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return WP_WRITE;
    int bad = write_all(fd, d, n);
    if (close(fd)) bad = -1;
    if (bad || rename(tmp, path)) { unlink(tmp); return WP_WRITE; }
    return WP_OK;
}

/* In the child, as the user. keep: saves the picture and its thumbnail among the user's copies, and the result is its
 * name; otherwise the result is the JPEG. */
static int convert(const AdminReq *r, const char *path, int keep, unsigned char **full, size_t *fn) {
    unsigned char *in = NULL, *thumb = NULL;
    size_t n = 0, tn = 0;
    int rc = wp_read_file(path, &in, &n);
    if (rc) return rc;
    rc = wp_encode(in, n, full, fn, keep ? &thumb : NULL, &tn);
    free(in);
    if (!rc && keep) {
        char hex[17], name[32];
        image_hex(*full, *fn, hex);
        snprintf(name, sizeof name, "%s.jpg", hex);
        rc = save_mine(r->home, name, *full, *fn);
        snprintf(name, sizeof name, "%s.thumb.jpg", hex);
        if (!rc) rc = save_mine(r->home, name, thumb, tn);
        free(*full);
        free(thumb);
        if (rc) return rc;
        *full = (unsigned char *)strdup(hex);
        *fn = 16;
    }
    return rc;
}

/* Runs convert() in a child and collects the result: a WP_ code first, then its bytes. Returns the code. */
static int in_child(const AdminReq *r, const char *path, int keep, unsigned char **out, size_t *n) {
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return WP_WRITE;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return WP_WRITE; }
    if (!pid) {
        close(p[0]);
        if (become_user(r, p[1])) _exit(1);
        unsigned char *d = NULL;
        size_t dn = 0;
        unsigned char code = (unsigned char)convert(r, path, keep, &d, &dn);
        if (write_all(3, &code, 1) || (!code && write_all(3, d, dn))) _exit(1);
        _exit(0);
    }
    close(p[1]);
    size_t cap = (size_t)WP_MAX_JPEG + 1, got = 0;
    unsigned char *buf = malloc(cap);
    time_t end = time(NULL) + IMAGE_SECONDS;
    int rc = WP_WRITE;
    while (buf) {
        struct pollfd f = { p[0], POLLIN, 0 };
        long left = (long)(end - time(NULL));
        if (left <= 0 || poll(&f, 1, (int)left * 1000) == 0) { kill(pid, SIGKILL); got = 0; break; }
        if (got == cap) { kill(pid, SIGKILL); got = 0; break; }
        ssize_t k = read(p[0], buf + got, cap - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        got += (size_t)k;
    }
    close(p[0]);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) { }
    if (got) {
        rc = buf[0];
        if (rc == WP_OK) {
            if (got == 1) rc = WP_WRITE;
            else { *n = got - 1; memmove(buf, buf + 1, got - 1); *out = buf; return WP_OK; }
        }
    }
    free(buf);
    return rc;
}

static void image_err(int rc) {
    if (rc == WP_BAD) admin_err("wallpaper.badImage", NULL);
    else if (rc == WP_TOOBIG) admin_err("wallpaper.imageTooBig", NULL);
    else if (rc == WP_READ) admin_err("wallpaper.imageUnreadable", NULL);
    else admin_err("wallpaper.imageFailed", NULL);
}

/* Deletes the user's copy, as the user. */
static void image_remove(const AdminReq *r, const char *name) {
    pid_t pid = fork();
    if (pid < 0) { admin_err("wallpaper.imageFailed", NULL); return; }
    if (!pid) {
        int nul = open("/dev/null", O_RDONLY);
        if (nul < 0 || become_user(r, nul)) _exit(1);
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%.3900s" MINE "/%s.jpg", r->home, name);
        int bad = unlink(p) && errno != ENOENT;
        snprintf(p, sizeof p, "%.3900s" MINE "/%s.thumb.jpg", r->home, name);
        bad |= unlink(p) && errno != ENOENT;
        _exit(bad);
    }
    int st = 1;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
    if (WIFEXITED(st) && !WEXITSTATUS(st)) admin_say("OK", "%s", "removed");    /* i18n-ok: parsed by sdk/motion.jsx */
    else admin_err("wallpaper.imageFailed", NULL);
}

void hw_act_wallpaper_image(const AdminReq *r, const char *arg, size_t len) {
    if (strlen(arg) != len || !len || len >= PATH_MAX) { admin_err("admin.invalidArgument", NULL); return; }
    if (len == 23 && !strncmp(arg, "remove ", 7) && name_ok(arg + 7, 16)) { image_remove(r, arg + 7); return; }
    if (arg[0] != '/') { admin_err("admin.invalidArgument", NULL); return; }
    for (size_t i = 0; i < len; i++) if ((unsigned char)arg[i] < 32) { admin_err("admin.invalidArgument", NULL); return; }
    unsigned char *out = NULL;
    size_t n = 0;
    int rc = in_child(r, arg, 1, &out, &n);
    if (rc) { image_err(rc); return; }
    if (n != 16 || !name_ok((char *)out, 16)) { free(out); admin_err("wallpaper.imageFailed", NULL); return; }
    admin_say("OK", "%.16s", (char *)out);              /* i18n-ok: parsed by sdk/motion.jsx */
    free(out);
}

void hw_act_wallpaper_login_image(const AdminReq *r, const char *arg, size_t len) {
    if (strlen(arg) != len || !name_ok(arg, len)) { admin_err("admin.invalidArgument", NULL); return; }
    char src[PATH_MAX];
    if (snprintf(src, sizeof src, "%s" MINE "/%.16s.jpg", r->home, arg) >= (int)sizeof src) { admin_err("admin.invalidArgument", NULL); return; }
    unsigned char *jpg = NULL;
    size_t n = 0;
    int rc = in_child(r, src, 0, &jpg, &n);
    if (rc) { image_err(rc); return; }
    if (!wp_is_jpeg(jpg, n)) { free(jpg); admin_err("wallpaper.imageFailed", NULL); return; }
    char hex[17], name[32], dst[128], tmp[128], mark[32];
    image_hex(jpg, n, hex);
    snprintf(name, sizeof name, "login-%s.jpg", hex);
    snprintf(dst, sizeof dst, STORE "/%s", name);
    snprintf(tmp, sizeof tmp, STORE "/.login-image.%d", (int)getpid());
    mkdir("/var/lib/lordos", 0755);
    mkdir(STORE, 0755);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    int bad = fd < 0 || fchmod(fd, 0644) || write_all(fd, jpg, n) || fsync(fd);
    if (fd >= 0 && close(fd)) bad = 1;
    free(jpg);
    if (bad || rename(tmp, dst)) { unlink(tmp); admin_err("wallpaper.imageFailed", NULL); return; }
    int ml = snprintf(mark, sizeof mark, "image:%s", hex);
    if (put_still(mark, (size_t)ml)) { unlink(dst); admin_err("wallpaper.imageFailed", NULL); return; }
    unlink(STORE "/login.mp4");
    login_images_clear(name);
    admin_say("OK", "%s", hex);                         /* i18n-ok: parsed by sdk/motion.jsx */
}

void hw_act_wallpaper_login(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    const char *dst = STORE "/login.mp4", *tmp = STORE "/.login.part", *still = STORE "/login-still";
    if (len > 6 && len < 60 && !strncmp(arg, "still ", 6)) {            /* a still wallpaper by its id, never a path */
        for (size_t i = 6; i < len; i++) if (!((arg[i] >= 'a' && arg[i] <= 'z') || (arg[i] >= '0' && arg[i] <= '9') || arg[i] == '-')) { admin_err("admin.invalidArgument", NULL); return; }
        if (put_still(arg + 6, len - 6)) { admin_err("wallpaper.removeFailed", NULL); return; }
        unlink(dst);
        login_images_clear(NULL);
        admin_say("OK", "%s", "still");                 /* i18n-ok: parsed by sdk/motion.js */
        return;
    }
    if (len == 7 && !strncmp(arg, "default", 7)) {
        unlink(still);
        login_images_clear(NULL);
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
    unlink(still);
    login_images_clear(NULL);
    admin_say("OK", "%s", sha);                         /* i18n-ok: parsed by sdk/motion.js */
}
