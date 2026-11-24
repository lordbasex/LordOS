/* nvgpu.c — lord-server's side of lord-nvgpu (nvgpu.h). See nvgpu_main.c for the helper and nvproto.h for the wire. */
#include "monitor/nvgpu.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define REQ_EVERY_MS   1000             /* 1 Hz */
#define BACKOFF_MIN_MS 2000
#define BACKOFF_MAX_MS 60000
#define PRESENT_EVERY_MS 30000
#define PID_MAX        4194304          /* the kernel's PID_MAX_LIMIT */
#define SIZE_MAX_B     (1LL << 50)      /* 1 PiB */
#define POWER_MAX_MW   10000000LL       /* 10 kW */
#define CLK_MAX_MHZ    100000
#define JSON_MAX       (128 * 1024)
#define ZOMBIES        8

static struct {
    pthread_mutex_t mu;
    int want, sock, pending, first, have;
    pid_t pid;
    double last_req, next_start, present_at;
    int present, backoff;
    pid_t zombies[ZOMBIES];
    char json[JSON_MAX];
    int jlen;
} N = { .mu = PTHREAD_MUTEX_INITIALIZER, .sock = -1, .backoff = BACKOFF_MIN_MS };

static NvSample S;                      /* only the Monitor's thread (nvgpu_poll) touches it, under N.mu */
static unsigned char RX[NVP_MSG_MAX + 1];
static int timeout_ms = 5000, backoff_min = BACKOFF_MIN_MS;

#ifdef NVGPU_TEST
static const char *t_path;
static char *const *t_argv, *const *t_envp;
void nvgpu_test_helper(const char *path, char *const *argv, char *const *envp) { t_path = path; t_argv = argv; t_envp = envp; }
void nvgpu_test_timing(int to, int bo) { timeout_ms = to; backoff_min = bo; N.backoff = bo; }
static int t_starts;
int nvgpu_test_pid(int *starts) { pthread_mutex_lock(&N.mu); int p = N.pid; if (starts) *starts = t_starts; pthread_mutex_unlock(&N.mu); return p; }
#endif

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ---------- checking a message ---------- */

int nvgpu_pid_owned(int pid, unsigned uid) {
    char p[32];
    struct stat st;
    snprintf(p, sizeof p, "/proc/%d", pid);
    return !stat(p, &st) && st.st_uid == uid;
}

static int32_t in32(int32_t v, int32_t lo, int32_t hi) { return v >= lo && v <= hi ? v : NVP_NONE; }
static int64_t in64(int64_t v, int64_t hi) { return v >= 0 && v <= hi ? v : NVP_NONE64; }

int nvgpu_parse(const unsigned char *msg, size_t len, unsigned uid, int (*own)(int, unsigned), NvSample *s) {
    memset(s, 0, sizeof *s);
    NvHdr h;
    if (!msg || len < sizeof h) return -1;
    memcpy(&h, msg, sizeof h);
    if (h.magic != NVP_MAGIC || h.ngpu > NVP_MAX_GPUS || h.nproc > NVP_MAX_PROCS) return -1;
    if (len != sizeof h + (size_t)h.ngpu * sizeof(NvGpu) + (size_t)h.nproc * sizeof(NvProc)) return -1;
    if (h.status != 0) return 0;                         /* NVML failed: no cards */
    /* the driver's version: NUL-terminated within the field, digits and dots only */
    size_t k = 0;
    while (k < NVP_DRIVER_LEN && ((h.driver[k] >= '0' && h.driver[k] <= '9') || h.driver[k] == '.')) k++;
    if (k < NVP_DRIVER_LEN && h.driver[k] == 0) memcpy(s->driver, h.driver, k + 1);
    if (h.cuda >= 1000 && h.cuda <= 999999) snprintf(s->cuda, sizeof s->cuda, "%d.%d", h.cuda / 1000, (h.cuda % 1000) / 10);
    const unsigned char *p = msg + sizeof h;
    for (int i = 0; i < h.ngpu; i++, p += sizeof(NvGpu)) {
        NvGpu g;
        memcpy(&g, p, sizeof g);
        if (g.domain == UINT32_MAX || g.bus > 255 || g.dev > 31 || g.fn > 7) g.domain = UINT32_MAX, g.bus = g.dev = g.fn = 0;
        g.util_sm = in32(g.util_sm, 0, 100);
        g.util_mem = in32(g.util_mem, 0, 100);
        g.util_enc = in32(g.util_enc, 0, 100);
        g.util_dec = in32(g.util_dec, 0, 100);
        g.temp = g.temp == NVP_NONE ? NVP_NONE : in32(g.temp, -50, 150);
        g.fan = in32(g.fan, 0, 100);
        g.clk_core = in32(g.clk_core, 0, CLK_MAX_MHZ);
        g.clk_mem = in32(g.clk_mem, 0, CLK_MAX_MHZ);
        g.compute_mode = in32(g.compute_mode, 0, 3);
        g.ecc = in32(g.ecc, 0, 1);
        g.power_mw = in64(g.power_mw, POWER_MAX_MW);
        g.power_limit_mw = in64(g.power_limit_mw, POWER_MAX_MW);
        g.vram_used = in64(g.vram_used, SIZE_MAX_B);
        g.vram_total = in64(g.vram_total, SIZE_MAX_B);
        s->gpu[s->ngpu++] = g;
    }
    for (int i = 0; i < h.nproc; i++, p += sizeof(NvProc)) {    /* at most NVP_MAX_PROCS ownership checks */
        NvProc q;
        memcpy(&q, p, sizeof q);
        if (q.gpu >= (uint32_t)h.ngpu || q.pid < 1 || q.pid > PID_MAX) continue;
        if (!own || !own(q.pid, uid)) continue;
        q.sm = in32(q.sm, 0, 100);
        q.enc = in32(q.enc, 0, 100);
        q.dec = in32(q.dec, 0, 100);
        q.mem = in64(q.mem, SIZE_MAX_B);
        q.pad = 0;
        s->proc[s->nproc++] = q;
    }
    return 0;
}

/* ---------- JSON ---------- */

typedef struct { char *p; size_t n, len; int over; } W;

static void w(W *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void w(W *o, const char *fmt, ...) {
    if (o->over) return;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(o->p + o->len, o->n - o->len, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= o->n - o->len) { o->over = 1; return; }
    o->len += (size_t)r;
}
static void wi(W *o, const char *k, int32_t v) { if (v == NVP_NONE) w(o, "\"%s\":null", k); else w(o, "\"%s\":%d", k, v); }
static void wl(W *o, const char *k, int64_t v) { if (v == NVP_NONE64) w(o, "\"%s\":null", k); else w(o, "\"%s\":%lld", k, (long long)v); }
static void ww(W *o, const char *k, int64_t mw) { if (mw == NVP_NONE64) w(o, "\"%s\":null", k); else w(o, "\"%s\":%lld.%03lld", k, (long long)(mw / 1000), (long long)(mw % 1000)); }

int nvgpu_format(const NvSample *s, char *out, size_t n) {
    if (!out || !n) return 0;
    out[0] = 0;
    if (!s->ngpu) return 0;
    static const char *const modes[] = { "default", "exclusiveThread", "prohibited", "exclusiveProcess" };
    W o = { out, n, 0, 0 };
    w(&o, "\"nv\":[");
    for (int i = 0; i < s->ngpu; i++) {
        const NvGpu *g = &s->gpu[i];
        w(&o, "%s{", i ? "," : "");
        if (g->domain == UINT32_MAX) w(&o, "\"bus\":null,");
        else w(&o, g->domain <= 0xffff ? "\"bus\":\"%04x:%02x:%02x.%x\"," : "\"bus\":\"%08x:%02x:%02x.%x\",", g->domain, g->bus, g->dev, g->fn);
        w(&o, "\"util\":{"); wi(&o, "sm", g->util_sm); w(&o, ","); wi(&o, "mem", g->util_mem); w(&o, ",");
        wi(&o, "enc", g->util_enc); w(&o, ","); wi(&o, "dec", g->util_dec); w(&o, "},");
        wi(&o, "temp", g->temp); w(&o, ","); wi(&o, "fan", g->fan); w(&o, ",");
        ww(&o, "power", g->power_mw); w(&o, ","); ww(&o, "powerLimit", g->power_limit_mw); w(&o, ",");
        w(&o, "\"clk\":{"); wi(&o, "core", g->clk_core); w(&o, ","); wi(&o, "mem", g->clk_mem); w(&o, "},");
        w(&o, "\"vram\":{"); wl(&o, "used", g->vram_used); w(&o, ","); wl(&o, "total", g->vram_total); w(&o, "},");
        /* cuda and driver hold only digits and dots (checked in nvgpu_parse): nothing to escape */
        if (s->cuda[0]) w(&o, "\"cuda\":\"%s\",", s->cuda); else w(&o, "\"cuda\":null,");
        if (s->driver[0]) w(&o, "\"driver\":\"%s\",", s->driver); else w(&o, "\"driver\":null,");
        if (g->compute_mode == NVP_NONE) w(&o, "\"computeMode\":null,"); else w(&o, "\"computeMode\":\"%s\",", modes[g->compute_mode]);
        if (g->ecc == NVP_NONE) w(&o, "\"ecc\":null,"); else w(&o, "\"ecc\":%s,", g->ecc ? "true" : "false");
        w(&o, "\"procs\":[");
        int first = 1;
        for (int j = 0; j < s->nproc; j++) {
            const NvProc *q = &s->proc[j];
            if (q->gpu != (uint32_t)i) continue;
            w(&o, "%s{\"pid\":%d,", first ? "" : ",", q->pid);
            first = 0;
            wl(&o, "mem", q->mem); w(&o, ","); wi(&o, "sm", q->sm); w(&o, ","); wi(&o, "enc", q->enc); w(&o, ",");
            wi(&o, "dec", q->dec); w(&o, "}");
        }
        w(&o, "]}");
    }
    w(&o, "]");
    if (o.over) { out[0] = 0; return 0; }
    return (int)o.len;
}

/* ---------- the helper ---------- */

/* a PCI device of vendor 0x10de bound to `nvidia` */
static int nvidia_present(void) {
#ifdef NVGPU_TEST
    if (t_path) return 1;
#endif
    DIR *d = opendir("/sys/bus/pci/devices");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[PATH_MAX], v[16] = { 0 }, drv[PATH_MAX];
        snprintf(p, sizeof p, "/sys/bus/pci/devices/%.64s/vendor", e->d_name);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t r = read(fd, v, sizeof v - 1);
        close(fd);
        if (r < 6 || strncmp(v, "0x10de", 6)) continue;
        snprintf(p, sizeof p, "/sys/bus/pci/devices/%.64s/driver", e->d_name);
        ssize_t l = readlink(p, drv, sizeof drv - 1);
        if (l <= 0) continue;
        drv[l] = 0;
        const char *b = strrchr(drv, '/');
        if (b && !strcmp(b + 1, "nvidia")) found = 1;
    }
    closedir(d);
    return found;
}

/* lord-nvgpu next to lord-server, root's (or ours in development) and not writable by group or others */
static int helper_path(char *bin, size_t size) {
#ifdef NVGPU_TEST
    if (t_path) { snprintf(bin, size, "%s", t_path); return 0; }
#endif
    ssize_t n = readlink("/proc/self/exe", bin, size - 16);
    if (n <= 0) return -1;
    bin[n] = 0;
    char *slash = strrchr(bin, '/');
    if (!slash) return -1;
    strcpy(slash + 1, "lord-nvgpu");
    struct stat st;
    if (stat(bin, &st) || !S_ISREG(st.st_mode) || (st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & 022)) return -1;
    return 0;
}

static void reap_zombies(void) {
    for (int i = 0; i < ZOMBIES; i++)
        if (N.zombies[i] > 0) {
            pid_t r = waitpid(N.zombies[i], NULL, WNOHANG);
            if (r == N.zombies[i] || (r < 0 && errno == ECHILD)) N.zombies[i] = 0;
        }
}

/* kill it without waiting: a process stuck in the driver is reaped on a later tick */
static void stop_locked(int failed) {
    if (N.sock >= 0) { close(N.sock); N.sock = -1; }
    if (N.pid > 0) {
        kill(N.pid, SIGKILL);
        pid_t r = waitpid(N.pid, NULL, WNOHANG);
        if (r != N.pid && !(r < 0 && errno == ECHILD)) {
            int i;
            for (i = 0; i < ZOMBIES && N.zombies[i] > 0; i++) {}
            if (i < ZOMBIES) N.zombies[i] = N.pid;
        }
        N.pid = 0;
    }
    N.pending = 0;
    if (failed) {
        N.next_start = now_ms() + N.backoff;
        N.backoff = N.backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : N.backoff * 2;
    }
}

static int start_locked(void) {
    char bin[PATH_MAX];
    if (helper_path(bin, sizeof bin)) return -1;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    if (sv[1] == 3) {                                   /* dup2(3, 3) wouldn't clear CLOEXEC */
        int t = fcntl(sv[1], F_DUPFD_CLOEXEC, 10);
        close(sv[1]);
        if (t < 0) { close(sv[0]); return -1; }
        sv[1] = t;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 3);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    char *argv[] = { "lord-nvgpu", NULL }, *envp[] = { NULL };
    char *const *av = argv, *const *ev = envp;
#ifdef NVGPU_TEST
    if (t_argv) av = t_argv;
    if (t_envp) ev = t_envp;
#endif
    pid_t pid;
    int e = posix_spawn(&pid, bin, &fa, &at, av, ev);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(sv[1]);
    if (e) { close(sv[0]); return -1; }
    N.pid = pid;
#ifdef NVGPU_TEST
    t_starts++;
#endif
    N.sock = sv[0];
    N.pending = 0;
    N.first = 1;
    N.last_req = 0;
    return 0;
}

static unsigned session_uid(void) { return (unsigned)getuid(); }

void nvgpu_poll(void) {
    pthread_mutex_lock(&N.mu);
    reap_zombies();
    if (!N.want) goto out;
    double t = now_ms();
    if (N.pid <= 0) {
        if (t < N.next_start) goto out;
        if (!N.present_at || t - N.present_at > PRESENT_EVERY_MS) { N.present = nvidia_present(); N.present_at = t; }
        if (!N.present) goto out;
        if (start_locked()) { N.next_start = t + BACKOFF_MAX_MS; goto out; }
    }
    /* the answers that came (normally one), the last good one kept */
    for (int i = 0; i < 4; i++) {
        ssize_t r = recv(N.sock, RX, sizeof RX, MSG_DONTWAIT | MSG_TRUNC);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0 || (size_t)r > NVP_MSG_MAX || !N.pending) { stop_locked(1); N.have = 0; goto out; }  /* gone, too big or unasked */
        N.pending = 0;
        if (nvgpu_parse(RX, (size_t)r, session_uid(), nvgpu_pid_owned, &S)) { stop_locked(1); N.have = 0; goto out; }
        N.jlen = nvgpu_format(&S, N.json, sizeof N.json);
        N.have = N.jlen > 0;
        N.first = 0;
        N.backoff = backoff_min;
    }
    if (N.pending && t - N.last_req > (N.first ? 3 * timeout_ms : timeout_ms)) { stop_locked(1); N.have = 0; goto out; }   /* slow */
    if (!N.pending && t - N.last_req >= REQ_EVERY_MS) {
        unsigned char req = NVP_REQ_SAMPLE;
        if (send(N.sock, &req, 1, MSG_DONTWAIT | MSG_NOSIGNAL) == 1) { N.pending = 1; N.last_req = t; }
        else if (errno != EAGAIN && errno != EINTR) { stop_locked(1); N.have = 0; }
    }
out:
    pthread_mutex_unlock(&N.mu);
}

void nvgpu_want(int on) {
    pthread_mutex_lock(&N.mu);
    N.want = !!on;
    if (!on) { stop_locked(0); N.have = 0; }
    pthread_mutex_unlock(&N.mu);
}

void nvgpu_suspend(void) {
    pthread_mutex_lock(&N.mu);
    if (N.pid > 0) stop_locked(0);
    N.have = 0;
    pthread_mutex_unlock(&N.mu);
}

int nvgpu_json(char *out, size_t n) {
    int r = 0;
    pthread_mutex_lock(&N.mu);
    if (N.have && out && n > (size_t)N.jlen) { memcpy(out, N.json, (size_t)N.jlen + 1); r = N.jlen; }
    pthread_mutex_unlock(&N.mu);
    return r;
}
