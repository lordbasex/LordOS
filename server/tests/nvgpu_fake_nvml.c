/* nvgpu_fake_nvml.c — test-only fake libnvidia-ml for lord-nvgpu-test (tests/nvgpu_test.c). Never installed.
 * NVFAKE_MODE picks the behaviour: good (canned values), hostile (out-of-range numbers, huge counts, a driver string
 * with no NUL and control bytes, pids that aren't the user's), fail (every call fails), initfail, slow (a sampling
 * call that hangs). */
#include "monitor/nvmlmin.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int mode(void) {
    const char *m = getenv("NVFAKE_MODE");
    if (!m || !strcmp(m, "good")) return 0;
    if (!strcmp(m, "hostile")) return 1;
    if (!strcmp(m, "fail")) return 2;
    if (!strcmp(m, "initfail")) return 3;
    if (!strcmp(m, "slow")) return 4;
    return 0;
}
#define FAIL_IF() do { if (mode() == 2) return 999; } while (0)
static unsigned char devmem[2];

nvmlReturn_t nvmlInit_v2(void) { return mode() == 3 ? 9 : NVML_SUCCESS; }
nvmlReturn_t nvmlShutdown(void) { return NVML_SUCCESS; }
nvmlReturn_t nvmlDeviceGetCount_v2(unsigned *c) { FAIL_IF(); *c = mode() == 1 ? 1000000000u : 2; return NVML_SUCCESS; }
nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned i, nvmlDevice_t *d) { FAIL_IF(); *d = (nvmlDevice_t)(void *)&devmem[i & 1]; return NVML_SUCCESS; }
static unsigned idx(nvmlDevice_t d) { return (unsigned char *)(void *)d == &devmem[1]; }
nvmlReturn_t nvmlDeviceGetPciInfo_v3(nvmlDevice_t d, nvmlPciInfo_t *p) {
    FAIL_IF();
    if (mode() == 1) { memset(p->busId, 'Z', sizeof p->busId); p->domain = 0xffffffff; p->bus = 9999; p->device = 77; return NVML_SUCCESS; }
    strcpy(p->busId, idx(d) ? "00000000:02:00.0" : "00000000:01:00.0");
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetUtilizationRates(nvmlDevice_t d, nvmlUtilization_t *u) {
    FAIL_IF();
    if (mode() == 4) sleep(60);
    if (mode() == 1) { u->gpu = 500; u->memory = 0xffffffffu; return NVML_SUCCESS; }
    u->gpu = 42 + idx(d); u->memory = 17;
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetEncoderUtilization(nvmlDevice_t d, unsigned *u, unsigned *p) { (void)d; FAIL_IF(); *u = mode() == 1 ? 101 : 3; *p = 1000; return NVML_SUCCESS; }
nvmlReturn_t nvmlDeviceGetDecoderUtilization(nvmlDevice_t d, unsigned *u, unsigned *p) { (void)d; FAIL_IF(); *u = 4; *p = 1000; return NVML_SUCCESS; }
nvmlReturn_t nvmlDeviceGetTemperature(nvmlDevice_t d, int s, unsigned *t) { (void)d; (void)s; FAIL_IF(); *t = mode() == 1 ? 9999 : 61; return NVML_SUCCESS; }
nvmlReturn_t nvmlDeviceGetFanSpeed(nvmlDevice_t d, unsigned *f) { (void)d; FAIL_IF(); *f = mode() == 1 ? 0x80000000u : 35; return NVML_SUCCESS; }
nvmlReturn_t nvmlDeviceGetPowerUsage(nvmlDevice_t d, unsigned *p) { (void)d; FAIL_IF(); *p = mode() == 1 ? 0xffffffffu : 123456; return NVML_SUCCESS; }
nvmlReturn_t nvmlDeviceGetEnforcedPowerLimit(nvmlDevice_t d, unsigned *p) { (void)d; FAIL_IF(); *p = 250000; return NVML_SUCCESS; }
nvmlReturn_t nvmlDeviceGetClockInfo(nvmlDevice_t d, int t, unsigned *c) { (void)d; FAIL_IF(); *c = mode() == 1 ? 4000000000u : (t == NVML_CLOCK_MEM ? 7001 : 1905); return NVML_SUCCESS; }
nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t d, nvmlMemory_t *m) {
    (void)d; FAIL_IF();
    if (mode() == 1) { m->total = ~0ULL - 1; m->used = 1ULL << 62; m->free = 0; return NVML_SUCCESS; }
    m->total = 8ULL << 30; m->used = 1ULL << 30; m->free = m->total - m->used;
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetComputeMode(nvmlDevice_t d, int *c) { (void)d; FAIL_IF(); *c = mode() == 1 ? 77 : 0; return NVML_SUCCESS; }
nvmlReturn_t nvmlDeviceGetEccMode(nvmlDevice_t d, int *c, int *p) { (void)d; FAIL_IF(); *c = 0; *p = 0; return mode() == 1 ? 3 : NVML_SUCCESS; }
static nvmlReturn_t procs(nvmlDevice_t d, unsigned *count, nvmlProcessInfo_t *p, int graphics) {
    FAIL_IF();
    unsigned room = *count;
    if (mode() == 1) {
        /* claims a billion processes but writes only what fits: pids 1 (root's), 0, huge and its parent */
        unsigned v[] = { 1, 0, 0x7fffffffu, 0xffffffffu, (unsigned)getppid() };
        for (unsigned i = 0; i < room; i++) { p[i].pid = v[i % 5]; p[i].usedGpuMemory = ~0ULL - i; }
        *count = 1000000000u;
        return NVML_SUCCESS;
    }
    if (room < 1) { *count = 1; return NVML_ERROR_INSUFFICIENT_SIZE; }
    /* the test's own process (its parent) on GPU 0 for compute and graphics, pid 1 (root's) on GPU 1 */
    p[0].pid = idx(d) ? 1 : (unsigned)getppid();
    p[0].usedGpuMemory = graphics ? (256ULL << 20) : (512ULL << 20);
    *count = 1;
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetComputeRunningProcesses_v3(nvmlDevice_t d, unsigned *c, nvmlProcessInfo_t *p) { return procs(d, c, p, 0); }
nvmlReturn_t nvmlDeviceGetGraphicsRunningProcesses_v3(nvmlDevice_t d, unsigned *c, nvmlProcessInfo_t *p) { return procs(d, c, p, 1); }
nvmlReturn_t nvmlDeviceGetProcessUtilization(nvmlDevice_t d, nvmlProcessUtilizationSample_t *s, unsigned *c, unsigned long long last) {
    (void)d; (void)last; FAIL_IF();
    unsigned room = *c;
    if (mode() == 1) {
        for (unsigned i = 0; i < room; i++) { s[i].pid = (unsigned)getppid(); s[i].timeStamp = i; s[i].smUtil = 200 + i; s[i].encUtil = ~0u; s[i].decUtil = 5; }
        *c = ~0u;
        return NVML_SUCCESS;
    }
    if (room < 2) { *c = 2; return NVML_ERROR_INSUFFICIENT_SIZE; }
    s[0] = (nvmlProcessUtilizationSample_t){ (unsigned)getppid(), 10, 20, 1, 2, 3 };
    s[1] = (nvmlProcessUtilizationSample_t){ (unsigned)getppid(), 11, 33, 1, 5, 6 };
    *c = 2;
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlSystemGetCudaDriverVersion_v2(int *v) { FAIL_IF(); *v = mode() == 1 ? -5 : 12040; return NVML_SUCCESS; }
nvmlReturn_t nvmlSystemGetDriverVersion(char *b, unsigned n) {
    FAIL_IF();
    if (mode() == 1) { memset(b, 'A', n); b[3] = '\n'; return NVML_SUCCESS; }   /* no NUL at all */
    strncpy(b, "550.54.14", n);
    return NVML_SUCCESS;
}
