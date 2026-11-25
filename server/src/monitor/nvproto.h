/* nvproto.h — lord-nvgpu's wire format (docs/MONITOR.md, "NVIDIA"; AGENTS.md, lord-nvgpu).
 *
 * lord-server and lord-nvgpu share a SOCK_SEQPACKET socketpair (the helper's fd 3). lord-server sends one byte
 * (NVP_REQ_SAMPLE) when it wants a sample, at most once a second; the helper answers with exactly one message:
 * an NvHdr, then hdr.ngpu NvGpu records, then hdr.nproc NvProc records. Fixed sizes, numbers only. The one text
 * field, the driver version, is a bounded, NUL-terminated string of digits and dots, re-checked by lord-server.
 * Every value the helper doesn't have is NVP_NONE (or NVP_NONE64). lord-server range-checks every field
 * (nvgpu_parse) and turns anything out of range into "absent"; it never trusts a count or an index. */
#ifndef LORD_MON_NVPROTO_H
#define LORD_MON_NVPROTO_H

#include <stdint.h>

#define NVP_MAGIC      0x3247564eu      /* "NVG2" (0.8.9.6.1: the failure code and the sandbox's refusals) */
#define NVP_REQ_SAMPLE 's'
#define NVP_MAX_GPUS   64
#define NVP_MAX_PROCS  256              /* per sample, all GPUs together */
#define NVP_DRIVER_LEN 32
#define NVP_NONE       (-1)
#define NVP_NONE64     (-1LL)

#define NVP_DENIED_MAX 8                /* distinct refused syscalls and ioctl requests kept */

/* Why there is no NVIDIA data: a fixed code and a few numbers, never a string from NVML or the driver. The helper sends
 * NVP_ST_OK..NVP_ST_NO_DEVICE in NvHdr.code; the rest lord-server sets itself (nvgpu.c) from what it saw: the
 * helper's exit status, a timeout, a bad message. nvgpu.c's ST_NAMES gives each its fixed name for the JSON. */
enum {
    NVP_ST_OK = 0,
    NVP_ST_NO_DRIVER,                   /* helper: no 0x10de device bound to `nvidia` */
    NVP_ST_LIB_MISSING,                 /* helper: libnvidia-ml.so.1 not found, or not root's / writable by others */
    NVP_ST_DLOPEN,                      /* helper: dlopen failed */
    NVP_ST_SYMBOL,                      /* helper: nvmlInit / DeviceGetCount / GetHandleByIndex missing; detail = which (1-3) */
    NVP_ST_INIT,                        /* helper: nvmlInit failed; detail = nvmlReturn_t */
    NVP_ST_COUNT,                       /* helper: nvmlDeviceGetCount failed; detail = nvmlReturn_t */
    NVP_ST_NO_DEVICE,                   /* helper: NVML found no device (or no handle) */
    NVP_ST_SANDBOX,                     /* lord-server: the helper exited 70 (seccomp couldn't be applied) */
    NVP_ST_KILLED,                      /* lord-server: the helper died of SIGSYS (killed by the sandbox) */
    NVP_ST_CRASH,                       /* lord-server: the helper died of another signal; detail = the signal */
    NVP_ST_EXIT,                        /* lord-server: the helper exited; detail = its exit code */
    NVP_ST_TIMEOUT,                     /* lord-server: no answer in time (killed) */
    NVP_ST_BAD_MESSAGE,                 /* lord-server: an answer that failed nvgpu_parse */
    NVP_ST_NO_HELPER,                   /* lord-server: lord-nvgpu missing or not root's */
    NVP_ST_SPAWN,                       /* lord-server: socketpair / posix_spawn failed; detail = errno */
    NVP_ST_STARTING,                    /* lord-server: started, no answer yet */
    NVP_ST_COUNT_
};

/* nvmlComputeMode_t, sent as its number; lord-server names it */
enum { NVP_CM_DEFAULT = 0, NVP_CM_EXCLUSIVE_THREAD = 1, NVP_CM_PROHIBITED = 2, NVP_CM_EXCLUSIVE_PROCESS = 3 };

typedef struct {
    uint32_t magic;
    uint16_t ngpu, nproc;
    int32_t  cuda;                      /* nvmlSystemGetCudaDriverVersion: 12040 = 12.4; NVP_NONE */
    int32_t  status;                    /* 0 ok; otherwise an NVML error number (no GPUs are sent) */
    char     driver[NVP_DRIVER_LEN];    /* "550.54.14", NUL-terminated, digits and dots; "" unknown */
    int32_t  code;                      /* NVP_ST_OK, or NVP_ST_NO_DRIVER..NVP_ST_NO_DEVICE (no GPUs are sent) */
    int32_t  detail;                    /* the code's number: an nvmlReturn_t, the missing symbol (1-3); else 0 */
    /* what the sandbox refused since it was applied (the SIGSYS handler in nvgpu_main.c records them; see there) */
    uint32_t denied;                    /* how many refused calls in all */
    int32_t  denied_nr[NVP_DENIED_MAX]; /* the first distinct syscall numbers, NVP_NONE in unused slots */
    uint32_t denied_ioctl[NVP_DENIED_MAX];  /* the first distinct refused ioctl requests (whole number), 0 in unused slots */
    uint32_t pad;                       /* 0; keeps the records after the header 8-byte aligned */
} NvHdr;

_Static_assert(sizeof(NvHdr) % 8 == 0, "NvGpu and NvProc follow the header at 8-byte alignment");

typedef struct {
    uint32_t domain;                    /* PCI address, numbers only: lord-server formats it */
    uint32_t bus, dev, fn;
    int32_t  util_sm, util_mem, util_enc, util_dec;   /* percent */
    int32_t  temp;                      /* °C */
    int32_t  fan;                       /* percent */
    int32_t  clk_core, clk_mem;         /* MHz */
    int32_t  compute_mode;              /* NVP_CM_* */
    int32_t  ecc;                       /* 0 off, 1 on */
    int64_t  power_mw, power_limit_mw;
    int64_t  vram_used, vram_total;     /* bytes */
} NvGpu;

typedef struct {
    uint32_t gpu;                       /* index into this message's GPUs */
    int32_t  pid;
    int32_t  sm, enc, dec;              /* percent, NVP_NONE */
    int32_t  pad;
    int64_t  mem;                       /* bytes, NVP_NONE64 */
} NvProc;

#define NVP_MSG_MAX (sizeof(NvHdr) + NVP_MAX_GPUS * sizeof(NvGpu) + NVP_MAX_PROCS * sizeof(NvProc))

#endif
