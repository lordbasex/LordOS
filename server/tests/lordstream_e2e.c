/* lordstream_e2e.c — lord-stream end to end, as lord-server drives it (socket on fd 3, MM_JOB, MM_SIZE, MM_ACK):
 *   build/lordstream-e2e build/lord-stream play <url> [seconds]   frames shown, the stream's info, errors
 *   build/lordstream-e2e build/lord-stream m3u <url>               the channels of a list
 * Prints one summary line ("frames=N ..."), exit 0 if frames (or channels) came. A tool for docs/MEDIA.md's
 * "Network streams" checks against local test servers, never installed. */
#include "../src/media/proto.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s lord-stream play|m3u URL [seconds]\n", argv[0]); return 2; }
    int m3u = !strcmp(argv[2], "m3u");
    double secs = argc > 4 ? atof(argv[4]) : 8;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return 1;
    pid_t p = fork();
    if (p == 0) {
        dup2(sv[1], 3);
        char *const a[] = { "lord-stream", NULL };
        char *const env[] = { "LANG=C.UTF-8", getenv("LORD_MEDIA_DEBUG") ? "LORD_MEDIA_DEBUG=1" : NULL, NULL };
        execve(argv[1], a, env);
        _exit(127);
    }
    close(sv[1]);
    char params[4200];
    if (m3u) snprintf(params, sizeof params, "url=%s\n", argv[3]);
    else snprintf(params, sizeof params, "url=%s\nautoplay=1\naudio=0\nhw=0\nhwaccel=none\n", argv[3]);
    int64_t v[6] = { m3u ? MK_M3U : MK_PLAY, 0, 0, 0, 0, 0 };
    mp_send(sv[0], MM_JOB, 0, v, params, strlen(params), NULL, 0);
    if (!m3u) mp_sendv(sv[0], MM_SIZE, 0, 320, 180, 0, 0);
    static char payload[MP_PAYLOAD_MAX + 1];
    int frames = 0, channels = 0, done = 0;
    long long st_dec = 0, st_shown = 0, st_drop = 0;
    char err[200] = "", info[300] = "";
    double t0 = now_ms(), first = 0;
    while (!done && now_ms() - t0 < secs * 1000) {
        struct pollfd pf = { sv[0], POLLIN, 0 };
        if (poll(&pf, 1, 200) <= 0) continue;
        MediaMsg m;
        int fds[MP_MAX_FDS];
        int r = mp_recv(sv[0], &m, payload, sizeof payload, fds, 0);
        for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]);
        if (r == 0) break;
        if (r < 0) continue;
        if (m.type == MM_FRAME) { if (!frames++) first = now_ms() - t0; mp_sendv(sv[0], MM_ACK, m.id, 0, 0, 0, 0); }
        else if (m.type == MM_STATS) { st_dec = m.v[0]; st_shown = m.v[1]; st_drop = m.v[2]; }
        else if (m.type == MM_INFO) snprintf(info, sizeof info, "%.290s", payload);
        else if (m.type == MM_ERROR) { snprintf(err, sizeof err, "%.190s", payload); }
        else if (m.type == MM_RESULT && m3u) {
            if (m.v[0]) { snprintf(err, sizeof err, "%.190s", payload); done = 1; }
            else { for (char *q = payload; (q = strstr(q, "\"url\":")); q++) channels++; if (!m.v[1]) done = 1; }
        }
    }
    double el = now_ms() - t0;
    mp_sendv(sv[0], MM_STOP, 0, 0, 0, 0, 0);
    close(sv[0]);
    int st = 0;
    for (int i = 0; i < 30 && waitpid(p, &st, WNOHANG) != p; i++) usleep(100000);
    if (waitpid(p, &st, WNOHANG) == 0) { kill(p, SIGKILL); waitpid(p, &st, 0); }
    if (m3u) printf("channels=%d err=%s time_ms=%.0f\n", channels, err[0] ? err : "-", el);
    else printf("frames=%d first_ms=%.0f decoded=%lld shown=%lld dropped=%lld err=%s info=%.80s\n", frames, first, st_dec, st_shown, st_drop, err[0] ? err : "-", info);
    return (m3u ? channels > 0 : frames > 0) ? 0 : 1;
}
