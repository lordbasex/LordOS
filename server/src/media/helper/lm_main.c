/* lm_main.c — entry point of lord-media (see lm.h and docs/MEDIA.md).
 *
 * lord-server launches it with no arguments, with the socket on fd 3. The first message (MM_JOB)
 * says what type of helper it is and brings the descriptors. Before reading a single byte of the file
 * the sandbox is applied: Landlock (no home folder or network) and, after initializing
 * VA-API if needed, seccomp. */
#include "lm.h"
#include "../../sec/sandbox.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>

int lm_debug;

static void av_log_cb(void *avcl, int level, const char *fmt, va_list vl) {
    (void)avcl;
    if (level > (lm_debug ? AV_LOG_WARNING : AV_LOG_ERROR)) return;
    static int count;
    if (++count > 200) return;                  /* a broken file doesn't flood the log */
    char line[512];
    vsnprintf(line, sizeof line, fmt, vl);
    fprintf(stderr, LM_WHO "[%d]: ffmpeg: %s", (int)getpid(), line);
}

int main(int argc, char **argv) {
    (void)argv;
    if (argc != 1) { fprintf(stderr, "lord-media: launched by lord-server (no arguments)\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    prctl(PR_SET_PDEATHSIG, SIGKILL);           /* if lord-server goes away, so does this one */
    if (getppid() == 1) return 1;
#ifdef LM_NET
    prctl(PR_SET_NAME, "lord-stream");
#elif defined(LM_STUDIO)
    prctl(PR_SET_NAME, "lord-studio");
#else
    prctl(PR_SET_NAME, "lord-media");
#endif
    lm_debug = getenv("LORD_MEDIA_DEBUG") != NULL;
    /* only the socket: any other inherited descriptor is closed */
#ifdef SYS_close_range
    syscall(SYS_close_range, LM_SOCK + 1, ~0U, 0);
#endif
    struct rlimit core = { 0, 0 };
    setrlimit(RLIMIT_CORE, &core);
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);         /* another process of the user can't debug it */
    av_log_set_callback(av_log_cb);

    static char payload[MP_PAYLOAD_MAX + 1];
    MediaMsg m;
    int fds[MP_MAX_FDS];
    if (mp_recv(LM_SOCK, &m, payload, sizeof payload, fds, 1) != 1 || m.type != MM_JOB) {
        LM_LOG("the job didn't arrive");
        return 1;
    }
    /* mp_send skips missing descriptors: a silent video has no audio ring, and its render node comes in the ring's place */
#ifdef LM_NET
    /* lord-stream: a network player or a channel list, nothing else; its fds are the audio ring and the render node (no
     * file): shifted into the player's places (mp_send skips the missing ones) */
    if (m.v[0] != MK_PLAY && m.v[0] != MK_M3U) { LM_LOG("lord-stream: not a stream job"); return 1; }
    if (m.v[0] == MK_PLAY) {
        int ring = mp_param_int(payload, "audio", 0) ? fds[0] : -1;
        int render = mp_param_int(payload, "hw", 0) ? fds[ring >= 0 ? 1 : 0] : -1;
        fds[0] = -1; fds[1] = ring; fds[2] = render;
    }
#else
    if (m.v[0] == MK_M3U) { LM_LOG("lord-media: no network jobs"); return 1; }
#endif
#ifdef LM_STUDIO
    /* lord-studio: the camera with LordOS Studio's effects, nothing else. Its fds, in order (mp_send skips the missing
     * ones): the camera (none with probe=1), the render node (when render=1), the segmenter's model */
    if (m.v[0] != MK_CAMERA) { LM_LOG("lord-studio: only the camera"); return 1; }
    int st_probe = mp_param_int(payload, "probe", 0) == 1, sk = 0;
    int st_cam = st_probe ? -1 : fds[sk++];
    int studio_render = mp_param_int(payload, "render", 0) == 1 ? fds[sk++] : -1;
    int st_model = fds[sk];
    for (int i = sk + 1; i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]);
    fds[0] = st_cam; fds[1] = studio_render; fds[2] = fds[3] = -1;
#endif
    if (m.v[0] == MK_PLAY && mp_param_int(payload, "hw", 0) && fds[2] < 0 && fds[1] >= 0) { fds[2] = fds[1]; fds[1] = -1; }
    /* The shared sandbox (sec/sandbox.c, the same as lord-doc's), before reading a byte
     * of the file and before initializing VA-API: no network, nothing opened outside the system's libraries and
     * data (read-only), no /dev (devices arrive as fds) and with the
     * DRM/V4L2/dma-buf ioctls only if the job needs them. The VA-API driver is loaded
     * afterwards, from /usr/lib, which stays read-only. */
    static const char *const ro[] = { "/usr/lib", "/usr/lib64", "/lib", "/lib64", "/usr/share", "/etc/ld.so.cache",
                                      "/sys/dev/char", "/sys/devices", NULL };
    static const int extra[] = { SYS_getcpu, SYS_kcmp, SYS_membarrier, SYS_sched_getparam, SYS_sched_getscheduler,
#ifdef SYS_futex_waitv
                                 SYS_futex_waitv,
#endif
                                 -1 };
    int ioctls = 0;
    if ((m.v[0] == MK_PLAY || m.v[0] == MK_SCREENREC) && fds[2] >= 0) ioctls |= SANDBOX_IOCTL_DRM | SANDBOX_IOCTL_DMABUF;
    if (m.v[0] == MK_HWPROBE && fds[0] >= 0) ioctls |= SANDBOX_IOCTL_DRM;
    /* MK_STREAM: the descriptors that came, in order (mp_send skips the missing ones) */
    int st_k = 0, st_ring = -1, st_render = -1;
    if (m.v[0] == MK_STREAM) {
        if (mp_param_int(payload, "audio", 0) == 1) st_ring = fds[st_k++];
        if (mp_param_int(payload, "render", 0) == 1) st_render = fds[st_k++];
        if (st_render >= 0) ioctls |= SANDBOX_IOCTL_DRM | SANDBOX_IOCTL_DMABUF;
    }
    if (m.v[0] == MK_CAMERA) ioctls |= (fds[0] >= 0 ? SANDBOX_IOCTL_V4L2 : 0) | (fds[1] >= 0 ? SANDBOX_IOCTL_DRM | SANDBOX_IOCTL_DMABUF : 0);
    /* The Pi 5's HEVC decoder: the Raspberry Pi FFmpeg opens these by path, so only here is /dev reachable, and only
     * the two exact files of that decoder, found from sysfs before the sandbox. */
    static char dev_video[32], dev_media[32];
    static const char *devs[3];
    static const char *const ro_pi[] = { "/usr/lib", "/usr/lib64", "/lib", "/lib64", "/usr/share", "/etc/ld.so.cache",
                                         "/sys/dev/char", "/sys/devices", "/sys/class", "/sys/bus", "/run/udev", NULL };   /* udev lists these folders to find the decoder */
    int pi_hevc = m.v[0] == MK_PLAY && fds[2] >= 0 && lm_hwaccel_is(payload, "v4l2request") &&
                  !lm_hevc_dec_paths(dev_video, sizeof dev_video, dev_media, sizeof dev_media);
    if (pi_hevc) {
        ioctls |= SANDBOX_IOCTL_V4L2 | SANDBOX_IOCTL_MEDIA;
        devs[0] = dev_video;
        devs[1] = dev_media;
    }
#ifdef LM_NET
    /* lord-stream: the same sandbox plus internet sockets (never unix or netlink), in the firewall scope
     * lordos-net-streams (firewall app lordos:streams), and what name resolution and TLS read */
    static const char *const ro_net[] = { "/usr/lib", "/usr/lib64", "/lib", "/lib64", "/usr/share", "/etc/ld.so.cache",
                                          "/sys/dev/char", "/sys/devices", "/etc/resolv.conf", "/etc/hosts", "/etc/nsswitch.conf",
                                          "/etc/host.conf", "/etc/gai.conf", "/etc/ssl", "/etc/gnutls", "/run/systemd/resolve", NULL };
    SandboxHelperCfg cfg = { .name = "lordos-net-streams", .ro_paths = ro_net, .extra_syscalls = extra, .allow_ioctl_classes = ioctls,
                             .mem_max_mb = 768, .network = 1 };
    (void)ro_pi; (void)pi_hevc; (void)ro; (void)devs;
#elif defined(LM_STUDIO)
    /* lord-studio: lord-media's sandbox for a camera, in its own scope; no network, no PipeWire, no exec */
    SandboxHelperCfg cfg = { .name = "app-lordos-studio", .ro_paths = ro, .extra_syscalls = extra, .allow_ioctl_classes = ioctls,
                             .mem_max_mb = 1024 };
    (void)ro_pi; (void)pi_hevc; (void)devs;
#else
    SandboxHelperCfg cfg = { .name = "app-lordos-media", .ro_paths = pi_hevc ? ro_pi : ro, .extra_syscalls = extra, .allow_ioctl_classes = ioctls,
                             .mem_max_mb = m.v[0] == MK_WORKER ? 1024 : 1536, .dev_files = pi_hevc ? devs : NULL };
#endif
    if (m.v[0] == MK_WORKER) {                 /* thumbnails and probes: in the background (before the sandbox) */
        setpriority(PRIO_PROCESS, 0, 10);
        syscall(SYS_ioprio_set, 1, 0, (2 << 13) | 7);   /* IOPRIO_WHO_PROCESS, this process, best effort, lowest */
    }
    sandbox_apply_helper(&cfg);
#ifdef LM_STUDIO
    /* the model is read and checked inside the sandbox; Mesa loads here too (from /usr/lib, read-only) */
    if (stcam_init(st_model, studio_render)) { lm_error("studio.failed"); mp_sendv(LM_SOCK, MM_STATE, 0, MS_FAILED, 0, 0, 0); return 1; }
    if (st_probe) { stcam_probe(); stcam_free(); return 0; }
#endif
    switch (m.v[0]) {
    case MK_PLAY: lm_player(payload, fds[0], fds[1], fds[2]); break;
    case MK_WORKER:
        lm_worker();
        break;
    case MK_SCREENREC: lm_screenrec(payload, fds[0], fds[1], fds[2]); break;
    case MK_CAMERA: lm_camera(payload, fds[0], fds[1]); break;
    case MK_AUDIOREC: lm_audiorec(payload, fds[0], fds[1]); break;
    case MK_HWPROBE: lm_hwprobe(fds, MP_MAX_FDS); break;
    case MK_STREAM: lm_stream(payload, st_ring, st_render); break;
    case MK_VBENCH: lm_vbench(payload); break;
    case MK_LIVE: lm_live(payload, fds[0], fds[1]); break;
#ifdef LM_NET
    case MK_M3U: ls_m3u(payload); break;
#endif
    default: LM_LOG("unknown kind %lld", (long long)m.v[0]); return 1;
    }
#ifdef LM_STUDIO
    stcam_free();
#endif
    return 0;
}
