/* nvgpu_test.c — lord-nvgpu and lord-server's side of it (src/monitor/nvgpu.c, nvgpu_main.c; docs/MONITOR.md,
 * "NVIDIA"). No NVIDIA hardware needed: build/libnvfake.so (tests/nvgpu_fake_nvml.c) stands in for NVML, loaded by the
 * test-only build/lord-nvgpu-test through --lib.
 *   1. the parser and the JSON on hand-made messages: canned, out of range, bad headers and sizes;
 *   2. a fuzz of the parser (100k+ mutated messages; built with ASan and UBSan): it never crashes and its JSON
 *      always parses;
 *   3. the helper end to end through nvgpu_poll/nvgpu_json with the fake NVML: good, hostile, failing, init failing
 *      and hanging (killed, restarted with backoff, the caller never blocked), and stopped by nvgpu_want(0);
 *   4. the sandbox: after it, the helper can't make a socket, exec, write a file, read /etc or do a tty ioctl, and
 *      what it refused is recorded (syscall numbers, the ioctl's request) while the call still fails with EPERM;
 *   5. the failure report (nvDiag): the helper's codes (a missing symbol, dlopen, nvmlInit and the count failing, no
 *      driver), a refused syscall and ioctl counted with data still flowing, a helper killed by SIGSYS, a timeout;
 *      the fragment's JSON holds only numbers and fixed names. */
#include "monitor/nvgpu.h"
#include "sec/json.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int own_all(int pid, unsigned uid) { (void)uid; return pid != 1; }
static int own_even(int pid, unsigned uid) { (void)uid; return pid % 2 == 0; }

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int valid_json(const char *frag, JDoc *keep) {
    static char buf[256 * 1024];
    snprintf(buf, sizeof buf, "{%s}", frag);
    JDoc d;
    char err[128];
    if (json_parse(buf, strlen(buf), &d, err, sizeof err)) { fprintf(stderr, "bad json (%s): %.300s\n", err, buf); return 0; }
    if (keep) *keep = d; else json_free(&d);
    return 1;
}

static size_t make(unsigned char *m, int ngpu, int nproc) {
    NvHdr h = { .magic = NVP_MAGIC, .ngpu = (uint16_t)ngpu, .nproc = (uint16_t)nproc, .cuda = 12040, .status = 0 };
    strcpy(h.driver, "550.54.14");
    memcpy(m, &h, sizeof h);
    NvGpu *g = (NvGpu *)(m + sizeof h);
    for (int i = 0; i < ngpu; i++)
        g[i] = (NvGpu){ .domain = 0, .bus = 1 + i, .dev = 0, .fn = 0, .util_sm = 50, .util_mem = 20, .util_enc = 1, .util_dec = 2,
                        .temp = 60, .fan = 30, .clk_core = 1800, .clk_mem = 7000, .compute_mode = 0, .ecc = 1,
                        .power_mw = 123456, .power_limit_mw = 250000, .vram_used = 1LL << 30, .vram_total = 8LL << 30 };
    NvProc *p = (NvProc *)(g + ngpu);
    for (int i = 0; i < nproc; i++) p[i] = (NvProc){ .gpu = (uint32_t)(i % (ngpu ? ngpu : 1)), .pid = 100 + i, .sm = 5, .enc = NVP_NONE, .dec = 0, .mem = 1 << 20 };
    return sizeof h + ngpu * sizeof(NvGpu) + nproc * sizeof(NvProc);
}

static NvSample S;
static unsigned char M[NVP_MSG_MAX + 64];
static char J[128 * 1024];

static void unit(void) {
    size_t len = make(M, 2, 4);
    CHECK(!nvgpu_parse(M, len, 1000, own_all, &S), "parse canned");
    CHECK(S.ngpu == 2 && S.nproc == 4, "counts %d %d", S.ngpu, S.nproc);
    int n = nvgpu_format(&S, J, sizeof J);
    CHECK(n > 0 && valid_json(J, NULL), "format canned");
    CHECK(strstr(J, "\"bus\":\"0000:01:00.0\"") && strstr(J, "\"cuda\":\"12.4\"") && strstr(J, "\"driver\":\"550.54.14\""), "strings: %s", J);
    CHECK(strstr(J, "\"power\":123.456") && strstr(J, "\"powerLimit\":250.000") && strstr(J, "\"ecc\":true"), "power/ecc: %s", J);
    CHECK(strstr(J, "\"computeMode\":\"default\"") && strstr(J, "\"enc\":null"), "mode/null: %s", J);
    /* ownership: odd pids dropped */
    CHECK(!nvgpu_parse(M, len, 1000, own_even, &S) && S.nproc == 2, "own filter %d", S.nproc);
    CHECK(!nvgpu_parse(M, len, 1000, NULL, &S) && S.nproc == 0, "no owner check → no procs");
    /* out of range → null */
    NvGpu *g = (NvGpu *)(M + sizeof(NvHdr));
    g->util_sm = 101; g->temp = 151; g->fan = -2; g->clk_core = 100001; g->compute_mode = 4; g->ecc = 2;
    g->power_mw = 10000001; g->vram_total = (1LL << 50) + 1; g->vram_used = -5; g->bus = 256;
    NvProc *p = (NvProc *)(M + sizeof(NvHdr) + 2 * sizeof(NvGpu));
    p[0].gpu = 2; p[1].pid = 0; p[2].pid = 4194305; p[3].mem = INT64_MAX; p[3].sm = 1000;
    NvHdr *h = (NvHdr *)M;
    memset(h->driver, '9', sizeof h->driver);           /* no NUL */
    h->cuda = 999;
    CHECK(!nvgpu_parse(M, len, 1000, own_all, &S), "parse hostile");
    CHECK(S.nproc == 1 && S.proc[0].mem == NVP_NONE64 && S.proc[0].sm == NVP_NONE, "hostile procs %d", S.nproc);
    n = nvgpu_format(&S, J, sizeof J);
    CHECK(n > 0 && valid_json(J, NULL), "format hostile");
    CHECK(strstr(J, "\"bus\":null") && strstr(J, "\"temp\":null") && strstr(J, "\"fan\":null") && strstr(J, "\"cuda\":null") &&
          strstr(J, "\"driver\":null") && strstr(J, "\"computeMode\":null") && strstr(J, "\"ecc\":null") && strstr(J, "\"power\":null") &&
          strstr(J, "\"total\":null"), "hostile nulls: %.400s", J);
    make(M, 1, 0);
    strcpy(((NvHdr *)M)->driver, "550\n");
    CHECK(!nvgpu_parse(M, sizeof(NvHdr) + sizeof(NvGpu), 1000, own_all, &S) && !S.driver[0], "driver with a newline");
    /* bad headers and sizes */
    len = make(M, 1, 1);
    CHECK(nvgpu_parse(M, len - 1, 0, own_all, &S) < 0, "short");
    CHECK(nvgpu_parse(M, len + 1, 0, own_all, &S) < 0, "long");
    CHECK(nvgpu_parse(M, 3, 0, own_all, &S) < 0, "tiny");
    ((NvHdr *)M)->magic ^= 1;
    CHECK(nvgpu_parse(M, len, 0, own_all, &S) < 0, "magic");
    make(M, 1, 1);
    ((NvHdr *)M)->ngpu = 65;
    CHECK(nvgpu_parse(M, len, 0, own_all, &S) < 0, "65 gpus");
    make(M, 1, 1);
    ((NvHdr *)M)->status = 9;
    CHECK(!nvgpu_parse(M, len, 0, own_all, &S) && S.ngpu == 0 && nvgpu_format(&S, J, sizeof J) == 0 && !J[0], "status → nothing");
    /* the biggest sample fits in the JSON buffer; a small buffer gives 0 and nothing */
    len = make(M, NVP_MAX_GPUS, NVP_MAX_PROCS);
    CHECK(!nvgpu_parse(M, len, 0, own_all, &S) && S.ngpu == 64 && S.nproc == 256, "max");
    n = nvgpu_format(&S, J, sizeof J);
    CHECK(n > 0 && n < 128 * 1024 && valid_json(J, NULL), "max json %d", n);
    CHECK(nvgpu_format(&S, J, 100) == 0 && !J[0], "small buffer");
    CHECK(nvgpu_pid_owned(getpid(), (unsigned)getuid()) && !nvgpu_pid_owned(1, (unsigned)getuid()) == (getuid() != 0), "pid owner");

    /* the failure codes and the refusals in the header */
    len = make(M, 0, 0);
    h = (NvHdr *)M;
    h->code = NVP_ST_INIT; h->detail = 18; h->status = 18;
    CHECK(!nvgpu_parse(M, len, 0, own_all, &S) && S.ngpu == 0 && S.diag.code == NVP_ST_INIT && S.diag.has_detail && S.diag.detail == 18, "init code");
    h->code = NVP_ST_SANDBOX;                           /* only lord-server sets that one */
    CHECK(nvgpu_parse(M, len, 0, own_all, &S) < 0, "a code the helper may not send");
    h->code = -1;
    CHECK(nvgpu_parse(M, len, 0, own_all, &S) < 0, "negative code");
    h->code = NVP_ST_DLOPEN; h->detail = 0; h->status = -1;
    CHECK(!nvgpu_parse(M, len, 0, own_all, &S) && S.diag.code == NVP_ST_DLOPEN && !S.diag.has_detail, "dlopen, no number");
    h->code = NVP_ST_COUNT; h->detail = 0x7fffffff;
    CHECK(!nvgpu_parse(M, len, 0, own_all, &S) && S.diag.code == NVP_ST_COUNT && !S.diag.has_detail, "absurd number dropped");
    h->code = 0; h->status = 0; h->detail = 0;
    CHECK(!nvgpu_parse(M, len, 0, own_all, &S) && S.diag.code == NVP_ST_NO_DEVICE, "ok with no GPU → noDevice");
    len = make(M, 1, 0);
    h = (NvHdr *)M;
    h->denied = 7;
    int32_t nrs[NVP_DENIED_MAX] = { 41, 41, -1, 5000, 59, NVP_NONE, 16, 16 };
    uint32_t ios[NVP_DENIED_MAX] = { 0x5401, 0, 0x5401, 0xc0104629u, 0, 0, 0, 0 };
    memcpy(h->denied_nr, nrs, sizeof nrs);
    memcpy(h->denied_ioctl, ios, sizeof ios);
    CHECK(!nvgpu_parse(M, len, 0, own_all, &S) && S.diag.code == NVP_ST_OK && S.ngpu == 1, "ok with refusals");
    CHECK(S.diag.denied == 7 && S.diag.nsys == 3 && S.diag.sys[0] == 41 && S.diag.sys[1] == 59 && S.diag.sys[2] == 16, "syscalls %d", S.diag.nsys);
    CHECK(S.diag.nioc == 2 && S.diag.ioc[0] == 0x5401 && S.diag.ioc[1] == 0xc0104629u, "ioctls %d", S.diag.nioc);

    /* the fragment: numbers and fixed names only, valid JSON, hostile names cleaned */
    NvPci pci[2];
    memset(pci, 0, sizeof pci);
    strcpy(pci[0].slot, "0000:01:00.0"); strcpy(pci[0].id, "10de:2684"); strcpy(pci[0].cls, "030000"); strcpy(pci[0].driver, "nvidia");
    strcpy(pci[0].name, "AD102 \"GeForce\" \\ RTX\n4090");
    strcpy(pci[1].slot, "0000:02:00.0"); strcpy(pci[1].id, "10de:1b80"); strcpy(pci[1].cls, "030000");
    n = nvgpu_diag_format(&S.diag, pci, 2, "550.54.14", J, sizeof J);
    JDoc d;
    CHECK(n > 0 && valid_json(J, &d), "diag json");
    if (n > 0) json_free(&d);
    CHECK(strstr(J, "\"state\":\"ok\"") && strstr(J, "\"denied\":7") && strstr(J, "\"syscalls\":[41,59,16]") &&
          strstr(J, "\"ioctls\":[21505,3222292009]") && strstr(J, "\"driverVersion\":\"550.54.14\"") &&
          strstr(J, "\"name\":\"AD102  GeForce    RTX 4090\"") && strstr(J, "\"driver\":null") && strstr(J, "\"detail\":null"), "diag: %s", J);
    NvDiag dz = { .code = NVP_ST_KILLED, .detail = 31, .has_detail = 1 };
    n = nvgpu_diag_format(&dz, NULL, 0, "550\n; rm", J, sizeof J);
    CHECK(n > 0 && valid_json(J, NULL) && strstr(J, "\"state\":\"killed\",\"detail\":31") && strstr(J, "\"driverVersion\":null") &&
          strstr(J, "\"pci\":[]"), "diag killed: %s", J);
    dz.code = 9999;
    CHECK(nvgpu_diag_format(&dz, NULL, 0, NULL, J, sizeof J) > 0 && strstr(J, "\"state\":\"exit\""), "unknown code → exit: %s", J);
    for (int c = 0; c < NVP_ST_COUNT_; c++) {
        dz.code = c;
        CHECK(nvgpu_diag_format(&dz, pci, 2, "1.2", J, sizeof J) > 0 && valid_json(J, NULL) && !strstr(J, "\"state\":\"\""), "code %d has a name", c);
    }
    CHECK(nvgpu_diag_format(&dz, pci, 2, "1.2", J, 40) == 0 && !J[0], "diag: small buffer");
}

static uint64_t rng = 0x9e3779b97f4a7c15ULL;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static void fuzz(int iters) {
    int ok = 0;
    for (int it = 0; it < iters; it++) {
        int ng = (int)(rnd() % 5), np = (int)(rnd() % 9);
        if (rnd() % 50 == 0) { ng = NVP_MAX_GPUS; np = NVP_MAX_PROCS; }
        size_t len = make(M, ng, np);
        int flips = (int)(rnd() % 24);
        for (int f = 0; f < flips; f++) {
            size_t at = rnd() % len;
            switch (rnd() % 4) {
            case 0: M[at] ^= (unsigned char)(1u << (rnd() % 8)); break;
            case 1: M[at] = (unsigned char)rnd(); break;
            case 2: if (at + 4 <= len) { uint32_t v = (uint32_t)rnd(); memcpy(M + at, &v, 4); } break;
            default: if (at + 8 <= len) { int64_t v = (int64_t)rnd(); memcpy(M + at, &v, 8); } break;
            }
        }
        if (rnd() % 10 == 0) len = rnd() % (len + 32);
        if (nvgpu_parse(M, len, 1000, own_even, &S) == 0) {
            ok++;
            int n = nvgpu_format(&S, J, sizeof J);
            if (n > 0 && !valid_json(J, NULL)) { fails++; fprintf(stderr, "FAIL fuzz %d: bad JSON\n", it); return; }
            for (int i = 0; i < S.ngpu; i++) {
                const NvGpu *g = &S.gpu[i];
                if ((g->util_sm != NVP_NONE && (g->util_sm < 0 || g->util_sm > 100)) || (g->temp != NVP_NONE && (g->temp < -50 || g->temp > 150)) ||
                    (g->vram_total != NVP_NONE64 && (g->vram_total < 0 || g->vram_total > (1LL << 50)))) { fails++; fprintf(stderr, "FAIL fuzz range\n"); return; }
            }
            for (int i = 0; i < S.nproc; i++)
                if (S.proc[i].pid < 1 || S.proc[i].pid % 2 || S.proc[i].gpu >= (uint32_t)S.ngpu) { fails++; fprintf(stderr, "FAIL fuzz proc\n"); return; }
        }
    }
    printf("fuzz: %d cases, %d parsed\n", iters, ok);
}

/* ---- the helper with the fake NVML ---- */

static char helper[512], lib[512], nosym[512];

static int run_lib(const char *mode, const char *with, int wait_ms, int want_json) {
    static char env[64], libarg[512];
    snprintf(env, sizeof env, "NVFAKE_MODE=%s", mode);
    snprintf(libarg, sizeof libarg, "%s", with ? with : "");
    static char *argv[5], *envp[2];
    argv[0] = "lord-nvgpu"; argv[1] = with ? "--lib" : NULL; argv[2] = libarg; argv[3] = NULL;
    envp[0] = env; envp[1] = NULL;
    nvgpu_test_helper(helper, argv, envp);
    nvgpu_want(1);
    double t0 = now_ms(), worst = 0;
    int got = 0;
    while (now_ms() - t0 < wait_ms) {
        double a = now_ms();
        nvgpu_poll();
        double d = now_ms() - a;
        if (d > worst) worst = d;
        got = nvgpu_json(J, sizeof J);
        if (want_json && got > 0) break;
        usleep(20000);
    }
    CHECK(worst < 100, "%s: nvgpu_poll blocked %.0f ms", mode, worst);
    return got;
}
static int run_mode(const char *mode, int wait_ms, int want_json) { return run_lib(mode, lib, wait_ms, want_json); }

/* runs the helper until the diagnostic says `want` (or wait_ms) and returns it; nothing to show meanwhile */
static NvDiag run_diag(const char *mode, const char *with, int want, int wait_ms) {
    static char env[64], libarg[512];
    snprintf(env, sizeof env, "NVFAKE_MODE=%s", mode);
    snprintf(libarg, sizeof libarg, "%s", with ? with : "");
    static char *argv[5], *envp[2];
    argv[0] = "lord-nvgpu"; argv[1] = with ? "--lib" : NULL; argv[2] = libarg; argv[3] = NULL;
    envp[0] = env; envp[1] = NULL;
    nvgpu_test_helper(helper, argv, envp);
    nvgpu_want(1);
    NvDiag d;
    double t0 = now_ms(), worst = 0;
    for (;;) {
        double a = now_ms();
        nvgpu_poll();
        double dt = now_ms() - a;
        if (dt > worst) worst = dt;
        nvgpu_test_diag(&d);
        if (d.code == want || now_ms() - t0 > wait_ms) break;
        usleep(20000);
    }
    CHECK(worst < 100, "%s: nvgpu_poll blocked %.0f ms", mode, worst);
    CHECK(d.code == want, "%s: diag %d, want %d", mode, d.code, want);
    return d;
}

static void failures(void) {
    if (access(helper, X_OK) || access(lib, R_OK) || access(nosym, R_OK)) { printf("failures: skipped: needs build/libnvfake-nosym.so\n"); return; }
    nvgpu_test_timing(800, 100);
    NvDiag d = run_diag("good", lib, NVP_ST_OK, 5000);
    CHECK(d.denied == 0, "good: %u refusals", d.denied);
    nvgpu_want(0);
    d = run_diag("initfail", lib, NVP_ST_INIT, 4000);
    CHECK(d.has_detail && d.detail == 9, "initfail: nvmlReturn %d", d.detail);
    CHECK(nvgpu_json(J, sizeof J) == 0, "initfail: a sample");
    nvgpu_want(0);
    d = run_diag("fail", lib, NVP_ST_COUNT, 4000);
    CHECK(d.has_detail && d.detail == 999, "fail: nvmlReturn %d", d.detail);
    nvgpu_want(0);
    d = run_diag("good", nosym, NVP_ST_SYMBOL, 4000);
    CHECK(d.has_detail && d.detail == 1, "nosym: which %d", d.detail);
    nvgpu_want(0);
    run_diag("good", "/nonexistent/libnvidia-ml.so.1", NVP_ST_DLOPEN, 4000);
    nvgpu_want(0);
    /* no --lib: the real path, on a machine without a card bound to `nvidia` (or without NVML) */
    {
        static char *argv[] = { "lord-nvgpu", NULL }, *envp[] = { NULL };
        nvgpu_test_helper(helper, argv, envp);
        nvgpu_want(1);
        double t0 = now_ms();
        do { nvgpu_poll(); nvgpu_test_diag(&d); usleep(20000); } while (now_ms() - t0 < 4000 && (d.code == NVP_ST_SYMBOL || d.code == NVP_ST_DLOPEN));
        CHECK(d.code >= NVP_ST_OK && d.code <= NVP_ST_NO_DEVICE && d.code != NVP_ST_SYMBOL, "no --lib: diag %d", d.code);
        printf("failures: no --lib on this machine: code %d\n", d.code);
        nvgpu_want(0);
    }
    /* a refused socket() and tty ioctl inside NVML's sampling: counted, EPERM to the caller, the data still flows */
    d = run_diag("forbidden", lib, NVP_ST_OK, 5000);
    int got = 0;
    for (double t0 = now_ms(); now_ms() - t0 < 4000; usleep(20000)) {
        nvgpu_poll();
        got = nvgpu_json(J, sizeof J);
        nvgpu_test_diag(&d);
        if (got > 0 && d.denied >= 2) break;
    }
    int sk = 0, ix = 0, tc = 0;
    for (int i = 0; i < d.nsys; i++) { sk |= d.sys[i] == SYS_socket; ix |= d.sys[i] == SYS_ioctl; }
    for (int i = 0; i < d.nioc; i++) tc |= d.ioc[i] == 0x5401;
    CHECK(got > 0 && d.code == NVP_ST_OK, "forbidden: no data (code %d)", d.code);
    CHECK(d.denied >= 2 && sk && ix && tc, "forbidden: refusals %u socket %d ioctl %d tcgets %d", d.denied, sk, ix, tc);
    nvgpu_want(0);
    /* the same with SIGSYS blocked: the kernel kills the helper, lord-server reads it from the exit status */
    d = run_diag("sigsys", lib, NVP_ST_KILLED, 5000);
    CHECK(d.has_detail && d.detail == SIGSYS, "sigsys: signal %d", d.detail);
    nvgpu_want(0);
    run_diag("slow", lib, NVP_ST_TIMEOUT, 6000);
    nvgpu_want(0);
    printf("failures: init, count, symbol, dlopen, no --lib, forbidden (counted), sigsys (killed), timeout: done\n");
}

static void e2e(void) {
    if (access(helper, X_OK) || access(lib, R_OK)) { printf("e2e: skipped: needs build/lord-nvgpu-test and build/libnvfake.so\n"); return; }
    nvgpu_test_timing(800, 100);
    int n = run_mode("good", 5000, 1);
    CHECK(n > 0 && valid_json(J, NULL), "good: no sample");
    char me[64];
    snprintf(me, sizeof me, "{\"pid\":%d,\"mem\":536870912,\"sm\":33,\"enc\":5,\"dec\":6}", (int)getpid());
    CHECK(strstr(J, "\"bus\":\"0000:01:00.0\"") && strstr(J, "\"bus\":\"0000:02:00.0\"") && strstr(J, "\"sm\":42") &&
          strstr(J, "\"temp\":61") && strstr(J, "\"power\":123.456") && strstr(J, "\"clk\":{\"core\":1905,\"mem\":7001}") &&
          strstr(J, "\"vram\":{\"used\":1073741824,\"total\":8589934592}") && strstr(J, "\"cuda\":\"12.4\"") &&
          strstr(J, "\"driver\":\"550.54.14\"") && strstr(J, "\"computeMode\":\"default\"") && strstr(J, "\"ecc\":false"), "good: %.600s", J);
    CHECK(strstr(J, me) != NULL, "good: own process row missing (%s): %.800s", me, J);
    CHECK(!strstr(J, "\"pid\":1,"), "good: root's pid 1 kept");
    int starts = 0, pid = nvgpu_test_pid(&starts);
    CHECK(pid > 0, "good: no helper");
    nvgpu_want(0);
    CHECK(nvgpu_test_pid(NULL) == 0 && nvgpu_json(J, sizeof J) == 0, "want(0) keeps it");
    usleep(100000);
    nvgpu_poll();                       /* reaps it */
    CHECK(kill(pid, 0) && errno == ESRCH, "want(0): helper %d still alive", pid);

    n = run_mode("hostile", 5000, 1);
    CHECK(n > 0 && valid_json(J, NULL), "hostile: no sample");
    CHECK(!strstr(J, "AAA") && !strstr(J, "\"pid\":1,") && !strstr(J, "\"pid\":0,") && !strstr(J, "\"pid\":2147483647") &&
          strstr(J, "\"temp\":null") && strstr(J, "\"bus\":null") && strstr(J, "\"cuda\":null") && strstr(J, "\"driver\":null"), "hostile: %.400s", J);
    int gpus = 0;
    for (const char *q = J; (q = strstr(q, "\"util\"")); q++) gpus++;
    CHECK(gpus == NVP_MAX_GPUS, "hostile: %d GPUs (want the cap)", gpus);
    nvgpu_want(0);

    CHECK(run_mode("fail", 2500, 1) == 0, "fail: a sample");
    nvgpu_want(0);
    CHECK(run_mode("initfail", 2500, 1) == 0, "initfail: a sample");
    nvgpu_want(0);
    int s0 = 0;
    nvgpu_test_pid(&s0);
    CHECK(run_mode("slow", 6000, 1) == 0, "slow: a sample");
    int s1 = 0;
    nvgpu_test_pid(&s1);
    CHECK(s1 - s0 >= 2, "slow: not restarted (%d starts)", s1 - s0);
    nvgpu_want(0);
    printf("e2e: good, hostile, fail, initfail, slow, want(0): done\n");
}

static void sandbox_probe(void) {
    if (access(helper, X_OK) || access(lib, R_OK)) return;
    int sv[2], out[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) || pipe(out)) { CHECK(0, "socketpair"); return; }
    pid_t p = fork();
    if (p == 0) {
        dup2(sv[1], 3);
        dup2(out[1], 1);
        int nul = open("/dev/null", O_RDWR);
        dup2(nul, 0);
        dup2(nul, 2);
        char *argv[] = { "lord-nvgpu", "--lib", lib, "--probe", NULL }, *envp[] = { "NVFAKE_MODE=good", NULL };
        execve(helper, argv, envp);
        _exit(127);
    }
    close(out[1]);
    close(sv[1]);
    char buf[256] = { 0 };
    size_t k = 0;
    ssize_t r;
    while (k < sizeof buf - 1 && (r = read(out[0], buf + k, sizeof buf - 1 - k)) > 0) k += (size_t)r;
    int st = 0;
    waitpid(p, &st, 0);
    CHECK(!strncmp(buf, "socket=0 write=0 read=0 ioctl=0 exec=0\n", 39), "sandbox probe: '%s'", buf);
    CHECK(strstr(buf, "\ntrapped=") && strstr(buf, " socket=1 ioctl=1 exec=1 tcgets=1 eperm=1\n"), "sandbox probe, refusals: '%s'", buf);
    CHECK(access("/var/tmp/lordos-nvgpu-probe", F_OK), "sandbox probe: the file was written");
    printf("sandbox: %s", buf);
    close(out[0]);
    close(sv[0]);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    snprintf(helper, sizeof helper, "%s", argc > 1 ? argv[1] : "build/lord-nvgpu-test");
    snprintf(lib, sizeof lib, "%s", argc > 2 ? argv[2] : "build/libnvfake.so");
    snprintf(nosym, sizeof nosym, "%s", argc > 3 ? argv[3] : "build/libnvfake-nosym.so");
    char real[PATH_MAX];
    if (realpath(lib, real) && strlen(real) < sizeof lib) memcpy(lib, real, strlen(real) + 1);
    if (realpath(nosym, real) && strlen(real) < sizeof nosym) memcpy(nosym, real, strlen(real) + 1);
    unit();
    fuzz(120000);
    e2e();
    failures();
    sandbox_probe();
    if (fails) { fprintf(stderr, "nvgpu-test: %d failures\n", fails); return 1; }
    printf("nvgpu-test: ok\n");
    return 0;
}
