/* webcam.c — the cameras in WebKit's web process sandbox (webcam.h). */
#include "webcam.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* "video<N>", N one to four digits: the number, else -1 */
static int node_number(const char *name) {
    if (strncmp(name, "video", 5)) return -1;
    const char *d = name + 5;
    size_t n = strlen(d);
    if (n < 1 || n > 4) return -1;
    for (size_t i = 0; i < n; i++) if (d[i] < '0' || d[i] > '9') return -1;
    return atoi(d);
}

/* sysfs's dev file: exactly "81:<minor>", a newline allowed; the minor, else -1 */
static int parse_dev(const char *t) {
    char b[16];
    size_t n = strlen(t);
    if (n >= sizeof b) return -1;
    memcpy(b, t, n + 1);
    if (n && b[n - 1] == '\n') b[--n] = 0;
    char maj[8];
    int len = snprintf(maj, sizeof maj, "%d:", WEBCAM_V4L_MAJOR);
    if (strncmp(b, maj, (size_t)len)) return -1;
    const char *m = b + len;
    size_t ml = strlen(m);
    if (ml < 1 || ml > 4) return -1;
    for (size_t i = 0; i < ml; i++) if (m[i] < '0' || m[i] > '9') return -1;
    return atoi(m);
}

int webcam_entry(const char *name, const char *dev_text, unsigned node_mode, unsigned long long node_rdev, unsigned db_mode) {
    if (!name || !dev_text || node_number(name) < 0) return -1;
    int minor = parse_dev(dev_text);
    if (minor < 0) return -1;
    if (!S_ISCHR(node_mode) || major(node_rdev) != WEBCAM_V4L_MAJOR || (int)minor(node_rdev) != minor) return -1;
    return S_ISREG(db_mode) ? minor : -1;
}

/* a small file's text (sysfs's dev), "" when it can't be read */
static void read_small(const char *path, char *b, size_t n) {
    b[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    ssize_t r = read(fd, b, n - 1);
    close(fd);
    b[r > 0 ? r : 0] = 0;
}

static int by_minor(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int webcam_udev_paths(const char *root, char out[][WEBCAM_PATH_MAX], int max) {
    if (!root || !out || max <= 0) return 0;
    char path[4096];
    snprintf(path, sizeof path, "%s/sys/class/video4linux", root);
    DIR *dir = opendir(path);
    if (!dir) return 0;
    int minors[WEBCAM_MAX], n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) && n < WEBCAM_MAX && n < max) {
        if (node_number(e->d_name) < 0) continue;
        char text[16];
        snprintf(path, sizeof path, "%s/sys/class/video4linux/%s/dev", root, e->d_name);
        read_small(path, text, sizeof text);
        int want = parse_dev(text);
        if (want < 0) continue;
        struct stat node, db;
        snprintf(path, sizeof path, "%s/dev/%s", root, e->d_name);
        if (lstat(path, &node)) continue;
        snprintf(path, sizeof path, "%s/run/udev/data/c%d:%d", root, WEBCAM_V4L_MAJOR, want);
        if (lstat(path, &db)) continue;
        int minor = webcam_entry(e->d_name, text, node.st_mode, (unsigned long long)node.st_rdev, db.st_mode);
        if (minor < 0) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) if (minors[i] == minor) dup = 1;
        if (!dup) minors[n++] = minor;
    }
    closedir(dir);
    qsort(minors, (size_t)n, sizeof minors[0], by_minor);
    for (int i = 0; i < n; i++) snprintf(out[i], WEBCAM_PATH_MAX, "/run/udev/data/c%d:%d", WEBCAM_V4L_MAJOR, minors[i]);
    return n;
}
