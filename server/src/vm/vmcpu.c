/* vmcpu.c — how much of its vCPUs a machine really uses (vmcpu.h) */
#include "vmcpu.h"
#include "../sec/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int vmcpu_stat_parse(const char *buf, size_t n, char *comm, size_t cn, unsigned long long *ticks) {
    /* "tid (comm) state ppid ... utime stime ...": comm may hold spaces and parentheses, so it ends at the last ')' */
    const char *open = memchr(buf, '(', n), *close = NULL;
    for (const char *p = buf + n; p > buf; p--) if (p[-1] == ')') { close = p - 1; break; }
    if (!open || !close || close < open) return -1;
    if (comm && cn) {
        size_t k = (size_t)(close - open - 1);
        if (k >= cn) k = cn - 1;
        memcpy(comm, open + 1, k);
        comm[k] = 0;
    }
    /* after ") ": field 3 (state) onwards; utime and stime are fields 14 and 15 */
    const char *p = close + 1, *end = buf + n;
    unsigned long long v[2] = { 0, 0 };
    for (int field = 3; field <= 15; field++) {
        while (p < end && *p == ' ') p++;
        if (p >= end) return -1;
        const char *s = p;
        while (p < end && *p != ' ' && *p != '\n') p++;
        if (field >= 14) {
            unsigned long long x = 0;
            for (const char *q = s; q < p; q++) {
                if (*q < '0' || *q > '9') return -1;
                x = x * 10 + (unsigned)(*q - '0');
            }
            if (p == s) return -1;
            v[field - 14] = x;
        }
    }
    if (ticks) *ticks = v[0] + v[1];
    return 0;
}

int vmcpu_is_vcpu(const char *comm) {
    if (!comm || strncmp(comm, "CPU ", 4)) return 0;
    const char *p = comm + 4;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') p++;
    return *p == '/' && p[1];
}

int vmcpu_open(long pid, int *fds, int max) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%ld/task", pid);
    DIR *d = opendir(path);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9' || strlen(e->d_name) > 12) continue;
        char sp[96];
        snprintf(sp, sizeof sp, "/proc/%ld/task/%s/stat", pid, e->d_name);
        int fd = open(sp, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char buf[512], comm[32];
        ssize_t k = pread(fd, buf, sizeof buf, 0);
        unsigned long long t;
        if (k > 0 && !vmcpu_stat_parse(buf, (size_t)k, comm, sizeof comm, &t) && vmcpu_is_vcpu(comm)) fds[n++] = fd;
        else close(fd);
    }
    closedir(d);
    return n;
}

long long vmcpu_ticks(const int *fds, int n) {
    if (n <= 0) return -1;
    long long sum = 0;
    for (int i = 0; i < n; i++) {
        char buf[512];
        ssize_t k = pread(fds[i], buf, sizeof buf, 0);
        unsigned long long t;
        if (k <= 0 || vmcpu_stat_parse(buf, (size_t)k, NULL, 0, &t)) return -1;
        sum += (long long)t;
    }
    return sum;
}

void vmcpu_init(VmCpu *c, int vcpus) {
    memset(c, 0, sizeof *c);
    c->vcpus = vcpus < 1 ? 1 : vcpus;
}

static void push(VmCpu *c, const VmCpuWin *w) {
    if (c->n == VMCPU_KEEP) { memmove(c->w, c->w + 1, sizeof c->w[0] * (VMCPU_KEEP - 1)); c->n--; }
    c->w[c->n++] = *w;
}

static void close_window(VmCpu *c, long long wall) {
    VmCpuWin w = { .t = wall, .secs = (int)((c->win_ms + 500) / 1000), .vcpus = c->vcpus,
                   .avg = (int)(c->core_ms * 100.0 / (double)c->win_ms + 0.5), .peak = c->peak };
    if (w.avg > c->vcpus * 100) w.avg = c->vcpus * 100;
    if (w.peak < w.avg) w.peak = w.avg;
    push(c, &w);
    c->win_ms = 0; c->core_ms = 0; c->peak = 0;
}

void vmcpu_hold(VmCpu *c) { c->have_base = 0; }

int vmcpu_sample(VmCpu *c, long long now_ms, long long ticks, long hz, long long wall) {
    if (ticks < 0 || hz <= 0) { c->have_base = 0; return 0; }
    long long dt = now_ms - c->base_ms;
    int fresh = !c->have_base || dt <= 0 || dt > 3LL * VMCPU_SAMPLE_MS || ticks < c->base_ticks;
    long long dticks = ticks - c->base_ticks;
    c->base_ticks = ticks; c->base_ms = now_ms; c->have_base = 1;
    if (fresh) return 0;
    double cores = (double)dticks / (double)hz / ((double)dt / 1000.0);
    if (cores > c->vcpus) cores = c->vcpus;
    c->core_ms += cores * (double)dt;
    c->win_ms += dt;
    int pc = (int)(cores * 100.0 + 0.5);
    if (pc > c->peak) c->peak = pc;
    if (c->win_ms < VMCPU_WINDOW_S * 1000LL) return 0;
    close_window(c, wall);
    return 1;
}

int vmcpu_flush(VmCpu *c, long long wall) {
    c->have_base = 0;
    if (c->win_ms < 60000) { c->win_ms = 0; c->core_ms = 0; c->peak = 0; return 0; }
    close_window(c, wall);
    return 1;
}

int vmcpu_load(VmCpu *c, const char *text, size_t n, long long wall) {
    c->n = 0;
    JDoc d;
    if (!text || json_parse(text, n, &d, NULL, 0)) return -1;
    const JVal *ws = json_get(d.root, "windows");
    if ((int)json_num(d.root, "format", 0) != VMCPU_FORMAT || !ws || ws->type != J_ARR) { json_free(&d); return -1; }
    for (const JVal *e = ws->kid; e; e = e->next) {
        double v[5];
        int k = 0;
        if (e->type != J_ARR || e->len != 5) continue;
        for (const JVal *x = e->kid; x && k < 5; x = x->next) { if (x->type != J_NUM) break; v[k++] = x->num; }
        if (k != 5) continue;
        VmCpuWin w = { .t = (long long)v[0], .secs = (int)v[1], .vcpus = (int)v[2], .avg = (int)v[3], .peak = (int)v[4] };
        if (w.t <= 0 || w.t > wall + 86400 || wall - w.t > VMCPU_MAX_AGE_S) continue;
        if (w.secs < 1 || w.secs > 4 * VMCPU_WINDOW_S || w.vcpus < 1 || w.vcpus > VMCPU_FDS) continue;
        if (w.avg < 0 || w.peak < 0 || w.avg > w.vcpus * 100 || w.peak > w.vcpus * 100) continue;
        push(c, &w);
    }
    json_free(&d);
    return 0;
}

size_t vmcpu_json(const VmCpu *c, char *out, size_t n) {
    size_t o = 0;
    int k = snprintf(out, n, "{\"format\":%d,\"windows\":[", VMCPU_FORMAT);
    if (k < 0 || (size_t)k >= n) return 0;
    o = (size_t)k;
    for (int i = 0; i < c->n; i++) {
        const VmCpuWin *w = &c->w[i];
        k = snprintf(out + o, n - o, "%s\n[%lld,%d,%d,%d,%d]", i ? "," : "", w->t, w->secs, w->vcpus, w->avg, w->peak);
        if (k < 0 || (size_t)k >= n - o) return 0;
        o += (size_t)k;
    }
    k = snprintf(out + o, n - o, "]}\n");
    if (k < 0 || (size_t)k >= n - o) return 0;
    return o + (size_t)k;
}

int vmcpu_save(const VmCpu *c, const char *dir) {
    static char buf[64 + VMCPU_KEEP * 64];
    size_t len = vmcpu_json(c, buf, sizeof buf);
    if (!len) return -1;
    char tmp[PATH_MAX], path[PATH_MAX];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s/.%s.tmp", dir, VMCPU_FILE) >= sizeof tmp ||
        (size_t)snprintf(path, sizeof path, "%s/%s", dir, VMCPU_FILE) >= sizeof path) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        off += (size_t)w;
    }
    int ok = off == len;
    if (close(fd)) ok = 0;
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}
