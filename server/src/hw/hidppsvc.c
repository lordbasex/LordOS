/* hidppsvc.c — runs lord-hidpp and keeps its answers (see hidppsvc.h). */
#include "hidppsvc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HIDPP_KEEP 12
#define HIDPP_EVERY 60          /* seconds between runs */

typedef struct { char supply[32]; int percent, status; } Answer;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Answer kept[HIDPP_KEEP];
static int nkept, running;
static time_t ran_at;

int hidpp_parse_line(const char *line, int *hidraw, int *percent, int *status) {
    if (!line) return -1;
    char extra;
    int h, p, s;
    if (sscanf(line, "%d %d %d %c", &h, &p, &s, &extra) != 3) return -1;
    for (const char *c = line; *c; c++) if (!isdigit((unsigned char)*c) && *c != ' ' && *c != '\n') return -1;
    if (h < 0 || h > 999 || p < 0 || p > 100 || s < 0 || s > 2) return -1;
    *hidraw = h; *percent = p; *status = s;
    return 0;
}

int hidpp_supply_of(const char *root, int hidraw, char *out, size_t n) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/hidraw%d/device/power_supply", root, hidraw);
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d))) {
        const char *s = e->d_name;
        if (strncmp(s, "hidpp_battery_", 14) || !s[14] || strlen(s) > 20) continue;
        int ok = 1;
        for (const char *c = s + 14; *c; c++) if (!isdigit((unsigned char)*c)) ok = 0;
        if (!ok) continue;
        snprintf(out, n, "%s", s);
        found = 0;
        break;
    }
    closedir(d);
    return found;
}

/* the helper next to lord-server, root's and not writable by others (its setgid bit is what gives it the group) */
static int helper_path(char *bin, size_t size) {
    ssize_t n = readlink("/proc/self/exe", bin, size - 16);
    if (n <= 0) return -1;
    bin[n] = 0;
    char *slash = strrchr(bin, '/');
    if (!slash) return -1;
    strcpy(slash + 1, "lord-hidpp");
    struct stat st;
    if (stat(bin, &st) || !S_ISREG(st.st_mode) || (st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & 002)) return -1;
    return 0;
}

static void *run(void *arg) {
    (void)arg;
    Answer got[HIDPP_KEEP];
    int ngot = 0;
    char bin[PATH_MAX];
    int p[2] = { -1, -1 };
    pid_t pid = 0;
    if (!helper_path(bin, sizeof bin) && !pipe2(p, O_CLOEXEC)) {
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, p[1], 1);
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawnattr_t at;
        posix_spawnattr_init(&at);
        sigset_t none, all;
        sigemptyset(&none);
        sigfillset(&all);
        posix_spawnattr_setsigmask(&at, &none);
        posix_spawnattr_setsigdefault(&at, &all);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
        char *argv[] = { "lord-hidpp", NULL };
        char *envp[] = { "LANG=C.UTF-8", getenv("LORDOS_SESSIOND") ? "LORDOS_SESSIOND=1" : NULL, NULL };
        int e = posix_spawn(&pid, bin, &fa, &at, argv, envp);
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&at);
        close(p[1]);
        if (e) pid = 0;
        else {
            char buf[1024];
            size_t len = 0;
            struct timespec t0;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (;;) {
                struct timespec t;
                clock_gettime(CLOCK_MONOTONIC, &t);
                long left = 12000 - ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
                if (left <= 0 || len >= sizeof buf - 1) break;
                struct pollfd pf = { p[0], POLLIN, 0 };
                int r = poll(&pf, 1, (int)left);
                if (r < 0 && errno == EINTR) continue;
                if (r <= 0) break;
                ssize_t k = read(p[0], buf + len, sizeof buf - 1 - len);
                if (k <= 0) break;
                len += (size_t)k;
            }
            buf[len] = 0;
            for (char *line = strtok(buf, "\n"); line && ngot < HIDPP_KEEP; line = strtok(NULL, "\n")) {
                int h, pc, st;
                if (hidpp_parse_line(line, &h, &pc, &st)) continue;
                if (hidpp_supply_of("/sys/class/hidraw", h, got[ngot].supply, sizeof got[ngot].supply)) continue;
                got[ngot].percent = pc;
                got[ngot].status = st;
                ngot++;
            }
        }
        close(p[0]);
        if (pid > 0) {
            if (waitpid(pid, NULL, WNOHANG) != pid) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
        }
    }
    pthread_mutex_lock(&lock);
    memcpy(kept, got, sizeof got[0] * (size_t)ngot);
    nkept = ngot;
    running = 0;
    ran_at = time(NULL);
    pthread_mutex_unlock(&lock);
    return NULL;
}

size_t hidpp_json(char *out, size_t n) {
    if (!n) return 0;
    pthread_mutex_lock(&lock);
    time_t now = time(NULL);
    if (!running && (ran_at == 0 || now - ran_at >= HIDPP_EVERY || now < ran_at)) {
        pthread_t th;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (!pthread_create(&th, &a, run, NULL)) running = 1;
        pthread_attr_destroy(&a);
    }
    size_t o = 0;
    o += (size_t)snprintf(out + o, n - o, "[");
    for (int i = 0; i < nkept && o < n; i++)
        o += (size_t)snprintf(out + o, n - o, "%s{\"supply\":\"%s\",\"percent\":%d,\"charging\":%s,\"full\":%s}", i ? "," : "",
                              kept[i].supply, kept[i].percent, kept[i].status == 1 ? "true" : "false", kept[i].status == 2 ? "true" : "false");
    if (o < n) o += (size_t)snprintf(out + o, n - o, "]");
    pthread_mutex_unlock(&lock);
    return o < n ? o : n - 1;
}
