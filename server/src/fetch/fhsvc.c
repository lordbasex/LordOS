/* fhsvc.c — the Store's rankings in lord-server (see fhsvc.h). */
#include "fhsvc.h"
#include "fetchproto.h"
#include "fetchspawn.h"
#include "../doc/wire.h"
#include "../sec/json.h"
#include "../sec/netconn.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FILE_MAX (64 * 1024)
#define FAILS_MAX 30

typedef struct {
    int have;                   /* ids is a good copy */
    FhIds ids;
    long long at;               /* when ids came */
    long long tried;            /* the last attempt */
    int fails;                  /* failures in a row since the last good copy */
    int want;                   /* the worker will fetch it */
} List;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static List L[FH_LISTS];
static int busy, inited;
static char fetch_bin[PATH_MAX], home_dir[PATH_MAX];

static long long sys_now(void) { return (long long)time(NULL); }
static long long (*clock_fn)(void) = sys_now;
void fhs_set_clock(long long (*now)(void)) { clock_fn = now ? now : sys_now; }

int fhs_due(long long now, long long at, long long tried, int fails) {
    if (at > 0 && at <= now + 60 && at / FH_DAY_SECONDS == now / FH_DAY_SECONDS) return 0;   /* fetched today */
    if (fails > 0 && tried > 0 && tried <= now + 60) {
        long long wait = FHS_RETRY;
        for (int i = 1; i < fails && wait < FH_DAY_SECONDS; i++) wait *= 2;
        if (wait > FH_DAY_SECONDS) wait = FH_DAY_SECONDS;
        if (now - tried < wait) return 0;
    }
    return 1;
}

/* ---- the cache: ~/.cache/lordos/flathub/<name>.json = {"at":N,"tried":N,"fails":N,"ids":[…]} ---- */

/* ~/.cache, ~/.cache/lordos, ~/.cache/lordos/flathub: each a folder of ours, no link on the way. → its path */
static int cache_dir(char *out, size_t n, int make) {
    static const char *const parts[] = { ".cache", ".cache/lordos", FHS_DIR };
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        if (snprintf(out, n, "%s/%s", home_dir, parts[i]) >= (int)n) return -1;
        if (make && mkdir(out, 0700) && errno != EEXIST) return -1;
        struct stat st;
        if (lstat(out, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) return -1;
    }
    return 0;
}

static void load(int list) {
    char dir[PATH_MAX], p[PATH_MAX + 32];
    if (cache_dir(dir, sizeof dir, 0) || snprintf(p, sizeof p, "%s/%s.json", dir, fh_list_name(list)) >= (int)sizeof p) return;
    int fd = open(p, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    char *b = NULL;
    ssize_t n = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() && st.st_size > 0 && st.st_size <= FILE_MAX &&
        (b = malloc((size_t)st.st_size + 1))) n = read(fd, b, (size_t)st.st_size);
    close(fd);
    JDoc d;
    if (n > 0 && !json_parse(b, (size_t)n, &d, NULL, 0)) {
        const JVal *r = d.root;
        double at = json_num(r, "at", 0), tried = json_num(r, "tried", 0), fails = json_num(r, "fails", 0);
        List *l = &L[list];
        if (r && r->type == J_OBJ && at >= 0 && at < 1e11 && tried >= 0 && tried < 1e11 && fails >= 0 && fails <= FAILS_MAX) {
            l->tried = (long long)tried;
            l->fails = (int)fails;
            if (at > 0 && !fh_ids_from(json_get(r, "ids"), &l->ids)) { l->have = 1; l->at = (long long)at; }
        }
        json_free(&d);
    }
    free(b);
}

/* written to a new file, then renamed over the old one */
static void save(int list, const List *l) {
    char dir[PATH_MAX], p[PATH_MAX + 32], tmp[PATH_MAX + 48];
    if (cache_dir(dir, sizeof dir, 1) || snprintf(p, sizeof p, "%s/%s.json", dir, fh_list_name(list)) >= (int)sizeof p ||
        snprintf(tmp, sizeof tmp, "%s/.%s.json.new", dir, fh_list_name(list)) >= (int)sizeof tmp) return;
    static char ids[FH_IDS_MAX * (FPAK_ID_MAX + 3) + 32];
    size_t n = l->have ? fh_compact(&l->ids, ids, sizeof ids) : 0;
    /* fh_compact gives {"ids":[…]}: the array is what follows its first colon */
    const char *arr = n ? strchr(ids, ':') : NULL;
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"at\":%lld,\"tried\":%lld,\"fails\":%d,\"ids\":", l->have ? l->at : 0, l->tried, l->fails);
    if (arr) jb_rawn(&b, arr + 1, n - (size_t)(arr + 1 - ids) - 1);
    else jb_raw(&b, "[]");
    jb_raw(&b, "}\n");
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        int ok = b.p && write(fd, b.p, b.n) == (ssize_t)b.n;
        close(fd);
        if (!ok || rename(tmp, p)) unlink(tmp);
    }
    jb_free(&b);
}

/* ---- the worker: one lord-fetch for the lists waiting, then it exits ---- */

/* one list: 0 with *out, or -1 */
static int fetch_one(FetchHelper *h, int list, FhIds *out) {
    if (h->fd < 0 && fetch_helper_start(fetch_bin, h)) return -1;
    int fd = fetch_connect(NC_FLATHUB, FH_HOST);
    if (fd < 0) return -1;
    char req[32];
    snprintf(req, sizeof req, "{\"list\":%d}", list);
    int sent = doc_send(h->fd, FQ_FLATHUB, (uint32_t)list + 1, req, strlen(req), &fd, 1) == 0;
    close(fd);
    struct pollfd p = { h->fd, POLLIN, 0 };
    DocMsg m;
    if (!sent || poll(&p, 1, 90000) != 1 || doc_recv(h->fd, &m)) { fetch_helper_stop(h); return -1; }
    int rc = m.h.op == FR_FLATHUB && m.h.id == (uint32_t)list + 1 && m.json && !m.nfds && !fh_from_compact(m.json, m.len, out) ? 0 : -1;
    doc_msg_free(&m);
    return rc;
}

static void *worker(void *arg) {
    (void)arg;
    FetchHelper h = { -1, 0 };
    static FhIds got;
    for (;;) {
        pthread_mutex_lock(&mu);
        int list = -1;
        for (int i = 0; i < FH_LISTS && list < 0; i++) if (L[i].want) list = i;
        if (list < 0) { busy = 0; pthread_mutex_unlock(&mu); break; }
        L[list].want = 0;
        pthread_mutex_unlock(&mu);
        int ok = !fetch_one(&h, list, &got);
        pthread_mutex_lock(&mu);
        List *l = &L[list];
        l->tried = clock_fn();
        if (ok) { l->ids = got; l->have = 1; l->at = l->tried; l->fails = 0; }
        else if (l->fails < FAILS_MAX) l->fails++;
        static List copy;
        copy = *l;
        pthread_mutex_unlock(&mu);
        save(list, &copy);
    }
    fetch_helper_stop(&h);
    return NULL;
}

int fhs_init(const char *bindir, const char *home) {
    pthread_mutex_lock(&mu);
    int rc = 0;
    if (!inited) {
        if (!bindir || !home || home[0] != '/' || snprintf(fetch_bin, sizeof fetch_bin, "%s/lord-fetch", bindir) >= (int)sizeof fetch_bin ||
            snprintf(home_dir, sizeof home_dir, "%s", home) >= (int)sizeof home_dir) rc = -1;
        else {
            for (int i = 0; i < FH_LISTS; i++) load(i);
            inited = 1;
        }
    }
    pthread_mutex_unlock(&mu);
    return rc;
}

int fhs_request(void) {
    pthread_mutex_lock(&mu);
    if (!inited) { pthread_mutex_unlock(&mu); return -1; }
    if (!busy) {
        long long now = clock_fn();
        int any = 0;
        for (int i = 0; i < FH_LISTS; i++)
            if (fhs_due(now, L[i].have ? L[i].at : 0, L[i].tried, L[i].fails)) { L[i].want = 1; any = 1; }
        if (any) {
            busy = 1;
            pthread_t th;
            pthread_attr_t a;
            pthread_attr_init(&a);
            pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&th, &a, worker, NULL)) {
                busy = 0;
                for (int i = 0; i < FH_LISTS; i++) L[i].want = 0;
            }
            pthread_attr_destroy(&a);
        }
    }
    int rc = busy;
    pthread_mutex_unlock(&mu);
    return rc;
}

int fhs_busy(void) {
    pthread_mutex_lock(&mu);
    int b = busy;
    pthread_mutex_unlock(&mu);
    return b;
}

int fhs_get(int list, FhIds *out, long long *at) {
    if (list < 0 || list >= FH_LISTS) return 0;
    pthread_mutex_lock(&mu);
    int have = L[list].have;
    if (have) { *out = L[list].ids; *at = L[list].at; }
    pthread_mutex_unlock(&mu);
    return have;
}
