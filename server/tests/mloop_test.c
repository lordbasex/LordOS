/* mloop_test.c — the moving wallpapers' rules without a screen (docs/MOVING-WALLPAPERS.md):
 *   - the loop cache's budget, memory pressure, recording of one whole pass and playback (media/mloop.c);
 *   - the pause / stop rule and the frame-rate cap;
 *   - a view hidden by the union of opaque windows (ui/rectsub.h, paint.c paint_node_hidden);
 *   - the frame rings' layouts (MM_RING: proto.c mp_ring_layout) and the zero-copy frames' descriptions
 *     (MM_DMAFRAME: mp_dma_check), malformed ones refused, plus a fuzz of random descriptions;
 *   - the NV12 → RGB matrices on the CPU (nv12.c) against known colours. */
#include "media/mloop.h"
#include "media/nv12.h"
#include "media/proto.h"
#include "ui/rectsub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ok, bad;
#define CHECK(c, ...) do { if (c) ok++; else { bad++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define MB (1024LL * 1024)

static void test_budget(void) {
    /* RX 580 8 GB, 1 GB used: the setting (1 GB) fits */
    CHECK(mloop_budget(1024, 8192 * MB, 1024 * MB, 32768 * MB, 0) == 1024 * MB, "discrete, setting");
    /* never more than half the VRAM */
    CHECK(mloop_budget(8192, 4096 * MB, 0, 32768 * MB, 0) == 2048 * MB, "half the VRAM");
    /* the card nearly full: only what is free past the headroom */
    int64_t b = mloop_budget(1024, 8192 * MB, 7400 * MB, 32768 * MB, 0);
    CHECK(b == 0, "full card: %lld", (long long)b);
    b = mloop_budget(1024, 8192 * MB, 7000 * MB, 32768 * MB, 0);
    CHECK(b > 0 && b < 1024 * MB, "nearly full: %lld MB", (long long)(b / MB));
    CHECK(mloop_budget(0, 8192 * MB, 0, 32768 * MB, 0) == 0, "0 = off");
    /* integrated: a sixteenth of the RAM, at most 1 GB */
    CHECK(mloop_budget(1024, -1, -1, 8192 * MB, 1) == 512 * MB, "integrated 8 GB");
    CHECK(mloop_budget(4096, 512 * MB, 100 * MB, 65536 * MB, 1) == 1024 * MB, "integrated cap 1 GB");
    CHECK(mloop_budget(256, -1, -1, 32768 * MB, 1) == 256 * MB, "integrated, setting smaller");
    /* pressure */
    CHECK(!mloop_pressure(8192 * MB, 2048 * MB, -1, -1, 0), "no pressure");
    CHECK(mloop_pressure(8192 * MB, 8000 * MB, -1, -1, 0), "VRAM pressure");
    CHECK(!mloop_pressure(8192 * MB, -1, -1, -1, 0), "unknown: none");
    CHECK(mloop_pressure(-1, -1, 500 * MB, 8192 * MB, 1), "RAM pressure");
    CHECK(!mloop_pressure(-1, -1, 4000 * MB, 8192 * MB, 1), "RAM fine");
    /* 1080p30 8 s ≈ 750 MB, 4K doesn't fit 1 GB */
    int64_t e = mloop_estimate(1920, 1080, 8000, 33);
    CHECK(e > 700 * MB && e < 800 * MB, "1080p30 8 s: %lld MB", (long long)(e / MB));
    CHECK(mloop_estimate(3840, 2160, 8000, 33) > 1024 * MB, "4K too large");
    CHECK(mloop_estimate(1920, 1080, 0, 33) < 0, "no duration");
}

static void test_record(void) {
    MLoopRec r;
    memset(&r, 0, sizeof r);
    /* first frames before a start is allowed: skipped */
    CHECK(mloop_rec_frame(&r, 0, 1920, 1080, 0) == MLR_SKIP, "no start yet");
    CHECK(mloop_rec_frame(&r, 33, 1920, 1080, 0) == MLR_SKIP, "no start yet 2");
    /* mid-clip: may start, but not at the loop's start: skipped */
    CHECK(mloop_rec_frame(&r, 66, 1920, 1080, 1) == MLR_SKIP, "mid clip");
    int64_t p = 99;
    for (; p < 6000; p += 33) mloop_rec_frame(&r, p, 1920, 1080, 1);
    /* the loop starts over: recording begins with this frame */
    CHECK(mloop_rec_frame(&r, 0, 1920, 1080, 1) == MLR_KEEP && r.n == 1, "starts at the loop's start");
    int kept = 1;
    for (p = 33; p < 6000; p += 33) kept += mloop_rec_frame(&r, p, 1920, 1080, 1) == MLR_KEEP;
    CHECK(kept == r.n && r.n == 182, "kept %d n %d", kept, r.n);
    CHECK(mloop_rec_frame(&r, 0, 1920, 1080, 1) == MLR_DONE, "the next turn ends the pass");
    int64_t period = 0;
    CHECK(mloop_rec_whole(&r, 6006, &period) && period == 6006, "whole, period %lld", (long long)period);
    /* a gap of 4 intervals: not whole */
    MLoopRec g;
    memset(&g, 0, sizeof g);
    mloop_rec_frame(&g, 0, 100, 100, 0);
    mloop_rec_frame(&g, 33, 100, 100, 0);
    mloop_rec_frame(&g, 0, 100, 100, 1);
    for (p = 33; p < 3000; p += 33) if (p < 1000 || p > 1140) mloop_rec_frame(&g, p, 100, 100, 1);
    CHECK(!mloop_rec_whole(&g, 3000, NULL), "a gap");
    /* doesn't reach the clip's end */
    MLoopRec e;
    memset(&e, 0, sizeof e);
    mloop_rec_frame(&e, 0, 100, 100, 0);
    mloop_rec_frame(&e, 33, 100, 100, 0);
    mloop_rec_frame(&e, 0, 100, 100, 1);
    for (p = 33; p < 2000; p += 33) mloop_rec_frame(&e, p, 100, 100, 1);
    CHECK(!mloop_rec_whole(&e, 8000, NULL), "short of the end");
    /* a size change aborts */
    CHECK(mloop_rec_frame(&e, 2100, 200, 100, 1) == MLR_ABORT && !e.on, "size change");
    /* too long */
    MLoopRec l;
    memset(&l, 0, sizeof l);
    mloop_rec_frame(&l, 0, 64, 64, 0);
    mloop_rec_frame(&l, 10, 64, 64, 0);
    mloop_rec_frame(&l, 0, 64, 64, 1);
    int ab = 0;
    for (p = 10; p < 20000 && !ab; p += 10) ab = mloop_rec_frame(&l, p, 64, 64, 1) == MLR_ABORT;
    CHECK(ab, "a pass past MLOOP_MAX_MS or MLOOP_MAX_FRAMES aborts");
}

static void test_pick(void) {
    int64_t pts[10];
    for (int i = 0; i < 10; i++) pts[i] = i * 100;
    int64_t next;
    CHECK(mloop_pick(pts, 10, 1000, 0, 0, -1, &next) == 0 && next == 100, "start");
    CHECK(mloop_pick(pts, 10, 1000, 250, 0, -1, &next) == 2 && next == 300, "middle");
    CHECK(mloop_pick(pts, 10, 1000, 999, 0, -1, &next) == 9 && next == 1000, "end");
    CHECK(mloop_pick(pts, 10, 1000, 1050, 0, -1, &next) == 0, "wraps");
    /* a 5 fps cap over a 10 fps loop: every other frame */
    CHECK(mloop_pick(pts, 10, 1000, 100, 5, 0, &next) == 0 && next == 198, "capped holds: next %lld", (long long)next);
    CHECK(mloop_pick(pts, 10, 1000, 200, 5, 0, &next) == 2, "capped moves on");
    CHECK(mloop_pick(pts, 0, 1000, 0, 0, -1, &next) == -1, "empty");
    /* the cap and the battery rule */
    CHECK(mloop_cap(0, 0) == 0 && mloop_cap(30, 0) == 30 && mloop_cap(15, 0) == 15 && mloop_cap(60, 0) == 0, "cap");
    CHECK(mloop_cap(0, 1) == 15 && mloop_cap(30, 1) == 15 && mloop_cap(15, 1) == 15, "battery lowers");
}

static void test_hold(void) {
    CHECK(mloop_hold(1, 0, 0) == MLH_RUN, "visible runs");
    CHECK(mloop_hold(1, 1, 0) == MLH_PAUSE, "JS paused");
    CHECK(mloop_hold(0, 0, 100) == MLH_PAUSE, "hidden pauses");
    CHECK(mloop_hold(0, 0, MLOOP_HIDDEN_STOP_MS - 1) == MLH_PAUSE, "hidden a while still pauses");
    CHECK(mloop_hold(0, 0, MLOOP_HIDDEN_STOP_MS) == MLH_STOP, "hidden long stops");
    CHECK(mloop_hold(0, 1, MLOOP_HIDDEN_STOP_MS + 5) == MLH_STOP, "hidden long and paused stops");
}

static void test_cover(void) {
    RsLeft l;
    RsBox d = { 0, 0, 1920, 1080 };
    rs_start(&l, d);
    rs_sub(&l, (RsBox){ 0, 0, 960, 1080 });
    CHECK(!rs_empty(&l), "half covered");
    rs_sub(&l, (RsBox){ 960, 0, 1920, 1080 });
    CHECK(rs_empty(&l), "two windows side by side cover it");
    rs_start(&l, d);
    rs_sub(&l, (RsBox){ 0, 28, 1920, 1080 });                 /* a maximized window under a menu bar */
    CHECK(!rs_empty(&l), "the menu bar's strip still shows");
    rs_sub(&l, (RsBox){ 0, 0, 1920, 28 });                    /* an opaque menu bar */
    CHECK(rs_empty(&l), "window + opaque bar");
    rs_start(&l, d);
    rs_sub(&l, (RsBox){ -100, -100, 3000, 3000 });
    CHECK(rs_empty(&l), "a fullscreen node");
    rs_start(&l, d);
    for (int i = 0; i < 400; i++) rs_sub(&l, (RsBox){ (float)(i * 4 % 1900) + 1, (float)(i * 7 % 1000) + 1, (float)(i * 4 % 1900) + 3, (float)(i * 7 % 1000) + 3 });
    CHECK(!rs_empty(&l), "many small covers: never 'hidden' by overflow");
    rs_start(&l, (RsBox){ 5, 5, 5, 9 });
    CHECK(rs_empty(&l), "an empty box");
}

static void test_ring(void) {
    MpRing L;
    CHECK(!mp_ring_layout(MF_BGRA, 1920, 1080, 3, 0, &L) && L.slot_bytes == 1920u * 1080 * 4, "BGRA");
    CHECK(!mp_ring_layout(MF_NV12, 1920, 1080, 3, MC_709, &L) && L.slot_bytes == 1920u * 1080 * 3 / 2 && L.uv_off == 1920u * 1080 &&
          L.y_stride == 1920 && L.uv_stride == 1920 && L.matrix == MC_709 && !L.full, "NV12");
    CHECK(!mp_ring_layout(MF_NV12, 2, 2, 1, MC_601 | MC_FULL, &L) && L.full && L.slot_bytes == 6, "NV12 small, full");
    /* refused */
    CHECK(mp_ring_layout(MF_NV12, 1921, 1080, 3, 0, &L), "odd width");
    CHECK(mp_ring_layout(MF_NV12, 1920, 1081, 3, 0, &L), "odd height");
    CHECK(mp_ring_layout(MF_NV12, 0, 1080, 3, 0, &L), "zero width");
    CHECK(mp_ring_layout(MF_NV12, -2, 1080, 3, 0, &L), "negative");
    CHECK(mp_ring_layout(MF_NV12, MP_MAX_DIM + 2, 1080, 3, 0, &L), "too wide");
    CHECK(mp_ring_layout(MF_NV12, 1920, 1080, 4, 0, &L), "too many slots");
    CHECK(mp_ring_layout(MF_NV12, 1920, 1080, 0, 0, &L), "no slot");
    CHECK(mp_ring_layout(MF_NV12, 1920, 1080, 3, 7, &L), "unknown matrix");
    CHECK(mp_ring_layout(MF_NV12, 1920, 1080, 3, 0x200, &L), "unknown colour bit");
    CHECK(mp_ring_layout(MF_NV12, 1920, 1080, 3, -1, &L), "negative colours");
    CHECK(mp_ring_layout(MF_BGRA, 1920, 1080, 3, 1, &L), "BGRA with colours");
    CHECK(mp_ring_layout(MF_DMABUF, 1920, 1080, 3, 0, &L), "dmabuf is never a ring");
    CHECK(mp_ring_layout(9, 1920, 1080, 3, 0, &L), "unknown format");
    CHECK(mp_ring_layout(MF_NV12, (int64_t)1 << 40, 1080, 3, 0, &L), "huge");
}

static MpDma good(void) {
    MpDma d;
    memset(&d, 0, sizeof d);
    d.magic = MP_DMA_MAGIC;
    d.w = 1920; d.h = 1080; d.colors = MC_709; d.nobj = 1;
    d.modifier[0] = 0;
    d.plane[0] = (typeof(d.plane[0])){ MP_FOURCC_R8, 0, 0, 2048 };
    d.plane[1] = (typeof(d.plane[0])){ MP_FOURCC_GR88, 0, 2048 * 1088, 2048 };
    return d;
}
static int allow_all(uint32_t f, uint64_t m) { (void)f; (void)m; return 1; }
static int allow_linear(uint32_t f, uint64_t m) { (void)f; return m == 0; }

static void test_dma(void) {
    int64_t size[2] = { 2048 * 1088 * 3 / 2, 0 };
    MpDma d = good();
    CHECK(!mp_dma_check(&d, sizeof d, 1, size, 2400, 1360, allow_all), "a good frame");
    CHECK(mp_dma_check(&d, sizeof d - 1, 1, size, 2400, 1360, allow_all), "short payload");
    CHECK(mp_dma_check(&d, sizeof d, 2, size, 2400, 1360, allow_all), "an extra fd");
    CHECK(mp_dma_check(&d, sizeof d, 0, size, 2400, 1360, allow_all), "no fd");
    CHECK(mp_dma_check(&d, sizeof d, 1, size, 1920, 1000, allow_all), "larger than the box");
    MpDma x;
    x = d; x.magic = 0; CHECK(mp_dma_check(&x, sizeof x, 1, size, 2400, 1360, NULL), "magic");
    x = d; x.w = 1921; CHECK(mp_dma_check(&x, sizeof x, 1, size, 2400, 1360, NULL), "odd width");
    x = d; x.h = 0; CHECK(mp_dma_check(&x, sizeof x, 1, size, 2400, 1360, NULL), "no height");
    x = d; x.colors = 9; CHECK(mp_dma_check(&x, sizeof x, 1, size, 2400, 1360, NULL), "bad colours");
    x = d; x.pad = 1; CHECK(mp_dma_check(&x, sizeof x, 1, size, 2400, 1360, NULL), "padding set");
    x = d; x.plane[0].pitch = 1000; CHECK(mp_dma_check(&x, sizeof x, 1, size, 2400, 1360, NULL), "pitch shorter than a row");
    x = d; x.plane[0].pitch = 70000; CHECK(mp_dma_check(&x, sizeof x, 1, size, 2400, 1360, NULL), "pitch too large");
    x = d; x.plane[1].offset = 2048 * 1088 * 3 / 2 - 100; CHECK(mp_dma_check(&x, sizeof x, 1, size, 2400, 1360, NULL), "U-V past the end");
    x = d; x.plane[1].offset = 0xffffffffu; CHECK(mp_dma_check(&x, sizeof x, 1, size, 2400, 1360, NULL), "offset overflow");
    x = d; x.plane[1].offset = 1000; CHECK(mp_dma_check(&x, sizeof x, 1, size, 2400, 1360, NULL), "planes overlap");
    x = d; x.plane[1].obj = 1; CHECK(mp_dma_check(&x, sizeof x, 1, size, 2400, 1360, NULL), "object out of range");
    x = d; x.plane[0].fourcc = MP_FOURCC('A', 'R', '2', '4'); CHECK(mp_dma_check(&x, sizeof x, 1, size, 2400, 1360, NULL), "not a plane format");
    x = d; x.plane[0].fourcc = MP_FOURCC_R16; CHECK(mp_dma_check(&x, sizeof x, 1, size, 2400, 1360, NULL), "R16 with GR88");
    x = d; x.nobj = 2; { int64_t s2[2] = { size[0], 4096 }; CHECK(mp_dma_check(&x, sizeof x, 2, s2, 2400, 1360, NULL), "an object unused"); }
    { int64_t s0[2] = { 0, 0 }; CHECK(mp_dma_check(&d, sizeof d, 1, s0, 2400, 1360, NULL), "empty object"); }
    { int64_t sb[2] = { (int64_t)MP_DMA_MAX_BYTES + 1, 0 }; CHECK(mp_dma_check(&d, sizeof d, 1, sb, 2400, 1360, NULL), "huge object"); }
    x = d; x.modifier[0] = 0x0200000000000001ull; CHECK(mp_dma_check(&x, sizeof x, 1, size, 2400, 1360, allow_linear), "modifier not offered");
    /* two objects, 10 bits */
    MpDma t = d;
    t.nobj = 2;
    t.plane[0] = (typeof(t.plane[0])){ MP_FOURCC_R16, 0, 0, 4096 };
    t.plane[1] = (typeof(t.plane[0])){ MP_FOURCC_GR1616, 1, 0, 4096 };
    int64_t s2[2] = { 4096 * 1080, 4096 * 540 };
    CHECK(!mp_dma_check(&t, sizeof t, 2, s2, 2400, 1360, allow_all), "P010 in two objects");
    s2[1] = 4096 * 539;
    CHECK(mp_dma_check(&t, sizeof t, 2, s2, 2400, 1360, allow_all), "P010 U-V short");
    CHECK(mp_dma_check(NULL, sizeof d, 1, size, 2400, 1360, NULL), "NULL");
    /* fuzz: random bytes and random single changes never pass unless the result is still inside its objects */
    srand(1);
    int passed = 0, wrong = 0;
    for (int i = 0; i < 200000; i++) {
        MpDma f = good();
        uint8_t *b = (uint8_t *)&f;
        int n = 1 + rand() % 4;
        for (int k = 0; k < n; k++) b[rand() % sizeof f] ^= (uint8_t)(1u << (rand() % 8));
        int64_t sz[2] = { (int64_t)(rand() % 4 ? 2048 * 1088 * 3 / 2 : rand()), rand() % 2 ? 4096 : 0 };
        int nf = 1 + rand() % 2;
        if (mp_dma_check(&f, sizeof f, nf, sz, 8192, 8192, NULL)) continue;
        passed++;
        /* what passed must hold: each plane inside its object, the objects all used */
        int deep = f.plane[0].fourcc == MP_FOURCC_R16;
        for (int p = 0; p < 2; p++) {
            uint64_t rows = p ? f.h / 2 : f.h, row = (uint64_t)f.w * (deep ? 2 : 1);
            if (f.plane[p].obj >= f.nobj || (uint64_t)f.plane[p].offset + (uint64_t)f.plane[p].pitch * (rows - 1) + row > (uint64_t)sz[f.plane[p].obj] ||
                f.plane[p].pitch < row || (f.w & 1) || (f.h & 1) || (int)f.nobj != nf) wrong++;
        }
    }
    CHECK(wrong == 0, "fuzz: %d passed, %d wrong", passed, wrong);
}

static void test_nv12(void) {
    /* limited-range BT.709: Y 16 → black, 235 → white; full range BT.601 mid grey */
    uint8_t y[4] = { 16, 235, 16, 235 }, uv[2] = { 128, 128 };
    uint32_t out[4];
    nv12_to_argb(y, 2, uv, 2, 2, 2, MC_709, 0, out, 2, 0, 2);
    CHECK(out[0] == 0xff000000u && out[1] == 0xffffffffu, "black and white: %08x %08x", out[0], out[1]);
    uint8_t y2[4] = { 128, 128, 128, 128 };
    nv12_to_argb(y2, 2, uv, 2, 2, 2, MC_601, 1, out, 2, 0, 2);
    CHECK(out[0] == 0xff808080u, "grey: %08x", out[0]);
    /* BT.709 limited red (Y 63, Cb 102, Cr 240) */
    uint8_t yr[4] = { 63, 63, 63, 63 }, uvr[2] = { 102, 240 };
    nv12_to_argb(yr, 2, uvr, 2, 2, 2, MC_709, 0, out, 2, 0, 2);
    int r = (int)(out[0] >> 16 & 255), g = (int)(out[0] >> 8 & 255), b = (int)(out[0] & 255);
    CHECK(r > 250 && g < 5 && b < 5, "red: %d %d %d", r, g, b);
    /* rows outside the frame are clamped, never read */
    nv12_to_argb(y, 2, uv, 2, 2, 2, MC_709, 0, out, 2, -5, 99);
    CHECK(out[3] == 0xffffffffu, "clamped rows");
}

int main(void) {
    test_budget();
    test_record();
    test_pick();
    test_hold();
    test_cover();
    test_ring();
    test_dma();
    test_nv12();
    printf("mloop-test: %d ok, %d failed\n", ok, bad);
    return bad ? 1 : 0;
}
