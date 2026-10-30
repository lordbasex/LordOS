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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define IDLE_MS 30000                       /* the helper reports at least every ~1024 entries, or while hashing */
#define SAFE_INT 9007199254740992.0         /* 2^53: exact in a JSON number */
#define HISTORY_MAX 100
#define HISTORY_LINE 512

enum { JOB_NONE, JOB_SCAN, JOB_PLAN, JOB_EXEC, JOB_EMPTY };
enum { ST_IDLE, ST_RUNNING, ST_DONE, ST_FAILED, ST_CANCELLED };
static const char *const job_name[] = { "none", "scan", "plan", "execute", "emptyTrash" };
static const char *const st_name[] = { "idle", "running", "done", "failed", "cancelled" };
static const char *const kind_name[] = { "rules", "tree", "dups" };

/* a plan item's state; after ready, the results an item can end with */
enum { I_READY, I_DONE, I_SKIPPED, I_CHANGED, I_DENIED, I_FAILED, I_CANCELLED, I_UNSUPPORTED, I_RESTORED, I_COUNT };
static const char *const item_name[] = { "ready", "done", "skipped", "changed", "denied", "failed", "cancelled", "unsupported", "restored" };

enum { ROOT_OK, ROOT_ABSENT, ROOT_DENIED, ROOT_CHANGED };
static const char *const root_name[] = { "ok", "absent", "denied", "changed" };

typedef struct {
    uint64_t ino, dev;
    int64_t size, alloc, mtime;
    uint32_t nsec, nlink;
    uint32_t links;                         /* first link of an inode: how many the scan saw */
    uint32_t selected;                      /* first link of an inode: how many the plan holds */
    uint32_t first;                         /* a later link: index + 1 of the first; 0 for the first */
    uint32_t group;                         /* duplicates: group + 1 */
    uint16_t src;
    uint8_t type;
    char *rel;
} Cand;

typedef struct { long files; double logical, allocated; } Tot;

/* a place a scan looked at: a rule's root, a folder or a volume */
typedef struct {
    int rule;                               /* index in the rule table, or -1 */
    int writable;                           /* not a rule: files here can go to the Trash */
    int state;
    dev_t dev;
    char base[PATH_MAX];
    Tot tot;
} Src;

typedef struct { int32_t up; uint32_t files; double logical, allocated; char *name; } Node;
typedef struct { uint32_t first, count; double reclaim; } Group;

typedef struct {
    int id, kind, complete;
    Cand *c;
    size_t n, cap;
    uint32_t *ht;                           /* (dev, ino) → index + 1, open addressing */
    size_t htcap;
    uint32_t *order;                        /* tree: candidates by size, largest first */
    long seen, dirs, hashed, symlinks, special, mounts, unreadable, protect, names, nomatch, refused, foreign, excluded;
    char limit[8];
    double logical, allocated;
    int nsrc;
    Src src[CLEAN_MAX_ROOTS];
    Tot types[CLEAN_TYPES];
    char *exclude[32];
    int nexclude;
    Node *node;
    size_t nnode;
    uint32_t *kid_start, *kid;              /* tree: children of node i are kid[kid_start[i]..kid_start[i + 1]) */
    Group *grp;
    size_t ngrp, grp_cap, grp_from;
} Scan;

typedef struct { uint32_t cand, keeper; uint8_t status; char *trashed; } Item;

enum { P_BUILDING, P_READY, P_EXECUTING, P_FINISHED, P_DEAD };
static const char *const plan_name[] = { "building", "ready", "executing", "finished", "dead" };

typedef struct {
    int id, state;
    Scan *scan;
    Item *it;
    size_t n, done;
    long count[I_COUNT];
    double logical, allocated, estimated;
    long trash_items, delete_items, trashed, deleted;
    double trash_alloc, delete_alloc, trashed_alloc, deleted_alloc;
    double expires, free_before, free_after;
    int links_elsewhere, keep_one;
} Plan;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int job = JOB_NONE, state = ST_IDLE, forget_req;
static volatile int cancel_req;
static char error[16];
static pid_t child = -1;
static Scan *scans[CLEAN_KINDS], *scan;
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

static void scan_clear(Scan *s) {
    for (size_t i = 0; i < s->n; i++) free(s->c[i].rel);
    s->n = 0;
    s->logical = s->allocated = 0;
    for (int i = 0; i < s->nsrc; i++) memset(&s->src[i].tot, 0, sizeof s->src[i].tot);
    memset(s->types, 0, sizeof s->types);
    for (size_t i = 0; i < s->nnode; i++) free(s->node[i].name);
    free(s->node);
    s->node = NULL;
    s->nnode = 0;
    s->ngrp = 0;
    if (s->ht) memset(s->ht, 0, s->htcap * sizeof *s->ht);
}

static void scan_free(Scan *s) {
    if (!s) return;
    scan_clear(s);
    free(s->c);
    free(s->ht);
    free(s->order);
    free(s->kid_start);
    free(s->kid);
    free(s->grp);
    for (int i = 0; i < s->nexclude; i++) free(s->exclude[i]);
    free(s);
}

static void plan_free(Plan *p) {
    if (!p) return;
    for (size_t i = 0; i < p->n; i++) free(p->it[i].trashed);
    free(p->it);
    free(p);
}

/* with mu held */
static void forget_all(void) {
    plan_free(plan);
    plan = NULL;
    for (int i = 0; i < CLEAN_KINDS; i++) { scan_free(scans[i]); scans[i] = NULL; }
    scan = NULL;
    job = JOB_NONE;
    state = ST_IDLE;
    error[0] = 0;
    empty_items = -1;
}

static Scan *scan_by_id(int id) {
    for (int i = 0; i < CLEAN_KINDS; i++) if (scans[i] && scans[i]->id == id) return scans[i];
    return NULL;
}

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

/* ---- a source's facts ---- */

static const CleanRule *src_rule(const Src *src) {
    int n;
    const CleanRule *t = clean_rules(&n);
    return src->rule >= 0 && src->rule < n ? &t[src->rule] : NULL;
}

static int src_action(const Src *src) {
    if (src->rule < 0) return src->writable ? CLEAN_ACT_TRASH : CLEAN_ACT_NONE;
    const CleanRule *r = src_rule(src);
    return r ? (int)r->action : CLEAN_ACT_NONE;
}

static int src_risk(const Src *src) {
    if (src->rule < 0) return CLEAN_RISK_REVIEW;
    const CleanRule *r = src_rule(src);
    return r ? (int)r->risk : CLEAN_RISK_PROTECTED;
}

static int src_path(const Src *src, const char *rel, char *out, size_t n) {
    return (size_t)snprintf(out, n, "%s/%s", strcmp(src->base, "/") ? src->base : "", rel) >= n ? -1 : 0;
}

static int refused_path(const Scan *s, const Src *src, const char *rel, int *excl) {
    char abs[PATH_MAX * 2];
    if (src_path(src, rel, abs, sizeof abs)) return 1;
    for (int i = 0; i < s->nexclude; i++) if (clean_under(abs, s->exclude[i])) { if (excl) *excl = 1; return 1; }
    return fsops_protected(abs);
}

static int covered(const Src *src, const char *name, double mtime) {
    const CleanRule *r = src_rule(src);
    if (src->rule < 0) return 1;
    if (!r || !clean_glob(r->pattern, name)) return 0;
    for (const char *const *e = r->exclude; e && *e; e++) if (clean_glob(*e, name)) return 0;
    return !r->min_age_days || (double)time(NULL) - mtime >= r->min_age_days * 86400.0;
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

/* One record of a batch. 0 kept, 1 dropped and counted, -1 the helper broke the protocol. With mu held. */
static int take_record(Scan *s, const JVal *rec) {
    double r, maj, min, nl, size, alloc, mt, ns, uid;   /* r: the source's index */
    uint64_t ino;
    const char *rel = json_str(rec, "p");
    if (json_count(rec, "r", &r) || json_count(rec, "M", &maj) || json_count(rec, "m", &min) || json_count(rec, "l", &nl) ||
        json_count(rec, "s", &size) || json_count(rec, "a", &alloc) || json_count(rec, "t", &mt) || json_count(rec, "n", &ns) ||
        json_count(rec, "u", &uid) || parse_ino(json_str(rec, "i"), &ino) || !rel) return -1;
    if (r >= s->nsrc || maj > 0xffffffffu || min > 0xffffffffu || nl < 1 || nl > 0xffffffffu || ns >= 1e9 || uid > 0xffffffffu) return -1;
    if (!clean_relpath_ok(rel, CLEAN_MAX_DEPTH)) return -1;
    Src *src = &s->src[(int)r];
    dev_t dev = makedev((unsigned)maj, (unsigned)min);
    if (src->state != ROOT_OK || dev != src->dev) return -1;    /* it never crosses a device */
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    int excl = 0;
    if (refused_path(s, src, rel, &excl)) { if (excl) s->excluded++; else { s->protect++; s->refused++; } return 1; }
    if (!covered(src, base, mt)) { s->refused++; return 1; }
    if ((uid_t)uid != getuid() && s->kind == CLEAN_KIND_RULES) { s->foreign++; return 1; }
    if (s->n >= (size_t)CLEAN_MAX_FILES) return -1;

    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 1024;
        Cand *c = realloc(s->c, cap * sizeof *c);
        if (!c) return -1;
        s->c = c;
        s->cap = cap;
    }
    char *copy = strdup(rel);
    if (!copy) return -1;
    Cand *first = NULL;
    if (s->kind == CLEAN_KIND_RULES) {
        first = link_lookup(s, dev, ino, (uint32_t)s->n);
        if (!s->ht) { free(copy); return -1; }
    }
    Cand *c = &s->c[s->n];
    *c = (Cand){ .ino = ino, .dev = dev, .size = (int64_t)size, .alloc = (int64_t)alloc, .mtime = (int64_t)mt, .nsec = (uint32_t)ns,
                 .nlink = (uint32_t)nl, .links = 1, .src = (uint16_t)r, .type = (uint8_t)clean_type(base), .rel = copy };
    if (first) {                                /* a hard link: one file, counted once */
        c->first = (uint32_t)(first - s->c) + 1;
        first->links++;
    } else {
        s->logical += size;
        s->allocated += alloc;
        src->tot.logical += size;
        src->tot.allocated += alloc;
    }
    src->tot.files++;
    s->n++;
    return 0;
}

/* A candidate's bytes a cleanup could give back: its inode's, once all its links are in the scan */
static double estimated(const Cand *c) {
    return !c->first && c->links >= c->nlink ? (double)c->alloc : 0;
}

static int take_node(Scan *s, const JVal *v) {
    double id, up_d = -1, sz, al, fl;
    const JVal *up = json_get(v, "up");
    const char *name = json_str(v, "name");
    if (json_count(v, "id", &id) || json_count(v, "s", &sz) || json_count(v, "a", &al) || json_count(v, "f", &fl) || !up || up->type != J_NUM || !name) return -1;
    up_d = up->num;
    if (id >= CLEAN_MAX_NODES || up_d != floor(up_d) || (id == 0 ? up_d != -1 : (up_d < 0 || up_d >= id))) return -1;
    if (id != 0 && !clean_name_ok(name)) return -1;
    if (!s->node) {
        s->node = calloc(CLEAN_MAX_NODES, sizeof *s->node);
        if (!s->node) return -1;
        s->nnode = CLEAN_MAX_NODES;
    }
    Node *n = &s->node[(size_t)id];
    if (n->name) return -1;                     /* reported twice */
    n->name = strdup(name);
    if (!n->name) return -1;
    n->up = (int32_t)up_d;
    n->files = fl > 0xffffffffu ? 0xffffffffu : (uint32_t)fl;
    n->logical = sz;
    n->allocated = al;
    return 0;
}

/* the records since the last group are one group: two inodes at least, or they are dropped */
static int close_group(Scan *s) {
    size_t a = s->grp_from, b = s->n;
    int distinct = 0;
    for (size_t i = a + 1; i < b; i++) if (s->c[i].ino != s->c[a].ino || s->c[i].dev != s->c[a].dev) distinct = 1;
    if (!distinct || b - a < 2) {
        for (size_t i = a; i < b; i++) free(s->c[i].rel);
        s->n = a;
        return 0;
    }
    for (size_t i = a + 1; i < b; i++) if (s->c[i].size != s->c[a].size) return -1;
    if (s->ngrp == s->grp_cap) {
        size_t cap = s->grp_cap ? s->grp_cap * 2 : 256;
        Group *g = realloc(s->grp, cap * sizeof *g);
        if (!g) return -1;
        s->grp = g;
        s->grp_cap = cap;
    }
    double reclaim = 0;
    for (size_t i = a; i < b; i++) {
        int again = 0;
        for (size_t k = a; k < i; k++) if (s->c[k].ino == s->c[i].ino && s->c[k].dev == s->c[i].dev) again = 1;
        if (i != a && !again) reclaim += (double)s->c[i].alloc;
        s->c[i].group = (uint32_t)s->ngrp + 1;
    }
    s->grp[s->ngrp++] = (Group){ (uint32_t)a, (uint32_t)(b - a), reclaim };
    s->grp_from = b;
    return 0;
}

static int take_batch(Scan *s, const JVal *root) {
    double seen, dirs, hashed = 0;
    const JVal *items = json_get(root, "items"), *nodes = json_get(root, "nodes");
    if (json_count(root, "seen", &seen) || json_count(root, "dirs", &dirs) || !items || items->type != J_ARR ||
        !nodes || nodes->type != J_ARR || items->len > CLEAN_BATCH || nodes->len > CLEAN_BATCH) return -1;
    if (json_get(root, "hashed") && json_count(root, "hashed", &hashed)) return -1;
    if (s->kind != CLEAN_KIND_TREE && nodes->len) return -1;
    int r = 0;
    pthread_mutex_lock(&mu);
    s->seen = (long)seen;
    s->dirs = (long)dirs;
    s->hashed = (long)hashed;
    for (const JVal *v = items->kid; v && r >= 0; v = v->next) if (take_record(s, v) < 0) r = -1;
    for (const JVal *v = nodes->kid; v && r >= 0; v = v->next) if (take_node(s, v) < 0) r = -1;
    if (r >= 0 && s->kind == CLEAN_KIND_DUPS && json_bool(root, "group", 0)) r = close_group(s);
    pthread_mutex_unlock(&mu);
    return r;
}

static int take_done(Scan *s, const JVal *root) {
    double v[10];
    static const char *const key[] = { "seen", "dirs", "symlinks", "special", "mounts", "unreadable", "protected", "names", "nomatch", "excluded" };
    for (int i = 0; i < 10; i++) if (json_count(root, key[i], &v[i])) return -1;
    const char *limit = json_str(root, "limit");
    if (!limit || (*limit && strcmp(limit, "files") && strcmp(limit, "depth") && strcmp(limit, "nodes") && strcmp(limit, "results"))) return -1;
    const JVal *ty = json_get(root, "types");
    if (!ty || ty->type != J_ARR || ty->len != CLEAN_TYPES) return -1;
    Tot types[CLEAN_TYPES];
    int k = 0;
    for (const JVal *t = ty->kid; t; t = t->next, k++) {
        double f;
        if (json_count(t, "s", &types[k].logical) || json_count(t, "a", &types[k].allocated) || json_count(t, "f", &f)) return -1;
        types[k].files = (long)f;
    }
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
    s->excluded += (long)v[9];
    memcpy(s->types, types, sizeof types);
    snprintf(s->limit, sizeof s->limit, "%s", limit);
    s->complete = !*limit && !s->unreadable && !s->names;
    for (int i = 0; i < s->nsrc; i++) if (s->src[i].state == ROOT_DENIED || s->src[i].state == ROOT_CHANGED) s->complete = 0;
    pthread_mutex_unlock(&mu);
    return 0;
}

#ifdef CLEAN_TEST
int clean_test_record(const char *json, int nrules) {
    static Scan s;
    int r = -1;
    JDoc d = { 0 };
    memset(&s, 0, sizeof s);
    s.nsrc = nrules;
    struct stat st;
    if (stat(fsops_home(), &st)) return -1;
    const CleanRule *t = clean_rules(&r);
    for (int i = 0; i < nrules && i < CLEAN_MAX_ROOTS; i++) {
        s.src[i].rule = i;
        s.src[i].dev = st.st_dev;
        snprintf(s.src[i].base, sizeof s.src[i].base, "%s/%s", fsops_home(), t[i].root);
    }
    if (json_parse(json, strlen(json), &d, NULL, 0)) return -1;
    r = take_record(&s, d.root);
    json_free(&d);
    for (size_t i = 0; i < s.n; i++) free(s.c[i].rel);
    free(s.c);
    free(s.ht);
    return r;
}
#endif

/* ---- after a scan ---- */

static const Scan *sort_scan;
static int by_size_desc(const void *a, const void *b) {
    const Cand *x = &sort_scan->c[*(const uint32_t *)a], *y = &sort_scan->c[*(const uint32_t *)b];
    return x->size > y->size ? -1 : x->size < y->size;
}
static int by_alloc_desc(const void *a, const void *b) {
    const Node *x = &sort_scan->node[*(const uint32_t *)a], *y = &sort_scan->node[*(const uint32_t *)b];
    return x->allocated > y->allocated ? -1 : x->allocated < y->allocated;
}

/* the tree's indexes; on the scan's thread, before anyone else may read them */
static void finish_tree(Scan *s) {
    s->order = malloc((s->n ? s->n : 1) * sizeof *s->order);
    if (s->order) {
        for (size_t i = 0; i < s->n; i++) s->order[i] = (uint32_t)i;
        sort_scan = s;
        qsort(s->order, s->n, sizeof *s->order, by_size_desc);
    }
    if (!s->node) return;
    s->kid_start = calloc(s->nnode + 2, sizeof *s->kid_start);
    s->kid = malloc(s->nnode * sizeof *s->kid);
    if (!s->kid_start || !s->kid) return;
    for (size_t i = 1; i < s->nnode; i++) if (s->node[i].name) s->kid_start[s->node[i].up + 1]++;
    for (size_t i = 0; i < s->nnode; i++) s->kid_start[i + 1] += s->kid_start[i];
    uint32_t *fill = malloc((s->nnode + 1) * sizeof *fill);
    if (!fill) return;
    memcpy(fill, s->kid_start, (s->nnode + 1) * sizeof *fill);
    for (size_t i = 1; i < s->nnode; i++) if (s->node[i].name) s->kid[fill[s->node[i].up]++] = (uint32_t)i;
    free(fill);
    sort_scan = s;
    for (size_t i = 0; i < s->nnode; i++) {
        uint32_t a = s->kid_start[i], b = s->kid_start[i + 1];
        if (b - a > 1) qsort(s->kid + a, b - a, sizeof *s->kid, by_alloc_desc);
    }
}

/* ---- the scan ---- */

static int root_failure(int e) {
    if (e == ENOENT) return ROOT_ABSENT;
    return e == EACCES || e == EPERM ? ROOT_DENIED : ROOT_CHANGED;     /* a link or a mount where a folder was: not offered */
}

/* a source's folder, read-only: from the home folder beneath it, or a volume C found itself */
static int open_src(const Src *src, int home, int flags) {
    size_t n = strlen(fsops_home());
    if (!strncmp(src->base, fsops_home(), n) && src->base[n] == '/') return clean_open(home, src->base + n + 1, flags);
    return open(src->base, flags | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

static void *scan_thread(void *arg) {
    Scan *s = arg;
    int st = ST_FAILED, sock = -1, fds[CLEAN_MAX_ROOTS], nfd = 0, home = -1, finished = 0;
    const char *why = "helper";
    pid_t pid = -1;
    char path[PATH_MAX];
    JBuf go;
    jb_init(&go);
    if (helper_path(path, sizeof path)) goto end;
    home = open(fsops_home(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (home < 0) goto end;
    jb_printf(&go, "{\"mode\":\"%s\",\"maxFiles\":%ld,\"maxDepth\":%d,\"minSize\":%.0f,\"home\":", kind_name[s->kind],
              s->kind == CLEAN_KIND_TREE ? (long)CLEAN_TREE_MAX_FILES : (long)MAX_FILES, CLEAN_MAX_DEPTH, s->seen < 0 ? (double)-s->seen : 0.0);
    s->seen = 0;
    jb_str(&go, fsops_home());
    jb_raw(&go, ",\"exclude\":[");
    for (int i = 0; i < s->nexclude; i++) { if (i) jb_raw(&go, ","); jb_str(&go, s->exclude[i]); }
    jb_raw(&go, "],\"roots\":[");
    for (int i = 0; i < s->nsrc; i++) {
        Src *src = &s->src[i];
        int fd = strcmp(src->base, fsops_home()) ? open_src(src, home, O_RDONLY | O_DIRECTORY) : open(fsops_home(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        struct stat sb;
        if (fd >= 0 && fstat(fd, &sb)) { close(fd); fd = -1; errno = EIO; }
        if (fd < 0) { src->state = root_failure(errno); continue; }
        src->dev = sb.st_dev;
        fds[nfd++] = fd;
        const CleanRule *r = src_rule(src);
        jb_printf(&go, "%s{\"rule\":%d,\"pattern\":", nfd > 1 ? "," : "", i);
        jb_str(&go, r ? r->pattern : "*");
        jb_raw(&go, ",\"base\":");
        jb_str(&go, src->base);
        jb_raw(&go, "}");
    }
    jb_raw(&go, "]}");
    close(home);
    if (go.oom) goto end;
    if (!nfd) {                                     /* nothing to walk */
        pthread_mutex_lock(&mu);
        s->complete = 1;
        for (int i = 0; i < s->nsrc; i++) if (s->src[i].state != ROOT_ABSENT) s->complete = 0;
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
    if (st == ST_DONE && s->kind == CLEAN_KIND_TREE) finish_tree(s);
    if (st != ST_DONE) {                            /* nothing from a failed or cancelled scan is offered */
        pthread_mutex_lock(&mu);
        scan_clear(s);
        s->complete = 0;
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

/* an option's path: relative to the home folder, or empty for the home folder itself */
static int home_path(const char *rel, char *out, size_t n) {
    if (*rel && !clean_relpath_ok(rel, CLEAN_MAX_DEPTH)) return -1;
    return (size_t)snprintf(out, n, *rel ? "%s/%s" : "%s", fsops_home(), rel) >= n ? -1 : 0;
}

typedef struct { const char *want; int n; char path[PATH_MAX]; } VolPick;
static void pick_volume(void *ud, const char *path, const char *fstype, double total, double avail) {
    VolPick *v = ud;
    (void)fstype; (void)total; (void)avail;
    char id[32];
    snprintf(id, sizeof id, "mount:%d", v->n++);
    if (!strcmp(id, v->want)) snprintf(v->path, sizeof v->path, "%s", path);
}

static int fill_sources(Scan *s, const CleanOpts *o) {
    if (s->kind == CLEAN_KIND_RULES) {
        int nrules;
        const CleanRule *t = clean_rules(&nrules);
        s->nsrc = nrules > CLEAN_MAX_ROOTS ? CLEAN_MAX_ROOTS : nrules;
        for (int i = 0; i < s->nsrc; i++) {
            s->src[i].rule = i;
            if ((size_t)snprintf(s->src[i].base, sizeof s->src[i].base, "%s/%s", fsops_home(), t[i].root) >= sizeof s->src[i].base) return -1;
        }
        return 0;
    }
    if (s->kind == CLEAN_KIND_TREE) {
        Src *src = &s->src[0];
        const char *v = o && o->volume ? o->volume : "home";
        s->nsrc = 1;
        src->rule = -1;
        if (!strcmp(v, "home")) snprintf(src->base, sizeof src->base, "%s", fsops_home());
        else if (!strcmp(v, "root")) snprintf(src->base, sizeof src->base, "/");
        else {
            VolPick p = { v, 0, "" };
            fs_volumes(pick_volume, &p);
            if (!p.path[0]) return -1;
            snprintf(src->base, sizeof src->base, "%s", p.path);
        }
        src->writable = fsops_allowed(src->base);
        s->seen = -(long)(o && o->min_size >= 1 ? o->min_size : 1048576);     /* carried to the thread */
        return 0;
    }
    if (!o || o->nfolders < 1 || o->nfolders > CLEAN_MAX_FOLDERS) return -1;
    s->nsrc = o->nfolders;
    for (int i = 0; i < o->nfolders; i++) {
        s->src[i].rule = -1;
        s->src[i].writable = 1;
        if (home_path(o->folders[i], s->src[i].base, sizeof s->src[i].base)) return -1;
        for (int k = 0; k < i; k++)
            if (clean_under(s->src[i].base, s->src[k].base) || clean_under(s->src[k].base, s->src[i].base)) return -1;   /* one file counted twice is no duplicate */
    }
    return 0;
}

int clean_start_kind(int kind, const CleanOpts *o) {
    if (kind < 0 || kind >= CLEAN_KINDS) return CLEAN_E_BADFOLDER;
    if (!clean_available()) return CLEAN_E_UNAVAILABLE;
    Scan *s = calloc(1, sizeof *s);
    if (!s) return CLEAN_E_UNAVAILABLE;
    s->kind = kind;
    if (fill_sources(s, o)) { free(s); return CLEAN_E_BADFOLDER; }
    for (int i = 0; o && i < o->nexclude && i < 32; i++) {
        char p[PATH_MAX];
        if (!*o->exclude[i] || home_path(o->exclude[i], p, sizeof p) || !(s->exclude[s->nexclude] = strdup(p))) { scan_free(s); return CLEAN_E_BADFOLDER; }
        s->nexclude++;
    }
    pthread_mutex_lock(&mu);
    if (begin(JOB_SCAN)) { pthread_mutex_unlock(&mu); scan_free(s); return CLEAN_E_BUSY; }
    Scan *old = scans[kind];
    if (plan && plan->scan == old) { plan_free(plan); plan = NULL; }
    scan_free(old);
    s->id = next_id++;
    scans[kind] = s;
    scan = s;
    int id = s->id;
    if (start_thread(scan_thread, s)) {
        scans[kind] = NULL;
        scan = NULL;
        state = ST_IDLE;
        job = JOB_NONE;
        pthread_mutex_unlock(&mu);
        scan_free(s);
        return CLEAN_E_UNAVAILABLE;
    }
    pthread_mutex_unlock(&mu);
    return id;
}

int clean_start(void) { return clean_start_kind(CLEAN_KIND_RULES, NULL); }

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

/* Each source's root, opened for this job. rootfd[i] < 0: root_err[i] holds the item status. */
static void open_roots(const Scan *s, int *rootfd, int *root_err) {
    int home = open(fsops_home(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    for (int i = 0; i < s->nsrc; i++) {
        const Src *src = &s->src[i];
        char base[PATH_MAX + 8];
        snprintf(base, sizeof base, "%s", src->base);
        const CleanRule *r = src_rule(src);
        if (r) snprintf(base, sizeof base, "%s/%s", fsops_home(), r->root);        /* the table decides, not what the scan stored */
        if (home < 0) rootfd[i] = -1;
        else if (!strcmp(base, fsops_home())) rootfd[i] = dup(home);
        else if (r) rootfd[i] = clean_open(home, r->root, O_PATH | O_DIRECTORY);
        else rootfd[i] = open_src(src, home, O_PATH | O_DIRECTORY);
        if (rootfd[i] < 0) root_err[i] = errno == EACCES || errno == EPERM ? I_DENIED : I_CHANGED;
    }
    if (home >= 0) close(home);
}
static void close_roots(const Scan *s, int *rootfd) {
    for (int i = 0; i < s->nsrc; i++) if (rootfd[i] >= 0) close(rootfd[i]);
}

static int status_of_errno(int e) {
    if (e == EACCES || e == EPERM || e == EROFS) return I_DENIED;
    if (e == ENOENT || e == ENOTDIR || e == ELOOP || e == EXDEV) return I_CHANGED;    /* not the path the scan saw */
    return I_FAILED;
}

static int same_identity(const struct statx *sx, const Cand *c) {
    return S_ISREG(sx->stx_mode) && makedev(sx->stx_dev_major, sx->stx_dev_minor) == c->dev && sx->stx_ino == c->ino &&
           (int64_t)sx->stx_size == c->size && sx->stx_mtime.tv_sec == c->mtime && sx->stx_mtime.tv_nsec == c->nsec;
}

/* Is this still the object the scan saw? I_READY with its folder open in *parent and its name in *name;
 * otherwise the status. Runs for every item at plan time and again right before the action. */
static int item_check(const Scan *s, const Cand *c, const int *rootfd, const int *root_err, int *parent, const char **name) {
    const Src *src = &s->src[c->src];
    int action = src_action(src), risk = src_risk(src);
    *parent = -1;
    if (risk == CLEAN_RISK_PROTECTED || (action != CLEAN_ACT_TRASH && action != CLEAN_ACT_DELETE) ||
        (action == CLEAN_ACT_DELETE && risk != CLEAN_RISK_REGENERABLE)) return I_UNSUPPORTED;
    const CleanRule *r = src_rule(src);
    Src now = *src;
    if (r) snprintf(now.base, sizeof now.base, "%s/%s", fsops_home(), r->root);
    if (refused_path(s, &now, c->rel, NULL)) return I_DENIED;
    if (rootfd[c->src] < 0) return root_err[c->src];
    const char *base = strrchr(c->rel, '/');
    int fd;
    if (base) {
        char dir[CLEAN_PATH_MAX + 1];
        snprintf(dir, sizeof dir, "%.*s", (int)(base - c->rel), c->rel);
        fd = clean_open(rootfd[c->src], dir, O_PATH | O_DIRECTORY);
        base++;
    } else {
        fd = fcntl(rootfd[c->src], F_DUPFD_CLOEXEC, 0);
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
    if (!same_identity(&sx, c)) st = I_CHANGED;
    else if (sx.stx_uid != getuid()) st = I_DENIED;
    else if (!covered(src, base, (double)c->mtime)) st = I_SKIPPED;
    if (st != I_READY) { close(fd); return st; }
    *parent = fd;
    *name = base;
    return I_READY;
}

/* ---- the plan ---- */

/* Duplicates: every item needs a file that stays, of another inode. None: the group is left alone. */
static void pick_keepers(Plan *p, Scan *s) {
    for (size_t i = 0; i < p->n; i++) if (p->it[i].status == I_READY) s->c[p->it[i].cand].selected = (uint32_t)i + 1;
    for (size_t i = 0; i < p->n; i++) {
        Item *it = &p->it[i];
        const Cand *c = &s->c[it->cand];
        if (it->status != I_READY || !c->group) continue;
        const Group *g = &s->grp[c->group - 1];
        for (uint32_t k = g->first; k < g->first + g->count; k++) {
            const Cand *o = &s->c[k];
            if (!o->selected && (o->ino != c->ino || o->dev != c->dev)) { it->keeper = k + 1; break; }
        }
    }
    int refused = 0;
    for (size_t i = 0; i < p->n; i++) {
        Item *it = &p->it[i];
        if (it->status == I_READY && s->c[it->cand].group && !it->keeper) { it->status = I_SKIPPED; refused = 1; }
    }
    for (size_t i = 0; i < p->n; i++) s->c[p->it[i].cand].selected = 0;
    p->keep_one = refused;
}

static void *plan_thread(void *arg) {
    Plan *p = arg;
    Scan *s = p->scan;                              /* stays: a job is running, so nothing frees it */
    int rootfd[CLEAN_MAX_ROOTS], root_err[CLEAN_MAX_ROOTS];
    memset(root_err, 0, sizeof root_err);
    open_roots(s, rootfd, root_err);
    int cancelled = 0;
    for (size_t i = 0; i < p->n; i++) {
        if (cancel_req) { cancelled = 1; break; }
        const Cand *c = &s->c[p->it[i].cand];
        int fd = -1;
        const char *name;
        int st = item_check(s, c, rootfd, root_err, &fd, &name);
        if (fd >= 0) close(fd);
        pthread_mutex_lock(&mu);
        p->it[i].status = (uint8_t)st;
        pthread_mutex_unlock(&mu);
    }
    close_roots(s, rootfd);
    pthread_mutex_lock(&mu);
    if (!cancelled && s->kind == CLEAN_KIND_DUPS) pick_keepers(p, s);
    pthread_mutex_unlock(&mu);
    long count[I_COUNT] = { 0 }, trash_items = 0, delete_items = 0;
    double logical = 0, allocated = 0, est = 0, trash_alloc = 0, delete_alloc = 0;
    int elsewhere = 0;
    for (size_t i = 0; i < p->n; i++) {
        const Cand *c = &s->c[p->it[i].cand];
        count[p->it[i].status]++;
        if (p->it[i].status != I_READY) continue;
        s->c[c->first ? c->first - 1 : p->it[i].cand].selected++;
        int del = src_action(&s->src[c->src]) == CLEAN_ACT_DELETE;
        if (del) delete_items++; else trash_items++;
        if (!c->first) {
            logical += (double)c->size;
            allocated += (double)c->alloc;
            if (del) delete_alloc += (double)c->alloc; else trash_alloc += (double)c->alloc;
        }
    }
    /* what the plan could give back: an inode with every link selected */
    for (size_t i = 0; i < p->n && !cancelled; i++) {
        if (p->it[i].status != I_READY) continue;
        const Cand *c = &s->c[p->it[i].cand];
        if (c->first) continue;
        if (c->selected >= c->nlink) est += (double)c->alloc;
        else elsewhere = 1;
    }
    for (size_t i = 0; i < s->n; i++) s->c[i].selected = 0;
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
static int plan_begin(Scan *s, Item *it, size_t k) {
    Plan *p = calloc(1, sizeof *p);
    if (!p) { free(it); return CLEAN_E_UNAVAILABLE; }
    p->id = next_id++;
    p->scan = s;
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
    Scan *s = scan_by_id(sid);
    if (state == ST_RUNNING) r = CLEAN_E_BUSY;
    else if (!s) r = CLEAN_E_NOSCAN;
    else if (!n || n > CLEAN_MAX_FILES) r = CLEAN_E_BADID;
    else {
        unsigned char *seen = calloc(s->n ? s->n : 1, 1);
        Item *it = malloc(n * sizeof *it);
        size_t k = 0;
        int ok = seen && it;
        for (size_t i = 0; i < n && ok; i++) {
            if (ids[i] < 1 || ids[i] > s->n) { ok = 0; break; }
            if (seen[ids[i] - 1]) continue;         /* the same id twice is one item */
            seen[ids[i] - 1] = 1;
            it[k++] = (Item){ .cand = ids[i] - 1, .status = I_READY };
        }
        free(seen);
        if (ok) r = plan_begin(s, it, k);
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
    Scan *s = scan_by_id(sid);
    if (state == ST_RUNNING) r = CLEAN_E_BUSY;
    else if (!s || s->kind != CLEAN_KIND_RULES) r = CLEAN_E_NOSCAN;
    else {
        size_t k = 0;
        Item *it = malloc((s->n ? s->n : 1) * sizeof *it);
        if (!it) r = CLEAN_E_UNAVAILABLE;
        else {
            for (size_t i = 0; i < s->n; i++) if (wanted >> s->c[i].src & 1) it[k++] = (Item){ .cand = (uint32_t)i, .status = I_READY };
            if (k) r = plan_begin(s, it, k);
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
    return same_identity(&sx, c);
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

static int trash_file(int fd, const char *name, const char *real, const Cand *c, char **kept) {
    char dest[PATH_MAX * 2], err[256];
    if (fstrash_put_at(fd, name, real, dest, sizeof dest, err, sizeof err))
        return errno == EXDEV ? I_UNSUPPORTED : status_of_errno(errno);
    /* the name could have been pointed at another file between the check and the move: then that file
     * goes back where it was */
    struct stat sb;
    if (lstat(dest, &sb) || sb.st_ino != c->ino || sb.st_dev != c->dev)
        return fstrash_restore(dest, err, sizeof err) ? I_FAILED : I_CHANGED;
    *kept = strdup(dest);
    return I_DONE;
}

/* two open files with the same bytes, read to the end; a cancel counts as different */
static int same_bytes(int a, int b) {
    enum { SZ = 1 << 18 };
    char *x = malloc(SZ), *y = malloc(SZ);
    int same = x && y;
    off_t off = 0;
    while (same) {
        ssize_t n = pread(a, x, SZ, off), m = pread(b, y, SZ, off);
        if (n < 0 || n != m || cancel_req) { same = 0; break; }
        if (!n) break;
        if (memcmp(x, y, (size_t)n)) same = 0;
        off += n;
    }
    free(x);
    free(y);
    return same;
}

/* Duplicates: right before the action, the copy that stays is the file the scan saw and byte for byte the
 * same as the one that goes. */
static int keeper_check(const Scan *s, const Item *it, int fd, const char *name, const int *rootfd, const int *root_err) {
    const Cand *k = &s->c[it->keeper - 1];
    int kfd = -1;
    const char *kname;
    int st = item_check(s, k, rootfd, root_err, &kfd, &kname);
    if (st != I_READY) return I_CHANGED;
    int a = openat(fd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC), b = openat(kfd, kname, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int same = a >= 0 && b >= 0 && same_bytes(a, b);
    if (a >= 0) close(a);
    if (b >= 0) close(b);
    close(kfd);
    return same ? I_READY : I_CHANGED;
}

/* What ran, in ~/.local/state/lordos/cleaner-history.json: counts, bytes and rule ids. Never a path. */
static void history_path(char *out, size_t n) { snprintf(out, n, "%s/.local/state/lordos/cleaner-history.json", fsops_home()); }

static char *history_read(size_t *len) {
    char p[PATH_MAX + 64];
    history_path(p, sizeof p);
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > (HISTORY_MAX + 1) * HISTORY_LINE) { close(fd); return NULL; }
    char *t = malloc((size_t)st.st_size + 1);
    ssize_t n = t ? read(fd, t, (size_t)st.st_size) : -1;
    close(fd);
    if (n < 0) { free(t); return NULL; }
    t[n] = 0;
    *len = (size_t)n;
    return t;
}

static void history_add(const char *line) {
    char p[PATH_MAX + 64], tmp[PATH_MAX + 96];
    history_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.%d", p, (int)getpid());
    char dir[PATH_MAX + 64];
    snprintf(dir, sizeof dir, "%s/.local/state", fsops_home());
    mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/.local/state/lordos", fsops_home());
    mkdir(dir, 0700);
    size_t len = 0;
    char *old = history_read(&len);
    const char *start = old ? old : "";
    int lines = 0;
    for (size_t i = 0; i < len; i++) if (old[i] == '\n') lines++;
    while (lines >= HISTORY_MAX) { const char *nl = strchr(start, '\n'); if (!nl) break; start = nl + 1; lines--; }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        int ok = dprintf(fd, "%s%s\n", start, line) > 0;
        close(fd);
        if (!ok || rename(tmp, p)) unlink(tmp);
    }
    free(old);
}

char *clean_history(void) {
    size_t len = 0;
    char *t = history_read(&len);
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "[");
    int k = 0;
    for (char *line = t; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        JDoc d = { 0 };
        if (!json_parse(line, strlen(line), &d, NULL, 0) && d.root->type == J_OBJ) {   /* a damaged line is skipped */
            if (k++) jb_raw(&b, ",");
            jb_raw(&b, line);
        }
        json_free(&d);
        line = nl ? nl + 1 : NULL;
    }
    free(t);
    jb_raw(&b, "]");
    if (b.oom) { jb_free(&b); return NULL; }
    return b.p;
}

void clean_history_clear(void) {
    char p[PATH_MAX + 64];
    history_path(p, sizeof p);
    unlink(p);
}

static void history_plan(const Plan *p, int cancelled) {
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"at\":%lld,\"action\":\"clean\",\"cancelled\":%s,\"kind\":\"%s\",\"items\":%zu,\"counts\":{", (long long)time(NULL),
              cancelled ? "true" : "false", kind_name[p->scan->kind], p->n);
    for (int i = I_DONE, k = 0; i < I_RESTORED; i++) jb_printf(&b, "%s\"%s\":%ld", k++ ? "," : "", item_name[i], p->count[i]);
    jb_raw(&b, "},\"allocated\":");
    jb_num(&b, p->allocated);
    jb_raw(&b, ",\"freeBefore\":");
    jb_num(&b, p->free_before);
    jb_raw(&b, ",\"freeAfter\":");
    jb_num(&b, p->free_after);
    jb_raw(&b, "}");
    if (!b.oom) history_add(b.p);
    jb_free(&b);
}

static void *exec_thread(void *arg) {
    Plan *p = arg;
    const Scan *s = p->scan;
    int rootfd[CLEAN_MAX_ROOTS], root_err[CLEAN_MAX_ROOTS], cancelled = 0;
    memset(root_err, 0, sizeof root_err);
    open_roots(s, rootfd, root_err);
    double before = free_bytes();
    pthread_mutex_lock(&mu);
    p->free_before = before;
    pthread_mutex_unlock(&mu);
    for (size_t i = 0; i < p->n; i++) {
        int st = p->it[i].status;
        char *kept = NULL;
        if (st == I_READY) {
            const Cand *c = &s->c[p->it[i].cand];
            const Src *src = &s->src[c->src];
            int fd = -1;
            const char *name = NULL;
            if (cancel_req) { cancelled = 1; st = I_CANCELLED; }
            else st = item_check(s, c, rootfd, root_err, &fd, &name);
            if (st == I_READY && p->it[i].keeper) st = keeper_check(s, &p->it[i], fd, name, rootfd, root_err);
            if (st == I_READY) {
                char real[PATH_MAX * 2];
                const CleanRule *r = src_rule(src);
                if (r) snprintf(real, sizeof real, "%s/%s/%s", fsops_home(), r->root, c->rel);
                else src_path(src, c->rel, real, sizeof real);
#ifdef CLEAN_TEST
                if (clean_test_before_trash) clean_test_before_trash(real);
#endif
                st = src_action(src) == CLEAN_ACT_DELETE ? delete_file(fd, name, c) : trash_file(fd, name, real, c, &kept);
            }
            if (fd >= 0) close(fd);
        }
        pthread_mutex_lock(&mu);
        if (st == I_DONE) {
            const Cand *c = &s->c[p->it[i].cand];
            double a = c->first ? 0 : (double)c->alloc;
            if (kept) { p->trashed++; p->trashed_alloc += a; }
            else { p->deleted++; p->deleted_alloc += a; }
        }
        p->count[p->it[i].status]--;
        p->count[st]++;
        p->it[i].status = (uint8_t)st;
        p->it[i].trashed = kept;
        p->done = i + 1;
        pthread_mutex_unlock(&mu);
    }
    close_roots(s, rootfd);
    double after = free_bytes();
    pthread_mutex_lock(&mu);
    p->free_after = after;
    p->state = P_FINISHED;
    pthread_mutex_unlock(&mu);
    history_plan(p, cancelled);
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
    char line[HISTORY_LINE];
    snprintf(line, sizeof line, "{\"at\":%lld,\"action\":\"emptyTrash\",\"items\":%ld,\"freeBefore\":%.0f,\"freeAfter\":%.0f}",
             (long long)time(NULL), n, before, after);
    history_add(line);
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

int clean_restore(int id, uint32_t cand) {
    pthread_mutex_lock(&mu);
    int r = CLEAN_E_NOPLAN;
    if (state == ST_RUNNING) r = CLEAN_E_BUSY;
    else if (plan && plan->id == id && plan->state == P_FINISHED) {
        r = CLEAN_E_BADID;
        for (size_t i = 0; i < plan->n; i++) {
            Item *it = &plan->it[i];
            if (it->cand + 1 != cand || it->status != I_DONE || !it->trashed) continue;
            char err[256];
            if (fstrash_restore(it->trashed, err, sizeof err)) { r = CLEAN_E_BADFOLDER; break; }
            free(it->trashed);
            it->trashed = NULL;
            it->status = I_RESTORED;
            plan->count[I_DONE]--;
            plan->count[I_RESTORED]++;
            plan->trashed--;
            r = 0;
            break;
        }
    }
    pthread_mutex_unlock(&mu);
    return r;
}

/* ---- what JS sees ---- */

static void tot_json(JBuf *b, const Tot *t) {
    jb_printf(b, "\"files\":%ld,\"logical\":", t->files);
    jb_num(b, t->logical);
    jb_raw(b, ",\"allocated\":");
    jb_num(b, t->allocated);
}

static void scan_json(JBuf *b, const Scan *s, int running) {
    double est = 0;
    for (size_t i = 0; i < s->n && !running; i++) est += estimated(&s->c[i]);
    jb_printf(b, "{\"id\":%d,\"kind\":\"%s\",\"running\":%s,\"complete\":%s,\"limit\":\"%s\",\"seen\":%ld,\"dirs\":%ld,\"hashed\":%ld,\"candidates\":%zu,\"groups\":%zu,",
              s->id, kind_name[s->kind], running ? "true" : "false", s->complete ? "true" : "false", s->limit, s->seen < 0 ? 0 : s->seen, s->dirs, s->hashed, s->n, s->ngrp);
    jb_raw(b, "\"logical\":"); jb_num(b, s->logical);
    jb_raw(b, ",\"allocated\":"); jb_num(b, s->allocated);
    jb_raw(b, ",\"estimated\":"); jb_num(b, est);
    double reclaim = 0;
    for (size_t i = 0; i < s->ngrp; i++) reclaim += s->grp[i].reclaim;
    jb_raw(b, ",\"reclaim\":"); jb_num(b, reclaim);
    jb_printf(b, ",\"skipped\":{\"symlinks\":%ld,\"special\":%ld,\"mounts\":%ld,\"unreadable\":%ld,\"protected\":%ld,"
                 "\"names\":%ld,\"nomatch\":%ld,\"refused\":%ld,\"foreign\":%ld,\"excluded\":%ld},\"rules\":[",
              s->symlinks, s->special, s->mounts, s->unreadable, s->protect, s->names, s->nomatch, s->refused, s->foreign, s->excluded);
    int k = 0;
    for (int i = 0; i < s->nsrc; i++) {
        const CleanRule *r = src_rule(&s->src[i]);
        if (!r) continue;
        jb_printf(b, "%s{\"id\":\"%s\",\"category\":\"%s\",\"risk\":\"%s\",\"action\":\"%s\",\"text\":\"%s\",\"root\":\"%s\",", k++ ? "," : "", r->id, r->category,
                  clean_risk_name(r->risk), clean_action_name(r->action), r->text_key, root_name[s->src[i].state]);
        tot_json(b, &s->src[i].tot);
        jb_raw(b, "}");
    }
    jb_raw(b, "],\"types\":[");
    for (int i = 0; i < CLEAN_TYPES; i++) {
        jb_printf(b, "%s{\"type\":\"%s\",", i ? "," : "", clean_type_name(i));
        tot_json(b, &s->types[i]);
        jb_raw(b, "}");
    }
    jb_raw(b, "]}");
}

char *clean_poll(int kind) {
    JBuf b;
    jb_init(&b);
    pthread_mutex_lock(&mu);
    jb_printf(&b, "{\"job\":\"%s\",\"state\":\"%s\",\"error\":\"%s\",\"scan\":", job_name[job], st_name[state], error);
    const Scan *s = kind >= 0 && kind < CLEAN_KINDS ? scans[kind] : scan;
    if (s) scan_json(&b, s, job == JOB_SCAN && state == ST_RUNNING && s == scan);
    else jb_raw(&b, "null");
    jb_raw(&b, ",\"plan\":");
    if (plan) {
        const Plan *p = plan;
        double left = p->state == P_READY ? (p->expires - now_ms()) / 1000 : 0;
        jb_printf(&b, "{\"id\":%d,\"scan\":%d,\"state\":\"%s\",\"items\":%zu,\"processed\":%zu,\"expiresInS\":%.0f,\"counts\":{", p->id, p->scan->id,
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
        jb_printf(&b, ",\"deleted\":%ld,\"trashed\":%ld,\"deletedAllocated\":", p->deleted, p->trashed);
        jb_num(&b, p->deleted_alloc);
        jb_raw(&b, ",\"trashedAllocated\":");
        jb_num(&b, p->trashed_alloc);
        jb_raw(&b, ",\"warnings\":[");
        const char *const ws[] = { p->trash_items ? "trashKeepsSpace" : NULL, p->delete_items ? "deleteIsFinal" : NULL,
                                   !p->scan->complete ? "scanIncomplete" : NULL, p->links_elsewhere ? "linksElsewhere" : NULL,
                                   p->keep_one ? "keepOneCopy" : NULL };
        for (int i = 0, k = 0; i < 5; i++) if (ws[i]) jb_printf(&b, "%s\"%s\"", k++ ? "," : "", ws[i]);
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

static void volume_json(JBuf *b, int *k, const char *id, const char *path, double total, double avail) {
    jb_printf(b, "%s{\"id\":\"%s\",\"path\":", (*k)++ ? "," : "", id);
    jb_str(b, path);
    jb_raw(b, ",\"total\":");
    jb_num(b, total);
    jb_raw(b, ",\"free\":");
    jb_num(b, avail);
    jb_raw(b, "}");
}

typedef struct { JBuf *b; int k, n; } VolList;
static void list_volume(void *ud, const char *path, const char *fstype, double total, double avail) {
    VolList *v = ud;
    (void)fstype;
    char id[32];
    snprintf(id, sizeof id, "mount:%d", v->n++);
    volume_json(v->b, &v->k, id, path, total, avail);
}

char *clean_volumes(void) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "[");
    VolList v = { &b, 0, 0 };
    struct statvfs s;
    if (!statvfs("/", &s)) volume_json(&b, &v.k, "root", "/", (double)s.f_blocks * s.f_frsize, (double)s.f_bavail * s.f_frsize);
    if (!statvfs(fsops_home(), &s)) volume_json(&b, &v.k, "home", fsops_home(), (double)s.f_blocks * s.f_frsize, (double)s.f_bavail * s.f_frsize);
    fs_volumes(list_volume, &v);
    jb_raw(&b, "]");
    if (b.oom) { jb_free(&b); return NULL; }
    return b.p;
}

/* with mu held */
static void cand_json(JBuf *b, const Scan *s, const Cand *c) {
    char abs[PATH_MAX * 2], shown[PATH_MAX * 2 + 2];
    const Src *src = &s->src[c->src];
    const CleanRule *r = src_rule(src);
    src_path(src, c->rel, abs, sizeof abs);
    size_t n = strlen(fsops_home());
    if (!strncmp(abs, fsops_home(), n) && abs[n] == '/') snprintf(shown, sizeof shown, "~%s", abs + n);
    else snprintf(shown, sizeof shown, "%s", abs);
    jb_printf(b, "{\"id\":%u,\"rule\":\"%s\",\"action\":\"%s\",\"type\":\"%s\",\"path\":", (uint32_t)(c - s->c) + 1, r ? r->id : "",
              clean_action_name((CleanAction)src_action(src)), clean_type_name(c->type));
    jb_str(b, shown);
    jb_raw(b, ",\"abs\":");
    jb_str(b, abs);
    jb_raw(b, ",\"size\":");
    jb_num(b, (double)c->size);
    jb_raw(b, ",\"allocated\":");
    jb_num(b, (double)c->alloc);
    jb_raw(b, ",\"estimated\":");
    jb_num(b, estimated(c));
    jb_raw(b, ",\"mtime\":");
    jb_num(b, (double)c->mtime);
    jb_printf(b, ",\"links\":%u,\"linked\":%s", c->nlink, c->first ? "true" : "false");
}

static int passes(const Scan *s, const Cand *c, const CleanFilter *f) {
    if (!f) return 1;
    if (f->rule) {
        const CleanRule *r = src_rule(&s->src[c->src]);
        if (!r || strcmp(r->id, f->rule)) return 0;
    }
    if ((double)c->size < f->min_size || (f->older_than > 0 && (double)c->mtime > f->older_than)) return 0;
    return f->type < 0 || c->type == f->type;
}

char *clean_results_of(int sid, long off, int limit, const CleanFilter *f) {
    JBuf b;
    jb_init(&b);
    pthread_mutex_lock(&mu);
    const Scan *s = scan_by_id(sid);
    size_t total = 0, shown = 0;
    jb_raw(&b, "{\"items\":[");
    if (s) for (size_t i = 0; i < s->n; i++) {
        const Cand *c = &s->c[s->order ? s->order[i] : i];
        if (!passes(s, c, f)) continue;
        if (total >= (size_t)off && shown < (size_t)limit) {
            if (shown++) jb_raw(&b, ",");
            cand_json(&b, s, c);
            jb_raw(&b, "}");
        }
        total++;
    }
    jb_printf(&b, "],\"total\":%zu}", total);
    pthread_mutex_unlock(&mu);
    if (b.oom) { jb_free(&b); return NULL; }
    return b.p;
}

char *clean_results(int want_plan, long off, int limit) {
    if (!want_plan) {
        pthread_mutex_lock(&mu);
        int id = scan ? scan->id : 0;
        pthread_mutex_unlock(&mu);
        return clean_results_of(id, off, limit, NULL);
    }
    JBuf b;
    jb_init(&b);
    pthread_mutex_lock(&mu);
    size_t total = plan ? plan->n : 0;
    jb_printf(&b, "{\"total\":%zu,\"items\":[", total);
    for (size_t i = (size_t)off, k = 0; i < total && k < (size_t)limit; i++, k++) {
        const Scan *s = plan->scan;
        if (k) jb_raw(&b, ",");
        cand_json(&b, s, &s->c[plan->it[i].cand]);
        jb_printf(&b, ",\"status\":\"%s\",\"restorable\":%s}", item_name[plan->it[i].status], plan->it[i].trashed ? "true" : "false");
    }
    jb_raw(&b, "]}");
    pthread_mutex_unlock(&mu);
    if (b.oom) { jb_free(&b); return NULL; }
    return b.p;
}

static void node_json(JBuf *b, const Node *n, long id) {
    jb_printf(b, "{\"id\":%ld,\"up\":%d,\"name\":", id, n->up);
    jb_str(b, n->name);
    jb_printf(b, ",\"files\":%u,\"logical\":", n->files);
    jb_num(b, n->logical);
    jb_raw(b, ",\"allocated\":");
    jb_num(b, n->allocated);
    jb_raw(b, "}");
}

char *clean_tree(int sid, long id, int limit) {
    JBuf b;
    jb_init(&b);
    pthread_mutex_lock(&mu);
    const Scan *s = scan_by_id(sid);
    int ok = s && s->node && s->kid_start && id >= 0 && (size_t)id < s->nnode && s->node[id].name;
    if (!ok) { pthread_mutex_unlock(&mu); jb_free(&b); return NULL; }
    const Node *n = &s->node[id];
    jb_raw(&b, "{\"node\":");
    node_json(&b, n, id);
    jb_raw(&b, ",\"base\":");
    jb_str(&b, s->src[0].base);
    jb_raw(&b, ",\"trail\":[");
    long trail[CLEAN_MAX_DEPTH + 2];
    int depth = 0;
    for (long k = id; k > 0 && depth < CLEAN_MAX_DEPTH + 2; k = s->node[k].up) trail[depth++] = k;
    for (int i = depth - 1; i >= 0; i--) {
        node_json(&b, &s->node[trail[i]], trail[i]);
        if (i) jb_raw(&b, ",");
    }
    jb_raw(&b, "],\"children\":[");
    uint32_t a = s->kid_start[id], z = s->kid_start[id + 1];
    double used = 0, used_l = 0;
    long more = 0;
    for (uint32_t i = a; i < z; i++) {
        const Node *c = &s->node[s->kid[i]];
        used += c->allocated;
        used_l += c->logical;
        if ((int)(i - a) < limit) {
            if (i > a) jb_raw(&b, ",");
            node_json(&b, c, s->kid[i]);
            if (s->kid_start[s->kid[i]] < s->kid_start[s->kid[i] + 1]) { b.n--; b.p[b.n] = 0; jb_raw(&b, ",\"hasKids\":true}"); }
        } else more++;
    }
    jb_printf(&b, "],\"more\":%ld,\"ownAllocated\":", more);
    jb_num(&b, n->allocated - used);
    jb_raw(&b, ",\"ownLogical\":");
    jb_num(&b, n->logical - used_l);
    jb_raw(&b, "}");
    pthread_mutex_unlock(&mu);
    if (b.oom) { jb_free(&b); return NULL; }
    return b.p;
}

char *clean_groups(int sid, long off, int limit) {
    JBuf b;
    jb_init(&b);
    pthread_mutex_lock(&mu);
    const Scan *s = scan_by_id(sid);
    size_t total = s ? s->ngrp : 0;
    jb_printf(&b, "{\"total\":%zu,\"groups\":[", total);
    for (size_t i = (size_t)off, k = 0; i < total && k < (size_t)limit; i++, k++) {
        const Group *g = &s->grp[i];
        jb_printf(&b, "%s{\"id\":%zu,\"size\":", k ? "," : "", i + 1);
        jb_num(&b, (double)s->c[g->first].size);
        jb_raw(&b, ",\"reclaim\":");
        jb_num(&b, g->reclaim);
        jb_raw(&b, ",\"members\":[");
        for (uint32_t m = 0; m < g->count; m++) {
            if (m) jb_raw(&b, ",");
            cand_json(&b, s, &s->c[g->first + m]);
            jb_raw(&b, "}");
        }
        jb_raw(&b, "]}");
    }
    jb_raw(&b, "]}");
    pthread_mutex_unlock(&mu);
    if (b.oom) { jb_free(&b); return NULL; }
    return b.p;
}
