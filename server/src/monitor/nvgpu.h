/* nvgpu.h — Monitor → GPU's NVIDIA cards, through the sandboxed helper lord-nvgpu (nvgpu_main.c, nvproto.h;
 * docs/MONITOR.md, "NVIDIA"). lord-server never loads NVML and never runs nvidia-smi.
 *
 * The Monitor's thread calls nvgpu_poll() on every tick: it starts the helper when wanted (at most one, with a
 * restart backoff), asks for a sample once a second, takes the answer without blocking, checks every field and keeps
 * the latest. nvgpu_want() is the GPU tab's / Overview's visibility; nvgpu_suspend() stops the helper when the
 * Monitor's thread goes idle. All three and nvgpu_json() are thread-safe and never block on the helper. */
#ifndef LORD_MON_NVGPU_H
#define LORD_MON_NVGPU_H

#include <stddef.h>

/* the fragment "nv":[{bus,util:{sm,mem,enc,dec},temp,fan,power,powerLimit,clk:{core,mem},vram:{used,total},cuda,
 * driver,computeMode,ecc,procs:[{pid,mem,sm,enc,dec}]}] (absent values null; power in watts, clocks in MHz, memory in
 * bytes, temp in °C, the rest percent). Its length; 0, writing nothing, without an NVIDIA card, a helper or a sample,
 * or when it doesn't fit in n. */
int nvgpu_json(char *out, size_t n);
/* the fragment "nvDiag":{state,detail,denied,syscalls,ioctls,driverVersion,pci:[{slot,id,class,driver,name}]}: why
 * there is (or isn't) NVIDIA data, for Monitor → GPU's "Couldn't read NVIDIA data" and its report (apps/monitor/
 * nvreport.js). state is one fixed name (ST_NAMES in nvgpu.c: "ok", "starting", "noDriver", "libMissing", "dlopen",
 * "symbol", "init", "count", "noDevice", "sandbox", "killed", "crash", "exit", "timeout", "badMessage", "noHelper",
 * "spawn"); detail its number (an nvmlReturn_t, a signal, an exit code, an errno) or null; denied, syscalls and
 * ioctls what the helper's sandbox refused (numbers). driverVersion is /sys/module/nvidia/version (digits and dots)
 * or null; pci the PCI display devices of vendor 0x10de from sysfs, named from pci_ids (may be NULL). Only numbers and
 * fixed names, no path, host, user or pid. Its length; 0, writing nothing, without a 0x10de display device or when
 * it doesn't fit. Rescans sysfs at most every 30 s. */
int nvgpu_diag_json(char *out, size_t n, const char *pci_ids);
void nvgpu_want(int on);
void nvgpu_poll(void);
void nvgpu_suspend(void);

/* ---- internals, exposed for tests (tests/nvgpu_test.c) ---- */
#include "monitor/nvproto.h"

/* why there is no data (NVP_ST_*, its number) and what the helper's sandbox refused */
typedef struct {
    int code, detail, has_detail;
    unsigned denied;
    int nsys, sys[NVP_DENIED_MAX];
    int nioc;
    unsigned ioc[NVP_DENIED_MAX];
} NvDiag;

typedef struct {
    int ngpu, nproc;
    char cuda[16], driver[NVP_DRIVER_LEN];
    NvGpu gpu[NVP_MAX_GPUS];
    NvProc proc[NVP_MAX_PROCS];
    NvDiag diag;                        /* code NVP_ST_OK with GPUs, or the helper's reason (no GPUs then) */
} NvSample;

/* a 0x10de display device on PCI, from sysfs (nvgpu_diag_json) */
typedef struct { char slot[16], id[10], cls[8], driver[24], name[160]; } NvPci;

/* One message from the helper → a checked sample: bad header or sizes → -1; every out-of-range value becomes
 * absent; processes whose GPU index is wrong, whose pid is out of range or not owned by uid (own(pid, uid), at most
 * NVP_MAX_PROCS calls) are dropped. */
int nvgpu_parse(const unsigned char *msg, size_t len, unsigned uid, int (*own)(int pid, unsigned uid), NvSample *s);
/* the fragment of a checked sample; its length, or 0 when it doesn't fit (nothing written but a NUL) */
int nvgpu_format(const NvSample *s, char *out, size_t n);
/* the diagnostic's fragment (see nvgpu_diag_json) from its parts; its length, or 0 when it doesn't fit */
int nvgpu_diag_format(const NvDiag *d, const NvPci *pci, int npci, const char *driver_version, char *out, size_t n);
/* /proc/<pid> belongs to uid */
int nvgpu_pid_owned(int pid, unsigned uid);

#ifdef NVGPU_TEST
/* test builds only: the helper to run (path + argv + envp) and whether to skip the NVIDIA device check */
void nvgpu_test_helper(const char *path, char *const *argv, char *const *envp);
/* test builds only: the request timeout in ms (default 5000; the first sample gets twice) and the backoff's first step */
void nvgpu_test_timing(int timeout_ms, int backoff_ms);
/* test builds only: the current diagnostic */
void nvgpu_test_diag(NvDiag *d);
/* test builds only: the running helper's pid (0 none) and how many times it was started */
int nvgpu_test_pid(int *starts);
#endif

#endif
