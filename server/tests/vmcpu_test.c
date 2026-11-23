/* vmcpu_test.c — what the vCPUs use (src/vm/vmcpu.c): the stat line read as the kernel writes it, QEMU's vCPU thread
 * names, the windows of five minutes (average, busiest minute, a computer that slept, a pause, the last short window),
 * the rolling limit, and cpu-usage.json written, read back and refused when malformed or too old. Also this process's
 * own threads through vmcpu_open/vmcpu_ticks (a thread named like a vCPU is found and its time read). */
#include "../src/vm/vmcpu.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void test_parse(void) {
    const char *l = "4242 (CPU 3/KVM) S 4100 4100 4100 0 -1 4194624 1234 0 0 0 1500 250 0 0 20 0 26 0 77 0 0 18446744073709551615\n";
    char comm[32];
    unsigned long long t = 0;
    CHECK(!vmcpu_stat_parse(l, strlen(l), comm, sizeof comm, &t));
    CHECK(!strcmp(comm, "CPU 3/KVM"));
    CHECK(t == 1750);
    /* a name with ") " in it ends at the last parenthesis */
    const char *m = "7 (a) b (c)) R 1 1 1 0 -1 0 0 0 0 0 10 20 0 0\n";
    CHECK(!vmcpu_stat_parse(m, strlen(m), comm, sizeof comm, &t) && t == 30 && !strcmp(comm, "a) b (c)"));
    const char *cut = "7 (x) R 1 1 1 0 -1 0 0 0 0 0 10";
    CHECK(vmcpu_stat_parse(cut, strlen(cut), comm, sizeof comm, &t) == -1);
    const char *bad = "7 (x) R 1 1 1 0 -1 0 0 0 0 0 1x 20 0";
    CHECK(vmcpu_stat_parse(bad, strlen(bad), NULL, 0, &t) == -1);
    CHECK(vmcpu_stat_parse("no parens", 9, NULL, 0, &t) == -1);
    CHECK(vmcpu_is_vcpu("CPU 0/KVM") && vmcpu_is_vcpu("CPU 15/TCG"));
    CHECK(!vmcpu_is_vcpu("qemu-system-x86") && !vmcpu_is_vcpu("CPU x/KVM") && !vmcpu_is_vcpu("CPU 1") && !vmcpu_is_vcpu("CPU 1/") && !vmcpu_is_vcpu("IO mon_iothread"));
}

/* minutes of samples at `cores` cores (hz 100) */
static long long feed(VmCpu *c, long long *ms, long long ticks, int minutes, double cores, long long wall, int *closed) {
    for (int i = 0; i < minutes; i++) {
        *ms += VMCPU_SAMPLE_MS;
        ticks += (long long)(cores * 60 * 100 + 0.5);
        *closed += vmcpu_sample(c, *ms, ticks, 100, wall + i * 60);
    }
    return ticks;
}

static void test_windows(void) {
    VmCpu c;
    vmcpu_init(&c, 10);
    long long ms = 1000000, t = 5000;
    int closed = 0;
    CHECK(!vmcpu_sample(&c, ms, t, 100, 1700000000));               /* the first sample only sets the base */
    t = feed(&c, &ms, t, 4, 0.2, 1700000000, &closed);
    t = feed(&c, &ms, t, 1, 2.0, 1700000000, &closed);              /* the fifth minute closes the window */
    CHECK(closed == 1 && c.n == 1);
    CHECK(c.w[0].secs == 300 && c.w[0].vcpus == 10);
    CHECK(c.w[0].avg == 56);                                        /* (4 × 0.2 + 2) / 5 = 0.56 cores */
    CHECK(c.w[0].peak == 200);
    /* more than the vCPUs (rounding, a late read) is counted as all of them */
    t = feed(&c, &ms, t, 5, 12.0, 1700000300, &closed);
    CHECK(c.n == 2 && c.w[1].avg == 1000 && c.w[1].peak == 1000);
    /* the computer slept 10 minutes: that sample only starts over */
    ms += 10 * 60000; t += 100;
    CHECK(!vmcpu_sample(&c, ms, t, 100, 1700001000) && c.win_ms == 0);
    /* paused: held, then the next sample starts over */
    t = feed(&c, &ms, t, 2, 1.0, 1700001100, &closed);
    vmcpu_hold(&c);
    ms += 60000; t += 99999;
    CHECK(!vmcpu_sample(&c, ms, t, 100, 1700001200));
    CHECK(c.win_ms == 120000);                                      /* the two minutes before stay */
    /* fewer ticks than the last (threads changed): starts over */
    CHECK(!vmcpu_sample(&c, ms + 60000, t - 10, 100, 1700001300) && c.win_ms == 120000);
    ms += 60000; t -= 10;
    /* the run ends with two minutes in the window: kept */
    CHECK(vmcpu_flush(&c, 1700001400) == 1 && c.n == 3 && c.w[2].secs == 120 && c.w[2].avg == 100);
    /* less than a minute: dropped */
    t = feed(&c, &ms, t, 0, 0, 0, &closed);
    CHECK(!vmcpu_sample(&c, ms, t, 100, 1700001500));
    CHECK(vmcpu_sample(&c, ms + 30000, t + 10, 100, 1700001530) == 0 && c.win_ms == 30000);
    CHECK(vmcpu_flush(&c, 1700001540) == 0 && c.n == 3 && c.win_ms == 0);
}

static void test_keep(void) {
    VmCpu c;
    vmcpu_init(&c, 2);
    long long ms = 0, t = 0, wall = 1700000000;
    int closed = 0;
    vmcpu_sample(&c, ms, t, 100, wall);
    for (int i = 0; i < VMCPU_KEEP + 10; i++) t = feed(&c, &ms, t, 5, (i % 3) * 0.5, wall + i * 300, &closed);
    CHECK(closed == VMCPU_KEEP + 10 && c.n == VMCPU_KEEP);
    CHECK(c.w[0].t == wall + 10 * 300 + 4 * 60);                    /* the oldest ten went */
}

static void test_file(void) {
    VmCpu c, d;
    long long wall = 1700000000;
    vmcpu_init(&c, 4);
    c.w[0] = (VmCpuWin){ wall - 600, 300, 4, 37, 120 };
    c.w[1] = (VmCpuWin){ wall - 300, 300, 4, 12, 40 };
    c.n = 2;
    char buf[64 + VMCPU_KEEP * 64];
    size_t n = vmcpu_json(&c, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "\"format\":1"));
    vmcpu_init(&d, 4);
    CHECK(!vmcpu_load(&d, buf, n, wall) && d.n == 2 && d.w[1].t == wall - 300 && d.w[0].avg == 37 && d.w[0].peak == 120);
    CHECK(vmcpu_json(&c, buf, 20) == 0);                            /* too small */
    /* written and read back */
    char dir[] = "/tmp/vmcpu-test-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    CHECK(!vmcpu_save(&c, dir));
    char path[128];
    snprintf(path, sizeof path, "%s/%s", dir, VMCPU_FILE);
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    if (f) { size_t k = fread(buf, 1, sizeof buf, f); fclose(f); vmcpu_init(&d, 4); CHECK(!vmcpu_load(&d, buf, k, wall) && d.n == 2); }
    unlink(path);
    rmdir(dir);
    /* malformed entries dropped, the rest kept; old ones dropped; other files refused */
    const char *mix = "{\"format\":1,\"windows\":[[1699999000,300,4,50,90],[1699999000,300,4,500,90],[1699999000,300,0,1,1],"
                      "[1699999000,300,4,\"x\",1],[1,300,4,1,1],[1699999300,300,4],[1699999600,60,4,400,400]]}";
    vmcpu_init(&d, 4);
    CHECK(!vmcpu_load(&d, mix, strlen(mix), wall) && d.n == 2 && d.w[0].avg == 50 && d.w[1].secs == 60);
    const char *old = "{\"format\":1,\"windows\":[[1690000000,300,4,50,90]]}";
    CHECK(!vmcpu_load(&d, old, strlen(old), wall) && d.n == 0);
    CHECK(vmcpu_load(&d, "{\"format\":2,\"windows\":[]}", 25, wall) == -1);
    CHECK(vmcpu_load(&d, "[", 1, wall) == -1 && d.n == 0);
}

/* a thread named like a vCPU, busy for a while */
static volatile int stop;
static void *spin(void *a) { (void)a; while (!stop) { } return NULL; }

static void test_proc(void) {
    pthread_t th;
    CHECK(!pthread_create(&th, NULL, spin, NULL));
    pthread_setname_np(th, "CPU 0/KVM");
    int fds[VMCPU_FDS];
    int n = vmcpu_open((long)getpid(), fds, VMCPU_FDS);
    CHECK(n == 1);
    long long a = vmcpu_ticks(fds, n);
    struct timespec ts = { 0, 300 * 1000000 };
    nanosleep(&ts, NULL);
    long long b = vmcpu_ticks(fds, n);
    CHECK(a >= 0 && b > a);                                         /* it spun: its time moved on */
    stop = 1;
    pthread_join(th, NULL);
    /* the thread ended: once the kernel has let it go (a joined thread may be gone a moment later), it can't be read */
    long long gone = 0;
    for (int i = 0; i < 200 && (gone = vmcpu_ticks(fds, n)) != -1; i++) { struct timespec w = { 0, 5 * 1000000 }; nanosleep(&w, NULL); }
    CHECK(gone == -1);
    for (int i = 0; i < n; i++) close(fds[i]);
    CHECK(vmcpu_ticks(fds, 0) == -1);
}

int main(void) {
    test_parse();
    test_windows();
    test_keep();
    test_file();
    test_proc();
    if (fails) { fprintf(stderr, "vmcpu-test: %d failed\n", fails); return 1; }
    printf("vmcpu-test: ok\n");
    return 0;
}
