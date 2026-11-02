/* doc.c — axia-doc sessions on the axia-server side (see doc.h and proto.h). */
#include "doc.h"
#include "proto.h"
#include "wire.h"
#include "../sec/json.h"

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
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define MAX_SESS   32
#define MAX_IMG    768
#define MAX_BLOB   32
#define IMG_BUDGET ((size_t)160 << 20)      /* memory cap for returned images (all sessions) */
#define IDLE_MS    60000                    /* no deadline: killed if this passes without progress */
#define SPAWN_MS   15000

enum { S_FREE, S_NEW, S_INIT, S_READY, S_DEAD };

typedef struct Req {
    int id;
    uint32_t op;
    char *json;
    size_t len;
    int fds[DOC_MAX_FDS - 1];
    int nfds, prio, timeout;
    double queued, sent;
    struct Req *next;
} Req;

typedef struct {
    int state, sid;
    char mode[8];
    pid_t pid;
    int sock;
    DocInitFd init[DOC_INIT_FDS];
    char roles[DOC_INIT_FDS][8];
    int ninit;
    Req *queue, *inflight;
    double deadline;
    int closing;
} Sess;

typedef struct { int handle, w, h; uint32_t *px; uint64_t seq; } Img;
typedef struct { int handle, fd; size_t size; } Blob;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static int started, wakefd = -1;
static Sess sess[MAX_SESS];
static Img imgs[MAX_IMG];
static Blob blobs[MAX_BLOB];
static size_t img_bytes;
static uint64_t img_seq;
static int next_sid = 1, next_req = 1, next_handle = 1, pending;
static JBuf events;
static pid_t reap[64];
static int nreap;
static char helper[PATH_MAX];

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void wake(void) {
    uint64_t one = 1;
    if (wakefd >= 0 && write(wakefd, &one, sizeof one) < 0) { /* a wakeup is already pending */ }
}

/* Measurements in the log: LORD_DOC_TRACE=1 or the file ~/.cache/axia/doc-trace */
static int trace(void) {
    static int t = -1;
    if (t < 0) {
        char p[1024];
        const char *h = getenv("HOME");
        snprintf(p, sizeof p, "%s/.cache/axia/doc-trace", h ? h : "/nonexistent");
        t = getenv("LORD_DOC_TRACE") || access(p, F_OK) == 0;
    }
    return t;
}

/* ---- fixed path of the helper ---- */

int doc_available(void) {
    if (!helper[0]) {
        char self[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n <= 0) return -1;
        self[n] = 0;
        char *slash = strrchr(self, '/');
        if (!slash) return -1;
        *slash = 0;
        if (strlen(self) + 10 > sizeof helper) return -1;
        memcpy(helper, self, strlen(self));
        memcpy(helper + strlen(self), "/axia-doc", 10);
    }
    struct stat st, me;
    if (stat(helper, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) return -1;
    /* owned by the same user as axia-server (or by root) and writable by nobody else */
    if (stat("/proc/self/exe", &me) || (st.st_uid != 0 && st.st_uid != me.st_uid) || (st.st_mode & 022)) return -1;
    return 0;
}

/* ---- events ---- */

static void ev_begin(int sid, int req, const char *type) {
    if (events.n) jb_raw(&events, ",");
    jb_printf(&events, "{\"sid\":%d,\"req\":%d,\"type\":\"%s\"", sid, req, type);
}

static void ev_simple(int sid, int req, const char *type, const char *data) {
    ev_begin(sid, req, type);
    if (data) { jb_raw(&events, ",\"data\":"); jb_raw(&events, data); }
    jb_raw(&events, "}");
}

static void req_free(Req *r) {
    if (!r) return;
    for (int i = 0; i < r->nfds; i++) if (r->fds[i] >= 0) close(r->fds[i]);
    free(r->json);
    free(r);
}

/* The session ended (closed, crashed or killed by deadline): its requests fail. */
static void sess_kill(Sess *s, const char *why) {
    if (s->pid > 0) {
        kill(s->pid, SIGKILL);
        if (nreap < 64) reap[nreap++] = s->pid;
        else waitpid(s->pid, NULL, 0);
    }
    s->pid = 0;
    if (s->sock >= 0) close(s->sock);
    s->sock = -1;
    char data[96];
    snprintf(data, sizeof data, "{\"error\":\"%s\"}", why);
    if (s->inflight) { ev_simple(s->sid, s->inflight->id, "err", data); req_free(s->inflight); s->inflight = NULL; pending--; }
    while (s->queue) { Req *r = s->queue; s->queue = r->next; ev_simple(s->sid, r->id, "err", data); req_free(r); pending--; }
    for (int i = 0; i < s->ninit; i++) if (s->init[i].fd >= 0) close(s->init[i].fd);
    s->ninit = 0;
    if (!s->closing) ev_simple(s->sid, 0, "crash", data);
    s->state = s->closing ? S_FREE : S_DEAD;
}

static Sess *sess_get(int sid) {
    for (int i = 0; i < MAX_SESS; i++) if (sess[i].state != S_FREE && sess[i].sid == sid) return &sess[i];
    return NULL;
}

/* ---- starting a helper ---- */

static int spawn(Sess *s) {
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
    /* minimal environment: nothing from the caller */
    char henv[PATH_MAX + 8], fenv[PATH_MAX + 20], *envp[8];
    int ne = 0;
    const char *home = getenv("HOME");
    if (home) { snprintf(henv, sizeof henv, "HOME=%s", home); envp[ne++] = henv; }
    envp[ne++] = "LANG=C.UTF-8";
    const char *assets = getenv("LORD_DOC_FONTS");
    if (assets) { snprintf(fenv, sizeof fenv, "LORD_DOC_FONTS=%s", assets); envp[ne++] = fenv; }
    if (getenv("LORD_DOC_DEBUG")) envp[ne++] = "LORD_DOC_DEBUG=1";
    envp[ne] = NULL;
    char *argv[] = { "axia-doc", NULL };
    pid_t pid;
    int r = posix_spawn(&pid, helper, &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(sv[1]);
    if (r) { close(sv[0]); errno = r; return -1; }
    s->pid = pid;
    s->sock = sv[0];
    /* startup fds, grouped by role, and the order to lock itself down */
    for (int i = 0; i < s->ninit;) {
        int fds[DOC_MAX_FDS - 1], k = 0;
        const char *role = s->roles[i];
        while (i < s->ninit && k < DOC_MAX_FDS - 1 && !strcmp(s->roles[i], role)) fds[k++] = s->init[i++].fd;
        char j[48];
        int n = snprintf(j, sizeof j, "{\"role\":\"%s\"}", role);
        if (doc_send(s->sock, DQ_INIT_FD, 0, j, (size_t)n, fds, k)) return -1;
    }
    for (int i = 0; i < s->ninit; i++) { close(s->init[i].fd); s->init[i].fd = -1; }
    s->ninit = 0;
    char j[48];
    int n = snprintf(j, sizeof j, "{\"mode\":\"%s\"}", s->mode);
    if (doc_send(s->sock, DQ_INIT_GO, 0, j, (size_t)n, NULL, 0)) return -1;
    s->state = S_INIT;
    s->deadline = now_ms() + SPAWN_MS;
    return 0;
}

static void send_next(Sess *s) {
    if (s->state != S_READY || s->inflight || !s->queue) return;
    Req **best = &s->queue;
    for (Req **pp = &s->queue; *pp; pp = &(*pp)->next) if ((*pp)->prio > (*best)->prio) best = pp;
    Req *r = *best;
    *best = r->next;
    r->next = NULL;
    if (doc_send(s->sock, r->op, (uint32_t)r->id, r->json, r->len, r->fds, r->nfds)) {
        s->inflight = r;
        sess_kill(s, "crash");
        return;
    }
    for (int i = 0; i < r->nfds; i++) { close(r->fds[i]); r->fds[i] = -1; }
    r->nfds = 0;
    free(r->json);
    r->json = NULL;
    s->inflight = r;
    r->sent = now_ms();
    s->deadline = r->sent + (r->timeout > 0 ? r->timeout : IDLE_MS);
}

/* ---- images and blobs ---- */

static void img_evict(size_t need) {
    while (img_bytes + need > IMG_BUDGET) {
        Img *old = NULL;
        for (int i = 0; i < MAX_IMG; i++) if (imgs[i].px && (!old || imgs[i].seq < old->seq)) old = &imgs[i];
        if (!old) return;
        img_bytes -= (size_t)old->w * old->h * 4;
        free(old->px);
        memset(old, 0, sizeof *old);
    }
}

static int img_add(int fd, int w, int h) {
    if (w < 1 || h < 1 || w > DOC_IMG_MAX || h > DOC_IMG_MAX) return -1;
    size_t len = (size_t)w * h * 4;
    if (doc_check_sealed(fd, len) < 0) return -1;
    void *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) return -1;
    img_evict(len);
    Img *slot = NULL;
    for (int i = 0; i < MAX_IMG && !slot; i++) if (!imgs[i].px) slot = &imgs[i];
    if (!slot) {                                     /* no room: the oldest */
        for (int i = 0; i < MAX_IMG; i++) if (!slot || imgs[i].seq < slot->seq) slot = &imgs[i];
        img_bytes -= (size_t)slot->w * slot->h * 4;
        free(slot->px);
        memset(slot, 0, sizeof *slot);
    }
    uint32_t *px = malloc(len);
    if (!px) { munmap(map, len); return -1; }
    memcpy(px, map, len);
    munmap(map, len);
    slot->handle = next_handle++;
    slot->w = w;
    slot->h = h;
    slot->px = px;
    slot->seq = ++img_seq;
    img_bytes += len;
    return slot->handle;
}

static int blob_add(int fd) {
    long size = doc_check_sealed(fd, 1);
    if (size < 0) return -1;
    Blob *slot = NULL;
    for (int i = 0; i < MAX_BLOB && !slot; i++) if (!blobs[i].handle) slot = &blobs[i];
    if (!slot) {                                     /* the oldest */
        slot = &blobs[0];
        for (int i = 1; i < MAX_BLOB; i++) if (blobs[i].handle < slot->handle) slot = &blobs[i];
        close(slot->fd);
    }
    slot->fd = dup(fd);
    if (slot->fd < 0) { memset(slot, 0, sizeof *slot); return -1; }
    slot->size = (size_t)size;
    slot->handle = next_handle++;
    return slot->handle;
}

/* ---- what arrives from a helper ---- */

static void on_message(Sess *s) {
    DocMsg m;
    int r = doc_recv(s->sock, &m);
    if (r) { sess_kill(s, r > 0 ? "crash" : "protocol"); return; }
    /* the JSON comes from a process that may have read a malicious document: it is validated before
     * being put into the event queue */
    JDoc d = { 0 };
    if (m.json && json_parse(m.json, m.len, &d, NULL, 0)) { doc_msg_free(&m); sess_kill(s, "protocol"); return; }
    if (s->state == S_INIT) {
        if (m.h.op == DR_DONE && m.h.id == 0) { s->state = S_READY; ev_simple(s->sid, 0, "ready", NULL); }
        else sess_kill(s, "protocol");
        json_free(&d);
        doc_msg_free(&m);
        return;
    }
    Req *q = s->inflight;
    if (!q || m.h.id != (uint32_t)q->id) { json_free(&d); doc_msg_free(&m); sess_kill(s, "protocol"); return; }
    const char *data = m.json ? m.json : "{}";
    switch (m.h.op) {
    case DR_PROG:
    case DR_PART:
        ev_simple(s->sid, q->id, m.h.op == DR_PROG ? "prog" : "part", data);
        if (q->timeout <= 0) s->deadline = now_ms() + IDLE_MS;
        json_free(&d);
        doc_msg_free(&m);
        return;
    case DR_DONE:
    case DR_ERR:
        ev_simple(s->sid, q->id, m.h.op == DR_DONE ? "done" : "err", data);
        break;
    case DR_IMG: {
        int w = (int)json_num(d.root, "w", 0), h = (int)json_num(d.root, "h", 0);
        int hd = m.nfds ? img_add(m.fds[0], w, h) : -1;
        if (hd < 0) ev_simple(s->sid, q->id, "err", "{\"error\":\"image\"}");
        else {
            ev_begin(s->sid, q->id, "img");
            jb_printf(&events, ",\"image\":%d,\"w\":%d,\"h\":%d,\"data\":", hd, w, h);
            jb_raw(&events, data);
            jb_raw(&events, "}");
        }
        break;
    }
    case DR_BLOB: {
        int hd = m.nfds ? blob_add(m.fds[0]) : -1;
        if (hd < 0) ev_simple(s->sid, q->id, "err", "{\"error\":\"blob\"}");
        else {
            size_t size = 0;
            for (int i = 0; i < MAX_BLOB; i++) if (blobs[i].handle == hd) size = blobs[i].size;
            ev_begin(s->sid, q->id, "blob");
            jb_printf(&events, ",\"blob\":%d,\"size\":%zu}", hd, size);
        }
        break;
    }
    default:
        json_free(&d);
        doc_msg_free(&m);
        sess_kill(s, "protocol");
        return;
    }
    json_free(&d);
    doc_msg_free(&m);
    if (trace())
        fprintf(stderr, "axia-doc: request %u: queue %.1f ms, helper %.1f ms\n", q->op, q->sent - q->queued, now_ms() - q->sent);
    req_free(q);
    s->inflight = NULL;
    pending--;
    send_next(s);
}

static void *manager(void *arg) {
    (void)arg;
    for (;;) {
        struct pollfd pfd[MAX_SESS + 1];
        int idx[MAX_SESS + 1], n = 0;
        pthread_mutex_lock(&mu);
        for (int i = 0; i < nreap;) {
            if (waitpid(reap[i], NULL, WNOHANG) != 0) reap[i] = reap[--nreap];
            else i++;
        }
        pfd[n++] = (struct pollfd){ wakefd, POLLIN, 0 };
        double t = now_ms(), next = -1;
        for (int i = 0; i < MAX_SESS; i++) {
            Sess *s = &sess[i];
            if (s->state == S_NEW && spawn(s)) sess_kill(s, "spawn");
            if (s->state == S_READY && s->closing && !s->inflight) {
                /* close: the helper reads the end of the channel and exits on its own */
                if (s->sock >= 0) close(s->sock);
                s->sock = -1;
                if (s->pid > 0 && nreap < 64) reap[nreap++] = s->pid;
                s->pid = 0;
                while (s->queue) { Req *r = s->queue; s->queue = r->next; req_free(r); pending--; }
                s->state = S_FREE;
                continue;
            }
            send_next(s);
            if ((s->state == S_INIT || s->state == S_READY) && s->sock >= 0) {
                if ((s->state == S_INIT || s->inflight) && s->deadline < t) { sess_kill(s, "timeout"); continue; }
                pfd[n] = (struct pollfd){ s->sock, POLLIN, 0 };
                idx[n++] = i;
                if (s->state == S_INIT || s->inflight) { double d = s->deadline - t; if (next < 0 || d < next) next = d; }
            }
        }
        if (nreap && (next < 0 || next > 200)) next = 200;
        pthread_mutex_unlock(&mu);
        int wait = next < 0 ? -1 : (int)next + 1;
        if (poll(pfd, (nfds_t)n, wait) < 0 && errno != EINTR) continue;
        pthread_mutex_lock(&mu);
        if (pfd[0].revents & POLLIN) { uint64_t v; if (read(wakefd, &v, sizeof v) < 0) { /* nothing */ } }
        for (int k = 1; k < n; k++) {
            Sess *s = &sess[idx[k]];
            if (s->sock != pfd[k].fd || (s->state != S_INIT && s->state != S_READY)) continue;
            if (pfd[k].revents & POLLIN) on_message(s);
            else if (pfd[k].revents & (POLLHUP | POLLERR | POLLNVAL)) sess_kill(s, "crash");
        }
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

static int ensure_started(void) {
    if (started) return 0;
    if (doc_available()) return -1;
    wakefd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wakefd < 0) return -1;
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);          /* the thread doesn't receive the server's signals */
    int r = pthread_create(&thread, NULL, manager, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (r) { close(wakefd); wakefd = -1; return -1; }
    pthread_detach(thread);
    started = 1;
    return 0;
}

/* ---- API ---- */

int doc_session(const char *mode, DocInitFd *fds, int n) {
    pthread_mutex_lock(&mu);
    Sess *s = NULL;
    for (int i = 0; i < MAX_SESS && !s; i++) if (sess[i].state == S_FREE) s = &sess[i];
    /* crashed sessions that nobody closed free their slot */
    for (int i = 0; i < MAX_SESS && !s; i++) if (sess[i].state == S_DEAD) { sess[i].state = S_FREE; s = &sess[i]; }
    if (!s || n > DOC_INIT_FDS || ensure_started()) {
        pthread_mutex_unlock(&mu);
        for (int i = 0; i < n; i++) close(fds[i].fd);
        return -1;
    }
    memset(s, 0, sizeof *s);
    s->sock = -1;
    s->sid = next_sid++;
    snprintf(s->mode, sizeof s->mode, "%s", mode);
    for (int i = 0; i < n; i++) {
        s->init[i].fd = fds[i].fd;
        snprintf(s->roles[i], sizeof s->roles[i], "%s", fds[i].role);
    }
    s->ninit = n;
    s->state = S_NEW;
    int sid = s->sid;
    pthread_mutex_unlock(&mu);
    wake();
    return sid;
}

int doc_request(int sid, uint32_t op, const char *json, size_t len, int *fds, int nfds, int prio, int timeout_ms) {
    if (nfds < 0 || nfds > DOC_MAX_FDS - 2 || len > DOC_JSON_MAX) { for (int i = 0; i < nfds; i++) close(fds[i]); return -1; }
    Req *r = calloc(1, sizeof *r);
    if (!r) { for (int i = 0; i < nfds; i++) close(fds[i]); return -1; }
    if (json && len) {
        r->json = malloc(len + 1);
        if (!r->json) { free(r); for (int i = 0; i < nfds; i++) close(fds[i]); return -1; }
        memcpy(r->json, json, len);
        r->json[len] = 0;
        r->len = len;
    }
    r->op = op;
    r->nfds = nfds;
    for (int i = 0; i < nfds; i++) r->fds[i] = fds[i];
    r->prio = prio;
    r->timeout = timeout_ms;
    r->queued = now_ms();
    pthread_mutex_lock(&mu);
    Sess *s = sess_get(sid);
    if (!s || s->state == S_DEAD || s->closing) { pthread_mutex_unlock(&mu); req_free(r); return -1; }
    r->id = next_req++;
    /* at the end of the queue (send_next picks the priority) */
    Req **pp = &s->queue;
    while (*pp) pp = &(*pp)->next;
    *pp = r;
    pending++;
    int id = r->id;
    pthread_mutex_unlock(&mu);
    wake();
    return id;
}

int doc_cancel(int sid, int req) {
    pthread_mutex_lock(&mu);
    Sess *s = sess_get(sid);
    int found = 0;
    if (s) {
        for (Req **pp = &s->queue; *pp; pp = &(*pp)->next) {
            if ((*pp)->id != req) continue;
            Req *r = *pp;
            *pp = r->next;
            req_free(r);
            pending--;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&mu);
    return found;
}

void doc_close(int sid) {
    pthread_mutex_lock(&mu);
    Sess *s = sess_get(sid);
    if (s) {
        s->closing = 1;
        if (s->state == S_DEAD) s->state = S_FREE;
        else if (s->state != S_READY) sess_kill(s, "closed");   /* starting up: killed */
        else {
            while (s->queue) { Req *r = s->queue; s->queue = r->next; req_free(r); pending--; }
            if (s->inflight) sess_kill(s, "closed");          /* something long in progress: cut off */
        }
    }
    pthread_mutex_unlock(&mu);
    wake();
}

int doc_alive(int sid) {
    pthread_mutex_lock(&mu);
    Sess *s = sess_get(sid);
    int ok = s && s->state != S_DEAD && !s->closing;
    pthread_mutex_unlock(&mu);
    return ok;
}

char *doc_poll(void) {
    pthread_mutex_lock(&mu);
    char *out = NULL;
    if (events.n) {
        size_t len = events.n;
        out = malloc(len + 3);
        if (out) { out[0] = '['; memcpy(out + 1, events.p, len); out[len + 1] = ']'; out[len + 2] = 0; }
        jb_free(&events);
    }
    pthread_mutex_unlock(&mu);
    return out;
}

int doc_pending(void) {
    pthread_mutex_lock(&mu);
    int p = pending;
    for (int i = 0; i < MAX_SESS; i++) if (sess[i].state == S_NEW || sess[i].state == S_INIT) p++;
    pthread_mutex_unlock(&mu);
    return p;
}

const uint32_t *doc_image_lock(int handle, int *w, int *h) {
    pthread_mutex_lock(&mu);
    for (int i = 0; i < MAX_IMG; i++)
        if (imgs[i].px && imgs[i].handle == handle) { *w = imgs[i].w; *h = imgs[i].h; imgs[i].seq = ++img_seq; return imgs[i].px; }
    return NULL;
}
void doc_image_unlock(void) { pthread_mutex_unlock(&mu); }

void doc_image_release(int handle) {
    pthread_mutex_lock(&mu);
    for (int i = 0; i < MAX_IMG; i++)
        if (imgs[i].px && imgs[i].handle == handle) {
            img_bytes -= (size_t)imgs[i].w * imgs[i].h * 4;
            free(imgs[i].px);
            memset(&imgs[i], 0, sizeof imgs[i]);
        }
    pthread_mutex_unlock(&mu);
}

size_t doc_image_bytes(void) {
    pthread_mutex_lock(&mu);
    size_t b = img_bytes;
    pthread_mutex_unlock(&mu);
    return b;
}

int doc_blob_dup(int handle, size_t *size) {
    pthread_mutex_lock(&mu);
    int fd = -1;
    for (int i = 0; i < MAX_BLOB; i++)
        if (blobs[i].handle == handle) { fd = fcntl(blobs[i].fd, F_DUPFD_CLOEXEC, 0); if (size) *size = blobs[i].size; }
    pthread_mutex_unlock(&mu);
    return fd;
}

void doc_blob_release(int handle) {
    pthread_mutex_lock(&mu);
    for (int i = 0; i < MAX_BLOB; i++)
        if (handle && blobs[i].handle == handle) { close(blobs[i].fd); memset(&blobs[i], 0, sizeof blobs[i]); }
    pthread_mutex_unlock(&mu);
}

int doc_blob_copy(int handle, int outfd) {
    size_t size;
    int fd = doc_blob_dup(handle, &size);
    if (fd < 0) return -1;
    char buf[1 << 16];
    off_t off = 0;
    int ok = 0;
    while ((size_t)off < size) {
        ssize_t r = pread(fd, buf, sizeof buf, off);
        if (r <= 0) { ok = -1; break; }
        for (ssize_t w = 0; w < r;) {
            ssize_t k = write(outfd, buf + w, (size_t)(r - w));
            if (k <= 0) { if (k < 0 && errno == EINTR) continue; ok = -1; break; }
            w += k;
        }
        if (ok) break;
        off += r;
    }
    close(fd);
    if (!ok && fsync(outfd)) ok = -1;
    return ok;
}
