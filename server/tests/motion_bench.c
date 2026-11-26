/* motion_bench.c — moving wallpapers measured on this machine's GPU without a screen (docs/MOVING-WALLPAPERS.md,
 * "Measurements"). It launches build/lord-media as lord-server does and does lord-server's per-frame work on a
 * surfaceless GPU context of a render node (gpu_init_offscreen): never a display, never the session's.
 *
 *   build/motion-bench check RENDER
 *        the NV12 programs against the CPU's conversion (nv12.c), and a frame packed for the loop cache and drawn back
 *   build/motion-bench run RENDER FILE MODE VIEWS SECONDS [W H] [CAP]
 *        MODE: bgra    0.8.9.6's path: lord-media converts to BGRA; per view a copy into a display's canvas and its upload
 *              nv12    the decoder's planes through the memfd ring, one upload and one GPU conversion a frame
 *              dmabuf  zero-copy: the VA-API surface imported (mp_dma_check, gpu_yuv_import), one GPU conversion
 *              cache   zero-copy until one whole pass is packed on the GPU, then the loop from there, lord-media paused
 *        VIEWS: displays showing it (bgra: per-view copies; the GPU modes compose on the GPU, nothing per view here)
 *        prints frames/s, the largest gap, lord-media's and this process's CPU (% of one core) and the cache's MB. */
#include "../src/media/proto.h"
#include "../src/media/mloop.h"
#include "../src/media/nv12.h"
#include "../src/gpu/gpu.h"
#include "../src/canvas.h"

#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char payload[MP_PAYLOAD_MAX + 1];

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double self_cpu_ms(void) {
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
    return (u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1e3 + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e3;
}
static double proc_cpu_ms(pid_t pid) {
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(p, "r");
    long long ut = 0, st = 0;
    if (f) { if (fscanf(f, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lld %lld", &ut, &st) != 2) ut = st = 0; fclose(f); }
    return (ut + st) * 1000.0 / (double)sysconf(_SC_CLK_TCK);
}

static int spawn(pid_t *pid) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    int sz = 1 << 18;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
    *pid = fork();
    if (*pid == 0) {
        dup2(sv[1], 3);
        char *const argv[] = { "lord-media", NULL };
        char *env[] = { "LANG=C.UTF-8", NULL };
        execve("build/lord-media", argv, env);
        _exit(127);
    }
    close(sv[1]);
    return sv[0];
}

/* ---- check ---- */

static int check(void) {
    int w = 640, h = 360, bad = 0;
    uint8_t *y = malloc((size_t)w * h), *uv = malloc((size_t)w * h / 2);
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) y[j * w + i] = (uint8_t)(16 + (i * 219 / w + j) % 220);
    for (int j = 0; j < h / 2; j++) for (int i = 0; i < w / 2; i++) { uv[j * w + 2 * i] = (uint8_t)(16 + i * 224 / (w / 2)); uv[j * w + 2 * i + 1] = (uint8_t)(240 - j * 224 / (h / 2)); }   /* smooth: bilinear and nearest agree within a step */
    uint32_t *cpu = malloc((size_t)w * h * 4), *gpu = malloc((size_t)w * h * 4), *gp2 = malloc((size_t)w * h * 4);
    int matrices[3] = { MC_601, MC_709, MC_2020 };
    for (int mi = 0; mi < 3; mi++)
        for (int full = 0; full < 2; full++) {
            nv12_to_argb(y, w, uv, w, w, h, matrices[mi], full, cpu, w, 0, h);
            GpuYuv *t = gpu_yuv_upload(NULL, y, w, uv, w, w, h);
            GpuTex *rgb = NULL;
            if (!t || gpu_yuv_convert(t, &rgb, matrices[mi], full) || gpu_tex_read(rgb, gpu, w, h)) { printf("FAIL convert m%d f%d\n", mi, full); bad++; continue; }
            /* the CPU's chroma is nearest, the GPU's bilinear: compare where chroma is flat along x (even pixels) */
            int worst = 0;
            for (int j = 0; j < h; j += 2) for (int i = 0; i < w; i += 2) {
                uint32_t a = cpu[j * w + i], b = gpu[j * w + i];
                for (int k = 0; k < 24; k += 8) { int d = abs((int)(a >> k & 255) - (int)(b >> k & 255)); if (d > worst) worst = d; }
            }
            GpuPack *k = gpu_yuv_pack(t);
            GpuTex *rgb2 = NULL;
            int pk = k && !gpu_pack_convert(k, &rgb2, matrices[mi], full) && !gpu_tex_read(rgb2, gp2, w, h);
            int pworst = 0;
            if (pk) for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
                uint32_t a = cpu[j * w + i], b = gp2[j * w + i];
                for (int q = 0; q < 24; q += 8) { int d = abs((int)(a >> q & 255) - (int)(b >> q & 255)); if (d > pworst) pworst = d; }
            }
            printf("matrix %d full %d: GPU vs CPU max %d, packed (cache) vs CPU max %d%s\n", matrices[mi], full, worst, pworst, pk ? "" : " (no pack)");
            if (worst > 6 || !pk || pworst > 6) bad++;
            gpu_tex_free(rgb); gpu_tex_free(rgb2); gpu_pack_free(k); gpu_yuv_free(t);
        }
    printf("check: %s\n", bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}

/* ---- run ---- */

static int plane_ok(uint32_t f, uint64_t m) { return gpu_dmabuf_plane_ok(f, m); }
static const char *render_path;

static int run(const char *file, const char *mode, int views, double secs, int bw, int bh, int cap, int render) {
    (void)render;
    int m_bgra = !strcmp(mode, "bgra"), m_nv12 = !strcmp(mode, "nv12"), m_dma = !strcmp(mode, "dmabuf"), m_cache = !strcmp(mode, "cache");
    if (!m_bgra && !m_nv12 && !m_dma && !m_cache) return 2;
    pid_t pid = 0;
    int s = spawn(&pid);
    if (s < 0) return 1;
    int in = open(file, O_RDONLY | O_CLOEXEC);
    if (in < 0) { perror(file); return 1; }
    int fmt = m_bgra ? MF_BGRA : m_nv12 ? MF_NV12 : MF_DMABUF;
    char params[256];
    snprintf(params, sizeof params, "native=0\nloop=1\nautoplay=1\naudio=0\nhw=1\nhwonly=1\nrate=100\nhwaccel=vaapi\nnv12=%d\nfpscap=%d\n", fmt, cap);
    /* the decoder's node: the GPU's own, or MOTION_BENCH_VA (another card's, as lord-server's media_render_fds may pick) */
    const char *va = getenv("MOTION_BENCH_VA");
    int rfd = open(va ? va : render_path, O_RDWR | O_CLOEXEC);      /* a file of its own, as media_render_fd() opens */
    int fds[3] = { in, -1, rfd };
    int64_t v[6] = { MK_PLAY };
    mp_send(s, MM_JOB, 0, v, params, strlen(params), fds, 3);
    close(in);
    close(rfd);
    mp_sendv(s, MM_SIZE, 0, bw, bh, 0, 0);
    /* bgra: each display's canvas, uploaded whole as the UI texture's damage */
    uint32_t *canvas[4] = { 0 };
    Canvas cv[4];
    GpuTex *ui[4] = { 0 };
    if (m_bgra) for (int i = 0; i < views && i < 4; i++) { canvas[i] = calloc((size_t)bw * bh, 4); canvas_init(&cv[i], canvas[i], bw, bh, bw); }
    uint8_t *ring = NULL;
    size_t ring_len = 0;
    MpRing L = { 0 };
    int gen = -1;
    GpuYuv *yuv = NULL;
    GpuTex *vtex = NULL;
    MLoopRec rec;
    memset(&rec, 0, sizeof rec);
    GpuPack **cache = calloc(MLOOP_MAX_FRAMES, sizeof *cache);
    int ncache = 0, playing_cache = 0, shown = -1, cw = 0, ch = 0, cmat = 0, cfull = 0;
    int64_t period = 0, bytes = 0, dur = 0;
    double t0c = 0, mcpu_at = 0, scpu_at = 0, t_at = 0, measure_from = 0;
    long frames = 0, mframes = 0;
    double last = 0, gap = 0;
    double start = now_ms();
    int helper_playing = 1, refused = 0;
    for (;;) {
        double now = now_ms();
        if (now - start > secs * 1000) break;
        /* measure from 2 s in (the decoder warmed up); for the cache from when it plays */
        if (!measure_from && now - start > 2000 && (!m_cache || playing_cache)) {
            measure_from = now; mcpu_at = proc_cpu_ms(pid); scpu_at = self_cpu_ms(); t_at = now; mframes = 0; gap = 0;
        }
        int timeout = 20;
        if (playing_cache) {
            int64_t t = (int64_t)(now - t0c), next = -1;
            int k = mloop_pick(rec.pts, ncache, period, t, cap, shown, &next);
            if (k >= 0 && k != shown) {
                if (gpu_pack_convert(cache[k], &vtex, cmat, cfull)) { printf("cache draw failed\n"); break; }
                shown = k;
                frames++; mframes++;
                if (last && measure_from && now - last > gap) gap = now - last;
                last = now;
            }
            int64_t turn = t / period * period;
            double due = t0c + (double)(turn + (next > t % period ? next : period)) - now;
            timeout = due < 1 ? 1 : (int)due;
        }
        struct pollfd p = { s, POLLIN, 0 };
        if (poll(&p, 1, timeout) <= 0) continue;
        MediaMsg m;
        int rf[MP_MAX_FDS];
        int r = mp_recv(s, &m, payload, sizeof payload, rf, 0);
        if (r == 0) { printf("lord-media ended\n"); break; }
        if (r < 0) continue;
        int frame_w = 0, frame_h = 0, matrix = 0, full = 0;
        int64_t pts = 0;
        GpuYuv *src = NULL, *imported = NULL;
        if (m.type == MM_ERROR) printf("error: %s\n", payload);
        else if (m.type == MM_POS) dur = m.v[1];
        else if (m.type == MM_RING && rf[0] >= 0) {
            if (ring) munmap(ring, ring_len);
            ring = NULL;
            if (!mp_ring_layout(m.v[4], m.v[1], m.v[2], m.v[3], m.v[5], &L)) {
                ring_len = L.slot_bytes * (size_t)L.slots;
                ring = mmap(NULL, ring_len, PROT_READ, MAP_SHARED, rf[0], 0);
                if (ring == MAP_FAILED) ring = NULL;
                gen = (int)m.v[0];
                printf("ring: %s %dx%d\n", L.fmt == MF_NV12 ? "NV12" : "BGRA", L.w, L.h);
            }
        } else if (m.type == MM_FRAME && ring && (int)(m.id >> 8) == gen) {
            int slot = (int)(m.id & 0xff);
            mp_sendv(s, MM_ACK, m.id, 0, 0, 0, 0);
            const uint8_t *base = ring + (size_t)slot * L.slot_bytes;
            pts = m.v[0];
            frame_w = L.w; frame_h = L.h;
            if (L.fmt == MF_BGRA) {
                for (int i = 0; i < views && i < 4; i++) {
                    canvas_blit(&cv[i], (const uint32_t *)base, L.w, L.h, (bw - L.w) / 2, (bh - L.h) / 2, 0, 1, 1);
                    ui[i] = gpu_tex_pixels(ui[i], canvas[i], bw, bh, 1);
                }
            } else {
                yuv = gpu_yuv_upload(yuv, base + L.y_off, L.y_stride, base + L.uv_off, L.uv_stride, L.w, L.h);
                if (!yuv || gpu_yuv_convert(yuv, &vtex, L.matrix, L.full)) { printf("NV12 draw failed\n"); break; }
                src = yuv; matrix = L.matrix; full = L.full;
            }
            frames++; mframes++;
        } else if (m.type == MM_DMAFRAME) {
            mp_sendv(s, MM_ACK, m.id, 0, 0, 0, 0);
            int nfds = 0;
            for (int i = 0; i < MP_MAX_FDS; i++) if (rf[i] >= 0) nfds = i + 1;
            int64_t size[2] = { 0, 0 };
            for (int i = 0; i < nfds && i < 2; i++) { off_t e = lseek(rf[i], 0, SEEK_END); size[i] = e > 0 ? e : 0; }
            if (mp_dma_check(payload, m.len, nfds, size, 2 * bw + 16, 2 * bh + 16, plane_ok)) { if (!refused++) printf("zero-copy frame refused\n"); }
            else {
                MpDma d;
                memcpy(&d, payload, sizeof d);
                GpuDmabuf py = { .width = (int)d.w, .height = (int)d.h, .nplanes = 1, .fourcc = d.plane[0].fourcc, .modifier = d.modifier[d.plane[0].obj],
                                 .fd = { rf[d.plane[0].obj], -1, -1, -1 }, .offset = { d.plane[0].offset }, .stride = { d.plane[0].pitch } };
                GpuDmabuf puv = { .width = (int)d.w / 2, .height = (int)d.h / 2, .nplanes = 1, .fourcc = d.plane[1].fourcc, .modifier = d.modifier[d.plane[1].obj],
                                  .fd = { rf[d.plane[1].obj], -1, -1, -1 }, .offset = { d.plane[1].offset }, .stride = { d.plane[1].pitch } };
                imported = gpu_yuv_import(&py, &puv);
                int tw = (int)d.w, th = (int)d.h;
                if (tw > bw || th > bh) { double k = fmin((double)bw / tw, (double)bh / th); tw = (int)lround(tw * k) & ~1; th = (int)lround(th * k) & ~1; }
                if (!imported || gpu_yuv_convert_to(imported, &vtex, tw, th, (int)(d.colors & 0xff), (d.colors & MC_FULL) != 0)) { if (!refused++) printf("zero-copy import failed\n"); }
                else { src = imported; frame_w = (int)d.w; frame_h = (int)d.h; matrix = (int)(d.colors & 0xff); full = (d.colors & MC_FULL) != 0; pts = m.v[0]; frames++; mframes++; }
            }
        }
        for (int i = 0; i < MP_MAX_FDS; i++) if (rf[i] >= 0) close(rf[i]);
        if (src) {
            double t = now_ms();
            if (last && measure_from && t - last > gap) gap = t - last;
            last = t;
        }
        /* the cache: one whole pass packed, then the loop from it */
        if (m_cache && src && !playing_cache) {
            int startok = dur > 0 && dur <= MLOOP_MAX_MS && rec.interval > 0;
            int rr = mloop_rec_frame(&rec, pts, frame_w, frame_h, startok);
            if (rr == MLR_KEEP) {
                if (rec.n == 1) { cw = frame_w; ch = frame_h; cmat = matrix; cfull = full; ncache = 0; }
                GpuPack *k = gpu_yuv_pack(src);
                if (!k || rec.n - 1 != ncache) { printf("pack failed\n"); break; }
                cache[ncache++] = k;
                bytes += gpu_pack_bytes(k);
            } else if (rr == MLR_DONE && mloop_rec_whole(&rec, dur, &period)) {
                playing_cache = 1;
                t0c = now_ms() - (double)pts;
                mp_sendv(s, MM_PAUSE, 0, 0, 0, 0, 0);
                helper_playing = 0;
                printf("cache: %d frames %dx%d, %.1f s, %lld MB; lord-media paused\n", ncache, cw, ch, period / 1000.0, (long long)(bytes >> 20));
            } else if (rr == MLR_DONE || rr == MLR_ABORT) {
                for (int i = 0; i < ncache; i++) gpu_pack_free(cache[i]);
                ncache = 0; bytes = 0;
            }
        }
        gpu_yuv_free(imported);
    }
    gpu_offscreen_finish();
    double el = (now_ms() - t_at) / 1000;
    if (!measure_from || el <= 0) { printf("nothing measured (no frames?)\n"); kill(pid, SIGKILL); waitpid(pid, NULL, 0); return 1; }
    double mc = (proc_cpu_ms(pid) - mcpu_at) / 10 / el, sc = (self_cpu_ms() - scpu_at) / 10 / el;
    printf("RESULT mode=%s views=%d box=%dx%d cap=%d: %.1f frames/s, largest gap %.0f ms, lord-media %.1f %%, lord-server's work %.1f %%, cache %lld MB%s\n",
           mode, views, bw, bh, cap, mframes / el, gap, mc, sc, (long long)(bytes >> 20), helper_playing ? "" : " (lord-media paused)");
    mp_send(s, MM_STOP, 0, NULL, NULL, 0, NULL, 0);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: motion-bench check RENDER | run RENDER FILE MODE VIEWS SECONDS [W H] [CAP]\n"); return 2; }
    int render = open(argv[2], O_RDWR | O_CLOEXEC);
    render_path = argv[2];
    if (render < 0 || gpu_init_offscreen(render)) { printf("skipped: needs a GPU render node with EGL (%s)\n", argv[2]); return 77; }
    if (!strcmp(argv[1], "check")) return check();
    if (!strcmp(argv[1], "run") && argc >= 7)
        return run(argv[3], argv[4], atoi(argv[5]), atof(argv[6]), argc > 8 ? atoi(argv[7]) : 1920, argc > 8 ? atoi(argv[8]) : 1080,
                   argc > 9 ? atoi(argv[9]) : 0, render);
    return 2;
}
