/* vmpin_test.c — vCPU pinning's choice of CPUs (vmpin.c) over invented computers: SMT, hybrid P/E, two packages, small
 * hosts, a restricted cpuset and other machines' pinned cores; the sysfs reader over invented trees; setting it on this
 * process's own threads. `vmpin-test --plan N` prints the plan for N vCPUs on this computer (measurements, the VM). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../src/vm/vmpin.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static VmTopo T;
static void add(int cpu, int pkg, int core, int cap) { T.c[T.n].cpu = cpu; T.c[T.n].pkg = pkg; T.c[T.n].core = core; T.c[T.n].cap = cap; T.n++; }
static void all(cpu_set_t *s) { CPU_ZERO(s); for (int i = 0; i < T.n; i++) CPU_SET(T.c[i].cpu, s); }
static void set_of(cpu_set_t *s, const char *list) {
    CPU_ZERO(s);
    for (const char *p = list; *p;) {
        char *e;
        long a = strtol(p, &e, 10), b = a;
        if (*e == '-') b = strtol(e + 1, &e, 10);
        for (long i = a; i <= b; i++) CPU_SET((int)i, s);
        p = *e == ',' ? e + 1 : e;
    }
}
static const char *vcpus(const VmPinPlan *p) {
    static char b[512];
    size_t o = 0;
    b[0] = 0;
    for (int i = 0; i < p->nv && p->mode == VMPIN_EACH; i++) o += (size_t)snprintf(b + o, sizeof b - o, "%s%d", i ? "," : "", p->vcpu_cpu[i]);
    return b;
}
static const char *rest(const VmPinPlan *p) { static char b[512]; vmpin_list(&p->rest, b, sizeof b); return b; }

/* i9-10900: 10 cores, 20 threads, CPU n and n+10 siblings, all the same speed */
static void i9(void) { memset(&T, 0, sizeof T); for (int c = 0; c < 10; c++) add(c, 0, c, 5200000); for (int c = 0; c < 10; c++) add(c + 10, 0, c, 5200000); }

static void test_smt(void) {
    VmPinPlan p;
    cpu_set_t a;
    i9(); all(&a);
    CHECK(vmpin_plan(&T, &a, NULL, 4, &p) == VMPIN_EACH, "i9 4: %s", p.why);
    CHECK(!strcmp(vcpus(&p), "2,3,4,5"), "i9 4 vcpus %s", vcpus(&p));
    CHECK(p.reserved == 2 && p.cores == 10 && !p.shared, "i9 4 reserved %d cores %d shared %d", p.reserved, p.cores, p.shared);
    CHECK(!strcmp(rest(&p), "0-1,6-11,16-19"), "i9 4 rest %s", rest(&p));
    vmpin_plan(&T, &a, NULL, 8, &p);
    CHECK(!strcmp(vcpus(&p), "2,3,4,5,6,7,8,9"), "i9 8 vcpus %s", vcpus(&p));
    CHECK(!strcmp(rest(&p), "0-1,10-11"), "i9 8 rest %s", rest(&p));
    /* past one per core: the siblings, in the same order */
    vmpin_plan(&T, &a, NULL, 10, &p);
    CHECK(!strcmp(vcpus(&p), "2,3,4,5,6,7,8,9,12,13"), "i9 10 vcpus %s", vcpus(&p));
    CHECK(p.shared == 4, "i9 10 shared %d", p.shared);
    vmpin_plan(&T, &a, NULL, 16, &p);
    CHECK(p.mode == VMPIN_EACH && !strcmp(rest(&p), "0-1,10-11"), "i9 16 %s rest %s", p.why, rest(&p));
    CHECK(vmpin_plan(&T, &a, NULL, 17, &p) == VMPIN_NONE && strstr(p.why, "17 vCPUs, 16 CPUs"), "i9 17: %s", p.why);
    CHECK(vmpin_plan(&T, &a, NULL, 0, &p) == VMPIN_NONE, "0 vCPUs");
    CHECK(vmpin_plan(&T, &a, NULL, VMPIN_VCPUS + 1, &p) == VMPIN_NONE, "65 vCPUs");
    /* 1 vCPU: still one core of its own */
    vmpin_plan(&T, &a, NULL, 1, &p);
    CHECK(!strcmp(vcpus(&p), "2"), "i9 1 vcpus %s", vcpus(&p));
}

/* 6 P cores with SMT (CPUs 0-11, siblings in pairs) and 8 E cores (12-19), the cpu_core/cpu_atom capacities */
static void hybrid(int pcores, int ecores) {
    memset(&T, 0, sizeof T);
    int cpu = 0;
    for (int c = 0; c < pcores; c++) { add(cpu++, 0, c * 4, 1024); add(cpu++, 0, c * 4, 1024); }
    for (int c = 0; c < ecores; c++) add(cpu++, 0, 100 + c, 512);
}

static void test_hybrid(void) {
    VmPinPlan p;
    cpu_set_t a;
    hybrid(6, 8); all(&a);
    vmpin_plan(&T, &a, NULL, 4, &p);
    CHECK(!strcmp(vcpus(&p), "4,6,8,10"), "6P8E 4 vcpus %s (P cores beside the desktop's two)", vcpus(&p));
    CHECK(!strcmp(rest(&p), "0-3,12-19"), "6P8E 4 rest %s", rest(&p));
    vmpin_plan(&T, &a, NULL, 8, &p);
    CHECK(!strcmp(vcpus(&p), "4,6,8,10,12,13,14,15"), "6P8E 8 vcpus %s (one per core: P, then E)", vcpus(&p));
    /* 2 P cores: the desktop keeps E cores and the machine gets the P ones */
    hybrid(2, 8); all(&a);
    vmpin_plan(&T, &a, NULL, 4, &p);
    CHECK(p.mode == VMPIN_EACH && !strcmp(vcpus(&p), "0,2,6,7"), "2P8E 4 vcpus %s (%s)", vcpus(&p), p.why);
    CHECK(!strcmp(rest(&p), "4-5,8-11"), "2P8E 4 rest %s", rest(&p));
    /* only E cores left in the cpuset: they're used as they are */
    hybrid(2, 8); set_of(&a, "4-11");
    vmpin_plan(&T, &a, NULL, 4, &p);
    CHECK(!strcmp(vcpus(&p), "6,7,8,9"), "E only 4 vcpus %s", vcpus(&p));
}

static void test_small(void) {
    VmPinPlan p;
    cpu_set_t a;
    memset(&T, 0, sizeof T);
    add(0, 0, 0, 0); add(1, 0, 1, 0); add(2, 0, 0, 0); add(3, 0, 1, 0);       /* 2 cores, 4 threads */
    all(&a);
    CHECK(vmpin_plan(&T, &a, NULL, 1, &p) == VMPIN_NONE && strstr(p.why, "2 physical cores"), "2 cores: %s", p.why);
    memset(&T, 0, sizeof T);
    for (int c = 0; c < 4; c++) add(c, 0, c, 0);                                 /* 4 cores, no SMT, no capacity */
    all(&a);
    vmpin_plan(&T, &a, NULL, 3, &p);
    CHECK(p.mode == VMPIN_EACH && p.reserved == 1 && !strcmp(vcpus(&p), "1,2,3") && !strcmp(rest(&p), "0"), "4 cores 3: %s %s", vcpus(&p), rest(&p));
    CHECK(vmpin_plan(&T, &a, NULL, 4, &p) == VMPIN_NONE, "4 cores 4: %s", p.why);
    memset(&T, 0, sizeof T);
    for (int c = 0; c < 6; c++) add(c, 0, c, 0);                                 /* 6 cores: 2 kept */
    all(&a);
    vmpin_plan(&T, &a, NULL, 4, &p);
    CHECK(p.reserved == 2 && !strcmp(vcpus(&p), "2,3,4,5"), "6 cores 4: %s", vcpus(&p));
    CHECK(vmpin_plan(&T, &a, NULL, 5, &p) == VMPIN_NONE, "6 cores 5");
}

static void test_cpuset(void) {
    VmPinPlan p;
    cpu_set_t a;
    i9(); set_of(&a, "0-3,10-13");                                              /* 4 cores of the 10 */
    vmpin_plan(&T, &a, NULL, 2, &p);
    CHECK(p.cores == 4 && p.reserved == 1 && !strcmp(vcpus(&p), "1,2"), "cpuset 2: %s", vcpus(&p));
    CHECK(!strcmp(rest(&p), "0,3,10,13"), "cpuset 2 rest %s", rest(&p));
    vmpin_plan(&T, &a, NULL, 6, &p);
    CHECK(!strcmp(vcpus(&p), "1,2,3,11,12,13"), "cpuset 6: %s", vcpus(&p));
    CHECK(!strcmp(rest(&p), "0,10"), "cpuset 6 rest %s", rest(&p));
    CHECK(vmpin_plan(&T, &a, NULL, 7, &p) == VMPIN_NONE, "cpuset 7");
    /* a cpuset that leaves only one thread of each core */
    set_of(&a, "4-9");
    vmpin_plan(&T, &a, NULL, 4, &p);
    CHECK(!strcmp(vcpus(&p), "6,7,8,9") && !strcmp(rest(&p), "4-5"), "cpuset no siblings: %s / %s", vcpus(&p), rest(&p));
    /* nothing pinned outside what's allowed */
    for (int i = 0; i < p.nv; i++) CHECK(CPU_ISSET(p.vcpu_cpu[i], &a), "vcpu %d outside the cpuset", p.vcpu_cpu[i]);
}

static void test_taken(void) {
    VmPinPlan p;
    cpu_set_t a, tk;
    i9(); all(&a); set_of(&tk, "2-5");                                           /* another machine's 4 vCPUs */
    vmpin_plan(&T, &a, &tk, 4, &p);
    CHECK(!strcmp(vcpus(&p), "6,7,8,9"), "taken 4: %s", vcpus(&p));
    CHECK(!strcmp(rest(&p), "0-1,10-11"), "taken 4 rest %s (no other machine's cores)", rest(&p));
    vmpin_plan(&T, &a, &tk, 6, &p);
    CHECK(!strcmp(vcpus(&p), "6,7,8,9,16,17"), "taken 6: %s", vcpus(&p));
    CHECK(vmpin_plan(&T, &a, &tk, 9, &p) == VMPIN_NONE && strstr(p.why, "other machines"), "taken 9: %s", p.why);
}

static void test_packages(void) {
    VmPinPlan p;
    cpu_set_t a;
    memset(&T, 0, sizeof T);
    for (int k = 0; k < 2; k++) for (int c = 0; c < 4; c++) add(k * 4 + c, k, c, 0);   /* core ids repeat per package */
    all(&a);
    vmpin_plan(&T, &a, NULL, 6, &p);
    CHECK(p.cores == 8 && p.reserved == 2 && !strcmp(vcpus(&p), "2,3,4,5,6,7"), "2 packages: %d cores, %s", p.cores, vcpus(&p));
}

static void test_default(void) {
    cpu_set_t a;
    i9(); all(&a);
    CHECK(vmpin_default_on(&T, &a, 4) && vmpin_default_on(&T, &a, 16) && !vmpin_default_on(&T, &a, 3), "default on i9");
    set_of(&a, "0-6,10-16");                                                    /* 7 cores, 14 threads */
    CHECK(!vmpin_default_on(&T, &a, 4), "default on 7 cores");
    set_of(&a, "0-7");
    CHECK(vmpin_default_on(&T, &a, 4), "default on 8 cores without their siblings");
    memset(&T, 0, sizeof T);
    for (int c = 0; c < 4; c++) add(c, 0, c, 0);
    all(&a);
    CHECK(!vmpin_default_on(&T, &a, 4), "default on a 4-core host");
}

static void test_list(void) {
    cpu_set_t s;
    char b[64];
    set_of(&s, "0-3,5,7-8"); vmpin_list(&s, b, sizeof b);
    CHECK(!strcmp(b, "0-3,5,7-8"), "list %s", b);
    CPU_ZERO(&s); vmpin_list(&s, b, sizeof b);
    CHECK(!b[0], "empty list");
    set_of(&s, "0,2,4,6,8,10,12,14"); vmpin_list(&s, b, 8);
    CHECK(strlen(b) < 8, "list truncated: %s", b);
}

/* ---- the sysfs reader ---- */

static char root[256];
static void put(const char *rel, const char *txt) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    mkdir(root, 0755);
    for (char *s = p + strlen(root) + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    fputs(txt, f);
    fclose(f);
}
static void cpu_files(int cpu, int pkg, int core, const char *capfile, const char *capval) {
    char rel[256], v[32];
    snprintf(rel, sizeof rel, "devices/system/cpu/cpu%d/topology/physical_package_id", cpu); snprintf(v, sizeof v, "%d\n", pkg); put(rel, v);
    snprintf(rel, sizeof rel, "devices/system/cpu/cpu%d/topology/core_id", cpu); snprintf(v, sizeof v, "%d\n", core); put(rel, v);
    if (capfile) { snprintf(rel, sizeof rel, "devices/system/cpu/cpu%d/%s", cpu, capfile); put(rel, capval); }
}

static void test_sysfs(void) {
    VmTopo t;
    /* SMT, the same max frequency everywhere, CPU 4-5 offline */
    snprintf(root, sizeof root, "/tmp/vmpin-test-%d-a", (int)getpid());
    put("devices/system/cpu/online", "0-3,6-7\n");
    for (int c = 0; c < 8; c++) cpu_files(c, 0, c % 4, "cpufreq/cpuinfo_max_freq", "4000000\n");
    CHECK(!vmpin_topo_read(root, &t) && t.n == 6 && !t.hybrid, "smt tree: n %d hybrid %d", t.n, t.hybrid);
    CHECK(t.c[4].cpu == 6 && t.c[4].core == 2 && t.c[4].cap == 4000000, "smt tree cpu6: %d %d %d", t.c[4].cpu, t.c[4].core, t.c[4].cap);
    /* Intel hybrid: cpu_core/cpu_atom */
    snprintf(root, sizeof root, "/tmp/vmpin-test-%d-b", (int)getpid());
    put("devices/system/cpu/online", "0-5\n");
    put("devices/cpu_core/cpus", "0-3\n");
    put("devices/cpu_atom/cpus", "4-5\n");
    for (int c = 0; c < 6; c++) cpu_files(c, 0, c < 4 ? c / 2 : 10 + c, NULL, NULL);
    CHECK(!vmpin_topo_read(root, &t) && t.n == 6 && t.hybrid && t.c[0].cap == 1024 && t.c[5].cap == 512, "pmu tree: n %d hybrid %d", t.n, t.hybrid);
    /* arm64: cpu_capacity, which wins over cpufreq */
    snprintf(root, sizeof root, "/tmp/vmpin-test-%d-c", (int)getpid());
    put("devices/system/cpu/online", "0-7\n");
    for (int c = 0; c < 8; c++) {
        cpu_files(c, 0, c, "cpu_capacity", c < 4 ? "446\n" : "1024\n");
        char rel[128]; snprintf(rel, sizeof rel, "devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c); put(rel, "1800000\n");
    }
    CHECK(!vmpin_topo_read(root, &t) && t.hybrid && t.c[0].cap == 446 && t.c[7].cap == 1024, "capacity tree");
    VmPinPlan p;
    cpu_set_t a;
    CPU_ZERO(&a); for (int c = 0; c < 8; c++) CPU_SET(c, &a);
    vmpin_plan(&t, &a, NULL, 2, &p);
    CHECK(!strcmp(vcpus(&p), "6,7"), "big.LITTLE 2: %s (big cores beside the desktop's two)", vcpus(&p));
    vmpin_plan(&t, &a, NULL, 4, &p);
    CHECK(!strcmp(vcpus(&p), "6,7,0,1"), "big.LITTLE 4: %s (big, then little)", vcpus(&p));
    /* malformed: nothing read */
    snprintf(root, sizeof root, "/tmp/vmpin-test-%d-d", (int)getpid());
    put("devices/system/cpu/online", "0-x\n");
    CHECK(vmpin_topo_read(root, &t), "malformed online");
    put("devices/system/cpu/online", "3-1\n");
    CHECK(vmpin_topo_read(root, &t), "reversed range");
    put("devices/system/cpu/online", "0-99999\n");
    CHECK(vmpin_topo_read(root, &t), "range past VMPIN_CPUS");
    CHECK(vmpin_topo_read("/nonexistent", &t), "no tree");
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf /tmp/vmpin-test-%d-a /tmp/vmpin-test-%d-b /tmp/vmpin-test-%d-c /tmp/vmpin-test-%d-d", (int)getpid(), (int)getpid(), (int)getpid(), (int)getpid());
    if (system(cmd)) {}
}

/* ---- on real threads: this process's own ---- */

static volatile int stop_spin;
static volatile int spin_tid;
static void *spin(void *x) { (void)x; spin_tid = (int)syscall(SYS_gettid); while (!stop_spin) usleep(1000); return NULL; }

static void test_apply(void) {
    VmTopo t;
    cpu_set_t a;
    if (vmpin_topo_read("/sys", &t) || sched_getaffinity(0, sizeof a, &a)) { printf("apply: skipped: no topology\n"); return; }
    VmPinPlan p;
    if (vmpin_plan(&t, &a, NULL, 1, &p) != VMPIN_EACH) { printf("apply: skipped: %s\n", p.why); return; }
    pthread_t th;
    pthread_create(&th, NULL, spin, NULL);
    while (!spin_tid) usleep(1000);
    int dfd = open("/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int bad = 1;                                                        /* pid 1 is no thread of ours */
    CHECK(vmpin_apply(&p, dfd, &bad, 1) == -1, "a thread outside the process is refused");
    int tid = spin_tid;
    int r = vmpin_apply(&p, dfd, &tid, 1);
    CHECK(r >= 2, "apply set %d threads", r);
    cpu_set_t s;
    CHECK(!sched_getaffinity(tid, sizeof s, &s) && CPU_COUNT(&s) == 1 && CPU_ISSET(p.vcpu_cpu[0], &s), "the vCPU thread is on CPU %d", p.vcpu_cpu[0]);
    CHECK(!sched_getaffinity(0, sizeof s, &s) && CPU_EQUAL(&s, &p.rest), "the other thread is on the rest");
    stop_spin = 1;
    pthread_join(th, NULL);
    close(dfd);
    sched_setaffinity(0, sizeof a, &a);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--plan")) {
        VmTopo t;
        cpu_set_t a, tk;
        if (vmpin_topo_read("/sys", &t) || sched_getaffinity(0, sizeof a, &a)) { fprintf(stderr, "no topology\n"); return 1; }
        vmpin_taken(0, &tk);
        VmPinPlan p;
        vmpin_plan(&t, &a, &tk, atoi(argv[2]), &p);
        printf("%s\n%s\n%s\n", p.mode == VMPIN_EACH ? vcpus(&p) : "-", rest(&p), p.why);
        return p.mode == VMPIN_EACH ? 0 : 1;
    }
    test_smt();
    test_hybrid();
    test_small();
    test_cpuset();
    test_taken();
    test_packages();
    test_default();
    test_list();
    test_sysfs();
    test_apply();
    printf("vmpin-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
