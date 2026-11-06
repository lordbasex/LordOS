/* vclient.c — lord-server talks to lord-vault (see vclient.h). */
#include "vclient.h"
#include "vwire.h"
#include "fidodev.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

typedef struct Ev { struct Ev *next; uint32_t id, kind; char *json; size_t len; } Ev;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int sock = -1;
static pid_t pid;
static pthread_t reader;
static int reader_on;
static uint32_t next_id = 1;
static int inflight;
static Ev *qhead, *qtail;
static char helper[PATH_MAX];
static int helper_ok = -1;
static char *pending_pass;              /* session password that arrived before lord-vault was running */

/* Private requests (the broker's keyring op, src/sec/brokerkr.c): their replies go to their own
 * queue, read by lord-server's C code (vc_take_private), never to the JS queue (vc_poll). */
#define VC_PRIV_MAX 64
static uint32_t priv_ids[VC_PRIV_MAX];
static int npriv;
static Ev *phead, *ptail;
static int priv_efd = -1;

static int priv_take_id(uint32_t id) {          /* with mu held: 1 if id was a private request */
    for (int i = 0; i < npriv; i++) if (priv_ids[i] == id) { priv_ids[i] = priv_ids[--npriv]; return 1; }
    return 0;
}
static void priv_wake(void) { if (priv_efd >= 0) { uint64_t one = 1; ssize_t r = write(priv_efd, &one, sizeof one); (void)r; } }

int vc_available(void) {
    if (helper_ok < 0) {
        char self[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        helper_ok = 0;
        if (n > 0) {
            self[n] = 0;
            char *sl = strrchr(self, '/');
            if (sl) {
                *sl = 0;
                snprintf(helper, sizeof helper, "%.*s/lord-vault", (int)(sizeof helper - 16), self);
                struct stat st;
                /* owned by the user or root, and nobody else can replace it */
                if (stat(helper, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR) && !(st.st_mode & S_IWOTH) &&
                    (st.st_uid == 0 || st.st_uid == getuid())) helper_ok = 1;
            }
        }
    }
    return helper_ok;
}

/* Blocking private requests from a lord-server thread (Cloud's keyring items, src/cloud/cloud_kr.c): the reader
 * thread hands the reply straight to the waiting thread. One abandoned after its time keeps its slot until the
 * reply comes, which is then wiped: it never reaches a queue. */
#define VC_WAIT_MAX 8
static struct { uint32_t id; int used, done, gone; char *json; size_t len; } waiters[VC_WAIT_MAX];
static pthread_cond_t wcv = PTHREAD_COND_INITIALIZER;

static int waiter_take(uint32_t id, char *json, size_t len) {     /* with mu held: 1 if it was a waited request */
    for (int i = 0; i < VC_WAIT_MAX; i++) {
        if (!waiters[i].used || waiters[i].done || waiters[i].id != id) continue;
        if (waiters[i].gone) { explicit_bzero(json, len); free(json); waiters[i].used = 0; return 1; }
        waiters[i].json = json;
        waiters[i].len = len;
        waiters[i].done = 1;
        pthread_cond_broadcast(&wcv);
        return 1;
    }
    return 0;
}

static void push(uint32_t id, uint32_t kind, char *json, size_t len) {
    if (kind == VW_REP) {
        pthread_mutex_lock(&mu);
        int w = waiter_take(id, json, len);
        if (w && inflight > 0) inflight--;
        pthread_mutex_unlock(&mu);
        if (w) return;
    }
    Ev *e = calloc(1, sizeof *e);
    if (!e) { explicit_bzero(json, len); free(json); return; }
    e->id = id; e->kind = kind; e->json = json; e->len = len;
    pthread_mutex_lock(&mu);
    int priv = kind == VW_REP && priv_take_id(id);
    if (priv) { if (ptail) ptail->next = e; else phead = e; ptail = e; }
    else { if (qtail) qtail->next = e; else qhead = e; qtail = e; }
    if (kind == VW_REP && inflight > 0) inflight--;
    pthread_mutex_unlock(&mu);
    if (priv) priv_wake();
}

/* lord-server's own C code told of lord-vault's events (Cloud: the keyring was unlocked); they still reach JS too */
static void (*event_hook)(const char *json, size_t len);
void vc_set_event_hook(void (*fn)(const char *json, size_t len)) { event_hook = fn; }

static void *read_loop(void *arg) {
    int s = (int)(intptr_t)arg;
    for (;;) {
        VMsg m;
        int r = vw_recv(s, &m, &vw_malloc);
        if (r) break;
        for (int i = 0; i < m.nfds; i++) close(m.fds[i]);
        if (m.kind == VW_EVT && event_hook && m.json) event_hook(m.json, m.len);   /* events carry no secret */
        push(m.id, m.kind, m.json, m.len);
    }
    /* it went away: requests without a reply end with an error */
    pthread_mutex_lock(&mu);
    int lost = inflight;
    inflight = 0;
    if (sock == s) sock = -1;
    uint32_t orphans[VC_PRIV_MAX];
    int norph = npriv;
    memcpy(orphans, priv_ids, sizeof(uint32_t) * (size_t)npriv);
    for (int i = 0; i < VC_WAIT_MAX; i++) {            /* waited requests end with "crash" too */
        if (!waiters[i].used || waiters[i].done) continue;
        if (waiters[i].gone) { waiters[i].used = 0; continue; }
        waiters[i].json = strdup("{\"error\":\"crash\"}");
        waiters[i].len = waiters[i].json ? strlen(waiters[i].json) : 0;
        waiters[i].done = 1;
    }
    pthread_cond_broadcast(&wcv);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < norph; i++) {                   /* private requests get a "crash" reply */
        char *cj = strdup("{\"error\":\"crash\"}");
        if (cj) push(orphans[i], VW_REP, cj, strlen(cj));
    }
    char *j = strdup("{\"ev\":\"crash\"}");
    if (j) push(0, VW_EVT, j, strlen(j));
    (void)lost;
    close(s);
    return NULL;
}

/* the session bus socket (unauthenticated: lord-vault does that) */
static int connect_bus(void) {
    const char *a = getenv("DBUS_SESSION_BUS_ADDRESS");
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    socklen_t sl = 0;
    if (a && !strncmp(a, "unix:", 5)) {
        const char *p = strstr(a, "path="), *ab = strstr(a, "abstract=");
        if (p) {
            p += 5;
            size_t n = strcspn(p, ",;");
            if (n >= sizeof sa.sun_path) return -1;
            memcpy(sa.sun_path, p, n);
            sl = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
        } else if (ab) {
            ab += 9;
            size_t n = strcspn(ab, ",;");
            if (n + 1 >= sizeof sa.sun_path) return -1;
            memcpy(sa.sun_path + 1, ab, n);
            sl = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
        }
    }
    if (!sl) {
        const char *rt = getenv("XDG_RUNTIME_DIR");
        if (!rt) return -1;
        snprintf(sa.sun_path, sizeof sa.sun_path, "%s/bus", rt);
        sl = (socklen_t)sizeof sa;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&sa, sl)) { close(fd); return -1; }
    return fd;
}

static int is_greeter(void) {
    struct passwd *pw = getpwuid(getuid());
    return pw && !strcmp(pw->pw_name, "lordos-greeter");
}

static int ctl(const char *json, int *fds, int nfds);

int vc_start(void) {
    pthread_mutex_lock(&mu);
    int running = sock >= 0;
    pthread_mutex_unlock(&mu);
    if (running) return 0;
    if (!vc_available() || is_greeter() || getuid() == 0) return -1;
    if (pid > 0) { waitpid(pid, NULL, WNOHANG); pid = 0; }
    /* vaults folder */
    const char *home = getenv("HOME");
    if (!home) return -1;
    char dir[PATH_MAX];
    const char *parts[] = { ".local", ".local/share", ".local/share/lordos", ".local/share/lordos/vault" };
    for (int i = 0; i < 4; i++) { snprintf(dir, sizeof dir, "%s/%s", home, parts[i]); mkdir(dir, 0700); }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dfd < 0) return -1;
    struct stat st;
    if (fstat(dfd, &st) || st.st_uid != getuid()) { close(dfd); return -1; }
    fchmod(dfd, 0700);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { close(dfd); return -1; }
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
    char henv[PATH_MAX + 8], *envp[6];
    int ne = 0;
    snprintf(henv, sizeof henv, "HOME=%s", home);
    envp[ne++] = henv;
    envp[ne++] = "LANG=C.UTF-8";
    if (getenv("LORD_VAULT_DEBUG")) envp[ne++] = "LORD_VAULT_DEBUG=1";
    envp[ne] = NULL;
    char *argv[] = { "lord-vault", NULL };
    pid_t p;
    int r = posix_spawn(&p, helper, &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(sv[1]);
    if (r) { close(sv[0]); close(dfd); return -1; }
    pid = p;
    int fds[2] = { dfd, connect_bus() };
    int nf = fds[1] >= 0 ? 2 : 1;
    if (vw_send(sv[0], VW_CTL, 0, "{\"op\":\"init\"}", 13, fds, nf)) { close(sv[0]); close(dfd); if (fds[1] >= 0) close(fds[1]); return -1; }
    close(dfd);
    if (fds[1] >= 0) close(fds[1]);
    /* the init reply also goes through the queue (id 0) */
    pthread_mutex_lock(&mu);
    sock = sv[0];
    pthread_mutex_unlock(&mu);
    if (pthread_create(&reader, NULL, read_loop, (void *)(intptr_t)sv[0]) == 0) { pthread_detach(reader); reader_on = 1; }
    fprintf(stderr, "lord-server: lord-vault started (pid %d)\n", (int)p);
    if (pending_pass) {
        vc_session_password(pending_pass);
        explicit_bzero(pending_pass, strlen(pending_pass));
        free(pending_pass);
        pending_pass = NULL;
    }
    return 0;
}

static int send_kind_p(uint32_t kind, const char *json, size_t len, int *fds, int nfds, int priv) {
    if (vc_start()) { for (int i = 0; i < nfds; i++) if (fds[i] >= 0) close(fds[i]); return -1; }
    pthread_mutex_lock(&mu);
    if (priv && npriv >= VC_PRIV_MAX) { pthread_mutex_unlock(&mu); return -1; }
    uint32_t id = next_id++;
    if (next_id > 0x7fffffff) next_id = 1;
    int s = sock;
    inflight++;
    if (priv) priv_ids[npriv++] = id;           /* registered before sending: the reply may come at once */
    pthread_mutex_unlock(&mu);
    int r = vw_send(s, kind, id, json, len, fds, nfds);
    for (int i = 0; i < nfds; i++) if (fds[i] >= 0) close(fds[i]);
    if (r) {
        pthread_mutex_lock(&mu);
        if (inflight) inflight--;
        if (priv) priv_take_id(id);
        pthread_mutex_unlock(&mu);
        return -1;
    }
    return (int)id;
}
static int send_kind(uint32_t kind, const char *json, size_t len, int *fds, int nfds) { return send_kind_p(kind, json, len, fds, nfds, 0); }

int vc_request(const char *json, size_t len, int *fds, int nfds) { return send_kind(VW_REQ, json, len, fds, nfds); }
static int ctl(const char *json, int *fds, int nfds) { return send_kind(VW_CTL, json, strlen(json), fds, nfds); }

int vc_private_fd(void) {
    pthread_mutex_lock(&mu);
    if (priv_efd < 0) priv_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    int f = priv_efd;
    pthread_mutex_unlock(&mu);
    return f;
}

int vc_request_wait(const char *json, size_t len, char **rep, size_t *rlen, int timeout_ms) {
    *rep = NULL;
    if (rlen) *rlen = 0;
    if (vc_start()) return -1;
    pthread_mutex_lock(&mu);
    int slot = -1;
    for (int i = 0; i < VC_WAIT_MAX && slot < 0; i++) if (!waiters[i].used) slot = i;
    if (slot < 0 || sock < 0) { pthread_mutex_unlock(&mu); return -1; }
    uint32_t id = next_id++;
    if (next_id > 0x7fffffff) next_id = 1;
    waiters[slot].id = id;
    waiters[slot].used = 1;
    waiters[slot].done = waiters[slot].gone = 0;
    waiters[slot].json = NULL;
    int s = sock;
    inflight++;
    pthread_mutex_unlock(&mu);
    if (vw_send(s, VW_REQ, id, json, len, NULL, 0)) {
        pthread_mutex_lock(&mu);
        waiters[slot].used = 0;
        if (inflight) inflight--;
        pthread_mutex_unlock(&mu);
        return -1;
    }
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += timeout_ms / 1000;
    t.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&mu);
    while (!waiters[slot].done) if (pthread_cond_timedwait(&wcv, &mu, &t)) break;
    int r = -1;
    if (waiters[slot].done) {
        *rep = waiters[slot].json;
        if (rlen) *rlen = waiters[slot].len;
        waiters[slot].used = 0;
        r = 0;
    } else waiters[slot].gone = 1;
    pthread_mutex_unlock(&mu);
    return r;
}

int vc_request_private(const char *json, size_t len) {
    vc_private_fd();
    return send_kind_p(VW_REQ, json, len, NULL, 0, 1);
}

char *vc_take_private(uint32_t *id, size_t *len) {
    pthread_mutex_lock(&mu);
    Ev *e = phead;
    if (e) { phead = e->next; if (!phead) ptail = NULL; }
    else if (priv_efd >= 0) { uint64_t v; ssize_t r = read(priv_efd, &v, sizeof v); (void)r; }   /* drained: re-arm */
    pthread_mutex_unlock(&mu);
    if (!e) return NULL;
    char *j = e->json;
    if (id) *id = e->id;
    if (len) *len = e->len;
    free(e);
    return j;
}

int vc_private_pending(void) {
    pthread_mutex_lock(&mu);
    int n = npriv + (phead != NULL);
    pthread_mutex_unlock(&mu);
    return n;
}

/* JSON of the text as a string inside another reply: data goes as-is (it is already JSON) */
char *vc_poll(void) {
    pthread_mutex_lock(&mu);
    Ev *list = qhead;
    qhead = qtail = NULL;
    pthread_mutex_unlock(&mu);
    if (!list) return NULL;
    size_t total = 3;
    for (Ev *e = list; e; e = e->next) total += e->len + 64;
    char *out = malloc(total);
    size_t o = 0;
    if (out) out[o++] = '[';
    for (Ev *e = list; e;) {
        Ev *n = e->next;
        if (out) {
            o += (size_t)snprintf(out + o, total - o, "%s{\"id\":%u,\"kind\":\"%s\",\"data\":", o > 1 ? "," : "", e->id,
                                  e->kind == VW_EVT ? "evt" : "rep");
            if (e->len && e->json[0]) { memcpy(out + o, e->json, e->len); o += e->len; }
            else { memcpy(out + o, "{}", 2); o += 2; }
            out[o++] = '}';
        }
        explicit_bzero(e->json, e->len);
        free(e->json);
        free(e);
        e = n;
    }
    if (out) { out[o++] = ']'; out[o] = 0; }
    return out;
}

void vc_poll_free(char *s) { if (s) { explicit_bzero(s, strlen(s)); free(s); } }

int vc_pending(void) {
    pthread_mutex_lock(&mu);
    int n = inflight + (qhead != NULL);
    pthread_mutex_unlock(&mu);
    return n;
}

void vc_lock_all(const char *why) {
    pthread_mutex_lock(&mu);
    int running = sock >= 0;
    pthread_mutex_unlock(&mu);
    if (!running) return;
    char j[96];
    snprintf(j, sizeof j, "{\"op\":\"lock-all\",\"why\":\"%s\"}", !strcmp(why, "logout") ? "logout" : !strcmp(why, "switch") ? "switch" : "screen");
    ctl(j, NULL, 0);
}

static void json_escape(char *out, size_t n, const char *s) {
    size_t o = 0;
    for (; *s && o + 7 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

void vc_session_password(const char *pass) {
    if (!pass || !pass[0]) return;
    if (vc_start()) {
        /* it could not be launched yet (lord-server is just starting): saved for when it is running */
        if (!pending_pass && vc_available() && !is_greeter()) pending_pass = strdup(pass);
        return;
    }
    char esc[1024 * 6 + 8], j[sizeof esc + 64];
    json_escape(esc, sizeof esc, pass);
    snprintf(j, sizeof j, "{\"op\":\"session-password\",\"password\":\"%s\"}", esc);
    ctl(j, NULL, 0);
    explicit_bzero(esc, sizeof esc);
    explicit_bzero(j, sizeof j);
}

int vc_fido_attach(char *name, size_t n) {
    FidoDev d[4];
    int k = fidodev_list(d, 4);
    if (k <= 0) return -1;
    int fd = fidodev_open(d[0].path);
    if (fd < 0) return -2;
    char esc[300], j[400];
    json_escape(esc, sizeof esc, d[0].name);
    snprintf(j, sizeof j, "{\"op\":\"fido-device\",\"name\":\"%s\"}", esc);
    if (name && n) snprintf(name, n, "%s", d[0].name);
    return ctl(j, &fd, 1) > 0 ? 0 : -1;
}

void vc_prompt_done(unsigned prompt, int ok) {
    char j[96];
    snprintf(j, sizeof j, "{\"op\":\"prompt-done\",\"prompt\":%u,\"ok\":%s}", prompt, ok ? "true" : "false");
    ctl(j, NULL, 0);
}

int vc_attach_app(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    int give = sv[1];
    if (ctl("{\"op\":\"attach\"}", &give, 1) < 0) { close(sv[0]); return -1; }
    return sv[0];
}
