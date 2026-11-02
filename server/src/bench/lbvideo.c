/* lbvideo.c — see lbvideo.h. */
#include "lbvideo.h"
#include "lbproto.h"
#include "bench.h"
#include "../media/proto.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int media_path(char *out, size_t n) {
    char self[PATH_MAX];
    ssize_t k = readlink("/proc/self/exe", self, sizeof self - 1);
    if (k <= 0) return -1;
    self[k] = 0;
    char *slash = strrchr(self, '/');
    if (!slash) return -1;
    *slash = 0;
    if ((size_t)snprintf(out, n, "%s/lord-media", self) >= n) return -1;
    struct stat st;
    if (stat(out, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) return -1;
    if ((st.st_uid != 0 && st.st_uid != getuid()) || (st.st_mode & (S_IWGRP | S_IWOTH))) return -1;
    return 0;
}

int lbv_record_valid(unsigned id, long long kind, long long fps_milli, long long spread_milli, long long why) {
    if (id >= VB_COUNT) return 0;
    switch (kind) {
    case VBR_STEP: return 1;
    case VBR_RESULT: return fps_milli > 0 && fps_milli < 100000000LL && spread_milli >= 0 && spread_milli < 100000000LL;
    case VBR_SKIP: return why >= VBS_NOCODEC && why <= VBS_MODE;
    }
    return 0;
}

int lbv_run(int full, LbvOn on, int (*tick)(void *ctx), void *ctx) {
    char path[PATH_MAX];
    if (media_path(path, sizeof path)) return -1;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 3);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    char *envp[] = { "LANG=C.UTF-8", "SVT_LOG=1", NULL };      /* SVT-AV1 prints its settings otherwise */
    char *argv[] = { "lord-media", NULL };
    pid_t pid;
    int e = posix_spawn(&pid, path, &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(sv[1]);
    if (e) { close(sv[0]); return -1; }
    int sock = sv[0], rc = -4;
    int64_t v[6] = { MK_VBENCH, 0, 0, 0, 0, 0 };
    const char *params = full ? "full=1\n" : "full=0\n";
    if (mp_send(sock, MM_JOB, 0, v, params, strlen(params), NULL, 0)) { rc = -1; goto out; }
    static const int map[VB_COUNT] = { LB_VID_H264_1080, LB_VID_HEVC_1080, LB_VID_VP9_1080, LB_VID_AV1_1080,
                                       LB_VID_H264_2160, LB_VID_HEVC_2160, LB_VENC_H264, LB_VENC_VP8 };
    static const int skip_map[] = { 0, LB_SKIP_NOCODEC, LB_SKIP_FAILED, LB_SKIP_MODE };
    double deadline = bench_now_ms() + (full ? 600.0 : 120.0) * 1000;
    for (;;) {
        if (tick && tick(ctx)) { rc = -2; break; }
        if (bench_now_ms() > deadline) { rc = -4; break; }
        struct pollfd pf = { sock, POLLIN, 0 };
        int pr = poll(&pf, 1, 250);
        if (pr < 0 && errno != EINTR) { rc = -3; break; }
        if (pr <= 0) continue;
        static char payload[MP_PAYLOAD_MAX + 1];
        MediaMsg m;
        int fds[MP_MAX_FDS];
        int r = mp_recv(sock, &m, payload, sizeof payload, fds, 0);
        for (int i = 0; r > 0 && i < MP_MAX_FDS; i++) if (fds[i] >= 0) { close(fds[i]); r = -2; }   /* no fds expected */
        if (r == -1) continue;
        if (r <= 0) { rc = -3; break; }
        if (m.type == MM_STATE && m.v[0] == MS_DONE) { rc = 0; break; }
        if (m.type == MM_LEVEL || m.type == MM_ERROR) continue;
        if (m.type != MM_BENCH || !lbv_record_valid(m.id, m.v[0], m.v[1], m.v[2], m.v[3])) { rc = -3; break; }
        int id = map[m.id];
        if (m.v[0] == VBR_STEP) on(ctx, LB_MSG_STEP, id, 0, 0);
        else if (m.v[0] == VBR_RESULT) on(ctx, LB_MSG_RESULT, id, m.v[1] / 1000.0, m.v[2] / 1000.0);
        else on(ctx, LB_MSG_SKIP, id, 0, skip_map[m.v[3]]);
    }
out:
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    close(sock);
    return rc;
}
