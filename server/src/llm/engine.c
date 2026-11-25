/* engine.c — Sentinela's local model engine, started on demand in its sandbox (see engine.h and docs/SENTINELA.md). */
#include "engine.h"
#include "llmjail.h"
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
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define QUEUE 8
#define RESULTS 16

enum { J_ANALYZE = 1, J_FACTS, J_INVESTIGATE, J_TRANSLATE };
typedef struct { int id, kind; char model[64]; char *system, *user; int pid; unsigned long long start; } Job;
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
} E = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .pid = -1, .dirfd = -1, .state = "off",
        .idle_ms = LLM_IDLE_MS, .model_dir = LLM_MODEL_DIR, .engine = LLM_ENGINE, .dpkg_info = "/var/lib/dpkg/info" };

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

static int spawn_engine(const char *model, char **err) {
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
    if (dfd < 0 || devnull < 0) {
        *err = "llm.start";
        if (dfd >= 0) close(dfd);
        if (devnull >= 0) close(devnull);
        close(efd);
        close(mfd);
        return -1;
    }
    unlinkat(dfd, "engine.sock", 0);
    /* every source above the targets, so one dup2 never overwrites another's source */
    int *fds[] = { &mfd, &dfd, &efd, &devnull };
    for (int i = 0; i < 4; i++) {
        int hi = fcntl(*fds[i], F_DUPFD_CLOEXEC, 10);
        if (hi < 0) { close(mfd); close(dfd); close(efd); close(devnull); *err = "llm.start"; return -1; }
        close(*fds[i]);
        *fds[i] = hi;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, devnull, 0);
    posix_spawn_file_actions_adddup2(&fa, devnull, 1);
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
    char *envp[4];
    int ne = 0;
    if (getenv("LORDOS_SESSIOND")) envp[ne++] = "LORDOS_SESSIOND=1";      /* cgroup_enter asks lord-sessiond */
    const char *log = getenv("LORD_LLM_LOG");
    if (log && !strcmp(log, "1")) envp[ne++] = "LORD_LLM_LOG=1";
    envp[ne] = NULL;
    char *argv[] = { "lord-llm", NULL };
    pid_t pid;
    int r = posix_spawn(&pid, l, &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(devnull);
    close(efd);
    close(mfd);
    if (r) { close(dfd); *err = "llm.start"; return -1; }
    pthread_mutex_lock(&E.mu);
    E.pid = pid;
    E.dirfd = dfd;
    snprintf(E.model, sizeof E.model, "%s", model);
    pthread_mutex_unlock(&E.mu);
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

static char *run_analyze(Job *j) {
    char *err = "llm.start";
    double t0 = now_ms(), started = t0;
    pthread_mutex_lock(&E.mu);
    int same = E.pid > 0 && !strcmp(E.model, j->model);
    pthread_mutex_unlock(&E.mu);
    if (!same) {
        engine_kill();
        pthread_mutex_lock(&E.mu);
        E.state = "starting";
        pthread_mutex_unlock(&E.mu);
        if (spawn_engine(j->model, &err) || wait_ready(&err)) {
            engine_kill();
            pthread_mutex_lock(&E.mu);
            E.state = "off";
            pthread_mutex_unlock(&E.mu);
            return errjson(err);
        }
        started = now_ms();
    }
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
    if (r || status != 200) { free(ans); if (timed_out) engine_kill(); return errjson(timed_out ? "llm.timeout" : "llm.answer"); }
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

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&E.mu);
        while (!E.nq) {
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
        if (j.kind == J_ANALYZE || j.kind == J_TRANSLATE) res = run_analyze(&j);
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
    char *o = malloc(256);
    if (!o) return NULL;
    pthread_mutex_lock(&E.mu);
    double idle = E.pid > 0 && E.last_used > 0 ? now_ms() - E.last_used : 0;
    if (E.model[0]) snprintf(o, 256, "{\"state\":\"%s\",\"model\":\"%s\",\"pid\":%d,\"idleMs\":%.0f,\"queued\":%d}", E.state, E.model, (int)E.pid, idle, E.nq);
    else snprintf(o, 256, "{\"state\":\"%s\",\"model\":null,\"pid\":%d,\"idleMs\":0,\"queued\":%d}", E.state, (int)E.pid, E.nq);
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
