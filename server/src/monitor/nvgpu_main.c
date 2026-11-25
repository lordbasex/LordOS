/* lord-nvgpu — Monitor → GPU's NVIDIA numbers (docs/MONITOR.md, "NVIDIA"; AGENTS.md, lord-nvgpu; approved by the
 * maintainer on 2026-11-24).
 *
 * The only LordOS program that loads NVIDIA's NVML (libnvidia-ml.so.1), so lord-server never links or loads it and
 * never runs nvidia-smi. lord-server starts it only while Monitor → GPU or the Overview is on screen and a PCI device
 * of vendor 0x10de is bound to the `nvidia` driver. No arguments, an empty environment, fd 3 a SOCK_SEQPACKET socket
 * to lord-server, fds 0-2 /dev/null.
 *
 *  1. PR_SET_PDEATHSIG, no_new_privs, every other fd closed.
 *  2. libnvidia-ml.so.1 only from the fixed system library folders, the folder and the file (after its symlink)
 *     root's and not writable by group or others; dlopen(RTLD_NOW | RTLD_LOCAL); no LD_LIBRARY_PATH (the
 *     environment is empty and the path absolute).
 *  3. nvmlInit_v2, the device handles and one warm-up sample, then sandbox_apply_helper: no network or new sockets,
 *     no exec, threads only, Landlock read-only (/proc/driver/nvidia, /sys/bus/pci/devices, /sys/devices, the
 *     library folders) and read/ioctl only on /dev/nvidiactl and /dev/nvidia<N>; ioctl only of type 'F'.
 *     Why after init: NVML's init opens the device nodes by path, may read driver files we can't list without the
 *     hardware and asks nvidia-persistenced; the sampling afterwards needs only the fds NVML already holds plus the
 *     read-only paths above. Before init no input reaches this program at all (no arguments, no environment).
 *  4. Then, for each byte NVP_REQ_SAMPLE on fd 3, one message of fixed-size records (nvproto.h), numbers only.
 *
 * When it can't read the cards it still answers, with NvHdr.code saying why (NVP_ST_*: no device bound to `nvidia`,
 * no library, dlopen, a missing symbol, nvmlInit or the count failing, no device) and a number (the nvmlReturn_t),
 * never NVML's or the driver's text. What the sandbox refuses is counted too: the filter is built with trap_denied,
 * so a refused syscall or ioctl type raises SIGSYS instead of failing with EPERM, and on_sigsys() (no syscall, no
 * lock, only atomics on static arrays) records its number (and an ioctl's request), puts -EPERM in the return
 * register and returns: NVML sees the same EPERM as before, and the numbers go out in every header. If the kernel
 * kills the helper with SIGSYS instead (SIGSYS blocked in a thread, a wrong architecture), lord-server sees it in
 * the exit status and reports "killed by the sandbox".
 *
 * Test build only (-DNVGPU_TEST, build/lord-nvgpu-test, never installed): `--lib <path>` loads a fake NVML without
 * the ownership checks and without the NVIDIA device check, and `--probe` prints what the sandbox still allows. */
#include "monitor/nvproto.h"
#include "monitor/nvmlmin.h"
#include "sec/sandbox.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#define NV_PROCS_PER_DEV 64
#define NV_SAMPLES_MAX   256

static const char *const LIB_DIRS[] = { "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu", "/usr/lib64", "/usr/lib", NULL };

static struct {
    nvmlReturn_t (*init)(void);
    nvmlReturn_t (*count)(unsigned *);
    nvmlReturn_t (*handle)(unsigned, nvmlDevice_t *);
    nvmlReturn_t (*pci)(nvmlDevice_t, nvmlPciInfo_t *);
    nvmlReturn_t (*util)(nvmlDevice_t, nvmlUtilization_t *);
    nvmlReturn_t (*enc)(nvmlDevice_t, unsigned *, unsigned *);
    nvmlReturn_t (*dec)(nvmlDevice_t, unsigned *, unsigned *);
    nvmlReturn_t (*temp)(nvmlDevice_t, int, unsigned *);
    nvmlReturn_t (*fan)(nvmlDevice_t, unsigned *);
    nvmlReturn_t (*power)(nvmlDevice_t, unsigned *);
    nvmlReturn_t (*plimit)(nvmlDevice_t, unsigned *);
    nvmlReturn_t (*clock)(nvmlDevice_t, int, unsigned *);
    nvmlReturn_t (*mem)(nvmlDevice_t, nvmlMemory_t *);
    nvmlReturn_t (*cmode)(nvmlDevice_t, int *);
    nvmlReturn_t (*ecc)(nvmlDevice_t, int *, int *);
    nvmlReturn_t (*cprocs)(nvmlDevice_t, unsigned *, nvmlProcessInfo_t *);
    nvmlReturn_t (*gprocs)(nvmlDevice_t, unsigned *, nvmlProcessInfo_t *);
    nvmlReturn_t (*putil)(nvmlDevice_t, nvmlProcessUtilizationSample_t *, unsigned *, unsigned long long);
    nvmlReturn_t (*cuda)(int *);
    nvmlReturn_t (*driver)(char *, unsigned);
} F;

/* a folder of the system: root's, not writable by group or others, every parent too */
static int root_dir_ok(const char *dir) {
    char p[PATH_MAX];
    if (snprintf(p, sizeof p, "%s", dir) >= (int)sizeof p || p[0] != '/') return 0;
    for (;;) {
        struct stat st;
        if (lstat(p, &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022)) return 0;
        char *sl = strrchr(p, '/');
        if (!sl) return 0;
        if (sl == p) { if (p[1]) { p[1] = 0; continue; } return 1; }
        *sl = 0;
    }
}

/* libnvidia-ml.so.1 from the fixed folders only; the real file (it is a symlink to the versioned one) must be in the
 * same kind of folder, root's and not writable by group or others. 0 with its real path in out. */
static int find_nvml(char *out, size_t n) {
    for (int i = 0; LIB_DIRS[i]; i++) {
        if (!root_dir_ok(LIB_DIRS[i])) continue;
        char p[PATH_MAX], real[PATH_MAX];
        snprintf(p, sizeof p, "%s/libnvidia-ml.so.1", LIB_DIRS[i]);
        if (!realpath(p, real)) continue;
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", real);
        char *sl = strrchr(dir, '/');
        if (!sl || sl == dir) continue;
        *sl = 0;
        int fixed = 0;
        for (int j = 0; LIB_DIRS[j]; j++) if (!strcmp(dir, LIB_DIRS[j])) fixed = 1;
        if (!fixed || !root_dir_ok(dir)) continue;
        struct stat st;
        if (lstat(real, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022)) continue;
        if (strlen(real) >= n) continue;
        memcpy(out, real, strlen(real) + 1);
        return 0;
    }
    return -1;
}

/* a PCI device of vendor 0x10de bound to the `nvidia` driver */
static int nvidia_present(void) {
    DIR *d = opendir("/sys/bus/pci/devices");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[PATH_MAX], v[16] = { 0 }, drv[PATH_MAX];
        snprintf(p, sizeof p, "/sys/bus/pci/devices/%.64s/vendor", e->d_name);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t r = read(fd, v, sizeof v - 1);
        close(fd);
        if (r < 6 || strncmp(v, "0x10de", 6)) continue;
        snprintf(p, sizeof p, "/sys/bus/pci/devices/%.64s/driver", e->d_name);
        ssize_t l = readlink(p, drv, sizeof drv - 1);
        if (l <= 0) continue;
        drv[l] = 0;
        const char *b = strrchr(drv, '/');
        if (b && !strcmp(b + 1, "nvidia")) found = 1;
    }
    closedir(d);
    return found;
}

/* ---- what the sandbox refused (async-signal-safe: atomics on static arrays only) ---- */

static uint32_t den_total;
static uint32_t den_nr[NVP_DENIED_MAX];         /* syscall number + 1; 0 = free */
static uint32_t den_ioc[NVP_DENIED_MAX];        /* ioctl request; 0 = free */

static void den_add(uint32_t *slots, uint32_t v) {
    for (int i = 0; i < NVP_DENIED_MAX; i++) {
        uint32_t cur = __atomic_load_n(&slots[i], __ATOMIC_RELAXED);
        if (cur == v) return;
        if (cur == 0) {
            uint32_t zero = 0;
            if (__atomic_compare_exchange_n(&slots[i], &zero, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) || zero == v) return;
        }
    }
}

static void on_sigsys(int sig, siginfo_t *si, void *ctx) {
    (void)sig;
    if (!si || si->si_code != 1 /* SYS_SECCOMP */ || !ctx) return;
    ucontext_t *uc = ctx;
    int nr = si->si_syscall;
    __atomic_add_fetch(&den_total, 1, __ATOMIC_RELAXED);
    if (nr >= 0 && nr < 4096) den_add(den_nr, (uint32_t)nr + 1);
#if defined(__x86_64__)
    if (nr == SYS_ioctl) den_add(den_ioc, (uint32_t)uc->uc_mcontext.gregs[REG_RSI]);
    uc->uc_mcontext.gregs[REG_RAX] = -EPERM;
#elif defined(__aarch64__)
    if (nr == SYS_ioctl) den_add(den_ioc, (uint32_t)uc->uc_mcontext.regs[1]);
    uc->uc_mcontext.regs[0] = (unsigned long long)(long long)-EPERM;
#else
#  error "lord-nvgpu: on_sigsys needs the return register of this architecture"
#endif
}

static void put_denied(NvHdr *h) {
    h->denied = __atomic_load_n(&den_total, __ATOMIC_RELAXED);
    for (int i = 0; i < NVP_DENIED_MAX; i++) {
        uint32_t v = __atomic_load_n(&den_nr[i], __ATOMIC_RELAXED);
        h->denied_nr[i] = v ? (int32_t)(v - 1) : NVP_NONE;
        h->denied_ioctl[i] = __atomic_load_n(&den_ioc[i], __ATOMIC_RELAXED);
    }
}

static void *sym(void *h, const char *a, const char *b) {
    void *f = dlsym(h, a);
    if (!f && b) f = dlsym(h, b);
    return f;
}

/* NVP_ST_OK, NVP_ST_DLOPEN or NVP_ST_SYMBOL (*which: 1 init, 2 count, 3 handle) */
static int load(const char *path, int *which) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) return NVP_ST_DLOPEN;
    F.init = sym(h, "nvmlInit_v2", "nvmlInit");
    F.count = sym(h, "nvmlDeviceGetCount_v2", "nvmlDeviceGetCount");
    F.handle = sym(h, "nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex");
    F.pci = sym(h, "nvmlDeviceGetPciInfo_v3", "nvmlDeviceGetPciInfo_v2");
    F.util = sym(h, "nvmlDeviceGetUtilizationRates", NULL);
    F.enc = sym(h, "nvmlDeviceGetEncoderUtilization", NULL);
    F.dec = sym(h, "nvmlDeviceGetDecoderUtilization", NULL);
    F.temp = sym(h, "nvmlDeviceGetTemperature", NULL);
    F.fan = sym(h, "nvmlDeviceGetFanSpeed", NULL);
    F.power = sym(h, "nvmlDeviceGetPowerUsage", NULL);
    F.plimit = sym(h, "nvmlDeviceGetEnforcedPowerLimit", NULL);
    F.clock = sym(h, "nvmlDeviceGetClockInfo", NULL);
    F.mem = sym(h, "nvmlDeviceGetMemoryInfo", NULL);
    F.cmode = sym(h, "nvmlDeviceGetComputeMode", NULL);
    F.ecc = sym(h, "nvmlDeviceGetEccMode", NULL);
    F.cprocs = sym(h, "nvmlDeviceGetComputeRunningProcesses_v3", "nvmlDeviceGetComputeRunningProcesses_v2");
    F.gprocs = sym(h, "nvmlDeviceGetGraphicsRunningProcesses_v3", "nvmlDeviceGetGraphicsRunningProcesses_v2");
    F.putil = sym(h, "nvmlDeviceGetProcessUtilization", NULL);
    F.cuda = sym(h, "nvmlSystemGetCudaDriverVersion_v2", "nvmlSystemGetCudaDriverVersion");
    F.driver = sym(h, "nvmlSystemGetDriverVersion", NULL);
    *which = !F.init ? 1 : !F.count ? 2 : !F.handle ? 3 : 0;
    return *which ? NVP_ST_SYMBOL : NVP_ST_OK;
}

static int32_t pct(nvmlReturn_t r, unsigned v) { return r == NVML_SUCCESS && v <= 100 ? (int32_t)v : NVP_NONE; }
static int32_t small(nvmlReturn_t r, unsigned v) { return r == NVML_SUCCESS && v <= INT32_MAX ? (int32_t)v : NVP_NONE; }
static int64_t big(unsigned long long v) { return v == NVML_VALUE_NOT_AVAILABLE_ULL || v > (unsigned long long)INT64_MAX ? NVP_NONE64 : (int64_t)v; }

static nvmlDevice_t devs[NVP_MAX_GPUS];
static unsigned long long last_seen[NVP_MAX_GPUS];
static unsigned ndev;
static unsigned char msg[NVP_MSG_MAX];

static void gpu_sample(unsigned i, NvGpu *g) {
    nvmlDevice_t d = devs[i];
    memset(g, 0, sizeof *g);
    g->util_sm = g->util_mem = g->util_enc = g->util_dec = g->temp = g->fan = NVP_NONE;
    g->clk_core = g->clk_mem = g->compute_mode = g->ecc = NVP_NONE;
    g->power_mw = g->power_limit_mw = g->vram_used = g->vram_total = NVP_NONE64;
    g->domain = UINT32_MAX;
    nvmlPciInfo_t pi;
    memset(&pi, 0, sizeof pi);
    if (F.pci && F.pci(d, &pi) == NVML_SUCCESS) {
        unsigned dom, bus, dev, fn;
        char id[sizeof pi.busId + 1];
        memcpy(id, pi.busId, sizeof pi.busId);
        id[sizeof pi.busId] = 0;
        char tail;
        if (sscanf(id, "%x:%x:%x.%x%c", &dom, &bus, &dev, &fn, &tail) == 4) { g->domain = dom; g->bus = bus; g->dev = dev; g->fn = fn; }
        else { g->domain = pi.domain; g->bus = pi.bus; g->dev = pi.device; g->fn = 0; }
    }
    nvmlUtilization_t u = { 0, 0 };
    if (F.util && F.util(d, &u) == NVML_SUCCESS) { g->util_sm = pct(0, u.gpu); g->util_mem = pct(0, u.memory); }
    unsigned v = 0, per = 0;
    if (F.enc) { nvmlReturn_t r = F.enc(d, &v, &per); g->util_enc = pct(r, v); }
    if (F.dec) { nvmlReturn_t r = F.dec(d, &v, &per); g->util_dec = pct(r, v); }
    if (F.temp) { nvmlReturn_t r = F.temp(d, NVML_TEMPERATURE_GPU, &v); g->temp = small(r, v); }
    if (F.fan) { nvmlReturn_t r = F.fan(d, &v); g->fan = small(r, v); }
    if (F.power) { nvmlReturn_t r = F.power(d, &v); g->power_mw = r == NVML_SUCCESS ? (int64_t)v : NVP_NONE64; }
    if (F.plimit) { nvmlReturn_t r = F.plimit(d, &v); g->power_limit_mw = r == NVML_SUCCESS ? (int64_t)v : NVP_NONE64; }
    if (F.clock) {
        nvmlReturn_t r = F.clock(d, NVML_CLOCK_GRAPHICS, &v); g->clk_core = small(r, v);
        r = F.clock(d, NVML_CLOCK_MEM, &v); g->clk_mem = small(r, v);
    }
    nvmlMemory_t m = { 0, 0, 0 };
    if (F.mem && F.mem(d, &m) == NVML_SUCCESS) { g->vram_used = big(m.used); g->vram_total = big(m.total); }
    int cm = 0, cur = 0, pend = 0;
    if (F.cmode && F.cmode(d, &cm) == NVML_SUCCESS) g->compute_mode = cm;
    if (F.ecc && F.ecc(d, &cur, &pend) == NVML_SUCCESS) g->ecc = cur ? 1 : 0;
}

/* the running processes (compute and graphics, one row per pid) and their latest utilization sample */
static unsigned procs_sample(unsigned gi, NvProc *out, unsigned room) {
    nvmlDevice_t d = devs[gi];
    static nvmlProcessInfo_t pinfo[NV_PROCS_PER_DEV];
    static nvmlProcessUtilizationSample_t us[NV_SAMPLES_MAX];
    unsigned n = 0;
    nvmlReturn_t (*fs[2])(nvmlDevice_t, unsigned *, nvmlProcessInfo_t *) = { F.cprocs, F.gprocs };
    for (int k = 0; k < 2; k++) {
        if (!fs[k]) continue;
        unsigned cnt = NV_PROCS_PER_DEV;
        memset(pinfo, 0, sizeof pinfo);
        nvmlReturn_t r = fs[k](d, &cnt, pinfo);
        if (r != NVML_SUCCESS) continue;
        if (cnt > NV_PROCS_PER_DEV) cnt = NV_PROCS_PER_DEV;     /* never trust the count */
        for (unsigned i = 0; i < cnt && n < room; i++) {
            if (!pinfo[i].pid || pinfo[i].pid > INT32_MAX) continue;
            unsigned j;
            for (j = 0; j < n; j++) if (out[j].pid == (int32_t)pinfo[i].pid) break;
            int64_t mem = big(pinfo[i].usedGpuMemory);
            if (j < n) { if (mem > out[j].mem) out[j].mem = mem; continue; }
            out[n] = (NvProc){ .gpu = gi, .pid = (int32_t)pinfo[i].pid, .sm = NVP_NONE, .enc = NVP_NONE, .dec = NVP_NONE, .mem = mem };
            n++;
        }
    }
    if (n && F.putil) {
        unsigned cnt = NV_SAMPLES_MAX;
        memset(us, 0, sizeof us);
        if (F.putil(d, us, &cnt, last_seen[gi]) == NVML_SUCCESS) {
            if (cnt > NV_SAMPLES_MAX) cnt = NV_SAMPLES_MAX;
            unsigned long long newest[NV_PROCS_PER_DEV * 2] = { 0 };
            for (unsigned i = 0; i < cnt; i++) {
                if (us[i].timeStamp > last_seen[gi]) last_seen[gi] = us[i].timeStamp;
                for (unsigned j = 0; j < n; j++) {
                    if (out[j].pid != (int32_t)us[i].pid || us[i].timeStamp < newest[j]) continue;
                    newest[j] = us[i].timeStamp;
                    out[j].sm = pct(0, us[i].smUtil);
                    out[j].enc = pct(0, us[i].encUtil);
                    out[j].dec = pct(0, us[i].decUtil);
                }
            }
        }
    }
    return n;
}

static size_t build(void) {
    NvHdr *h = (NvHdr *)msg;
    memset(h, 0, sizeof *h);
    h->magic = NVP_MAGIC;
    h->cuda = NVP_NONE;
    int cv = 0;
    if (F.cuda && F.cuda(&cv) == NVML_SUCCESS && cv > 0) h->cuda = cv;
    char dv[96];
    memset(dv, 0, sizeof dv);
    if (F.driver && F.driver(dv, 80) == NVML_SUCCESS) {
        dv[80] = 0;                                     /* NVML promises a NUL; don't rely on it */
        size_t k = 0;
        while (k < NVP_DRIVER_LEN - 1 && ((dv[k] >= '0' && dv[k] <= '9') || dv[k] == '.')) k++;
        if (dv[k] == 0) memcpy(h->driver, dv, k);       /* only all digits and dots, else "" */
    }
    NvGpu *gs = (NvGpu *)(h + 1);
    for (unsigned i = 0; i < ndev; i++) gpu_sample(i, &gs[i]);
    NvProc *ps = (NvProc *)(gs + ndev);
    unsigned np = 0;
    for (unsigned i = 0; i < ndev && np < NVP_MAX_PROCS; i++) np += procs_sample(i, ps + np, NVP_MAX_PROCS - np);
    h->ngpu = (uint16_t)ndev;
    h->nproc = (uint16_t)np;
    put_denied(h);                                      /* after sampling: this sample's refusals too */
    return sizeof *h + ndev * sizeof(NvGpu) + np * sizeof(NvProc);
}

/* the /dev/nvidia<N> nodes and nvidiactl, for Landlock */
static char devpaths[NVP_MAX_GPUS + 2][32];
static const char *devlist[NVP_MAX_GPUS + 2];

static void dev_files(void) {
    int n = 0;
    snprintf(devpaths[n], sizeof devpaths[n], "/dev/nvidiactl");
    devlist[n] = devpaths[n];
    n++;
    DIR *d = opendir("/dev");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < NVP_MAX_GPUS + 1) {
            if (strncmp(e->d_name, "nvidia", 6)) continue;
            const char *q = e->d_name + 6;
            if (!*q || strlen(q) > 3) continue;
            while (*q >= '0' && *q <= '9') q++;
            if (*q) continue;
            snprintf(devpaths[n], sizeof devpaths[n], "/dev/nvidia%.3s", e->d_name + 6);
            devlist[n] = devpaths[n];
            n++;
        }
        closedir(d);
    }
    devlist[n] = NULL;
}

int main(int argc, char **argv) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() == 1) return 2;
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    int ty = 0;
    socklen_t tl = sizeof ty;
    if (getsockopt(3, SOL_SOCKET, SO_TYPE, &ty, &tl) || ty != SOCK_SEQPACKET) return 2;
#ifdef SYS_close_range
    syscall(SYS_close_range, 4u, ~0u, 0u);
#endif
    const char *lib = NULL;
    int probe = 0;
    char real[PATH_MAX];
#ifdef NVGPU_TEST
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lib") && i + 1 < argc) lib = argv[++i];
        else if (!strcmp(argv[i], "--probe")) probe = 1;
    }
#else
    (void)argv;
    if (argc != 1) return 2;
#endif
    int code = NVP_ST_OK, detail = 0;
    if (!lib) {
        if (!nvidia_present()) code = NVP_ST_NO_DRIVER;
        else if (find_nvml(real, sizeof real)) code = NVP_ST_LIB_MISSING;
        else lib = real;
    }
    if (code == NVP_ST_OK) code = load(lib, &detail);
    unsigned cnt = 0;
    if (code == NVP_ST_OK) {
        nvmlReturn_t r = F.init();
        if (r != NVML_SUCCESS) code = NVP_ST_INIT, detail = r;
        else if ((r = F.count(&cnt)) != NVML_SUCCESS) code = NVP_ST_COUNT, detail = r;
    }
    if (cnt > NVP_MAX_GPUS) cnt = NVP_MAX_GPUS;
    for (unsigned i = 0; code == NVP_ST_OK && i < cnt; i++)
        if (F.handle(i, &devs[ndev]) == NVML_SUCCESS) ndev++;
    if (code == NVP_ST_OK && !ndev) code = NVP_ST_NO_DEVICE;
    if (code == NVP_ST_OK) build();                     /* warm-up: NVML opens what it needs now */
    /* the refusals' recorder, before the filter that raises it (trap_denied below) */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sigsys;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSYS, &sa, NULL);
    sigset_t sys;
    sigemptyset(&sys);
    sigaddset(&sys, SIGSYS);
    sigprocmask(SIG_UNBLOCK, &sys, NULL);
    dev_files();
    static const char *const ro[] = { "/proc/driver/nvidia", "/sys/bus/pci/devices", "/sys/devices", "/usr/lib/x86_64-linux-gnu",
                                      "/usr/lib/aarch64-linux-gnu", "/usr/lib64", NULL };
    SandboxHelperCfg sc = { .name = "lordos-nvgpu", .ro_paths = ro, .extra_syscalls = NULL,
                            .allow_ioctl_classes = SANDBOX_IOCTL_NVIDIA, .mem_max_mb = 256, .dev_files = devlist,
                            .trap_denied = 1 };
    sandbox_apply_helper(&sc);
#ifdef NVGPU_TEST
    if (probe) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        int w = open("/var/tmp/lordos-nvgpu-probe", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int rd = open("/etc/hostname", O_RDONLY);
        int io = ioctl(0, 0x5401 /* TCGETS */, NULL);
        char *const av[] = { "/bin/true", NULL };
        int ex = execve("/bin/true", av, av + 1);
        int se = errno;
        printf("socket=%d write=%d read=%d ioctl=%d exec=%d\n", s >= 0, w >= 0, rd >= 0, io == 0, ex == 0);
        /* the refusals the SIGSYS handler recorded: socket, ioctl and execve, each still failing with EPERM */
        NvHdr h;
        memset(&h, 0, sizeof h);
        put_denied(&h);
        int sk = 0, ix = 0, ec = 0, tc = 0;
        for (int i = 0; i < NVP_DENIED_MAX; i++) {
            if (h.denied_nr[i] == SYS_socket) sk = 1;
            if (h.denied_nr[i] == SYS_ioctl) ix = 1;
            if (h.denied_nr[i] == SYS_execve) ec = 1;
            if (h.denied_ioctl[i] == 0x5401) tc = 1;
        }
        printf("trapped=%u socket=%d ioctl=%d exec=%d tcgets=%d eperm=%d\n", h.denied, sk, ix, ec, tc, se == EPERM);
        fflush(stdout);
        return 0;
    }
#else
    (void)probe;
#endif
    for (;;) {
        unsigned char req;
        ssize_t got = recv(3, &req, 1, 0);
        if (got <= 0) return 0;
        if (req != NVP_REQ_SAMPLE) return 2;
        size_t len;
        if (code != NVP_ST_OK) {
            NvHdr err = { .magic = NVP_MAGIC, .cuda = NVP_NONE, .code = code, .detail = detail, .status = detail ? detail : -1 };
            put_denied(&err);
            memcpy(msg, &err, sizeof err);
            len = sizeof err;
        } else len = build();
        if (send(3, msg, len, MSG_NOSIGNAL) < 0) return 0;
    }
}
