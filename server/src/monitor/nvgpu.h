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
void nvgpu_want(int on);
void nvgpu_poll(void);
void nvgpu_suspend(void);

/* ---- internals, exposed for tests (tests/nvgpu_test.c) ---- */
#include "monitor/nvproto.h"

typedef struct {
    int ngpu, nproc;
    char cuda[16], driver[NVP_DRIVER_LEN];
    NvGpu gpu[NVP_MAX_GPUS];
    NvProc proc[NVP_MAX_PROCS];
} NvSample;

/* One message from the helper → a checked sample: bad header or sizes → -1; every out-of-range value becomes
 * absent; processes whose GPU index is wrong, whose pid is out of range or not owned by uid (own(pid, uid), at most
 * NVP_MAX_PROCS calls) are dropped. */
int nvgpu_parse(const unsigned char *msg, size_t len, unsigned uid, int (*own)(int pid, unsigned uid), NvSample *s);
/* the fragment of a checked sample; its length, or 0 when it doesn't fit (nothing written but a NUL) */
int nvgpu_format(const NvSample *s, char *out, size_t n);
/* /proc/<pid> belongs to uid */
int nvgpu_pid_owned(int pid, unsigned uid);

#ifdef NVGPU_TEST
/* test builds only: the helper to run (path + argv + envp) and whether to skip the NVIDIA device check */
void nvgpu_test_helper(const char *path, char *const *argv, char *const *envp);
/* test builds only: the request timeout in ms (default 5000; the first sample gets twice) and the backoff's first step */
void nvgpu_test_timing(int timeout_ms, int backoff_ms);
/* test builds only: the running helper's pid (0 none) and how many times it was started */
int nvgpu_test_pid(int *starts);
#endif

#endif
