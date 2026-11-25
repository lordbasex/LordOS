/* facts.c — what Sentinela knows about a process's origin (docs/SENTINELA.md, "What it reads"): its executable, whether
 * it was deleted or replaced after it started, the package that installed it (dpkg's own lists, read-only), where it
 * lives, and who started it (its parents). Only metadata: never a file's contents, the command line's arguments, the
 * environment or anything of a window. Runs in the engine's thread (engine.c), never on the screen's. */
#include "engine.h"
#include "../sec/json.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- the package of a file ---- */

#define OWN_CACHE 64
static struct { char path[PATH_MAX]; char pkg[128]; int found; long long mt; } cache[OWN_CACHE];
static int cache_next;
static pthread_mutex_t cache_mu = PTHREAD_MUTEX_INITIALIZER;

static long long info_mtime(const char *info) {
    struct stat st;
    return stat(info, &st) ? -1 : st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
}

/* one pass over the lists: the package whose list has exactly one of the two paths */
static int scan(const char *info, const char *a, const char *b, char *out, size_t n) {
    DIR *d = opendir(info);
    if (!d) return -1;
    struct dirent *de;
    int found = -1, lists = 0;
    char line[4096];
    while (found < 0 && (de = readdir(d)) && lists < 50000) {
        size_t l = strlen(de->d_name);
        if (l < 6 || l - 5 >= 128 || strcmp(de->d_name + l - 5, ".list")) continue;
        char file[PATH_MAX];
        if (snprintf(file, sizeof file, "%s/%s", info, de->d_name) >= (int)sizeof file) continue;
        lists++;
        FILE *f = fopen(file, "re");
        if (!f) continue;
        while (fgets(line, sizeof line, f)) {
            size_t k = strlen(line);
            if (k && line[k - 1] == '\n') line[--k] = 0;
            if (strcmp(line, a) && (!b || strcmp(line, b))) continue;
            char pkg[128];
            snprintf(pkg, sizeof pkg, "%.*s", (int)(l - 5), de->d_name);
            char *colon = strchr(pkg, ':');                  /* "libfoo:amd64": the name without its architecture */
            if (colon) *colon = 0;
            if (strlen(pkg) < n) { snprintf(out, n, "%s", pkg); found = 0; }
            break;
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

int llm_pkg_owner(const char *info, const char *path, char *out, size_t n) {
    if (!info || !path || path[0] != '/' || strlen(path) >= PATH_MAX || !out || !n) return -1;
    long long mt = info_mtime(info);
    if (mt < 0) return -1;
    pthread_mutex_lock(&cache_mu);
    for (int i = 0; i < OWN_CACHE; i++) {
        if (cache[i].mt != mt || strcmp(cache[i].path, path)) continue;
        int r = cache[i].found ? 0 : -1;
        if (!r) snprintf(out, n, "%s", cache[i].pkg);
        pthread_mutex_unlock(&cache_mu);
        return r;
    }
    pthread_mutex_unlock(&cache_mu);
    /* the /usr merge: dpkg may have recorded /bin/x for what runs as /usr/bin/x, and the other way round */
    char twin[PATH_MAX] = "";
    static const char *const merged[] = { "/bin/", "/sbin/", "/lib/", "/lib64/", NULL };
    if (!strncmp(path, "/usr/", 5)) {
        for (const char *const *m = merged; *m; m++) if (!strncmp(path + 4, *m, strlen(*m))) snprintf(twin, sizeof twin, "%s", path + 4);
    } else {
        for (const char *const *m = merged; *m; m++) if (!strncmp(path, *m, strlen(*m))) snprintf(twin, sizeof twin, "/usr%s", path);
    }
    char pkg[128] = "";
    int r = scan(info, path, twin[0] ? twin : NULL, pkg, sizeof pkg);
    pthread_mutex_lock(&cache_mu);
    int i = cache_next++ % OWN_CACHE;
    snprintf(cache[i].path, sizeof cache[i].path, "%s", path);
    snprintf(cache[i].pkg, sizeof cache[i].pkg, "%s", pkg);
    cache[i].found = !r;
    cache[i].mt = mt;
    pthread_mutex_unlock(&cache_mu);
    if (!r && strlen(pkg) < n) { snprintf(out, n, "%s", pkg); return 0; }
    return -1;
}

const char *llm_exe_place(const char *exe, const char *home) {
    if (!exe || exe[0] != '/') return "none";
    static const char *const sys[] = { "/usr/", "/bin/", "/sbin/", "/lib/", "/lib64/", "/opt/", "/snap/", NULL };
    static const char *const tmp[] = { "/tmp/", "/var/tmp/", "/dev/shm/", "/run/user/", NULL };
    for (const char *const *p = tmp; *p; p++) if (!strncmp(exe, *p, strlen(*p))) return "temp";
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && !strncmp(exe, home, hl) && exe[hl] == '/') return "home";
    if (!strncmp(exe, "/home/", 6) || !strncmp(exe, "/root/", 6)) return "home";
    for (const char *const *p = sys; *p; p++) if (!strncmp(exe, *p, strlen(*p))) return "system";
    return "other";
}

/* ---- one process ---- */

#define PID_MAX 4194304
typedef struct { int ppid; unsigned long long start; char comm[64]; } PStat;

/* /proc/<pid>/stat: the name between the first '(' and the last ')', the parent (field 4), the start time (field 22) */
static int pstat(int pid, PStat *s) {
    char p[64], t[1024];
    snprintf(p, sizeof p, "/proc/%d/stat", pid);
    FILE *f = fopen(p, "re");
    if (!f) return -1;
    size_t n = fread(t, 1, sizeof t - 1, f);
    fclose(f);
    t[n] = 0;
    char *l = strchr(t, '('), *r = strrchr(t, ')');
    if (!l || !r || r < l || r[1] != ' ') return -1;
    snprintf(s->comm, sizeof s->comm, "%.*s", (int)(r - l - 1 < 63 ? r - l - 1 : 63), l + 1);
    char *q = r + 2;
    unsigned long long v[20];
    if (!*q) return -1;
    q++;                                            /* the state */
    for (int i = 0; i < 19; i++) v[i] = strtoull(q, &q, 10);
    s->ppid = (int)v[0];
    s->start = v[18];
    return 0;
}

typedef struct { char *p; size_t n, cap; int bad; } Out;

static void put(Out *o, const char *s, size_t n) {
    if (o->bad) return;
    if (o->n + n + 1 > o->cap) {
        size_t c = o->cap ? o->cap * 2 : 1024;
        while (c < o->n + n + 1) c *= 2;
        char *q = realloc(o->p, c);
        if (!q) { o->bad = 1; return; }
        o->p = q;
        o->cap = c;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = 0;
}
static void puts_(Out *o, const char *s) { put(o, s, strlen(s)); }
static void putq(Out *o, const char *s) {
    char q[PATH_MAX * 2 + 8];
    size_t k = json_quote(q, sizeof q, s ? s : "");
    put(o, q, k < sizeof q ? k : strlen(q));
}
static void putn(Out *o, long long v) { char b[32]; snprintf(b, sizeof b, "%lld", v); puts_(o, b); }

/* readlink /proc/<pid>/exe: the path and whether it was deleted (or replaced: dpkg renames over it) */
static int exe_of(int pid, char *exe, size_t n, int *deleted) {
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/exe", pid);
    ssize_t k = readlink(p, exe, n - 1);
    if (k <= 0) { exe[0] = 0; *deleted = 0; return -1; }
    exe[k] = 0;
    const char *suf = " (deleted)";
    size_t sl = strlen(suf);
    *deleted = (size_t)k > sl && !strcmp(exe + k - sl, suf);
    if (*deleted) exe[k - sl] = 0;
    return 0;
}

char *llm_facts_json(int pid, unsigned long long start, const char *info, const char *home) {
    Out o = { 0 };
    PStat s;
    if (pid <= 0 || pid > PID_MAX || pstat(pid, &s) || s.start != start) {
        puts_(&o, "{\"ok\":false,\"error\":\"proc.gone\"}");
        return o.bad ? NULL : o.p;
    }
    char exe[PATH_MAX], pkg[128];
    int deleted = 0;
    exe_of(pid, exe, sizeof exe, &deleted);
    puts_(&o, "{\"ok\":true,\"pid\":");
    putn(&o, pid);
    puts_(&o, ",\"exe\":");
    putq(&o, exe);
    puts_(&o, ",\"deleted\":");
    puts_(&o, deleted ? "true" : "false");
    int own = exe[0] && !llm_pkg_owner(info, exe, pkg, sizeof pkg);
    puts_(&o, ",\"pkg\":");
    if (own) putq(&o, pkg); else puts_(&o, "null");
    puts_(&o, ",\"place\":");
    putq(&o, llm_exe_place(exe, home));
    struct stat st;
    /* the file now at that path (after a deletion, a later file of the same name, if any) */
    if (exe[0] && !stat(exe, &st)) {
        puts_(&o, ",\"exeUid\":");
        putn(&o, (long long)st.st_uid);
        puts_(&o, ",\"exeMtime\":");
        putn(&o, (long long)st.st_mtim.tv_sec);
        puts_(&o, ",\"exeWritable\":");
        puts_(&o, access(exe, W_OK) == 0 ? "true" : "false");
    }
    /* who started it: up to six parents, stopping at PID 1 */
    puts_(&o, ",\"chain\":[");
    int p = s.ppid, first = 1;
    for (int i = 0; i < 6 && p > 1; i++) {
        PStat ps;
        if (pstat(p, &ps)) break;
        char pe[PATH_MAX], pp[128];
        int pd = 0;
        exe_of(p, pe, sizeof pe, &pd);
        int pown = pe[0] && !llm_pkg_owner(info, pe, pp, sizeof pp);
        if (!first) puts_(&o, ",");
        first = 0;
        puts_(&o, "{\"pid\":");
        putn(&o, p);
        puts_(&o, ",\"name\":");
        putq(&o, ps.comm);
        puts_(&o, ",\"exe\":");
        putq(&o, pe);
        puts_(&o, ",\"pkg\":");
        if (pown) putq(&o, pp); else puts_(&o, "null");
        puts_(&o, "}");
        p = ps.ppid;
    }
    puts_(&o, "]}");
    if (o.bad) { free(o.p); return NULL; }
    return o.p;
}
