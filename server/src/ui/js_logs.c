/* js_logs.c — lord.logs: logs for the Logs app (docs/SYSTEM.md).
 *
 * Everything is read-only and runs as the session user: the journal files that the kernel and
 * journald's ACLs let them read (their user-<uid>.journal; the system one only if they are in
 * systemd-journal or adm) and their LordOS logs (lordlog.h: /var/log/lordos/users/<user>, or
 * ~/.cache/lordos without lord-sessiond). The system journal for an administrator goes through the
 * helper's logs-system action (logs/logs_admin.c).
 *
 *   sources()                  { user: n files, system: bool (readable directly), uid }
 *   query({ scope, since, until, prio, kernel, match, grep, limit })  → id (>0)
 *                              a thread of its own does the reading; when done,
 *                              __lordLogs('result', id) arrives; fetch the result with take(id)
 *   take(id)                   [{ t, s, p, pid, uid, k, id, u, b, m }] (t in µs) or null
 *   dir()                      { dir, system }: the user's log folder, and whether it is under /var/log/lordos
 *   files()                    [{ name, size, mtime }]: "<name>.log" and "<name>.log.prev" (the previous
 *                              session) in it, "apps/<app>.log", "web/<app>-<profile>.log", and when it is
 *                              /var/log/lordos the logs left in ~/.cache/lordos by older versions as "cache/<name>.log"
 *   tail(name, bytes)          the end of one of those files (max. 1 MB), from a line start
 *   follow(on)                 inotify on the journal folders and those folders:
 *                              __lordLogs('journal') or __lordLogs('file', name) on change
 *
 * The thread and inotify go through a single epoll fd that the main loop watches (logs_fd()), so
 * the compositor never waits on a read and there is no polling. */
#include "js_logs.h"
#include "js_err.h"
#include "../logs/journal.h"
#include "../lordlog.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define QMAX 8

typedef struct { int id; int scope; JQuery q; } Req;
typedef struct { int id; char *json; } Res;

static int ep = -1, wake_r = -1, wake_w = -1, ino = -1;
static int next_id = 1;
static pthread_t th;
static int th_started;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static Req queue[QMAX];
static int nq;
static Res done[QMAX];
static int jdirs[2] = { -1, -1 };

/* the folders of LordOS logs: "" (the log folder itself), "apps", "web", and "cache" (the old place) */
#define NSUB 4
static const char *const subs[NSUB] = { "", "apps", "web", "cache" };
static int subwd[NSUB] = { -1, -1, -1, -1 };

int logs_fd(void) { return ep; }

static const char *home(void) { const char *h = getenv("HOME"); return h && *h == '/' ? h : NULL; }

/* The folder of subs[i]; 0, or -1 if there is none (the old place only when the logs moved). */
static int sub_dir(int i, char *out, size_t n) {
    const char *d = lordlog_dir();
    if (!*d) return -1;
    if (i == 3) return lordlog_dir_is_system() && home() && (size_t)snprintf(out, n, "%s/.cache/lordos", home()) < n ? 0 : -1;
    return (size_t)snprintf(out, n, i ? "%s/%s" : "%s", d, subs[i]) < n ? 0 : -1;
}

/* a log file's own name: <safe name>.log or <safe name>.log.prev */
static int file_ok(const char *n) {
    size_t l = strlen(n);
    if (!lordlog_name_ok(n) || l < 5) return 0;
    return !strcmp(n + l - 4, ".log") || (l >= 10 && !strcmp(n + l - 9, ".log.prev"));
}

/* "name", "apps/name", "web/name" or "cache/name" → its folder's index and the file's name; -1 if not one */
static int split_name(const char *n, const char **file) {
    const char *slash = strchr(n, '/');
    int i = 0;
    if (slash) {
        for (i = 1; i < NSUB; i++) if ((size_t)(slash - n) == strlen(subs[i]) && !strncmp(n, subs[i], (size_t)(slash - n))) break;
        if (i == NSUB) return -1;
        n = slash + 1;
    }
    if (!file_ok(n)) return -1;
    *file = n;
    return i;
}

/* ---- reader thread ---- */

static char *run_query(int scope, const JQuery *q) {
    char **paths;
    int np = jq_files(scope, getuid(), &paths);
    JEntry *e = NULL;
    long n = np > 0 ? jq_run(paths, np, q, &e) : 0;
    jq_files_free(paths, np);
    if (n < 0) n = 0;
    size_t cap = 64 + (size_t)n * 512, len = 1;
    char *out = malloc(cap);
    if (!out) { jq_free(e, n); return NULL; }
    out[0] = '[';
    char *buf = malloc(JMSG_MAX * 6 + 512);
    for (long i = 0; buf && i < n; i++) {
        size_t k = jentry_json(&e[i], buf, JMSG_MAX * 6 + 512, JMSG_MAX + 8);
        if (!k) continue;
        if (len + k + 4 > cap) {
            size_t nc = (len + k + 4) * 2;
            char *no = realloc(out, nc);
            if (!no) break;
            out = no;
            cap = nc;
        }
        if (len > 1) out[len++] = ',';
        memcpy(out + len, buf, k);
        len += k;
    }
    free(buf);
    out[len++] = ']';
    out[len] = 0;
    jq_free(e, n);
    return out;
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!nq) pthread_cond_wait(&cv, &mu);
        Req r = queue[0];
        memmove(queue, queue + 1, (size_t)(nq - 1) * sizeof *queue);
        nq--;
        pthread_mutex_unlock(&mu);
        char *json = run_query(r.scope, &r.q);
        pthread_mutex_lock(&mu);
        int slot = -1;
        for (int i = 0; i < QMAX; i++) if (!done[i].id) { slot = i; break; }
        if (slot < 0) { free(done[0].json); slot = 0; }     /* nobody took it: the oldest is dropped */
        done[slot] = (Res){ r.id, json };
        pthread_mutex_unlock(&mu);
        char c = 1;
        if (write(wake_w, &c, 1) < 0) { /* the loop already has something to read */ }
    }
    return NULL;
}

/* ---- inotify ---- */

static void unfollow(void) {
    if (ino < 0) return;
    epoll_ctl(ep, EPOLL_CTL_DEL, ino, NULL);
    close(ino);
    ino = -1;
    jdirs[0] = jdirs[1] = -1;
    for (int i = 0; i < NSUB; i++) subwd[i] = -1;
}

static int follow(void) {
    if (ino >= 0) return 0;
    ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino < 0) return -1;
    char mid[64] = "", p[512];
    FILE *m = fopen("/etc/machine-id", "r");
    if (m) { if (!fgets(mid, sizeof mid, m)) mid[0] = 0; fclose(m); }
    mid[strcspn(mid, "\n")] = 0;
    int ok = strlen(mid) == 32;
    for (const char *c = mid; ok && *c; c++) ok = isxdigit((unsigned char)*c);
    if (ok) {
        snprintf(p, sizeof p, "/var/log/journal/%s", mid);
        jdirs[0] = inotify_add_watch(ino, p, IN_MODIFY | IN_CREATE | IN_MOVED_TO);
        snprintf(p, sizeof p, "/run/log/journal/%s", mid);
        jdirs[1] = inotify_add_watch(ino, p, IN_MODIFY | IN_CREATE | IN_MOVED_TO);
    }
    for (int i = 0; i < NSUB; i++) {
        if (sub_dir(i, p, sizeof p)) continue;
        if (i == 1 || i == 2) mkdir(p, 0700);          /* so a first app's log is seen too */
        subwd[i] = inotify_add_watch(ino, p, IN_MODIFY | IN_CREATE | IN_MOVED_TO | IN_DONT_FOLLOW | IN_ONLYDIR);
    }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = ino };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, ino, &ev)) { close(ino); ino = -1; return -1; }
    return 0;
}


void logs_dispatch(void (*fn)(const char *kind, int id, const char *name)) {
    struct epoll_event evs[4];
    int ne = epoll_wait(ep, evs, 4, 0);
    for (int i = 0; i < ne; i++) {
        if (evs[i].data.fd == wake_r) {
            char junk[64];
            while (read(wake_r, junk, sizeof junk) > 0) {}
            int ids[QMAX], k = 0;
            pthread_mutex_lock(&mu);
            for (int j = 0; j < QMAX; j++) if (done[j].id) ids[k++] = done[j].id;
            pthread_mutex_unlock(&mu);
            for (int j = 0; j < k; j++) fn("result", ids[j], "");
        } else if (ino >= 0 && evs[i].data.fd == ino) {
            /* coalesce all events of this round: one per kind (and per LordOS file) */
            char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
            int journal = 0;
            char files[4][LORDLOG_NAME_MAX + 8];
            int nf = 0;
            ssize_t r;
            while ((r = read(ino, buf, sizeof buf)) > 0) {
                for (char *p = buf; p < buf + r;) {
                    struct inotify_event *e = (struct inotify_event *)p;
                    if (e->wd == jdirs[0] || e->wd == jdirs[1]) {
                        if (e->len && strstr(e->name, ".journal")) journal = 1;
                    } else if (e->len && file_ok(e->name)) {
                        int s = -1;
                        for (int j = 0; j < NSUB; j++) if (subwd[j] >= 0 && e->wd == subwd[j]) s = j;
                        char name[LORDLOG_NAME_MAX + 8];
                        if (s < 0 || (size_t)snprintf(name, sizeof name, "%s%s%s", subs[s], s ? "/" : "", e->name) >= sizeof name) {
                            p += sizeof *e + e->len;
                            continue;
                        }
                        int seen = 0;
                        for (int j = 0; j < nf; j++) if (!strcmp(files[j], name)) seen = 1;
                        if (!seen && nf < 4) snprintf(files[nf++], sizeof files[0], "%s", name);
                    }
                    p += sizeof *e + e->len;
                }
            }
            if (journal) fn("journal", 0, "");
            for (int j = 0; j < nf; j++) fn("file", 0, files[j]);
        }
    }
}

/* ---- JS ---- */

static int get_u64(JSContext *c, JSValueConst o, const char *k, uint64_t *out) {
    JSValue v = JS_GetPropertyStr(c, o, k);
    double d = 0;
    int r = 0;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) r = JS_ToFloat64(c, &d, v) == 0 && d >= 0 && d < 1.8e19 ? 1 : -1;
    JS_FreeValue(c, v);
    if (r == 1) *out = (uint64_t)d;
    return r;
}

static int get_str(JSContext *c, JSValueConst o, const char *k, char *out, size_t n) {
    out[0] = 0;
    JSValue v = JS_GetPropertyStr(c, o, k);
    int r = 0;
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        if (s && strlen(s) < n) { snprintf(out, n, "%s", s); r = 1; } else r = -1;
        JS_FreeCString(c, s);
    }
    JS_FreeValue(c, v);
    return r;
}

static JSValue f_query(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsObject(argv[0])) return js_throw_code(c, "logs.badQuery", NULL);
    JSValueConst o = argv[0];
    Req r = { 0 };
    char scope[16], tmp[160];
    get_str(c, o, "scope", scope, sizeof scope);
    r.scope = !strcmp(scope, "system") ? JSCOPE_SYSTEM : JSCOPE_USER;
    /* the query is built as text and goes through the same validator as the helper's action */
    char text[600];
    size_t n = 0;
    uint64_t u;
    if (get_u64(c, o, "since", &u) == 1) n += (size_t)snprintf(text + n, sizeof text - n, "since=%llu\n", (unsigned long long)u);
    if (get_u64(c, o, "until", &u) == 1) n += (size_t)snprintf(text + n, sizeof text - n, "until=%llu\n", (unsigned long long)u);
    if (get_u64(c, o, "prio", &u) == 1 && u <= 7) n += (size_t)snprintf(text + n, sizeof text - n, "prio=%llu\n", (unsigned long long)u);
    if (get_u64(c, o, "limit", &u) == 1) n += (size_t)snprintf(text + n, sizeof text - n, "limit=%llu\n", (unsigned long long)u);
    JSValue k = JS_GetPropertyStr(c, o, "kernel");
    if (JS_ToBool(c, k) > 0) n += (size_t)snprintf(text + n, sizeof text - n, "kernel=1\n");
    JS_FreeValue(c, k);
    if (get_str(c, o, "match", tmp, 97) == 1 && tmp[0]) n += (size_t)snprintf(text + n, sizeof text - n, "match=%s\n", tmp);
    if (get_str(c, o, "grep", tmp, 128) == 1 && tmp[0] && !strchr(tmp, '\n')) n += (size_t)snprintf(text + n, sizeof text - n, "grep=%s\n", tmp);
    if (n >= sizeof text || jq_parse(text, &r.q, 20000)) return js_throw_code(c, "logs.badQuery", NULL);
    pthread_mutex_lock(&mu);
    if (nq == QMAX) { pthread_mutex_unlock(&mu); return js_throw_code(c, "logs.tooManyQueries", NULL); }
    if (!th_started) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        th_started = pthread_create(&th, &a, worker, NULL) == 0;
        pthread_attr_destroy(&a);
        if (!th_started) { pthread_mutex_unlock(&mu); return js_throw_code(c, "logs.noThread", NULL); }
    }
    r.id = next_id++;
    if (next_id > 1 << 30) next_id = 1;
    queue[nq++] = r;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    return JS_NewInt32(c, r.id);
}

static JSValue f_take(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int id = 0;
    if (argc < 1 || JS_ToInt32(c, &id, argv[0])) return JS_NULL;
    char *json = NULL;
    int found = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < QMAX; i++) if (done[i].id == id) { json = done[i].json; done[i] = (Res){ 0 }; found = 1; break; }
    pthread_mutex_unlock(&mu);
    if (!found) return JS_NULL;
    if (!json) return JS_NewArray(c);
    JSValue v = JS_ParseJSON(c, json, strlen(json), "<logs>");
    free(json);
    return v;
}

static JSValue f_sources(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char **p;
    int nu = jq_files(JSCOPE_USER, getuid(), &p);
    jq_files_free(p, nu);
    int ns = jq_files(JSCOPE_SYSTEM, 0, &p);
    jq_files_free(p, ns);
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "user", JS_NewInt32(c, nu));
    JS_SetPropertyStr(c, o, "system", JS_NewBool(c, ns > 0));
    JS_SetPropertyStr(c, o, "uid", JS_NewInt32(c, (int)getuid()));
    return o;
}

static JSValue f_files(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue arr = JS_NewArray(c);
    uint32_t n = 0;
    for (int i = 0; i < NSUB && n < 256; i++) {
        char dir[PATH_MAX];
        if (sub_dir(i, dir, sizeof dir)) continue;
        int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        DIR *d = dfd >= 0 ? fdopendir(dfd) : NULL;
        if (!d) { if (dfd >= 0) close(dfd); continue; }
        struct dirent *de;
        while ((de = readdir(d)) && n < 256) {
            if (!file_ok(de->d_name)) continue;
            struct stat st;
            if (fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) continue;
            char name[LORDLOG_NAME_MAX + 8];
            if ((size_t)snprintf(name, sizeof name, "%s%s%s", subs[i], i ? "/" : "", de->d_name) >= sizeof name) continue;
            JSValue o = JS_NewObject(c);
            JS_SetPropertyStr(c, o, "name", JS_NewString(c, name));
            JS_SetPropertyStr(c, o, "size", JS_NewFloat64(c, (double)st.st_size));
            JS_SetPropertyStr(c, o, "mtime", JS_NewFloat64(c, (double)st.st_mtime * 1000.0));
            JS_SetPropertyUint32(c, arr, n++, o);
        }
        closedir(d);
    }
    return arr;
}

static JSValue f_dir(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "dir", JS_NewString(c, lordlog_dir()));
    JS_SetPropertyStr(c, o, "system", JS_NewBool(c, lordlog_dir_is_system()));
    return o;
}

static JSValue f_tail(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsString(argv[0])) return JS_NULL;
    const char *name = JS_ToCString(c, argv[0]);
    if (!name) return JS_EXCEPTION;
    const char *file = NULL;
    char dir[PATH_MAX], fname[LORDLOG_NAME_MAX + 8];
    int i = split_name(name, &file);
    if (i >= 0) snprintf(fname, sizeof fname, "%s", file);
    JS_FreeCString(c, name);
    if (i < 0) return js_throw_code(c, "logs.badName", NULL);
    if (sub_dir(i, dir, sizeof dir)) return JS_NULL;
    double want = 262144;
    if (argc > 1) JS_ToFloat64(c, &want, argv[1]);
    size_t max = want < 1024 ? 1024 : want > 1048576 ? 1048576 : (size_t)want;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int fd = dfd >= 0 ? openat(dfd, fname, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) : -1;
    if (dfd >= 0) close(dfd);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) { if (fd >= 0) close(fd); return JS_NULL; }
    off_t from = st.st_size > (off_t)max ? st.st_size - (off_t)max : 0;
    char *buf = malloc(max + 1);
    ssize_t r = buf ? pread(fd, buf, max, from) : -1;
    close(fd);
    if (r < 0) { free(buf); return JS_NULL; }
    size_t start = 0;
    if (from > 0) { char *nl = memchr(buf, '\n', (size_t)r); start = nl ? (size_t)(nl - buf) + 1 : (size_t)r; }
    JSValue s = JS_NewStringLen(c, buf + start, (size_t)r - start);
    free(buf);
    return s;
}

static JSValue f_follow(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int on = argc > 0 && JS_ToBool(c, argv[0]) > 0;
    if (!on) { unfollow(); return JS_FALSE; }
    return JS_NewBool(c, follow() == 0);
}

static const JSCFunctionListEntry logs_funcs[] = {
    JS_CFUNC_DEF("sources", 0, f_sources),
    JS_CFUNC_DEF("query", 1, f_query),
    JS_CFUNC_DEF("take", 1, f_take),
    JS_CFUNC_DEF("files", 0, f_files),
    JS_CFUNC_DEF("dir", 0, f_dir),
    JS_CFUNC_DEF("tail", 2, f_tail),
    JS_CFUNC_DEF("follow", 1, f_follow),
};

void js_logs_register(JSContext *c, JSValue lord) {
    if (ep < 0) {
        int p[2];
        ep = epoll_create1(EPOLL_CLOEXEC);
        if (ep >= 0 && !pipe2(p, O_CLOEXEC | O_NONBLOCK)) {
            wake_r = p[0];
            wake_w = p[1];
            struct epoll_event ev = { .events = EPOLLIN, .data.fd = wake_r };
            epoll_ctl(ep, EPOLL_CTL_ADD, wake_r, &ev);
        }
    }
    unfollow();                         /* new context (theme change): nobody is watching anymore */
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, logs_funcs, sizeof logs_funcs / sizeof logs_funcs[0]);
    JS_SetPropertyStr(c, lord, "logs", o);
}
