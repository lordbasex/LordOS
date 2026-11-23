/* vmpin.c — vCPU pinning: the host's topology, the choice of CPUs and setting it on QEMU's threads (vmpin.h) */
#include "vmpin.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- the topology ---- */

static int read_text(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r <= 0) return -1;
    buf[r] = 0;
    return 0;
}

static int read_int(const char *path, long *v) {
    char b[64], *e;
    if (read_text(path, b, sizeof b)) return -1;
    errno = 0;
    long x = strtol(b, &e, 10);
    if (e == b || errno || x < 0) return -1;
    *v = x;
    return 0;
}

/* "0-3,8,10-11\n" into s; 0 ok, -1 malformed */
static int parse_list(const char *txt, cpu_set_t *s) {
    CPU_ZERO(s);
    const char *p = txt;
    while (*p && *p != '\n') {
        if (!isdigit((unsigned char)*p)) return -1;
        char *e;
        long a = strtol(p, &e, 10), b = a;
        p = e;
        if (*p == '-') { p++; if (!isdigit((unsigned char)*p)) return -1; b = strtol(p, &e, 10); p = e; }
        if (a < 0 || b < a || b >= VMPIN_CPUS) return -1;
        for (long i = a; i <= b; i++) CPU_SET((int)i, s);
        if (*p == ',') p++;
        else if (*p && *p != '\n') return -1;
    }
    return 0;
}

int vmpin_topo_read(const char *root, VmTopo *t) {
    char p[512], b[4096];
    memset(t, 0, sizeof *t);
    cpu_set_t on, pcores;
    snprintf(p, sizeof p, "%s/devices/system/cpu/online", root);
    if (read_text(p, b, sizeof b) || parse_list(b, &on)) return -1;
    /* Intel hybrid without cpu_capacity: the cpu_core PMU lists the P cores (cpu_atom the E cores) */
    int pmu = 0;
    snprintf(p, sizeof p, "%s/devices/cpu_atom/cpus", root);
    if (!read_text(p, b, sizeof b)) {
        snprintf(p, sizeof p, "%s/devices/cpu_core/cpus", root);
        if (!read_text(p, b, sizeof b) && !parse_list(b, &pcores)) pmu = 1;
    }
    for (int cpu = 0; cpu < VMPIN_CPUS; cpu++) {
        if (!CPU_ISSET(cpu, &on)) continue;
        long pkg, core, cap = 0;
        snprintf(p, sizeof p, "%s/devices/system/cpu/cpu%d/topology/physical_package_id", root, cpu);
        if (read_int(p, &pkg)) continue;
        snprintf(p, sizeof p, "%s/devices/system/cpu/cpu%d/topology/core_id", root, cpu);
        if (read_int(p, &core)) continue;
        snprintf(p, sizeof p, "%s/devices/system/cpu/cpu%d/cpu_capacity", root, cpu);
        if (read_int(p, &cap)) {
            if (pmu) cap = CPU_ISSET(cpu, &pcores) ? 1024 : 512;
            else {
                snprintf(p, sizeof p, "%s/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", root, cpu);
                if (read_int(p, &cap)) cap = 0;
            }
        }
        if (pkg > 1000000 || core > 1000000000L || cap > 1000000000L) continue;
        t->c[t->n].cpu = cpu; t->c[t->n].pkg = (int)pkg; t->c[t->n].core = (int)core; t->c[t->n].cap = (int)cap;
        t->n++;
    }
    int lo = 0, hi = 0;
    for (int i = 0; i < t->n; i++) {
        if (t->c[i].cap > hi) hi = t->c[i].cap;
        if (t->c[i].cap && (!lo || t->c[i].cap < lo)) lo = t->c[i].cap;
    }
    t->hybrid = hi && (long)lo * 100 < (long)hi * VMPIN_EFF_PCT;
    return t->n ? 0 : -1;
}

/* ---- the choice ---- */

typedef struct {
    int pkg, core, cap, first, perf, taken, reserved, used;
    int n, cpus[8];                 /* its allowed threads, ascending */
} Core;

static int cores_of(const VmTopo *t, const cpu_set_t *allowed, const cpu_set_t *taken, Core *c, int max) {
    int n = 0;
    for (int i = 0; i < t->n; i++) {
        int cpu = t->c[i].cpu;
        if (cpu < 0 || cpu >= VMPIN_CPUS || !CPU_ISSET(cpu, allowed)) continue;
        int k;
        for (k = 0; k < n; k++) if (c[k].pkg == t->c[i].pkg && c[k].core == t->c[i].core) break;
        if (k == n) {
            if (n == max) continue;
            memset(&c[n], 0, sizeof c[n]);
            c[n].pkg = t->c[i].pkg; c[n].core = t->c[i].core; c[n].first = cpu;
            n++;
        }
        Core *o = &c[k];
        if (o->n < 8) {
            int j = o->n++;
            while (j > 0 && o->cpus[j - 1] > cpu) { o->cpus[j] = o->cpus[j - 1]; j--; }
            o->cpus[j] = cpu;
        }
        if (cpu < o->first) o->first = cpu;
        if (t->c[i].cap > o->cap) o->cap = t->c[i].cap;
        if (taken && CPU_ISSET(cpu, taken)) o->taken = 1;
    }
    int hi = 0;
    for (int k = 0; k < n; k++) if (c[k].cap > hi) hi = c[k].cap;
    for (int k = 0; k < n; k++) c[k].perf = !hi || !c[k].cap || (long)c[k].cap * 100 >= (long)hi * VMPIN_EFF_PCT;
    return n;
}

/* the machine's order: P before E, the faster first, then by number */
static int rank_cmp(const void *a, const void *b) {
    const Core *x = *(Core *const *)a, *y = *(Core *const *)b;
    if (x->perf != y->perf) return y->perf - x->perf;
    if (x->cap != y->cap) return y->cap > x->cap ? 1 : -1;
    return x->first - y->first;
}

static int first_cmp(const void *a, const void *b) {
    const Core *x = *(Core *const *)a, *y = *(Core *const *)b;
    return x->first - y->first;
}

static int count_physical(const VmTopo *t, const cpu_set_t *allowed) {
    static Core c[VMPIN_CPUS];
    return cores_of(t, allowed, NULL, c, VMPIN_CPUS);
}

int vmpin_plan(const VmTopo *t, const cpu_set_t *allowed, const cpu_set_t *taken, int nvcpu, VmPinPlan *p) {
    static Core c[VMPIN_CPUS];
    static Core *order[VMPIN_CPUS];
    memset(p, 0, sizeof *p);
    p->nv = nvcpu;
    CPU_ZERO(&p->rest);
    if (nvcpu < 1 || nvcpu > VMPIN_VCPUS) { snprintf(p->why, sizeof p->why, "%d vCPUs", nvcpu); return p->mode; }
    int n = cores_of(t, allowed, taken, c, VMPIN_CPUS);
    p->cores = n;
    if (n < 3) { snprintf(p->why, sizeof p->why, "%d physical core%s: too few to keep one for the desktop", n, n == 1 ? "" : "s"); return p->mode; }
    int reserve = n >= 6 ? 2 : 1;
    p->reserved = reserve;
    /* the desktop's cores: the lowest-numbered P cores, unless that leaves the machine fewer than 2 P cores (then E
     * cores, when there are enough of them) */
    int np = 0;
    for (int k = 0; k < n; k++) np += c[k].perf;
    int from_perf = np >= reserve + 2 || n - np < reserve;
    for (int k = 0; k < n; k++) order[k] = &c[k];
    qsort(order, (size_t)n, sizeof order[0], first_cmp);
    int got = 0;
    for (int pass = 0; pass < 2 && got < reserve; pass++)
        for (int k = 0; k < n && got < reserve; k++)
            if (!order[k]->reserved && (pass || order[k]->perf == from_perf)) { order[k]->reserved = 1; got++; }
    /* the machine's cores, best first */
    int m = 0;
    for (int k = 0; k < n; k++) if (!c[k].reserved && !c[k].taken) order[m++] = &c[k];
    qsort(order, (size_t)m, sizeof order[0], rank_cmp);
    int slots = 0;
    for (int k = 0; k < m; k++) slots += order[k]->n;
    if (nvcpu > slots) {
        int tk = 0;
        for (int k = 0; k < n; k++) tk += c[k].taken;
        if (tk) snprintf(p->why, sizeof p->why, "%d vCPUs, %d free CPUs (other machines hold %d core%s)", nvcpu, slots, tk, tk == 1 ? "" : "s");
        else snprintf(p->why, sizeof p->why, "%d vCPUs, %d CPUs beside the desktop's %d core%s", nvcpu, slots, reserve, reserve == 1 ? "" : "s");
        return p->mode;
    }
    cpu_set_t used;
    CPU_ZERO(&used);
    int v = 0;
    for (int th = 0; v < nvcpu && th < 8; th++)
        for (int k = 0; k < m && v < nvcpu; k++)
            if (th < order[k]->n) { p->vcpu_cpu[v++] = order[k]->cpus[th]; order[k]->used++; CPU_SET(order[k]->cpus[th], &used); }
    for (int k = 0; k < m; k++) if (order[k]->used > 1) p->shared += order[k]->used;
    /* the other threads: what no vCPU uses, without the vCPUs' siblings and other machines' cores while some is left */
    cpu_set_t rest, strict;
    CPU_ZERO(&rest); CPU_ZERO(&strict);
    for (int k = 0; k < n; k++)
        for (int j = 0; j < c[k].n; j++) {
            if (CPU_ISSET(c[k].cpus[j], &used)) continue;
            CPU_SET(c[k].cpus[j], &rest);
            if (!c[k].used && !c[k].taken) CPU_SET(c[k].cpus[j], &strict);
        }
    p->rest = CPU_COUNT(&strict) ? strict : rest;
    p->mode = VMPIN_EACH;
    int pc = 0;
    for (int k = 0; k < m; k++) pc += !!order[k]->used;
    snprintf(p->why, sizeof p->why, "%d vCPU%s on %d physical core%s%s, %d kept for the desktop", nvcpu, nvcpu == 1 ? "" : "s", pc,
             pc == 1 ? "" : "s", p->shared ? " (SMT siblings shared)" : "", reserve);
    return p->mode;
}

/* measured on a 10-core i9-10900 with a desktop-like load (docs/VIRTUALIZATION.md 3.7.1): with 8 vCPUs pinning cut the
 * host's frame p99 from 15.2 to 9.9 ms and its missed frames from 1.7% to 0.1%, and the guest ran 2.7% faster; with 4
 * the p99 went from 7.5 to 6.1 ms with the guest 0.6% slower; with 2 the guest was 1.7% slower: not worth it by default */
int vmpin_default_on(const VmTopo *t, const cpu_set_t *allowed, int nvcpu) {
    return nvcpu >= VMPIN_AUTO_VCPUS && count_physical(t, allowed) >= VMPIN_AUTO_CORES;
}

/* ---- the threads ---- */

static int is_num(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

void vmpin_taken(pid_t self_qemu, cpu_set_t *taken) {
    CPU_ZERO(taken);
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *e;
    uid_t me = getuid();
    while ((e = readdir(d))) {
        if (!is_num(e->d_name) || atoi(e->d_name) == self_qemu) continue;
        char p[300], comm[64];
        struct stat st;
        snprintf(p, sizeof p, "/proc/%s", e->d_name);
        if (stat(p, &st) || st.st_uid != me) continue;
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        if (read_text(p, comm, sizeof comm) || strncmp(comm, "qemu-system", 11)) continue;
        snprintf(p, sizeof p, "/proc/%s/task", e->d_name);
        DIR *td = opendir(p);
        if (!td) continue;
        struct dirent *te;
        while ((te = readdir(td))) {
            if (!is_num(te->d_name)) continue;
            cpu_set_t s;
            if (sched_getaffinity(atoi(te->d_name), sizeof s, &s) || CPU_COUNT(&s) != 1) continue;
            for (int cpu = 0; cpu < VMPIN_CPUS; cpu++) if (CPU_ISSET(cpu, &s)) { CPU_SET(cpu, taken); break; }
        }
        closedir(td);
    }
    closedir(d);
}

int vmpin_apply(const VmPinPlan *p, int task_dfd, const int *tids, int n) {
    if (p->mode != VMPIN_EACH || n != p->nv || task_dfd < 0) return -1;
    int set = 0;
    /* QEMU says which threads are vCPUs: only its own threads are believed */
    for (int i = 0; i < n; i++) {
        char name[16];
        snprintf(name, sizeof name, "%d", tids[i]);
        if (tids[i] <= 0 || faccessat(task_dfd, name, F_OK, 0)) return -1;
    }
    for (int i = 0; i < n; i++) {
        cpu_set_t s;
        CPU_ZERO(&s);
        CPU_SET(p->vcpu_cpu[i], &s);
        if (sched_setaffinity(tids[i], sizeof s, &s)) return -1;
        set++;
    }
    int fd = dup(task_dfd);
    DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
    if (!d) { if (fd >= 0) close(fd); return set; }
    rewinddir(d);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!is_num(e->d_name)) continue;
        int tid = atoi(e->d_name), vcpu = 0;
        for (int i = 0; i < n; i++) if (tids[i] == tid) vcpu = 1;
        if (vcpu) continue;
        if (!sched_setaffinity(tid, sizeof p->rest, &p->rest)) set++;
    }
    closedir(d);
    return set;
}

void vmpin_list(const cpu_set_t *s, char *out, size_t len) {
    size_t o = 0;
    if (len) out[0] = 0;
    for (int a = 0; a < VMPIN_CPUS; a++) {
        if (!CPU_ISSET(a, s)) continue;
        int b = a;
        while (b + 1 < VMPIN_CPUS && CPU_ISSET(b + 1, s)) b++;
        int w = b > a ? snprintf(out + o, len - o, "%s%d-%d", o ? "," : "", a, b) : snprintf(out + o, len - o, "%s%d", o ? "," : "", a);
        if (w < 0 || (size_t)w >= len - o) { if (len) out[o] = 0; return; }
        o += (size_t)w;
        a = b;
    }
}
