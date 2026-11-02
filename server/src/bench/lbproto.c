/* lbproto.c — see lbproto.h. */
#include "lbproto.h"

#include <math.h>
#include <stddef.h>

static const struct { const char *name, *unit; } meas[LB_COUNT] = {
    [LB_CPU_SHA1] = { "cpu.sha256.1", "MB/s" },     [LB_CPU_SHAN] = { "cpu.sha256.n", "MB/s" },
    [LB_CPU_MAT1] = { "cpu.matmul.1", "GFLOP/s" },  [LB_CPU_MATN] = { "cpu.matmul.n", "GFLOP/s" },
    [LB_CPU_XZ1] = { "cpu.xz.1", "MB/s" },          [LB_CPU_XZN] = { "cpu.xz.n", "MB/s" },
    [LB_CPU_JS1] = { "cpu.js.1", "runs/s" },        [LB_CPU_JSN] = { "cpu.js.n", "runs/s" },
    [LB_CPU_UI1] = { "cpu.ui.1", "fps" },           [LB_CPU_UIN] = { "cpu.ui.n", "fps" },
    [LB_CPU_SHAPE1] = { "cpu.shape.1", "runs/s" },  [LB_CPU_SHAPEN] = { "cpu.shape.n", "runs/s" },
    [LB_MEM_COPY] = { "mem.copy", "GB/s" },         [LB_MEM_TRIAD] = { "mem.triad", "GB/s" },
    [LB_MEM_LAT] = { "mem.latency", "ns" },
    [LB_GPU_SCENE] = { "gpu.scene", "fps" },        [LB_GPU_FILL] = { "gpu.fill", "Mpx/s" },
    [LB_GPU_UPLOAD] = { "gpu.upload", "MB/s" },     [LB_GPU_LAYERS] = { "gpu.layers", "fps" },
    [LB_DISK_SEQW] = { "disk.seqwrite", "MB/s" },   [LB_DISK_SEQR] = { "disk.seqread", "MB/s" },
    [LB_DISK_RAND] = { "disk.random4k", "IOPS" },
    [LB_SUS_START] = { "sustained.start", "GFLOP/s" }, [LB_SUS_END] = { "sustained.end", "GFLOP/s" },
    [LB_VID_H264_1080] = { "video.h264.1080", "fps" }, [LB_VID_HEVC_1080] = { "video.hevc.1080", "fps" },
    [LB_VID_VP9_1080] = { "video.vp9.1080", "fps" },   [LB_VID_AV1_1080] = { "video.av1.1080", "fps" },
    [LB_VID_H264_2160] = { "video.h264.2160", "fps" }, [LB_VID_HEVC_2160] = { "video.hevc.2160", "fps" },
    [LB_VENC_H264] = { "video.enc.h264", "fps" },      [LB_VENC_VP8] = { "video.enc.vp8", "fps" },
    [LB_COMP_FPS] = { "comp.fps", "fps" },  [LB_COMP_P50] = { "comp.p50", "ms" },
    [LB_COMP_P99] = { "comp.p99", "ms" },   [LB_COMP_MISSED] = { "comp.missed", "%" },
    [LB_COMP_IN50] = { "comp.in50", "ms" }, [LB_COMP_IN99] = { "comp.in99", "ms" },
    [LB_DESK_FPS] = { "desk.fps", "fps" },  [LB_DESK_P50] = { "desk.p50", "ms" },
    [LB_DESK_P99] = { "desk.p99", "ms" },   [LB_DESK_MISSED] = { "desk.missed", "%" },
    [LB_DESK_IN50] = { "desk.in50", "ms" }, [LB_DESK_IN99] = { "desk.in99", "ms" },
    [LB_DESK_WORK] = { "desk.work", "GFLOP/s" },
};

static const char *const sbx[LB_SBX_COUNT] = {
    [LB_SBX_SOCKET] = "helperSocket", [LB_SBX_INET] = "helperInet", [LB_SBX_EXEC] = "helperExec",
    [LB_SBX_FORK] = "helperFork", [LB_SBX_NEWFILE] = "helperNewFile", [LB_SBX_READ] = "helperRead",
};

const char *lb_name(int id) { return id >= 0 && id < LB_COUNT ? meas[id].name : NULL; }
const char *lb_sbx_name(int id) { return id >= 0 && id < LB_SBX_COUNT ? sbx[id] : NULL; }
const char *lb_unit(int id) { return id >= 0 && id < LB_COUNT ? meas[id].unit : NULL; }
int lb_lower_better(int id) {
    return id == LB_MEM_LAT || id == LB_COMP_P50 || id == LB_COMP_P99 || id == LB_COMP_MISSED || id == LB_COMP_IN50 ||
           id == LB_COMP_IN99 || id == LB_DESK_P50 || id == LB_DESK_P99 || id == LB_DESK_MISSED || id == LB_DESK_IN50 ||
           id == LB_DESK_IN99;
}

int lb_msg_valid(const LbMsg *m) {
    if (m->magic != LB_MAGIC || !isfinite(m->value) || !isfinite(m->aux)) return 0;
    switch (m->kind) {
    case LB_MSG_STEP: return m->id <= LB_HELPER_LAST;
    case LB_MSG_RESULT: return m->id <= LB_HELPER_LAST && m->value > 0 && m->value < 1e12 && m->aux >= 0 && m->aux < 1e12;
    case LB_MSG_SKIP: return m->id <= LB_HELPER_LAST && m->aux >= LB_SKIP_NODEV && m->aux <= LB_SKIP_MEMORY;
    case LB_MSG_SAMPLE: return m->value >= 0 && m->value < 1e9 && m->aux >= 0 && m->aux < 86400;
    case LB_MSG_DONE: return m->aux >= 0 && m->aux < 65536;
    case LB_MSG_GPUINFO: return m->value == 0 || m->value == 1 || m->value == 2;
    case LB_MSG_SANDBOX: return m->id < LB_SBX_COUNT && (m->value == 0 || m->value == 1) && m->aux >= 0 && m->aux < 4096 && m->aux == (double)(int)m->aux;
    }
    return 0;
}
