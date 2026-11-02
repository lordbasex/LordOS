/* lbmain.c — lord-bench, LordBench's helper (docs/BENCHMARK.md, "0.8.6 — LordBench").
 *
 * lord-server (lbrun.c) launches it from a fixed path next to its own binary, with no arguments, the
 * channel on fd 3 and a minimal environment. The first message (LbGo) says what to run and brings at
 * most two fds lord-server opened: the GPU's render node and a scratch file for the storage test. Then,
 * before any work, it locks itself in with sandbox_apply_helper (sec/sandbox.c, the same as
 * lord-doc's and lord-media's): no network, no exec, no new files, read-only system libraries for Mesa,
 * DRM ioctls only on the fd it was given. Right after that it checks its own lock-in (the helper-sandbox
 * self-test, sandbox_tries: things that must fail). It answers only with fixed-size LbMsg records
 * (lbproto.h); lord-server checks each one, watches temperatures and throttling, and kills it to stop. */
#include "lbproto.h"
#include "lbwork.h"
#include "lbui.h"
#ifdef LORD_GPU
#include "lbgpu.h"
#endif
#include "../sec/sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define SOCK 3

static void send_msg(uint32_t kind, int id, double value, double aux) {
    LbMsg m = { LB_MAGIC, kind, (uint32_t)id, 0, value, aux };
    if (send(SOCK, &m, sizeof m, MSG_NOSIGNAL) != (ssize_t)sizeof m) _exit(1);   /* lord-server is gone */
}

/* runs a measurement `reps` times and reports the median with its spread */
static void measure(int id, int reps, double (*fn)(void *), void *ctx) {
    send_msg(LB_MSG_STEP, id, 0, 0);
    double v[5];
    int n = 0;
    for (int i = 0; i < reps && i < 5; i++) {
        double r = fn(ctx);
        if (r <= 0) { send_msg(LB_MSG_SKIP, id, 0, LB_SKIP_FAILED); return; }
        v[n++] = r;
    }
    double spread, med = lb_median(v, n, &spread);
    send_msg(LB_MSG_RESULT, id, med, spread);
}

typedef struct { int threads; double secs; size_t mb; long hops; } Ctx;
static double w_sha(void *c)   { Ctx *x = c; return lb_sha256(x->threads, x->secs); }
static double w_mat(void *c)   { Ctx *x = c; return lb_matmul(x->threads, x->secs); }
static double w_xz(void *c)    { Ctx *x = c; return lb_xz(x->threads, x->secs); }
static double w_js(void *c)    { Ctx *x = c; return lb_js(x->threads, x->secs); }
static double w_ui(void *c)    { Ctx *x = c; return lb_ui(x->threads, x->secs); }
static double w_shape(void *c) { Ctx *x = c; return lb_shape(x->threads, x->secs); }
static double w_copy(void *c)  { Ctx *x = c; return lb_mem_copy(x->threads, x->mb, x->secs); }
static double w_triad(void *c) { Ctx *x = c; return lb_mem_triad(x->threads, x->mb, x->secs); }
static double w_lat(void *c)   { Ctx *x = c; return lb_mem_latency(x->mb, x->hops); }
#ifdef LORD_GPU
static double w_scene(void *c)  { return lb_gpu_scene(((Ctx *)c)->secs); }
static double w_fill(void *c)   { return lb_gpu_fill(((Ctx *)c)->secs); }
static double w_upload(void *c) { return lb_gpu_upload(((Ctx *)c)->secs); }
static double w_layers(void *c) { return lb_gpu_layers(((Ctx *)c)->secs); }
#endif

typedef struct { double first, last; int n; } Sus;
static void on_sample(double gflops, double t_s, void *ctx) {
    Sus *s = ctx;
    if (!s->n++) s->first = gflops;
    s->last = gflops;
    send_msg(LB_MSG_SAMPLE, 0, gflops, t_s);
}

/* ---- the helper-sandbox self-test (lbproto.h, LB_SBX_*): each try must fail; none changes anything ---- */

static void sbx_put(int id, int refused, int err) { send_msg(LB_MSG_SANDBOX, id, refused, refused && err > 0 && err < 4096 ? err : 0); }
static void sbx_fd(int id, int fd) {
    int e = errno;
    sbx_put(id, fd < 0, e);
    if (fd >= 0) close(fd);                     /* it worked: an unnamed file or an unconnected socket, gone */
}

static void sandbox_tries(void) {
    errno = 0;
    sbx_fd(LB_SBX_SOCKET, socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    errno = 0;
    sbx_fd(LB_SBX_INET, socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    /* an invalid environment pointer: an exec the sandbox let through fails with EFAULT before anything is
     * replaced, so EFAULT means it was not refused */
    static char *const argv[] = { (char *)"/usr/bin/true", NULL };
    errno = 0;
    long r = syscall(SYS_execve, "/usr/bin/true", argv, (char *const *)1);
    int e = errno;
    sbx_put(LB_SBX_EXEC, r < 0 && e != EFAULT, e);
    errno = 0;
    pid_t pid = fork();
    e = errno;
    if (pid == 0) _exit(0);
    if (pid > 0) waitpid(pid, NULL, 0);
    sbx_put(LB_SBX_FORK, pid < 0, e);
    errno = 0;
    sbx_fd(LB_SBX_NEWFILE, open("/tmp", O_TMPFILE | O_WRONLY | O_CLOEXEC, 0600));
    errno = 0;
    sbx_fd(LB_SBX_READ, open("/etc/passwd", O_RDONLY | O_CLOEXEC));
}

static void skip_all(int from, int to, int why) {
    for (int id = from; id <= to; id++) send_msg(LB_MSG_SKIP, id, 0, why);
}

int main(int argc, char **argv) {
    (void)argv;
    if (argc != 1) { fprintf(stderr, "lord-bench: launched by lord-server (no arguments)\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() == 1) return 1;
    prctl(PR_SET_NAME, "lord-bench");
#ifdef SYS_close_range
    syscall(SYS_close_range, SOCK + 1, ~0U, 0);
#endif
    struct rlimit core = { 0, 0 };
    setrlimit(RLIMIT_CORE, &core);
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

    /* the job and its fds */
    LbGo go;
    int fds[2] = { -1, -1 };
    char cbuf[CMSG_SPACE(2 * sizeof(int))];
    struct iovec iov = { &go, sizeof go };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    ssize_t r = recvmsg(SOCK, &mh, MSG_CMSG_CLOEXEC);
    if (r != (ssize_t)sizeof go || go.magic != LB_MAGIC || (go.mode != LB_MODE_QUICK && go.mode != LB_MODE_FULL && go.mode != LB_MODE_LOAD)) return 1;
    int nfd = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            nfd = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            if (nfd > 2) return 1;
            memcpy(fds, CMSG_DATA(c), (size_t)nfd * sizeof(int));
        }
    int k = 0, gpu_fd = go.flags & LB_GO_GPU ? (k < nfd ? fds[k++] : -1) : -1;
    int disk_fd = go.flags & LB_GO_STORAGE ? (k < nfd ? fds[k++] : -1) : -1;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int threads = (int)go.threads;
    if (threads < 1 || threads > cpus || threads > 256) threads = cpus > 0 && cpus <= 256 ? (int)cpus : 1;
    int full = go.mode == LB_MODE_FULL;

    if (go.flags & LB_GO_LOW) {                 /* desktop impact: below everything the user runs */
        setpriority(PRIO_PROCESS, 0, 19);
        syscall(SYS_ioprio_set, 1, 0, (3 << 13));       /* IOPRIO_CLASS_IDLE */
    }
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);    /* no cache folder: nothing to write */
    /* Mesa's loader names the GPU behind the render node it got through libdrm, which lists /dev/dri and, for a
     * GPU that isn't on PCI (the Raspberry Pi 5's v3d), reads its device tree node; without them "MESA-LOADER:
     * failed to retrieve device information" and no GPU tests (found on the Pi 5, 0.8.6). Read-only: a device
     * opened here gets no ioctls (Landlock's IOCTL_DEV, ABI 5 and later), only the fd lord-server passed does */
    static const char *const ro[] = { "/usr/lib", "/usr/lib64", "/lib", "/lib64", "/usr/share", "/etc/ld.so.cache",
                                      "/sys/dev/char", "/sys/devices", "/sys/firmware/devicetree", "/dev/dri", NULL };
    static const int extra[] = { SYS_getcpu, SYS_kcmp, SYS_membarrier, SYS_sched_getparam, SYS_sched_getscheduler,
#ifdef SYS_futex_waitv
                                 SYS_futex_waitv,
#endif
                                 -1 };
    SandboxHelperCfg cfg = { .name = "lordos-bench", .ro_paths = ro, .extra_syscalls = extra,
                             .allow_ioctl_classes = gpu_fd >= 0 ? SANDBOX_IOCTL_DRM | SANDBOX_IOCTL_DMABUF : 0,
                             .mem_max_mb = 2048 };
    int mask = sandbox_apply_helper(&cfg);
    sandbox_tries();

    if (go.mode == LB_MODE_LOAD) {              /* desktop impact: only the load, while lord-server records frames */
        Sus s = { 0, 0, 0 };
        if (go.sustained_s >= 5 && go.sustained_s <= 120) lb_sustained(threads, go.sustained_s, 1, on_sample, &s);
        send_msg(LB_MSG_DONE, 0, 0, mask < 0 ? 0 : mask);
        return 0;
    }

    Ctx one = { 1, full ? 1.5 : 0.5, 0, 0 }, all = { threads, full ? 1.5 : 0.5, 0, 0 };
    int reps = 3;
    measure(LB_CPU_SHA1, reps, w_sha, &one);
    measure(LB_CPU_SHAN, reps, w_sha, &all);
    measure(LB_CPU_MAT1, reps, w_mat, &one);
    measure(LB_CPU_MATN, reps, w_mat, &all);
    measure(LB_CPU_XZ1, reps, w_xz, &one);
    measure(LB_CPU_XZN, reps, w_xz, &all);
    measure(LB_CPU_JS1, reps, w_js, &one);
    measure(LB_CPU_JSN, reps, w_js, &all);
    measure(LB_CPU_UI1, reps, w_ui, &one);
    measure(LB_CPU_UIN, reps, w_ui, &all);
    measure(LB_CPU_SHAPE1, reps, w_shape, &one);
    measure(LB_CPU_SHAPEN, reps, w_shape, &all);
    Ctx mem = { threads, full ? 1.5 : 0.5, full ? 128 : 32, 0 };
    measure(LB_MEM_COPY, reps, w_copy, &mem);
    measure(LB_MEM_TRIAD, reps, w_triad, &mem);
    Ctx lat = { 1, 0, 64, full ? 10000000 : 2000000 };
    measure(LB_MEM_LAT, reps, w_lat, &lat);

#ifdef LORD_GPU
    if (gpu_fd >= 0) {
        BenchGlInfo gi;
        if (lb_gpu_open(gpu_fd, &gi) == 0) {
            send_msg(LB_MSG_GPUINFO, 0, !strcmp(gi.kind, "HARDWARE GPU") ? 0 : !strcmp(gi.kind, "VIRTUAL GPU (HOST)") ? 1 : 2, 0);
            Ctx g = { 1, full ? 1.5 : 0.5, 0, 0 };
            measure(LB_GPU_SCENE, reps, w_scene, &g);
            measure(LB_GPU_FILL, reps, w_fill, &g);
            measure(LB_GPU_UPLOAD, reps, w_upload, &g);
            measure(LB_GPU_LAYERS, reps, w_layers, &g);
            lb_gpu_close();
        } else skip_all(LB_GPU_SCENE, LB_GPU_LAYERS, LB_SKIP_FAILED);
    } else skip_all(LB_GPU_SCENE, LB_GPU_LAYERS, LB_SKIP_NODEV);
#else
    skip_all(LB_GPU_SCENE, LB_GPU_LAYERS, LB_SKIP_NODEV);
#endif

    if (disk_fd >= 0 && go.storage_mb >= 8) {
        send_msg(LB_MSG_STEP, LB_DISK_SEQW, 0, 0);
        double w, rd, iops;
        if (lb_disk(disk_fd, go.storage_mb, 3, &w, &rd, &iops) == 0) {
            send_msg(LB_MSG_RESULT, LB_DISK_SEQW, w, 0);
            send_msg(LB_MSG_RESULT, LB_DISK_SEQR, rd, 0);
            send_msg(LB_MSG_RESULT, LB_DISK_RAND, iops, 0);
        } else skip_all(LB_DISK_SEQW, LB_DISK_RAND, LB_SKIP_FAILED);
        close(disk_fd);
    } else skip_all(LB_DISK_SEQW, LB_DISK_RAND, full ? LB_SKIP_NODEV : LB_SKIP_MODE);

    if (full && go.sustained_s >= 20 && go.sustained_s <= 1800) {
        send_msg(LB_MSG_STEP, LB_SUS_START, 0, 0);
        Sus s = { 0, 0, 0 };
        lb_sustained(threads, go.sustained_s, 10, on_sample, &s);
        if (s.n >= 2 && s.first > 0 && s.last > 0) {
            send_msg(LB_MSG_RESULT, LB_SUS_START, s.first, 0);
            send_msg(LB_MSG_RESULT, LB_SUS_END, s.last, 0);
        } else skip_all(LB_SUS_START, LB_SUS_END, LB_SKIP_FAILED);
    } else skip_all(LB_SUS_START, LB_SUS_END, LB_SKIP_MODE);

    send_msg(LB_MSG_DONE, 0, 0, mask < 0 ? 0 : mask);
    return 0;
}
