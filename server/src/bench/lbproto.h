/* lbproto.h — the channel between lord-server (lbrun.c) and lord-bench (lbmain.c), LordBench's
 * sandboxed helper (docs/BENCHMARK.md, "0.8.6 — LordBench").
 *
 * A SOCK_SEQPACKET pair on fd 3. lord-server sends one LbGo (with up to two fds: the GPU's render node
 * and a storage scratch file, both opened by lord-server); lord-bench answers only with fixed-size
 * LbMsg records: no text, no paths. lord-server checks every field (lb_msg_valid) and builds the JSON
 * itself. To stop a run, lord-server kills the helper. */
#ifndef LORD_BENCH_LBPROTO_H
#define LORD_BENCH_LBPROTO_H

#include <stdint.h>

#define LB_MAGIC   0x4c424e31u          /* "LBN1" */
#define LB_VERSION 1                    /* LordBench's version: results compare only with the same one */

/* what to run */
enum { LB_MODE_QUICK = 1, LB_MODE_FULL = 2,
       LB_MODE_LOAD = 3 };              /* desktop impact: only the all-core load for sustained_s, a sample a second */
/* LbGo.flags */
enum {
    LB_GO_GPU     = 1,                  /* an fd with the render node follows */
    LB_GO_STORAGE = 2,                  /* an fd with the scratch file follows (after the GPU's) */
    LB_GO_LOW     = 4,                  /* desktop impact: lowest priority */
};

typedef struct {
    uint32_t magic, mode, flags, threads;
    uint32_t storage_mb;                /* the scratch file's size limit */
    uint32_t sustained_s;               /* the sustained test's length (Full only), 0 = none */
} LbGo;

/* the measurements; their names and units are lb_name()/lb_unit() */
enum {
    LB_CPU_SHA1 = 0,                    /* SHA-256, one core, MB/s */
    LB_CPU_SHAN,                        /* SHA-256, all cores, MB/s */
    LB_CPU_MAT1,                        /* matrix multiply, one core, GFLOP/s */
    LB_CPU_MATN,                        /* matrix multiply, all cores, GFLOP/s */
    LB_CPU_XZ1,                         /* xz decompression (the package engine's decoder), one core, MB/s */
    LB_CPU_XZN,                         /* the same on all cores, MB/s */
    LB_CPU_JS1,                         /* QuickJS running a fixed script, one core, runs/s */
    LB_CPU_JSN,                         /* the same on all cores, runs/s */
    LB_CPU_UI1,                         /* the UI engine's layout and paint of a fixed 1920×1080 scene, one core, frames/s (lbui.h) */
    LB_CPU_UIN,                         /* the same with the paint's row bands on up to 8 threads (as lord-server's paint threads), frames/s */
    LB_CPU_SHAPE1,                      /* shaping fixed paragraphs in five scripts (lbtext.h), one core, runs/s */
    LB_CPU_SHAPEN,                      /* the same on all cores, runs/s */
    LB_MEM_COPY,                        /* memory bandwidth, copy, GB/s */
    LB_MEM_TRIAD,                       /* memory bandwidth, triad, GB/s */
    LB_MEM_LAT,                         /* memory latency, a pointer chase over 64 MB, ns */
    LB_GPU_SCENE,                       /* the 3D scene offscreen at 1920×1080, frames/s */
    LB_GPU_FILL,                        /* fill rate, Mpixel/s */
    LB_GPU_UPLOAD,                      /* texture upload, MB/s */
    LB_GPU_LAYERS,                      /* composing 8 screen-size layers, frames/s */
    LB_DISK_SEQW,                       /* storage, sequential write, MB/s */
    LB_DISK_SEQR,                       /* storage, sequential read, MB/s */
    LB_DISK_RAND,                       /* storage, random 4 KiB reads, IOPS */
    LB_SUS_START,                       /* sustained: all-core matrix multiply, first 10 s, GFLOP/s */
    LB_SUS_END,                         /* sustained: last 10 s, GFLOP/s */
    /* measured by lord-media (bench/lbvideo.c, media/helper/lm_vbench.c), never by lord-bench */
    LB_VID_H264_1080,                   /* software decoding of a generated clip, frames/s */
    LB_VID_HEVC_1080,
    LB_VID_VP9_1080,
    LB_VID_AV1_1080,
    LB_VID_H264_2160,
    LB_VID_HEVC_2160,
    LB_VENC_H264,                       /* the remote desktop's encoders with its own settings, 1920×1080, frames/s */
    LB_VENC_VP8,
    /* measured by lord-server itself (bench/lbframes.c), desktop mode only */
    LB_COMP_FPS,                        /* the compositor, a scripted scene, nothing else running: frames shown per second */
    LB_COMP_P50,                        /* ms to build a frame, median */
    LB_COMP_P99,                        /* ms to build a frame, 99th percentile */
    LB_COMP_MISSED,                     /* intervals longer than 1.5 refresh periods, % */
    LB_COMP_IN50,                       /* ms from a synthetic pointer event to the frame showing it, median (lbinput.h) */
    LB_COMP_IN99,                       /* the same, 99th percentile */
    LB_DESK_FPS,                        /* the same scene with an all-core load at the lowest priority */
    LB_DESK_P50,
    LB_DESK_P99,
    LB_DESK_MISSED,
    LB_DESK_IN50,
    LB_DESK_IN99,
    LB_DESK_WORK,                       /* what the load got done meanwhile, GFLOP/s */
    LB_COUNT
};
#define LB_HELPER_LAST LB_SUS_END       /* lord-bench may only report ids up to this one */
#define LB_VID_FIRST   LB_VID_H264_1080
#define LB_VID_LAST    LB_VENC_VP8
#define LB_DESK_FIRST  LB_COMP_FPS
#define LB_DESK_LAST   LB_DESK_WORK

/* LbMsg.kind */
enum {
    LB_MSG_STEP = 1,                    /* id: the measurement starting now */
    LB_MSG_RESULT,                      /* id, value (and aux: the spread, max − min over the repeats) */
    LB_MSG_SKIP,                        /* id, why (aux, an LB_SKIP_*) */
    LB_MSG_SAMPLE,                      /* sustained: value = GFLOP/s of the last interval, aux = seconds since start */
    LB_MSG_DONE,                        /* the run ended; aux = the sandbox mask */
    LB_MSG_GPUINFO,                     /* value = 0 hardware, 1 virtual, 2 software (bench_gl_kind) */
    LB_MSG_SANDBOX,                     /* id: an LB_SBX_* try, value 1 refused (0 it worked), aux = its errno */
};

/* The helper-sandbox self-test (sec.helper-sandbox; the same tries as the Sandbox Probe's helper child,
 * app/probe.c), made by lord-bench right after sandbox_apply_helper and before any work. Every one must
 * fail, and none could change anything if it didn't: a socket or a file that opened is closed at once
 * (the file is unnamed, O_TMPFILE), exec is called with an invalid environment pointer (an exec the
 * sandbox allows fails with EFAULT before anything is replaced), and a fork that worked would only
 * _exit(0) in the child. */
enum {
    LB_SBX_SOCKET = 0,                  /* socket(AF_UNIX): no new sockets (a unix one could reach the session bus) */
    LB_SBX_INET,                        /* socket(AF_INET): no network */
    LB_SBX_EXEC,                        /* execve("/usr/bin/true"): no programs */
    LB_SBX_FORK,                        /* fork(): threads only */
    LB_SBX_NEWFILE,                     /* O_TMPFILE in /tmp: no writable folder */
    LB_SBX_READ,                        /* /etc/passwd: no files outside its read-only system folders */
    LB_SBX_COUNT
};
enum { LB_SKIP_NODEV = 1, LB_SKIP_FAILED, LB_SKIP_MODE, LB_SKIP_MEMORY,
       LB_SKIP_NOCODEC,                 /* video: this system's FFmpeg has no encoder or decoder for it */
       LB_SKIP_NOFRAMES };              /* desktop: the compositor showed too few frames (another session in front) */

typedef struct {
    uint32_t magic, kind, id, pad;
    double value, aux;
} LbMsg;

const char *lb_name(int id);            /* "cpu.sha256.1", …; NULL out of range */
const char *lb_sbx_name(int id);        /* "helperSocket", …; NULL out of range */
const char *lb_unit(int id);            /* "MB/s", "GFLOP/s", "runs/s", "GB/s", "ns", "fps", "Mpx/s", "IOPS", "ms", "%" */
int         lb_lower_better(int id);    /* 1 for latency, frame times and missed frames */
/* 1 if a record from lord-bench is acceptable: magic, kind, an id lord-bench measures (or a sandbox try),
 * finite values in range */
int         lb_msg_valid(const LbMsg *m);

#endif
