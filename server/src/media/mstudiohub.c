/* mstudiohub.c — LordOS Studio's camera for every app: plain logic (mstudiohub.h). */
#include "mstudiohub.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int reading(int readers, int scanned) { return readers > 0 || (readers < 0 && scanned > 0); }

int studio_hub_show(int on, int readers, int scanned, int preview) {
    if (!on) return reading(readers, scanned) ? STUDIO_SHOW_OFF : STUDIO_SHOW_IDLE;
    return reading(readers, scanned) || preview ? STUDIO_SHOW_CAMERA : STUDIO_SHOW_IDLE;
}

int studio_hub_wanted(int on, int available, int device, int readers, int scanned) {
    if (!available || !device) return 0;
    return on || reading(readers, scanned);
}

static int numeric(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

/* the PPid line of /proc/<pid>/status, or -1 */
static pid_t parent_of(int dirfd) {
    int fd = openat(dirfd, "status", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char b[1024];
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    const char *p = strstr(b, "\nPPid:");
    return p ? (pid_t)strtol(p + 6, NULL, 10) : -1;
}

/* the process has an fd on rdev: readlink first (cheap), stat only when the name could be a video device */
static int holds(int dirfd, dev_t rdev) {
    int fdd = openat(dirfd, "fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fdd < 0) return 0;
    DIR *d = fdopendir(fdd);
    if (!d) { close(fdd); return 0; }
    int hit = 0;
    struct dirent *e;
    while (!hit && (e = readdir(d))) {
        if (!numeric(e->d_name)) continue;
        char t[64];
        ssize_t n = readlinkat(fdd, e->d_name, t, sizeof t - 1);
        if (n <= 0) continue;
        t[n] = 0;
        if (strncmp(t, "/dev/", 5)) continue;
        struct stat st;
        if (fstatat(fdd, e->d_name, &st, 0) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == rdev) hit = 1;
    }
    closedir(d);
    return hit;
}

int studio_scan_readers(const char *proc, dev_t rdev, pid_t self, uid_t uid, pid_t *out, int max) {
    DIR *d = opendir(proc);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!numeric(e->d_name)) continue;
        pid_t pid = (pid_t)strtol(e->d_name, NULL, 10);
        if (pid <= 0 || pid == self) continue;
        int pd = openat(dirfd(d), e->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (pd < 0) continue;
        struct stat st;
        if (fstat(pd, &st) == 0 && st.st_uid == uid && parent_of(pd) != self && holds(pd, rdev)) {
            if (n < max && out) out[n] = pid;
            n++;
        }
        close(pd);
    }
    closedir(d);
    return n;
}

static int plain(const char *s) {
    for (; *s; s++) if (*s == '"' || *s == '\\' || *s == '[' || *s == ']' || (unsigned char)*s < 0x20) return 0;
    return 1;
}

int studio_use_format(const char *apps, size_t stride, int n, const char *camera, int effects, char *out, size_t cap) {
    size_t at = 0;
    int w = snprintf(out, cap, "{\"apps\":[");
    if (w < 0 || (size_t)w >= cap) return -1;
    at = (size_t)w;
    int first = 1;
    for (int i = 0; i < n; i++) {
        const char *a = apps + (size_t)i * stride;
        if (!plain(a)) a = "";
        int seen = 0;
        for (int k = 0; k < i && !seen; k++) {
            const char *b = apps + (size_t)k * stride;
            if (!plain(b)) b = "";
            seen = !strcmp(a, b);
        }
        if (seen) continue;
        w = snprintf(out + at, cap - at, "%s\"%s\"", first ? "" : ",", a);
        if (w < 0 || (size_t)w >= cap - at) return -1;
        at += (size_t)w;
        first = 0;
    }
    char cam[128];
    size_t c = 0;
    for (const char *p = camera ? camera : ""; *p && c + 1 < sizeof cam; p++)
        cam[c++] = (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) ? ' ' : *p;
    cam[c] = 0;
    w = snprintf(out + at, cap - at, "],\"camera\":\"%s\",\"effects\":%s}", cam, effects ? "true" : "false");
    if (w < 0 || (size_t)w >= cap - at) return -1;
    return 0;
}

/* ---- phase 3 ---- */

static const char *const FILTERS[STUDIO_FILTERS + 1] = { "none", "warm", "cool", "vivid", "mono", "noir", "film", "fade", "sepia" };

const char *studio_filter_name(int id) { return id >= 0 && id <= STUDIO_FILTERS ? FILTERS[id] : "none"; }

int studio_filter_id(const char *name) {
    for (int i = 0; name && i <= STUDIO_FILTERS; i++) if (!strcmp(name, FILTERS[i])) return i;
    return -1;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void studio_pack_config(int mode, int key, int filter, int mirror, int blur, int similarity, int smoothness, int spill, int framing,
                        int light, int match, int filter_amt, int retouch, int64_t v[6]) {
#define P(a, b, c, d) ((int64_t)(a) | (int64_t)(b) << 8 | (int64_t)(c) << 16 | (int64_t)(d) << 24)
    v[0] = P(clampi(mode, 0, 3), clampi(key, 0, 2), clampi(filter, 0, STUDIO_FILTERS), mirror != 0);
    v[1] = P(clampi(blur, 0, 100), clampi(similarity, 0, 100), clampi(smoothness, 0, 100), clampi(spill, 0, 100));
    v[2] = P(clampi(framing, 0, 100), clampi(light, 0, 100), clampi(match, 0, 60), clampi(filter_amt, 0, 100));
    v[3] = clampi(retouch, 0, 100);
    v[4] = v[5] = 0;
#undef P
}

/* a file name with nothing but [A-Za-z0-9._-], not starting with a dot */
static int plain_name(const char *s) {
    if (!*s || *s == '.') return 0;
    for (; *s; s++) if (!(isalnum((unsigned char)*s) || *s == '.' || *s == '_' || *s == '-')) return 0;
    return 1;
}

int studio_bg_path_ok(const char *path) {
    if (!path || !*path || strlen(path) >= 4096) return -1;
    for (const char *p = path; *p; p++) if ((unsigned char)*p < 0x20 || *p == 0x7f) return -1;
    if (strstr(path, "..")) return -1;
    if (!strncmp(path, "asset:wallpapers/", 17)) {
        /* category/file.jpg: plain names, one folder at most */
        const char *a = path + 17, *sl = strchr(a, '/');
        if (!sl) return plain_name(a) ? 0 : -1;
        char dir[64];
        if ((size_t)(sl - a) >= sizeof dir || strchr(sl + 1, '/')) return -1;
        memcpy(dir, a, (size_t)(sl - a));
        dir[sl - a] = 0;
        return plain_name(dir) && plain_name(sl + 1) ? 0 : -1;
    }
    if (!strncmp(path, "asset:studio/backgrounds/", 25)) return plain_name(path + 25) ? 0 : -1;
    if (!strncmp(path, "asset:", 6)) return -1;
    if (!strncmp(path, "/var/lib/lordos/studio/", 23)) {
        const char *n = path + 23;
        if (strlen(n) != 68) return -1;
        for (int i = 0; i < 64; i++) if (!((n[i] >= '0' && n[i] <= '9') || (n[i] >= 'a' && n[i] <= 'f'))) return -1;
        return !strcmp(n + 64, ".jpg") || !strcmp(n + 64, ".mp4") ? 0 : -1;
    }
    return path[0] == '/' ? 0 : -1;
}
