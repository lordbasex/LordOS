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
