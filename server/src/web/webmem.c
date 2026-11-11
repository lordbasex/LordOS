/* webmem.c — the Pss of a lord-web process tree (webmem.h). */
#include "webmem.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static ssize_t read_small(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < n - 1) {
        ssize_t r = read(fd, buf + got, n - 1 - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = 0;
    return (ssize_t)got;
}

long long webmem_pss_line(const char *text) {
    for (const char *p = text; p && *p; ) {
        if (!strncmp(p, "Pss:", 4)) {
            char *end;
            long long v = strtoll(p + 4, &end, 10);
            return end != p + 4 && v >= 0 ? v : -1;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return -1;
}

static long long pss_of(const char *proc, pid_t pid) {
    char path[512], buf[4096];
    if (snprintf(path, sizeof path, "%s/%d/smaps_rollup", proc, (int)pid) >= (int)sizeof path) return -1;
    if (read_small(path, buf, sizeof buf) <= 0) return -1;
    return webmem_pss_line(buf);
}

/* the parent in /proc/<pid>/stat (after the last ')': the name may hold spaces and parentheses), or 0 */
static pid_t parent_of(const char *proc, pid_t pid) {
    char path[512], buf[1024];
    if (snprintf(path, sizeof path, "%s/%d/stat", proc, (int)pid) >= (int)sizeof path) return 0;
    if (read_small(path, buf, sizeof buf) <= 0) return 0;
    char *e = strrchr(buf, ')');
    int ppid = 0;
    if (!e || sscanf(e + 1, " %*c %d", &ppid) != 1 || ppid < 0) return 0;
    return (pid_t)ppid;
}

typedef struct { pid_t pid, ppid; } Pair;

int webmem_tree(const char *proc, pid_t pid, WebMem *out) {
    memset(out, 0, sizeof *out);
    if (pid <= 1) return -1;
    long long own = pss_of(proc, pid);
    if (own < 0) return -1;
    out->pss_kb = own;
    out->procs = 1;

    /* every process's parent, once */
    Pair *all = malloc(sizeof *all * WEBMEM_MAX_SCAN);
    if (!all) return 0;
    int n = 0;
    DIR *d = opendir(proc);
    for (struct dirent *e; d && n < WEBMEM_MAX_SCAN && (e = readdir(d)); ) {
        char *end;
        long v = strtol(e->d_name, &end, 10);
        if (*end || end == e->d_name || v <= 1 || v == pid) continue;
        pid_t pp = parent_of(proc, (pid_t)v);
        if (pp > 0) all[n++] = (Pair){ (pid_t)v, pp };
    }
    if (d) closedir(d);

    /* breadth first from pid: a process is taken once (taken[] doubles as the queue) */
    pid_t taken[WEBMEM_MAX_PROCS];
    int nt = 0;
    taken[nt++] = pid;
    for (int head = 0; head < nt; head++) {
        for (int i = 0; i < n && nt < WEBMEM_MAX_PROCS; i++) {
            if (all[i].ppid != taken[head]) continue;
            int seen = 0;
            for (int j = 0; j < nt && !seen; j++) seen = taken[j] == all[i].pid;
            if (seen) continue;
            taken[nt++] = all[i].pid;
            long long k = pss_of(proc, all[i].pid);
            if (k < 0) out->unreadable++;
            else { out->pss_kb += k; out->procs++; }
        }
    }
    free(all);
    return 0;
}
