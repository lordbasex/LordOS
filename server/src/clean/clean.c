/* clean.c — see clean.h. */
#include "clean.h"
#include "cleanfs.h"
#include "cleanproto.h"
#include "rules.h"
#include "../doc/wire.h"
#include "../fsops.h"
#include "../sec/json.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define IDLE_MS 30000                       /* the helper reports at least every ~1024 entries */
#define SAFE_INT 9007199254740992.0         /* 2^53: exact in a JSON number */

enum { JOB_NONE, JOB_SCAN, JOB_PLAN, JOB_EXEC, JOB_EMPTY };
enum { ST_IDLE, ST_RUNNING, ST_DONE, ST_FAILED, ST_CANCELLED };
static const char *const job_name[] = { "none", "scan", "plan", "execute", "emptyTrash" };
static const char *const st_name[] = { "idle", "running", "done", "failed", "cancelled" };

/* a plan item's state; the last seven are the results an item can end with */
enum { I_READY, I_DONE, I_SKIPPED, I_CHANGED, I_DENIED, I_FAILED, I_CANCELLED, I_UNSUPPORTED, I_COUNT };
static const char *const item_name[] = { "ready", "done", "skipped", "changed", "denied", "failed", "cancelled", "unsupported" };

enum { ROOT_OK, ROOT_ABSENT, ROOT_DENIED, ROOT_CHANGED };
static const char *const root_name[] = { "ok", "absent", "denied", "changed" };

typedef struct {
    uint64_t ino, dev;
    int64_t size, alloc, mtime;
    uint32_t nsec, nlink;
    uint32_t links;                         /* first link of an inode: how many the scan saw */
    uint32_t selected;                      /* first link of an inode: how many the plan holds */
    uint32_t first;                         /* a later link: index + 1 of the first; 0 for the first */
    uint16_t rule;
    char *rel;
} Cand;

typedef struct { long files; double logical, allocated; } Tot;

typedef struct {
    int id, complete;
    Cand *c;
    size_t n, cap;
    uint32_t *ht;                           /* (dev, ino) → index + 1, open addressing */
    size_t htcap;
    long seen, dirs, symlinks, special, mounts, unreadable, protect, names, nomatch, refused, foreign;
    char limit[8];
    double logical, allocated;
    int nrules, root_state[CLEAN_MAX_ROOTS];
    dev_t root_dev[CLEAN_MAX_ROOTS];
    Tot tot[CLEAN_MAX_ROOTS];
} Scan;

typedef struct { uint32_t cand; uint8_t status; } Item;

enum { P_BUILDING, P_READY, P_EXECUTING, P_FINISHED, P_DEAD };
static const char *const plan_name[] = { "building", "ready", "executing", "finished", "dead" };

typedef struct {
    int id, state;
    Item *it;
    size_t n, done;
    long count[I_COUNT];
    double logical, allocated, estimated;
    long trash_items, delete_items;
    double trash_alloc, delete_alloc;
    double expires, free_before, free_after;
    int links_elsewhere;
} Plan;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int job = JOB_NONE, state = ST_IDLE, forget_req;
static volatile int cancel_req;
static char error[16];
static pid_t child = -1;
static Scan *scan;
static Plan *plan;
static int next_id = 1;
static long empty_items = -1;
static double empty_before, empty_after;

#ifdef CLEAN_TEST
long clean_test_max_files = CLEAN_MAX_FILES;
int  clean_test_plan_ttl = CLEAN_PLAN_TTL_S;
int  clean_test_stop_helper;
void (*clean_test_before_trash)(const char *real);
int clean_test_helper_pid(void) { pthread_mutex_lock(&mu); int p = child; pthread_mutex_unlock(&mu); return p; }
#define MAX_FILES clean_test_max_files
#define PLAN_TTL  clean_test_plan_ttl
#else
#define MAX_FILES CLEAN_MAX_FILES
#define PLAN_TTL  CLEAN_PLAN_TTL_S
#endif

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static double free_bytes(void) {
    struct statvfs v;
    return statvfs(fsops_home(), &v) ? -1 : (double)v.f_bavail * (double)v.f_frsize;
}

static void scan_free(Scan *s) {
    if (!s) return;
    for (size_t i = 0; i < s->n; i++) free(s->c[i].rel);
    free(s->c);
    free(s->ht);
    free(s);
}
static void plan_free(Plan *p) { if (p) { free(p->it); free(p); } }

/* with mu held */
static void forget_all(void) {
    plan_free(plan);
    plan = NULL;
    scan_free(scan);
    scan = NULL;
    job = JOB_NONE;
    state = ST_IDLE;
    error[0] = 0;
}

/* the job ended; every worker's last step */
static void job_end(int st, const char *why) {
    pthread_mutex_lock(&mu);
    state = st;
    snprintf(error, sizeof error, "%s", why ? why : "");
    if (forget_req) forget_all();
    forget_req = 0;
    pthread_mutex_unlock(&mu);
}

/* ---- the helper's fixed path, next to lord-server (as lord-bench and lord-doc) ---- */

static int helper_path(char *out, size_t n) {
    char self[PATH_MAX];
    ssize_t k = readlink("/proc/self/exe", self, sizeof self - 1);
    if (k <= 0) return -1;
    self[k] = 0;
    char *slash = strrchr(self, '/');
    if (!slash) return -1;
    *slash = 0;
    if ((size_t)snprintf(out, n, "%s/lord-clean", self) >= n) return -1;
    struct stat st;
    if (stat(out, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) return -1;
    if ((st.st_uid != 0 && st.st_uid != getuid()) || (st.st_mode & (S_IWGRP | S_IWOTH))) return -1;
    return 0;
}

int clean_available(void) {
    char p[PATH_MAX];
    return !helper_path(p, sizeof p) && fsops_home()[0] == '/';
}

static int spawn_helper(const char *path, const char *go, size_t len, const int *fds, int nfd, pid_t *pid) {
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
    char sessiond[32] = "LORDOS_SESSIOND=1";
    char *envp[] = { "LANG=C.UTF-8", getenv("LORDOS_SESSIOND") ? sessiond : NULL, NULL };
    char *argv[] = { "lord-clean", NULL };
    int e = posix_spawn(pid, path, &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(sv[1]);
    if (e) { close(sv[0]); return -1; }
#ifdef CLEAN_TEST
    if (clean_test_stop_helper) kill(*pid, SIGSTOP);
#endif
    if (doc_send(sv[0], CQ_GO, 1, go, len, fds, nfd)) {
        kill(*pid, SIGKILL);
        waitpid(*pid, NULL, 0);
        close(sv[0]);
        return -1;
    }
    return sv[0];
}

/* ---- what the helper reports: nothing here is trusted ---- */

static int json_count(const JVal *o, const char *key, double *out) {
    const JVal *v = json_get(o, key);
    if (!v || v->type != J_NUM || v->num < 0 || v->num > SAFE_INT || v->num != floor(v->num)) return -1;
    *out = v->num;
    return 0;
}

static int parse_ino(const char *s, uint64_t *out) {
    if (!s || !*s || strlen(s) > 20) return -1;
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        unsigned d = (unsigned)(*s - '0');
        if (v > (UINT64_MAX - d) / 10) return -1;
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

static uint64_t hash_id(uint64_t dev, uint64_t ino) {
    uint64_t h = (dev ^ (ino * 0x9e3779b97f4a7c15ull)) * 0xff51afd7ed558ccdull;
    return h ^ (h >> 33);
}

/* with mu held: the first candidate with that device and inode, or NULL; else it goes in as index i */
static Cand *link_lookup(Scan *s, uint64_t dev, uint64_t ino, uint32_t i) {
    if ((s->n + 1) * 2 > s->htcap) {
        size_t cap = s->htcap ? s->htcap * 2 : 1024;
        uint32_t *ht = calloc(cap, sizeof *ht);
        if (!ht) return NULL;
        for (size_t k = 0; k < s->htcap; k++) if (s->ht[k]) {
            const Cand *c = &s->c[s->ht[k] - 1];
            size_t h = hash_id(c->dev, c->ino) & (cap - 1);
            while (ht[h]) h = (h + 1) & (cap - 1);
            ht[h] = s->ht[k];
        }
        free(s->ht);
        s->ht = ht;
        s->htcap = cap;
    }
    size_t h = hash_id(dev, ino) & (s->htcap - 1);
    for (; s->ht[h]; h = (h + 1) & (s->htcap - 1)) {
        Cand *c = &s->c[s->ht[h] - 1];
        if (c->dev == dev && c->ino == ino) return c;
    }
    s->ht[h] = i + 1;
    return NULL;
}

static int protected_path(const CleanRule *r, const char *rel) {
    char abs[PATH_MAX * 2 + CLEAN_PATH_MAX];
    if ((size_t)snprintf(abs, sizeof abs, "%s/%s/%s", fsops_home(), r->root, rel) >= sizeof abs) return 1;
    return fsops_protected(abs);
}

static int rule_matches(const CleanRule *r, const char *name, double mtime) {
    if (!clean_glob(r->pattern, name)) return 0;
    for (const char *const *e = r->exclude; e && *e; e++) if (clean_glob(*e, name)) return 0;
    return !r->min_age_days || (double)time(NULL) - mtime >= r->min_age_days * 86400.0;
}

/* One record of a batch. 0 kept, 1 dropped and counted, -1 the helper broke the protocol. With mu held. */
static int take_record(Scan *s, const JVal *rec) {
    double r, maj, min, nl, size, alloc, mt, ns, uid;   /* r: the rule's index */
    uint64_t ino;
    const char *rel = json_str(rec, "p");
    if (json_count(rec, "r", &r) || json_count(rec, "M", &maj) || json_count(rec, "m", &min) || json_count(rec, "l", &nl) ||
        json_count(rec, "s", &size) || json_count(rec, "a", &alloc) || json_count(rec, "t", &mt) || json_count(rec, "n", &ns) ||
        json_count(rec, "u", &uid) || parse_ino(json_str(rec, "i"), &ino) || !rel) return -1;
    if (r >= s->nrules || maj > 0xffffffffu || min > 0xffffffffu || nl < 1 || nl > 0xffffffffu || ns >= 1e9 || uid > 0xffffffffu) return -1;
    if (!clean_relpath_ok(rel, CLEAN_MAX_DEPTH)) return -1;
    int nrules;
    const CleanRule *rule = &clean_rules(&nrules)[(int)r];
    dev_t dev = makedev((unsigned)maj, (unsigned)min);
    if (s->root_state[(int)r] != ROOT_OK || dev != s->root_dev[(int)r]) return -1;   /* it never crosses a device */
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    if (protected_path(rule, rel)) { s->protect++; s->refused++; return 1; }
    if (!rule_matches(rule, base, mt)) { s->refused++; return 1; }
    if ((uid_t)uid != getuid()) { s->foreign++; return 1; }
    if (s->n >= (size_t)MAX_FILES) return -1;

    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 1024;
        Cand *c = realloc(s->c, cap * sizeof *c);
        if (!c) return -1;
        s->c = c;
        s->cap = cap;
    }
    char *copy = strdup(rel);
    if (!copy) return -1;
    Cand *first = link_lookup(s, dev, ino, (uint32_t)s->n);
    if (!s->ht) { free(copy); return -1; }
    Cand *c = &s->c[s->n];
    *c = (Cand){ .ino = ino, .dev = dev, .size = (int64_t)size, .alloc = (int64_t)alloc, .mtime = (int64_t)mt, .nsec = (uint32_t)ns,
                 .nlink = (uint32_t)nl, .links = 1, .rule = (uint16_t)r, .rel = copy };
    if (first) {                                /* a hard link: one file, counted once */
        c->first = (uint32_t)(first - s->c) + 1;
        first->links++;
    } else {
        s->logical += size;
        s->allocated += alloc;
        s->tot[(int)r].logical += size;
        s->tot[(int)r].allocated += alloc;
    }
    s->tot[(int)r].files++;
    s->n++;
    return 0;
}

/* A candidate's bytes a cleanup could give back: its inode's, once all its links are in the scan */
static double estimated(const Cand *c) {
    return !c->first && c->links >= c->nlink ? (double)c->alloc : 0;
}

static int take_batch(Scan *s, const JVal *root) {
    double seen, dirs;
    const JVal *items = json_get(root, "items");
    if (json_count(root, "seen", &seen) || json_count(root, "dirs", &dirs) || !items || items->type != J_ARR ||
        items->len > CLEAN_BATCH) return -1;
    int r = 0;
    pthread_mutex_lock(&mu);
    s->seen = (long)seen;
    s->dirs = (long)dirs;
    for (const JVal *v = items->kid; v && r >= 0; v = v->next) if (take_record(s, v) < 0) r = -1;
    pthread_mutex_unlock(&mu);
    return r;
}

static int take_done(Scan *s, const JVal *root) {
    double v[9];
    static const char *const key[] = { "seen", "dirs", "symlinks", "special", "mounts", "unreadable", "protected", "names", "nomatch" };
    for (int i = 0; i < 9; i++) if (json_count(root, key[i], &v[i])) return -1;
    const char *limit = json_str(root, "limit");
    if (!limit || (*limit && strcmp(limit, "files") && strcmp(limit, "depth"))) return -1;
    pthread_mutex_lock(&mu);
    s->seen = (long)v[0];
    s->dirs = (long)v[1];
    s->symlinks = (long)v[2];
    s->special = (long)v[3];
    s->mounts = (long)v[4];
    s->unreadable = (long)v[5];
    s->protect += (long)v[6];
    s->names = (long)v[7];
    s->nomatch += (long)v[8];
    snprintf(s->limit, sizeof s->limit, "%s", limit);
    s->complete = !*limit && !s->unreadable && !s->names;
    for (int i = 0; i < s->nrules; i++) if (s->root_state[i] == ROOT_DENIED || s->root_state[i] == ROOT_CHANGED) s->complete = 0;
    pthread_mutex_unlock(&mu);
    return 0;
}

#ifdef CLEAN_TEST
int clean_test_record(const char *json, int nrules) {
    static Scan s;
    int r = -1;
    JDoc d = { 0 };
    memset(&s, 0, sizeof s);
    s.nrules = nrules;
    struct stat st;
    if (stat(fsops_home(), &st)) return -1;
    for (int i = 0; i < nrules && i < CLEAN_MAX_ROOTS; i++) s.root_dev[i] = st.st_dev;
    if (json_parse(json, strlen(json), &d, NULL, 0)) return -1;
    r = take_record(&s, d.root);
    json_free(&d);
    for (size_t i = 0; i < s.n; i++) free(s.c[i].rel);
    free(s.c);
    free(s.ht);
    return r;
}
#endif

/* ---- the scan ---- */

static int root_failure(int e) {
    if (e == ENOENT) return ROOT_ABSENT;
    return e == EACCES || e == EPERM ? ROOT_DENIED : ROOT_CHANGED;     /* a link or a mount where a folder was: not offered */
}

static void *scan_thread(void *arg) {
    Scan *s = arg;
    int st = ST_FAILED, sock = -1, fds[CLEAN_MAX_ROOTS], nfd = 0, nrules, home = -1, finished = 0;
    const char *why = "helper";
    pid_t pid = -1;
    char path[PATH_MAX];
    const CleanRule *rules = clean_rules(&nrules);
    JBuf go;
    jb_init(&go);
    if (helper_path(path, sizeof path)) goto end;
    home = open(fsops_home(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (home < 0) goto end;
    jb_printf(&go, "{\"maxFiles\":%ld,\"maxDepth\":%d,\"home\":", (long)MAX_FILES, CLEAN_MAX_DEPTH);
    jb_str(&go, fsops_home());
    jb_raw(&go, ",\"roots\":[");
    for (int i = 0; i < nrules && i < CLEAN_MAX_ROOTS; i++) {
        int fd = clean_open(home, rules[i].root, O_RDONLY | O_DIRECTORY);
        struct stat sb;
        if (fd >= 0 && fstat(fd, &sb)) { close(fd); fd = -1; errno = EIO; }
        if (fd < 0) { s->root_state[i] = root_failure(errno); continue; }
        s->root_dev[i] = sb.st_dev;
        fds[nfd++] = fd;
        char base[PATH_MAX * 2];
        snprintf(base, sizeof base, "%s/%s", fsops_home(), rules[i].root);
        jb_printf(&go, "%s{\"rule\":%d,\"pattern\":", nfd > 1 ? "," : "", i);
        jb_str(&go, rules[i].pattern);
        jb_raw(&go, ",\"base\":");
        jb_str(&go, base);
        jb_raw(&go, "}");
    }
    jb_raw(&go, "]}");
    close(home);
    if (go.oom) goto end;
    if (!nfd) {                                     /* nothing to walk */
        pthread_mutex_lock(&mu);
        s->complete = 1;
        for (int i = 0; i < s->nrules; i++) if (s->root_state[i] != ROOT_ABSENT) s->complete = 0;
        pthread_mutex_unlock(&mu);
        st = ST_DONE;
        why = NULL;
        goto end;
    }
    sock = spawn_helper(path, go.p, go.n, fds, nfd, &pid);
    for (int i = 0; i < nfd; i++) close(fds[i]);
    nfd = 0;
    if (sock < 0) goto end;
    pthread_mutex_lock(&mu);
    child = pid;
    if (cancel_req) kill(pid, SIGKILL);
    pthread_mutex_unlock(&mu);

    double deadline = now_ms() + IDLE_MS;
    while (!finished) {
        if (cancel_req) { st = ST_CANCELLED; why = NULL; break; }
        if (now_ms() > deadline) { why = "timeout"; break; }
        struct pollfd pf = { sock, POLLIN, 0 };
        int pr = poll(&pf, 1, 100);
        if (pr < 0 && errno != EINTR) break;
        if (pr <= 0) continue;
        DocMsg m;
        int r = doc_recv(sock, &m);
        if (r) break;                               /* it died without CR_DONE */
        JDoc d = { 0 };
        int bad = !m.json || json_parse(m.json, m.len, &d, NULL, 0);
        if (bad) why = "protocol";
        else if (m.h.op == CR_BATCH) {
            deadline = now_ms() + IDLE_MS;
            if (take_batch(s, d.root)) { bad = 1; why = "protocol"; }
        } else if (m.h.op == CR_DONE) {
            if (take_done(s, d.root)) { bad = 1; why = "protocol"; }
            else { finished = 1; st = ST_DONE; why = NULL; }
        } else if (m.h.op == CR_ERR) {
            const char *e = json_str(d.root, "error");
            why = e && !strcmp(e, "sandbox") ? "sandbox" : "helper";
            bad = 1;
        } else { bad = 1; why = "protocol"; }
        json_free(&d);
        doc_msg_free(&m);
        if (bad) break;
    }
    if (cancel_req && st != ST_DONE) { st = ST_CANCELLED; why = NULL; }     /* the kill may be seen as a closed channel */
end:
    jb_free(&go);
    for (int i = 0; i < nfd; i++) close(fds[i]);
    pthread_mutex_lock(&mu);
    child = -1;
    pthread_mutex_unlock(&mu);
    if (pid > 0) {                                  /* a done helper exits by itself; SIGKILL is harmless then */
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    if (sock >= 0) close(sock);
    if (st != ST_DONE) {                            /* nothing from a failed or cancelled scan is offered */
        pthread_mutex_lock(&mu);
        for (size_t i = 0; i < s->n; i++) free(s->c[i].rel);
        s->n = 0;
        s->complete = 0;
        s->logical = s->allocated = 0;
        memset(s->tot, 0, sizeof s->tot);
        if (s->ht) memset(s->ht, 0, s->htcap * sizeof *s->ht);
        pthread_mutex_unlock(&mu);
    }
    job_end(st, why);
    return NULL;
}

static int start_thread(void *(*fn)(void *), void *arg) {
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);         /* the thread doesn't take the server's signals */
    int e = pthread_create(&th, &a, fn, arg);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&a);
    return e ? -1 : 0;
}

/* with mu held */
static int begin(int kind) {
    if (state == ST_RUNNING) return -1;
    job = kind;
    state = ST_RUNNING;
    cancel_req = 0;
    forget_req = 0;
    error[0] = 0;
    return 0;
}

int clean_start(void) {
    if (!clean_available()) return CLEAN_E_UNAVAILABLE;
    Scan *s = calloc(1, sizeof *s);
    if (!s) return CLEAN_E_UNAVAILABLE;
    pthread_mutex_lock(&mu);
    if (begin(JOB_SCAN)) { pthread_mutex_unlock(&mu); free(s); return CLEAN_E_BUSY; }
    plan_free(plan);
    plan = NULL;
    scan_free(scan);
    s->id = next_id++;
    int nrules;
    clean_rules(&nrules);
    s->nrules = nrules > CLEAN_MAX_ROOTS ? CLEAN_MAX_ROOTS : nrules;
    scan = s;
    int id = s->id;
    if (start_thread(scan_thread, s)) {
        scan = NULL;
        state = ST_IDLE;
        job = JOB_NONE;
        pthread_mutex_unlock(&mu);
        free(s);
        return CLEAN_E_UNAVAILABLE;
    }
    pthread_mutex_unlock(&mu);
    return id;
}

void clean_cancel(int forget) {
    pthread_mutex_lock(&mu);
    if (state == ST_RUNNING) {
        cancel_req = 1;
        if (forget) forget_req = 1;
        if (child > 0) kill(child, SIGKILL);        /* cancel kills the helper at once */
    } else if (forget) forget_all();
    pthread_mutex_unlock(&mu);
}

/* ---- reaching a candidate again ---- */

/* Each rule's root, opened from the home folder for this job. rootfd[i] < 0: root_err[i] holds the item status. */
static void open_roots(int *rootfd, int *root_err) {
    int nrules;
    const CleanRule *rules = clean_rules(&nrules);
    int home = open(fsops_home(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    for (int i = 0; i < nrules && i < CLEAN_MAX_ROOTS; i++) {
        rootfd[i] = home < 0 ? -1 : clean_open(home, rules[i].root, O_PATH | O_DIRECTORY);
        if (rootfd[i] < 0) root_err[i] = errno == EACCES || errno == EPERM ? I_DENIED : I_CHANGED;
    }
    if (home >= 0) close(home);
}
static void close_roots(int *rootfd) {
    int nrules;
    clean_rules(&nrules);
    for (int i = 0; i < nrules && i < CLEAN_MAX_ROOTS; i++) if (rootfd[i] >= 0) close(rootfd[i]);
}

static int status_of_errno(int e) {
    if (e == EACCES || e == EPERM || e == EROFS) return I_DENIED;
    if (e == ENOENT || e == ENOTDIR || e == ELOOP || e == EXDEV) return I_CHANGED;    /* not the path the scan saw */
    return I_FAILED;
}

/* Is this still the object the scan saw? I_READY with its folder open in *parent and its name in *name;
 * otherwise the status. Runs for every item at plan time and again right before the action. */
static int item_check(const Cand *c, const int *rootfd, const int *root_err, int *parent, const char **name) {
    int nrules;
    const CleanRule *rule = &clean_rules(&nrules)[c->rule];
    *parent = -1;
    if (rule->risk == CLEAN_RISK_PROTECTED || (rule->action != CLEAN_ACT_TRASH && rule->action != CLEAN_ACT_DELETE) ||
        (rule->action == CLEAN_ACT_DELETE && rule->risk != CLEAN_RISK_REGENERABLE)) return I_UNSUPPORTED;
    if (protected_path(rule, c->rel)) return I_DENIED;
    if (rootfd[c->rule] < 0) return root_err[c->rule];
    const char *base = strrchr(c->rel, '/');
    int fd;
    if (base) {
        char dir[CLEAN_PATH_MAX + 1];
        snprintf(dir, sizeof dir, "%.*s", (int)(base - c->rel), c->rel);
        fd = clean_open(rootfd[c->rule], dir, O_PATH | O_DIRECTORY);
        base++;
    } else {
        fd = fcntl(rootfd[c->rule], F_DUPFD_CLOEXEC, 0);
        base = c->rel;
    }
    if (fd < 0) return status_of_errno(errno);
    struct statx sx;
    if (statx(fd, base, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, STATX_TYPE | STATX_UID | STATX_INO | STATX_SIZE | STATX_MTIME, &sx)) {
        int e = errno;
        close(fd);
        return status_of_errno(e);
    }
    int st = I_READY;
    if (!S_ISREG(sx.stx_mode) || makedev(sx.stx_dev_major, sx.stx_dev_minor) != c->dev || sx.stx_ino != c->ino ||
        (int64_t)sx.stx_size != c->size || sx.stx_mtime.tv_sec != c->mtime || sx.stx_mtime.tv_nsec != c->nsec) st = I_CHANGED;
    else if (sx.stx_uid != getuid()) st = I_DENIED;
    else if (!rule_matches(rule, base, (double)c->mtime)) st = I_SKIPPED;
    if (st != I_READY) { close(fd); return st; }
    *parent = fd;
    *name = base;
    return I_READY;
}

/* ---- the plan ---- */

static void *plan_thread(void *arg) {
    Plan *p = arg;
    Scan *s = scan;                                 /* stays: a job is running, so nothing frees it */
    int rootfd[CLEAN_MAX_ROOTS], root_err[CLEAN_MAX_ROOTS];
    memset(root_err, 0, sizeof root_err);
    open_roots(rootfd, root_err);
    long count[I_COUNT] = { 0 };
    double logical = 0, allocated = 0, est = 0;
    int cancelled = 0;
    for (size_t i = 0; i < p->n; i++) {
        if (cancel_req) { cancelled = 1; break; }
        const Cand *c = &s->c[p->it[i].cand];
        int fd = -1;
        const char *name;
        int st = item_check(c, rootfd, root_err, &fd, &name);
        if (fd >= 0) close(fd);
        pthread_mutex_lock(&mu);
        p->it[i].status = (uint8_t)st;
        pthread_mutex_unlock(&mu);
        count[st]++;
        if (st != I_READY) continue;
        s->c[c->first ? c->first - 1 : p->it[i].cand].selected++;
        if (!c->first) { logical += (double)c->size; allocated += (double)c->alloc; }
    }
    /* what the plan could give back: an inode with every link selected */
    int elsewhere = 0;
    for (size_t i = 0; i < p->n && !cancelled; i++) {
        if (p->it[i].status != I_READY) continue;
        const Cand *c = &s->c[p->it[i].cand];
        if (c->first) continue;
        if (c->selected >= c->nlink) est += (double)c->alloc;
        else elsewhere = 1;
    }
    for (size_t i = 0; i < s->n; i++) s->c[i].selected = 0;
    long trash_items = 0, delete_items = 0;
    double trash_alloc = 0, delete_alloc = 0;
    int nrules;
    const CleanRule *rules = clean_rules(&nrules);
    for (size_t i = 0; i < p->n; i++) {
        if (p->it[i].status != I_READY) continue;
        const Cand *c = &s->c[p->it[i].cand];
        int del = rules[c->rule].action == CLEAN_ACT_DELETE;
        if (del) delete_items++; else trash_items++;
        if (!c->first) { if (del) delete_alloc += (double)c->alloc; else trash_alloc += (double)c->alloc; }
    }
    close_roots(rootfd);
    pthread_mutex_lock(&mu);
    p->trash_items = trash_items;
    p->delete_items = delete_items;
    p->trash_alloc = trash_alloc;
    p->delete_alloc = delete_alloc;
    memcpy(p->count, count, sizeof count);
    p->logical = logical;
    p->allocated = allocated;
    p->estimated = est;
    p->links_elsewhere = elsewhere;
    p->state = cancelled ? P_DEAD : P_READY;
    p->expires = now_ms() + PLAN_TTL * 1000.0;
    pthread_mutex_unlock(&mu);
    job_end(cancelled ? ST_CANCELLED : ST_DONE, NULL);
    return NULL;
}

/* with mu held; takes it */
static int plan_begin(Item *it, size_t k) {
    Plan *p = calloc(1, sizeof *p);
    if (!p) { free(it); return CLEAN_E_UNAVAILABLE; }
    p->id = next_id++;
    p->it = it;
    p->n = k;
    p->state = P_BUILDING;
    plan_free(plan);
    plan = p;
    begin(JOB_PLAN);
    if (start_thread(plan_thread, p)) { plan = NULL; state = ST_IDLE; plan_free(p); return CLEAN_E_UNAVAILABLE; }
    return p->id;
}

int clean_plan(int sid, const uint32_t *ids, size_t n) {
    pthread_mutex_lock(&mu);
    int r;
    if (state == ST_RUNNING) r = CLEAN_E_BUSY;
    else if (!scan || scan->id != sid || job == JOB_NONE) r = CLEAN_E_NOSCAN;
    else if (!n || n > CLEAN_MAX_FILES) r = CLEAN_E_BADID;
    else {
        unsigned char *seen = calloc(scan->n ? scan->n : 1, 1);
        Item *it = malloc(n * sizeof *it);
        size_t k = 0;
        int ok = seen && it;
        for (size_t i = 0; i < n && ok; i++) {
            if (ids[i] < 1 || ids[i] > scan->n) { ok = 0; break; }
            if (seen[ids[i] - 1]) continue;         /* the same id twice is one item */
            seen[ids[i] - 1] = 1;
            it[k++] = (Item){ ids[i] - 1, I_READY };
        }
        free(seen);
        if (ok) r = plan_begin(it, k);
        else { free(it); r = CLEAN_E_BADID; }
    }
    pthread_mutex_unlock(&mu);
    return r;
}

int clean_plan_rules(int sid, const char *const *names, int n) {
    int nrules;
    const CleanRule *rules = clean_rules(&nrules);
    unsigned wanted = 0;
    for (int i = 0; i < n; i++) {
        int found = 0;
        for (int k = 0; k < nrules && k < CLEAN_MAX_ROOTS; k++) if (!strcmp(names[i], rules[k].id)) { wanted |= 1u << k; found = 1; }
        if (!found) return CLEAN_E_BADID;
    }
    pthread_mutex_lock(&mu);
    int r;
    if (state == ST_RUNNING) r = CLEAN_E_BUSY;
    else if (!scan || scan->id != sid || job == JOB_NONE) r = CLEAN_E_NOSCAN;
    else {
        size_t k = 0;
        Item *it = malloc((scan->n ? scan->n : 1) * sizeof *it);
        if (!it) r = CLEAN_E_UNAVAILABLE;
        else {
            for (size_t i = 0; i < scan->n; i++) if (wanted >> scan->c[i].rule & 1) it[k++] = (Item){ (uint32_t)i, I_READY };
            if (k) r = plan_begin(it, k);
            else { free(it); r = CLEAN_E_BADID; }
        }
    }
    pthread_mutex_unlock(&mu);
    return r;
}

/* ---- execution ---- */

static int same_file(int fd, const char *name, const Cand *c) {
    struct statx sx;
    if (statx(fd, name, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, STATX_TYPE | STATX_INO | STATX_SIZE | STATX_MTIME, &sx)) return 0;
    return S_ISREG(sx.stx_mode) && makedev(sx.stx_dev_major, sx.stx_dev_minor) == c->dev && sx.stx_ino == c->ino &&
           (int64_t)sx.stx_size == c->size && sx.stx_mtime.tv_sec == c->mtime && sx.stx_mtime.tv_nsec == c->nsec;
}

/* The real delete, for a regenerable rule's file only, in the folder item_check opened beneath the root:
 * nothing is resolved by path here. The name moves to a private one in the same folder, the object now
 * behind that private name is compared, and only that object is unlinked. Anything else goes back. */
static int delete_file(int fd, const char *name, const Cand *c) {
    static unsigned serial;
    char tmp[64];
    snprintf(tmp, sizeof tmp, ".lordos-clean-%d-%u", (int)getpid(), __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED));
    if (renameat2(fd, name, fd, tmp, RENAME_NOREPLACE)) return status_of_errno(errno);
    if (!same_file(fd, tmp, c)) return renameat2(fd, tmp, fd, name, RENAME_NOREPLACE) ? I_FAILED : I_CHANGED;
    if (unlinkat(fd, tmp, 0)) {
        int e = errno;
        return renameat2(fd, tmp, fd, name, RENAME_NOREPLACE) ? I_FAILED : status_of_errno(e);
    }
    return I_DONE;
}

static int trash_file(int fd, const char *name, const char *real, const Cand *c) {
    char dest[PATH_MAX * 2], err[256];
    if (fstrash_put_at(fd, name, real, dest, sizeof dest, err, sizeof err))
        return errno == EXDEV ? I_UNSUPPORTED : status_of_errno(errno);
    /* the name could have been pointed at another file between the check and the move: then that file
     * goes back where it was */
    struct stat sb;
    if (lstat(dest, &sb) || sb.st_ino != c->ino || sb.st_dev != c->dev)
        return fstrash_restore(dest, err, sizeof err) ? I_FAILED : I_CHANGED;
    return I_DONE;
}

static void *exec_thread(void *arg) {
    Plan *p = arg;
    const Scan *s = scan;
    int rootfd[CLEAN_MAX_ROOTS], root_err[CLEAN_MAX_ROOTS], nrules, cancelled = 0;
    const CleanRule *rules = clean_rules(&nrules);
    memset(root_err, 0, sizeof root_err);
    open_roots(rootfd, root_err);
    double before = free_bytes();
    pthread_mutex_lock(&mu);
    p->free_before = before;
    pthread_mutex_unlock(&mu);
    for (size_t i = 0; i < p->n; i++) {
        int st = p->it[i].status;
        if (st == I_READY) {
            const Cand *c = &s->c[p->it[i].cand];
            int fd = -1;
            const char *name = NULL;
            if (cancel_req) { cancelled = 1; st = I_CANCELLED; }
            else st = item_check(c, rootfd, root_err, &fd, &name);
            if (st == I_READY) {
                char real[PATH_MAX * 2 + CLEAN_PATH_MAX];
                snprintf(real, sizeof real, "%s/%s/%s", fsops_home(), rules[c->rule].root, c->rel);
#ifdef CLEAN_TEST
                if (clean_test_before_trash) clean_test_before_trash(real);
#endif
                st = rules[c->rule].action == CLEAN_ACT_DELETE ? delete_file(fd, name, c) : trash_file(fd, name, real, c);
            }
            if (fd >= 0) close(fd);
        }
        pthread_mutex_lock(&mu);
        p->count[p->it[i].status]--;
        p->count[st]++;
        p->it[i].status = (uint8_t)st;
        p->done = i + 1;
        pthread_mutex_unlock(&mu);
    }
    close_roots(rootfd);
    double after = free_bytes();
    pthread_mutex_lock(&mu);
    p->free_after = after;
    p->state = P_FINISHED;
    pthread_mutex_unlock(&mu);
    job_end(cancelled ? ST_CANCELLED : ST_DONE, NULL);
    return NULL;
}

static void *empty_thread(void *arg) {
    (void)arg;
    double before = free_bytes();
    long n = cancel_req ? 0 : fstrash_empty();
    double after = free_bytes();
    pthread_mutex_lock(&mu);
    empty_items = n;
    empty_before = before;
    empty_after = after;
    pthread_mutex_unlock(&mu);
    job_end(ST_DONE, NULL);
    return NULL;
}

int clean_empty_trash(void) {
    pthread_mutex_lock(&mu);
    int r = 0;
    if (state == ST_RUNNING) r = CLEAN_E_BUSY;
    else {
        begin(JOB_EMPTY);
        empty_items = -1;
        if (start_thread(empty_thread, NULL)) { state = ST_IDLE; r = CLEAN_E_UNAVAILABLE; }
    }
    pthread_mutex_unlock(&mu);
    return r;
}

int clean_execute(int id) {
    pthread_mutex_lock(&mu);
    int r;
    if (state == ST_RUNNING) r = CLEAN_E_BUSY;
    else if (!plan || plan->id != id || plan->state != P_READY) r = CLEAN_E_NOPLAN;
    else if (now_ms() > plan->expires) { plan->state = P_DEAD; r = CLEAN_E_EXPIRED; }
    else {
        plan->state = P_EXECUTING;
        begin(JOB_EXEC);
        if (start_thread(exec_thread, plan)) { plan->state = P_DEAD; state = ST_IDLE; r = CLEAN_E_UNAVAILABLE; }
        else r = id;
    }
    pthread_mutex_unlock(&mu);
    return r;
}

/* ---- what JS sees ---- */

char *clean_poll(void) {
    JBuf b;
    jb_init(&b);
    pthread_mutex_lock(&mu);
    jb_printf(&b, "{\"job\":\"%s\",\"state\":\"%s\",\"error\":\"%s\",\"scan\":", job_name[job], st_name[state], error);
    if (scan) {
        const Scan *s = scan;
        double est = 0;
        int run = job == JOB_SCAN && state == ST_RUNNING;
        for (size_t i = 0; i < s->n && !run; i++) est += estimated(&s->c[i]);
        jb_printf(&b, "{\"id\":%d,\"complete\":%s,\"limit\":\"%s\",\"seen\":%ld,\"dirs\":%ld,\"candidates\":%zu,", s->id,
                  s->complete ? "true" : "false", s->limit, s->seen, s->dirs, s->n);
        jb_raw(&b, "\"logical\":"); jb_num(&b, s->logical);
        jb_raw(&b, ",\"allocated\":"); jb_num(&b, s->allocated);
        jb_raw(&b, ",\"estimated\":"); jb_num(&b, est);
        jb_printf(&b, ",\"skipped\":{\"symlinks\":%ld,\"special\":%ld,\"mounts\":%ld,\"unreadable\":%ld,\"protected\":%ld,"
                      "\"names\":%ld,\"nomatch\":%ld,\"refused\":%ld,\"foreign\":%ld},\"rules\":[",
                  s->symlinks, s->special, s->mounts, s->unreadable, s->protect, s->names, s->nomatch, s->refused, s->foreign);
        int nrules;
        const CleanRule *rules = clean_rules(&nrules);
        for (int i = 0; i < s->nrules; i++) {
            jb_printf(&b, "%s{\"id\":\"%s\",\"category\":\"%s\",\"risk\":\"%s\",\"action\":\"%s\",\"text\":\"%s\",\"root\":\"%s\",\"files\":%ld,\"logical\":",
                      i ? "," : "", rules[i].id, rules[i].category, clean_risk_name(rules[i].risk), clean_action_name(rules[i].action),
                      rules[i].text_key, root_name[s->root_state[i]], s->tot[i].files);
            jb_num(&b, s->tot[i].logical);
            jb_raw(&b, ",\"allocated\":");
            jb_num(&b, s->tot[i].allocated);
            jb_raw(&b, "}");
        }
        jb_raw(&b, "]}");
    } else jb_raw(&b, "null");
    jb_raw(&b, ",\"plan\":");
    if (plan) {
        const Plan *p = plan;
        double left = p->state == P_READY ? (p->expires - now_ms()) / 1000 : 0;
        jb_printf(&b, "{\"id\":%d,\"state\":\"%s\",\"items\":%zu,\"processed\":%zu,\"expiresInS\":%.0f,\"counts\":{", p->id,
                  plan_name[left < 0 ? P_DEAD : p->state], p->n, p->done, left < 0 ? 0 : left);
        for (int i = 0; i < I_COUNT; i++) jb_printf(&b, "%s\"%s\":%ld", i ? "," : "", item_name[i], p->count[i]);
        jb_raw(&b, "},\"logical\":"); jb_num(&b, p->logical);
        jb_raw(&b, ",\"allocated\":"); jb_num(&b, p->allocated);
        jb_raw(&b, ",\"estimated\":"); jb_num(&b, p->estimated);
        jb_raw(&b, ",\"freeBefore\":"); jb_num(&b, p->free_before);
        jb_raw(&b, ",\"freeAfter\":"); jb_num(&b, p->free_after);
        jb_printf(&b, ",\"trashItems\":%ld,\"deleteItems\":%ld,\"trashAllocated\":", p->trash_items, p->delete_items);
        jb_num(&b, p->trash_alloc);
        jb_raw(&b, ",\"deleteAllocated\":");
        jb_num(&b, p->delete_alloc);
        jb_raw(&b, ",\"warnings\":[");
        const char *const ws[] = { p->trash_items ? "trashKeepsSpace" : NULL, p->delete_items ? "deleteIsFinal" : NULL,
                                   scan && !scan->complete ? "scanIncomplete" : NULL, p->links_elsewhere ? "linksElsewhere" : NULL };
        for (int i = 0, k = 0; i < 4; i++) if (ws[i]) jb_printf(&b, "%s\"%s\"", k++ ? "," : "", ws[i]);
        jb_raw(&b, "]}");
    } else jb_raw(&b, "null");
    jb_raw(&b, ",\"emptied\":");
    if (empty_items >= 0) {
        jb_printf(&b, "{\"items\":%ld,\"freeBefore\":", empty_items);
        jb_num(&b, empty_before);
        jb_raw(&b, ",\"freeAfter\":");
        jb_num(&b, empty_after);
        jb_raw(&b, "}");
    } else jb_raw(&b, "null");
    jb_raw(&b, "}");
    pthread_mutex_unlock(&mu);
    if (b.oom) { jb_free(&b); return NULL; }
    return b.p;
}

char *clean_status(void) {
    JBuf b;
    jb_init(&b);
    struct statvfs v;
    int have = !statvfs(fsops_home(), &v);
    jb_printf(&b, "{\"available\":%s,\"free\":", clean_available() ? "true" : "false");
    jb_num(&b, have ? (double)v.f_bavail * (double)v.f_frsize : -1);
    jb_raw(&b, ",\"total\":");
    jb_num(&b, have ? (double)v.f_blocks * (double)v.f_frsize : -1);
    jb_raw(&b, "}");
    if (b.oom) { jb_free(&b); return NULL; }
    return b.p;
}

static void cand_json(JBuf *b, const Scan *s, const Cand *c, const CleanRule *rules) {
    char path[CLEAN_PATH_MAX * 2 + 8];
    snprintf(path, sizeof path, "~/%s/%s", rules[c->rule].root, c->rel);
    jb_printf(b, "{\"id\":%u,\"rule\":\"%s\",\"path\":", (uint32_t)(c - s->c) + 1, rules[c->rule].id);
    jb_str(b, path);
    jb_raw(b, ",\"size\":");
    jb_num(b, (double)c->size);
    jb_raw(b, ",\"allocated\":");
    jb_num(b, (double)c->alloc);
    jb_raw(b, ",\"estimated\":");
    jb_num(b, estimated(c));
    jb_printf(b, ",\"links\":%u,\"linked\":%s", c->nlink, c->first ? "true" : "false");
}

char *clean_results(int want_plan, long off, int limit) {
    JBuf b;
    jb_init(&b);
    int nrules;
    const CleanRule *rules = clean_rules(&nrules);
    pthread_mutex_lock(&mu);
    size_t total = !scan ? 0 : want_plan ? (plan ? plan->n : 0) : scan->n;
    jb_printf(&b, "{\"total\":%zu,\"items\":[", total);
    for (size_t i = (size_t)off, k = 0; i < total && k < (size_t)limit; i++, k++) {
        const Cand *c = want_plan ? &scan->c[plan->it[i].cand] : &scan->c[i];
        if (k) jb_raw(&b, ",");
        cand_json(&b, scan, c, rules);
        if (want_plan) jb_printf(&b, ",\"status\":\"%s\"", item_name[plan->it[i].status]);
        jb_raw(&b, "}");
    }
    jb_raw(&b, "]}");
    pthread_mutex_unlock(&mu);
    if (b.oom) { jb_free(&b); return NULL; }
    return b.p;
}
