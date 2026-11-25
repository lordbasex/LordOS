/* nofile.c — lord-server's limit of open files (nofile.h, GitHub #251) */
#include "nofile.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int nofile_raise(rlim_t *before, rlim_t *after) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl)) return -1;
    if (before) *before = rl.rlim_cur;
    if (after) *after = rl.rlim_cur;
    rlim_t t = nofile_target(rl.rlim_cur, rl.rlim_max, NOFILE_WANT);
    if (t == rl.rlim_cur) return 0;
    struct rlimit nr = { t, rl.rlim_max };
    if (setrlimit(RLIMIT_NOFILE, &nr)) return -1;
    if (after) *after = t;
    return 0;
}

int nofile_count(rlim_t *limit) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl)) return -1;
    if (limit) *limit = rl.rlim_cur;
    DIR *d = opendir("/proc/self/fd");
    if (d) {
        int n = -1;                         /* (the directory's own fd is listed too) */
        struct dirent *e;
        while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
        closedir(d);
        return n;
    }
    /* /proc/self is root's (lord-server dropped root, so it isn't dumpable): probe each number up to the limit;
     * at most NOFILE_WANT cheap calls, only when something already went wrong */
    rlim_t top = rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > NOFILE_WANT ? NOFILE_WANT : rl.rlim_cur;
    int n = 0;
    for (rlim_t fd = 0; fd < top; fd++) n += fcntl((int)fd, F_GETFD) != -1;
    return n;
}

void nofile_wl_log(const char *fmt, va_list ap) {
    vfprintf(stderr, fmt, ap);
    if (!strstr(fmt, "file descriptor expected")) return;
    static time_t last;
    time_t now = time(NULL);
    if (last && now - last < 10) return;
    last = now;
    rlim_t lim = 0;
    int n = nofile_count(&lim);
    fprintf(stderr, "lord-server: %d files open of %llu allowed (an app's file was lost on the way if the limit was reached)\n",
            n, (unsigned long long)lim);
}
