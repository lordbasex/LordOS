/* blockwatch.c — Sentinela's blocklist: the programs the person blocked, ended whenever they start (blockwatch.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "blockwatch.h"
#include "engine.h"
#include "../sec/json.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char id[65];                /* the SHA-256, hex */
    char kind[8];               /* "binary" | "script" */
    char path[PATH_MAX];
    long long size;
    char interp[16];
    char pkg[128];
    char name[64];
    double at;
} Entry;

typedef struct { int pid; unsigned long long start; } Seen;
typedef struct { dev_t dev; ino_t ino; long long size, mtime; char hex[65]; } HashMemo;
typedef struct { char id[65]; char name[64]; int pid; double at; } Event;

#define EVENTS 32
#define MEMO 256

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int started;
    char path[PATH_MAX];
    Entry e[BLOCKWATCH_MAX];
    int n;
    Seen *seen;
    int nseen, capseen;
    HashMemo memo[MEMO];
    int memo_next;
    Event ev[EVENTS];
    int nev;
    pid_t self, parents[8];
    int nparents;
    int gen;                    /* the list changed: every process is looked at again */
} B = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static double wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ---- the file ---- */

static void load(void) {
    B.n = 0;
    int fd = open(B.path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    char *buf = NULL;
    ssize_t k = -1;
    /* the user's own file, not writable by others, bounded */
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() && !(st.st_mode & 022) && st.st_size < 512 * 1024 && (buf = malloc((size_t)st.st_size + 1)))
        k = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (k <= 0) { free(buf); return; }
    JDoc d;
    if (json_parse(buf, (size_t)k, &d, NULL, 0)) { free(buf); return; }
    const JVal *list = json_get(d.root, "entries");
    for (const JVal *v = list && list->type == J_ARR ? list->kid : NULL; v && B.n < BLOCKWATCH_MAX; v = v->next) {
        const char *id = json_str(v, "id"), *kind = json_str(v, "kind"), *path = json_str(v, "path");
        if (!id || strlen(id) != 64 || strspn(id, "0123456789abcdef") != 64 || !kind || (strcmp(kind, "binary") && strcmp(kind, "script"))) continue;
        Entry *e = &B.e[B.n++];
        memset(e, 0, sizeof *e);
        snprintf(e->id, sizeof e->id, "%s", id);
        snprintf(e->kind, sizeof e->kind, "%s", kind);
        snprintf(e->path, sizeof e->path, "%s", path ? path : "");
        e->size = (long long)json_num(v, "size", -1);
        const char *s;
        if ((s = json_str(v, "interp"))) snprintf(e->interp, sizeof e->interp, "%s", s);
        if ((s = json_str(v, "pkg"))) snprintf(e->pkg, sizeof e->pkg, "%s", s);
        if ((s = json_str(v, "name"))) snprintf(e->name, sizeof e->name, "%s", s);
        e->at = json_num(v, "at", 0);
    }
    json_free(&d);
    free(buf);
}

static void q(FILE *f, const char *s) {
    size_t cap = strlen(s) * 6 + 3;
    char *b = malloc(cap);
    if (!b) { fputs("\"\"", f); return; }
    json_quote(b, cap, s);
    fputs(b, f);
    free(b);
}

static void entry_json(FILE *f, const Entry *e) {
    fprintf(f, "{\"id\":\"%s\",\"kind\":\"%s\",\"path\":", e->id, e->kind);
    q(f, e->path);
    fprintf(f, ",\"size\":%lld,\"interp\":", e->size);
    if (e->interp[0]) q(f, e->interp); else fputs("null", f);
    fputs(",\"pkg\":", f);
    if (e->pkg[0]) q(f, e->pkg); else fputs("null", f);
    fputs(",\"name\":", f);
    q(f, e->name);
    fprintf(f, ",\"at\":%.0f}", e->at);
}

static char *list_json_locked(void) {
    char *out = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&out, &n);
    if (!f) return NULL;
    fputs("{\"version\":1,\"entries\":[", f);
    for (int i = 0; i < B.n; i++) { if (i) fputc(',', f); entry_json(f, &B.e[i]); }
    fputs("]}", f);
    fclose(f);
    return out;
}

/* written next to it and renamed over it (0600, in a 0700 folder) */
static int save_locked(void) {
    char dir[PATH_MAX], tmp[PATH_MAX + 8];
    snprintf(dir, sizeof dir, "%s", B.path);
    char *slash = strrchr(dir, '/');
    if (!slash) return -1;
    *slash = 0;
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", dir);
    char *ps = strrchr(parent, '/');
    if (ps) { *ps = 0; mkdir(parent, 0700); }
    mkdir(dir, 0700);
    snprintf(tmp, sizeof tmp, "%s.new", B.path);
    char *j = list_json_locked();
    if (!j) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    int ok = fd >= 0 && write(fd, j, strlen(j)) == (ssize_t)strlen(j) && write(fd, "\n", 1) == 1 && !fsync(fd);
    if (fd >= 0) close(fd);
    free(j);
    if (!ok || rename(tmp, B.path)) { unlink(tmp); return -1; }
    return 0;
}

/* ---- processes ---- */

static int stat_of(int pid, unsigned long long *start, int *ppid, uid_t *uid, char *comm, size_t cn) {
    char p[64], t[1024];
    snprintf(p, sizeof p, "/proc/%d/stat", pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    ssize_t n = fstat(fd, &st) ? -1 : read(fd, t, sizeof t - 1);
    close(fd);
    if (n <= 0) return -1;
    t[n] = 0;
    char *l = strchr(t, '('), *r = strrchr(t, ')');
    if (!l || !r || r < l || r[1] != ' ' || !r[2]) return -1;
    if (comm) snprintf(comm, cn, "%.*s", (int)(r - l - 1 < (long)cn - 1 ? r - l - 1 : (long)cn - 1), l + 1);
    char *s = r + 3;
    long long v[19];
    for (int i = 0; i < 19; i++) v[i] = strtoll(s, &s, 10);
    if (ppid) *ppid = (int)v[0];
    *start = (unsigned long long)v[18];
    if (uid) *uid = st.st_uid;
    return 0;
}

/* lord-server and its parents up to PID 1: never ended */
static void note_self(void) {
    B.self = getpid();
    B.nparents = 0;
    int p = getppid();
    for (int i = 0; i < 8 && p > 1; i++) {
        unsigned long long s;
        int pp = 0;
        B.parents[B.nparents++] = p;
        if (stat_of(p, &s, &pp, NULL, NULL, 0)) break;
        p = pp;
    }
}
static int is_us(int pid) {
    if (pid <= 1 || pid == B.self) return 1;
    for (int i = 0; i < B.nparents; i++) if (B.parents[i] == pid) return 1;
    return 0;
}

static const char *memo_hash(int fd, const struct stat *st) {
    long long mt = st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
    for (int i = 0; i < MEMO; i++)
        if (B.memo[i].hex[0] && B.memo[i].dev == st->st_dev && B.memo[i].ino == st->st_ino && B.memo[i].size == st->st_size && B.memo[i].mtime == mt)
            return B.memo[i].hex;
    HashMemo *m = &B.memo[B.memo_next++ % MEMO];
    long long size = 0;
    llm_sha256_fd(fd, m->hex, &size);
    m->dev = st->st_dev; m->ino = st->st_ino; m->size = st->st_size; m->mtime = mt;
    return m->hex;
}

/* the entry this process runs, or NULL: its program (or its script) has an entry's size and hash, or an entry's package
 * installed its program */
static const Entry *match(int pid, const Entry *E, int n) {
    char p[64], exe[PATH_MAX];
    snprintf(p, sizeof p, "/proc/%d/exe", pid);
    ssize_t el = readlink(p, exe, sizeof exe - 1);
    if (el <= 0) return NULL;
    exe[el] = 0;
    int any_script = 0, any_pkg = 0;
    for (int i = 0; i < n; i++) { any_script |= !strcmp(E[i].kind, "script"); any_pkg |= E[i].pkg[0] != 0; }
    if (any_script && llm_interpreter(exe)) {
        char sp[PATH_MAX];
        int fd = llm_script_of(pid, sp, sizeof sp);
        if (fd >= 0) {
            struct stat st;
            const Entry *hit = NULL;
            if (!fstat(fd, &st))
                for (int i = 0; i < n && !hit; i++)
                    if (!strcmp(E[i].kind, "script") && E[i].size == (long long)st.st_size && !strcmp(memo_hash(fd, &st), E[i].id)) hit = &E[i];
            close(fd);
            if (hit) return hit;
        }
    }
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    const Entry *hit = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode))
        for (int i = 0; i < n && !hit; i++)
            if (!strcmp(E[i].kind, "binary") && E[i].size == (long long)st.st_size && !strcmp(memo_hash(fd, &st), E[i].id)) hit = &E[i];
    close(fd);
    if (!hit && any_pkg) {
        char own[128];
        const char *del = strstr(exe, " (deleted)");
        if (del) exe[del - exe] = 0;
        if (!llm_pkg_owner("/var/lib/dpkg/info", exe, own, sizeof own))
            for (int i = 0; i < n && !hit; i++) if (E[i].pkg[0] && !strcmp(E[i].pkg, own)) hit = &E[i];
    }
    return hit;
}

static int end_process(int pid, unsigned long long start) {
    int pfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pfd < 0) return -1;
    unsigned long long now = 0;
    int r = -1;
    /* the pidfd holds this very process: checked once more, then signalled through it */
    if (!stat_of(pid, &now, NULL, NULL, NULL, 0) && now == start) r = (int)syscall(SYS_pidfd_send_signal, pfd, SIGKILL, NULL, 0);
    close(pfd);
    return r;
}

static int cmp_seen(const void *a, const void *b) { const Seen *x = a, *y = b; return x->pid < y->pid ? -1 : x->pid > y->pid; }

/* LordOS's own programs, an interpreter itself and the system's core never go on the list */
static int refused(const char *exe, const char *pkg) {
    const char *b = strrchr(exe, '/');
    b = b ? b + 1 : exe;
    if (!strncmp(b, "lord-", 5) || !strncmp(exe, "/usr/lib/lordos/", 16)) return 1;
    static const char *const core[] = { "lordos", "libc-bin", "libc6", "coreutils", "bash", "dash", "util-linux", "systemd", "systemd-sysv", "dbus",
                                        "dbus-daemon", "dbus-broker", "login", "passwd", "sudo", "pipewire-bin", "wireplumber", "xwayland", "procps", NULL };
    if (pkg && pkg[0] && !strncmp(pkg, "lordos-", 7)) return 1;
    for (const char *const *c = core; *c && pkg && pkg[0]; c++) if (!strcmp(pkg, *c)) return 1;
    return 0;
}

/* one pass over the person's processes with a copy of the list (the lock is not held while files are hashed) */
static int scan_with(const Entry *list, int n) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    Seen *now = NULL;
    int nn = 0, cap = 0, ended = 0;
    uid_t me = getuid();
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        int pid = atoi(de->d_name);
        unsigned long long st;
        uid_t uid;
        char comm[64];
        if (is_us(pid) || stat_of(pid, &st, NULL, &uid, comm, sizeof comm) || uid != me) continue;
        if (nn == cap) { cap = cap ? cap * 2 : 1024; Seen *x = realloc(now, sizeof *x * (size_t)cap); if (!x) break; now = x; }
        now[nn++] = (Seen){ pid, st };
        Seen key = { pid, 0 };
        Seen *old = B.seen && B.nseen ? bsearch(&key, B.seen, (size_t)B.nseen, sizeof *B.seen, cmp_seen) : NULL;
        if (old && old->start == st) continue;          /* looked at already */
        const Entry *e = match(pid, list, n);
        /* a program with the same bytes as one of the system's core programs or LordOS's own (a copy of it): never ended
         * (a script entry matches the script, whatever its interpreter is) */
        if (e && !strcmp(e->kind, "binary")) {
            char xp[64], exe[PATH_MAX], own[128] = "";
            snprintf(xp, sizeof xp, "/proc/%d/exe", pid);
            ssize_t el = readlink(xp, exe, sizeof exe - 1);
            exe[el > 0 ? el : 0] = 0;
            char *del = strstr(exe, " (deleted)");
            if (del) *del = 0;
            if (exe[0]) llm_pkg_owner("/var/lib/dpkg/info", exe, own, sizeof own);
            if (!exe[0] || refused(exe, own)) e = NULL;
        }
        if (e && !end_process(pid, st)) {
            ended++;
            pthread_mutex_lock(&B.mu);
            Event *v = &B.ev[B.nev < EVENTS ? B.nev++ : (memmove(B.ev, B.ev + 1, sizeof B.ev[0] * (EVENTS - 1)), EVENTS - 1)];
            snprintf(v->id, sizeof v->id, "%s", e->id);
            snprintf(v->name, sizeof v->name, "%s", comm);          /* what it was called this time */
            v->pid = pid;
            v->at = wall_ms();
            pthread_mutex_unlock(&B.mu);
        }
    }
    closedir(d);
    if (now) qsort(now, (size_t)nn, sizeof *now, cmp_seen);
    free(B.seen);
    B.seen = now;
    B.nseen = nn;
    B.capseen = cap;
    return ended;
}

/* the person's Block: the entry for the process's program (or script), on the worker's thread */
static char *do_block(int pid, unsigned long long start, int with_pkg) {
    char *out = NULL;
    size_t on = 0;
    FILE *mf;
#define FAIL(code) do { if ((mf = open_memstream(&out, &on))) { fprintf(mf, "{\"ok\":false,\"error\":\"%s\"}", code); fclose(mf); } return out; } while (0)
    unsigned long long st;
    uid_t uid;
    char comm[64];
    if (pid <= 1 || stat_of(pid, &st, NULL, &uid, comm, sizeof comm) || st != start) FAIL("proc.gone");
    if (uid != getuid()) FAIL("proc.notOwn");
    if (is_us(pid)) FAIL("sentinela.blockRefused");
    Entry e;
    memset(&e, 0, sizeof e);
    char p[64], exe[PATH_MAX] = "";
    snprintf(p, sizeof p, "/proc/%d/exe", pid);
    ssize_t el = readlink(p, exe, sizeof exe - 1);
    if (el > 0) exe[el] = 0;
    char *del = strstr(exe, " (deleted)");
    if (del) *del = 0;
    char own[128] = "";
    if (exe[0]) llm_pkg_owner("/var/lib/dpkg/info", exe, own, sizeof own);
    int fd = -1, f = llm_interpreter(exe);
    if (f) {
        char sp[PATH_MAX];
        fd = llm_script_of(pid, sp, sizeof sp);
        if (fd < 0) FAIL("sentinela.blockRefused");                     /* the interpreter itself */
        char fdp[64];
        snprintf(fdp, sizeof fdp, "/proc/self/fd/%d", fd);
        ssize_t k = readlink(fdp, e.path, sizeof e.path - 1);
        if (k > 0) e.path[k] = 0; else snprintf(e.path, sizeof e.path, "%s", sp);
        snprintf(e.kind, sizeof e.kind, "script");
        static const char *const fam[] = { "s", "shell", "p", "Python", "l", "Perl", "r", "Ruby", "n", "JavaScript", "u", "Lua", "h", "PHP", "t", "Tcl", NULL };
        for (int i = 0; fam[i]; i += 2) if (fam[i][0] == f) snprintf(e.interp, sizeof e.interp, "%s", fam[i + 1]);
        own[0] = 0;                                    /* a script's package is its interpreter's: never kept */
    } else {
        if (!exe[0] || refused(exe, own)) FAIL("sentinela.blockRefused");
        fd = open(p, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
        snprintf(e.path, sizeof e.path, "%s", exe);
        snprintf(e.kind, sizeof e.kind, "binary");
    }
    struct stat fs;
    if (fd < 0 || fstat(fd, &fs) || !S_ISREG(fs.st_mode)) { if (fd >= 0) close(fd); FAIL("sys.failed"); }
    long long size = 0;
    llm_sha256_fd(fd, e.id, &size);
    close(fd);
    e.size = (long long)fs.st_size;
    if (with_pkg && own[0]) snprintf(e.pkg, sizeof e.pkg, "%s", own);
    snprintf(e.name, sizeof e.name, "%s", comm);
    e.at = wall_ms();
    pthread_mutex_lock(&B.mu);
    int at = -1;
    for (int i = 0; i < B.n; i++) if (!strcmp(B.e[i].id, e.id)) at = i;
    if (at < 0 && B.n >= BLOCKWATCH_MAX) { pthread_mutex_unlock(&B.mu); FAIL("sentinela.blockFull"); }
    if (at < 0) at = B.n++;
    B.e[at] = e;
    B.gen++;
    if (save_locked()) { load(); pthread_mutex_unlock(&B.mu); FAIL("sys.failed"); }
    pthread_mutex_unlock(&B.mu);
    if ((mf = open_memstream(&out, &on))) { fputs("{\"ok\":true,\"entry\":", mf); entry_json(mf, &e); fputs("}", mf); fclose(mf); }
    return out;
#undef FAIL
}

/* ---- the requests (Block and Unblock, from JS) and the thread ---- */

#define REQS 8
#define RESULTS 8
typedef struct { int id, pid, with_pkg, unblock; unsigned long long start; char entry[65]; } Req;
typedef struct { int id; char *json; } Res;
static Req reqs[REQS];
static int nreq, next_id;
static Res results[RESULTS];

static void put_result(int id, char *json) {
    pthread_mutex_lock(&B.mu);
    int slot = 0;
    for (int i = 0; i < RESULTS; i++) if (!results[i].id) { slot = i; break; } else if (results[i].id < results[slot].id) slot = i;
    free(results[slot].json);
    results[slot].id = id;
    results[slot].json = json;
    pthread_mutex_unlock(&B.mu);
}

static void *worker(void *arg) {
    (void)arg;
    Entry *copy = malloc(sizeof(Entry) * BLOCKWATCH_MAX);
    if (!copy) return NULL;
    int seen_gen = -1;
    for (;;) {
        pthread_mutex_lock(&B.mu);
        while (!B.n && !nreq) pthread_cond_wait(&B.cv, &B.mu);
        Req r = { 0 };
        int have = nreq > 0;
        if (have) { r = reqs[0]; memmove(reqs, reqs + 1, sizeof reqs[0] * (size_t)(--nreq)); }
        int n = B.n;
        memcpy(copy, B.e, sizeof(Entry) * (size_t)n);
        if (seen_gen != B.gen) { seen_gen = B.gen; free(B.seen); B.seen = NULL; B.nseen = 0; }
        pthread_mutex_unlock(&B.mu);
        if (have && !r.unblock) put_result(r.id, do_block(r.pid, r.start, r.with_pkg));
        else if (have) {
            pthread_mutex_lock(&B.mu);
            int hit = 0;
            for (int i = 0; i < B.n; i++)
                if (!strcmp(B.e[i].id, r.entry)) { memmove(B.e + i, B.e + i + 1, sizeof B.e[0] * (size_t)(B.n - i - 1)); B.n--; hit = 1; break; }
            B.gen += hit;
            int bad = hit && save_locked();
            if (bad) load();
            pthread_mutex_unlock(&B.mu);
            put_result(r.id, strdup(hit && !bad ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"sentinela.notBlocked\"}"));
        }
        if (have) continue;                             /* the next request first; the list is looked at after */
        scan_with(copy, n);
        pthread_mutex_lock(&B.mu);
        if (!nreq) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += BLOCKWATCH_MS * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&B.cv, &B.mu, &ts);
        }
        pthread_mutex_unlock(&B.mu);
    }
    return NULL;
}

static void wake_locked(void) {
    if (!B.started) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 512 * 1024);
        if (!pthread_create(&th, &at, worker, NULL)) B.started = 1;
        pthread_attr_destroy(&at);
    }
    pthread_cond_signal(&B.cv);
}

void blockwatch_init(const char *path) {
    pthread_mutex_lock(&B.mu);
    if (path) snprintf(B.path, sizeof B.path, "%s", path);
    else {
        const char *home = getenv("HOME");
        snprintf(B.path, sizeof B.path, "%s/.config/lordos/sentinela/blocked.json", home && home[0] == '/' ? home : "/nonexistent");
    }
    note_self();
    load();
    if (B.n) wake_locked();
    pthread_mutex_unlock(&B.mu);
}

static int submit(Req *r) {
    pthread_mutex_lock(&B.mu);
    if (!B.path[0] || nreq >= REQS) { pthread_mutex_unlock(&B.mu); errno = EBUSY; return -1; }
    if (++next_id <= 0) next_id = 1;
    r->id = next_id;
    reqs[nreq++] = *r;
    wake_locked();
    pthread_mutex_unlock(&B.mu);
    return r->id;
}

int blockwatch_block(int pid, unsigned long long start, int with_pkg) {
    if (pid <= 1) { errno = EINVAL; return -1; }
    Req r = { .pid = pid, .start = start, .with_pkg = !!with_pkg };
    return submit(&r);
}

int blockwatch_unblock(const char *id) {
    if (!id || strlen(id) != 64 || strspn(id, "0123456789abcdef") != 64) { errno = EINVAL; return -1; }
    Req r = { .unblock = 1 };
    snprintf(r.entry, sizeof r.entry, "%s", id);
    return submit(&r);
}

char *blockwatch_take(int id) {
    char *j = NULL;
    pthread_mutex_lock(&B.mu);
    for (int i = 0; i < RESULTS; i++) if (id > 0 && results[i].id == id) { j = results[i].json; results[i].json = NULL; results[i].id = 0; break; }
    pthread_mutex_unlock(&B.mu);
    return j;
}

char *blockwatch_list_json(void) {
    pthread_mutex_lock(&B.mu);
    char *j = list_json_locked();
    pthread_mutex_unlock(&B.mu);
    return j;
}

char *blockwatch_events_json(void) {
    char *out = NULL;
    size_t n = 0;
    pthread_mutex_lock(&B.mu);
    FILE *f = open_memstream(&out, &n);
    if (f) {
        fputc('[', f);
        for (int i = 0; i < B.nev; i++) {
            if (i) fputc(',', f);
            fprintf(f, "{\"id\":\"%s\",\"name\":", B.ev[i].id);
            q(f, B.ev[i].name);
            fprintf(f, ",\"pid\":%d,\"at\":%.0f}", B.ev[i].pid, B.ev[i].at);
        }
        fputc(']', f);
        fclose(f);
    }
    B.nev = 0;
    pthread_mutex_unlock(&B.mu);
    return out;
}
