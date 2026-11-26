/* engine.c — Sentinela's local model engine, started on demand in its sandbox (see engine.h and docs/SENTINELA.md). */
#include "engine.h"
#include "llmjail.h"
#include "writing.h"
#include "../sec/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <sys/random.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define QUEUE 8
#define RESULTS 16

enum { J_ANALYZE = 1, J_FACTS, J_INVESTIGATE, J_TRANSLATE, J_WRITE };
typedef struct { int id, kind; char model[64]; char *system, *user; int pid; unsigned long long start; int action; char target[16]; } Job;
typedef struct { int id; char *json; } Result;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int started, next_id, stop_now;
    Job q[QUEUE];
    int nq;
    Result r[RESULTS];
    /* the engine: its process, model, state */
    pid_t pid;
    int dirfd;
    char model[64];
    const char *state;          /* "off", "starting", "ready", "busy" */
    double last_used;
    int idle_ms;
    char model_dir[PATH_MAX], engine[PATH_MAX], launcher[PATH_MAX], dpkg_info[PATH_MAX];
    /* (0.8.9.6.2) the writing job being streamed: its id, the text so far, and the one cancelled */
    int wr_id, cancel_id;
    char *wr_text;
    /* (0.8.9.6.2) where it computes: the person's choice (M_*), the running (or last) engine's backend (LLMJAIL_*),
     * what its log said (the device, integrated or not, the layers on the GPU, out of memory), the GPUs given up on
     * this session (FB_* per backend, until the choice changes), and why the last start didn't use one */
    int mode, mode_gen, run_gen, backend, has_run;
    char device[96];
    int layers, layers_total, oom;
    unsigned log_gen;
    int failed[3];
    int fallback;
    char ggml_dir[PATH_MAX], dev_dir[PATH_MAX];
    /* (0.8.9.6.2) the processor threads: the person's setting (0 automatic), the running (or last) engine's count, and
     * a restart asked for while the engine was idle (llm_set_threads) */
    int threads_set, run_threads, restart_now;
} E = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .pid = -1, .dirfd = -1, .state = "off",
        .idle_ms = LLM_IDLE_MS, .model_dir = LLM_MODEL_DIR, .engine = LLM_ENGINE, .dpkg_info = "/var/lib/dpkg/info",
        .ggml_dir = LLM_GGML_DIR, .dev_dir = "/dev" };

enum { M_AUTO, M_CPU, M_GPU };
enum { FB_NONE_YET, FB_START, FB_MEMORY, FB_UNTESTED, FB_NOGPU };
static const char *const BACKEND_WORD[3] = { "cpu", "vulkan", "cuda" };
static const char *const MODE_WORD[3] = { "auto", "cpu", "gpu" };
static const char *const FALLBACK_WORD[5] = { NULL, "start", "memory", "untested", "none" };

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

void llm_set_paths(const char *model_dir, const char *engine, const char *launcher, const char *dpkg_info) {
    pthread_mutex_lock(&E.mu);
    if (model_dir) snprintf(E.model_dir, sizeof E.model_dir, "%s", model_dir);
    if (engine) snprintf(E.engine, sizeof E.engine, "%s", engine);
    if (launcher) snprintf(E.launcher, sizeof E.launcher, "%s", launcher);
    if (dpkg_info) snprintf(E.dpkg_info, sizeof E.dpkg_info, "%s", dpkg_info);
    pthread_mutex_unlock(&E.mu);
}

void llm_set_idle_ms(int ms) { E.idle_ms = ms > 0 ? ms : LLM_IDLE_MS; }

void llm_set_gpu_paths(const char *ggml_dir, const char *dev_dir) {
    pthread_mutex_lock(&E.mu);
    if (ggml_dir) snprintf(E.ggml_dir, sizeof E.ggml_dir, "%s", ggml_dir);
    if (dev_dir) snprintf(E.dev_dir, sizeof E.dev_dir, "%s", dev_dir);
    pthread_mutex_unlock(&E.mu);
}

int llm_set_backend(const char *mode) {
    int m = -1;
    for (int i = 0; i < 3; i++) if (mode && !strcmp(mode, MODE_WORD[i])) m = i;
    if (m < 0) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&E.mu);
    if (E.mode != m) {
        E.mode = m;
        E.mode_gen++;                       /* the running engine is restarted at the next job */
        memset(E.failed, 0, sizeof E.failed);
        E.fallback = FB_NONE_YET;
    }
    pthread_mutex_unlock(&E.mu);
    return 0;
}

int llm_set_threads(int n) {
    if (n < 0) { errno = EINVAL; return -1; }
    int cpus = llmjail_cpus();
    pthread_mutex_lock(&E.mu);
    int was = llmjail_threads_for(E.threads_set, cpus), now = llmjail_threads_for(n, cpus);
    E.threads_set = n;
    if (was != now) {
        E.mode_gen++;                       /* a running engine is restarted at its next job */
        /* an idle one now, by the engine's thread (a busy one finishes its job first) */
        if (E.pid > 0 && !E.nq && !strcmp(E.state, "ready")) { E.restart_now = 1; pthread_cond_signal(&E.cv); }
    }
    pthread_mutex_unlock(&E.mu);
    return 0;
}

/* ---- what may run ---- */

/* lord-llm next to lord-server's own executable */
static int launcher_path(char *out, size_t n) {
    if (E.launcher[0]) { snprintf(out, n, "%s", E.launcher); return 0; }
    char self[PATH_MAX];
    ssize_t k = readlink("/proc/self/exe", self, sizeof self - 1);
    if (k <= 0) return -1;
    self[k] = 0;
    char *slash = strrchr(self, '/');
    if (!slash) return -1;
    *slash = 0;
    return snprintf(out, n, "%s/lord-llm", self) >= (int)n ? -1 : 0;
}

/* a program owned by root or the user (root only: root_only), writable by no one else, executable */
static int prog_ok(const struct stat *st, int root_only) {
    return S_ISREG(st->st_mode) && (st->st_mode & S_IXUSR) && !(st->st_mode & 022) &&
           (st->st_uid == 0 || (!root_only && st->st_uid == getuid()));
}

int llm_available(void) {
    char l[PATH_MAX];
    struct stat a, b;
    if (launcher_path(l, sizeof l) || stat(l, &a) || !prog_ok(&a, 0)) return 0;
    /* the engine: root's; a test's own copy may be the user's */
    return !stat(E.engine, &b) && prog_ok(&b, strcmp(E.engine, LLM_ENGINE) ? 0 : 1);
}

/* (0.8.9.6.2) a GPU backend can be tried: its ggml module is installed (in the backend folder, owned by root or the user,
 * writable by no one else: llama.cpp loads it from there) and its device is there (a render node for Vulkan, NVIDIA's
 * control node for CUDA). Whether the card really works only the engine's start tells. */
static int module_ok(const char *dir, const char *name) {
    char p[PATH_MAX];
    struct stat st;
    if (snprintf(p, sizeof p, "%s/%s", dir, name) >= (int)sizeof p || stat(p, &st)) return 0;
    return S_ISREG(st.st_mode) && !(st.st_mode & 022) && (st.st_uid == 0 || st.st_uid == getuid());
}

static int has_render_node(const char *dev) {
    char p[PATH_MAX + 8];
    snprintf(p, sizeof p, "%s/dri", dev);
    DIR *d = opendir(p);
    if (!d) return 0;
    int found = 0;
    for (struct dirent *de; !found && (de = readdir(d));) {
        if (strncmp(de->d_name, "renderD", 7) || !de->d_name[7]) continue;
        char q[PATH_MAX + 300];
        struct stat st;
        snprintf(q, sizeof q, "%s/%s", p, de->d_name);
        found = !stat(q, &st) && S_ISCHR(st.st_mode);
    }
    closedir(d);
    return found;
}

static int gpu_avail(int backend) {
    char g[PATH_MAX], dev[PATH_MAX];
    pthread_mutex_lock(&E.mu);
    snprintf(g, sizeof g, "%s", E.ggml_dir);
    snprintf(dev, sizeof dev, "%s", E.dev_dir);
    pthread_mutex_unlock(&E.mu);
    if (backend == LLMJAIL_VULKAN) return module_ok(g, "libggml-vulkan.so") && has_render_node(dev);
    if (backend == LLMJAIL_CUDA) {
        char p[PATH_MAX + 16];
        struct stat st;
        snprintf(p, sizeof p, "%s/nvidiactl", dev);
        return module_ok(g, "libggml-cuda.so") && !stat(p, &st) && S_ISCHR(st.st_mode);
    }
    return 1;
}

int llm_model_id_ok(const char *id) {
    size_t n = id ? strlen(id) : 0;
    if (!n || n > 48 || strstr(id, "..")) return 0;
    if (!((id[0] >= 'a' && id[0] <= 'z') || (id[0] >= '0' && id[0] <= '9'))) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9') || id[i] == '.' || id[i] == '-')) return 0;
    return 1;
}

/* the model file, opened without following a link: a regular file owned by root or the user, writable by no one else */
static int open_model(const char *id, struct stat *st) {
    if (!llm_model_id_ok(id)) { errno = EINVAL; return -1; }
    char p[PATH_MAX];
    if (snprintf(p, sizeof p, "%s/%s.gguf", E.model_dir, id) >= (int)sizeof p) { errno = ENAMETOOLONG; return -1; }
    int fd = open(p, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, st) || !S_ISREG(st->st_mode) || (st->st_mode & 022) || (st->st_uid != 0 && st->st_uid != getuid())) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    return fd;
}

char *llm_models_json(void) {
    char model_dir[PATH_MAX];
    pthread_mutex_lock(&E.mu);
    snprintf(model_dir, sizeof model_dir, "%s", E.model_dir);
    pthread_mutex_unlock(&E.mu);
    size_t cap = 4096, n = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    n += (size_t)snprintf(out, cap, "[");
    int dfd = open(model_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        DIR *d = fdopendir(dfd);
        struct dirent *de;
        int count = 0;
        while (d && (de = readdir(d)) && count < 32) {
            size_t l = strlen(de->d_name);
            if (l < 6 || strcmp(de->d_name + l - 5, ".gguf")) continue;
            char id[64];
            if (l - 5 >= sizeof id) continue;
            snprintf(id, sizeof id, "%.*s", (int)(l - 5), de->d_name);
            struct stat st;
            int fd = open_model(id, &st);
            if (fd < 0) continue;
            close(fd);
            if (n + 128 > cap) break;
            n += (size_t)snprintf(out + n, cap - n, "%s{\"id\":\"%s\",\"bytes\":%lld}", count ? "," : "", id, (long long)st.st_size);
            count++;
        }
        if (d) closedir(d); else close(dfd);
    }
    snprintf(out + n, cap - n, "]");
    return out;
}

/* ---- the engine's process ---- */

/* $XDG_RUNTIME_DIR/lordos/llm: made 0700, opened without following a link, the user's */
static int runtime_dir(void) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || rt[0] != '/') { errno = ENOENT; return -1; }
    int base = open(rt, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (base < 0) return -1;
    mkdirat(base, "lordos", 0700);
    int l = openat(base, "lordos", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(base);
    if (l < 0) return -1;
    mkdirat(l, "llm", 0700);
    int d = openat(l, "llm", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(l);
    if (d < 0) return -1;
    struct stat st;
    if (fstat(d, &st) || st.st_uid != getuid() || ((st.st_mode & 077) && fchmod(d, 0700))) { close(d); errno = EPERM; return -1; }
    return d;
}

static void engine_kill(void) {
    if (E.pid > 0) {
        kill(E.pid, SIGTERM);
        for (int i = 0; i < 50; i++) {
            if (waitpid(E.pid, NULL, WNOHANG) == E.pid) { E.pid = -1; break; }
            struct timespec ts = { 0, 100 * 1000000L };
            nanosleep(&ts, NULL);
        }
        if (E.pid > 0) { kill(E.pid, SIGKILL); waitpid(E.pid, NULL, 0); }
    }
    pthread_mutex_lock(&E.mu);
    E.pid = -1;
    if (E.dirfd >= 0) { unlinkat(E.dirfd, "engine.sock", 0); close(E.dirfd); }
    E.dirfd = -1;
    E.model[0] = 0;
    pthread_mutex_unlock(&E.mu);
}

static int sock_connect(void) {
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "/proc/self/fd/%d/engine.sock", E.dirfd);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    /* only our own engine answers: the process at the other end is the one lord-llm became */
    struct ucred cr;
    socklen_t cl = sizeof cr;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) || cr.pid != E.pid || cr.uid != getuid()) { close(fd); errno = EPERM; return -1; }
    return fd;
}

/* One HTTP/1.1 request with Connection: close; the answer's status and body (malloc'd, at most LLM_ANSWER_MAX). */
static int http(const char *method, const char *path, const char *body, int timeout_ms, int *status, char **out, size_t *outn) {
    *out = NULL;
    *outn = 0;
    int fd = sock_connect();
    if (fd < 0) return -1;
    char head[256];
    size_t bl = body ? strlen(body) : 0;
    int hl = body ? snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                                              "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n", method, path, bl)
                  : snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", method, path);
    const char *parts[2] = { head, body };
    size_t lens[2] = { (size_t)hl, bl };
    for (int i = 0; i < 2; i++) {
        size_t off = 0;
        while (off < lens[i]) {
            ssize_t w = send(fd, parts[i] + off, lens[i] - off, MSG_NOSIGNAL);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { close(fd); return -1; }
            off += (size_t)w;
        }
    }
    size_t cap = 8192, n = 0;
    char *buf = malloc(cap + 1);
    if (!buf) { close(fd); return -1; }
    double end = now_ms() + timeout_ms;
    for (;;) {
        int left = (int)(end - now_ms());
        if (left <= 0) { free(buf); close(fd); errno = ETIMEDOUT; return -1; }
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int pr = poll(&p, 1, left > 1000 ? 1000 : left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr == 0) {
            pthread_mutex_lock(&E.mu);
            int stop = E.stop_now;
            pthread_mutex_unlock(&E.mu);
            if (stop) { free(buf); close(fd); errno = ECANCELED; return -1; }
            continue;
        }
        if (n == cap) {
            if (cap >= LLM_ANSWER_MAX) { free(buf); close(fd); errno = EMSGSIZE; return -1; }
            cap *= 2;
            char *nb = realloc(buf, cap + 1);
            if (!nb) { free(buf); close(fd); return -1; }
            buf = nb;
        }
        ssize_t r = recv(fd, buf + n, cap - n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { free(buf); close(fd); return -1; }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    /* status line, headers, body (Content-Length or chunked) */
    int st = 0;
    if (sscanf(buf, "HTTP/1.%*d %d", &st) != 1) { free(buf); errno = EPROTO; return -1; }
    char *sep = strstr(buf, "\r\n\r\n");
    if (!sep) { free(buf); errno = EPROTO; return -1; }
    *sep = 0;
    char *b = sep + 4;
    size_t blen = n - (size_t)(b - buf);
    int chunked = 0;
    for (char *h = strstr(buf, "\r\n"); h; h = strstr(h + 2, "\r\n")) {
        if (!strncasecmp(h + 2, "Transfer-Encoding:", 18) && strstr(h + 2, "chunked")) chunked = 1;
    }
    char *res = malloc(blen + 1);
    if (!res) { free(buf); return -1; }
    size_t rn = 0;
    if (chunked) {
        char *p = b, *e = b + blen;
        while (p < e) {
            char *end;
            unsigned long k = strtoul(p, &end, 16);
            char *crlf = strstr(p, "\r\n");
            if (end == p || !crlf || crlf >= e) break;
            p = crlf + 2;
            if (!k) break;
            if (k > (unsigned long)(e - p)) { free(res); free(buf); errno = EPROTO; return -1; }
            memcpy(res + rn, p, k);
            rn += k;
            p += k + 2;
        }
    } else {
        memcpy(res, b, blen);
        rn = blen;
    }
    res[rn] = 0;
    free(buf);
    *status = st;
    *out = res;
    *outn = rn;
    return 0;
}

/* (0.8.9.6.2) the engine's log (its stderr, a pipe): read on a thread of its own for as long as the engine runs, so it
 * never blocks on a full pipe. Only a few lines are understood, as untrusted text: the device's name (printable ASCII,
 * up to its first " (", at most 80 characters), the layers put on the GPU, and the GPU running out of memory. With LORD_LLM_LOG=1 every line is copied to
 * lord-server's stderr. */
static void device_name(const char *s, const char *end) {
    char n[sizeof E.device];
    size_t k = 0;
    int open_ = 0, close_ = 0;
    for (; s < end && *s && k < 80; s++) {
        if (s[0] == ' ' && s[1] == '(') break;
        if (*s >= 0x20 && *s < 0x7f && *s != '"' && *s != '\\') {
            n[k++] = *s;
            open_ += *s == '(';
            close_ += *s == ')';
        }
    }
    /* "Intel(R) UHD Graphics 630" keeps its own parentheses; the one closing the log's "(name)" goes */
    while (k && (n[k - 1] == ' ' || (n[k - 1] == ')' && close_ > open_))) close_ -= n[--k] == ')';
    n[k] = 0;
    snprintf(E.device, sizeof E.device, "%s", n);
}

/* "using device Vulkan0 (AMD Radeon RX 580 Series (RADV POLARIS10)) (0000:01:00.0) - 7726 MiB free",
 * "offloaded 29/29 layers to GPU", and the GPU's allocation failures */
static void log_line(const char *l, unsigned gen) {
    pthread_mutex_lock(&E.mu);
    if (gen != E.log_gen) { pthread_mutex_unlock(&E.mu); return; }
    const char *v;
    static const char *const dev0[3] = { NULL, "using device Vulkan0 (", "using device CUDA0 (" };
    if (E.backend != LLMJAIL_CPU && (v = strstr(l, dev0[E.backend]))) {
        v += strlen(dev0[E.backend]);
        device_name(v, v + strlen(v));
    }
    if ((v = strstr(l, "offloaded "))) {
        int a = -1, b = -1;
        if (sscanf(v, "offloaded %d/%d layers to GPU", &a, &b) == 2 && a >= 0 && b > 0 && a <= b && b < 10000) { E.layers = a; E.layers_total = b; }
    }
    if (E.backend != LLMJAIL_CPU && (strstr(l, "ErrorOutOfDeviceMemory") || strstr(l, "out of memory") || strstr(l, "failed to allocate")))
        E.oom = 1;
    pthread_mutex_unlock(&E.mu);
}

typedef struct { int fd; unsigned gen; } LogArg;

static void *log_reader(void *arg) {
    LogArg a = *(LogArg *)arg;
    free(arg);
    const char *lg = getenv("LORD_LLM_LOG");
    int echo = lg && !strcmp(lg, "1");
    char buf[4096], line[1024];
    size_t ln = 0;
    for (;;) {
        ssize_t n = read(a.fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != '\n') { if (ln < sizeof line - 1) line[ln++] = buf[i]; continue; }
            line[ln] = 0;
            log_line(line, a.gen);
            if (echo) fprintf(stderr, "llama-server: %s\n", line);
            ln = 0;
        }
    }
    close(a.fd);
    return NULL;
}

static int start_log(int fd) {
    LogArg *a = malloc(sizeof *a);
    if (!a) return -1;
    pthread_mutex_lock(&E.mu);
    a->fd = fd;
    a->gen = ++E.log_gen;
    pthread_mutex_unlock(&E.mu);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 64 * 1024);
    int r = pthread_create(&th, &at, log_reader, a);
    pthread_attr_destroy(&at);
    if (r) { free(a); return -1; }
    return 0;
}

static int spawn_engine(const char *model, int backend, char **err) {
    struct stat ms, es;
    char l[PATH_MAX];
    *err = "llm.noEngine";
    if (!llm_available() || launcher_path(l, sizeof l)) return -1;
    int mfd = open_model(model, &ms);
    if (mfd < 0) { *err = "llm.noModel"; return -1; }
    int efd = open(E.engine, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (efd < 0 || fstat(efd, &es) || !prog_ok(&es, strcmp(E.engine, LLM_ENGINE) ? 0 : 1)) {
        if (efd >= 0) close(efd);
        close(mfd);
        return -1;
    }
    int dfd = runtime_dir();
    int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    int lp[2] = { -1, -1 };
    if (dfd < 0 || devnull < 0 || pipe2(lp, O_CLOEXEC)) {
        *err = "llm.start";
        if (dfd >= 0) close(dfd);
        if (devnull >= 0) close(devnull);
        close(efd);
        close(mfd);
        return -1;
    }
    unlinkat(dfd, "engine.sock", 0);
    /* every source above the targets, so one dup2 never overwrites another's source */
    int *fds[] = { &mfd, &dfd, &efd, &devnull, &lp[1] };
    for (int i = 0; i < 5; i++) {
        int hi = fcntl(*fds[i], F_DUPFD_CLOEXEC, 10);
        if (hi < 0) { close(mfd); close(dfd); close(efd); close(devnull); close(lp[0]); close(lp[1]); *err = "llm.start"; return -1; }
        close(*fds[i]);
        *fds[i] = hi;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, devnull, 0);
    posix_spawn_file_actions_adddup2(&fa, devnull, 1);
    posix_spawn_file_actions_adddup2(&fa, lp[1], 2);          /* the engine's log, read by log_reader */
    posix_spawn_file_actions_adddup2(&fa, mfd, LLMJAIL_MODEL_FD);
    posix_spawn_file_actions_adddup2(&fa, dfd, LLMJAIL_DIR_FD);
    posix_spawn_file_actions_adddup2(&fa, efd, LLMJAIL_ENGINE_FD);
    posix_spawn_file_actions_addclosefrom_np(&fa, LLMJAIL_ENGINE_FD + 1);   /* nothing of lord-server's (DRM, sockets) */
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setpgroup(&at, 0);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
    char *envp[5];
    int ne = 0;
    if (getenv("LORDOS_SESSIOND")) envp[ne++] = "LORDOS_SESSIOND=1";      /* cgroup_enter asks lord-sessiond */
    static const char *const be_env[3] = { "LORD_LLM_BACKEND=cpu", "LORD_LLM_BACKEND=vulkan", "LORD_LLM_BACKEND=cuda" };
    envp[ne++] = (char *)be_env[backend];
    /* the processor threads, a number lord-llm checks again (1..its online cores) */
    pthread_mutex_lock(&E.mu);
    int threads = llmjail_threads_for(E.threads_set, llmjail_cpus());
    pthread_mutex_unlock(&E.mu);
    char th_env[32];
    snprintf(th_env, sizeof th_env, "LORD_LLM_THREADS=%d", threads);
    envp[ne++] = th_env;
    envp[ne] = NULL;
    char *argv[] = { "lord-llm", NULL };
    pid_t pid;
    int r = posix_spawn(&pid, l, &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(devnull);
    close(efd);
    close(mfd);
    close(lp[1]);
    if (r) { close(lp[0]); close(dfd); *err = "llm.start"; return -1; }
    pthread_mutex_lock(&E.mu);
    E.pid = pid;
    E.dirfd = dfd;
    snprintf(E.model, sizeof E.model, "%s", model);
    E.backend = backend;
    E.run_threads = threads;
    E.has_run = 1;
    E.run_gen = E.mode_gen;
    E.device[0] = 0;
    E.layers = E.layers_total = 0;
    E.oom = 0;
    pthread_mutex_unlock(&E.mu);
    if (start_log(lp[0])) {                                 /* nobody would read its log: not started */
        close(lp[0]);
        engine_kill();
        *err = "llm.start";
        return -1;
    }
    return 0;
}

/* waits for the engine's /health; 0 ready */
static int wait_ready(char **err) {
    double end = now_ms() + LLM_START_MS;
    while (now_ms() < end) {
        int stt;
        if (waitpid(E.pid, &stt, WNOHANG) == E.pid) { E.pid = -1; *err = "llm.start"; return -1; }
        int status = 0;
        char *b = NULL;
        size_t bn = 0;
        if (!http("GET", "/health", NULL, 5000, &status, &b, &bn)) {
            int ok = status == 200;
            free(b);
            if (ok) return 0;
        }
        pthread_mutex_lock(&E.mu);
        int stop = E.stop_now;
        pthread_mutex_unlock(&E.mu);
        if (stop) { *err = "llm.stopped"; return -1; }
        struct timespec ts = { 0, 250 * 1000000L };
        nanosleep(&ts, NULL);
    }
    *err = "llm.timeout";
    return -1;
}

/* ---- jobs ---- */

/* the request: the texts from Sentinela, everything else fixed here */
static const char SCHEMA[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"verdict\":{\"type\":\"string\",\"enum\":[\"normal\",\"suspicious\",\"dangerous\"]},"
    "\"reason\":{\"type\":\"string\",\"maxLength\":400},"
    "\"action\":{\"type\":\"string\",\"enum\":[\"end_block\",\"pause\",\"limit\",\"monitor\",\"ignore\"]}},"
    "\"required\":[\"verdict\",\"reason\",\"action\"],\"additionalProperties\":false}";

/* (0.8.9.6) "Investigate with Lumo": what the program does, why it probably runs, a risk and one action of the closed list */
static const char SCHEMA_INVESTIGATE[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"what\":{\"type\":\"string\",\"maxLength\":400},"
    "\"why\":{\"type\":\"string\",\"maxLength\":300},"
    "\"risk\":{\"type\":\"string\",\"enum\":[\"low\",\"medium\",\"high\"]},"
    "\"action\":{\"type\":\"string\",\"enum\":[\"none\",\"end\",\"pause\",\"block_network\",\"blocklist\"]}},"
    "\"required\":[\"what\",\"why\",\"risk\",\"action\"],\"additionalProperties\":false}";
/* (0.8.9.6) a description translated into the person's language */
static const char SCHEMA_TRANSLATE[] =
    "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"maxLength\":600}},"
    "\"required\":[\"text\"],\"additionalProperties\":false}";

static char *request_body(const char *system, const char *user, int kind) {
    const char *schema = kind == J_INVESTIGATE ? SCHEMA_INVESTIGATE : kind == J_TRANSLATE ? SCHEMA_TRANSLATE : SCHEMA;
    int max_tokens = kind == J_INVESTIGATE ? 480 : kind == J_TRANSLATE ? 360 : 256;
    size_t cap = strlen(system) * 6 + strlen(user) * 6 + strlen(schema) + 512;
    char *b = malloc(cap), *qs = malloc(strlen(system) * 6 + 3), *qu = malloc(strlen(user) * 6 + 3);
    if (!b || !qs || !qu) { free(b); free(qs); free(qu); return NULL; }
    json_quote(qs, strlen(system) * 6 + 3, system);
    json_quote(qu, strlen(user) * 6 + 3, user);
    snprintf(b, cap, "{\"messages\":[{\"role\":\"system\",\"content\":%s},{\"role\":\"user\",\"content\":%s}],"
                     "\"max_tokens\":%d,\"temperature\":0.2,\"cache_prompt\":false,\"stream\":false,"
                     "\"chat_template_kwargs\":{\"enable_thinking\":false},"
                     "\"response_format\":{\"type\":\"json_schema\",\"json_schema\":{\"schema\":%s}}}", qs, qu, max_tokens, schema);
    free(qs);
    free(qu);
    return b;
}

static char *errjson(const char *code) {
    char *o = malloc(96);
    if (o) snprintf(o, 96, "{\"ok\":false,\"error\":\"%s\"}", code);
    return o;
}

/* (0.8.9.6.2) the backend the next start uses: the processor when chosen; else CUDA, then Vulkan, each only when
 * installed, its device there and not given up on this session; else the processor (fallback says why) */
static int pick_backend(void) {
    pthread_mutex_lock(&E.mu);
    int mode = E.mode, fc = E.failed[LLMJAIL_CUDA], fv = E.failed[LLMJAIL_VULKAN];
    pthread_mutex_unlock(&E.mu);
    if (mode == M_CPU) return LLMJAIL_CPU;
    int ac = gpu_avail(LLMJAIL_CUDA), av = gpu_avail(LLMJAIL_VULKAN);
    if (ac && !fc) return LLMJAIL_CUDA;
#if defined(__aarch64__)
    /* the Raspberry Pi's GPU (V3D) isn't known to run the engine well: "auto" keeps the processor there, "gpu" tries it */
    if (mode == M_AUTO && av && !fv) {
        pthread_mutex_lock(&E.mu);
        E.fallback = FB_UNTESTED;
        pthread_mutex_unlock(&E.mu);
        return LLMJAIL_CPU;
    }
#endif
    if (av && !fv) return LLMJAIL_VULKAN;
    /* the processor: say why (no GPU, or the one given up on and its reason) */
    pthread_mutex_lock(&E.mu);
    E.fallback = av ? fv : ac ? fc : FB_NOGPU;
    pthread_mutex_unlock(&E.mu);
    return LLMJAIL_CPU;
}

static void give_up(int backend, int why) {
    pthread_mutex_lock(&E.mu);
    if (backend != LLMJAIL_CPU) E.failed[backend] = why;
    E.fallback = why;
    pthread_mutex_unlock(&E.mu);
}

/* the engine with this job's model, started when it isn't running (or the choice of where changed); NULL, or the
 * error's JSON. A GPU that can't start it is given up on and the next backend is tried, the processor last. */
static char *ensure_engine(Job *j, double *started) {
    char *err = "llm.start";
    pthread_mutex_lock(&E.mu);
    int same = E.pid > 0 && !strcmp(E.model, j->model) && E.run_gen == E.mode_gen;
    pthread_mutex_unlock(&E.mu);
    if (same) return NULL;
    engine_kill();
    pthread_mutex_lock(&E.mu);
    E.state = "starting";
    E.fallback = FB_NONE_YET;
    pthread_mutex_unlock(&E.mu);
    for (;;) {
        int be = pick_backend();
        err = "llm.start";
        if (!spawn_engine(j->model, be, &err) && !wait_ready(&err)) {
            if (be == LLMJAIL_CPU) break;
            /* the log's line about the layers comes before the engine answers; its reader may lag */
            int layers = -1;
            for (int i = 0; i < 20 && layers < 0; i++) {
                pthread_mutex_lock(&E.mu);
                layers = E.layers_total ? E.layers : -1;
                pthread_mutex_unlock(&E.mu);
                if (layers < 0) { struct timespec ts = { 0, 25 * 1000000L }; nanosleep(&ts, NULL); }
            }
            /* a card with no room for a single layer isn't worth it */
            if (layers != 0) break;
            engine_kill();
            give_up(be, FB_MEMORY);
            continue;
        }
        engine_kill();
        if (be == LLMJAIL_CPU || !strcmp(err, "llm.stopped") || !strcmp(err, "llm.noModel") || !strcmp(err, "llm.noEngine")) {
            pthread_mutex_lock(&E.mu);
            E.state = "off";
            pthread_mutex_unlock(&E.mu);
            return errjson(err);
        }
        give_up(be, FB_START);
    }
    *started = now_ms();
    return NULL;
}

/* (0.8.9.6.2) after a request failed: a GPU engine that died or ran out of the card's memory is given up on (the
 * caller runs the job once more, on the next backend); 1 when that happened */
static int gpu_gave_up(void) {
    pthread_mutex_lock(&E.mu);
    int be = E.backend, oom = E.oom;
    pid_t pid = E.pid;
    pthread_mutex_unlock(&E.mu);
    if (be == LLMJAIL_CPU || pid <= 0) return 0;
    int dead = 0;
    for (int i = 0; i < 10 && !dead; i++) {
        dead = waitpid(pid, NULL, WNOHANG) == pid;
        if (!dead) { struct timespec ts = { 0, 50 * 1000000L }; nanosleep(&ts, NULL); }
    }
    /* the out-of-memory line comes through the log's reader, which may lag the death: a moment more for it */
    for (int i = 0; i < 10 && !oom; i++) {
        pthread_mutex_lock(&E.mu);
        oom = E.oom;
        pthread_mutex_unlock(&E.mu);
        if (!oom && dead) { struct timespec ts = { 0, 25 * 1000000L }; nanosleep(&ts, NULL); }
        else break;
    }
    if (!dead && !oom) return 0;
    if (dead) {
        pthread_mutex_lock(&E.mu);
        E.pid = -1;
        pthread_mutex_unlock(&E.mu);
    }
    engine_kill();
    give_up(be, oom ? FB_MEMORY : FB_START);
    return 1;
}

static char *run_analyze(Job *j) {
    for (int attempt = 0;; attempt++) {
        double t0 = now_ms(), started = t0;
        char *e = ensure_engine(j, &started);
        if (e) return e;
        pthread_mutex_lock(&E.mu);
        E.state = "busy";
        pthread_mutex_unlock(&E.mu);
        char *body = request_body(j->system, j->user, j->kind);
        int status = 0;
        char *ans = NULL;
        size_t an = 0;
        int r = body ? http("POST", "/v1/chat/completions", body, LLM_REPLY_MS, &status, &ans, &an) : -1;
        int timed_out = r && errno == ETIMEDOUT;
        free(body);
        pthread_mutex_lock(&E.mu);
        E.state = "ready";
        E.last_used = now_ms();
        pthread_mutex_unlock(&E.mu);
        if (r || status != 200) {
            free(ans);
            if (timed_out) engine_kill();
            else if (!attempt && gpu_gave_up()) continue;      /* the GPU failed it: once more on the next backend */
            return errjson(timed_out ? "llm.timeout" : "llm.answer");
        }
        JDoc d;
        char *out = NULL;
        if (json_parse(ans, an, &d, NULL, 0)) { free(ans); return errjson("llm.answer"); }
        const JVal *ch = json_get(d.root, "choices");
        const JVal *c0 = ch && ch->type == J_ARR ? ch->kid : NULL;
        const JVal *msg = c0 ? json_get(c0, "message") : NULL;
        const char *content = msg ? json_str(msg, "content") : NULL;
        const JVal *usage = json_get(d.root, "usage");
        int tokens = usage ? (int)json_num(usage, "completion_tokens", 0) : 0;
        if (content && strlen(content) <= LLM_CONTENT_MAX) {
            size_t cap = strlen(content) * 6 + 160;
            out = malloc(cap);
            if (out) {
                int k = snprintf(out, cap, "{\"ok\":true,\"ms\":%.0f,\"startMs\":%.0f,\"tokens\":%d,\"content\":", now_ms() - t0, started - t0, tokens);
                size_t q = json_quote(out + k, cap - (size_t)k - 2, content);
                snprintf(out + k + q, cap - (size_t)k - q, "}");
            }
        }
        json_free(&d);
        free(ans);
        return out ? out : errjson("llm.answer");
    }
}

/* the person's click: the facts from JS, then what evidence.c reads of the program (fenced as data), then the model;
 * the result says what was read ("evidence") */
static char *run_investigate(Job *j) {
    char *block = NULL;
    char *meta = llm_evidence(j->pid, j->start, &block);
    if (!meta) { free(block); return errjson("llm.failed"); }
    if (strstr(meta, "\"why\":\"gone\"")) { free(meta); free(block); return errjson("proc.gone"); }
    if (block) {
        size_t n = strlen(j->user) + strlen(block) + 2;
        char *u = malloc(n);
        if (!u) { free(meta); free(block); return errjson("llm.failed"); }
        snprintf(u, n, "%s\n%s", j->user, block);
        free(j->user);
        j->user = u;
    }
    free(block);
    char *r = run_analyze(j);
    if (!r) { free(meta); return NULL; }
    size_t rl = strlen(r), n = rl + strlen(meta) + 16;
    char *o = rl && r[rl - 1] == '}' ? malloc(n) : NULL;
    if (o) snprintf(o, n, "%.*s,\"evidence\":%s}", (int)(rl - 1), r, meta);
    free(meta);
    if (!o) return r;
    free(r);
    return o;
}

/* (0.8.9.6.2) "✨ AI" on selected text: the fixed request (writing.c), the answer streamed into E.wr_text so the shell
 * shows it as it comes (llm_peek); cancelled by llm_cancel or llm_stop */
static char *run_write_once(Job *j, int attempt) {
    double t0 = now_ms(), started = t0;
    char *e = ensure_engine(j, &started);
    if (e) return e;
    uint8_t r[8] = { 0 };
    char nonce[17];
    if (getrandom(r, sizeof r, 0) != (ssize_t)sizeof r) return errjson("llm.failed");
    for (int i = 0; i < 8; i++) snprintf(nonce + 2 * i, 3, "%02x", r[i]);
    char *body = wr_request(j->action, j->target, j->user, nonce);
    if (!body) return errjson("llm.failed");
    pthread_mutex_lock(&E.mu);
    E.state = "busy";
    pthread_mutex_unlock(&E.mu);
    int fd = sock_connect();
    char head[256];
    size_t bl = strlen(body);
    int hl = snprintf(head, sizeof head, "POST /v1/chat/completions HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                                         "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n", bl);
    int ok = fd >= 0;
    const char *parts[2] = { head, body };
    size_t lens[2] = { (size_t)hl, bl };
    for (int i = 0; ok && i < 2; i++)
        for (size_t off = 0; ok && off < lens[i];) {
            ssize_t w = send(fd, parts[i] + off, lens[i] - off, MSG_NOSIGNAL);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) ok = 0; else off += (size_t)w;
        }
    free(body);
    WrStream *st = malloc(sizeof *st);
    const char *why = ok && st ? NULL : "llm.answer";
    if (st) wr_stream_init(st);
    double end = now_ms() + LLM_REPLY_MS;
    size_t got = 0;
    char buf[8192];
    while (!why) {
        int left = (int)(end - now_ms());
        if (left <= 0) { why = "llm.timeout"; break; }
        pthread_mutex_lock(&E.mu);
        int stop = E.stop_now || E.cancel_id == j->id;
        pthread_mutex_unlock(&E.mu);
        if (stop) { why = "llm.cancelled"; break; }
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int pr = poll(&p, 1, left > 250 ? 250 : left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) continue;
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!st->done && !(st->phase == 4 && st->tn)) why = "llm.answer"; break; }
        got += (size_t)n;
        if (got > LLM_ANSWER_MAX * 4) { why = "llm.answer"; break; }
        int fr = wr_stream_feed(st, buf, (size_t)n);
        pthread_mutex_lock(&E.mu);
        char *copy = strdup(st->text);
        if (copy) { free(E.wr_text); E.wr_text = copy; }
        pthread_mutex_unlock(&E.mu);
        if (fr < 0) why = "llm.answer";
        if (fr > 0) break;
    }
    if (fd >= 0) close(fd);
    pthread_mutex_lock(&E.mu);
    E.state = "ready";
    E.last_used = now_ms();
    if (E.cancel_id == j->id) E.cancel_id = 0;
    pthread_mutex_unlock(&E.mu);
    if (why && !strcmp(why, "llm.timeout")) engine_kill();
    /* the GPU failed it before a word came: once more on the next backend (run_write) */
    if (why && !attempt && strcmp(why, "llm.timeout") && strcmp(why, "llm.cancelled") && st && !st->tn && gpu_gave_up()) {
        free(st);
        return NULL;
    }
    char *out = NULL;
    if (!why && st) {
        size_t cap = st->tn * 6 + 160;
        out = malloc(cap);
        if (out) {
            int k = snprintf(out, cap, "{\"ok\":true,\"ms\":%.0f,\"startMs\":%.0f,\"truncated\":%s,\"content\":", now_ms() - t0,
                             started - t0, st->truncated ? "true" : "false");
            size_t q = json_quote(out + k, cap - (size_t)k - 2, st->text);
            snprintf(out + k + q, cap - (size_t)k - q, "}");
        }
    }
    free(st);
    return out ? out : errjson(why ? why : "llm.answer");
}

static char *run_write(Job *j) {
    char *r = run_write_once(j, 0);
    return r ? r : run_write_once(j, 1);
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&E.mu);
        while (!E.nq) {
            /* (0.8.9.6.2) the thread count changed while the engine was idle: started again with the new one now, so
             * the next job doesn't wait for it; its idle time keeps counting from its last answer */
            if (E.restart_now) {
                E.restart_now = 0;
                if (E.pid > 0 && !E.stop_now && !strcmp(E.state, "ready") && E.run_gen != E.mode_gen && E.model[0]) {
                    Job rj = { .kind = J_ANALYZE };
                    snprintf(rj.model, sizeof rj.model, "%s", E.model);
                    double used = E.last_used, started = 0;
                    pthread_mutex_unlock(&E.mu);
                    char *e = ensure_engine(&rj, &started);
                    pthread_mutex_lock(&E.mu);
                    if (e) free(e);                 /* "off" now: the next job tries again */
                    else { E.state = "ready"; E.last_used = used; }
                    continue;
                }
            }
            if (E.stop_now || (E.pid > 0 && now_ms() - E.last_used > E.idle_ms && !strcmp(E.state, "ready"))) {
                E.stop_now = 0;
                pthread_mutex_unlock(&E.mu);
                engine_kill();
                pthread_mutex_lock(&E.mu);
                E.state = "off";
                continue;
            }
            if (E.pid > 0) {
                /* the engine may have died by itself */
                if (waitpid(E.pid, NULL, WNOHANG) == E.pid) {
                    E.pid = -1;
                    pthread_mutex_unlock(&E.mu);
                    engine_kill();
                    pthread_mutex_lock(&E.mu);
                    E.state = "off";
                    continue;
                }
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += 1;
                pthread_cond_timedwait(&E.cv, &E.mu, &ts);
            } else {
                pthread_cond_wait(&E.cv, &E.mu);
            }
        }
        Job j = E.q[0];
        memmove(E.q, E.q + 1, sizeof E.q[0] * (size_t)(--E.nq));
        E.stop_now = 0;
        char info[PATH_MAX];
        snprintf(info, sizeof info, "%s", E.dpkg_info);
        pthread_mutex_unlock(&E.mu);
        char *res = NULL;
        if (j.kind == J_WRITE) {
            pthread_mutex_lock(&E.mu);
            E.wr_id = j.id;
            free(E.wr_text);
            E.wr_text = strdup("");
            pthread_mutex_unlock(&E.mu);
            res = run_write(&j);
            pthread_mutex_lock(&E.mu);
            E.wr_id = 0;
            pthread_mutex_unlock(&E.mu);
        } else if (j.kind == J_ANALYZE || j.kind == J_TRANSLATE) res = run_analyze(&j);
        else if (j.kind == J_INVESTIGATE) res = run_investigate(&j);
        else res = llm_facts_json(j.pid, j.start, info, getenv("HOME"));
        if (!res) res = errjson("llm.failed");
        free(j.system);
        free(j.user);
        pthread_mutex_lock(&E.mu);
        int slot = 0;
        for (int i = 0; i < RESULTS; i++) if (!E.r[i].id) { slot = i; break; } else if (E.r[i].id < E.r[slot].id) slot = i;
        free(E.r[slot].json);                    /* the oldest one never taken */
        E.r[slot].id = j.id;
        E.r[slot].json = res;
        pthread_mutex_unlock(&E.mu);
    }
    return NULL;
}

static int submit(Job *j) {
    pthread_mutex_lock(&E.mu);
    if (!E.started) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 512 * 1024);
        if (!pthread_create(&th, &at, worker, NULL)) E.started = 1;
        pthread_attr_destroy(&at);
        if (!E.started) { pthread_mutex_unlock(&E.mu); errno = EAGAIN; return -1; }
    }
    if (E.nq >= QUEUE) { pthread_mutex_unlock(&E.mu); errno = EBUSY; return -1; }
    if (++E.next_id <= 0) E.next_id = 1;
    j->id = E.next_id;
    E.q[E.nq++] = *j;
    pthread_cond_signal(&E.cv);
    pthread_mutex_unlock(&E.mu);
    return j->id;
}

static int valid_text(const char *s, size_t max) {
    size_t n = s ? strlen(s) : 0;
    return n > 0 && n <= max;
}

int llm_submit_analyze(const char *model, const char *system, const char *user) {
    if (!llm_model_id_ok(model) || !valid_text(system, LLM_SYSTEM_MAX) || !valid_text(user, LLM_USER_MAX)) { errno = EINVAL; return -1; }
    Job j = { .kind = J_ANALYZE };
    snprintf(j.model, sizeof j.model, "%s", model);
    j.system = strdup(system);
    j.user = strdup(user);
    if (!j.system || !j.user) { free(j.system); free(j.user); errno = ENOMEM; return -1; }
    int id = submit(&j);
    if (id < 0) { free(j.system); free(j.user); }
    return id;
}

int llm_submit_investigate(const char *model, const char *system, const char *user, int pid, unsigned long long start) {
    if (pid <= 0 || !llm_model_id_ok(model) || !valid_text(system, LLM_SYSTEM_MAX) || !valid_text(user, LLM_USER_MAX)) { errno = EINVAL; return -1; }
    Job j = { .kind = J_INVESTIGATE, .pid = pid, .start = start };
    snprintf(j.model, sizeof j.model, "%s", model);
    j.system = strdup(system);
    j.user = strdup(user);
    if (!j.system || !j.user) { free(j.system); free(j.user); errno = ENOMEM; return -1; }
    int id = submit(&j);
    if (id < 0) { free(j.system); free(j.user); }
    return id;
}

int llm_submit_translate(const char *model, const char *system, const char *text) {
    if (!llm_model_id_ok(model) || !valid_text(system, LLM_SYSTEM_MAX) || !valid_text(text, LLM_TRANSLATE_MAX)) { errno = EINVAL; return -1; }
    Job j = { .kind = J_TRANSLATE };
    snprintf(j.model, sizeof j.model, "%s", model);
    j.system = strdup(system);
    j.user = strdup(text);
    if (!j.system || !j.user) { free(j.system); free(j.user); errno = ENOMEM; return -1; }
    int id = submit(&j);
    if (id < 0) { free(j.system); free(j.user); }
    return id;
}

int llm_submit_write(const char *model, const char *action, const char *target, const char *text) {
    int a = wr_action(action);
    size_t tl = text ? strlen(text) : 0;
    if (!llm_model_id_ok(model) || !a || tl == 0 || tl > LLM_WRITE_MAX || (a == WR_TRANSLATE && !wr_language(target))) { errno = EINVAL; return -1; }
    Job j = { .kind = J_WRITE, .action = a };
    snprintf(j.model, sizeof j.model, "%s", model);
    if (a == WR_TRANSLATE) snprintf(j.target, sizeof j.target, "%s", target);
    j.user = strdup(text);
    if (!j.user) { errno = ENOMEM; return -1; }
    int id = submit(&j);
    if (id < 0) free(j.user);
    return id;
}

char *llm_peek(int id) {
    char *o = NULL;
    pthread_mutex_lock(&E.mu);
    int queued = 0;
    for (int i = 0; i < E.nq; i++) if (E.q[i].id == id) queued = 1;
    if (id > 0 && E.wr_id == id) {
        const char *t = E.wr_text ? E.wr_text : "";
        size_t cap = strlen(t) * 6 + 48;
        if ((o = malloc(cap))) {
            int k = snprintf(o, cap, "{\"state\":\"writing\",\"text\":");
            size_t q = json_quote(o + k, cap - (size_t)k - 2, t);
            snprintf(o + k + q, cap - (size_t)k - q, "}");
        }
    } else if (queued) o = strdup(strcmp(E.state, "off") && strcmp(E.state, "starting") ? "{\"state\":\"queued\"}" : "{\"state\":\"starting\"}");
    pthread_mutex_unlock(&E.mu);
    return o;
}

void llm_cancel(int id) {
    pthread_mutex_lock(&E.mu);
    for (int i = 0; i < E.nq; i++)
        if (E.q[i].id == id) {
            free(E.q[i].system);
            free(E.q[i].user);
            memmove(E.q + i, E.q + i + 1, sizeof E.q[0] * (size_t)(E.nq - i - 1));
            E.nq--;
            break;
        }
    if (id > 0 && E.wr_id == id) E.cancel_id = id;
    pthread_mutex_unlock(&E.mu);
}

int llm_submit_facts(int pid, unsigned long long start) {
    if (pid <= 0) { errno = EINVAL; return -1; }
    Job j = { .kind = J_FACTS, .pid = pid, .start = start };
    return submit(&j);
}

char *llm_take(int id) {
    char *r = NULL;
    pthread_mutex_lock(&E.mu);
    for (int i = 0; i < RESULTS; i++) if (E.r[i].id == id && id > 0) { r = E.r[i].json; E.r[i].json = NULL; E.r[i].id = 0; break; }
    pthread_mutex_unlock(&E.mu);
    return r;
}

char *llm_state_json(void) {
    int gv = gpu_avail(LLMJAIL_VULKAN), gc = gpu_avail(LLMJAIL_CUDA), cpus = llmjail_cpus();
    char *o = malloc(1024);
    if (!o) return NULL;
    pthread_mutex_lock(&E.mu);
    double idle = E.pid > 0 && E.last_used > 0 ? now_ms() - E.last_used : 0;
    int n;
    if (E.model[0]) n = snprintf(o, 1024, "{\"state\":\"%s\",\"model\":\"%s\",\"pid\":%d,\"idleMs\":%.0f,\"queued\":%d", E.state, E.model, (int)E.pid, idle, E.nq);
    else n = snprintf(o, 1024, "{\"state\":\"%s\",\"model\":null,\"pid\":%d,\"idleMs\":0,\"queued\":%d", E.state, (int)E.pid, E.nq);
    n += snprintf(o + n, 1024 - (size_t)n, ",\"mode\":\"%s\",\"gpus\":{\"vulkan\":%s,\"cuda\":%s}", MODE_WORD[E.mode],
                  gv ? "true" : "false", gc ? "true" : "false");
    n += snprintf(o + n, 1024 - (size_t)n, ",\"cpus\":%d,\"threadsAuto\":%d,\"threads\":%d,\"runThreads\":%d", cpus,
                  llmjail_threads_auto(cpus), llmjail_threads_for(E.threads_set, cpus), E.has_run ? E.run_threads : 0);
    if (E.has_run) {
        n += snprintf(o + n, 1024 - (size_t)n, ",\"backend\":\"%s\",\"device\":", BACKEND_WORD[E.backend]);
        if (E.backend != LLMJAIL_CPU && E.device[0]) n += (int)json_quote(o + n, 1024 - (size_t)n, E.device);
        else n += snprintf(o + n, 1024 - (size_t)n, "null");
        n += snprintf(o + n, 1024 - (size_t)n, ",\"layers\":%d,\"layersTotal\":%d", E.layers, E.layers_total);
    } else {
        n += snprintf(o + n, 1024 - (size_t)n, ",\"backend\":null,\"device\":null,\"layers\":0,\"layersTotal\":0");
    }
    if (E.fallback) snprintf(o + n, 1024 - (size_t)n, ",\"fallback\":\"%s\"}", FALLBACK_WORD[E.fallback]);
    else snprintf(o + n, 1024 - (size_t)n, ",\"fallback\":null}");
    pthread_mutex_unlock(&E.mu);
    return o;
}

void llm_stop(void) {
    pthread_mutex_lock(&E.mu);
    for (int i = 0; i < E.nq; i++) { free(E.q[i].system); free(E.q[i].user); }
    E.nq = 0;
    E.stop_now = 1;
    pthread_cond_signal(&E.cv);
    pthread_mutex_unlock(&E.mu);
}
