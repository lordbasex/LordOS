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

#define NVP_MAGIC      0x3147564eu      /* "NVG1" */
#define NVP_REQ_SAMPLE 's'
#define NVP_MAX_GPUS   64
#define NVP_MAX_PROCS  256              /* per sample, all GPUs together */
#define NVP_DRIVER_LEN 32
#define NVP_NONE       (-1)
#define NVP_NONE64     (-1LL)

/* nvmlComputeMode_t, sent as its number; lord-server names it */
enum { NVP_CM_DEFAULT = 0, NVP_CM_EXCLUSIVE_THREAD = 1, NVP_CM_PROHIBITED = 2, NVP_CM_EXCLUSIVE_PROCESS = 3 };

typedef struct {
    uint32_t magic;
    uint16_t ngpu, nproc;
    int32_t  cuda;                      /* nvmlSystemGetCudaDriverVersion: 12040 = 12.4; NVP_NONE */
    int32_t  status;                    /* 0 ok; otherwise an NVML error number (no GPUs are sent) */
    char     driver[NVP_DRIVER_LEN];    /* "550.54.14", NUL-terminated, digits and dots; "" unknown */
} NvHdr;

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
