/* vmlistcache.c — see vmlistcache.h. */
#include "vmlistcache.h"
#include "../pkg/crypto.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define MASK (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | \
              IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW | IN_EXCL_UNLINK)

void vmlc_init(VmListCache *c) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->dirty = 1;
}

static void forget(VmListCache *c) {
    free(c->json);
    c->json = NULL;
    c->len = 0;
    c->npids = 0;
}

void vmlc_free(VmListCache *c) {
    forget(c);
    for (int i = 0; i < c->nw; i++) free(c->w[i].path);
    if (c->fd >= 0) close(c->fd);
    vmlc_init(c);
}

static int ends(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

int vmlc_name_matters(const char *name) {
    if (!name || !name[0]) return 1;
    if (name[0] == '.' || ends(name, ".tmp") || ends(name, "~") || ends(name, ".log")) return 0;
    if (!strcmp(name, "thumb.png")) return 0;
    return 1;
}

int vmlc_fd(const VmListCache *c) { return c->fd; }

static const char *path_of(const VmListCache *c, int wd) {
    for (int i = 0; i < c->nw; i++) if (c->w[i].wd == wd) return c->w[i].path;
    return NULL;
}

static int add(VmListCache *c, const char *dir) {
    if (c->fd < 0 && !c->broken) {
        c->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (c->fd < 0) c->broken = 1;
    }
    if (c->fd < 0) return -1;
    int wd = inotify_add_watch(c->fd, dir, MASK);
    if (wd < 0) {
        if (errno == ENOENT || errno == ENOTDIR) return 0;   /* gone meanwhile: the event that said so is enough */
        c->broken = 1;
        return -1;
    }
    for (int i = 0; i < c->nw; i++) {
        if (c->w[i].wd != wd) continue;
        if (strcmp(c->w[i].path, dir)) { char *p = strdup(dir); if (p) { free(c->w[i].path); c->w[i].path = p; } }
        return 0;
    }
    char *p = strdup(dir);
    if (!p || c->nw >= VMLC_WATCHES) { free(p); inotify_rm_watch(c->fd, wd); c->broken = 1; return -1; }
    c->w[c->nw].wd = wd;
    c->w[c->nw].path = p;
    c->nw++;
    return 0;
}

int vmlc_watch(VmListCache *c, const char *dir) {
    if (add(c, dir)) return -1;
    if (ends(dir, ".lordvm")) {
        char d[PATH_MAX];
        if ((size_t)snprintf(d, sizeof d, "%s/disks", dir) < sizeof d && add(c, d)) return -1;
    }
    return 0;
}

/* the folders of a library or group folder that hold machines or groups */
static int watch_children(VmListCache *c, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int r = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || (!ends(e->d_name, ".lordvm") && !ends(e->d_name, ".lordgroup"))) continue;
        char p[PATH_MAX];
        struct stat st;
        if ((size_t)snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= sizeof p || lstat(p, &st) || !S_ISDIR(st.st_mode)) continue;
        if (vmlc_watch(c, p)) { r = -1; break; }
        if (ends(e->d_name, ".lordgroup") && watch_children(c, p)) { r = -1; break; }
    }
    closedir(d);
    return r;
}

int vmlc_watch_library(VmListCache *c, const char *lib) {
    if (add(c, lib)) return -1;
    return watch_children(c, lib);
}

int vmlc_drain(VmListCache *c) {
    if (c->fd < 0) return 0;
    int mattered = 0;
    char buf[8192] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(c->fd, buf, sizeof buf);
        if (n <= 0) break;
        for (char *p = buf; p < buf + n;) {
            const struct inotify_event *ev = (const struct inotify_event *)p;
            p += sizeof *ev + ev->len;
            if (ev->mask & IN_Q_OVERFLOW) { mattered = 1; continue; }
            if (ev->mask & IN_IGNORED) {
                for (int i = 0; i < c->nw; i++) if (c->w[i].wd == ev->wd) { free(c->w[i].path); c->w[i] = c->w[--c->nw]; break; }
                continue;
            }
            const char *name = ev->len ? ev->name : "";
            if (name[0] && !vmlc_name_matters(name)) continue;
            mattered = 1;
            /* a machine or group folder (or a machine's disks/) that appears here: watched from now on */
            if ((ev->mask & IN_ISDIR) && (ev->mask & (IN_CREATE | IN_MOVED_TO)) &&
                (ends(name, ".lordvm") || ends(name, ".lordgroup") || !strcmp(name, "disks"))) {
                const char *at = path_of(c, ev->wd);
                char d[PATH_MAX];
                if (at && (size_t)snprintf(d, sizeof d, "%s/%s", at, name) < sizeof d) {
                    if (!strcmp(name, "disks")) add(c, d);
                    else {
                        vmlc_watch(c, d);
                        if (ends(name, ".lordgroup")) watch_children(c, d);
                    }
                }
            }
        }
    }
    if (mattered) {
        c->dirty = 1;
        c->gen++;
    }
    return mattered;
}

const char *vmlc_get(VmListCache *c, const char *lib, double now, int (*alive)(long), size_t *len) {
    vmlc_drain(c);
    if (c->broken || c->dirty || !c->json || strcmp(c->lib, lib) || now - c->at >= VMLC_TTL_MS || now < c->at) return NULL;
    for (int i = 0; i < c->npids; i++) if (alive && !alive(c->pids[i])) { c->dirty = 1; c->gen++; return NULL; }
    if (len) *len = c->len;
    return c->json;
}

void vmlc_begin(VmListCache *c, const char *lib) {
    vmlc_drain(c);
    if (strcmp(c->lib, lib)) {
        /* another library (HOME changed): its own watches from now on */
        for (int i = 0; i < c->nw; i++) { inotify_rm_watch(c->fd, c->w[i].wd); free(c->w[i].path); }
        c->nw = 0;
        if (c->fd >= 0) { close(c->fd); c->fd = -1; }
        c->broken = 0;
        forget(c);
        snprintf(c->lib, sizeof c->lib, "%s", lib);
    }
    if (c->broken) {
        /* a watch that couldn't be added (too many, or no inotify) last time: tried again from scratch */
        for (int i = 0; i < c->nw; i++) free(c->w[i].path);
        c->nw = 0;
        if (c->fd >= 0) { close(c->fd); c->fd = -1; }
        c->broken = 0;
    }
    c->npids = 0;
    c->dirty = 0;
    vmlc_watch_library(c, lib);
}

void vmlc_pid(VmListCache *c, long pid) {
    if (c->npids < VMLC_PIDS) c->pids[c->npids++] = pid;
}

void vmlc_store(VmListCache *c, const char *json, size_t len, double now) {
    int differs = !c->json || c->len != len || memcmp(c->json, json, len);
    if (differs) c->gen++;
    char *j = c->broken ? NULL : malloc(len + 1);
    free(c->json);
    c->json = j;
    c->len = 0;
    if (j) { memcpy(j, json, len); j[len] = 0; c->len = len; }
    c->at = now;
    /* an event between vmlc_begin and now already set dirty: the list read may be older than it */
}

/* ---- the firmware's hash, once per file version ---- */

unsigned vmlc_fw_hashes;

int vmlc_firmware_changed(int fd, const char *want) {
    static struct { int used; dev_t dev; ino_t ino; off_t size; long long mt, ct; char sum[65]; } fw[VMLC_FW];
    static int next;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || !want) return -1;
    long long mt = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
    long long ct = (long long)st.st_ctim.tv_sec * 1000000000LL + st.st_ctim.tv_nsec;
    int hit = -1;
    for (int i = 0; i < VMLC_FW; i++)
        if (fw[i].used && fw[i].dev == st.st_dev && fw[i].ino == st.st_ino && fw[i].size == st.st_size && fw[i].mt == mt && fw[i].ct == ct) hit = i;
    if (hit < 0) {
        Sha256 h;
        uint8_t dg[32], buf[65536];
        ssize_t r;
        sha256_init(&h);
        if (lseek(fd, 0, SEEK_SET) < 0) return -1;
        while ((r = read(fd, buf, sizeof buf)) > 0) sha256_update(&h, buf, (size_t)r);
        if (r < 0) return -1;
        sha256_final(&h, dg);
        vmlc_fw_hashes++;
        /* the same inode replaced in place: its old slot goes */
        for (int i = 0; i < VMLC_FW; i++) if (fw[i].used && fw[i].dev == st.st_dev && fw[i].ino == st.st_ino) fw[i].used = 0;
        hit = next;
        next = (next + 1) % VMLC_FW;
        fw[hit].used = 1;
        fw[hit].dev = st.st_dev;
        fw[hit].ino = st.st_ino;
        fw[hit].size = st.st_size;
        fw[hit].mt = mt;
        fw[hit].ct = ct;
        for (int q = 0; q < 32; q++) snprintf(fw[hit].sum + 2 * q, 3, "%02x", dg[q]);
    }
    return strcmp(fw[hit].sum, want) != 0;
}
