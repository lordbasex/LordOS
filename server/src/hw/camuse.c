/* camuse.c — the capture indicator's camera rows for what opens a camera itself (camuse.h). */
#include "camuse.h"
#include "../media/mstudiohub.h"
#include "../sec/appid.h"
#include "../fw/rules.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define V4L_MAJOR   81
#define SETTLE_MS   300.0               /* after an open or close: a probe that opens and closes at once isn't shown */
#define RECHECK_MS  3000.0              /* while somebody holds a camera */
#define MAX_HOLDERS 64

static void (*emit)(const char *kind, const char *json);
static int ifd = -1;
static double due = -1;                 /* CLOCK_MONOTONIC ms of the next scan, -1 none */
static char last[2048] = "{\"apps\":[]}";
static int holding;

/* "video<N>", N digits */
static int video_node(const char *name) {
    if (strncmp(name, "video", 5) || !name[5]) return 0;
    for (const char *p = name + 5; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}

void camuse_rewatch(void) {
    if (ifd < 0) return;
    DIR *d = opendir("/dev");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!video_node(e->d_name)) continue;
        char p[300];
        struct stat st;
        snprintf(p, sizeof p, "/dev/%s", e->d_name);
        if (lstat(p, &st) || !S_ISCHR(st.st_mode) || major(st.st_rdev) != V4L_MAJOR) continue;
        /* the same inode again gives the same watch; a node that went away dropped its own (IN_IGNORED) */
        inotify_add_watch(ifd, p, IN_OPEN | IN_CLOSE_WRITE | IN_CLOSE_NOWRITE | IN_DONT_FOLLOW);
    }
    closedir(d);
    due = 0;                            /* a camera that went may have been in use: look again now */
}

void camuse_start(void (*fn)(const char *kind, const char *json)) {
    emit = fn;
    ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) { fprintf(stderr, "hw: no inotify (a camera a web app opens itself isn't shown in the capture indicator)\n"); return; }
    camuse_rewatch();
    due = -1;                           /* nothing to say before the first open */
}

int camuse_fd(void) { return ifd; }

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

void camuse_dispatch(void) {
    char b[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    int seen = 0;
    for (;;) {
        ssize_t n = read(ifd, b, sizeof b);
        if (n <= 0) break;
        seen = 1;
    }
    if (!seen) return;
    double at = now_ms() + SETTLE_MS;
    if (due < 0 || at < due) due = at;
}

static void scan(void) {
    static pid_t pids[MAX_HOLDERS];
    static char apps[MAX_HOLDERS][FW_APP_MAX];
    int n = camera_scan_holders("/proc", V4L_MAJOR, 0, getpid(), getuid(), pids, MAX_HOLDERS);
    if (n > MAX_HOLDERS) n = MAX_HOLDERS;
    for (int i = 0; i < n; i++) if (appid_of_pid((unsigned)pids[i], apps[i], sizeof apps[i])) apps[i][0] = 0;
    holding = n > 0;
    char json[sizeof last];
    if (camera_use_format(&apps[0][0], FW_APP_MAX, n, json, sizeof json)) snprintf(json, sizeof json, "{\"apps\":[\"\"]}");
    if (!strcmp(json, last)) return;
    snprintf(last, sizeof last, "%s", json);
    if (emit) emit("camera-use", last);
}

void camuse_tick(double now) {
    if (due < 0 || now < due) return;
    scan();
    due = holding ? now + RECHECK_MS : -1;
}

double camuse_timeout(double now) {
    if (due < 0) return -1;
    return due > now ? due - now : 0;
}
