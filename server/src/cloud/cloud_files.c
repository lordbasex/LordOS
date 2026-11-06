/* cloud_files.c — files on demand (docs/CLOUD.md 7): the status of each file, Free Up Space and the automatic pass,
 * and Keep on This Computer.
 *
 * rclone never evicts (the mounts have no age or size limit); LordOS does, by removing a file's cached data and its
 * vfsMeta entry while the mount runs (measured in Phase 0: the next read fetches it again). A file goes only if it
 * is clean: not open (no process of this user has it open under ~/Cloud, and rclone isn't holding it — vfs/stats
 * inUse plus the 5 s rclone keeps a closed handle), not changed here and not waiting in vfs/queue, and not kept. */
#include "cloud_int.h"
#include "../doc/wire.h"
#include "../sec/json.h"
#include "../errcode.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#define HANDLE_HOLD_S 10            /* rclone keeps a closed file's handle 5 s (HandleCaching); twice that */
#define SKIP_MAX 200
#define CAND_MAX 50000
#define WALK_DEPTH 32

/* what one account looks like, copied out from under cl_mu */
typedef struct {
    char id[16], name[CLOUD_NAME_MAX + 1];
    int in_use, mounted, paused;
    char **keep;
    int nkeep;
    char **q;
    int nq;
} Snap;

static int snap(const char *name, const char *id, Snap *s) {
    memset(s, 0, sizeof *s);
    pthread_mutex_lock(&cl_mu);
    Acc *a = name ? cl_find_name(name) : cl_find(id);
    if (!a) { pthread_mutex_unlock(&cl_mu); return -1; }
    cl_pf(s->id, sizeof s->id, "%s", a->id);
    cl_pf(s->name, sizeof s->name, "%s", a->name);
    s->in_use = a->in_use;
    s->mounted = a->mounted;
    s->paused = cl_paused(a);
    s->keep = calloc((size_t)a->nkeep + 1, sizeof *s->keep);
    s->q = calloc((size_t)a->nq + 1, sizeof *s->q);
    for (int i = 0; s->keep && i < a->nkeep; i++) s->keep[s->nkeep++] = strdup(a->keep[i]);
    for (int i = 0; s->q && i < a->nq; i++) s->q[s->nq++] = strdup(a->q[i].name);
    pthread_mutex_unlock(&cl_mu);
    return 0;
}

static void snap_free(Snap *s) {
    for (int i = 0; i < s->nkeep; i++) free(s->keep[i]);
    for (int i = 0; i < s->nq; i++) free(s->q[i]);
    free(s->keep);
    free(s->q);
}

static int queued(const Snap *s, const char *rel) { for (int i = 0; i < s->nq; i++) if (s->q[i] && !strcmp(s->q[i], rel)) return 1; return 0; }

static int read_meta(const char *id, const char *rel, CloudMeta *m) {
    char p[PATH_MAX * 2];
    cl_pf(p, sizeof p, "%s/vfsMeta/%s/%s", cl_cache, id, rel);
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    char b[65536];
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    return cloud_meta_parse(b, (size_t)n, m);
}

/* ---- the files this user's programs have open under ~/Cloud ---- */

typedef struct { char **p; int n, cap; } OpenSet;

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void open_scan(OpenSet *o) {
    memset(o, 0, sizeof *o);
    DIR *pd = opendir("/proc");
    if (!pd) return;
    size_t rl = strlen(cl_root);
    uid_t me = getuid();
    for (struct dirent *e; (e = readdir(pd)) && o->n < 65536; ) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char fdd[64];
        cl_pf(fdd, sizeof fdd, "/proc/%s/fd", e->d_name);
        struct stat st;
        if (stat(fdd, &st) || st.st_uid != me) continue;
        DIR *fd = opendir(fdd);
        if (!fd) continue;
        for (struct dirent *f; (f = readdir(fd)); ) {
            if (f->d_name[0] == '.') continue;
            char lp[96], t[PATH_MAX];
            cl_pf(lp, sizeof lp, "%s/%s", fdd, f->d_name);
            ssize_t n = readlink(lp, t, sizeof t - 1);
            if (n <= 0) continue;
            t[n] = 0;
            if (strncmp(t, cl_root, rl) || t[rl] != '/') continue;
            if (o->n == o->cap) {
                int nc = o->cap ? o->cap * 2 : 64;
                char **np = realloc(o->p, sizeof *np * (size_t)nc);
                if (!np) break;
                o->p = np;
                o->cap = nc;
            }
            o->p[o->n++] = strdup(t);
        }
        closedir(fd);
    }
    closedir(pd);
    if (o->n) qsort(o->p, (size_t)o->n, sizeof *o->p, cmp_str);
}

static int open_has(const OpenSet *o, const char *path) {
    const char *k = path;
    return o->n && bsearch(&k, o->p, (size_t)o->n, sizeof *o->p, cmp_str) != NULL;
}

static void open_free(OpenSet *o) { for (int i = 0; i < o->n; i++) free(o->p[i]); free(o->p); memset(o, 0, sizeof *o); }

/* the candidate for eviction that rel is */
static void judge(const Snap *s, const OpenSet *o, const char *rel, CloudCand *c, CloudMeta *m) {
    memset(c, 0, sizeof *c);
    int have = !read_meta(s->id, rel, m);
    if (!have) memset(m, 0, sizeof *m);
    char full[PATH_MAX * 2], data[PATH_MAX * 2];
    cl_pf(full, sizeof full, "%s/%s/%s", cl_root, s->name, rel);
    cl_pf(data, sizeof data, "%s/vfs/%s/%s", cl_cache, s->id, rel);
    struct stat st;
    c->bytes = !lstat(data, &st) && S_ISREG(st.st_mode) ? (long long)st.st_blocks * 512 : 0;
    c->atime = have ? m->atime : 0;
    c->kept = cloud_kept((const char *const *)s->keep, s->nkeep, rel);
    c->dirty = have && m->dirty;
    c->queued = queued(s, rel);
    c->open = open_has(o, full) || (s->in_use > 0 && have && m->atime >= 0 && time(NULL) - m->atime < HANDLE_HOLD_S);
}

/* drops one clean file's cached copy: bytes freed, or -1 */
static long long evict(const Snap *s, const char *rel) {
    CloudMeta m;
    char meta[PATH_MAX * 2], data[PATH_MAX * 2];
    /* again, right before: a write may have started since the choice */
    if (!read_meta(s->id, rel, &m) && m.dirty) return -1;
    cl_pf(meta, sizeof meta, "%s/vfsMeta/%s/%s", cl_cache, s->id, rel);
    cl_pf(data, sizeof data, "%s/vfs/%s/%s", cl_cache, s->id, rel);
    struct stat st;
    long long b = !lstat(data, &st) && S_ISREG(st.st_mode) ? (long long)st.st_blocks * 512 : 0;
    if (unlink(data) && errno != ENOENT) return -1;
    unlink(meta);
    return b;
}

/* every file under vfsMeta/<id>/<rel>: fn(rel of the file) */
typedef void (*FileFn)(const char *rel, void *arg);
static void walk_meta(const char *base, const char *rel, int depth, FileFn fn, void *arg) {
    char p[PATH_MAX * 2];
    cl_pf(p, sizeof p, "%s%s%s", base, rel[0] ? "/" : "", rel);
    struct stat st;
    if (lstat(p, &st)) return;
    if (S_ISREG(st.st_mode)) { fn(rel, arg); return; }
    if (!S_ISDIR(st.st_mode) || depth > WALK_DEPTH) return;
    DIR *d = opendir(p);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d)); ) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char r[CLOUD_REL_MAX * 2 + 2];
        if ((size_t)cl_pf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name) > CLOUD_REL_MAX) continue;
        walk_meta(base, r, depth + 1, fn, arg);
    }
    closedir(d);
}

/* ---- status (7.2) ---- */

static void live_of(Acc *a, const char *rel, CloudLive *l) {
    memset(l, 0, sizeof *l);
    for (int i = 0; i < a->nq; i++) if (!strcmp(a->q[i].name, rel)) l->queued = 1;
    for (int i = 0; i < a->nt; i++) {
        if (strcmp(a->t[i].name, rel)) continue;
        if (a->t[i].up) { l->uploading = 1; l->up_pct = a->t[i].pct; }
        else { l->downloading = 1; l->down_pct = a->t[i].pct; }
    }
    for (int i = 0; i < a->nerrn; i++) if (!strcmp(a->errn[i], rel)) l->error = 1;
    l->kept = cloud_kept((const char *const *)a->keep, a->nkeep, rel);
}

static int under(const char *name, const char *dir) {
    size_t l = strlen(dir);
    return !l || (!strncmp(name, dir, l) && name[l] == '/');
}

char *cf_status(const char *const *paths, int n) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{");
    int first = 1;
    for (int i = 0; i < n; i++) {
        char name[CLOUD_NAME_MAX + 1], rel[CLOUD_REL_MAX + 1];
        if (!paths[i] || cloud_split(cl_root, paths[i], name, sizeof name, rel, sizeof rel)) continue;
        pthread_mutex_lock(&cl_mu);
        Acc *a = cl_find_name(name);
        if (!a) { pthread_mutex_unlock(&cl_mu); continue; }
        char mp[PATH_MAX * 2];
        cl_pf(mp, sizeof mp, "%s/vfsMeta/%s%s%s", cl_cache, a->id, rel[0] ? "/" : "", rel);
        struct stat st;
        const char *s;
        double pct = -1;
        if (!rel[0] || (!stat(mp, &st) && S_ISDIR(st.st_mode))) {
            /* a folder: kept, or the direction of what moves inside it */
            int up = 0, down = 0;
            for (int k = 0; k < a->nq; k++) up |= under(a->q[k].name, rel);
            for (int k = 0; k < a->nt; k++) { if (a->t[k].up) up |= under(a->t[k].name, rel); else down |= under(a->t[k].name, rel); }
            s = up ? "up" : down ? "down" : cloud_kept((const char *const *)a->keep, a->nkeep, rel) ? "kept" : "cloud";
        } else {
            CloudMeta m;
            int have = !read_meta(a->id, rel, &m);
            CloudLive l;
            live_of(a, rel, &l);
            s = cloud_file_status(have ? &m : NULL, &l, &pct);
        }
        pthread_mutex_unlock(&cl_mu);
        if (!first) jb_raw(&b, ",");
        first = 0;
        jb_str(&b, paths[i]);
        jb_printf(&b, ":{\"st\":\"%s\"", s);
        if (pct >= 0) { jb_raw(&b, ",\"pct\":"); jb_num(&b, pct); }
        if (!strcmp(s, "error")) jb_raw(&b, ",\"error\":\"cloud.transferFailed\"");
        jb_raw(&b, "}");
    }
    jb_raw(&b, "}");
    return b.oom ? (jb_free(&b), NULL) : b.p;
}

/* ---- Free Up Space ---- */

typedef struct { const Snap *s; const OpenSet *o; long long freed; JBuf *skip; int nskip; } FreeCtx;

static void free_one(const char *rel, void *arg) {
    FreeCtx *f = arg;
    CloudCand c;
    CloudMeta m;
    judge(f->s, f->o, rel, &c, &m);
    int why = cloud_evict_why(&c);
    if (why == CE_OK) {
        long long b = evict(f->s, rel);
        if (b >= 0) { f->freed += b; return; }
        why = CE_PENDING;
    }
    if (f->nskip >= SKIP_MAX) return;
    char full[PATH_MAX * 2];
    cl_pf(full, sizeof full, "%s/%s/%s", cl_root, f->s->name, rel);
    jb_raw(f->skip, f->nskip++ ? ",{\"path\":" : "{\"path\":");
    jb_str(f->skip, full);
    jb_printf(f->skip, ",\"reason\":\"%s\"}", cloud_skip_name(why));
}

char *cf_free(const char *const *paths, int n) {
    JBuf skip;
    jb_init(&skip);
    long long freed = 0;
    int nskip = 0, scanned = 0;
    OpenSet o = { 0 };
    for (int i = 0; i < n; i++) {
        char name[CLOUD_NAME_MAX + 1], rel[CLOUD_REL_MAX + 1];
        if (!paths[i] || cloud_split(cl_root, paths[i], name, sizeof name, rel, sizeof rel)) continue;
        Snap s;
        if (snap(name, NULL, &s)) continue;
        if (!scanned && s.in_use > 0) { open_scan(&o); scanned = 1; }   /* rclone says something is open: who */
        char base[PATH_MAX];
        cl_pf(base, sizeof base, "%s/vfsMeta/%s", cl_cache, s.id);
        FreeCtx f = { &s, &o, 0, &skip, nskip };
        walk_meta(base, rel, 0, free_one, &f);
        /* a file that never came down: nothing to free, and not a skip */
        freed += f.freed;
        nskip = f.nskip;
        snap_free(&s);
    }
    open_free(&o);
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"freed\":%lld,\"skipped\":[", freed);
    if (!skip.oom && skip.n) jb_rawn(&b, skip.p, skip.n);
    jb_raw(&b, "]}");
    jb_free(&skip);
    cl_changed();
    return b.oom ? (jb_free(&b), NULL) : b.p;
}

/* ---- space used, and the automatic pass ---- */

typedef struct { const Snap *s; long long cache, kept; } MeasureCtx;

static void measure_one(const char *rel, void *arg) {
    MeasureCtx *m = arg;
    char data[PATH_MAX * 2];
    cl_pf(data, sizeof data, "%s/vfs/%s/%s", cl_cache, m->s->id, rel);
    struct stat st;
    if (lstat(data, &st) || !S_ISREG(st.st_mode)) return;
    long long b = (long long)st.st_blocks * 512;
    m->cache += b;
    if (cloud_kept((const char *const *)m->s->keep, m->s->nkeep, rel)) m->kept += b;
}

void cf_measure(void) {
    char ids[ACC_MAX][16];
    int n = 0;
    pthread_mutex_lock(&cl_mu);
    for (int i = 0; i < ACC_MAX; i++) if (cl_acc[i].used) cl_pf(ids[n++], sizeof ids[0], "%s", cl_acc[i].id);
    pthread_mutex_unlock(&cl_mu);
    for (int i = 0; i < n; i++) {
        Snap s;
        if (snap(NULL, ids[i], &s)) continue;
        char base[PATH_MAX];
        cl_pf(base, sizeof base, "%s/vfs/%s", cl_cache, s.id);
        MeasureCtx m = { &s, 0, 0 };
        walk_meta(base, "", 0, measure_one, &m);      /* the data tree has the same shape */
        pthread_mutex_lock(&cl_mu);
        Acc *a = cl_find(ids[i]);
        if (a) { a->cache_bytes = m.cache; a->kept_bytes = m.kept; }
        pthread_mutex_unlock(&cl_mu);
        snap_free(&s);
    }
    cl_changed();
}

typedef struct { const Snap *s; const OpenSet *o; CloudCand *c; char **rel; int *who; int n, si; } AutoCtx;
static void auto_one(const char *rel, void *arg) {
    AutoCtx *x = arg;
    if (x->n >= CAND_MAX) return;
    CloudMeta m;
    judge(x->s, x->o, rel, &x->c[x->n], &m);
    if (cloud_evict_why(&x->c[x->n]) != CE_OK || !x->c[x->n].bytes) return;
    x->rel[x->n] = strdup(rel);
    if (!x->rel[x->n]) return;
    x->who[x->n] = x->si;
    x->n++;
}

void cf_auto_free(int below_gb) {
    struct statvfs v;
    if (statvfs(cl_home, &v)) return;
    long long avail = (long long)v.f_bavail * (long long)v.f_frsize, want = ((long long)below_gb + 1) << 30;
    if (avail >= ((long long)below_gb << 30)) return;
    long long need = want - avail;
    char ids[ACC_MAX][16];
    int na = 0;
    pthread_mutex_lock(&cl_mu);
    for (int i = 0; i < ACC_MAX; i++) if (cl_acc[i].used) cl_pf(ids[na++], sizeof ids[0], "%s", cl_acc[i].id);
    pthread_mutex_unlock(&cl_mu);
    Snap *ss = calloc((size_t)na + 1, sizeof *ss);
    CloudCand *c = calloc(CAND_MAX, sizeof *c);
    char **rel = calloc(CAND_MAX, sizeof *rel);
    int *who = calloc(CAND_MAX, sizeof *who), *pick = calloc(CAND_MAX, sizeof *pick);
    if (!ss || !c || !rel || !who || !pick) goto out;
    OpenSet o = { 0 };
    int scanned = 0, ns = 0;
    AutoCtx x = { NULL, &o, c, rel, who, 0, 0 };
    for (int i = 0; i < na; i++) {
        if (snap(NULL, ids[i], &ss[ns])) continue;
        if (!scanned && ss[ns].in_use > 0) { open_scan(&o); scanned = 1; }
        char base[PATH_MAX];
        cl_pf(base, sizeof base, "%s/vfsMeta/%s", cl_cache, ss[ns].id);
        x.s = &ss[ns];
        x.si = ns;
        walk_meta(base, "", 0, auto_one, &x);
        ns++;
    }
    int k = cloud_evict_lru(c, x.n, need, pick);
    long long got = 0;
    for (int i = 0; i < k; i++) { long long b = evict(&ss[who[pick[i]]], rel[pick[i]]); if (b > 0) got += b; }
    if (k) fprintf(stderr, "cloud: the disk was low on space: freed %lld MB of cached cloud files (%d)\n", got >> 20, k);
    for (int i = 0; i < x.n; i++) free(rel[i]);
    for (int i = 0; i < ns; i++) snap_free(&ss[i]);
    open_free(&o);
out:
    free(ss); free(c); free(rel); free(who); free(pick);
    cl_changed();
}

static long long rm_bytes;
static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *f) {
    (void)f;
    if (flag == FTW_F || flag == FTW_SL) { rm_bytes += (long long)st->st_blocks * 512; unlink(p); }
    else if (flag == FTW_DP) rmdir(p);
    return 0;
}

long long cf_rm_tree(const char *path) {
    rm_bytes = 0;
    nftw(path, rm_one, 16, FTW_DEPTH | FTW_PHYS);
    return rm_bytes;
}

/* ---- Keep on This Computer: a thread at the lowest priority reads kept files through the mount ---- */

static pthread_mutex_t kmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kcv = PTHREAD_COND_INITIALIZER;
static unsigned kgen;

void cf_kick(void) {
    pthread_mutex_lock(&kmu);
    kgen++;
    pthread_cond_signal(&kcv);
    pthread_mutex_unlock(&kmu);
}

static int still_ok(const char *id) {
    pthread_mutex_lock(&cl_mu);
    Acc *a = cl_find(id);
    int ok = a && a->mounted && !cl_paused(a);
    pthread_mutex_unlock(&cl_mu);
    return ok;
}

/* reads one file to the end: rclone keeps what was read */
static void fetch(const Snap *s, const char *rel) {
    CloudMeta m;
    if (!read_meta(s->id, rel, &m) && m.cached >= m.size) return;
    char p[PATH_MAX * 2];
    cl_pf(p, sizeof p, "%s/%s/%s", cl_root, s->name, rel);
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    static char buf[1 << 20];
    long long done = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        done += n;
        if ((done & ((64 << 20) - 1)) < n && !still_ok(s->id)) break;      /* every 64 MB: paused, gone? */
    }
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
}

static void fetch_tree(const Snap *s, const char *rel, int depth) {
    char p[PATH_MAX * 2];
    cl_pf(p, sizeof p, "%s/%s%s%s", cl_root, s->name, rel[0] ? "/" : "", rel);
    struct stat st;
    if (lstat(p, &st)) return;
    if (S_ISREG(st.st_mode)) { fetch(s, rel); return; }
    if (!S_ISDIR(st.st_mode) || depth > WALK_DEPTH) return;
    DIR *d = opendir(p);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d)); ) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char r[CLOUD_REL_MAX * 2 + 2];
        if ((size_t)cl_pf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name) > CLOUD_REL_MAX) continue;
        if (!still_ok(s->id)) break;
        fetch_tree(s, r, depth + 1);
    }
    closedir(d);
}

static void *keeper(void *arg) {
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
    syscall(SYS_ioprio_set, 1, (int)syscall(SYS_gettid), 3 << 13);   /* idle I/O, like Backups */
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&kmu);
        while (kgen == seen) {
            struct timespec t;
            clock_gettime(CLOCK_REALTIME, &t);
            t.tv_sec += 300;                          /* new files in kept folders: every 5 minutes */
            if (pthread_cond_timedwait(&kcv, &kmu, &t) == ETIMEDOUT) break;
        }
        seen = kgen;
        pthread_mutex_unlock(&kmu);
        char ids[ACC_MAX][16];
        int na = 0;
        pthread_mutex_lock(&cl_mu);
        for (int i = 0; i < ACC_MAX; i++) if (cl_acc[i].used && cl_acc[i].mounted && cl_acc[i].nkeep) cl_pf(ids[na++], sizeof ids[0], "%s", cl_acc[i].id);
        pthread_mutex_unlock(&cl_mu);
        for (int i = 0; i < na; i++) {
            Snap s;
            if (snap(NULL, ids[i], &s)) continue;
            for (int k = 0; k < s.nkeep && s.mounted && !s.paused; k++) {
                JBuf b;
                jb_init(&b);
                jb_printf(&b, "{\"fs\":\"%s:\",\"recursive\":true", s.id);
                if (s.keep[k][0]) {
                    /* vfs/refresh takes folders: a kept file refreshes its folder */
                    char dir[CLOUD_REL_MAX + 1];
                    cl_pf(dir, sizeof dir, "%s", s.keep[k]);
                    char full[PATH_MAX * 2];
                    cl_pf(full, sizeof full, "%s/%s/%s", cl_root, s.name, s.keep[k]);
                    struct stat st;
                    if (!stat(full, &st) && !S_ISDIR(st.st_mode)) { char *sl = strrchr(dir, '/'); if (sl) *sl = 0; else dir[0] = 0; }
                    if (dir[0]) { jb_raw(&b, ",\"dir\":"); jb_str(&b, dir); }
                }
                jb_raw(&b, "}");
                if (!b.oom) cl_rc_simple(RCM_VFS_REFRESH, b.p, 60000);
                jb_free(&b);
                fetch_tree(&s, s.keep[k], 0);
            }
            snap_free(&s);
        }
        cf_measure();
    }
    return NULL;
}

void cf_start(void) {
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 2 * 1024 * 1024);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    pthread_create(&th, &at, keeper, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&at);
}

/* ---- Delete in the cloud (docs/CLOUD.md 17.2 item 6) ----
 * Files' Move to Trash under ~/Cloud would leave a .Trash-<uid> folder in the cloud: it deletes through the mount instead,
 * and the provider keeps its own trash where it has one. Only paths strictly inside an account's folder, never the folder
 * itself, with no "..", "." or "//", whose parent folders are real folders (no link on the way); links are removed, never
 * followed; a folder is removed with what is in it, without leaving the mount. One thread per request. */

typedef struct { int req, n; char **paths; } DelJob;
static __thread int del_count;
static __thread int del_err;

static int del_one(const char *p, const struct stat *st, int flag, struct FTW *f) {
    (void)st; (void)f;
    int r = flag == FTW_DP ? rmdir(p) : (flag == FTW_D || flag == FTW_DNR) ? 0 : unlink(p);
    if (r) del_err = errno; else del_count++;
    return 0;
}

static void *del_thread(void *arg) {
    DelJob *j = arg;
    JBuf b, f;
    jb_init(&b);
    jb_init(&f);
    int total = 0, nf = 0;
    for (int i = 0; i < j->n; i++) {
        struct stat st;
        del_count = 0;
        del_err = 0;
        if (lstat(j->paths[i], &st)) del_err = errno;
        else if (S_ISDIR(st.st_mode)) nftw(j->paths[i], del_one, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
        else if (unlink(j->paths[i])) del_err = errno; else del_count = 1;
        total += del_count;
        if (del_err) {
            jb_raw(&f, nf++ ? ",{\"path\":" : "{\"path\":");
            jb_str(&f, j->paths[i]);
            jb_raw(&f, ",\"error\":\"cloud.deleteFailed\",\"errno\":");
            jb_str(&f, lerr_errno(del_err));
            jb_raw(&f, "}");
        }
        free(j->paths[i]);
    }
    jb_printf(&b, "{\"ok\":%s,\"deleted\":%d,\"failed\":[", nf ? "false" : "true", total);
    if (!f.oom && f.n) jb_rawn(&b, f.p, f.n);
    jb_raw(&b, "]}");
    jb_free(&f);
    cl_reply(j->req, b.oom ? (jb_free(&b), NULL) : b.p);
    free(j->paths);
    free(j);
    cf_measure();
    cl_changed();
    return NULL;
}

static char *bad_path(const char *code, const char *path) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"error\":");
    jb_str(&b, code);
    jb_raw(&b, ",\"path\":");
    jb_str(&b, path ? path : "");
    jb_raw(&b, "}");
    return b.oom ? (jb_free(&b), NULL) : b.p;
}

char *cf_delete(const char *const *paths, int n, int *req) {
    *req = 0;
    if (n <= 0 || n > 4096) return bad_path("cloud.badArgument", "");
    for (int i = 0; i < n; i++) {
        char name[CLOUD_NAME_MAX + 1], rel[CLOUD_REL_MAX + 1];
        if (!paths[i] || cloud_split(cl_root, paths[i], name, sizeof name, rel, sizeof rel) || !rel[0]) return bad_path("cloud.badPath", paths[i]);
        pthread_mutex_lock(&cl_mu);
        Acc *a = cl_find_name(name);
        int mounted = a && a->mounted;
        const char *why = !a || mounted ? NULL : !cl_running ? "cloud.notRunning" : a->locked == 1 ? "cloud.keyringLocked"
                        : a->locked == 2 ? "cloud.noKeyring" : "cloud.notConnected";
        pthread_mutex_unlock(&cl_mu);
        if (!a) return bad_path("cloud.notFound", paths[i]);
        if (!mounted) return bad_path(why, paths[i]);
        /* every folder on the way is a real folder: the parent's real path is its own text */
        char parent[PATH_MAX], real[PATH_MAX];
        cl_pf(parent, sizeof parent, "%s", paths[i]);
        char *sl = strrchr(parent, '/');
        if (!sl) return bad_path("cloud.badPath", paths[i]);
        *sl = 0;
        if (!realpath(parent, real) || strcmp(real, parent)) return bad_path("cloud.badPath", paths[i]);
    }
    DelJob *j = calloc(1, sizeof *j);
    if (!j || !(j->paths = calloc((size_t)n, sizeof *j->paths))) { free(j); return bad_path("cloud.failed", ""); }
    for (int i = 0; i < n; i++) j->paths[j->n++] = strdup(paths[i]);
    j->req = *req = cl_next_req();
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    int r = pthread_create(&th, &at, del_thread, j);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&at);
    if (r) { for (int i = 0; i < j->n; i++) free(j->paths[i]); free(j->paths); free(j); *req = 0; return bad_path("cloud.failed", ""); }
    return NULL;
}
