/* sftpc.c — see sftpc.h. */
#include "sftpc.h"
#include "sshmux.h"
#include "sftp.h"
#include "sftpproto.h"
#include "../doc/proto.h"
#include "../doc/wire.h"
#include "../fsops.h"
#include "../sec/json.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
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

#define OPEN_TIMEOUT_MS  20000          /* the channel, lord-sftp and SFTP's hello */
#define INFO_TIMEOUT_MS  120000         /* sha256sum of a big file */
#define INFO_MAX         4096

/* the root form: the server's sftp-server through sudo, a fixed command (no part of it from JS) */
static const char ROOT_CMD_N[] =
    "for p in /usr/lib/openssh/sftp-server /usr/libexec/openssh/sftp-server /usr/libexec/sftp-server /usr/lib/ssh/sftp-server; "
    "do if [ -x \"$p\" ]; then exec sudo -n -- \"$p\"; fi; done; exit 127";
static const char ROOT_CMD_S[] =
    "for p in /usr/lib/openssh/sftp-server /usr/libexec/openssh/sftp-server /usr/libexec/sftp-server /usr/lib/ssh/sftp-server; "
    "do if [ -x \"$p\" ]; then exec sudo -S -p '' -- \"$p\"; fi; done; exit 127";

typedef struct {
    int used, conn, ready, busy, closing, root;
    int sock;                           /* lord-sftp's channel */
    pid_t pid;
    int open_req;
    char path[108];                     /* the master's control socket */
    double started;
} Conn;

typedef struct {
    int used, job, conn, req;
    pid_t pid;
    int cancel;
} Job;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static Conn conns[SFTP_MAX_CONNS];
static Job jobs[SFTP_MAX_JOBS];
static SftpItem *queue;
static int nqueue, capqueue;
static int next_id = 1, next_conn = 1, next_job = 1;
static char helper[PATH_MAX], muxdir[PATH_MAX];
static SftpFgFn fg_of;

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

void sftp_set_fg(SftpFgFn fn) { fg_of = fn; }

const char *sftp_mux_dir(void) {
    if (!muxdir[0]) {
        const char *rt = getenv("XDG_RUNTIME_DIR");
        if (!rt || rt[0] != '/' || strlen(rt) > 64) return NULL;
        char a[PATH_MAX];
        snprintf(a, sizeof a, "%s/lordos-term", rt);
        mkdir(a, 0700);
        snprintf(muxdir, sizeof muxdir, "%s/lordos-term/ssh", rt);
        mkdir(muxdir, 0700);
    }
    struct stat st;
    if (lstat(muxdir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) return NULL;
    if (st.st_mode & 077) chmod(muxdir, 0700);
    return muxdir;
}

int sftp_available(void) {
    if (!helper[0]) {
        char self[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n <= 0) return -1;
        self[n] = 0;
        char *slash = strrchr(self, '/');
        if (!slash) return -1;
        *slash = 0;
        if (strlen(self) + 11 > sizeof helper) return -1;
        snprintf(helper, sizeof helper, "%s/lord-sftp", self);
    }
    struct stat st, me;
    if (stat(helper, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) return -1;
    if (stat("/proc/self/exe", &me) || (st.st_uid != 0 && st.st_uid != me.st_uid) || (st.st_mode & 022)) return -1;
    return 0;
}

/* ---- the queue ---- */

static void push_own(int id, int type, char *data, size_t len) {     /* mu held; takes data */
    if (!data) return;
    if (nqueue == capqueue) {
        int nc = capqueue ? capqueue * 2 : 32;
        SftpItem *q = realloc(queue, sizeof *q * (size_t)nc);
        if (!q) { free(data); return; }
        queue = q;
        capqueue = nc;
    }
    queue[nqueue++] = (SftpItem){ id, type, data, len };
}

static void push(int id, int type, const char *json) { char *d = strdup(json); if (d) push_own(id, type, d, strlen(d)); }

static void push_err(int id, const char *code, const char *message) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"error\":");
    jb_str(&b, code);
    if (message && message[0]) { jb_raw(&b, ",\"message\":"); jb_str(&b, message); }
    jb_raw(&b, "}");
    if (!b.oom) push_own(id, SFTP_ERR, b.p, b.n); else jb_free(&b);
}

static void push_job_evt(int job, const char *ev, const char *rest) {
    char j[512];
    snprintf(j, sizeof j, "{\"ev\":\"%s\",\"job\":%d%s%s}", ev, job, rest && rest[0] ? "," : "", rest ? rest : "");
    push(0, SFTP_EVT, j);
}

int sftp_poll(SftpItem **items) {
    pthread_mutex_lock(&mu);
    int n = nqueue;
    *items = queue;
    queue = NULL;
    nqueue = capqueue = 0;
    pthread_mutex_unlock(&mu);
    return n;
}

void sftp_items_free(SftpItem *items, int n) {
    for (int i = 0; i < n; i++) free(items[i].data);
    free(items);
}

/* ---- a channel with lord-sftp on it ---- */

/* Opens a session on the master at path (sftp, or the root command) and starts lord-sftp on it, its
 * hello read. 0 ok: *sock its channel, *pid, *mux the control connection (kept open while it runs). */
static int start_channel(const char *path, int root, const char *password, Mux *mux, int *sock, pid_t *pid, char *why, size_t wn) {
    int in[2] = { -1, -1 }, out[2] = { -1, -1 }, sv[2] = { -1, -1 };
    int devnull = -1, rc = -1;
    *sock = -1;
    *pid = -1;
    if (mux_connect(mux, path, 5000)) { snprintf(why, wn, "%s", mux->why); return -1; }
    devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC) || devnull < 0) { snprintf(why, wn, "pipe"); goto done; }
    if (mux_session(mux, !root, root ? (password ? ROOT_CMD_S : ROOT_CMD_N) : "sftp", in[0], out[1], devnull, 10000)) {
        snprintf(why, wn, "%s", mux->why);
        goto done;
    }
    close(in[0]); in[0] = -1;
    close(out[1]); out[1] = -1;
    if (root && password) {
        size_t n = strlen(password);
        char *line = malloc(n + 2);
        int ok = line != NULL;
        if (ok) { memcpy(line, password, n); line[n] = '\n'; ok = write(in[1], line, n + 1) == (ssize_t)(n + 1); explicit_bzero(line, n + 2); free(line); }
        if (!ok) { snprintf(why, wn, "write"); goto done; }
    }
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { snprintf(why, wn, "socketpair"); goto done; }
    {
        int a = fcntl(sv[1], F_DUPFD_CLOEXEC, 10), b = fcntl(out[0], F_DUPFD_CLOEXEC, 10), c = fcntl(in[1], F_DUPFD_CLOEXEC, 10);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, a, 3);
        posix_spawn_file_actions_adddup2(&fa, b, 4);
        posix_spawn_file_actions_adddup2(&fa, c, 5);
        posix_spawn_file_actions_addclosefrom_np(&fa, 6);
        posix_spawnattr_t at;
        posix_spawnattr_init(&at);
        sigset_t none, all;
        sigemptyset(&none);
        sigfillset(&all);
        posix_spawnattr_setsigmask(&at, &none);
        posix_spawnattr_setsigdefault(&at, &all);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
        char *envp[] = { "LANG=C.UTF-8", NULL };
        char *argv[] = { "lord-sftp", NULL };
        int e = a >= 0 && b >= 0 && c >= 0 ? posix_spawn(pid, helper, &fa, &at, argv, envp) : -1;
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&at);
        if (a >= 0) close(a);
        if (b >= 0) close(b);
        if (c >= 0) close(c);
        if (e) { *pid = -1; snprintf(why, wn, "spawn"); goto done; }
    }
    close(sv[1]); sv[1] = -1;
    /* its hello: SFTP's version from the server */
    struct pollfd p = { sv[0], POLLIN, 0 };
    DocMsg m;
    if (poll(&p, 1, OPEN_TIMEOUT_MS) <= 0 || doc_recv(sv[0], &m)) { snprintf(why, wn, "%s", root ? "root" : "sftp"); goto done; }
    int ok = m.h.op == SE_OK;
    if (!ok) snprintf(why, wn, "%s", root ? "root" : "sftp");
    doc_msg_free(&m);
    if (!ok) goto done;
    *sock = sv[0];
    sv[0] = -1;
    rc = 0;
done:
    if (in[0] >= 0) close(in[0]);
    if (in[1] >= 0) close(in[1]);
    if (out[0] >= 0) close(out[0]);
    if (out[1] >= 0) close(out[1]);
    if (sv[0] >= 0) close(sv[0]);
    if (sv[1] >= 0) close(sv[1]);
    if (devnull >= 0) close(devnull);
    if (rc) {
        if (*pid > 0) { kill(*pid, SIGKILL); waitpid(*pid, NULL, 0); *pid = -1; }
        mux_close(mux);
    }
    return rc;
}

static void reap(pid_t pid) {
    if (pid <= 0) return;
    double t0 = now_ms();
    while (waitpid(pid, NULL, WNOHANG) == 0) {
        if (now_ms() - t0 > 1000) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); return; }
        struct timespec ts = { 0, 10 * 1000000 };
        nanosleep(&ts, NULL);
    }
}

/* a reply from lord-sftp as JSON text (a big listing comes in a sealed memfd) */
static char *reply_json(DocMsg *m, size_t *len) {
    if (m->nfds == 1) {
        long n = doc_check_sealed(m->fds[0], 1);
        if (n <= 0 || n > (64l << 20)) return NULL;
        char *d = malloc((size_t)n + 1);
        if (!d || pread(m->fds[0], d, (size_t)n, 0) != n) { free(d); return NULL; }
        d[n] = 0;
        *len = (size_t)n;
        return d;
    }
    if (!m->json) return NULL;
    char *d = m->json;
    *len = m->len;
    m->json = NULL;
    return d;
}

/* ---- connections ---- */

typedef struct { int slot, req, root; char path[108]; char *password; size_t pwcap; } OpenJob;

static void *conn_thread(void *arg) {
    OpenJob *j = arg;
    Conn *s = &conns[j->slot];
    Mux mux;
    int sock;
    pid_t pid;
    char why[160] = "";
    int rc = start_channel(j->path, j->root, j->password, &mux, &sock, &pid, why, sizeof why);
    if (j->password) { explicit_bzero(j->password, j->pwcap); free(j->password); }
    int req = j->req;
    free(j);
    pthread_mutex_lock(&mu);
    int cancelled = !rc && s->closing;
    if (rc || cancelled) {
        push_err(req, cancelled ? "closed" : !strcmp(why, "root") ? "root" : "unavailable", cancelled ? NULL : why);
        memset(s, 0, sizeof *s);
        s->sock = -1;
        pthread_mutex_unlock(&mu);
        if (cancelled) { close(sock); mux_close(&mux); kill(pid, SIGKILL); reap(pid); }
        return NULL;
    }
    s->sock = sock;
    s->pid = pid;
    s->ready = 1;
    s->open_req = 0;
    char ok[64];
    snprintf(ok, sizeof ok, "{\"conn\":%d,\"root\":%s}", s->conn, s->root ? "true" : "false");
    push(req, SFTP_OK, ok);
    pthread_mutex_unlock(&mu);
    for (;;) {
        struct pollfd p[2] = { { sock, POLLIN, 0 }, { mux.fd, POLLIN, 0 } };
        int r = poll(p, 2, 250);
        pthread_mutex_lock(&mu);
        int closing = s->closing;
        pthread_mutex_unlock(&mu);
        if (closing) break;
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (r < 0) break; continue; }
        if (p[1].revents) break;                      /* the master said something or went away: the ssh ended */
        DocMsg m;
        if (doc_recv(sock, &m)) break;
        size_t len = 0;
        char *data = reply_json(&m, &len);
        pthread_mutex_lock(&mu);
        int id = (int)m.h.id;
        if (id && id == s->busy) {
            s->busy = 0;
            if (data) push_own(id, m.h.op == SE_ERR ? SFTP_ERR : SFTP_OK, data, len);
            else push_err(id, "closed", "bad reply");
            data = NULL;
        }
        pthread_mutex_unlock(&mu);
        free(data);
        doc_msg_free(&m);
    }
    doc_send(sock, SP_END, 0, "{}", 2, NULL, 0);
    close(sock);
    mux_close(&mux);
    reap(pid);
    pthread_mutex_lock(&mu);
    if (s->busy) push_err(s->busy, "closed", NULL);
    if (!s->closing) { char ev[96]; snprintf(ev, sizeof ev, "{\"ev\":\"closed\",\"conn\":%d}", s->conn); push(0, SFTP_EVT, ev); }
    memset(s, 0, sizeof *s);
    s->sock = -1;
    pthread_mutex_unlock(&mu);
    return NULL;
}

static Conn *find_conn(int conn) {
    for (int i = 0; i < SFTP_MAX_CONNS; i++) if (conns[i].used && conns[i].conn == conn) return &conns[i];
    return NULL;
}

static int spawn_thread(void *(*fn)(void *), void *arg) {
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int e = pthread_create(&t, &at, fn, arg);
    pthread_attr_destroy(&at);
    return e;
}

/* the master of Terminal session sid, or -1 */
static int master_of(int sid, char *path, size_t cap) {
    const char *dir = sftp_mux_dir();
    pid_t pg = fg_of ? fg_of(sid) : -1;
    return dir && pg > 0 ? sshmux_find(dir, pg, path, cap) : -1;
}

static void do_open(int id, const JVal *r) {
    int sid = (int)json_num(r, "sid", -1);
    const JVal *rv = json_get(r, "root"), *pv = json_get(r, "password");
    int root = rv && rv->type == J_BOOL && rv->num;
    if (pv && (pv->type != J_STR || pv->len > 500 || memchr(pv->str, '\n', pv->len) || memchr(pv->str, 0, pv->len))) { push_err(id, "bad", NULL); return; }
    OpenJob *j = calloc(1, sizeof *j);
    if (!j) { push_err(id, "unavailable", NULL); return; }
    if (master_of(sid, j->path, sizeof j->path)) { free(j); push_err(id, "nossh", NULL); return; }
    if (root && pv && pv->len) {
        j->pwcap = pv->len + 1;
        j->password = malloc(j->pwcap);
        if (!j->password) { free(j); push_err(id, "unavailable", NULL); return; }
        memcpy(j->password, pv->str, pv->len + 1);
    }
    int slot = -1;
    for (int i = 0; i < SFTP_MAX_CONNS && slot < 0; i++) if (!conns[i].used) slot = i;
    if (slot < 0) { if (j->password) { explicit_bzero(j->password, j->pwcap); free(j->password); } free(j); push_err(id, "busy", NULL); return; }
    Conn *s = &conns[slot];
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->sock = -1;
    s->conn = next_conn++;
    if (next_conn > 1 << 30) next_conn = 1;
    s->open_req = id;
    s->root = root;
    s->started = now_ms();
    snprintf(s->path, sizeof s->path, "%s", j->path);
    j->slot = slot;
    j->req = id;
    j->root = root;
    if (spawn_thread(conn_thread, j)) {
        if (j->password) { explicit_bzero(j->password, j->pwcap); free(j->password); }
        free(j);
        memset(s, 0, sizeof *s);
        s->sock = -1;
        push_err(id, "unavailable", NULL);
    }
}

/* ---- transfers ---- */

typedef struct { int slot, job, conn, req, root, fd, op; char path[108]; char *json; size_t len; } XferJob;

static void *xfer_thread(void *arg) {
    XferJob *x = arg;
    Job *jb = &jobs[x->slot];
    Mux mux;
    int sock = -1;
    pid_t pid = -1;
    char why[160] = "";
    /* as root, a transfer goes through sudo -n: the password was given once, to sudo's cache on the server */
    int rc = start_channel(x->path, x->root, NULL, &mux, &sock, &pid, why, sizeof why);
    pthread_mutex_lock(&mu);
    if (!rc) jb->pid = pid;
    int cancel = jb->cancel;
    pthread_mutex_unlock(&mu);
    char rest[300];
    if (rc || cancel) {
        snprintf(rest, sizeof rest, "\"error\":\"%s\"", rc ? (!strcmp(why, "root") ? "root" : "unavailable") : "cancelled");
        pthread_mutex_lock(&mu);
        push_job_evt(x->job, "error", rest);
        pthread_mutex_unlock(&mu);
    } else if (doc_send(sock, (uint32_t)x->op, 1, x->json, x->len, &x->fd, 1)) {
        pthread_mutex_lock(&mu);
        push_job_evt(x->job, "error", "\"error\":\"closed\"");
        pthread_mutex_unlock(&mu);
    } else {
        close(x->fd);
        x->fd = -1;
        for (int done = 0; !done;) {
            DocMsg m;
            if (doc_recv(sock, &m)) {
                pthread_mutex_lock(&mu);
                push_job_evt(x->job, "error", jb->cancel ? "\"error\":\"cancelled\"" : "\"error\":\"closed\"");
                pthread_mutex_unlock(&mu);
                break;
            }
            JDoc d;
            int parsed = m.json && !json_parse(m.json, m.len, &d, NULL, 0) && d.root && d.root->type == J_OBJ;
            pthread_mutex_lock(&mu);
            if (m.h.op == SE_PROGRESS && parsed) {
                snprintf(rest, sizeof rest, "\"done\":%.0f,\"total\":%.0f", json_num(d.root, "done", 0), json_num(d.root, "total", 0));
                push_job_evt(x->job, "progress", rest);
            } else if (m.h.op == SE_OK && parsed) {
                snprintf(rest, sizeof rest, "\"bytes\":%.0f", json_num(d.root, "bytes", 0));
                push_job_evt(x->job, "done", rest);
                done = 1;
            } else {
                const char *e = parsed ? json_str(d.root, "error") : NULL, *msg = parsed ? json_str(d.root, "message") : NULL;
                JBuf b;
                jb_init(&b);
                jb_raw(&b, "\"error\":");
                jb_str(&b, e ? e : "bad");
                if (msg) { jb_raw(&b, ",\"message\":"); jb_str(&b, msg); }
                if (!b.oom) push_job_evt(x->job, "error", b.p);
                jb_free(&b);
                done = 1;
            }
            pthread_mutex_unlock(&mu);
            if (parsed) json_free(&d);
            doc_msg_free(&m);
        }
        doc_send(sock, SP_END, 0, "{}", 2, NULL, 0);
    }
    if (x->fd >= 0) close(x->fd);
    if (sock >= 0) close(sock);
    if (!rc) mux_close(&mux);
    reap(pid);
    pthread_mutex_lock(&mu);
    memset(jb, 0, sizeof *jb);
    pthread_mutex_unlock(&mu);
    free(x->json);
    free(x);
    return NULL;
}

/* the local file of a transfer, with lord.fs's rules: a download's in the writable roots (not protected),
 * an upload's where reading is allowed; never a link, always a regular file */
static int local_fd(const char *path, int put, int replace, long long *size, const char **why) {
    char real[PATH_MAX];
    if (put) {
        if (fsops_resolve_readable(path, real, NULL)) { *why = "denied"; return -1; }
        int fd = open(real, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY | O_NONBLOCK);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) { if (fd >= 0) close(fd); *why = "nofile"; return -1; }
        fcntl(fd, F_SETFL, 0);
        *size = st.st_size;
        return fd;
    }
    int r = fsops_resolve_target(path, real, sizeof real, NULL, 0, NULL);
    if (r) { *why = r == -2 ? "protected" : "denied"; return -1; }
    int fd = open(real, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY | (replace ? O_TRUNC : O_EXCL), 0644);
    if (fd < 0) { *why = errno == EEXIST ? "exists" : "denied"; return -1; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); *why = "denied"; return -1; }
    return fd;
}

static void do_xfer(int id, const JVal *r, int put) {
    Conn *s = find_conn((int)json_num(r, "conn", 0));
    if (!s || !s->ready || s->closing) { push_err(id, "closed", NULL); return; }
    const char *rpath = json_str(r, "path"), *lpath = json_str(r, put ? "src" : "dest");
    if (!sf_path_ok(rpath) || !lpath || !lpath[0] || strlen(lpath) >= PATH_MAX) { push_err(id, "bad", NULL); return; }
    const JVal *rep = json_get(r, "replace");
    long long size = 0;
    const char *why = NULL;
    int fd = local_fd(lpath, put, rep && rep->type == J_BOOL && rep->num, &size, &why);
    if (fd < 0) { push_err(id, why, NULL); return; }
    int slot = -1;
    for (int i = 0; i < SFTP_MAX_JOBS && slot < 0; i++) if (!jobs[i].used) slot = i;
    XferJob *x = slot >= 0 ? calloc(1, sizeof *x) : NULL;
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"path\":");
    jb_str(&b, rpath);
    if (put) {
        double pm = json_num(r, "perm", -1);
        jb_printf(&b, ",\"size\":%lld", size);
        if (pm >= 0 && pm <= 0777) jb_printf(&b, ",\"perm\":%d", (int)pm);
    }
    jb_raw(&b, "}");
    if (!x || b.oom) { free(x); jb_free(&b); close(fd); push_err(id, slot < 0 ? "busy" : "unavailable", NULL); return; }
    Job *jb = &jobs[slot];
    memset(jb, 0, sizeof *jb);
    jb->used = 1;
    jb->job = next_job++;
    if (next_job > 1 << 30) next_job = 1;
    jb->conn = s->conn;
    *x = (XferJob){ slot, jb->job, s->conn, id, s->root, fd, put ? SP_PUT : SP_GET, "", b.p, b.n };
    snprintf(x->path, sizeof x->path, "%s", s->path);
    if (spawn_thread(xfer_thread, x)) {
        free(x->json);
        free(x);
        close(fd);
        memset(jb, 0, sizeof *jb);
        push_err(id, "unavailable", NULL);
        return;
    }
    char ok[64];
    snprintf(ok, sizeof ok, "{\"job\":%d,\"size\":%lld}", jb->job, size);
    push(id, SFTP_OK, ok);
}

/* ---- remote information and forwards ---- */

typedef struct { int req; char path[108]; char cmd[SFTP_NAME_MAX * 4 + 64]; } InfoJob;

static void *info_thread(void *arg) {
    InfoJob *j = arg;
    Mux mux;
    int out[2] = { -1, -1 }, devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    const char *fail = NULL;
    char *text = calloc(1, INFO_MAX + 1);
    size_t n = 0;
    if (!text || devnull < 0 || pipe2(out, O_CLOEXEC)) fail = "unavailable";
    else if (mux_connect(&mux, j->path, 5000)) fail = "nossh";
    else if (mux_session(&mux, 0, j->cmd, devnull, out[1], devnull, 10000)) { fail = "failed"; mux_close(&mux); }
    else {
        close(out[1]);
        out[1] = -1;
        double t0 = now_ms();
        for (;;) {
            struct pollfd p = { out[0], POLLIN, 0 };
            long left = INFO_TIMEOUT_MS - (long)(now_ms() - t0);
            if (left <= 0) { fail = "timeout"; break; }
            int r = poll(&p, 1, (int)left);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { fail = "timeout"; break; }
            char buf[1024];
            ssize_t k = read(out[0], buf, sizeof buf);
            if (k <= 0) break;
            for (ssize_t i = 0; i < k && n < INFO_MAX; i++) text[n++] = (unsigned char)buf[i] < 0x20 && buf[i] != '\n' ? ' ' : buf[i] == 0x7f ? ' ' : buf[i];
        }
        mux_close(&mux);
    }
    if (out[0] >= 0) close(out[0]);
    if (out[1] >= 0) close(out[1]);
    if (devnull >= 0) close(devnull);
    pthread_mutex_lock(&mu);
    if (fail) push_err(j->req, fail, NULL);
    else {
        JBuf b;
        jb_init(&b);
        jb_raw(&b, "{\"text\":");
        jb_strn(&b, text, n);
        jb_raw(&b, "}");
        if (!b.oom) push_own(j->req, SFTP_OK, b.p, b.n); else jb_free(&b);
    }
    pthread_mutex_unlock(&mu);
    free(text);
    free(j);
    return NULL;
}

static void do_info(int id, const JVal *r) {
    Conn *s = find_conn((int)json_num(r, "conn", 0));
    const char *path = json_str(r, "path"), *what = json_str(r, "what");
    if (!s || !s->ready) { push_err(id, "closed", NULL); return; }
    if (!sf_path_ok(path) || !what) { push_err(id, "bad", NULL); return; }
    const char *file_argv[] = { "file", "-b", "--", path, NULL }, *sha_argv[] = { "sha256sum", "--", path, NULL };
    const char *const *argv = !strcmp(what, "file") ? file_argv : !strcmp(what, "sha256") ? sha_argv : NULL;
    InfoJob *j = argv ? calloc(1, sizeof *j) : NULL;
    if (!argv || !j || mux_quote_args(argv, j->cmd, sizeof j->cmd)) { free(j); push_err(id, "bad", NULL); return; }
    j->req = id;
    snprintf(j->path, sizeof j->path, "%s", s->path);
    if (spawn_thread(info_thread, j)) { free(j); push_err(id, "unavailable", NULL); }
}

/* a free port on 127.0.0.1 (taken by the master right after: a race another program could win,
 * reported as the master's failure) */
static int free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = 0 };
    socklen_t al = sizeof a;
    int port = -1;
    if (!bind(fd, (struct sockaddr *)&a, sizeof a) && !getsockname(fd, (struct sockaddr *)&a, &al)) port = ntohs(a.sin_port);
    close(fd);
    return port;
}

/* a fixed local port something already listens on (another tab's ssh with the same profile, or another program):
 * tried with SO_REUSEADDR as ssh binds it, so only a socket bound there counts, not connections still closing */
static int port_taken(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), on = 1;
    if (fd < 0) return 0;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons((uint16_t)port) };
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    int taken = bind(fd, (struct sockaddr *)&a, sizeof a) && errno == EADDRINUSE;
    close(fd);
    return taken;
}

typedef struct { int req, close; char path[108]; char host[256]; int port, local; } FwdJob;

static void *fwd_thread(void *arg) {
    FwdJob *j = arg;
    Mux mux;
    int local = j->local ? j->local : free_port();
    const char *fail = local <= 0 ? "unavailable" : j->local && !j->close && port_taken(local) ? "inuse" : NULL;
    char why[160] = "";
    if (!fail) {
        if (mux_connect(&mux, j->path, 5000)) fail = "nossh";
        else {
            if (mux_forward(&mux, !j->close, (uint32_t)local, j->host, (uint32_t)j->port, 10000)) { fail = "failed"; snprintf(why, sizeof why, "%s", mux.why); }
            mux_close(&mux);
        }
    }
    pthread_mutex_lock(&mu);
    if (fail) push_err(j->req, fail, why);
    else { char ok[48]; snprintf(ok, sizeof ok, "{\"local\":%d}", local); push(j->req, SFTP_OK, ok); }
    pthread_mutex_unlock(&mu);
    free(j);
    return NULL;
}

/* a host as ssh's -L takes it: a name or an address, no spaces, colons only for IPv6 */
static int host_ok(const char *h) {
    if (!h || !h[0] || strlen(h) > 253) return 0;
    for (const char *p = h; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == ':' || *p == '_')) return 0;
    return 1;
}

static void do_forward(int id, const JVal *r) {
    const char *host = json_str(r, "host");
    double port = json_num(r, "port", -1), local = json_num(r, "local", 0);
    const JVal *cv = json_get(r, "close");
    if (!host_ok(host) || port < 1 || port > 65535 || local < 0 || local > 65535 || (local && local < 1024)) { push_err(id, "bad", NULL); return; }
    FwdJob *j = calloc(1, sizeof *j);
    if (!j) { push_err(id, "unavailable", NULL); return; }
    if (master_of((int)json_num(r, "sid", -1), j->path, sizeof j->path)) { free(j); push_err(id, "nossh", NULL); return; }
    j->req = id;
    j->close = cv && cv->type == J_BOOL && cv->num;
    snprintf(j->host, sizeof j->host, "%s", host);
    j->port = (int)port;
    j->local = (int)local;
    if (spawn_thread(fwd_thread, j)) { free(j); push_err(id, "unavailable", NULL); }
}

/* ---- requests ---- */

int sftp_request(const char *json, size_t len) {
    if (sftp_available()) return -1;
    JDoc d;
    int parsed = len <= 65536 && !json_parse(json, len, &d, NULL, 0);
    pthread_mutex_lock(&mu);
    int id = next_id++;
    if (next_id > 1 << 30) next_id = 1;
    if (!parsed) { push_err(id, "bad", NULL); pthread_mutex_unlock(&mu); return id; }
    const JVal *r = d.root;
    const char *op = r && r->type == J_OBJ ? json_str(r, "op") : NULL;
    static const struct { const char *name; int op; } SIMPLE[] = {
        { "list", SP_LIST }, { "stat", SP_STAT }, { "mkdir", SP_MKDIR }, { "remove", SP_REMOVE },
        { "rename", SP_RENAME }, { "chmod", SP_CHMOD }, { "chown", SP_CHOWN },
    };
    int qop = 0;
    for (size_t i = 0; op && i < sizeof SIMPLE / sizeof SIMPLE[0]; i++) if (!strcmp(op, SIMPLE[i].name)) qop = SIMPLE[i].op;
    if (!op) push_err(id, "bad", NULL);
    else if (!strcmp(op, "open")) {
        do_open(id, r);
        const JVal *pv = json_get(r, "password");
        if (pv && pv->type == J_STR) explicit_bzero((char *)pv->str, pv->len);
    } else if (!strcmp(op, "get") || !strcmp(op, "put")) do_xfer(id, r, !strcmp(op, "put"));
    else if (!strcmp(op, "info")) do_info(id, r);
    else if (!strcmp(op, "forward")) do_forward(id, r);
    else if (!strcmp(op, "cancel")) {
        int job = (int)json_num(r, "job", 0);
        for (int i = 0; i < SFTP_MAX_JOBS; i++) if (jobs[i].used && jobs[i].job == job) { jobs[i].cancel = 1; if (jobs[i].pid > 0) kill(jobs[i].pid, SIGKILL); }
        push(id, SFTP_OK, "{}");
    } else if (!strcmp(op, "close")) {
        Conn *s = find_conn((int)json_num(r, "conn", 0));
        if (s) s->closing = 1;
        push(id, SFTP_OK, "{}");
    } else if (qop) {
        Conn *s = find_conn((int)json_num(r, "conn", 0));
        if (!s || !s->ready || s->closing || s->sock < 0) push_err(id, "closed", NULL);
        else if (s->busy) push_err(id, "busy", NULL);
        else if (doc_send(s->sock, (uint32_t)qop, (uint32_t)id, json, len, NULL, 0)) push_err(id, "closed", NULL);
        else s->busy = id;
    } else push_err(id, "bad", NULL);
    pthread_mutex_unlock(&mu);
    json_free(&d);
    return id;
}
