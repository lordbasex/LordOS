/* lmstream_test.c — the remote desktop's live encoder end to end (lord-media MK_STREAM, #94): frames go in
 * through the sealed ring as lord-server sends them, and the H.264 that comes back must start with a keyframe
 * a browser's decoder shows as that frame (lm_encheck.h). Run as
 *   build/lmstream-test build/lord-media [/dev/dri/renderDN ...]
 * with render nodes (up to three, in lord-server's order: the compositor's first), lord-media tries VA-API on
 * each until one encodes H.264; build/lord-media-encfail (every hardware keyframe fails the check) shows the
 * way out: the stream still starts, from libx264. Then, whichever encoder runs:
 *  - one frame in, its packet out: a lone frame (the end of a change) is not held until the next one
 *    (VA-API's async_depth=1; FFmpeg's default 2 kept one frame back);
 *  - the stream is tagged BT.709, limited range;
 *  - a higher MM_RATE gives a higher bit rate (libx264 reconfigures; VA-API opens again, from a keyframe).
 * LMSTREAM_SIZE=1920x1080 measures at another size (the numbers in CHANGELOG.md). */
#include "../src/media/proto.h"
#include "../src/media/helper/lm_encheck.h"

#include <errno.h>
#include <fcntl.h>
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

#include <libavcodec/avcodec.h>
#include <libavutil/log.h>

enum { SLOTS = 3, REFS = 4, STEPS = 12 };
static int W = 640, H = 368;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* grey levels only, so the luma that goes in is known: Y = 16 + v * 219 / 255 */
static int level(int i, int j, int seed) {
    int v = j < H / 8 ? 40 : i < W / 4 ? 200 : 16 + (i * 200 / W + seed * 37) % 220;
    if ((j / 6) % 4 == 1 && (i / 3 + seed) % 5 < 2 && i > W / 3) v = 20;
    return v;
}

static int sock = -1, busy[SLOTS], state = -1, hw = -1, hw_now = -1;   /* hw_now: MM_STATS, every 2 s */
static uint32_t *ring;
static uint8_t *pkt;
static size_t plen, pcap = 8u << 20;
static char payload[MP_PAYLOAD_MAX + 1];

/* the packets that came: count, bytes, keyframes, and the last one's pts and arrival */
static int packets, keys, first_key = -1, first_ok = -1, first_frame = -1;
static long bytes;
static int64_t last_pts = -1;
static double last_at;
static LmEncCheck chk[REFS];
static AVCodecContext *dec;
static int tag_range = -1, tag_space = -1, tag_prim = -1;

static void on_packet(int key) {
    if (packets == 0) {
        first_key = key;
        const char *why = "";
        first_ok = 0;
        for (int k = 0; k < REFS && !first_ok; k++) if (!lm_encheck_packet(&chk[k], pkt, plen, &why)) { first_ok = 1; first_frame = k; }
        if (!first_ok) printf("lmstream: first packet: %s\n", why);
        /* the colour tags, as a browser's decoder reads them from the SPS */
        const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_H264);
        dec = c ? avcodec_alloc_context3(c) : NULL;
        AVPacket *p = av_packet_alloc();
        AVFrame *f = av_frame_alloc();
        if (dec && p && f && avcodec_open2(dec, c, NULL) >= 0 && !av_new_packet(p, (int)plen)) {
            memcpy(p->data, pkt, plen);
            if (avcodec_send_packet(dec, p) >= 0 && avcodec_send_packet(dec, NULL) >= 0 && avcodec_receive_frame(dec, f) >= 0) {
                tag_range = f->color_range;
                tag_space = f->colorspace;
                tag_prim = f->color_primaries;
            }
        }
        av_packet_free(&p);
        av_frame_free(&f);
        avcodec_free_context(&dec);
    }
    packets++;
    keys += key;
    bytes += (long)plen;
}

/* What lord-media says, for up to ms milliseconds (0: what is there now). -1 if the helper went away. */
static int pump(double ms) {
    double until = now_ms() + ms;
    for (;;) {
        double left = until - now_ms();
        struct pollfd pf = { sock, POLLIN, 0 };
        int pr = poll(&pf, 1, left > 0 ? (int)left + 1 : 0);
        if (pr <= 0) return 0;
        MediaMsg m;
        int fds[MP_MAX_FDS], r;
        while ((r = mp_recv(sock, &m, payload, sizeof payload, fds, 0)) == 1) {
            for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]);
            if (m.type == MM_SLOTFREE && m.id < SLOTS) busy[m.id] = 0;
            else if (m.type == MM_STATE) { state = (int)m.v[0]; if (state == MS_RECORDING && hw < 0) hw_now = hw = (int)m.v[1]; }
            else if (m.type == MM_STATS) hw_now = (int)m.v[2];
            else if (m.type == MM_PACKET && m.id == 0) {
                if (m.v[1] == 0) plen = 0;
                if (plen + m.len > pcap || (size_t)m.v[1] != plen) { CHECK(!"chunks out of order"); plen = 0; continue; }
                memcpy(pkt + plen, payload, m.len);
                plen += m.len;
                if ((int64_t)plen == m.v[0]) {
                    last_pts = m.v[2];
                    last_at = now_ms();
                    on_packet(m.v[3] != 0);
                    plen = 0;
                    return 1;                       /* one packet at a time: the caller looks at each */
                }
            }
        }
        if (r == 0 || state == MS_FAILED) return -1;
        if (now_ms() >= until) return 0;
    }
}

/* kind 0: the grey test picture (seed varies it a little); 1: a gradient under faint noise: a few bits make it
 * passable, more bits keep more of the noise (so the bit rate follows what the encoder is allowed) */
static uint32_t rnd = 2463534242u;
static int send_frame(int kind, int seed, int64_t pts) {
    int s = -1;
    for (double t0 = now_ms(); s < 0 && now_ms() - t0 < 2000;) {
        for (int i = 0; i < SLOTS && s < 0; i++) if (!busy[i]) s = i;
        if (s < 0 && pump(5) < 0) return -1;
    }
    if (s < 0) return -1;
    uint32_t *px = ring + (size_t)s * W * H;
    for (int j = 0; j < H; j++)
        for (int i = 0; i < W; i++) {
            uint32_t g;
            if (kind) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; g = (uint32_t)(i * 160 / W + j * 64 / H); px[j * W + i] = 0xff000000u | ((g << 16 | g << 8 | g) + (rnd & 0x1f1f1f)); continue; }
            g = (uint32_t)level(i, j, seed);
            px[j * W + i] = 0xff000000u | g << 16 | g << 8 | g;
        }
    busy[s] = 1;
    return mp_sendv(sock, MM_INFRAME, (uint32_t)s, pts, 0, 0, 0);
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: lmstream-test lord-media [render node ...]\n"); return 2; }
    av_log_set_level(AV_LOG_QUIET);
    const char *sz = getenv("LMSTREAM_SIZE");
    if (sz && sscanf(sz, "%dx%d", &W, &H) == 2) { W &= ~1; H &= ~1; }
    if (W < 64 || H < 64 || W > 4096 || H > 4096) { W = 640; H = 368; }
    int render[3], nrender = 0;
    for (int i = 2; i < argc && nrender < 3; i++) {
        int fd = open(argv[i], O_RDWR | O_CLOEXEC);
        if (fd >= 0) render[nrender++] = fd;
    }
    pkt = malloc(pcap);
    int sv[2];
    if (!pkt || socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return 1;
    pid_t p = fork();
    if (p == 0) {
        if (dup2(sv[1], 3) < 0) _exit(127);
        char *const av[] = { "lord-media", NULL };
        char *const env[] = { "LANG=C.UTF-8", NULL };
        execve(argv[1], av, env);
        _exit(127);
    }
    close(sv[1]);
    sock = sv[0];
    char params[256];
    snprintf(params, sizeof params, "codec=h264\nwidth=%d\nheight=%d\nfps=30\nkbps=4000\nhw=%d\nhwaccel=vaapi\naudio=0\nrender=%d\n",
             W, H, nrender > 0, nrender);
    int64_t v[6] = { MK_STREAM, 0, 0, 0, 0, 0 };
    CHECK(!mp_send(sock, MM_JOB, 0, v, params, strlen(params), render, nrender));
    for (int i = 0; i < nrender; i++) close(render[i]);
    size_t n = (size_t)W * H * 4 * SLOTS;
    int fd = memfd_create("lmstream-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(fd >= 0 && !ftruncate(fd, (off_t)n) && !fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL));
    ring = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(ring != MAP_FAILED);
    if (ring == MAP_FAILED) return 1;
    int64_t rv[6] = { SLOTS, W, H, 0, 0, 0 };
    CHECK(!mp_send(sock, MM_INRING, 0, rv, NULL, 0, &fd, 1));
    close(fd);

    /* the references of the first frames, as the encoder's input sees them: the first packet is frame 0, or
     * after a switch to libx264 the frame that was going in then (the hardware may hold one or two) */
    uint8_t *y0 = malloc((size_t)W * H);
    if (!y0) return 1;
    for (int k = 0; k < REFS; k++) {
        for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) y0[j * W + i] = (uint8_t)(16 + level(i, j, k) * 219 / 255);
        CHECK(!lm_encheck_ref(&chk[k], y0, W, W, H));
    }
    free(y0);

    /* 1. the start: a frame every 30 ms until six packets came */
    int sent = 0, dead = 0;
    double t0 = now_ms();
    while (now_ms() - t0 < 15000 && packets < 6 && sent < 40 && !dead) {
        if (send_frame(0, sent, sent * 30) < 0) { dead = 1; break; }
        sent++;
        for (double t1 = now_ms(); now_ms() - t1 < 30;) if (pump(30 - (now_ms() - t1)) < 0) { dead = 1; break; }
    }
    pump(300);                                       /* what the encoder still had */
    int start_packets = packets, start_sent = sent;

    /* 2. lone frames, one at a time: each one's packet must come before the next frame goes in */
    double lat[STEPS];
    int in_step = 0;
    int64_t pts = sent * 30 + 1000;
    for (int k = 0; k < STEPS && !dead; k++, pts += 200) {
        if (send_frame(0, 100 + k, pts) < 0) { dead = 1; break; }
        sent++;
        double ts = now_ms();
        lat[k] = 500;
        while (now_ms() - ts < 500) {
            int r = pump(500 - (now_ms() - ts));
            if (r < 0) { dead = 1; break; }
            if (r == 1 && last_pts == pts) { lat[k] = last_at - ts; in_step++; break; }
        }
    }
    qsort(lat, STEPS, sizeof lat[0], cmp_d);

    /* 3. the rate: noise for 2 s at the starting 4000 kbit/s, then 12000 asked for (as the bridge does on a clean
     * link): a little later the encoder must make more bits (VA-API reopens with a keyframe first) */
    long b0 = 0, b1 = 0;
    double r0_ms = 0, r1_ms = 0, reopen_ms = -1;
    if (!dead) {
        pump(200);
        long b = bytes;
        double ts = now_ms();
        for (; now_ms() - ts < 2000 && !dead; pts += 33, sent++) {
            if (send_frame(1, 0, pts) < 0) { dead = 1; break; }
            for (double t1 = now_ms(); now_ms() - t1 < 33;) if (pump(33 - (now_ms() - t1)) < 0) { dead = 1; break; }
        }
        b0 = bytes - b;
        r0_ms = now_ms() - ts;
        mp_sendv(sock, MM_RATE, 0, 12000, 0, 0, 0);
        /* VA-API: wait for the keyframe of the new encoder (up to 4 s: it reopens at most every 3 s) */
        int k0 = keys;
        double tr = now_ms();
        for (; hw_now > 0 && keys == k0 && now_ms() - tr < 4000 && !dead; pts += 33, sent++) {
            if (send_frame(1, 0, pts) < 0) { dead = 1; break; }
            for (double t1 = now_ms(); now_ms() - t1 < 33;) if (pump(33 - (now_ms() - t1)) < 0) { dead = 1; break; }
        }
        if (hw_now > 0 && keys > k0) reopen_ms = now_ms() - tr;
        b = bytes;
        ts = now_ms();
        for (; now_ms() - ts < 2000 && !dead; pts += 33, sent++) {
            if (send_frame(1, 0, pts) < 0) { dead = 1; break; }
            for (double t1 = now_ms(); now_ms() - t1 < 33;) if (pump(33 - (now_ms() - t1)) < 0) { dead = 1; break; }
        }
        b1 = bytes - b;
        r1_ms = now_ms() - ts;
    }

    mp_sendv(sock, MM_STOP, 0, 0, 0, 0, 0);
    close(sock);
    int st = 0;
    waitpid(p, &st, 0);
    struct rusage ru;
    getrusage(RUSAGE_CHILDREN, &ru);
    double cpu = ru.ru_utime.tv_sec * 1e3 + ru.ru_utime.tv_usec / 1e3 + ru.ru_stime.tv_sec * 1e3 + ru.ru_stime.tv_usec / 1e3;
    double k0 = r0_ms > 0 ? b0 * 8.0 / r0_ms : 0, k1 = r1_ms > 0 ? b1 * 8.0 / r1_ms : 0;
    printf("lmstream: %s, %dx%d, %s at the start, %d packets from %d frames, the first one shows frame %d\n", argv[1], W, H,
           hw > 0 ? (hw_now > 0 ? "VA-API" : "VA-API then libx264") : "software", start_packets, start_sent, first_frame);
    printf("lmstream: lone frames: %d of %d out before the next one went in, median %.1f ms, max %.1f ms\n", in_step, STEPS,
           lat[STEPS / 2], lat[STEPS - 1]);
    printf("lmstream: rate: %.0f kbit/s asked 4000, %.0f kbit/s asked 12000%s; colour range %d space %d primaries %d\n", k0, k1,
           reopen_ms >= 0 ? " (VA-API reopened)" : "", tag_range, tag_space, tag_prim);
    printf("lmstream: lord-media CPU %.0f ms for %d frames (%.2f ms a frame)\n", cpu, sent, sent ? cpu / sent : 0.0);
    CHECK(!dead);
    CHECK(state != MS_FAILED);
    CHECK(start_packets >= 6);
    CHECK(first_key == 1);
    CHECK(first_ok == 1);
    CHECK(in_step == STEPS);
    CHECK(tag_range == AVCOL_RANGE_MPEG);
    CHECK(tag_space == AVCOL_SPC_BT709);
    CHECK(tag_prim == AVCOL_PRI_BT709);
    CHECK(k1 > k0 * 1.5);
    CHECK(hw_now <= 0 || reopen_ms >= 0);
    for (int k = 0; k < REFS; k++) lm_encheck_free(&chk[k]);
    if (fails) { printf("lmstream: %d failed\n", fails); return 1; }
    printf("lmstream: ok\n");
    return 0;
}
