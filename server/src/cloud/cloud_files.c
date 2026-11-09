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
#include <time.h>
#include <unistd.h>

#define HANDLE_HOLD_S 10            /* rclone keeps a closed file's handle 5 s (HandleCaching); twice that */
#define SKIP_MAX 200
#define CAND_MAX 50000
#define WALK_DEPTH 32

/* what one account looks like, copied out from under cl_mu */
typedef struct {
    char id[16], name[CLOUD_NAME_MAX + 1];
    int in_use, mounted, paused, offline;
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
    s->offline = cloud_err_offline(a->err) || a->keeper_wait;      /* or not checked yet since it was mounted or resumed */
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
            /* a folder: kept, or the direction of what moves inside it. A kept folder whose files are still coming down
             * stays "down" between one file and the next, not kept for a moment each time one ends (#71) */
            int up = 0, down = 0;
            for (int k = 0; k < a->nq; k++) up |= under(a->q[k].name, rel);
            for (int k = 0; k < a->nt; k++) { if (a->t[k].up) up |= under(a->t[k].name, rel); else down |= under(a->t[k].name, rel); }
            down |= cloud_kept_busy((const char *const *)a->keep, a->keep_busy, a->nkeep, rel);
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

int cloud_read_would_block(const char *path) {
    char name[CLOUD_NAME_MAX + 1], rel[CLOUD_REL_MAX + 1];
    if (!cl_root[0] || !path || cloud_split(cl_root, path, name, sizeof name, rel, sizeof rel) || !rel[0]) return 0;
    pthread_mutex_lock(&cl_mu);
    Acc *a = cl_find_name(name);
    int down = a && (!cl_running || !a->mounted || a->locked || cloud_err_offline(a->err));
    char id[16] = "";
    if (a) cl_pf(id, sizeof id, "%s", a->id);
    pthread_mutex_unlock(&cl_mu);
    if (!down) return 0;
    CloudMeta m;
    return read_meta(id, rel, &m) || m.cached < m.size;     /* what is here still opens */
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
    int ok = a && a->mounted && !cl_paused(a) && !cloud_err_offline(a->err);
    pthread_mutex_unlock(&cl_mu);
    return ok;
}

/* One kept entry's pass of the keeper (#71): the entry is marked as coming down when a file of it is missing, and
 * cleared when a pass read everything (a pass that stopped, paused or gone, leaves it as it was) */
typedef struct { const char *root; int marked, incomplete, gave_up; } KeepPass;

/* #85: a call through the mount failed because the provider didn't answer (pCloud after a pause, its address not found
 * for minutes): rclone held each folder while it waited, so a pass that went on walked folder after folder, 15 s each,
 * and whatever else read those folders waited with it. The pass stops at the first one and the account is checked now;
 * the keeper comes back when the check finds it answering (check_acc) */
static void gave_up(const Snap *s, KeepPass *kp, int err) {
    kp->incomplete = 1;
    if (kp->gave_up || !cloud_mount_gave_up(err)) return;
    kp->gave_up = 1;
    fprintf(stderr, "cloud: %s: the provider didn't answer through the mount: Keep on This Computer waits for it\n", s->id);
    cl_check_soon(s->id);
}

static void keep_busy_set(const char *id, const char *root, int busy) {
    int changed = 0;
    pthread_mutex_lock(&cl_mu);
    Acc *a = cl_find(id);
    for (int i = 0; a && i < a->nkeep; i++)
        if (!strcmp(a->keep[i], root) && a->keep_busy[i] != busy) { a->keep_busy[i] = (unsigned char)busy; changed = 1; }
    pthread_mutex_unlock(&cl_mu);
    if (changed) cl_changed();
}

/* reads one file to the end: rclone keeps what was read */
static void fetch(const Snap *s, const char *rel, KeepPass *kp) {
    CloudMeta m;
    if (!read_meta(s->id, rel, &m) && m.cached >= m.size) return;
    if (!kp->marked) { keep_busy_set(s->id, kp->root, 1); kp->marked = 1; }
    char p[PATH_MAX * 2];
    cl_pf(p, sizeof p, "%s/%s/%s", cl_root, s->name, rel);
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { gave_up(s, kp, errno); return; }
    static char buf[1 << 20];
    long long done = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        done += n;
        if ((done & ((64 << 20) - 1)) < n && !still_ok(s->id)) { kp->incomplete = 1; break; }   /* every 64 MB: paused, gone? */
    }
    if (n < 0) gave_up(s, kp, errno);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
}

static void fetch_tree(const Snap *s, const char *rel, int depth, KeepPass *kp) {
    char p[PATH_MAX * 2];
    cl_pf(p, sizeof p, "%s/%s%s%s", cl_root, s->name, rel[0] ? "/" : "", rel);
    struct stat st;
    if (lstat(p, &st)) { if (errno != ENOENT) gave_up(s, kp, errno); return; }   /* ENOENT: gone (in the cloud too) */
    if (S_ISREG(st.st_mode)) { fetch(s, rel, kp); return; }
    if (!S_ISDIR(st.st_mode) || depth > WALK_DEPTH) return;
    DIR *d = opendir(p);
    if (!d) { gave_up(s, kp, errno); return; }
    errno = 0;
    for (struct dirent *e; (e = readdir(d)); errno = 0) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char r[CLOUD_REL_MAX * 2 + 2];
        if ((size_t)cl_pf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name) > CLOUD_REL_MAX) continue;
        if (kp->gave_up || !still_ok(s->id)) { kp->incomplete = 1; break; }
        fetch_tree(s, r, depth + 1, kp);
    }
    if (errno && !kp->incomplete) gave_up(s, kp, errno);       /* readdir failed part way */
    closedir(d);
}

/* a whole pass over one kept entry; when it read everything, the entry is all here. -1: the provider didn't answer */
static int fetch_kept(const Snap *s, const char *root) {
    KeepPass kp = { root, 0, 0, 0 };
    fetch_tree(s, root, 0, &kp);
    if (!kp.incomplete) keep_busy_set(s->id, root, 0);
    return kp.gave_up ? -1 : 0;
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
            /* #85: nothing walks an account whose provider doesn't answer, nor one not checked yet since it was mounted or
             * resumed: check_acc lists it first (every 30 s while it doesn't answer) and calls the keeper back */
            if (!s.mounted || s.paused || s.offline) { snap_free(&s); continue; }
            /* the kept folders listed whole again (7.3): every pass where the provider sends no changes, else after each
             * mount and every half hour, inside the hour a listing is kept */
            int tree = 0;
            pthread_mutex_lock(&cl_mu);
            Acc *a = cl_find(s.id);
            time_t now = time(NULL);
            if (a && (!a->polls || a->tree_gen != a->mount_gen || now - a->tree_at >= CLOUD_TREE_POLL_S)) {
                tree = 1;
                a->tree_gen = a->mount_gen;
                a->tree_at = now;
            }
            pthread_mutex_unlock(&cl_mu);
            for (int k = 0; k < s.nkeep && still_ok(s.id); k++) {
                if (!tree) { if (fetch_kept(&s, s.keep[k])) break; continue; }
                JBuf b;
                jb_init(&b);
                /* vfs/refresh takes folders: a kept file refreshes its folder, alone (not what is under it). rclone takes
                 * "recursive" as text only: a JSON true is refused ("value must be string"), and was until 0.8.8.6 */
                char dir[CLOUD_REL_MAX + 1];
                cl_pf(dir, sizeof dir, "%s", s.keep[k]);
                int whole = 1;
                if (dir[0]) {
                    char full[PATH_MAX * 2];
                    cl_pf(full, sizeof full, "%s/%s/%s", cl_root, s.name, s.keep[k]);
                    struct stat st;
                    if (!stat(full, &st) && !S_ISDIR(st.st_mode)) { whole = 0; char *sl = strrchr(dir, '/'); if (sl) *sl = 0; else dir[0] = 0; }
                }
                jb_printf(&b, "{\"fs\":\"%s:\",\"recursive\":\"%s\"", s.id, whole ? "true" : "false");
                if (dir[0]) { jb_raw(&b, ",\"dir\":"); jb_str(&b, dir); }
                jb_raw(&b, "}");
                /* a whole account can take minutes; rclone's walk doesn't hold the folders it lists (measured) */
                int st = b.oom ? -1 : cl_rc_simple(RCM_VFS_REFRESH, b.p, 600000);
                jb_free(&b);
                if (st != 200) cl_check_soon(s.id);           /* the provider may not be answering: asked now */
                if (fetch_kept(&s, s.keep[k])) break;
            }
            snap_free(&s);
        }
        cf_measure();
    }
    return NULL;
}

/* ---- a folder Files shows (7.3): listed again off the interface's thread, its subfolders listed ahead ----
 * rclone lists a folder from the provider the first time it is read and when its listing expired, and the reader waits.
 * Files asks here instead (cloud.browse, on opening a folder of an account and every minute while it shows it): this
 * thread lists the folder again when it is old (cloud_browse_due), then the subfolders never listed in this mount, so the
 * next click doesn't wait. While rclone lists one folder again it holds that folder (measured), so a folder is never
 * listed again just because it is shown: only once it is CLOUD_STALE_S old, on a provider that sends no changes. */

static pthread_mutex_t bmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bcv = PTHREAD_COND_INITIALIZER;
static char bwant[PATH_MAX];
static unsigned bgen;
static CloudSeen seen;              /* the browser thread's only */

static long long ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

void cf_browse(const char *path) {
    if (!path || strlen(path) >= sizeof bwant) return;
    pthread_mutex_lock(&bmu);
    memcpy(bwant, path, strlen(path) + 1);
    bgen++;
    pthread_cond_signal(&bcv);
    pthread_mutex_unlock(&bmu);
}

static int browse_moved(unsigned gen) {
    pthread_mutex_lock(&bmu);
    int r = bgen != gen;
    pthread_mutex_unlock(&bmu);
    return r;
}

/* the account of a folder, as it is now: 0 and its id, mount and what it is; -1 not mounted or not reachable */
static int browse_acc(const char *name, const char *rel, char *id, unsigned *gen, int *polls, int *kept) {
    pthread_mutex_lock(&cl_mu);
    Acc *a = cl_find_name(name);
    int ok = a && cl_running && a->mounted && !a->locked && !cloud_err_offline(a->err);
    if (ok) {
        cl_pf(id, 16, "%s", a->id);
        *gen = a->mount_gen;
        *polls = a->polls;
        *kept = cloud_kept((const char *const *)a->keep, a->nkeep, rel);
    }
    pthread_mutex_unlock(&cl_mu);
    return ok ? 0 : -1;
}

static int refresh_dir(const char *id, const char *rel) {
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"fs\":\"%s:\"", id);
    if (rel[0]) { jb_raw(&b, ",\"dir\":"); jb_str(&b, rel); }
    jb_raw(&b, "}");
    int st = b.oom ? -1 : cl_rc_simple(RCM_VFS_REFRESH, b.p, 60000);
    jb_free(&b);
    return st;
}

static void *browser(void *arg) {
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    unsigned done = 0;
    for (;;) {
        char path[PATH_MAX], name[CLOUD_NAME_MAX + 1], rel[CLOUD_REL_MAX + 1], id[16];
        pthread_mutex_lock(&bmu);
        while (bgen == done) pthread_cond_wait(&bcv, &bmu);
        done = bgen;
        memcpy(path, bwant, sizeof path);
        pthread_mutex_unlock(&bmu);
        unsigned gen;
        int polls, kept;
        if (cloud_split(cl_root, path, name, sizeof name, rel, sizeof rel) || browse_acc(name, rel, id, &gen, &polls, &kept)) continue;
        unsigned long long key = cloud_seen_key(id, gen, rel);
        long long at = cloud_seen_at(&seen, key);
        if (cloud_browse_due(at, ms_now(), polls, kept)) {
            if (refresh_dir(id, rel) == 200) {
                pthread_mutex_lock(&cl_mu);
                Acc *a = cl_find(id);
                if (a && a->mount_gen == gen) a->listed++;      /* Files lists the folder again */
                pthread_mutex_unlock(&cl_mu);
                cl_changed();
            }
            cloud_seen_put(&seen, key, ms_now());
        } else if (at < 0) cloud_seen_put(&seen, key, ms_now());   /* Files has just listed it */
        if (kept) continue;                                         /* the keeper lists kept folders whole */
        /* the subfolders never listed in this mount, through rclone's listing of this one (no provider call) */
        char dir[PATH_MAX * 2], sub[CLOUD_AHEAD_MAX][256];
        int ns = 0;
        cl_pf(dir, sizeof dir, "%s/%s%s%s", cl_root, name, rel[0] ? "/" : "", rel);
        DIR *d = opendir(dir);
        for (struct dirent *e; d && ns < CLOUD_AHEAD_MAX && (e = readdir(d)); ) {
            if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof sub[0]) continue;
            int isdir = e->d_type == DT_DIR;
            struct stat st;
            if (e->d_type == DT_UNKNOWN) isdir = !fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) && S_ISDIR(st.st_mode);
            if (isdir) memcpy(sub[ns++], e->d_name, strlen(e->d_name) + 1);
        }
        if (d) closedir(d);
        for (int i = 0; i < ns && !browse_moved(done); i++) {
            char r2[CLOUD_REL_MAX + 1];
            if ((size_t)cl_pf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", sub[i]) >= sizeof r2 || !cloud_rel_ok(r2)) continue;
            unsigned g2;
            int p2, k2;
            if (browse_acc(name, r2, id, &g2, &p2, &k2) || g2 != gen) break;
            unsigned long long k = cloud_seen_key(id, gen, r2);
            if (k2 || cloud_seen_at(&seen, k) >= 0) continue;
            refresh_dir(id, r2);
            cloud_seen_put(&seen, k, ms_now());
        }
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
    pthread_create(&th, &at, browser, NULL);
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

/* ---- search inside an account (N2) ----
 * lord.fs.search doesn't enter mounts (the interface's thread must not wait on a server). cloud.search walks the mount in a
 * thread of its own, with a budget (15 s, 50 000 entries, 16 levels, 500 hits), and answers once: { hits, complete }. The
 * walk goes through rclone's directory cache, so a folder listed lately costs nothing and an unreachable server only slows
 * this thread. Names are matched without case, as lord.fs.search. */

typedef struct { int req; char *root, *needle; long long end; int seen, depth_max; JBuf hits; int nhits, complete; } SearchJob;

static int ci_has(const char *hay, const char *n) {
    size_t l = strlen(n);
    for (; *hay; hay++) if (!strncasecmp(hay, n, l)) return 1;
    return 0;
}

static long long mono_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static void search_walk(SearchJob *j, const char *dir, int depth) {
    if (depth > j->depth_max) { j->complete = 0; return; }
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d)); ) {
        if (e->d_name[0] == '.') continue;
        if (j->seen++ > 50000 || j->nhits >= 500 || mono_now() > j->end) { j->complete = 0; break; }
        char p[PATH_MAX * 2];
        if ((size_t)cl_pf(p, sizeof p, "%s/%s", dir, e->d_name) >= sizeof p) continue;
        struct stat st;
        if (lstat(p, &st)) continue;
        if (ci_has(e->d_name, j->needle)) {
            jb_raw(&j->hits, j->nhits++ ? ",{\"path\":" : "{\"path\":");
            jb_str(&j->hits, p);
            jb_printf(&j->hits, ",\"dir\":%s,\"size\":%lld,\"mtime\":%lld}", S_ISDIR(st.st_mode) ? "true" : "false", (long long)st.st_size, (long long)st.st_mtime * 1000);
        }
        if (S_ISDIR(st.st_mode)) search_walk(j, p, depth + 1);
    }
    closedir(d);
}

static void *search_thread(void *arg) {
    SearchJob *j = arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    jb_init(&j->hits);
    j->complete = 1;
    search_walk(j, j->root, 0);
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"hits\":[");
    if (!j->hits.oom && j->hits.n) jb_rawn(&b, j->hits.p, j->hits.n);
    jb_printf(&b, "],\"complete\":%s}", j->complete ? "true" : "false");
    jb_free(&j->hits);
    cl_reply(j->req, b.oom ? (jb_free(&b), NULL) : b.p);
    free(j->root);
    free(j->needle);
    free(j);
    return NULL;
}

char *cloud_search(const char *path, const char *needle, int *req) {
    char name[CLOUD_NAME_MAX + 1], rel[CLOUD_REL_MAX + 1];
    *req = 0;
    if (!needle || !needle[0] || strlen(needle) > 200) return bad_path("cloud.badArgument", "");
    if (!path || cloud_split(cl_root, path, name, sizeof name, rel, sizeof rel)) return bad_path("cloud.badPath", path);
    pthread_mutex_lock(&cl_mu);
    Acc *a = cl_find_name(name);
    int ok = a && a->mounted;
    const char *why = !a ? "cloud.notFound" : !cl_running ? "cloud.notRunning" : a->locked ? "cloud.keyringLocked" : "cloud.notConnected";
    pthread_mutex_unlock(&cl_mu);
    if (!ok) return bad_path(why, path);
    SearchJob *j = calloc(1, sizeof *j);
    if (!j) return bad_path("cloud.failed", "");
    j->root = strdup(path);
    j->needle = strdup(needle);
    j->end = mono_now() + 15000;
    j->depth_max = 16;
    j->req = *req = cl_next_req();
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 2 * 1024 * 1024);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    int r = pthread_create(&th, &at, search_thread, j);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&at);
    if (r) { free(j->root); free(j->needle); free(j); *req = 0; return bad_path("cloud.failed", ""); }
    return NULL;
}
