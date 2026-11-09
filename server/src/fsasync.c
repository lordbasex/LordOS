/* fsasync.c — lord.fs.listAsync / statAsync / trashAsync / freeAsync / mkdirAsync / renameAsync: the calls Files makes on a folder that may be
 * slow to answer (a Cloud account mounted by rclone, a Connect to Server or SMB/NFS share), run on worker threads.
 *
 * lord-server is one thread for input, layout, painting and JS. A readdir, stat or rename on a FUSE mount waits for the
 * daemon behind it (rclone asks the provider; while it deletes or lists a folder the kernel also holds that folder's
 * lock, so every other lookup in it waits too): on the interface's thread that froze the whole desktop (0.8.8.7 fix).
 * Here each request is resolved, checked and run on a worker; the answer comes back through an eventfd the main loop
 * polls, and lord-server calls __lordFsReply(req, answer).
 *
 * Security: the request carries the path exactly as JS gave it. The worker resolves it with the same fsops.c functions
 * the synchronous lord.fs calls use (fsops_resolve_readable / _target / _stat: the roots, the pseudo-filesystems, the
 * protected folders, a share that stopped answering), then fstrash_put adds its own checks (fsops_protected_below).
 * Nothing new is reachable: the operations are a closed list (list, stat, free, and three writes lord.fs already had:
 * move to the Trash, new folder, rename), each the synchronous call's own checks and code on another thread. */
#include "fsasync.h"
#include "fsops.h"
#include "errcode.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifdef FSASYNC_TEST
void (*fsasync_test_hook)(int op, const char *path);
#endif

typedef struct Req { int req, op; char *path, *path2, *json; struct Req *next; } Req;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static Req *todo, *todo_tail, *done, *done_tail;
static int nthreads, nidle, ntodo, inflight, next_req = 1, efd = -1;

/* ---- a small JSON writer (strings as valid UTF-8: a byte that isn't becomes U+FFFD) ---- */

typedef struct { char *p; size_t n, cap; int oom; } Jw;

static void jw_raw(Jw *w, const char *s, size_t l) {
    if (w->oom) return;
    if (w->n + l + 1 > w->cap) {
        size_t cap = w->cap ? w->cap : 4096;
        while (w->n + l + 1 > cap) cap *= 2;
        char *q = realloc(w->p, cap);
        if (!q) { w->oom = 1; return; }
        w->p = q;
        w->cap = cap;
    }
    memcpy(w->p + w->n, s, l);
    w->n += l;
    w->p[w->n] = 0;
}
static void jw_s(Jw *w, const char *s) { jw_raw(w, s, strlen(s)); }

static size_t utf8_len(const unsigned char *s) {
    unsigned c = s[0];
    size_t n = c >= 0xf0 && c <= 0xf4 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 && c < 0xe0 ? 2 : 0;
    if (!n || c > 0xf4) return 0;
    for (size_t i = 1; i < n; i++) if ((s[i] & 0xc0) != 0x80) return 0;
    if (n == 3 && ((c == 0xe0 && s[1] < 0xa0) || (c == 0xed && s[1] >= 0xa0))) return 0;     /* overlong, surrogates */
    if (n == 4 && ((c == 0xf0 && s[1] < 0x90) || (c == 0xf4 && s[1] >= 0x90))) return 0;
    return n;
}

static void jw_str(Jw *w, const char *s) {
    jw_raw(w, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        if (*p == '"' || *p == '\\') { char e[2] = { '\\', (char)*p }; jw_raw(w, e, 2); p++; }
        else if (*p < 0x20) { char e[8]; snprintf(e, sizeof e, "\\u%04x", *p); jw_s(w, e); p++; }
        else if (*p < 0x80) { jw_raw(w, (const char *)p, 1); p++; }
        else {
            size_t l = utf8_len(p);
            if (l) { jw_raw(w, (const char *)p, l); p += l; }
            else { jw_s(w, "\xef\xbf\xbd"); p++; }
        }
    }
    jw_raw(w, "\"", 1);
}

static void jw_num(Jw *w, double v) { char b[40]; snprintf(b, sizeof b, "%.17g", v); jw_s(w, b); }
static void jw_key(Jw *w, const char *k) { jw_str(w, k); jw_raw(w, ":", 1); }

static char *error_json(const char *line) {
    Jw w = { 0 };
    jw_s(&w, "{\"error\":");
    jw_str(&w, line);
    jw_s(&w, "}");
    if (w.oom) { free(w.p); return NULL; }
    return w.p;
}

static char *errno_json(int e) { char l[128]; lerr_set(l, sizeof l, "sys.errno", "errno", lerr_errno(e), NULL); return error_json(l); }

/* the same answers lord.fs's synchronous calls throw (js_sys.c fs_path_error) */
static char *path_error(int r, int down) { return error_json(r == -2 ? "fs.protected" : down ? "share.notResponding" : "fs.outsideHome"); }

/* ---- the operations ---- */

typedef struct { Jw *w; int n; uid_t uid; gid_t gid; char user[64], group[64]; } ListAcc;

static void name_cached(ListAcc *a, const FsEntry *e) {
    /* one listing is nearly always one owner: the names are looked up when it changes */
    if (!a->user[0] || e->uid != a->uid) { a->uid = e->uid; fsops_user_name(e->uid, a->user, sizeof a->user); }
    if (!a->group[0] || e->gid != a->gid) { a->gid = e->gid; fsops_group_name(e->gid, a->group, sizeof a->group); }
}

static void list_cb(void *ud, const FsEntry *e) {
    ListAcc *a = ud;
    Jw *w = a->w;
    name_cached(a, e);
    jw_s(w, a->n++ ? ",{" : "{");
    jw_key(w, "name"); jw_str(w, e->name);
    jw_s(w, e->link ? ",\"link\":true" : ",\"link\":false");
    jw_s(w, e->dir ? ",\"dir\":true" : ",\"dir\":false");
    if (e->locked) jw_s(w, ",\"locked\":true");
    jw_s(w, ",\"size\":"); jw_num(w, e->size);
    jw_s(w, ",\"mtime\":"); jw_num(w, e->mtime);
    /* what the list view's extra columns ask lord.fs.stat for, so they don't ask the slow file system again */
    char mode[8];
    snprintf(mode, sizeof mode, "%04o", e->mode & 07777);
    jw_s(w, ",\"mode\":"); jw_str(w, mode);
    jw_s(w, ",\"user\":"); jw_str(w, a->user);
    jw_s(w, ",\"group\":"); jw_str(w, a->group);
    jw_s(w, ",\"atime\":"); jw_num(w, e->atime);
    jw_s(w, ",\"ctime\":"); jw_num(w, e->ctime);
    jw_s(w, "}");
}

static char *op_list(const char *path) {
    char dir[PATH_MAX];
    int down = 0, r = fsops_resolve_readable(path, dir, &down);
    if (r == -2) return error_json("fs.systemFolder");
    if (r) return error_json(down ? "share.notResponding" : "fs.outsideHome");
    Jw w = { 0 };
    jw_s(&w, "{\"path\":");
    jw_str(&w, dir);
    jw_s(&w, ",\"entries\":[");
    ListAcc a = { &w, 0, 0, 0, "", "" };
    if (fsops_list(dir, FSA_LIST_MAX, list_cb, &a) < 0) { int e = errno; free(w.p); return errno_json(e); }
    jw_s(&w, "]}");
    if (w.oom) { free(w.p); return error_json("sys.outOfMemory"); }
    return w.p;
}

static char *op_stat(const char *path) {
    char target[PATH_MAX + 300];
    int down = 0, r = fsops_resolve_stat(path, target, sizeof target, &down);
    if (r == -1) return path_error(r, down);
    FsStat *s = malloc(sizeof *s);
    if (!s) return error_json("sys.outOfMemory");
    if (fsops_stat(target, r == 0, s)) { int e = errno; free(s); return errno_json(e); }
    Jw w = { 0 };
    char mode[8];
    snprintf(mode, sizeof mode, "%04o", s->mode & 07777);
    jw_s(&w, "{\"stat\":{\"name\":"); jw_str(&w, s->name);
    jw_s(&w, ",\"path\":"); jw_str(&w, s->path);
    jw_s(&w, s->dir ? ",\"dir\":true" : ",\"dir\":false");
    jw_s(&w, s->link ? ",\"link\":true" : ",\"link\":false");
    if (s->has_target) { jw_s(&w, ",\"target\":"); jw_str(&w, s->target); }
    jw_s(&w, ",\"mode\":"); jw_str(&w, mode);
    jw_s(&w, ",\"size\":"); jw_num(&w, s->size);
    jw_s(&w, ",\"user\":"); jw_str(&w, s->user);
    jw_s(&w, ",\"group\":"); jw_str(&w, s->group);
    jw_s(&w, ",\"mtime\":"); jw_num(&w, s->mtime);
    jw_s(&w, ",\"atime\":"); jw_num(&w, s->atime);
    jw_s(&w, ",\"ctime\":"); jw_num(&w, s->ctime);
    if (s->has_btime) { jw_s(&w, ",\"btime\":"); jw_num(&w, s->btime); }
    jw_s(&w, s->writable ? ",\"writable\":true}}" : ",\"writable\":false}}");
    free(s);
    if (w.oom) { free(w.p); return error_json("sys.outOfMemory"); }
    return w.p;
}

static char *op_trash(const char *path) {
    char target[PATH_MAX + 300], err[256] = "";
    int down = 0, r = fsops_resolve_target(path, target, sizeof target, NULL, 0, &down);
    if (r) return path_error(r, down);
    if (fstrash_put(target, err, sizeof err)) return error_json(err[0] ? err : "fs.noTrash");
    return strdup("{\"ok\":true}");
}

static char *op_free(const char *path) {
    char real[PATH_MAX];
    int down = 0;
    if (fsops_resolve_readable(path, real, &down)) return path_error(-1, down);
    struct statvfs vs;
    if (statvfs(real, &vs)) return errno_json(errno);
    char b[128];
    snprintf(b, sizeof b, "{\"total\":%.17g,\"free\":%.17g}", (double)vs.f_blocks * vs.f_frsize, (double)vs.f_bavail * vs.f_frsize);
    return strdup(b);
}

/* as lord.fs.mkdir: a new folder (the parent must exist) */
static char *op_mkdir(const char *path) {
    char target[PATH_MAX + 300];
    int down = 0, r = fsops_resolve_target(path, target, sizeof target, NULL, 0, &down);
    if (r) return path_error(r, down);
    if (mkdir(target, 0755) && errno != EEXIST) return errno_json(errno);
    return strdup("{\"ok\":true}");
}

/* as lord.fs.rename: both names resolved like destinations, then fsops_rename's own checks */
static char *op_rename(const char *from, const char *to) {
    char a[PATH_MAX + 300], b[PATH_MAX + 300], err[256] = "";
    int down = 0, r = fsops_resolve_target(from, a, sizeof a, NULL, 0, &down);
    if (!r) r = fsops_resolve_target(to, b, sizeof b, NULL, 0, &down);
    if (r) return path_error(r, down);
    if (fsops_rename(a, b, err, sizeof err)) return error_json(err[0] ? err : "sys.unknown");
    return strdup("{\"ok\":true}");
}

static char *run(int op, const char *path, const char *path2) {
#ifdef FSASYNC_TEST
    if (fsasync_test_hook) fsasync_test_hook(op, path);
#endif
    switch (op) {
    case FSA_LIST: return op_list(path);
    case FSA_STAT: return op_stat(path);
    case FSA_TRASH: return op_trash(path);
    case FSA_FREE: return op_free(path);
    case FSA_MKDIR: return op_mkdir(path);
    case FSA_RENAME: return path2 ? op_rename(path, path2) : error_json("sys.badArgument");
    default: return error_json("sys.badArgument");
    }
}

/* ---- the workers ---- */

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!todo) { nidle++; pthread_cond_wait(&cv, &mu); nidle--; }
        Req *r = todo;
        todo = r->next;
        if (!todo) todo_tail = NULL;
        ntodo--;
        pthread_mutex_unlock(&mu);
        r->json = run(r->op, r->path, r->path2);
        r->next = NULL;
        pthread_mutex_lock(&mu);
        if (done_tail) done_tail->next = r; else done = r;
        done_tail = r;
        pthread_mutex_unlock(&mu);
        uint64_t one = 1;
        ssize_t w = write(efd, &one, sizeof one);
        (void)w;
    }
    return NULL;
}

static int spawn(void) {
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);        /* signals stay with the main thread */
    int e = pthread_create(&th, &at, worker, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&at);
    return e ? -1 : 0;
}

int fsasync_submit(int op, const char *path, char *err, size_t errlen) { return fsasync_submit2(op, path, NULL, err, errlen); }

int fsasync_submit2(int op, const char *path, const char *path2, char *err, size_t errlen) {
    if (op < FSA_LIST || op > FSA_RENAME || !path || !path[0] || strlen(path) >= PATH_MAX ||
        (op == FSA_RENAME) != (path2 != NULL) || (path2 && (!path2[0] || strlen(path2) >= PATH_MAX))) {
        lerr_set(err, errlen, "sys.badArgument", NULL);
        return -1;
    }
    pthread_mutex_lock(&mu);
    if (efd < 0) efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) { pthread_mutex_unlock(&mu); lerr_set(err, errlen, "sys.errno", "errno", lerr_errno(errno), NULL); return -1; }
    if (inflight >= FSA_QUEUE) { pthread_mutex_unlock(&mu); lerr_set(err, errlen, "fs.busy", NULL); return -1; }
    Req *r = calloc(1, sizeof *r);
    if (!r || !(r->path = strdup(path)) || (path2 && !(r->path2 = strdup(path2)))) {
        if (r) free(r->path);
        free(r);
        pthread_mutex_unlock(&mu);
        lerr_set(err, errlen, "sys.outOfMemory", NULL);
        return -1;
    }
    /* a new worker when none is free (one waiting on a server that stopped answering never holds the others up),
     * up to FSA_THREADS; past that the request waits its turn, still off the interface's thread */
    if (ntodo + 1 > nidle && nthreads < FSA_THREADS && !spawn()) nthreads++;
    if (!nthreads) { free(r->path); free(r->path2); free(r); pthread_mutex_unlock(&mu); lerr_set(err, errlen, "sys.errno", "errno", lerr_errno(EAGAIN), NULL); return -1; }
    r->op = op;
    r->req = next_req++;
    if (next_req <= 0) next_req = 1;
    if (todo_tail) todo_tail->next = r; else todo = r;
    todo_tail = r;
    ntodo++;
    inflight++;
    int id = r->req;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    return id;
}

int fsasync_fd(void) {
    pthread_mutex_lock(&mu);
    int fd = efd;
    pthread_mutex_unlock(&mu);
    return fd;
}

int fsasync_dispatch(FsaReplyFn fn, void *ud) {
    uint64_t v;
    int fd = fsasync_fd();
    if (fd >= 0) { ssize_t n = read(fd, &v, sizeof v); (void)n; }
    pthread_mutex_lock(&mu);
    Req *list = done;
    done = done_tail = NULL;
    pthread_mutex_unlock(&mu);
    int n = 0;
    while (list) {
        Req *r = list;
        list = r->next;
        fn(ud, r->req, r->json ? r->json : "{\"error\":\"sys.outOfMemory\"}");
        free(r->json);
        free(r->path);
        free(r->path2);
        free(r);
        n++;
        pthread_mutex_lock(&mu);
        inflight--;
        pthread_mutex_unlock(&mu);
    }
    return n;
}
