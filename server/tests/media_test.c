/* media_test.c — test of lord-media without lord-server: launches it the same way the server does
 * (socket on fd 3, no arguments), passes it the files as fds and measures.
 *
 *   build/media-test probe   file
 *   build/media-test thumb   file out.png [side]
 *   build/media-test image   file
 *   build/media-test wave    file [bars]
 *   build/media-test export  file out (opus|mp3|wav) [from_ms to_ms]
 *   build/media-test edit    file out.png "rotate=90\nbrightness=20"
 *   build/media-test play    file [seconds] [width height] [hw 0|1] [audio 0|1]
 *        plays the way the Player would (with PipeWire if present) and counts frames
 *   build/media-test sandbox file
 *        the helper tries to open ~/.ssh and create a socket: it must fail
 *   build/media-test svg     file side (rrggbb|-) (ok|fail)
 *        an SVG icon (MW_SVG): it must be drawn (ok: side x side at most, painted pixels, all of that color when
 *        one is given) or refused with an error code (fail), and the helper must answer either way
 */
#include "../src/media/proto.h"
#include "../src/media/media_int.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char payload[MP_PAYLOAD_MAX + 1];
static const char *helper = "build/lord-media";

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int spawn(pid_t *pid) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    *pid = fork();
    if (*pid == 0) {
        dup2(sv[1], 3);
        char *const argv[] = { "lord-media", NULL };
        char *env[] = { "LANG=C.UTF-8", getenv("LORD_MEDIA_DEBUG") ? "LORD_MEDIA_DEBUG=1" : NULL, NULL };
        execve(helper, argv, env);
        _exit(127);
    }
    close(sv[1]);
    return sv[0];
}

static int wait_msg(int s, MediaMsg *m, int fds[MP_MAX_FDS], int ms) {
    struct pollfd p = { s, POLLIN, 0 };
    if (poll(&p, 1, ms) <= 0) return -1;
    return mp_recv(s, m, payload, sizeof payload, fds, 0);
}

static int open_in(const char *p) { int fd = open(p, O_RDONLY | O_CLOEXEC); if (fd < 0) perror(p); return fd; }
static int open_out(const char *p) { int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); if (fd < 0) perror(p); return fd; }

int media_pulse_fd(void);
int media_pulse_fd(void) {
    char path[256];
    const char *rt = getenv("XDG_RUNTIME_DIR");
    snprintf(path, sizeof path, "%s/pulse/native", rt ? rt : "/run/user/1000");
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%.100s", path);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    return fd;
}

/* One worker job: returns the status. */
static int job(int kind, const char *in, const char *out, int64_t a, int64_t b, const char *params) {
    pid_t pid;
    int s = spawn(&pid);
    int fds[2] = { open_in(in), out ? open_out(out) : -1 };
    if (fds[0] < 0) return 1;
    int64_t hello[6] = { MK_WORKER };
    mp_send(s, MM_JOB, 0, hello, NULL, 0, NULL, 0);
    int64_t v[6] = { kind, a, b };
    double t0 = now_ms();
    mp_send(s, MM_JOB, 7, v, params, params ? strlen(params) : 0, fds, out ? 2 : 1);
    MediaMsg m;
    int rf[MP_MAX_FDS];
    int r = wait_msg(s, &m, rf, 60000);
    double dt = now_ms() - t0;
    if (r != 1 || m.type != MM_RESULT) { printf("no reply (did the helper crash?)\n"); return 1; }
    printf("result %lld in %.1f ms: %s\n", (long long)m.v[0], dt, payload);
    if (rf[0] >= 0) {
        struct stat st;
        fstat(rf[0], &st);
        int seals = fcntl(rf[0], F_GET_SEALS);
        printf("memfd of %lld bytes, seals %s\n", (long long)st.st_size, (seals & F_SEAL_SHRINK) ? "no-shrink (good)" : "MISSING");
    }
    mp_send(s, MM_STOP, 0, NULL, NULL, 0, NULL, 0);
    waitpid(pid, NULL, 0);
    return m.v[0] == 0 ? 0 : 2;
}

static int play(const char *in, double secs, int w, int h, int hw, int audio) {
    pid_t pid;
    int s = spawn(&pid);
    int render = hw ? open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC) : -1;
    /* this process owns the audio, like lord-server (maudio.c): lord-media only fills the ring */
    int aring = -1;
    MAudio *au = audio ? maudio_open(0, "media-test", "media-test", "video", &aring) : NULL;
    int fds[3] = { open_in(in), aring, render };
    char params[128];
    snprintf(params, sizeof params, "hw=%d\naudio=%d\ntitle=media-test\n", hw, audio);
    int64_t v[6] = { MK_PLAY };
    mp_send(s, MM_JOB, 0, v, params, strlen(params), fds, 3);
    for (int i = 0; i < 3; i++) if (fds[i] >= 0 && i != 1) close(fds[i]);
    mp_sendv(s, MM_SIZE, 0, w, h, 0, 0);
    uint8_t *ring = NULL;
    size_t ring_bytes = 0;
    int gen = 0;
    long frames = 0, late = 0;
    double t0 = now_ms(), first = 0, last_frame = 0, max_gap = 0;
    int64_t last_pts = -1, pos = 0;
    char stat_path[64];
    snprintf(stat_path, sizeof stat_path, "/proc/%d/stat", (int)pid);
    long long cpu0 = 0;
    FILE *f = fopen(stat_path, "r");
    if (f) { long long ut, st; if (fscanf(f, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lld %lld", &ut, &st) == 2) cpu0 = ut + st; fclose(f); }
    int64_t stats[6] = { 0 };
    while (now_ms() - t0 < secs * 1000) {
        MediaMsg m;
        int rf[MP_MAX_FDS];
        struct pollfd pp[2] = { { s, POLLIN, 0 }, { -1, 0, 0 } };
        if (au) pp[1].fd = maudio_pollfd(au, &pp[1].events);
        poll(pp, 2, 100);
        if (pp[1].revents) maudio_service(au);
        if (!(pp[0].revents & (POLLIN | POLLHUP))) continue;
        int r = mp_recv(s, &m, payload, sizeof payload, rf, 0);
        if (r == 0) { printf("the helper closed\n"); break; }
        if (r < 0) continue;
        if (m.type == MM_INFO) printf("info: %.300s\n", payload);
        else if (m.type == MM_ERROR) printf("error: %s\n", payload);
        else if (m.type == MM_RING && rf[0] >= 0) {
            if (ring) munmap(ring, ring_bytes);
            ring_bytes = (size_t)m.v[1] * m.v[2] * 4 * (size_t)m.v[3];
            ring = mmap(NULL, ring_bytes, PROT_READ, MAP_SHARED, rf[0], 0);
            gen = (int)m.v[0];
            printf("ranuras %lldx%lld (gen %d)\n", (long long)m.v[1], (long long)m.v[2], gen);
            close(rf[0]);
        } else if (m.type == MM_FRAME) {
            double t = now_ms();
            if (!first) first = t;
            if (last_frame && t - last_frame > max_gap && frames > 10) max_gap = t - last_frame;
            last_frame = t;
            if (last_pts >= 0 && m.v[0] < last_pts) late++;
            last_pts = m.v[0];
            frames++;
            mp_sendv(s, MM_ACK, m.id, 0, 0, 0, 0);
        } else if (m.type == MM_POS) pos = m.v[0];
        else if (m.type == MM_STATS) memcpy(stats, m.v, sizeof stats);
        else if (m.type == MM_AUDIO) maudio_service(au);
        else if (m.type == MM_STATE) { maudio_playing(au, m.v[0] == MS_PLAYING); if (m.v[0] == MS_ENDED) { printf("ended\n"); break; } }
        for (int i = 0; i < MP_MAX_FDS; i++) if (rf[i] >= 0 && m.type != MM_RING) close(rf[i]);
    }
    long long cpu1 = 0;
    f = fopen(stat_path, "r");
    if (f) { long long ut, st; if (fscanf(f, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lld %lld", &ut, &st) == 2) cpu1 = ut + st; fclose(f); }
    double el = (now_ms() - t0) / 1000;
    double hz = (double)sysconf(_SC_CLK_TCK);
    printf("frames received %ld in %.1f s (%.1f/s since the first), largest gap %.0f ms, position %lld ms\n", frames, el,
           frames / ((now_ms() - first) / 1000), max_gap, (long long)pos);
    printf("helper: decoded %lld, shown %lld, dropped %lld, hw %lld, mean offset %.2f ms, max %lld ms\n",
           (long long)stats[0], (long long)stats[1], (long long)stats[2], (long long)stats[3], stats[4] / 100.0, (long long)stats[5]);
    printf("helper CPU: %.0f %% of one core\n", 100.0 * (cpu1 - cpu0) / hz / el);
    mp_send(s, MM_STOP, 0, NULL, NULL, 0, NULL, 0);
    waitpid(pid, NULL, 0);
    maudio_close(au);
    (void)late;
    return 0;
}

/* An SVG icon through the helper, as media/svgicon.c asks for it: the file opened here, never a path. */
static int svg(const char *in, int side, const char *color, const char *want) {
    pid_t pid;
    int s = spawn(&pid);
    int fd = open_in(in);
    if (fd < 0) return 1;
    long rgb = strcmp(color, "-") ? strtol(color, NULL, 16) : -1;
    int64_t hello[6] = { MK_WORKER };
    mp_send(s, MM_JOB, 0, hello, NULL, 0, NULL, 0);
    int64_t v[6] = { MW_SVG, side, rgb };
    double t0 = now_ms();
    mp_send(s, MM_JOB, 9, v, NULL, 0, &fd, 1);
    MediaMsg m;
    int rf[MP_MAX_FDS];
    int r = wait_msg(s, &m, rf, 10000);
    double dt = now_ms() - t0;
    mp_send(s, MM_STOP, 0, NULL, NULL, 0, NULL, 0);
    waitpid(pid, NULL, 0);
    if (r != 1 || m.type != MM_RESULT) { printf("no reply (did the helper crash?)\n"); return 1; }
    printf("result %lld in %.1f ms: %s\n", (long long)m.v[0], dt, payload);
    if (!strcmp(want, "fail")) return m.v[0] < 0 && rf[0] < 0 ? 0 : 1;
    int w = 0, h = 0;
    sscanf(payload, "{\"width\":%d,\"height\":%d", &w, &h);
    if (m.v[0] != 0 || rf[0] < 0 || w < 1 || h < 1 || w > side || h > side) { printf("not drawn\n"); return 1; }
    int seals = fcntl(rf[0], F_GET_SEALS);
    if (!(seals & F_SEAL_SHRINK)) { printf("memfd not sealed\n"); return 1; }
    const uint8_t *px = mmap(NULL, (size_t)w * h * 4, PROT_READ, MAP_SHARED, rf[0], 0);
    if (px == MAP_FAILED) { printf("memfd too small\n"); return 1; }
    int painted = 0, other = 0;
    for (int i = 0; i < w * h; i++) {
        const uint8_t *p = px + i * 4;
        if (p[3] < 64) continue;
        painted++;
        double a = p[3] / 255.0;
        if (rgb >= 0 && (fabs(p[2] / a - ((rgb >> 16) & 255)) > 3 || fabs(p[1] / a - ((rgb >> 8) & 255)) > 3 || fabs(p[0] / a - (rgb & 255)) > 3)) other++;
    }
    munmap((void *)px, (size_t)w * h * 4);
    printf("%dx%d, %d painted, %d of another color\n", w, h, painted, other);
    return painted > 0 && !other ? 0 : 1;
}

int main(int argc, char **argv) {
    if (getenv("LORD_MEDIA")) helper = getenv("LORD_MEDIA");
    if (argc < 3) { fprintf(stderr, "usage: see the comment in tests/media_test.c\n"); return 2; }
    const char *cmd = argv[1], *in = argv[2];
    if (!strcmp(cmd, "sandbox")) return job(MW_SELFTEST, in, NULL, 0, 0, NULL);
    if (!strcmp(cmd, "probe")) return job(MW_PROBE, in, NULL, 0, 0, NULL);
    if (!strcmp(cmd, "thumb") && argc >= 4) return job(MW_THUMB, in, argv[3], argc > 4 ? atoi(argv[4]) : 256, 0, NULL);
    if (!strcmp(cmd, "image")) return job(MW_IMAGE, in, NULL, 0, 0, NULL);
    if (!strcmp(cmd, "svg") && argc >= 6) return svg(in, atoi(argv[3]), argv[4], argv[5]);
    if (!strcmp(cmd, "wave")) return job(MW_WAVEFORM, in, NULL, argc > 3 ? atoi(argv[3]) : 100, 0, NULL);
    if (!strcmp(cmd, "edit") && argc >= 5) {
        char p[512]; size_t o = 0;
        for (const char *c = argv[4]; *c && o + 1 < sizeof p; c++) { if (c[0] == '\\' && c[1] == 'n') { p[o++] = '\n'; c++; } else p[o++] = *c; }
        p[o] = 0;
        return job(MW_EDIT, in, argv[3], 0, 0, p);
    }
    if (!strcmp(cmd, "export") && argc >= 5) {
        char p[128];
        snprintf(p, sizeof p, "format=%s\nfrom=%s\nto=%s\n", argv[4], argc > 5 ? argv[5] : "0", argc > 6 ? argv[6] : "-1");
        return job(MW_TRANSCODE, in, argv[3], 0, 0, p);
    }
    if (!strcmp(cmd, "play"))
        return play(in, argc > 3 ? atof(argv[3]) : 10, argc > 5 ? atoi(argv[4]) : 960, argc > 5 ? atoi(argv[5]) : 540,
                    argc > 6 ? atoi(argv[6]) : 1, argc > 7 ? atoi(argv[7]) : 1);
    fprintf(stderr, "orden desconocida\n");
    return 2;
}
