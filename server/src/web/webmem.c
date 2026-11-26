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

/* pid and its descendants, breadth first, into taken[0..max): taken[0] is pid. Their count (1 when the scan can't be done). */
static int scan_tree(const char *proc, pid_t pid, pid_t *taken, int max) {
    int nt = 0;
    taken[nt++] = pid;
    /* every process's parent, once */
    Pair *all = malloc(sizeof *all * WEBMEM_MAX_SCAN);
    if (!all) return nt;
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
    for (int head = 0; head < nt; head++) {
        for (int i = 0; i < n && nt < max; i++) {
            if (all[i].ppid != taken[head]) continue;
            int seen = 0;
            for (int j = 0; j < nt && !seen; j++) seen = taken[j] == all[i].pid;
            if (!seen) taken[nt++] = all[i].pid;
        }
    }
    free(all);
    return nt;
}

int webmem_tree(const char *proc, pid_t pid, WebMem *out) {
    memset(out, 0, sizeof *out);
    if (pid <= 1) return -1;
    long long own = pss_of(proc, pid);
    if (own < 0) return -1;
    out->pss_kb = own;
    out->procs = 1;
    pid_t taken[WEBMEM_MAX_PROCS];
    int nt = scan_tree(proc, pid, taken, WEBMEM_MAX_PROCS);
    for (int i = 1; i < nt; i++) {
        long long k = pss_of(proc, taken[i]);
        if (k < 0) out->unreadable++;
        else { out->pss_kb += k; out->procs++; }
    }
    return 0;
}

int webmem_descendants(const char *proc, pid_t pid, pid_t *out, int max) {
    if (pid <= 1 || max <= 0) return 0;
    pid_t taken[WEBMEM_MAX_PROCS + 1];
    int nt = scan_tree(proc, pid, taken, max + 1 < WEBMEM_MAX_PROCS + 1 ? max + 1 : WEBMEM_MAX_PROCS + 1);
    for (int i = 1; i < nt; i++) out[i - 1] = taken[i];
    return nt - 1;
}

unsigned long long webmem_start_time(const char *proc, pid_t pid) {
    char path[512], buf[1024];
    if (pid <= 1 || snprintf(path, sizeof path, "%s/%d/stat", proc, (int)pid) >= (int)sizeof path) return 0;
    if (read_small(path, buf, sizeof buf) <= 0) return 0;
    char *p = strrchr(buf, ')');
    if (!p) return 0;
    /* after the name: state is field 3, starttime field 22 */
    p++;
    for (int field = 2; field < 22 && p; field++) {
        p = strchr(p, ' ');
        if (p) p++;
    }
    if (!p) return 0;
    char *end;
    unsigned long long v = strtoull(p, &end, 10);
    return end != p ? v : 0;
}
