/* lmstream_test.c — the remote desktop's live encoder end to end (lord-media MK_STREAM, #94): frames go in
 * through the sealed ring as lord-server sends them, and the H.264 that comes back must start with a keyframe
 * a browser's decoder shows as that frame (lm_encheck.h). Run as
 *   build/lmstream-test build/lord-media [/dev/dri/renderDN]
 * with a render node, lord-media tries VA-API first; build/lord-media-encfail (every hardware keyframe fails
 * the check) shows the way out: the stream still starts, from libx264. */
#include "../src/media/proto.h"
#include "../src/media/helper/lm_encheck.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <libavutil/log.h>

enum { W = 640, H = 368, SLOTS = 3 };

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

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: lmstream-test lord-media [render node]\n"); return 2; }
    av_log_set_level(AV_LOG_QUIET);
    const char *node = argc > 2 ? argv[2] : NULL;
    int render = node ? open(node, O_RDWR | O_CLOEXEC) : -1;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return 1;
    pid_t p = fork();
    if (p == 0) {
        if (dup2(sv[1], 3) < 0) _exit(127);
        char *const av[] = { "lord-media", NULL };
        char *const env[] = { "LANG=C.UTF-8", NULL };
        execve(argv[1], av, env);
        _exit(127);
    }
    close(sv[1]);
    char params[256];
    snprintf(params, sizeof params, "codec=h264\nwidth=%d\nheight=%d\nfps=30\nkbps=4000\nhw=%d\nhwaccel=vaapi\naudio=0\nrender=%d\n",
             W, H, render >= 0, render >= 0);
    int64_t v[6] = { MK_STREAM, 0, 0, 0, 0, 0 };
    CHECK(!mp_send(sv[0], MM_JOB, 0, v, params, strlen(params), render >= 0 ? &render : NULL, render >= 0 ? 1 : 0));
    size_t n = (size_t)W * H * 4 * SLOTS;
    int fd = memfd_create("lmstream-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(fd >= 0 && !ftruncate(fd, (off_t)n) && !fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL));
    uint32_t *ring = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(ring != MAP_FAILED);
    int64_t rv[6] = { SLOTS, W, H, 0, 0, 0 };
    CHECK(!mp_send(sv[0], MM_INRING, 0, rv, NULL, 0, &fd, 1));
    close(fd);

    /* the references of the first frames, as the encoder's input sees them: the first packet is frame 0, or
     * after a switch to libx264 the frame that was going in then (the hardware may hold one or two) */
    enum { REFS = 4 };
    static uint8_t y0[W * H];
    LmEncCheck chk[REFS];
    for (int k = 0; k < REFS; k++) {
        for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) y0[j * W + i] = (uint8_t)(16 + level(i, j, k) * 219 / 255);
        CHECK(!lm_encheck_ref(&chk[k], y0, W, W, H));
    }

    static char payload[MP_PAYLOAD_MAX + 1];
    static uint8_t pkt[8 << 20];
    size_t plen = 0;
    int packets = 0, first_key = -1, first_ok = -1, first_frame = -1, hw = -1, state = -1, sent = 0, busy[SLOTS] = { 0 };
    double t0 = now_ms();
    while (now_ms() - t0 < 15000 && packets < 6 && state != MS_FAILED) {
        /* one frame every 30 ms into a free slot, each a little different */
        for (int s = 0; s < SLOTS && sent < 40; s++)
            if (!busy[s]) {
                uint32_t *px = ring + (size_t)s * W * H;
                for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) { uint32_t g = (uint32_t)level(i, j, sent); px[j * W + i] = 0xff000000u | g << 16 | g << 8 | g; }
                busy[s] = 1;
                CHECK(!mp_sendv(sv[0], MM_INFRAME, (uint32_t)s, sent * 30, 0, 0, 0));
                sent++;
                break;
            }
        struct pollfd pf = { sv[0], POLLIN, 0 };
        if (poll(&pf, 1, 30) <= 0) continue;
        MediaMsg m;
        int fds[MP_MAX_FDS], r;
        while ((r = mp_recv(sv[0], &m, payload, sizeof payload, fds, 0)) == 1) {
            for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]);
            if (m.type == MM_SLOTFREE && m.id < SLOTS) busy[m.id] = 0;
            else if (m.type == MM_STATE) { state = (int)m.v[0]; if (state == MS_RECORDING && hw < 0) hw = (int)m.v[1]; }
            else if (m.type == MM_PACKET && m.id == 0) {
                if (m.v[1] == 0) plen = 0;
                if (plen + m.len > sizeof pkt || (size_t)m.v[1] != plen) { CHECK(!"chunks out of order"); break; }
                memcpy(pkt + plen, payload, m.len);
                plen += m.len;
                if ((int64_t)plen == m.v[0]) {
                    if (packets == 0) {
                        first_key = m.v[3] != 0;
                        const char *why = "";
                        first_ok = 0;
                        for (int k = 0; k < REFS && !first_ok; k++) if (!lm_encheck_packet(&chk[k], pkt, plen, &why)) { first_ok = 1; first_frame = k; }
                        if (!first_ok) printf("lmstream: first packet: %s\n", why);
                    }
                    packets++;
                    plen = 0;
                }
            }
        }
        if (r == 0) break;
    }
    mp_sendv(sv[0], MM_STOP, 0, 0, 0, 0, 0);
    close(sv[0]);
    int st = 0;
    waitpid(p, &st, 0);
    printf("lmstream: %s, %s at the start, %d packets from %d frames, the first one shows frame %d\n", argv[1],
           hw > 0 ? "VA-API" : "software", packets, sent, first_frame);
    CHECK(state != MS_FAILED);
    CHECK(packets >= 6);
    CHECK(first_key == 1);
    CHECK(first_ok == 1);
    for (int k = 0; k < REFS; k++) lm_encheck_free(&chk[k]);
    if (fails) { printf("lmstream: %d failed\n", fails); return 1; }
    printf("lmstream: ok\n");
    return 0;
}
