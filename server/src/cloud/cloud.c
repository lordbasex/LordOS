/* cloud.c — see cloud.h and docs/CLOUD.md. The accounts, the rclone daemon and the thread that drives both.
 *
 *   ~/.config/lordos/cloud/accounts.json   id, name, kind, created, paused; the options (bandwidth, battery, space)
 *   ~/.config/lordos/cloud/rclone.conf     each account's type and non-secret options (0600, no secret in it)
 *   ~/.config/lordos/cloud/keep.json       Keep on This Computer, per account
 *   ~/.config/lordos/cloud/links.json      the links made from LordOS (rclone can't list them)
 *   ~/.config/lordos/cloud/rclone-debug    when it exists, rclone logs at DEBUG (a diagnosis, docs/CLOUD.md)
 *   ~/.cache/lordos/cloud/                 rclone's VFS cache (vfs/<id>/, vfsMeta/<id>/)
 *   ~/.cache/lordos/cloud.log              rclone's log (the Logs app shows it)
 *   $XDG_RUNTIME_DIR/lordos/cloud/rc.sock  the rc socket, in a 0700 folder
 *
 * The secrets of an account (token, password, key) are one keyring item, "cloud:<id>", a JSON object of rclone
 * options exactly as config/get gives them (passwords obscured by rclone). At each start of rclone they go to it
 * with config/create over the socket; rclone keeps its configuration in memory only (--config ""). A token rclone
 * changed (a refreshed token, a provider's session) is read back with config/get every 5 minutes and before rclone
 * stops, and saved in the keyring. */
#include "cloud_int.h"
#include "../doc/wire.h"
#include "../errcode.h"
#include "../sec/cgroup.h"
#include "../sec/json.h"
#include "../sec/sandbox.h"

#include <crypt.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define FUSERMOUNT "/usr/bin/fusermount3"
#define LOG_NAME "cloud.log"
#define LOG_MAX (4L << 20)
#define RC_SHORT 10000              /* ms: local calls */
#define RC_LONG 60000               /* ms: calls that reach the provider */
#define SIGNIN_S 300
#define RESTARTS_PER_MIN 5
#define IDLE_STOP_S 120
#define RECENT_MAX 20

pthread_mutex_t cl_mu = PTHREAD_MUTEX_INITIALIZER;
Acc cl_acc[ACC_MAX];
char cl_home[PATH_MAX], cl_root[PATH_MAX], cl_cache[PATH_MAX], cl_cfg[PATH_MAX];
RcConn cl_rc;
int cl_running;

static const CloudSecrets *secrets;
static int started, efd_push = -1, efd_job = -1;
static volatile int worker_done;
static char rt_dir[PATH_MAX], log_path[PATH_MAX];
static long long last_emit_ms;
static char *last_state;

static struct { long bw_up, bw_down; int pause_batt, pause_metered, auto_free, free_below, pause_all; } opt = { 0, 0, 1, 1, 1, 10, 0 };
static struct { pid_t pid; char version[40]; time_t started; int restarts; long long crash[RESTARTS_PER_MIN]; char error[64]; time_t idle_since; } dmn;
static int on_batt;

/* providers: what JS gets, and per kind the fields offered (and which are secret) */
typedef struct { char kind[24]; int n; char name[64][65]; char secret[64]; char required[64]; char *secrets_all; } KindInfo;
#define KI_MAX 192
static KindInfo *kinfo;             /* replaced whole, under cl_mu, by each providers() */
static int nkinfo;

/* the sign-in going on (6.2) */
static struct {
    int on;
    char id[16], kind[24], name[CLOUD_NAME_MAX + 1], url[200], phase[10], error[64], account[16];
    long long expires_ms;
    pid_t pid;
    int fo, fe, opened;
    char out[65536];
    size_t ol;
    char ebuf[4096];
    size_t el;
    char cid[256], csec[256];
    char update[16];                /* Sign In Again: the existing account */
    char message[201];              /* rclone's words when setting the account up failed (#87), cleaned, for the card */
} si;

static struct { char path[PATH_MAX]; char account[16]; int up; long long at; } recent[RECENT_MAX];
#define CONFLICTS_MAX 20
static struct { char path[PATH_MAX], kept[PATH_MAX]; char account[16]; long long at; } conflicts[CONFLICTS_MAX];
static int nconflicts;
static int nrecent;
static long long seen_done;          /* newest completed_at already in recent */

typedef struct Link { struct Link *next; char id[16]; char *rel, *url; long long expires, made; } Link;
static Link *links;

enum { J_RETRY = 100, J_ADDLATE };
enum { J_PROVIDERS = 1, J_ADD, J_REMOVE, J_RENAME, J_ABOUT, J_LINK, J_UNLINK, J_RESTART, J_OPTIONS, J_PAUSE, J_SIGNIN, J_STOP,
       J_ANSWER, J_TRASH, J_MEASURE, J_CANCEL_MEASURE };
typedef struct Job { struct Job *next; int type, req, flag; char id[16]; char *a, *b, *c, *d; } Job;
static Job *jhead, *jtail;

static void (*reply_fn)(int, const char *);
typedef struct Reply { struct Reply *next; int req; char *json; } Reply;
static Reply *rhead;

void ct_params(const Acc *a, JBuf *b, int *first);     /* cloud_trust.c */
static int presigned(const Acc *a);
static void changed_failed(Acc *a, int req, const char *code, char *msg);
static void note_synced(Acc *a, const char *rel);
static void wait_answer(Acc *a, int req, char *q, const char *sec, const char *tsec, int upd);
static char *add_impl(const char *kind, const char *name, const char *fields, const char *account_id, int *req, int use_req);
static const CloudKind *kind_get(const char *kind);
static const CloudKind *kind_put(const char *kind, const char *type, const char *label, const char *group);

/* ---- small helpers ---- */

int cl_pf(char *out, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    return r;
}

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static void wipe_free(char *s) { if (s) { explicit_bzero(s, strlen(s)); free(s); } }

static void mkdirs(const char *dir, mode_t m) {
    char p[PATH_MAX];
    cl_pf(p, sizeof p, "%s", dir);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, m); *s = '/'; }
    mkdir(p, m);
}

static char *read_file(const char *path, size_t max, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size > max) { close(fd); return NULL; }
    char *b = malloc((size_t)st.st_size + 1);
    ssize_t n = b ? read(fd, b, (size_t)st.st_size) : -1;
    close(fd);
    if (n < 0) { free(b); return NULL; }
    b[n] = 0;
    if (len) *len = (size_t)n;
    return b;
}

/* replaces path whole, 0600 */
static int write_file(const char *path, const char *data, size_t n) {
    char tmp[PATH_MAX + 8];
    cl_pf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int ok = write(fd, data, n) == (ssize_t)n && !fsync(fd);
    close(fd);
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

static void rand_hex(char *out, int bytes) {
    unsigned char r[64];
    if (bytes > 64) bytes = 64;
    if (getrandom(r, (size_t)bytes, 0) != bytes) for (int i = 0; i < bytes; i++) r[i] = (unsigned char)(random() ^ (int)mono_ms());
    for (int i = 0; i < bytes; i++) sprintf(out + 2 * i, "%02x", r[i]);
    explicit_bzero(r, sizeof r);
}

static unsigned long long fnv(const char *s) {
    unsigned long long h = 1469598103934665603ull;
    for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h;
}

static void cpy(char *d, size_t n, const char *s) { cl_pf(d, n, "%s", s ? s : ""); }
/* U5: rclone's words for Get Info, never a server's HTML page (tags out, spaces collapsed, at most 200 bytes) */
#define set_message(a, m) cloud_clean_text((m), (a)->message, 201)

static void kv_clear(Params *p) { for (int i = 0; i < p->n; i++) wipe_free(p->kv[i].val); p->n = 0; }
static const char *kv_get(const Params *p, const char *k) { for (int i = 0; i < p->n; i++) if (!strcmp(p->kv[i].key, k)) return p->kv[i].val; return NULL; }
static int kv_set(Params *p, const char *k, const char *v) {
    if (!cloud_key_ok(k) || !cloud_value_ok(v)) return -1;
    for (int i = 0; i < p->n; i++) if (!strcmp(p->kv[i].key, k)) { char *d = strdup(v); if (!d) return -1; wipe_free(p->kv[i].val); p->kv[i].val = d; return 0; }
    if (p->n >= KV_MAX) return -1;
    char *d = strdup(v);
    if (!d) return -1;
    cpy(p->kv[p->n].key, sizeof p->kv[0].key, k);
    p->kv[p->n++].val = d;
    return 0;
}

Acc *cl_find(const char *id) {
    for (int i = 0; id && i < ACC_MAX; i++) if (cl_acc[i].used && !strcmp(cl_acc[i].id, id)) return &cl_acc[i];
    return NULL;
}
Acc *cl_find_name(const char *name) {
    for (int i = 0; name && i < ACC_MAX; i++) if (cl_acc[i].used && !strcmp(cl_acc[i].name, name)) return &cl_acc[i];
    return NULL;
}
static int naccounts(void) { int n = 0; for (int i = 0; i < ACC_MAX; i++) n += cl_acc[i].used; return n; }

static void wake(int fd) { if (fd >= 0) { uint64_t one = 1; ssize_t r = write(fd, &one, sizeof one); (void)r; } }
void cl_changed(void) { wake(efd_job); }

static void acc_free_live(Acc *a) {
    for (int i = 0; i < a->nq; i++) free(a->q[i].name);
    for (int i = 0; i < a->nt; i++) free(a->t[i].name);
    a->nq = a->nt = 0;
}
static void acc_free(Acc *a) {
    acc_free_live(a);
    for (int i = 0; i < a->nchecked; i++) free(a->checked[i]);
    a->nchecked = 0;
    kv_clear(&a->p);
    wipe_free(a->pending_secret);
    for (int i = 0; i < a->nkeep; i++) free(a->keep[i]);
    for (int i = 0; i < a->nerrn; i++) free(a->errn[i]);
    memset(a, 0, sizeof *a);
}

int cl_paused(const Acc *a) { return a->paused || opt.pause_all || (opt.pause_batt && on_batt); }

/* ---- battery (same rule as Backups) ---- */

static int small_read(const char *p, char *out, size_t n) {
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t k = read(fd, out, n - 1);
    close(fd);
    if (k < 0) return -1;
    out[k] = 0;
    out[strcspn(out, "\n")] = 0;
    return 0;
}
static int on_battery(void) {
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return 0;
    int dis = 0, mains = 0;
    for (struct dirent *e; (e = readdir(d)); ) {
        if (e->d_name[0] == '.') continue;
        char p[320], v[64];
        cl_pf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
        if (small_read(p, v, sizeof v)) continue;
        if (!strcmp(v, "Battery")) { cl_pf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name); if (!small_read(p, v, sizeof v) && !strcmp(v, "Discharging")) dis = 1; }
        else if (!strcmp(v, "Mains") || !strcmp(v, "USB") || !strcmp(v, "USB_C")) { cl_pf(p, sizeof p, "/sys/class/power_supply/%s/online", e->d_name); if (!small_read(p, v, sizeof v) && !strcmp(v, "1")) mains = 1; }
    }
    closedir(d);
    return dis && !mains;
}

/* ---- the files in ~/.config/lordos/cloud ---- */

static void path_in(char *out, size_t n, const char *name) { cl_pf(out, n, "%s/%s", cl_cfg, name); }

static void save_accounts(void) {
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"options\":{\"bwUp\":%ld,\"bwDown\":%ld,\"pauseOnBattery\":%s,\"pauseOnMetered\":%s,\"autoFree\":%s,\"freeBelowGb\":%d,\"pauseAll\":%s},\"accounts\":[",
              opt.bw_up, opt.bw_down, opt.pause_batt ? "true" : "false", opt.pause_metered ? "true" : "false", opt.auto_free ? "true" : "false",
              opt.free_below, opt.pause_all ? "true" : "false");
    int first = 1;
    for (int i = 0; i < ACC_MAX; i++) {
        Acc *a = &cl_acc[i];
        if (!a->used) continue;
        jb_raw(&b, first ? "{\"id\":" : ",{\"id\":");
        first = 0;
        jb_str(&b, a->id);
        jb_raw(&b, ",\"name\":");
        jb_str(&b, a->name);
        jb_raw(&b, ",\"kind\":");
        jb_str(&b, a->k->kind);
        jb_raw(&b, ",\"type\":");
        jb_str(&b, a->k->type);
        jb_raw(&b, ",\"group\":");
        jb_str(&b, a->k->group);
        jb_raw(&b, ",\"provider\":");
        jb_str(&b, a->k->label);
        jb_printf(&b, ",\"created\":%lld,\"paused\":%s,\"ownClient\":%s,\"measuredBytes\":%lld,\"measuredCount\":%lld,\"measuredAt\":%lld}",
                  a->created, a->paused ? "true" : "false", a->own_client ? "true" : "false", a->measured_bytes, a->measured_count, a->measured_at);
    }
    jb_raw(&b, "]}\n");
    char p[PATH_MAX];
    path_in(p, sizeof p, "accounts.json");
    if (!b.oom && write_file(p, b.p, b.n)) fprintf(stderr, "cloud: could not write %s\n", p);
    jb_free(&b);

    /* rclone.conf: types and non-secret options only */
    jb_init(&b);
    jb_raw(&b, "# LordOS Cloud: each account's type and options. Its secrets are in the keyring, not here.\n");
    for (int i = 0; i < ACC_MAX; i++) {
        Acc *a = &cl_acc[i];
        if (!a->used) continue;
        jb_printf(&b, "\n[%s]\ntype = %s\n", a->id, a->k->type);
        for (int k = 0; k < a->p.n; k++) if (strcmp(a->p.kv[k].key, "type")) jb_printf(&b, "%s = %s\n", a->p.kv[k].key, a->p.kv[k].val);
    }
    path_in(p, sizeof p, "rclone.conf");
    if (!b.oom && write_file(p, b.p, b.n)) fprintf(stderr, "cloud: could not write %s\n", p);
    jb_free(&b);
}

void cl_save_keep(void) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{");
    int first = 1;
    for (int i = 0; i < ACC_MAX; i++) {
        Acc *a = &cl_acc[i];
        if (!a->used || !a->nkeep) continue;
        if (!first) jb_raw(&b, ",");
        first = 0;
        jb_str(&b, a->id);
        jb_raw(&b, ":[");
        for (int k = 0; k < a->nkeep; k++) { if (k) jb_raw(&b, ","); jb_str(&b, a->keep[k]); }
        jb_raw(&b, "]");
    }
    jb_raw(&b, "}\n");
    char p[PATH_MAX];
    path_in(p, sizeof p, "keep.json");
    if (!b.oom) write_file(p, b.p, b.n);
    jb_free(&b);
}

static void save_links(void) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"links\":[");
    for (Link *l = links; l; l = l->next) {
        jb_raw(&b, l == links ? "{\"id\":" : ",{\"id\":");
        jb_str(&b, l->id);
        jb_raw(&b, ",\"path\":");
        jb_str(&b, l->rel);
        jb_raw(&b, ",\"url\":");
        jb_str(&b, l->url);
        jb_printf(&b, ",\"expires\":%lld,\"made\":%lld}", l->expires, l->made);
    }
    jb_raw(&b, "]}\n");
    char p[PATH_MAX];
    path_in(p, sizeof p, "links.json");
    if (!b.oom) write_file(p, b.p, b.n);
    jb_free(&b);
}

/* rclone.conf (ours): [id] then "key = value" */
static void load_conf(void) {
    char p[PATH_MAX];
    path_in(p, sizeof p, "rclone.conf");
    char *t = read_file(p, 1 << 20, NULL);
    if (!t) return;
    Acc *a = NULL;
    for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || !line[0]) continue;
        if (line[0] == '[') { char *e = strchr(line, ']'); if (e) *e = 0; a = cl_find(line + 1); continue; }
        char *eq = strstr(line, " = ");
        if (!a || !eq) continue;
        *eq = 0;
        if (strcmp(line, "type")) kv_set(&a->p, line, eq + 3);
    }
    free(t);
}

static void load_all(void) {
    char p[PATH_MAX];
    path_in(p, sizeof p, "accounts.json");
    size_t n;
    char *t = read_file(p, 1 << 20, &n);
    JDoc d;
    if (t && !json_parse(t, n, &d, NULL, 0)) {
        const JVal *o = json_get(d.root, "options");
        if (o) {
            opt.bw_up = (long)json_num(o, "bwUp", 0);
            opt.bw_down = (long)json_num(o, "bwDown", 0);
            opt.pause_batt = json_bool(o, "pauseOnBattery", 1);
            opt.pause_metered = json_bool(o, "pauseOnMetered", 1);
            opt.auto_free = json_bool(o, "autoFree", 1);
            opt.free_below = (int)json_num(o, "freeBelowGb", 10);
            opt.pause_all = json_bool(o, "pauseAll", 0);
            char r[64];
            if (cloud_bw_rate(opt.bw_up, opt.bw_down, r, sizeof r)) opt.bw_up = opt.bw_down = 0;
            if (opt.free_below < 5 || opt.free_below > 100) opt.free_below = 10;
        }
        const JVal *as = json_get(d.root, "accounts");
        int k = 0;
        for (const JVal *x = as && as->type == J_ARR ? as->kid : NULL; x && k < ACC_MAX; x = x->next) {
            const char *id = json_str(x, "id"), *name = json_str(x, "name"), *kind = json_str(x, "kind");
            /* one entry per kind for the whole session (the list rclone gives later updates the same one), from what was
             * saved with the account and the overlay */
            const CloudKind *ov = kind ? cloud_kind(kind) : NULL;
            const char *ty = json_str(x, "type"), *pv = json_str(x, "provider"), *gr = json_str(x, "group");
            const CloudKind *ck = kind && (ov || ty) ? kind_put(kind, ty ? ty : ov->type, pv ? pv : ov ? ov->label : NULL, gr ? gr : ov ? ov->group : NULL) : NULL;
            if (!id || !cloud_key_ok(id) || strlen(id) >= sizeof cl_acc[0].id || !cloud_name_ok(name) || !ck || cl_find(id) || cl_find_name(name)) continue;
            Acc *a = &cl_acc[k++];
            memset(a, 0, sizeof *a);
            a->used = 1;
            cpy(a->id, sizeof a->id, id);
            cpy(a->name, sizeof a->name, name);
            a->k = ck;
            a->created = (long long)json_num(x, "created", 0);
            a->paused = json_bool(x, "paused", 0);
            a->own_client = json_bool(x, "ownClient", 0);
            a->trash_supported = -1;
            a->measured_bytes = (long long)json_num(x, "measuredBytes", 0);
            a->measured_count = (long long)json_num(x, "measuredCount", 0);
            a->measured_at = (long long)json_num(x, "measuredAt", 0);
            a->links_supported = -1;
        }
        json_free(&d);
    }
    free(t);
    load_conf();
    path_in(p, sizeof p, "keep.json");
    t = read_file(p, 4 << 20, &n);
    if (t && !json_parse(t, n, &d, NULL, 0)) {
        for (const JVal *x = d.root->type == J_OBJ ? d.root->kid : NULL; x; x = x->next) {
            Acc *a = cl_find(x->key);
            for (const JVal *y = a && x->type == J_ARR ? x->kid : NULL; y && a->nkeep < KEEP_MAX; y = y->next)
                if (y->type == J_STR && cloud_rel_ok(y->str)) a->keep[a->nkeep++] = strdup(y->str);
        }
        json_free(&d);
    }
    free(t);
    path_in(p, sizeof p, "links.json");
    t = read_file(p, 4 << 20, &n);
    if (t && !json_parse(t, n, &d, NULL, 0)) {
        Link **tail = &links;
        const JVal *ls = json_get(d.root, "links");
        for (const JVal *x = ls && ls->type == J_ARR ? ls->kid : NULL; x; x = x->next) {
            const char *id = json_str(x, "id"), *rel = json_str(x, "path"), *url = json_str(x, "url");
            if (!id || !cl_find(id) || !rel || !cloud_rel_ok(rel) || !url || !cloud_url_ok(url)) continue;
            Link *l = calloc(1, sizeof *l);
            if (!l) break;
            cpy(l->id, sizeof l->id, id);
            l->rel = strdup(rel);
            l->url = strdup(url);
            l->expires = (long long)json_num(x, "expires", 0);
            l->made = (long long)json_num(x, "made", 0);
            *tail = l;
            tail = &l->next;
        }
        json_free(&d);
    }
    free(t);
}

/* ---- rc ---- */

/* one call, parsed: status (200 ok), the document in *d when it parsed (json_free it). msg: rclone's error text. */
static int rcj(int m, const char *body, JDoc *d, char *msg, size_t mn, int timeout) {
    RcConn c;
    pthread_mutex_lock(&cl_mu);
    c = cl_rc;
    int run = cl_running;
    pthread_mutex_unlock(&cl_mu);
    if (msg && mn) msg[0] = 0;
    d->root = NULL;
    d->mem = NULL;
    if (!run) { if (msg) cpy(msg, mn, "not running"); return -1; }
    char *out = NULL;
    size_t ol = 0;
    int st = rc_call(&c, m, body, &out, &ol, timeout);
    explicit_bzero(&c, sizeof c);
    if (st < 0) { if (msg) cpy(msg, mn, "no answer"); return -1; }
    if (json_parse(out, ol, d, NULL, 0)) { d->root = NULL; d->mem = NULL; }
    if (st != 200 && msg) {
        const char *e = d->root ? json_str(d->root, "error") : NULL;
        cpy(msg, mn, e ? e : "rc error");
    }
    explicit_bzero(out, ol);
    free(out);
    return st;
}
static int rcs(int m, const char *body, char *msg, size_t mn, int timeout) {
    JDoc d;
    int st = rcj(m, body, &d, msg, mn, timeout);
    if (d.root) json_free(&d);
    return st;
}

/* a cloud.* code from rclone's words about an account */
static const char *code_of(const char *msg, int oauth) {
    if (!msg || !msg[0]) return "cloud.failed";
    if (strstr(msg, "invalid_grant") || strstr(msg, "token expired") || strstr(msg, "oauth2: ") ||
        (oauth && (strstr(msg, "401") || strstr(msg, "invalid_token") || strstr(msg, "Invalid 'access_token'"))))
        return "cloud.signInAgain";
    if (strstr(msg, "no such host") || strstr(msg, "network is unreachable") || strstr(msg, "i/o timeout") || strstr(msg, "connection refused") ||
        strstr(msg, "dial tcp") || strstr(msg, "no route to host"))
        return "cloud.offline";
    if (strstr(msg, "401") || strstr(msg, "403") || strstr(msg, "Unauthorized") || strstr(msg, "authentication") || strstr(msg, "permission denied") ||
        strstr(msg, "incorrect username or password"))
        return "cloud.authFailed";
    if (strstr(msg, "directory not empty") || strstr(msg, "not empty")) return "cloud.folderNotEmpty";
    return "cloud.failed";
}

int cl_rc_simple(int m, const char *body, int timeout) { return rcs(m, body, NULL, 0, timeout); }

/* ---- the daemon ---- */

const char *cloud_log_name(void) { return LOG_NAME; }

static const char *rclone_bin(void) {
#ifdef CLOUD_TEST
    const char *e = getenv("LORDOS_CLOUD_RCLONE");       /* test builds only */
    if (e && e[0] == '/') return e;
#endif
    return CLOUD_RCLONE;
}

int cloud_available(void) { return access(rclone_bin(), X_OK) == 0; }

static void child_common(void) {
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    prctl(PR_SET_PDEATHSIG, SIGTERM);       /* lord-server gone: rclone unmounts and ends */
    umask(077);
    setpriority(PRIO_PROCESS, 0, 10);
    syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, 0, 3 << 13 /* IOPRIO_CLASS_IDLE */);
}

static char **child_env(char **env, char *buf, size_t n, const char *extra1, const char *extra2) {
    size_t o = 0;
    int k = 0;
    const char *user = getenv("USER");
    char item[PATH_MAX + 32];
    const char *items[8];
    int ni = 0;
    static char h[PATH_MAX + 8], r[PATH_MAX + 24], u[96];
    cl_pf(h, sizeof h, "HOME=%s", cl_home);
    items[ni++] = h;
    items[ni++] = "PATH=/usr/bin:/bin";
    items[ni++] = "LANG=C.UTF-8";
    if (getenv("XDG_RUNTIME_DIR")) { cl_pf(r, sizeof r, "XDG_RUNTIME_DIR=%s", getenv("XDG_RUNTIME_DIR")); items[ni++] = r; }
    if (user && strlen(user) < 64 && !strpbrk(user, "\n=")) { cl_pf(u, sizeof u, "USER=%s", user); items[ni++] = u; }
    if (extra1) items[ni++] = extra1;
    if (extra2) items[ni++] = extra2;
    for (int i = 0; i < ni && k < 9; i++) {
        cl_pf(item, sizeof item, "%s", items[i]);
        size_t l = strlen(item) + 1;
        if (o + l > n) break;
        memcpy(buf + o, item, l);
        env[k++] = buf + o;
        o += l;
    }
    env[k] = NULL;
    return env;
}

static int fusermount_u(const char *dir) {
    pid_t p = fork();
    if (!p) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(null, 1); dup2(null, 2);
        syscall(SYS_close_range, 3, ~0u, 0);
        char *argv[] = { "fusermount3", "-u", "-z", (char *)dir, NULL };
        char *env[] = { "PATH=/usr/bin:/bin", NULL };
        execve(FUSERMOUNT, argv, env);
        _exit(127);
    }
    if (p < 0) return -1;
    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) && !WEXITSTATUS(st) ? 0 : -1;
}

static void mount_dir(const Acc *a, char *out, size_t n) { cl_pf(out, n, "%s/%s", cl_root, a->name); }

/* dir is a mount point now (/proc/self/mounts, spaces escaped as \040) */
static int is_mounted(const char *dir) {
    char want[PATH_MAX * 4];
    size_t o = 0;
    for (const char *p = dir; *p && o + 5 < sizeof want; p++) {
        if (*p == ' ') { memcpy(want + o, "\\040", 4); o += 4; }
        else if (*p == '\t') { memcpy(want + o, "\\011", 4); o += 4; }
        else if (*p == '\n') { memcpy(want + o, "\\012", 4); o += 4; }
        else if (*p == '\\') { memcpy(want + o, "\\134", 4); o += 4; }
        else want[o++] = *p;
    }
    want[o] = 0;
    FILE *f = fopen("/proc/self/mounts", "re");
    if (!f) return 0;
    char line[PATH_MAX * 4 + 256];
    int yes = 0;
    while (!yes && fgets(line, sizeof line, f)) {
        char *a = strchr(line, ' ');
        if (!a) continue;
        char *b = strchr(a + 1, ' ');
        if (b && (size_t)(b - a - 1) == o && !memcmp(a + 1, want, o)) yes = 1;
    }
    fclose(f);
    return yes;
}

/* a mount left behind: by an rclone that died (the folder answers ENOTCONN, "Transport endpoint is not connected"), or one
 * still there before mounting again. Lazy (-z): a program that still has a file open there doesn't keep it busy. */
static void clear_stale(const char *dir) {
    struct stat st;
    if ((stat(dir, &st) && (errno == ENOTCONN || errno == EIO)) || is_mounted(dir)) fusermount_u(dir);
}

static void daemon_gone(void) {
    pthread_mutex_lock(&cl_mu);
    cl_running = 0;
    dmn.pid = 0;
    for (int i = 0; i < ACC_MAX; i++) if (cl_acc[i].used) { cl_acc[i].configured = cl_acc[i].mounted = cl_acc[i].held = 0; cl_acc[i].in_use = 0; acc_free_live(&cl_acc[i]); }
    pthread_mutex_unlock(&cl_mu);
    for (int i = 0; i < ACC_MAX; i++) if (cl_acc[i].used) { char d[PATH_MAX]; mount_dir(&cl_acc[i], d, sizeof d); clear_stale(d); }
}

static int daemon_start(void) {
    if (!cloud_available()) { cpy(dmn.error, sizeof dmn.error, "cloud.unavailable"); return -1; }
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg || xdg[0] != '/') { cpy(dmn.error, sizeof dmn.error, "cloud.noRuntimeDir"); return -1; }
    cl_pf(rt_dir, sizeof rt_dir, "%s/lordos/cloud", xdg);
    mkdirs(rt_dir, 0700);
    struct stat st;
    if (lstat(rt_dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) { cpy(dmn.error, sizeof dmn.error, "cloud.noRuntimeDir"); return -1; }
    chmod(rt_dir, 0700);
    char sock[PATH_MAX];
    cl_pf(sock, sizeof sock, "%s/rc.sock", rt_dir);
    if (strlen(sock) >= sizeof cl_rc.sock) { cpy(dmn.error, sizeof dmn.error, "cloud.noRuntimeDir"); return -1; }
    unlink(sock);
    /* a random user and password per start; rclone gets only a bcrypt of the password, through a memfd */
    char user[17], pass[65], up[100], salt[64], line[200];
    rand_hex(user, 8);
    rand_hex(pass, 32);
    struct crypt_data *cd = calloc(1, sizeof *cd);
    const char *s = crypt_gensalt_rn("$2b$", 4, NULL, 0, salt, sizeof salt);   /* the password is random: cost 4 is enough */
    const char *h = cd && s ? crypt_r(pass, s, cd) : NULL;
    if (!h || h[0] != '$') { free(cd); cpy(dmn.error, sizeof dmn.error, "cloud.startFailed"); return -1; }
    int ll = cl_pf(line, sizeof line, "%s:%s\n", user, h);
    explicit_bzero(cd, sizeof *cd);
    free(cd);
    int mfd = (int)syscall(SYS_memfd_create, "lordos-cloud-htpasswd", 1u /* MFD_CLOEXEC */);
    if (mfd < 0 || write(mfd, line, (size_t)ll) != ll) { if (mfd >= 0) close(mfd); cpy(dmn.error, sizeof dmn.error, "cloud.startFailed"); return -1; }
    lseek(mfd, 0, SEEK_SET);
    cl_pf(up, sizeof up, "%s:%s", user, pass);
    explicit_bzero(pass, sizeof pass);

    char addr[PATH_MAX + 8], cache[PATH_MAX];
    cl_pf(addr, sizeof addr, "unix://%s", sock);
    cl_pf(cache, sizeof cache, "%s", cl_cache);
    mkdirs(cache, 0700);
    cl_pf(log_path, sizeof log_path, "%s/.cache/lordos/" LOG_NAME, cl_home);
    if (!stat(log_path, &st) && st.st_size > LOG_MAX) {
        char old[PATH_MAX + 8];
        cl_pf(old, sizeof old, "%s/.cache/lordos/cloud.old", cl_home);
        rename(log_path, old);
    }
    /* a diagnosis the person turns on: rclone at DEBUG while ~/.config/lordos/cloud/rclone-debug exists (iCloud Drive's
     * sign-in steps, with each cookie as its length and a hash, never its value) */
    char dbg[PATH_MAX + 32];
    cl_pf(dbg, sizeof dbg, "%s/.config/lordos/cloud/rclone-debug", cl_home);
    const char *level = access(dbg, F_OK) ? "NOTICE" : "DEBUG";
    if (level[0] == 'D') fprintf(stderr, "cloud: rclone logs at DEBUG (%s exists)\n", dbg);
    char *argv[] = { "rclone", "rcd", "--rc-addr", addr, "--rc-htpasswd", "/proc/self/fd/3", "--config", "", "--cache-dir", cache,
                     "--log-file", log_path, "--log-level", (char *)level, "--use-mmap", "--buffer-size", "16M", "--transfers", "4",
                     /* B10: a server that doesn't answer fails a cold read in about 8 s, not minutes (measured: 8.6 s
                      * refused, >200 s with rclone's defaults); uploads are retried by the VFS queue anyway */
                     "--contimeout", "5s", "--timeout", "30s", "--low-level-retries", "1",
                     "--checkers", "8", NULL };
    char *env[10], envbuf[4096];
    child_env(env, envbuf, sizeof envbuf, NULL, NULL);
    const char *bin = rclone_bin();
    pid_t p = fork();
    if (!p) {
        int null = open("/dev/null", O_RDWR);
        int f = fcntl(mfd, F_DUPFD, 10);
        dup2(f, 3);
        dup2(null, 0); dup2(null, 1); dup2(null, 2);
        syscall(SYS_close_range, 4, ~0u, 0);
        child_common();
        CgroupLimits lim = { .mem_high_mb = 512, .mem_max_mb = 0, .cpu_percent = 0 };
        if (cgroup_enter("app-lordos-" CLOUD_SCOPE ".scope", &lim) && getenv("LORDOS_SESSIOND")) _exit(126);
        execve(bin, argv, env);
        _exit(127);
    }
    close(mfd);
    if (p < 0) { explicit_bzero(up, sizeof up); cpy(dmn.error, sizeof dmn.error, "cloud.startFailed"); return -1; }
    pthread_mutex_lock(&cl_mu);
    cpy(cl_rc.sock, sizeof cl_rc.sock, sock);
    cloud_b64((const unsigned char *)up, strlen(up), cl_rc.auth);
    cl_running = 1;
    dmn.pid = p;
    pthread_mutex_unlock(&cl_mu);
    explicit_bzero(up, sizeof up);
    for (int i = 0; i < 100; i++) {                         /* up to 10 s */
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        int wst;
        if (waitpid(p, &wst, WNOHANG) == p) { daemon_gone(); cpy(dmn.error, sizeof dmn.error, "cloud.startFailed"); return -1; }
        JDoc d;
        if (rcj(RCM_VERSION, "{}", &d, NULL, 0, 2000) == 200) {
            const char *v = json_str(d.root, "version");
            pthread_mutex_lock(&cl_mu);
            cpy(dmn.version, sizeof dmn.version, v);
            dmn.started = time(NULL);
            dmn.error[0] = 0;
            pthread_mutex_unlock(&cl_mu);
            json_free(&d);
            char r[64], b[128];
            if (!cloud_bw_rate(opt.bw_up, opt.bw_down, r, sizeof r)) { cl_pf(b, sizeof b, "{\"rate\":\"%s\"}", r); rcs(RCM_BWLIMIT, b, NULL, 0, RC_SHORT); }
            fprintf(stderr, "cloud: rclone %s started (pid %d), rc on a Unix socket only\n", dmn.version, (int)p);
            return 0;
        }
        if (d.root) json_free(&d);
    }
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
    daemon_gone();
    cpy(dmn.error, sizeof dmn.error, "cloud.startFailed");
    return -1;
}

/* why an account can't do that now: the service, the keyring, or the account itself */
static const char *unavailable(const Acc *a) {
    if (!cl_running) return dmn.error[0] ? dmn.error : "cloud.notRunning";
    if (a->locked) return a->locked == 2 ? "cloud.noKeyring" : "cloud.keyringLocked";
    if (a->err[0]) return a->err;
    return "cloud.notConnected";
}

static void unmount_acc(Acc *a) {
    char d[PATH_MAX];
    mount_dir(a, d, sizeof d);
    JBuf j;
    jb_init(&j);
    jb_raw(&j, "{\"mountPoint\":");
    jb_str(&j, d);
    jb_raw(&j, "}");
    /* a file open in the folder makes it busy: try again for a second and a half, then detach it lazily (the open
     * file keeps working until it is closed, and the folder is free to mount again) */
    for (int i = 0; a->mounted && !j.oom && i < 4; i++) {
        if (i) { struct timespec ts = { 0, 500 * 1000 * 1000 }; nanosleep(&ts, NULL); }
        if (rcs(RCM_UNMOUNT, j.p, NULL, 0, RC_SHORT) == 200 && !is_mounted(d)) break;
    }
    jb_free(&j);
    a->mounted = 0;
    if (is_mounted(d)) { fprintf(stderr, "cloud: %s is busy: unmounted lazily\n", d); fusermount_u(d); }
    clear_stale(d);
}

static void token_check(Acc *a);

static void daemon_stop(void) {
    if (!cl_running) return;
    /* what rclone changed since the last check (up to 5 minutes) would be lost with it */
    for (int i = 0; i < ACC_MAX; i++) if (cl_acc[i].used && cl_acc[i].configured) token_check(&cl_acc[i]);
    for (int i = 0; i < ACC_MAX; i++) if (cl_acc[i].used && cl_acc[i].mounted) unmount_acc(&cl_acc[i]);
    rcs(RCM_QUIT, "{}", NULL, 0, 2000);
    pid_t p = dmn.pid;
    for (int i = 0; p > 0 && i < 50; i++) {
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (waitpid(p, NULL, WNOHANG) == p) { p = 0; break; }
    }
    if (p > 0) { kill(p, SIGKILL); waitpid(p, NULL, 0); }
    daemon_gone();
}

/* ---- providers ---- */

static KindInfo *kind_info(const char *kind) {
    for (int i = 0; i < nkinfo; i++) if (!strcmp(kinfo[i].kind, kind)) return &kinfo[i];
    return NULL;
}

static int secret_name(const KindInfo *ki, const char *key) {
    if (!strcmp(key, "token") || !strcmp(key, "client_secret")) return 1;
    if (!ki || !ki->secrets_all) return !strcmp(key, "pass") || !strcmp(key, "password") || !strcmp(key, "key");
    size_t l = strlen(key);
    for (const char *p = ki->secrets_all; *p; ) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == l && !strncmp(p, key, n)) return 1;
        p += n + (e ? 1 : 0);
    }
    return 0;
}

static const char *const oauth_fields[] = { "client_id", "client_secret", "token", "auth_url", "token_url", "client_credentials", NULL };
static int is_oauth_field(const char *n) { for (int i = 0; oauth_fields[i]; i++) if (!strcmp(oauth_fields[i], n)) return 1; return 0; }

/* The kinds known in this session: rclone's own backends as it lists them (label = its Description) with the overlay of
 * cloud_pure.c merged in, and the variants. Entries are never removed, so an account's pointer stays good. */
#define DK_MAX 192
static CloudKind dk[DK_MAX];
static int ndk;
static char cl_assets[PATH_MAX];

static const char *keep_str(const char *old, const char *v) {
    if (!v) return old;
    if (old && !strcmp(old, v)) return old;
    char *d = strdup(v);
    return d ? d : old;             /* the old one may be in use by a reader: it is left, a few bytes */
}

static const CloudKind *kind_get(const char *kind) {
    for (int i = 0; kind && i < ndk; i++) if (!strcmp(dk[i].kind, kind)) return &dk[i];
    return cloud_kind(kind);
}

/* with cl_mu held, or before the threads start */
static const CloudKind *kind_put(const char *kind, const char *type, const char *label, const char *group) {
    CloudKind *k = NULL;
    for (int i = 0; i < ndk && !k; i++) if (!strcmp(dk[i].kind, kind)) k = &dk[i];
    if (!k) {
        if (ndk >= DK_MAX || !cloud_key_ok(kind)) return NULL;
        k = &dk[ndk++];
        const CloudKind *o = cloud_kind(kind);
        if (o) *k = *o;
        else memset(k, 0, sizeof *k);
        k->kind = keep_str(NULL, kind);
    }
    k->type = keep_str(k->type, type);
    k->label = keep_str(k->label, label);
    k->group = keep_str(k->group, group);
    if (!k->type) k->type = k->kind;
    if (!k->label) k->label = k->kind;
    if (!k->group) k->group = "server";
    return k;
}

void cloud_set_assets(const char *dir) { cpy(cl_assets, sizeof cl_assets, dir); }

/* assets/cloud/providers.json: the kinds tested on LordOS (docs/CLOUD.md 19) */
static JDoc verified_doc(int *ok) {
    JDoc d = { 0 };
    *ok = 0;
    char p[PATH_MAX + 32];
    if (!cl_assets[0]) return d;
    cl_pf(p, sizeof p, "%s/cloud/providers.json", cl_assets);
    size_t n;
    char *t = read_file(p, 1 << 20, &n);
    if (t && !json_parse(t, n, &d, NULL, 0)) *ok = 1;
    free(t);
    return d;
}

static void emit_kind(JBuf *b, int *first, const CloudKind *k, const JVal *pv, KindInfo *ki, int verified) {
    memset(ki, 0, sizeof *ki);
    cpy(ki->kind, sizeof ki->kind, k->kind);
    JBuf sa;
    jb_init(&sa);
    jb_raw(b, *first ? "{\"kind\":" : ",{\"kind\":");
    *first = 0;
    jb_str(b, k->kind);
    jb_raw(b, ",\"label\":");
    jb_str(b, k->label);
    jb_raw(b, ",\"description\":");
    jb_str(b, k->hint ? k->hint : "");
    jb_raw(b, ",\"hint\":");
    jb_str(b, k->hint ? k->hint : "");
    jb_raw(b, ",\"group\":");
    jb_str(b, k->group);
    jb_printf(b, ",\"verified\":%s", verified ? "true" : "false");
    if (k->website) { jb_raw(b, ",\"website\":"); jb_str(b, k->website); }
    jb_raw(b, ",\"fields\":[");
    int ff = 1, oauth = !strcmp(k->group, "oauth");
    const JVal *os = json_get(pv, "Options");
    for (const JVal *o = os && os->type == J_ARR ? os->kid : NULL; o; o = o->next) {
        const char *nm = json_str(o, "Name");
        if (!nm || !cloud_key_ok(nm)) continue;
        int secret = json_bool(o, "IsPassword", 0) || json_bool(o, "Sensitive", 0);
        if (secret) { if (sa.n) jb_raw(&sa, ","); jb_raw(&sa, nm); }
        if (json_bool(o, "Advanced", 0) || json_num(o, "Hide", 0) != 0 || cloud_field_hidden(k, nm) || (oauth && is_oauth_field(nm))) continue;
        /* 4.2: an option that runs a program or reads a local file is neither offered nor accepted (sftp's ssh, key_file…) */
        if (cloud_option_refused(nm, json_str(o, "Type"))) continue;
        if (oauth) continue;                            /* signing in asks rclone's own questions */
        if (ki->n >= 64) continue;
        cpy(ki->name[ki->n], sizeof ki->name[0], nm);
        ki->secret[ki->n] = (char)secret;
        ki->required[ki->n] = (char)json_bool(o, "Required", 0);
        ki->n++;
        const char *help = json_str(o, "Help"), *def = json_str(o, "DefaultStr");
        char label[96] = "";
        if (help) { size_t l = strcspn(help, "\n"); if (l >= sizeof label) l = sizeof label - 1; memcpy(label, help, l); label[l] = 0; }
        jb_raw(b, ff ? "{\"name\":" : ",{\"name\":");
        ff = 0;
        jb_str(b, nm);
        jb_raw(b, ",\"label\":");
        jb_str(b, label);
        jb_raw(b, ",\"help\":");
        jb_str(b, help ? help : "");
        jb_printf(b, ",\"secret\":%s,\"required\":%s,\"default\":", secret ? "true" : "false", json_bool(o, "Required", 0) ? "true" : "false");
        jb_str(b, secret ? "" : def ? def : "");
        /* (interface, 2026-11-06) only a password is masked: Sensitive (a host, a user) stays plain text; the type for switches */
        jb_printf(b, ",\"isPassword\":%s,\"type\":", json_bool(o, "IsPassword", 0) ? "true" : "false");
        jb_str(b, json_str(o, "Type") ? json_str(o, "Type") : "string");
        jb_raw(b, ",\"examples\":[");
        const JVal *ex = json_get(o, "Examples");
        int ne = 0;
        for (const JVal *e = ex && ex->type == J_ARR ? ex->kid : NULL; e && ne < 60; e = e->next) {
            const char *v = json_str(e, "Value");
            if (!v) continue;
            if (ne++) jb_raw(b, ",");
            jb_str(b, v);
        }
        jb_raw(b, "]}");
    }
    jb_raw(b, "]}");
    ki->secrets_all = sa.oom ? NULL : sa.p;
    if (sa.oom) jb_free(&sa);
}

/* rclone's backends, now (17.2 item 1): the list JS gets (malloc), and the fields add() accepts */
static char *load_providers(void) {
    JDoc d;
    if (rcj(RCM_PROVIDERS, "{}", &d, NULL, 0, RC_SHORT) != 200 || !d.root) { if (d.root) json_free(&d); return NULL; }
    const JVal *ps = json_get(d.root, "providers");
    int vok;
    JDoc vd = verified_doc(&vok);
    const JVal *ver = vok ? json_get(vd.root, "verified") : NULL;
    KindInfo *nk = calloc(KI_MAX, sizeof *nk);
    if (!nk) { json_free(&d); if (vok) json_free(&vd); return NULL; }
    int n = 0, first = 1;
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "[");
    for (const JVal *x = ps && ps->type == J_ARR ? ps->kid : NULL; x && n < KI_MAX; x = x->next) {
        const char *nm = json_str(x, "Name"), *pre = json_str(x, "Prefix"), *desc = json_str(x, "Description");
        if (!nm || !pre || !cloud_key_ok(pre) || cloud_excluded(nm) || cloud_excluded(pre) || json_bool(x, "Hide", 0)) continue;
        /* an alias of another backend (tardigrade is storj) */
        int alias = 0;
        for (const JVal *y = ps->kid; y && !alias; y = y->next) {
            const JVal *al = json_get(y, "Aliases");
            const char *yn = json_str(y, "Name");
            for (const JVal *z = al && al->type == J_ARR ? al->kid : NULL; z; z = z->next)
                if (z->type == J_STR && !strcmp(z->str, nm) && yn && strcmp(yn, nm)) alias = 1;
        }
        if (alias) continue;
        const char *opts[256];
        int no = 0;
        const JVal *os = json_get(x, "Options");
        for (const JVal *o = os && os->type == J_ARR ? os->kid : NULL; o && no < 256; o = o->next) { const char *on = json_str(o, "Name"); if (on) opts[no++] = on; }
        const CloudKind *ov = cloud_kind(pre);
        char label[160];
        cpy(label, sizeof label, desc ? desc : nm);
        label[strcspn(label, "\n")] = 0;
        pthread_mutex_lock(&cl_mu);
        const CloudKind *k = kind_put(pre, nm, label, ov && ov->group ? ov->group : cloud_derive_group(nm, opts, no));
        pthread_mutex_unlock(&cl_mu);
        if (!k) continue;
        emit_kind(&b, &first, k, x, &nk[n++], ver && json_get(ver, k->kind));
        /* our variants on this backend */
        for (int i = 0; i < cloud_kind_count() && n < KI_MAX; i++) {
            const CloudKind *v = cloud_kind_at(i);
            if (!v->variant || strcmp(v->type, nm)) continue;
            pthread_mutex_lock(&cl_mu);
            const CloudKind *vk = kind_put(v->kind, nm, v->label, v->group);
            pthread_mutex_unlock(&cl_mu);
            if (vk) emit_kind(&b, &first, vk, x, &nk[n++], ver && json_get(ver, vk->kind));
        }
    }
    jb_raw(&b, "]");
    json_free(&d);
    if (vok) json_free(&vd);
    if (b.oom) { jb_free(&b); for (int i = 0; i < n; i++) free(nk[i].secrets_all); free(nk); return NULL; }
    pthread_mutex_lock(&cl_mu);
    KindInfo *old = kinfo;
    int oldn = nkinfo;
    kinfo = nk;
    nkinfo = n;
    pthread_mutex_unlock(&cl_mu);
    for (int i = 0; old && i < oldn; i++) free(old[i].secrets_all);
    free(old);
    return b.p;
}

/* ---- one account in rclone ---- */

/* rclone's state machine of config/create, answered non-interactively. 0 done. */
/* {"question":{…}} for JS (17.2 item 3): the state carries the account's id, "<id>:<rclone's state>" */
static char *question_json(const char *id, const char *state, const JVal *o) {
    char st[600];
    cl_pf(st, sizeof st, "%s:%s", id, state);
    const char *def = json_str(o, "DefaultStr");
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"question\":{\"state\":");
    jb_str(&b, st);
    jb_raw(&b, ",\"name\":");
    jb_str(&b, json_str(o, "Name") ? json_str(o, "Name") : "");
    jb_raw(&b, ",\"help\":");
    jb_str(&b, json_str(o, "Help") ? json_str(o, "Help") : "");
    jb_raw(&b, ",\"type\":");
    jb_str(&b, json_str(o, "Type") ? json_str(o, "Type") : "string");
    int pw = json_bool(o, "IsPassword", 0) || json_bool(o, "Sensitive", 0);
    jb_printf(&b, ",\"isPassword\":%s,\"required\":%s,\"exclusive\":%s,\"default\":", pw ? "true" : "false",
              json_bool(o, "Required", 0) ? "true" : "false", json_bool(o, "Exclusive", 0) ? "true" : "false");
    jb_str(&b, pw ? "" : def ? def : "");
    jb_raw(&b, ",\"examples\":[");
    const JVal *ex = json_get(o, "Examples");
    int ne = 0;
    for (const JVal *e = ex && ex->type == J_ARR ? ex->kid : NULL; e && ne < 60; e = e->next) {
        const char *v = json_str(e, "Value"), *h = json_str(e, "Help");
        if (!v) continue;
        jb_raw(&b, ne++ ? ",{\"value\":" : "{\"value\":");
        jb_str(&b, v);
        jb_raw(&b, ",\"help\":");
        jb_str(&b, h ? h : "");
        jb_raw(&b, "}");
    }
    jb_raw(&b, "]}}");
    return b.oom ? (jb_free(&b), NULL) : b.p;
}

/* rclone's state machine of config/create, non-interactive. question NULL: every question answered here (the
 * default, or cloud_answer's); otherwise only LordOS's own ones, and the first other one goes back in *question (1).
 * 0 done, -1 error (msg).
 * #87: an error after a drive was chosen (OneDrive's driveid_final can't open it) sends rclone back to an earlier state
 * (ConfigError's State); that state is run again, as rclone's own config does after showing the error, and the drive
 * question gets one not tried yet (cloud_choose), CLOUD_CHOICE_RETRIES times at most. */
static int answer_states(const char *id, JDoc *d, char *msg, size_t mn, char **question) {
    CloudChoices ch;
    memset(&ch, 0, sizeof ch);
    for (int i = 0; i < 16 + 3 * CLOUD_CHOICE_RETRIES; i++) {
        const char *e = json_str(d->root, "Error"), *state = json_str(d->root, "State");
        const char *ans;
        if (e && e[0]) {
            if (!state || !state[0] || !cloud_choice_retry(&ch)) { cpy(msg, mn, e); return -1; }
            char clean[201];
            cloud_clean_text(e, clean, sizeof clean);
            fprintf(stderr, "cloud: %s: rclone said \"%s\": choosing again\n", id, clean);
            ans = "";
        } else {
            if (!state || !state[0]) return 0;
            const JVal *o = json_get(d->root, "Option");
            const char *nm = json_str(o, "Name"), *def = json_str(o, "ValueStr");
            if (question && !cloud_internal_question(nm)) {
                *question = question_json(id, state, o);
                return *question ? 1 : -1;
            }
            if (!def || !def[0]) def = json_str(o, "DefaultStr");
            ans = cloud_default_answer(nm, def);
            const JVal *ex = json_get(o, "Examples");
            const char *vals[CLOUD_CHOICES_MAX];
            int nv = 0;
            for (const JVal *x = ex && ex->type == J_ARR ? ex->kid : NULL; x && nv < CLOUD_CHOICES_MAX; x = x->next)
                if (json_str(x, "Value")) vals[nv++] = json_str(x, "Value");
            if (!ans[0] && nv) ans = vals[0];
            ans = cloud_choose(&ch, nm, ans, vals, nv);
        }
        JBuf b;
        jb_init(&b);
        jb_raw(&b, "{\"name\":");
        jb_str(&b, id);
        jb_raw(&b, ",\"parameters\":{},\"opt\":{\"nonInteractive\":true,\"continue\":true,\"obscure\":true,\"state\":");
        jb_str(&b, state);
        jb_raw(&b, ",\"result\":");
        jb_str(&b, ans);
        jb_raw(&b, "}}");
        json_free(d);
        int st = b.oom ? -1 : rcj(RCM_UPDATE, b.p, d, msg, mn, RC_LONG);
        jb_free(&b);
        if (st != 200 || !d->root) { if (d->root) json_free(d); d->root = NULL; return -1; }
    }
    return -1;
}

/* options rclone itself keeps that the refusal rule would catch by name: OAuth's client id and secret, the token */
static int keep_internal(const char *k) { return !strcmp(k, "token") || !strcmp(k, "client_id") || !strcmp(k, "client_secret"); }

/* config/create with the non-secret options, the preset and the secrets (a JSON object's text, or NULL).
 * fresh 1: a new account, secrets plain, the state machine answered; 2: the same for an existing account (Sign In
 * Again, new credentials: config/update, so what isn't given stays); 0: what config/get gave (obscured), at start. */
/* the server this account talks to, from its fields wherever they live (some are secret-flagged), kept for the state */
static void note_server(Acc *a, const char *secret_json) {
    static const char *const ks[] = { "host", "port", "url", "endpoint", "explicit_tls", "tls" };
    char v[6][512];
    JDoc d;
    int have = secret_json && secret_json[0] && !json_parse(secret_json, strlen(secret_json), &d, NULL, 0);
    for (int k = 0; k < 6; k++) {
        v[k][0] = 0;
        for (int i = 0; i < a->p.n; i++) if (!strcmp(a->p.kv[i].key, ks[k])) { cpy(v[k], sizeof v[k], a->p.kv[i].val); break; }
        const char *x = !v[k][0] && have ? json_str(d.root, ks[k]) : NULL;
        if (x) cpy(v[k], sizeof v[k], x);
    }
    if (have) json_free(&d);
    char h[256];
    int port = cloud_server_of(a->k->type, v[0], v[1], v[2], v[3], v[5][0] ? v[5] : NULL, h, sizeof h);
    explicit_bzero(v, sizeof v);
    if (port <= 0) return;                  /* not a server kind, or not in what was given: the kept one stays */
    pthread_mutex_lock(&cl_mu);
    cpy(a->srv_host, sizeof a->srv_host, h);
    a->srv_port = port;
    pthread_mutex_unlock(&cl_mu);
}

static int create_remote_q(Acc *a, const char *secret_json, int fresh, char *msg, size_t mn, char **question) {
    note_server(a, secret_json);
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"name\":");
    jb_str(&b, a->id);
    if (fresh != 2) { jb_raw(&b, ",\"type\":"); jb_str(&b, a->k->type); }
    jb_raw(&b, ",\"parameters\":{");
    int first = 1;
    if (a->k->preset) { jb_raw(&b, a->k->preset); first = 0; }
    for (int i = 0; i < a->p.n; i++) {
        if (cloud_field_hidden(a->k, a->p.kv[i].key)) continue;
        if (cloud_option_refused(a->p.kv[i].key, "string") && !keep_internal(a->p.kv[i].key)) continue;   /* even from a file */
        if (!first) jb_raw(&b, ",");
        first = 0;
        jb_str(&b, a->p.kv[i].key);
        jb_raw(&b, ":");
        jb_str(&b, a->p.kv[i].val);
    }
    JDoc sd;
    if (secret_json && !json_parse(secret_json, strlen(secret_json), &sd, NULL, 0)) {
        for (const JVal *x = sd.root->type == J_OBJ ? sd.root->kid : NULL; x; x = x->next) {
            if (x->type != J_STR || !cloud_key_ok(x->key)) continue;
            if (cloud_option_refused(x->key, "string") && !keep_internal(x->key)) continue;
            if (!first) jb_raw(&b, ",");
            first = 0;
            jb_str(&b, x->key);
            jb_raw(&b, ":");
            jb_str(&b, x->str);
        }
        json_free(&sd);
    }
    ct_params(a, &b, &first);                   /* its own known_hosts and accepted certificate, never from JS */
    jb_printf(&b, "},\"opt\":{\"nonInteractive\":true,%s}}", fresh ? "\"obscure\":true" : "\"noObscure\":true");
    JDoc d;
    int st = b.oom ? -1 : rcj(fresh == 2 ? RCM_UPDATE : RCM_CREATE, b.p, &d, msg, mn, RC_LONG);
    explicit_bzero(b.p, b.n);
    jb_free(&b);
    if (st != 200) { if (d.root) json_free(&d); return -1; }
    int r = 0;
    if (fresh && d.root) r = answer_states(a->id, &d, msg, mn, question);
    if (d.root) json_free(&d);
    return r;
}
static int create_remote(Acc *a, const char *secret_json, int fresh, char *msg, size_t mn) { return create_remote_q(a, secret_json, fresh, msg, mn, NULL); }

/* A key rclone gives back may stay in rclone.conf only when it is known not to be secret: it already was one of the
 * account's non-secret options, or the provider offers it as a field that isn't secret. Anything else (an option rclone
 * added itself, such as a session, or every key while the provider list isn't loaded) goes to the keyring, so a value
 * can only move from the file into the keyring, never the other way. */
static int known_plain(Acc *a, const KindInfo *ki, const char *key) {
    for (int i = 0; ki && i < ki->n; i++) if (!strcmp(ki->name[i], key)) return !ki->secret[i];
    pthread_mutex_lock(&cl_mu);
    int was = kv_get(&a->p, key) != NULL;
    pthread_mutex_unlock(&cl_mu);
    return was;
}

/* config/get: the non-secret options into a->p, the secrets as a JSON object's text (malloc) */
static char *split_remote(Acc *a, char *msg, size_t mn) {
    char body[64];
    cl_pf(body, sizeof body, "{\"name\":\"%s\"}", a->id);
    JDoc d;
    if (rcj(RCM_GET, body, &d, msg, mn, RC_SHORT) != 200 || !d.root) { if (d.root) json_free(&d); return NULL; }
    const KindInfo *ki = kind_info(a->k->kind);
    JBuf s;
    jb_init(&s);
    jb_raw(&s, "{");
    Params np = { .n = 0 };
    for (const JVal *x = d.root->type == J_OBJ ? d.root->kid : NULL; x; x = x->next) {
        if (x->type != J_STR || !strcmp(x->key, "type") || !cloud_key_ok(x->key)) continue;
        if (secret_name(ki, x->key) || !known_plain(a, ki, x->key)) {
            if (s.n > 1) jb_raw(&s, ",");
            jb_str(&s, x->key);
            jb_raw(&s, ":");
            jb_str(&s, x->str);
        } else if (!cloud_field_hidden(a->k, x->key)) kv_set(&np, x->key, x->str);
    }
    jb_raw(&s, "}");
    json_free(&d);
    pthread_mutex_lock(&cl_mu);
    kv_clear(&a->p);
    a->p = np;
    pthread_mutex_unlock(&cl_mu);
    if (s.oom) { jb_free(&s); return NULL; }
    return s.p;
}

static void keyring_label(const Acc *a, char *out, size_t n) { cl_pf(out, n, "Cloud: %s (%s)", a->name, a->k->label); }

/* saves the secrets; if the keyring can't take them now they wait in memory */
static void store_secret(Acc *a, char *sec) {
    char label[160];
    keyring_label(a, label, sizeof label);
    int r = secrets ? secrets->set(a->id, label, sec) : -1;
    pthread_mutex_lock(&cl_mu);
    a->tok_hash = fnv(sec);
    if (r) { wipe_free(a->pending_secret); a->pending_secret = sec; a->locked = r == -3 ? 2 : r == -2 ? 1 : 0; if (!a->locked) cpy(a->err, sizeof a->err, "cloud.keyringFailed"); }
    else { wipe_free(a->pending_secret); a->pending_secret = NULL; wipe_free(sec); if (!strcmp(a->err, "cloud.keyringFailed")) a->err[0] = 0; }
    pthread_mutex_unlock(&cl_mu);
}

static void check_links(Acc *a) {
    char body[64];
    cl_pf(body, sizeof body, "{\"fs\":\"%s:\"}", a->id);
    JDoc d;
    if (rcj(RCM_FSINFO, body, &d, NULL, 0, RC_SHORT) == 200 && d.root) {
        const JVal *f = json_get(d.root, "Features");
        a->links_supported = json_bool(f, "PublicLink", 0);
        /* S3's CleanUp removes unfinished multipart uploads: not a trash to empty */
        a->trash_supported = json_bool(f, "CleanUp", 0) && !presigned(a);
        /* nanoseconds; rclone's "not supported" is a huge number: modtimes then don't count */
        double pr = json_num(d.root, "Precision", 1e9);
        a->precision_s = pr >= 1e17 ? 0 : pr <= 1e9 ? 1 : (int)(pr / 1e9 + 0.5);
        /* the provider tells rclone what changed (Google Drive, OneDrive, Dropbox, Box, pCloud…): listings stay
         * current without listing again (7.3) */
        int polls = json_bool(f, "ChangeNotify", 0);
        pthread_mutex_lock(&cl_mu);
        a->polls = polls;
        pthread_mutex_unlock(&cl_mu);
    }
    if (d.root) json_free(&d);
}

static void about_acc(Acc *a) {
    char body[64], msg[256];
    cl_pf(body, sizeof body, "{\"fs\":\"%s:\"}", a->id);
    JDoc d;
    int st = rcj(RCM_ABOUT, body, &d, msg, sizeof msg, RC_LONG);
    pthread_mutex_lock(&cl_mu);
    a->about_at = time(NULL);
    if (st == 200 && d.root) {
        static const char *const k[5] = { "total", "used", "free", "trashed", "other" };
        for (int i = 0; i < 5; i++) a->ab[i] = json_num(d.root, k[i], -1);
        a->have_about = 1;
        if (!strcmp(a->err, "cloud.offline") || !strcmp(a->err, "cloud.authFailed") || !strcmp(a->err, "cloud.signInAgain")) { a->err[0] = 0; a->message[0] = 0; }
    } else if (st > 0 && strstr(msg, "doesn't support about")) {
        a->have_about = 0;
    } else if (st != 200) {
        cpy(a->err, sizeof a->err, code_of(msg, !strcmp(a->k->group, "oauth")));
        if (!strcmp(a->err, "cloud.failed")) a->err[0] = 0;   /* about isn't worth an error state */
        else set_message(a, msg);
    }
    pthread_mutex_unlock(&cl_mu);
    if (d.root) json_free(&d);
    cl_changed();
}

/* The provider refused the saved session (rclone issue 9658: iCloud Drive, with or without Advanced Data Protection, after
 * a restart of rclone: "requestPCS … Missing X-APPLE-WEBAUTH-TOKEN cookie"). rclone reuses the saved cookies and never
 * signs in again on that error; without them it signs in with the password and the trust token, which asks for no code
 * while the token lasts (about 30 days), and saves a new session. So once per run, the cookies are dropped (the trust
 * token kept) and the mount is tried again. 1 if the account was updated. */
static int drop_session(Acc *a, const char *msg) {
    if (a->fresh_tried || strcmp(a->k->type, "iclouddrive") || !strstr(msg, "requestPCS")) return 0;
    a->fresh_tried = 1;
    char body[160];
    cl_pf(body, sizeof body, "{\"name\":\"%s\",\"parameters\":{\"cookies\":\"\"},\"opt\":{\"nonInteractive\":true,\"noObscure\":true}}", a->id);
    char m2[256] = "";
    JDoc d;
    int st = rcj(RCM_UPDATE, body, &d, m2, sizeof m2, RC_LONG);
    /* a question back (a code is needed after all): the person signs in again, as before */
    int question = d.root && json_str(d.root, "State") && json_str(d.root, "State")[0];
    if (d.root) json_free(&d);
    fprintf(stderr, "cloud: %s: the provider refused its saved session; signing in again with the kept trust token: %s\n",
            a->id, st == 200 && !question ? "done" : "a new sign-in is needed");
    return st == 200 && !question;
}

static int mount_acc(Acc *a) {
    char dir[PATH_MAX], msg[256];
    mkdirs(cl_root, 0700);
    mount_dir(a, dir, sizeof dir);
    clear_stale(dir);
    mkdir(dir, 0700);
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"fs\":\"%s:\",\"mountPoint\":", a->id);
    jb_str(&b, dir);
    /* what stays, uploads, and how long a listing is kept (cloud_mount_opts, 7.2 and 7.3) */
    char o[320];
    if (cloud_mount_opts(o, sizeof o)) b.oom = 1;
    jb_raw(&b, o);
    jb_raw(&b, "}");
    int st = b.oom ? -1 : rcs(RCM_MOUNT, b.p, msg, sizeof msg, RC_SHORT);
    jb_free(&b);
    pthread_mutex_lock(&cl_mu);
    if (st == 200) { a->mounted = 1; a->mount_gen++; a->err[0] = 0; a->message[0] = 0; a->check_at = 0; }
    else {
        a->mounted = 0;
        const char *fc = cloud_failure_code(msg);
        cpy(a->err, sizeof a->err, !strcmp(code_of(msg, 0), "cloud.folderNotEmpty") ? "cloud.folderNotEmpty" : fc ? fc : "cloud.mountFailed");
        set_message(a, msg);
        /* refused credentials don't come back by themselves: tried again in 30 minutes (or at once with Sign In Again,
         * Retry or a restart), not every 30 seconds, which some providers answer by locking the account */
        if (!strcmp(a->err, "cloud.signInAgain") || !strcmp(a->err, "cloud.authFailed")) a->retry_at = time(NULL) + 1800;
    }
    pthread_mutex_unlock(&cl_mu);
    if (st == 200) { check_links(a); about_acc(a); cf_kick(); }
    else if (drop_session(a, msg)) { token_check(a); return mount_acc(a); }   /* once: fresh_tried */
    cl_changed();
    return st == 200 ? 0 : -1;
}

static int mount_acc(Acc *a);
static void bring_up(Acc *a);
/* an account that isn't in this rclone yet (keyring locked, or it failed) before new credentials are merged in */
static void bring_up_config(Acc *a) {
    char *sec = NULL;
    int r = secrets ? secrets->get(a->id, &sec) : 1;
    char msg[256];
    if (!create_remote(a, r == 0 ? sec : NULL, 0, msg, sizeof msg)) { pthread_mutex_lock(&cl_mu); a->configured = 1; a->locked = 0; pthread_mutex_unlock(&cl_mu); }
    wipe_free(sec);
}

/* brings an account up in this rclone: secrets from the keyring, config/create, mount */
static void bring_up(Acc *a) {
    a->retry_at = time(NULL) + 30;
    if (!cl_running || a->configured) { if (a->configured && !a->mounted) mount_acc(a); return; }
    char *sec = NULL;
    int r;
    pthread_mutex_lock(&cl_mu);
    if (a->pending_secret) { sec = strdup(a->pending_secret); r = sec ? 0 : -1; }
    else r = 2;
    pthread_mutex_unlock(&cl_mu);
    if (r == 2) r = secrets ? secrets->get(a->id, &sec) : 1;
    pthread_mutex_lock(&cl_mu);
    a->retry_at = time(NULL) + 30;
    /* locked: the keyring's unlock event (cloud_kr.c) or cloud.retry() tries again; this is only a slow fallback */
    if (r == -2 || r == -3) { a->locked = r == -2 ? 1 : 2; a->retry_at = time(NULL) + 600; pthread_mutex_unlock(&cl_mu); cl_changed(); return; }
    a->locked = 0;
    if (r == -1) { cpy(a->err, sizeof a->err, "cloud.keyringFailed"); pthread_mutex_unlock(&cl_mu); cl_changed(); return; }
    pthread_mutex_unlock(&cl_mu);
    char msg[256];
    int ok = !create_remote(a, r == 0 ? sec : NULL, 0, msg, sizeof msg);
    pthread_mutex_lock(&cl_mu);
    if (ok) { a->configured = 1; if (sec) a->tok_hash = fnv(sec); }
    else { cpy(a->err, sizeof a->err, "cloud.failed"); set_message(a, msg); }
    pthread_mutex_unlock(&cl_mu);
    wipe_free(sec);
    if (ok) mount_acc(a);
    else cl_changed();
}

/* What rclone changed in an account's secrets, in memory, goes to the keyring: an OAuth token it refreshed, and also
 * the session a password provider keeps (iCloud's cookies and trust token, updated after the code from a device and
 * on later sign-ins). Only OAuth accounts were read back, so an iCloud account came up after a restart of rclone (a
 * logout, a reboot) with the session of its first sign-in, which the provider refuses ("Missing
 * X-APPLE-WEBAUTH-TOKEN cookie"): the person had to sign in again with a code every time. */
static void token_check(Acc *a) {
    char msg[128];
    a->tok_at = time(NULL);
    if (!a->configured) return;
    char *sec = split_remote(a, msg, sizeof msg);        /* a->p stays as it was if config/get fails */
    if (!sec) return;
    if (fnv(sec) == a->tok_hash) { wipe_free(sec); return; }
    store_secret(a, sec);
    pthread_mutex_lock(&cl_mu);
    save_accounts();
    pthread_mutex_unlock(&cl_mu);
}

static int ensure_daemon(void) {
    if (cl_running) return 0;
    dmn.idle_since = time(NULL);
    if (daemon_start()) { cl_changed(); return -1; }
    return 0;
}

/* ---- the jobs ---- */

static void push_job(Job *j) {
    pthread_mutex_lock(&cl_mu);
    if (jtail) jtail->next = j; else jhead = j;
    jtail = j;
    pthread_mutex_unlock(&cl_mu);
    wake(efd_job);
}

static Job *new_job(int type, const char *id, const char *a, const char *b) {
    Job *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->type = type;
    cpy(j->id, sizeof j->id, id);
    j->a = a ? strdup(a) : NULL;
    j->b = b ? strdup(b) : NULL;
    return j;
}

static void post_reply(int req, char *json) {
    Reply *r = calloc(1, sizeof *r);
    if (!r) { free(json); return; }
    r->req = req;
    r->json = json;
    pthread_mutex_lock(&cl_mu);
    r->next = rhead;
    rhead = r;
    pthread_mutex_unlock(&cl_mu);
    wake(efd_push);
}

static char *err_json(const char *code) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"error\":");
    jb_str(&b, code);
    jb_raw(&b, "}");
    return b.oom ? (jb_free(&b), NULL) : b.p;
}

/* a new account's options have been given to rclone: secrets to the keyring, the files, the mount */
static void finish_new(Acc *a) {
    char msg[256];
    char *sec = split_remote(a, msg, sizeof msg);
    if (sec) store_secret(a, sec);
    pthread_mutex_lock(&cl_mu);
    a->configured = 1;
    save_accounts();
    pthread_mutex_unlock(&cl_mu);
    mount_acc(a);
}

static const char *add_field;       /* the field an add error is about, for the next err_msg_json (the thread only) */
static char *err_msg_json(const char *code, const char *msg) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"error\":");
    jb_str(&b, code);
    if (add_field) { jb_raw(&b, ",\"field\":"); jb_str(&b, add_field); add_field = NULL; }
    if (msg && msg[0]) { char clean[201]; cloud_clean_text(msg, clean, sizeof clean); jb_raw(&b, ",\"message\":"); jb_str(&b, clean); }
    jb_raw(&b, "}");
    return b.oom ? (jb_free(&b), NULL) : b.p;
}

/* the setup of an account didn't take: a new one goes away, an existing one stays as it was with the error */
static void add_failed(Acc *a, int req, const char *code, const char *msg) {
    /* which field the person should look at */
    if (!strcmp(code, "cloud.badAddress") || !strcmp(code, "cloud.unreachable") || !strcmp(code, "cloud.certUntrusted"))
        add_field = kv_get(&a->p, "url") ? "url" : kv_get(&a->p, "endpoint") ? "endpoint" : kv_get(&a->p, "host") ? "host" : NULL;
    else if (!strcmp(code, "cloud.authFailed"))
        add_field = kv_get(&a->p, "key_pem") ? "key_pem" : kv_get(&a->p, "user") ? "pass" : NULL;
    if (req) post_reply(req, err_msg_json(code, msg));
    add_field = NULL;               /* only for the reply above */
    pthread_mutex_lock(&cl_mu);
    wipe_free(a->pend_sec);
    wipe_free(a->pend_tsec);
    a->pend_sec = a->pend_tsec = NULL;
    pthread_mutex_unlock(&cl_mu);
    if (a->new_acc) {
        ct_forget(a);
        char body[64];
        cl_pf(body, sizeof body, "{\"name\":\"%s\"}", a->id);
        if (cl_running) rcs(RCM_DELETE, body, NULL, 0, RC_SHORT);
        pthread_mutex_lock(&cl_mu);
        acc_free(a);
        pthread_mutex_unlock(&cl_mu);
    } else {
        /* as it was: the credentials in the keyring (the new ones are only saved once they work), mounted again; the
         * error says the new settings didn't take */
        if (cl_running) bring_up_config(a);
        if (!a->mounted && cl_running) mount_acc(a);
        pthread_mutex_lock(&cl_mu);
        cpy(a->err, sizeof a->err, code);
        set_message(a, msg);
        a->pending_req = 0;
        pthread_mutex_unlock(&cl_mu);
    }
    cl_changed();
}

/* lists the account's top folder (folders only, no times): 0 it answered, -1 and rclone's words */
/* B18: rclone's FTP and SMB backends keep a pooled connection the server closed when it stopped, and fail with it
 * ("broken pipe") long after the server is back; its cached remote is dropped so the next call connects anew */
static int stale_pool(Acc *a, const char *msg) {
    if (strcmp(a->k->type, "ftp") && strcmp(a->k->type, "smb")) return 0;
    const char *fc = cloud_failure_code(msg);
    if (!fc || strcmp(fc, "cloud.unreachable")) return 0;
    char m2[64];
    return rcs(RCM_FSCACHE_CLEAR, "{}", m2, sizeof m2, 5000) == 200;
}

static int verify_remote(Acc *a, char *msg, size_t mn) {
    char body[160];
    cl_pf(body, sizeof body, "{\"fs\":\"%s:\",\"remote\":\"\",\"opt\":{\"dirsOnly\":true,\"noModTime\":true,\"noMimeType\":true}}", a->id);
    int st = rcs(RCM_LIST, body, msg, mn, 20000);
    if (st != 200 && st > 0 && stale_pool(a, msg)) st = rcs(RCM_LIST, body, msg, mn, 20000);
    if (st == 200) return 0;
    if (st < 0) cpy(msg, mn, "i/o timeout: no answer within 20 s");
    return -1;
}

static int listing_code(const char *e) {
    static const char *const c[] = { "cloud.authFailed", "cloud.unreachable", "cloud.badAddress", "cloud.certUntrusted", "cloud.hostKeyChanged",
                                     "cloud.offline", "cloud.signInAgain", "cloud.listFailed", NULL };
    for (int i = 0; c[i]; i++) if (!strcmp(e, c[i])) return 1;
    return 0;
}

static void check_acc(Acc *a) {
    char msg[256] = "";
    int bad = verify_remote(a, msg, sizeof msg);
    const char *c = bad ? cloud_failure_code(msg) : NULL;
    pthread_mutex_lock(&cl_mu);
    /* offline: listed every 30 s, so it comes back as soon as the server does */
    a->check_at = time(NULL) + (!bad ? 600 : c && !strcmp(c, "cloud.unreachable") ? 30 : 120);
    if (bad) {
        cpy(a->err, sizeof a->err, c ? c : "cloud.listFailed");
        if (!strcmp(a->err, "cloud.authFailed") && !strcmp(a->k->group, "oauth")) cpy(a->err, sizeof a->err, "cloud.signInAgain");
        set_message(a, msg);
    } else if (listing_code(a->err)) { a->err[0] = 0; a->message[0] = 0; a->fp_old[0] = a->fp_new[0] = 0; }
    int key = bad && c && !strcmp(c, "cloud.hostKeyChanged") && !a->fp_new[0];
    pthread_mutex_unlock(&cl_mu);
    /* U12: a key that changed, seen by rclone: the kept fingerprint and, once, the server's new one */
    if (key) {
        char o[256] = "", n[256] = "", *ks = NULL;
        if (secrets && secrets->get(a->id, &ks)) ks = NULL;        /* its host and port live with its secrets */
        ct_fingerprints(a, ks, o, sizeof o, n, sizeof n);
        wipe_free(ks);
        pthread_mutex_lock(&cl_mu);
        cpy(a->fp_old, sizeof a->fp_old, o);
        cpy(a->fp_new, sizeof a->fp_new, n[0] ? n : "?");
        pthread_mutex_unlock(&cl_mu);
    }
    cl_changed();
}

/* after config/create or an answer: done (r 0), rclone asks (r 1, q), or failed */
static void add_step(Acc *a, int req, int r, char *q, const char *msg) {
    if (r == 1) {
        pthread_mutex_lock(&cl_mu);
        a->pending_req = 1;
        pthread_mutex_unlock(&cl_mu);
        if (req) post_reply(req, q); else free(q);
        cl_changed();
        return;
    }
    free(q);
    if (r < 0) {
        const char *c = cl_running ? code_of(msg, 0) : "cloud.notRunning";
        add_failed(a, req, !strcmp(c, "cloud.failed") ? "cloud.addFailed" : c, msg);
        return;
    }
    /* B4: before it counts, the account must answer: its top folder listed (rclone connects and signs in there) */
    char vmsg[256] = "";
    if (verify_remote(a, vmsg, sizeof vmsg)) {
        const char *c = cloud_failure_code(vmsg);
        int sftp = !strcmp(a->k->type, "sftp");
        if (sftp && c && !strcmp(c, "cloud.hostKeyChanged")) {
            /* rclone's connection found another key than the kept one: B15 and N1 */
            char code[64], msg2[2100], *q2 = NULL;
            int t = ct_key_mismatch(a, a->pend_tsec ? a->pend_tsec : a->pend_sec, &q2, code, sizeof code, msg2, sizeof msg2);
            if (t == 1) { wait_answer(a, req, q2, a->pend_sec, a->pend_tsec, a->pend_upd); return; }
            changed_failed(a, req, code, msg2);
            return;
        }
        /* B12: a reset right after a key probe is the server's penalty for the probe */
        if (sftp && ct_probed_lately(a) && (strstr(vmsg, "connection reset") || strstr(vmsg, "handshake failed: EOF") || (c && !strcmp(c, "cloud.unreachable"))))
            c = "cloud.tooManyAttempts";
        add_failed(a, req, c ? c : "cloud.addFailed", vmsg);
        return;
    }
    pthread_mutex_lock(&cl_mu);
    a->pending_req = 0;
    a->new_acc = 0;
    a->err[0] = a->message[0] = 0;
    a->fp_old[0] = a->fp_new[0] = 0;
    wipe_free(a->pend_sec);
    wipe_free(a->pend_tsec);
    a->pend_sec = a->pend_tsec = NULL;
    pthread_mutex_unlock(&cl_mu);
    finish_new(a);
    if (req) {
        char b[64];
        cl_pf(b, sizeof b, "{\"id\":\"%s\"}", a->id);
        post_reply(req, strdup(b));
    }
}

/* a key or certificate that changed: { error, old, new } (msg is "old\tnew"), and both kept for the state (U10) */
static void changed_failed(Acc *a, int req, const char *code, char *msg) {
    char *tab = strchr(msg, '\t');
    if (tab) *tab = 0;
    pthread_mutex_lock(&cl_mu);
    cpy(a->fp_old, sizeof a->fp_old, msg);
    cpy(a->fp_new, sizeof a->fp_new, tab ? tab + 1 : "");
    pthread_mutex_unlock(&cl_mu);
    JBuf e;
    jb_init(&e);
    jb_raw(&e, "{\"error\":");
    jb_str(&e, code);
    jb_raw(&e, ",\"old\":");
    jb_str(&e, msg);
    jb_raw(&e, ",\"new\":");
    jb_str(&e, tab ? tab + 1 : "");
    jb_raw(&e, "}");
    if (req) post_reply(req, e.oom ? (jb_free(&e), NULL) : e.p); else jb_free(&e);
    add_failed(a, 0, code, "");
}

/* a question waits: what add was given stays (wiped after) to go on with once it is answered */
static void wait_answer(Acc *a, int req, char *q, const char *sec, const char *tsec, int upd) {
    pthread_mutex_lock(&cl_mu);
    if (sec != a->pend_sec) { wipe_free(a->pend_sec); a->pend_sec = sec ? strdup(sec) : NULL; }
    if (tsec != a->pend_tsec) { wipe_free(a->pend_tsec); a->pend_tsec = tsec ? strdup(tsec) : NULL; }
    a->pend_upd = upd;
    a->pending_req = 1;
    a->pending_since = time(NULL);
    pthread_mutex_unlock(&cl_mu);
    if (req) post_reply(req, q); else free(q);
    cl_changed();
}

/* who the server is (cloud_trust.c), then config/create: a question for the person, or on to add_step. sec: the
 * secrets given with add; tsec: those over the account's kept ones (an update may leave the host empty, B15), for
 * the checks only */
static void add_continue(Acc *a, int req, const char *sec, const char *tsec, int upd) {
    char msg[256] = "", code[64] = "", *q = NULL;
    if (!tsec) tsec = sec;
    if (ct_needed(a, tsec)) {
        int lately = !strcmp(a->k->type, "sftp") && ct_probed_lately(a);
        int t = ct_check(a, tsec, &q, code, sizeof code, msg, sizeof msg);
        if (t == 1) { wait_answer(a, req, q, sec, tsec, upd); return; }
        if (t < 0) {
            if (!strcmp(code, "cloud.hostKeyChanged") || !strcmp(code, "cloud.certChanged")) { changed_failed(a, req, code, msg); return; }
            /* B12: the server stopped answering right after an earlier try: it is refusing this computer for a while */
            if (lately && !strcmp(code, "cloud.unreachable")) cpy(code, sizeof code, "cloud.tooManyAttempts");
            add_failed(a, req, code[0] ? code : "cloud.addFailed", msg);
            return;
        }
    }
    /* kept while rclone sets it up: a key mismatch found by its listing is asked about with them (add_step) */
    pthread_mutex_lock(&cl_mu);
    if (sec != a->pend_sec) { wipe_free(a->pend_sec); a->pend_sec = sec ? strdup(sec) : NULL; }
    if (tsec != a->pend_tsec) { wipe_free(a->pend_tsec); a->pend_tsec = tsec ? strdup(tsec) : NULL; }
    a->pend_upd = upd;
    pthread_mutex_unlock(&cl_mu);
    int r = create_remote_q(a, sec, upd ? 2 : 1, msg, sizeof msg, &q);
    add_step(a, req, r, q, msg);
}

/* B15: an update may give only the password: the checks need the host, port and user rclone keeps as secrets */
static char *merged_secrets(Acc *a, const char *given) {
    char *old = NULL;
    if (!secrets || secrets->get(a->id, &old) || !old) return NULL;
    JDoc o, g;
    int ok_o = !json_parse(old, strlen(old), &o, NULL, 0), ok_g = given && !json_parse(given, strlen(given), &g, NULL, 0);
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{");
    int first = 1;
    for (const JVal *x = ok_o && o.root->type == J_OBJ ? o.root->kid : NULL; x; x = x->next) {
        if (x->type != J_STR || (ok_g && json_get(g.root, x->key))) continue;
        if (!first) jb_raw(&b, ",");
        first = 0;
        jb_str(&b, x->key); jb_raw(&b, ":"); jb_str(&b, x->str);
    }
    for (const JVal *x = ok_g && g.root->type == J_OBJ ? g.root->kid : NULL; x; x = x->next) {
        if (x->type != J_STR) continue;
        if (!first) jb_raw(&b, ",");
        first = 0;
        jb_str(&b, x->key); jb_raw(&b, ":"); jb_str(&b, x->str);
    }
    jb_raw(&b, "}");
    if (ok_o) json_free(&o);
    if (ok_g) json_free(&g);
    wipe_free(old);
    if (b.oom) { jb_free(&b); return NULL; }
    return b.p;
}

static void job_add(Job *j) {
    Acc *a;
    pthread_mutex_lock(&cl_mu);
    a = cl_find(j->id);
    pthread_mutex_unlock(&cl_mu);
    if (!a) { if (j->req) post_reply(j->req, err_json("cloud.notFound")); return; }
    int upd = j->flag;
    if (ensure_daemon()) { add_failed(a, j->req, "cloud.notRunning", ""); return; }
    if (upd) {
        /* new credentials for an existing account: unmounted, merged into its remote, mounted again */
        if (a->mounted) unmount_acc(a);
        pthread_mutex_lock(&cl_mu);
        a->err[0] = a->message[0] = 0;
        pthread_mutex_unlock(&cl_mu);
        if (!a->configured) bring_up_config(a);
    }
    char *tsec = upd ? merged_secrets(a, j->a) : NULL;
    add_continue(a, j->req, j->a, tsec, upd);
    wipe_free(tsec);
}

/* the answer to one of rclone's questions (17.2 item 3): config/update with continue, state and result */
static void job_answer(Job *j) {
    Acc *a;
    pthread_mutex_lock(&cl_mu);
    a = cl_find(j->id);
    int waiting = a && a->pending_req;
    pthread_mutex_unlock(&cl_mu);
    if (!a || !waiting || !cl_running) { post_reply(j->req, err_json(!a ? "cloud.notFound" : !cl_running ? "cloud.notRunning" : "cloud.noQuestion")); return; }
    if (!strcmp(j->a, "lordos-trust")) {
        /* a host key or a certificate the person was shown (cloud_trust.c) */
        int yes = j->b && (!strcmp(j->b, "trust") || !strcmp(j->b, "accept") || !strcmp(j->b, "yes") || !strcmp(j->b, "true"));
        pthread_mutex_lock(&cl_mu);
        char *sec = a->pend_sec, *tsec = a->pend_tsec;
        int upd = a->pend_upd;
        a->pend_sec = a->pend_tsec = NULL;
        a->pending_req = 0;
        pthread_mutex_unlock(&cl_mu);
        if (!yes || !a->trust_kind) { ct_refuse(a); wipe_free(sec); wipe_free(tsec); add_failed(a, j->req, yes ? "cloud.noQuestion" : "cloud.notTrusted", ""); return; }
        if (ct_accept(a, j->flag != 2)) { wipe_free(sec); wipe_free(tsec); add_failed(a, j->req, "cloud.addFailed", "could not keep the key or certificate"); return; }
        if (upd && a->mounted) unmount_acc(a);
        add_continue(a, j->req, sec, tsec, upd);
        wipe_free(sec);
        wipe_free(tsec);
        return;
    }
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"name\":");
    jb_str(&b, a->id);
    jb_raw(&b, ",\"parameters\":{},\"opt\":{\"nonInteractive\":true,\"continue\":true,\"obscure\":true,\"state\":");
    jb_str(&b, j->a);
    jb_raw(&b, ",\"result\":");
    jb_str(&b, j->b ? j->b : "");
    jb_raw(&b, "}}");
    char msg[256] = "", *q = NULL;
    JDoc d;
    int st = b.oom ? -1 : rcj(RCM_UPDATE, b.p, &d, msg, sizeof msg, RC_LONG);
    explicit_bzero(b.p, b.n);
    jb_free(&b);
    int r = -1;
    if (st == 200 && d.root) r = answer_states(a->id, &d, msg, sizeof msg, &q);
    if (d.root) json_free(&d);
    add_step(a, j->req, r, q, msg);
}

static void remove_acc_files(Acc *a) {
    char p[PATH_MAX];
    cl_pf(p, sizeof p, "%s/vfs/%s", cl_cache, a->id);
    cf_rm_tree(p);
    cl_pf(p, sizeof p, "%s/vfsMeta/%s", cl_cache, a->id);
    cf_rm_tree(p);
    mount_dir(a, p, sizeof p);
    rmdir(p);
}

static void job_remove(Job *j) {
    Acc *a;
    pthread_mutex_lock(&cl_mu);
    a = cl_find(j->id);
    pthread_mutex_unlock(&cl_mu);
    if (!a) return;
    if (cl_running) {
        unmount_acc(a);
        char body[64];
        cl_pf(body, sizeof body, "{\"name\":\"%s\"}", a->id);
        if (a->configured) rcs(RCM_DELETE, body, NULL, 0, RC_SHORT);
    }
    remove_acc_files(a);
    ct_forget(a);
    if (secrets) secrets->del(a->id);
    pthread_mutex_lock(&cl_mu);
    for (Link **l = &links; *l; ) {
        if (!strcmp((*l)->id, a->id)) { Link *x = *l; *l = x->next; free(x->rel); free(x->url); free(x); }
        else l = &(*l)->next;
    }
    acc_free(a);
    save_accounts();
    cl_save_keep();
    save_links();
    pthread_mutex_unlock(&cl_mu);
    cl_changed();
}

static void job_rename(Job *j) {
    Acc *a;
    pthread_mutex_lock(&cl_mu);
    a = cl_find(j->id);
    pthread_mutex_unlock(&cl_mu);
    if (!a || !j->a) return;
    char from[PATH_MAX], to[PATH_MAX];
    int was = a->mounted;
    if (was) unmount_acc(a);
    mount_dir(a, from, sizeof from);
    cl_pf(to, sizeof to, "%s/%s", cl_root, j->a);
    /* the old folder is unmounted (lazily if a program keeps a file open there); if it can't be moved, a new folder is
     * made and the old one removed when it is empty, so the account never stays at its old name */
    int moved = !rename(from, to) || errno == ENOENT;
    if (!moved) { mkdir(to, 0700); rmdir(from); struct stat st; moved = !stat(to, &st) && S_ISDIR(st.st_mode); }
    if (!moved) {
        pthread_mutex_lock(&cl_mu);
        cpy(a->err, sizeof a->err, "cloud.renameFailed");
        pthread_mutex_unlock(&cl_mu);
    } else {
        pthread_mutex_lock(&cl_mu);
        cpy(a->name, sizeof a->name, j->a);
        save_accounts();
        pthread_mutex_unlock(&cl_mu);
    }
    if (was) mount_acc(a);
    cl_changed();
}

/* backends whose links are presigned addresses, which can't be revoked, and whose CleanUp isn't a trash */
static int presigned(const Acc *a) {
    static const char *const t[] = { "s3", "oracleobjectstorage", "qingstor", NULL };
    for (int i = 0; t[i]; i++) if (!strcmp(a->k->type, t[i])) return 1;
    return 0;
}

static void job_link(Job *j, int unlink) {
    Acc *a;
    pthread_mutex_lock(&cl_mu);
    a = cl_find(j->id);
    pthread_mutex_unlock(&cl_mu);
    if (!a || !cl_running || !a->configured) { post_reply(j->req, err_json(!a ? "cloud.notFound" : unavailable(a))); return; }
    char arg[16] = "", msg[256];
    long long secs = 0;
    if (!unlink && cloud_expire(j->b, arg, sizeof arg, &secs)) { post_reply(j->req, err_json("cloud.badExpire")); return; }
    /* B17: a presigned link lasts at most 7 days, whatever is asked: that is what is kept and answered */
    if (!unlink && presigned(a) && (!secs || secs > 7 * 86400)) { secs = 7 * 86400; cpy(arg, sizeof arg, "7d"); }
    if (unlink && presigned(a)) {
        /* B8: an S3 link is a signed address: nothing on the server can take it back before it expires. It leaves the
         * list, and the answer says when it stops working. */
        long long exp = -1;
        pthread_mutex_lock(&cl_mu);
        for (Link **l = &links; *l; ) {
            if (!strcmp((*l)->id, a->id) && !strcmp((*l)->rel, j->a)) { Link *x = *l; exp = x->expires; *l = x->next; free(x->rel); free(x->url); free(x); }
            else l = &(*l)->next;
        }
        save_links();
        pthread_mutex_unlock(&cl_mu);
        char b[128];
        if (exp > 0) cl_pf(b, sizeof b, "{\"error\":\"cloud.cantRevoke\",\"expires\":%lld}", exp);
        else cl_pf(b, sizeof b, "{\"error\":\"cloud.cantRevoke\",\"expires\":null}");
        post_reply(j->req, strdup(b));
        cl_changed();
        return;
    }
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"fs\":\"%s:\",\"remote\":", a->id);
    jb_str(&b, j->a);
    if (unlink) jb_raw(&b, ",\"unlink\":true");
    else if (arg[0]) { jb_raw(&b, ",\"expire\":"); jb_str(&b, arg); }
    jb_raw(&b, "}");
    JDoc d;
    int st = b.oom ? -1 : rcj(RCM_PUBLICLINK, b.p, &d, msg, sizeof msg, RC_LONG);
    jb_free(&b);
    const char *url = st == 200 && d.root ? json_str(d.root, "url") : NULL;
    char *res;
    if (st != 200) {
        int uns = strstr(msg, "support") != NULL || strstr(msg, "not implemented") != NULL;
        res = err_json(uns ? "unsupported" : "cloud.linkFailed");
        if (uns) { pthread_mutex_lock(&cl_mu); a->links_supported = 0; pthread_mutex_unlock(&cl_mu); }
    } else if (unlink) {
        pthread_mutex_lock(&cl_mu);
        for (Link **l = &links; *l; ) {
            if (!strcmp((*l)->id, a->id) && !strcmp((*l)->rel, j->a)) { Link *x = *l; *l = x->next; free(x->rel); free(x->url); free(x); }
            else l = &(*l)->next;
        }
        save_links();
        pthread_mutex_unlock(&cl_mu);
        res = strdup("{\"ok\":true}");
    } else if (!url || !cloud_url_ok(url)) {
        res = err_json("cloud.linkFailed");
    } else {
        Link *l = calloc(1, sizeof *l);
        pthread_mutex_lock(&cl_mu);
        for (Link **p = &links; *p; ) {
            if (!strcmp((*p)->id, a->id) && !strcmp((*p)->rel, j->a)) { Link *x = *p; *p = x->next; free(x->rel); free(x->url); free(x); }
            else p = &(*p)->next;
        }
        if (l) {
            cpy(l->id, sizeof l->id, a->id);
            l->rel = strdup(j->a);
            l->url = strdup(url);
            l->made = now_ms();
            l->expires = secs ? l->made + secs * 1000 : 0;
            l->next = links;
            links = l;
            save_links();
        }
        pthread_mutex_unlock(&cl_mu);
        JBuf r;
        jb_init(&r);
        jb_raw(&r, "{\"url\":");
        jb_str(&r, url);
        if (secs) jb_printf(&r, ",\"expires\":%lld", now_ms() + secs * 1000);
        else jb_raw(&r, ",\"expires\":null");
        jb_raw(&r, "}");
        res = r.oom ? (jb_free(&r), NULL) : r.p;
    }
    if (d.root) json_free(&d);
    post_reply(j->req, res);
    cl_changed();
}

/* ---- the sign-in (6.2) ---- */

static void signin_end(const char *phase, const char *error) {
    if (si.pid > 0) { kill(si.pid, SIGKILL); waitpid(si.pid, NULL, 0); }
    if (si.fo >= 0) close(si.fo);
    if (si.fe >= 0) close(si.fe);
    si.pid = 0;
    si.fo = si.fe = -1;
    explicit_bzero(si.out, sizeof si.out);
    si.ol = 0;
    explicit_bzero(si.cid, sizeof si.cid);
    explicit_bzero(si.csec, sizeof si.csec);
    cpy(si.phase, sizeof si.phase, phase);
    cpy(si.error, sizeof si.error, error);
    si.message[0] = 0;
}

/* the sign-in worked but the account couldn't be set up in rclone (with cl_mu held): the code and rclone's words (#87) */
static void signin_setup_failed(const char *msg) {
    signin_end("failed", cloud_no_drive_msg(msg) ? "cloud.signinNoDrive" : "cloud.signinFailed");
    cloud_clean_text(msg ? msg : "", si.message, sizeof si.message);
}

/* Sign In Again: the new token goes to the same account (same id, same folder) */
static void signin_again(const char *tok, const char *opts) {
    pthread_mutex_lock(&cl_mu);
    Acc *a = cl_find(si.update);
    if (!a) { signin_end("failed", "cloud.notFound"); pthread_mutex_unlock(&cl_mu); cl_changed(); return; }
    if (si.cid[0]) { kv_set(&a->p, "client_id", si.cid); a->own_client = 1; }
    JBuf s;
    jb_init(&s);
    jb_raw(&s, "{\"token\":");
    jb_str(&s, tok);
    jb_raw(&s, opts);                           /* checked by cloud_auth_token: pCloud's host */
    if (si.csec[0]) { jb_raw(&s, ",\"client_secret\":"); jb_str(&s, si.csec); }
    jb_raw(&s, "}");
    cpy(si.account, sizeof si.account, a->id);
    signin_end("waiting", "");
    a->err[0] = a->message[0] = 0;
    pthread_mutex_unlock(&cl_mu);
    if (a->mounted) unmount_acc(a);
    if (!a->configured) bring_up_config(a);
    char msg[256] = "";
    int ok = !s.oom && !create_remote(a, s.p, 2, msg, sizeof msg);
    if (!s.oom) explicit_bzero(s.p, s.n);
    jb_free(&s);
    if (!ok) {
        pthread_mutex_lock(&cl_mu);
        signin_setup_failed(msg);
        pthread_mutex_unlock(&cl_mu);
        fprintf(stderr, "cloud: signing in again failed: %s\n", msg);
        mount_acc(a);
        return;
    }
    finish_new(a);
    pthread_mutex_lock(&cl_mu);
    cpy(si.phase, sizeof si.phase, "done");
    pthread_mutex_unlock(&cl_mu);
    cl_changed();
}

/* the token came: a new account */
static void signin_token(void) {
    char tok[16384], opts[4096];
    if (!cloud_auth_token(si.out, tok, sizeof tok, opts, sizeof opts)) { pthread_mutex_lock(&cl_mu); signin_end("failed", "cloud.signinFailed"); pthread_mutex_unlock(&cl_mu); cl_changed(); return; }
    pthread_mutex_lock(&cl_mu);
    if (si.update[0]) { pthread_mutex_unlock(&cl_mu); signin_again(tok, opts); explicit_bzero(tok, sizeof tok); return; }
    Acc *a = NULL;
    for (int i = 0; i < ACC_MAX && !a; i++) if (!cl_acc[i].used) a = &cl_acc[i];
    const CloudKind *k = kind_get(si.kind);
    if (!a || !k || cl_find_name(si.name)) { signin_end("failed", !a ? "cloud.tooMany" : "cloud.nameTaken"); pthread_mutex_unlock(&cl_mu); explicit_bzero(tok, sizeof tok); cl_changed(); return; }
    memset(a, 0, sizeof *a);
    a->used = 1;
    a->k = k;
    a->links_supported = -1;
    a->created = time(NULL);
    cpy(a->name, sizeof a->name, si.name);
    a->id[0] = 'c';
    rand_hex(a->id + 1, 4);
    a->own_client = si.cid[0] != 0;
    if (si.cid[0]) kv_set(&a->p, "client_id", si.cid);
    JBuf s;
    jb_init(&s);
    jb_raw(&s, "{\"token\":");
    jb_str(&s, tok);
    jb_raw(&s, opts);                           /* checked by cloud_auth_token: pCloud's host */
    if (si.csec[0]) { jb_raw(&s, ",\"client_secret\":"); jb_str(&s, si.csec); }
    jb_raw(&s, "}");
    explicit_bzero(tok, sizeof tok);
    cpy(si.account, sizeof si.account, a->id);
    signin_end("waiting", "");                 /* still waiting: the account is being set up */
    pthread_mutex_unlock(&cl_mu);
    char msg[256] = "";
    int ok = !s.oom && !create_remote(a, s.p, 1, msg, sizeof msg);
    if (!s.oom) explicit_bzero(s.p, s.n);
    jb_free(&s);
    if (!ok) {
        /* its half-made remote goes from rclone too (it holds the token) */
        char body[64];
        cl_pf(body, sizeof body, "{\"name\":\"%s\"}", a->id);
        if (cl_running) rcs(RCM_DELETE, body, NULL, 0, RC_SHORT);
        pthread_mutex_lock(&cl_mu);
        acc_free(a);
        signin_setup_failed(msg);
        si.account[0] = 0;
        pthread_mutex_unlock(&cl_mu);
        fprintf(stderr, "cloud: the new account could not be set up: %s\n", msg);
        cl_changed();
        return;
    }
    finish_new(a);
    pthread_mutex_lock(&cl_mu);
    cpy(si.phase, sizeof si.phase, "done");
    pthread_mutex_unlock(&cl_mu);
    cl_changed();
}

/* reads what the authorize process wrote; 1 when it has ended */
static int signin_io(void) {
    int ended = 0;
    if (si.fe >= 0) {
        char b[2048];
        ssize_t n = read(si.fe, b, sizeof b);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (b[i] == '\n' || si.el + 1 >= sizeof si.ebuf) {
                    si.ebuf[si.el] = 0;
                    char url[200];
                    if (!si.url[0] && cloud_auth_url(si.ebuf, url, sizeof url)) { pthread_mutex_lock(&cl_mu); cpy(si.url, sizeof si.url, url); si.opened = 0; pthread_mutex_unlock(&cl_mu); wake(efd_push); }
                    si.el = 0;
                } else si.ebuf[si.el++] = b[i];
            }
        } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) { close(si.fe); si.fe = -1; }
    }
    if (si.fo >= 0) {
        ssize_t n = read(si.fo, si.out + si.ol, sizeof si.out - 1 - si.ol);
        if (n > 0) { si.ol += (size_t)n; si.out[si.ol] = 0; }
        else if (n == 0 || (errno != EAGAIN && errno != EINTR) || si.ol >= sizeof si.out - 1) { close(si.fo); si.fo = -1; }
    }
    if (si.fo < 0 && si.fe < 0) {
        int st = 0;
        pid_t p = si.pid;
        if (p > 0 && waitpid(p, &st, WNOHANG) == p) { si.pid = 0; ended = WIFEXITED(st) && !WEXITSTATUS(st) ? 1 : 2; }
        else if (p <= 0) ended = 2;
    }
    return ended;
}

static pid_t spawn_authorize(const CloudKind *k, const char *cid, const char *csec, int *fo, int *fe) {
    int o[2], e[2];
    if (pipe2(o, O_CLOEXEC)) return -1;
    if (pipe2(e, O_CLOEXEC)) { close(o[0]); close(o[1]); return -1; }
    char e1[400] = "", e2[400] = "", up[32];
    size_t i = 0;
    for (; k->kind[i] && i + 1 < sizeof up; i++) up[i] = (char)toupper((unsigned char)k->kind[i]);
    up[i] = 0;
    /* an own client ID (6.2): in the environment, which only this user can read (unlike the command line) */
    if (cid && cid[0]) cl_pf(e1, sizeof e1, "RCLONE_%s_CLIENT_ID=%s", up, cid);
    if (csec && csec[0]) cl_pf(e2, sizeof e2, "RCLONE_%s_CLIENT_SECRET=%s", up, csec);
    char *env[10], envbuf[4096];
    child_env(env, envbuf, sizeof envbuf, e1[0] ? e1 : NULL, e2[0] ? e2 : NULL);
    explicit_bzero(e1, sizeof e1);
    explicit_bzero(e2, sizeof e2);
    /* "e30" is "{}" as a config blob: rclone then prints every option the sign-in set, not the token alone (#71: pCloud's
     * host for an account in Europe comes back only with the redirect; without it every call says "Invalid access_token") */
    char *argv[] = { "rclone", "authorize", (char *)k->type, "e30", "--auth-no-open-browser", "--config", "", NULL };
    const char *bin = rclone_bin();
    pid_t p = fork();
    if (!p) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(o[1], 1); dup2(e[1], 2);
        syscall(SYS_close_range, 3, ~0u, 0);
        child_common();
        if (app_scope_enter(CLOUD_SCOPE) && getenv("LORDOS_SESSIOND")) _exit(126);
#ifdef CLOUD_TEST
        /* test builds only: tests/cloud_signin_fake.sh's stand-in lives in the build tree, which the sandbox can't run */
        if (!getenv("LORDOS_CLOUD_FAKE_AUTHORIZE"))
#endif
        sandbox_exec_child(1);              /* the system read-only, nothing written; the network for the redirect */
        execve(bin, argv, env);
        _exit(127);
    }
    explicit_bzero(envbuf, sizeof envbuf);
    close(o[1]);
    close(e[1]);
    if (p < 0) { close(o[0]); close(e[0]); return -1; }
    fcntl(o[0], F_SETFL, O_NONBLOCK);
    fcntl(e[0], F_SETFL, O_NONBLOCK);
    *fo = o[0];
    *fe = e[0];
    return p;
}

/* ---- the thread ---- */

static void record_transfers(void) {
    JDoc d;
    if (rcj(RCM_STATS, "{}", &d, NULL, 0, RC_SHORT) != 200 || !d.root) { if (d.root) json_free(&d); return; }
    pthread_mutex_lock(&cl_mu);
    for (int i = 0; i < ACC_MAX; i++) if (cl_acc[i].used) { for (int k = 0; k < cl_acc[i].nt; k++) free(cl_acc[i].t[k].name); cl_acc[i].nt = 0; }
    const JVal *tr = json_get(d.root, "transferring");
    for (const JVal *x = tr && tr->type == J_ARR ? tr->kid : NULL; x; x = x->next) {
        const char *src = json_str(x, "srcFs"), *dst = json_str(x, "dstFs"), *nm = json_str(x, "name");
        int up = dst != NULL;
        const char *fs = up ? dst : src;
        if (!fs || !nm) continue;
        char id[16];
        size_t l = strcspn(fs, ":");
        if (l >= sizeof id || fs[l] != ':' || fs[l + 1]) continue;
        memcpy(id, fs, l);
        id[l] = 0;
        Acc *a = cl_find(id);
        if (!a || a->nt >= T_MAX) continue;
        TItem *t = &a->t[a->nt++];
        t->name = strdup(nm);
        t->bytes = (long long)json_num(x, "bytes", 0);
        t->size = (long long)json_num(x, "size", 0);
        t->pct = json_num(x, "percentage", 0);
        t->up = up;
    }
    pthread_mutex_unlock(&cl_mu);
    json_free(&d);
}

static void record_done(void) {
    JDoc d;
    if (rcj(RCM_TRANSFERRED, "{}", &d, NULL, 0, RC_SHORT) != 200 || !d.root) { if (d.root) json_free(&d); return; }
    const JVal *tr = json_get(d.root, "transferred");
    long long newest = seen_done;
    struct { char id[16], rel[CLOUD_REL_MAX + 1]; } ups[16];
    int nup = 0;
    pthread_mutex_lock(&cl_mu);
    for (const JVal *x = tr && tr->type == J_ARR ? tr->kid : NULL; x; x = x->next) {
        const char *src = json_str(x, "srcFs"), *dst = json_str(x, "dstFs"), *nm = json_str(x, "name"), *at = json_str(x, "completed_at"), *er = json_str(x, "error");
        const char *fs = dst ? dst : src;
        long long t = at ? cloud_rfc3339(at) : -1;
        if (!fs || !nm || t < 0 || !cloud_rel_ok(nm)) continue;
        char id[16];
        size_t l = strcspn(fs, ":");
        if (l >= sizeof id || fs[l] != ':') continue;
        memcpy(id, fs, l);
        id[l] = 0;
        Acc *a = cl_find(id);
        if (!a) continue;
        /* the last error of each file */
        int k;
        for (k = 0; k < a->nerrn && strcmp(a->errn[k], nm); k++) {}
        /* B10: a transfer that failed for the network: list the account now, which marks it offline */
        if (er && er[0] && t > seen_done) { const char *fc = cloud_failure_code(er); if (fc && !strcmp(fc, "cloud.unreachable")) a->check_at = 0; }
        if (er && er[0] && k == a->nerrn) {
            if (a->nerrn == ERRN_MAX) { free(a->errn[0]); memmove(a->errn, a->errn + 1, sizeof a->errn[0] * (ERRN_MAX - 1)); a->nerrn--; }
            a->errn[a->nerrn++] = strdup(nm);
        } else if ((!er || !er[0]) && k < a->nerrn) { free(a->errn[k]); a->errn[k] = a->errn[--a->nerrn]; }
        if (dst && t > seen_done && (!er || !er[0]) && nup < 16) { cl_pf(ups[nup].id, sizeof ups[nup].id, "%s", a->id); cl_pf(ups[nup].rel, sizeof ups[nup].rel, "%s", nm); nup++; }
        if (t <= seen_done || (er && er[0])) continue;
        if (t > newest) newest = t;
        if (nrecent == RECENT_MAX) nrecent--;
        memmove(recent + 1, recent, sizeof recent[0] * (size_t)nrecent);
        cl_pf(recent[0].path, sizeof recent[0].path, "%s/%s/%s", cl_root, a->name, nm);
        cpy(recent[0].account, sizeof recent[0].account, a->id);
        recent[0].up = dst != NULL;
        recent[0].at = t * 1000;
        nrecent++;
    }
    seen_done = newest;
    pthread_mutex_unlock(&cl_mu);
    json_free(&d);
    for (int i = 0; i < nup; i++) {                     /* N5: what the cloud has after our upload */
        pthread_mutex_lock(&cl_mu);
        Acc *a = cl_find(ups[i].id);
        pthread_mutex_unlock(&cl_mu);
        if (a) note_synced(a, ups[i].rel);
    }
}

static void set_expiry(Acc *a, int qid, double e) {
    char b[160];
    cl_pf(b, sizeof b, "{\"fs\":\"%s:\",\"id\":%d,\"expiry\":%.0f,\"relative\":false}", a->id, qid, e);
    rcs(RCM_VFS_EXPIRY, b, NULL, 0, RC_SHORT);
}

static int was_checked(Acc *a, const char *name) {
    pthread_mutex_lock(&cl_mu);
    int y = 0;
    for (int i = 0; i < a->nchecked && !y; i++) y = !strcmp(a->checked[i], name);
    pthread_mutex_unlock(&cl_mu);
    return y;
}
/* N5: after our upload of a file, the cloud's object is ours: its fingerprint is kept (the VFS keeps the one from when it
 * came down) */
static const char *synced_fp(Acc *a, const char *rel) {
    for (int i = 0; i < a->nsynced; i++) if (!strcmp(a->synced[i].rel, rel)) return a->synced[i].fp;
    return NULL;
}
static void note_synced(Acc *a, const char *rel) {
    char body[CLOUD_REL_MAX * 2 + 128];
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"fs\":\"%s:\",\"remote\":", a->id);
    jb_str(&b, rel);
    jb_raw(&b, ",\"opt\":{\"showHash\":true}}");
    cl_pf(body, sizeof body, "%s", b.oom ? "" : b.p);
    jb_free(&b);
    JDoc d;
    if (rcj(RCM_STAT, body, &d, NULL, 0, 15000) != 200 || !d.root) { if (d.root) json_free(&d); return; }
    const JVal *it = json_get(d.root, "item");
    if (it && it->type == J_OBJ) {
        char fp[256];
        const char *mt = json_str(it, "ModTime"), *h = NULL;
        const JVal *hs = json_get(it, "Hashes");
        if (hs && hs->type == J_OBJ && hs->kid && hs->kid->type == J_STR) h = hs->kid->str;
        /* the VFS fingerprint's own shape: size, modtime as Go prints it in UTC, hash */
        long long t = mt ? cloud_rfc3339(mt) : -1;
        char when[64] = "";
        if (t >= 0) { time_t tt = (time_t)t; struct tm g; gmtime_r(&tt, &g); strftime(when, sizeof when, ",%Y-%m-%d %H:%M:%S +0000 UTC", &g); }
        cl_pf(fp, sizeof fp, "%lld%s%s%s", (long long)json_num(it, "Size", 0), when, h ? "," : "", h ? h : "");
        pthread_mutex_lock(&cl_mu);
        int k = -1;
        for (int i = 0; i < a->nsynced; i++) if (!strcmp(a->synced[i].rel, rel)) k = i;
        if (k < 0 && a->nsynced < SYNCED_MAX) k = a->nsynced++;
        if (k < 0) k = (int)(time(NULL) % SYNCED_MAX);
        cl_pf(a->synced[k].rel, sizeof a->synced[k].rel, "%s", rel);
        cl_pf(a->synced[k].fp, sizeof a->synced[k].fp, "%s", fp);
        pthread_mutex_unlock(&cl_mu);
    }
    json_free(&d);
}

static void unmark_checked(Acc *a, const char *name) {
    pthread_mutex_lock(&cl_mu);
    for (int i = 0; i < a->nchecked; i++) if (!strcmp(a->checked[i], name)) { free(a->checked[i]); a->checked[i] = a->checked[--a->nchecked]; break; }
    pthread_mutex_unlock(&cl_mu);
}
static void mark_checked(Acc *a, const char *name) {
    pthread_mutex_lock(&cl_mu);
    if (a->nchecked < Q_MAX) { char *d = strdup(name); if (d) a->checked[a->nchecked++] = d; }
    pthread_mutex_unlock(&cl_mu);
}

/* the conflict rule (7.2), as built: the file was changed here; if the cloud's object changed too since this copy came
 * down (or appeared meanwhile), the cloud's version is first copied, in the cloud, to "name (cloud copy).ext" next to it,
 * and then ours goes up under the name. Renaming ours in the mount instead (the design's "name (this computer)") moves
 * the cloud's object along with it in rclone's VFS, so the cloud's version would be lost: measured, not used.
 * 0 it goes up (kept: the copy, or "" when there was no conflict), 2 not yet (the copy failed: held, tried again) */
static int conflict_check(Acc *a, const char *rel, char *kept, size_t kn) {
    char mp[PATH_MAX * 2], body[CLOUD_REL_MAX * 2 + 128];
    cl_pf(mp, sizeof mp, "%s/vfsMeta/%s/%s", cl_cache, a->id, rel);
    char *mt = read_file(mp, 65536, NULL);
    CloudMeta m;
    int have = mt && !cloud_meta_parse(mt, strlen(mt), &m) && m.fingerprint[0];
    free(mt);
    /* made here (no copy came down): it is a conflict too if the cloud has an object with that name it hadn't shown yet */
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"fs\":\"%s:\",\"remote\":", a->id);
    jb_str(&b, rel);
    jb_raw(&b, ",\"opt\":{\"showHash\":true}}");
    cl_pf(body, sizeof body, "%s", b.oom ? "" : b.p);
    jb_free(&b);
    JDoc d;
    char smsg[256] = "";
    int st = rcj(RCM_STAT, body, &d, smsg, sizeof smsg, 15000);
    if (st != 200 && st > 0 && stale_pool(a, smsg)) {
        if (d.root) json_free(&d);
        smsg[0] = 0;
        st = rcj(RCM_STAT, body, &d, smsg, sizeof smsg, 15000);
    }
    if (st != 200) {
        /* B18: the cloud can't say what it has: held until it can (never "no conflict" by default) */
        if (d.root) json_free(&d);
        const char *fc = cloud_failure_code(smsg);
        pthread_mutex_lock(&cl_mu);
        if (fc && !strcmp(fc, "cloud.unreachable")) { cpy(a->err, sizeof a->err, "cloud.unreachable"); a->check_at = time(NULL) + 30; }
        pthread_mutex_unlock(&cl_mu);
        kept[0] = 0;
        return 2;
    }
    const JVal *item = d.root ? json_get(d.root, "item") : NULL;
    int changed = 0;
    if (item && item->type == J_OBJ) {
        const char *hs[8];
        int nh = 0;
        const JVal *h = json_get(item, "Hashes");
        for (const JVal *x = h && h->type == J_OBJ ? h->kid : NULL; x && nh < 8; x = x->next) if (x->type == J_STR) hs[nh++] = x->str;
        const char *mtm = json_str(item, "ModTime");
        /* N5: what this computer last knew of the cloud's object: its own last upload of it, else what came down */
        const char *fp = synced_fp(a, rel);
        changed = !have && !fp ? 1 : cloud_remote_changed(fp ? fp : m.fingerprint, (long long)json_num(item, "Size", -1), mtm ? (double)cloud_rfc3339(mtm) : -1, hs, nh, a->precision_s) == 1;
    }
    if (d.root) json_free(&d);
    kept[0] = 0;
    if (!changed) return 0;
    char from[PATH_MAX * 2], to[PATH_MAX * 2];
    cl_pf(from, sizeof from, "%s/%s/%s", cl_root, a->name, rel);
    for (int n = 1; n < 20; n++) {
        if (cloud_conflict_name(rel, n, kept, kn)) return 0;
        cl_pf(to, sizeof to, "%s/%s/%s", cl_root, a->name, kept);
        struct stat s2;
        if (!lstat(to, &s2)) continue;
        JBuf c;
        jb_init(&c);
        jb_printf(&c, "{\"srcFs\":\"%s:\",\"srcRemote\":", a->id);
        jb_str(&c, rel);
        jb_printf(&c, ",\"dstFs\":\"%s:\",\"dstRemote\":", a->id);
        jb_str(&c, kept);
        jb_raw(&c, "}");
        char msg[256] = "";
        int st2 = c.oom ? -1 : rcs(RCM_COPYFILE, c.p, msg, sizeof msg, 300000);
        jb_free(&c);
        if (st2 != 200) { fprintf(stderr, "cloud: conflict on %s, the cloud's version couldn't be copied (%s): held\n", rel, msg); kept[0] = 0; return 2; }
        char dir[CLOUD_REL_MAX + 1];
        cl_pf(dir, sizeof dir, "%s", rel);
        char *sl = strrchr(dir, '/');
        if (sl) *sl = 0; else dir[0] = 0;
        JBuf r;
        jb_init(&r);
        jb_printf(&r, "{\"fs\":\"%s:\"", a->id);
        if (dir[0]) { jb_raw(&r, ",\"dir\":"); jb_str(&r, dir); }
        jb_raw(&r, "}");
        if (!r.oom) rcs(RCM_VFS_REFRESH, r.p, NULL, 0, RC_SHORT);      /* the copy shows in Files */
        jb_free(&r);
        pthread_mutex_lock(&cl_mu);
        if (nconflicts == CONFLICTS_MAX) nconflicts--;
        memmove(conflicts + 1, conflicts, sizeof conflicts[0] * (size_t)nconflicts);
        cl_pf(conflicts[0].path, sizeof conflicts[0].path, "%s", from);
        cl_pf(conflicts[0].kept, sizeof conflicts[0].kept, "%s", to);
        cl_pf(conflicts[0].account, sizeof conflicts[0].account, "%s", a->id);
        conflicts[0].at = now_ms();
        nconflicts++;
        pthread_mutex_unlock(&cl_mu);
        fprintf(stderr, "cloud: %s changed in the cloud and here: the cloud's version kept as %s\n", rel, kept);
        cl_changed();
        return 0;
    }
    return 0;
}

static void poll_acc(Acc *a) {
    char body[64];
    cl_pf(body, sizeof body, "{\"fs\":\"%s:\"}", a->id);
    JDoc d;
    if (rcj(RCM_VFS_QUEUE, body, &d, NULL, 0, RC_SHORT) == 200 && d.root) {
        pthread_mutex_lock(&cl_mu);
        for (int k = 0; k < a->nq; k++) free(a->q[k].name);
        a->nq = 0;
        const JVal *q = json_get(d.root, "queue");
        for (const JVal *x = q && q->type == J_ARR ? q->kid : NULL; x && a->nq < Q_MAX; x = x->next) {
            const char *nm = json_str(x, "name");
            if (!nm) continue;
            QItem *it = &a->q[a->nq++];
            it->name = strdup(nm);
            it->size = (long long)json_num(x, "size", 0);
            it->expiry = json_num(x, "expiry", 0);
            it->uploading = json_bool(x, "uploading", 0);
            it->tries = (int)json_num(x, "tries", 0);
            it->qid = (int)json_num(x, "id", 0);
        }
        pthread_mutex_unlock(&cl_mu);
    }
    if (d.root) json_free(&d);
    if (rcj(RCM_VFS_STATS, body, &d, NULL, 0, RC_SHORT) == 200 && d.root) {
        pthread_mutex_lock(&cl_mu);
        a->in_use = (int)json_num(d.root, "inUse", 0);
        pthread_mutex_unlock(&cl_mu);
    }
    if (d.root) json_free(&d);
    /* Pause holds what waits to go up; otherwise each changed file is looked at once before it goes (the conflict rule):
     * held, compared with the cloud's object (the cloud's version copied aside on a conflict), then let go */
    int hold = cl_paused(a);
    struct { int id; double e; int up, tries; char name[CLOUD_REL_MAX + 1]; } it[64];
    int ni = 0;
    pthread_mutex_lock(&cl_mu);
    for (int k = 0; k < a->nq && ni < 64; k++) {
        it[ni].id = a->q[k].qid;
        it[ni].e = a->q[k].expiry;
        it[ni].up = a->q[k].uploading;
        it[ni].tries = a->q[k].tries;
        cl_pf(it[ni].name, sizeof it[ni].name, "%s", a->q[k].name);
        ni++;
    }
    /* forget the ones that went up */
    for (int c = 0; c < a->nchecked; ) {
        int in = 0;
        for (int k = 0; k < a->nq; k++) if (!strcmp(a->q[k].name, a->checked[c])) in = 1;
        if (in) { c++; continue; }
        free(a->checked[c]);
        a->checked[c] = a->checked[--a->nchecked];
    }
    a->held = hold;
    pthread_mutex_unlock(&cl_mu);
    /* B18: the server not answering, every waiting file is held: rclone mustn't retry an upload by itself, since the
     * cloud's copy may change meanwhile; each goes again through the check once the account answers */
    pthread_mutex_lock(&cl_mu);
    int offline = !strcmp(a->err, "cloud.unreachable") || !strcmp(a->err, "cloud.offline");
    pthread_mutex_unlock(&cl_mu);
    for (int k = 0; k < ni; k++) {
        if (it[k].up) continue;
        if (it[k].tries > 0 && was_checked(a, it[k].name)) {
            unmark_checked(a, it[k].name);              /* an attempt failed: looked at again before the next one */
            set_expiry(a, it[k].id, 1e9);
            pthread_mutex_lock(&cl_mu);
            a->check_at = 0;                            /* and the account listed now: offline? */
            pthread_mutex_unlock(&cl_mu);
            continue;
        }
        if (hold || offline) { if (it[k].e < 1e8) set_expiry(a, it[k].id, 1e9); continue; }
        if (!was_checked(a, it[k].name)) {
            set_expiry(a, it[k].id, 1e9);               /* held while it is looked at */
            char kept[CLOUD_REL_MAX + 1];
            if (conflict_check(a, it[k].name, kept, sizeof kept) == 2) continue;      /* held: tried again */
            mark_checked(a, it[k].name);
            set_expiry(a, it[k].id, 0);
            continue;
        }
        if (it[k].e > 1e8) set_expiry(a, it[k].id, 0);   /* looked at (or held by Pause): it goes */
    }
}

static void check_daemon(void) {
    if (dmn.pid <= 0) return;
    int st;
    if (waitpid(dmn.pid, &st, WNOHANG) != dmn.pid) return;
    fprintf(stderr, "cloud: rclone ended (%s %d)\n", WIFSIGNALED(st) ? "signal" : "status", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
    daemon_gone();
    long long now = mono_ms();
    int recent_n = 0;
    for (int i = 0; i < RESTARTS_PER_MIN; i++) if (dmn.crash[i] && now - dmn.crash[i] < 60000) recent_n++;
    if (recent_n >= RESTARTS_PER_MIN) { cpy(dmn.error, sizeof dmn.error, "cloud.crashed"); cl_changed(); return; }
    memmove(dmn.crash + 1, dmn.crash, sizeof dmn.crash[0] * (RESTARTS_PER_MIN - 1));
    dmn.crash[0] = now;
    if (naccounts() && !daemon_start()) {
        dmn.restarts++;
        for (int i = 0; i < ACC_MAX; i++) if (cl_acc[i].used) bring_up(&cl_acc[i]);
    }
    cl_changed();
}

/* the state for JS; light: without the numbers that change by themselves, to compare */
static char *state_json(int light) {
    JBuf b;
    jb_init(&b);
    long rss = 0;
    if (!light && dmn.pid > 0) {
        char p[64], line[256];
        cl_pf(p, sizeof p, "/proc/%d/status", (int)dmn.pid);
        FILE *f = fopen(p, "re");
        while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "VmRSS:", 6)) rss = atol(line + 6);
        if (f) fclose(f);
    }
    pthread_mutex_lock(&cl_mu);
    jb_printf(&b, "{\"available\":%s,\"service\":{\"running\":%s,\"version\":", cloud_available() ? "true" : "false", cl_running ? "true" : "false");
    jb_str(&b, cl_running ? dmn.version : "");
    jb_printf(&b, ",\"uptimeS\":%lld,\"rssKb\":%ld,\"restarts\":%d", light || !cl_running ? 0LL : (long long)(time(NULL) - dmn.started), rss, dmn.restarts);
    if (dmn.error[0]) { jb_raw(&b, ",\"error\":"); jb_str(&b, dmn.error); }
    jb_printf(&b, "},\"options\":{\"bwUp\":%ld,\"bwDown\":%ld,\"pauseOnBattery\":%s,\"pauseOnMetered\":%s,\"autoFree\":%s,\"freeBelowGb\":%d,\"pauseAll\":%s,\"onBattery\":%s},\"accounts\":[",
              opt.bw_up, opt.bw_down, opt.pause_batt ? "true" : "false", opt.pause_metered ? "true" : "false", opt.auto_free ? "true" : "false",
              opt.free_below, opt.pause_all ? "true" : "false", on_batt ? "true" : "false");
    int first = 1;
    for (int i = 0; i < ACC_MAX; i++) {
        Acc *a = &cl_acc[i];
        if (!a->used) continue;
        char dir[PATH_MAX];
        mount_dir(a, dir, sizeof dir);
        long long ub = 0, ud = 0, db = 0, dd = 0;
        int nd = 0;
        const char *un = NULL, *dn = NULL;
        for (int k = 0; k < a->nq; k++) { ub += a->q[k].size; if (a->q[k].uploading && !un) un = a->q[k].name; }
        for (int k = 0; k < a->nt; k++) {
            if (a->t[k].up) { ud += a->t[k].bytes; if (!un) un = a->t[k].name; }
            else { nd++; db += a->t[k].size; dd += a->t[k].bytes; if (!dn) dn = a->t[k].name; }
        }
        /* U7: with the service down every account says so (error cloud.notRunning or the service's own error) */
        int down = !cl_running && (dmn.error[0] || dmn.started);
        const char *st = a->locked ? "locked" : down ? "error" : !strcmp(a->err, "cloud.signInAgain") ? "signin"
                       : !strcmp(a->err, "cloud.offline") || !strcmp(a->err, "cloud.unreachable") ? "offline"
                       : a->err[0] ? "error" : cl_paused(a) ? "paused" : (a->nq || nd) ? "syncing" : !a->mounted ? "syncing" : "ok";
        jb_raw(&b, first ? "{\"id\":" : ",{\"id\":");
        first = 0;
        jb_str(&b, a->id);
        jb_raw(&b, ",\"name\":");
        jb_str(&b, a->name);
        jb_raw(&b, ",\"kind\":");
        jb_str(&b, a->k->kind);
        jb_raw(&b, ",\"provider\":");
        jb_str(&b, a->k->label);
        jb_raw(&b, ",\"group\":");
        jb_str(&b, a->k->group);
        jb_raw(&b, ",\"mount\":");
        jb_str(&b, dir);
        jb_printf(&b, ",\"mounted\":%s,\"state\":\"%s\"", a->mounted ? "true" : "false", st);
        if (a->locked) { jb_raw(&b, ",\"error\":"); jb_str(&b, a->locked == 2 ? "cloud.noKeyring" : "cloud.keyringLocked"); }
        else if (down) { jb_raw(&b, ",\"error\":"); jb_str(&b, dmn.error[0] ? dmn.error : "cloud.notRunning"); }
        else if (a->err[0]) {
            jb_raw(&b, ",\"error\":");
            jb_str(&b, a->err);
            /* U10: a key or certificate that changed: both fingerprints */
            if (a->fp_old[0] || a->fp_new[0]) { jb_raw(&b, ",\"old\":"); jb_str(&b, a->fp_old); jb_raw(&b, ",\"new\":"); jb_str(&b, a->fp_new); }
        }
        if (a->message[0]) { jb_raw(&b, ",\"message\":"); jb_str(&b, a->message); }
        if (a->have_about || a->measured_at) {
            static const char *const k[5] = { "total", "used", "free", "trashed", "other" };
            jb_raw(&b, ",\"about\":{");
            int f2 = 1;
            /* used: the provider's figure, or else the last Measure (17.2 item 5) */
            for (int x = 0; x < 5; x++) {
                double v = a->have_about ? a->ab[x] : -1;
                if (x == 1 && v < 0) v = (double)a->measured_bytes;
                if (v < 0) continue;
                jb_printf(&b, "%s\"%s\":", f2 ? "" : ",", k[x]);
                jb_num(&b, v);
                f2 = 0;
            }
            if (a->measured_at) jb_printf(&b, "%s\"measuredAt\":%lld,\"measuredBytes\":%lld,\"measuredCount\":%lld", f2 ? "" : ",", a->measured_at, a->measured_bytes, a->measured_count);
            jb_raw(&b, "}");
        } else jb_raw(&b, ",\"about\":null");
        jb_printf(&b, ",\"listed\":%u", a->listed);
        jb_printf(&b, ",\"cacheBytes\":%lld,\"keptBytes\":%lld,\"up\":{\"files\":%d,\"bytes\":%lld,\"done\":%lld", a->cache_bytes, a->kept_bytes, a->nq, ub, ud);
        if (un) { jb_raw(&b, ",\"name\":"); jb_str(&b, un); }
        jb_printf(&b, "},\"down\":{\"files\":%d,\"bytes\":%lld,\"done\":%lld", nd, db, light ? 0 : dd);
        if (dn) { jb_raw(&b, ",\"name\":"); jb_str(&b, dn); }
        jb_raw(&b, "},\"kept\":[");
        for (int k = 0; k < a->nkeep; k++) {
            char kp[PATH_MAX * 2];
            cl_pf(kp, sizeof kp, "%s%s%s", dir, a->keep[k][0] ? "/" : "", a->keep[k]);
            if (k) jb_raw(&b, ",");
            jb_str(&b, kp);
        }
        if (a->srv_port > 0) { jb_raw(&b, "],\"host\":"); jb_str(&b, a->srv_host); jb_printf(&b, ",\"port\":%d", a->srv_port); }
        else jb_raw(&b, "],\"host\":null,\"port\":null");
        jb_printf(&b, ",\"linksSupported\":%s,\"trashSupported\":%s,\"measuring\":%s,\"emptyingTrash\":%s,\"configuring\":%s,\"paused\":%s,\"ownClient\":%s}",
                  a->links_supported > 0 ? "true" : "false", a->trash_supported > 0 ? "true" : "false", a->rcjob_kind == 'm' ? "true" : "false",
                  a->rcjob_kind == 't' ? "true" : "false", a->pending_req || a->new_acc ? "true" : "false", a->paused ? "true" : "false", a->own_client ? "true" : "false");
    }
    jb_raw(&b, "],\"signin\":");
    if (si.on) {
        jb_raw(&b, "{\"id\":");
        jb_str(&b, si.id);
        jb_raw(&b, ",\"kind\":");
        jb_str(&b, si.kind);
        jb_raw(&b, ",\"name\":");
        jb_str(&b, si.name);
        jb_raw(&b, ",\"phase\":");
        jb_str(&b, si.phase);
        jb_raw(&b, ",\"url\":");
        if (si.url[0]) jb_str(&b, si.url); else jb_raw(&b, "null");
        jb_printf(&b, ",\"expiresAt\":%lld", si.expires_ms);
        if (si.error[0]) { jb_raw(&b, ",\"error\":"); jb_str(&b, si.error); }
        if (si.message[0]) { jb_raw(&b, ",\"message\":"); jb_str(&b, si.message); }
        if (si.account[0]) { jb_raw(&b, ",\"account\":"); jb_str(&b, si.account); }
        jb_raw(&b, "}");
    } else jb_raw(&b, "null");
    jb_raw(&b, ",\"conflicts\":[");
    for (int i = 0; i < nconflicts; i++) {
        jb_raw(&b, i ? ",{\"path\":" : "{\"path\":");
        jb_str(&b, conflicts[i].path);
        jb_raw(&b, ",\"kept\":");
        jb_str(&b, conflicts[i].kept);
        jb_raw(&b, ",\"account\":");
        jb_str(&b, conflicts[i].account);
        jb_printf(&b, ",\"at\":%lld}", conflicts[i].at);
    }
    jb_raw(&b, "]");
    jb_raw(&b, ",\"recent\":[");
    for (int i = 0; i < nrecent; i++) {
        jb_raw(&b, i ? ",{\"path\":" : "{\"path\":");
        jb_str(&b, recent[i].path);
        jb_raw(&b, ",\"account\":");
        jb_str(&b, recent[i].account);
        jb_printf(&b, ",\"dir\":\"%s\",\"at\":%lld}", recent[i].up ? "up" : "down", recent[i].at);
    }
    jb_raw(&b, "]}");
    pthread_mutex_unlock(&cl_mu);
    if (b.oom) { jb_free(&b); return NULL; }
    return b.p;
}

char *cloud_state_json(void) { return state_json(0); }

/* the state changed since the last push: wake the main loop, at most twice a second */
static long long maybe_push(void) {
    char *s = state_json(1);
    if (!s) return 0;
    int diff = !last_state || strcmp(s, last_state);
    if (!diff) { free(s); return 0; }
    long long now = mono_ms();
    if (now - last_emit_ms < 500) { free(s); return 500 - (now - last_emit_ms); }
    free(last_state);
    last_state = s;
    last_emit_ms = now;
    wake(efd_push);
    return 0;
}

/* one rclone job in the background for an account (Measure, Empty Trash): started here, followed by the tick */
static void rcjob_start(Job *j, char kind) {
    Acc *a;
    pthread_mutex_lock(&cl_mu);
    a = cl_find(j->id);
    int busy = a && a->rcjob_kind;
    pthread_mutex_unlock(&cl_mu);
    if (!a || !cl_running || !a->configured) { post_reply(j->req, err_json(!a ? "cloud.notFound" : unavailable(a))); return; }
    if (busy) { post_reply(j->req, err_json("cloud.busy")); return; }
    if (kind == 't' && a->trash_supported == 0) { post_reply(j->req, err_json("unsupported")); return; }
    char body[96], msg[256];
    cl_pf(body, sizeof body, "{\"fs\":\"%s:\",\"_async\":true}", a->id);
    JDoc d;
    int st = rcj(kind == 'm' ? RCM_SIZE : RCM_CLEANUP, body, &d, msg, sizeof msg, RC_SHORT);
    double id = st == 200 && d.root ? json_num(d.root, "jobid", -1) : -1;
    if (d.root) json_free(&d);
    if (id < 0) { post_reply(j->req, err_msg_json("cloud.failed", msg)); return; }
    pthread_mutex_lock(&cl_mu);
    a->rcjob_kind = kind;
    a->rcjob_id = (long long)id;
    a->rcjob_req = j->req;
    pthread_mutex_unlock(&cl_mu);
    cl_changed();
}

static void rcjob_poll(Acc *a) {
    char body[64], msg[256];
    cl_pf(body, sizeof body, "{\"jobid\":%lld}", a->rcjob_id);
    JDoc d;
    int st = rcj(RCM_JOB_STATUS, body, &d, msg, sizeof msg, RC_SHORT);
    if (st != 200 || !d.root) {
        if (d.root) json_free(&d);
        if (st > 0) { post_reply(a->rcjob_req, err_msg_json("cloud.failed", msg)); pthread_mutex_lock(&cl_mu); a->rcjob_kind = 0; pthread_mutex_unlock(&cl_mu); cl_changed(); }
        return;
    }
    if (!json_bool(d.root, "finished", 0)) { json_free(&d); return; }
    char kind = a->rcjob_kind, *res;
    const char *e = json_str(d.root, "error");
    if (!json_bool(d.root, "success", 0)) {
        int uns = e && (strstr(e, "not supported") || strstr(e, "doesn't support") || strstr(e, "not implemented"));
        int stopped = e && strstr(e, "context canceled");
        res = err_msg_json(uns ? "unsupported" : stopped ? "cloud.cancelled" : "cloud.failed", e);
        if (uns && kind == 't') a->trash_supported = 0;
    } else if (kind == 'm') {
        const JVal *o = json_get(d.root, "output");
        long long by = (long long)json_num(o, "bytes", 0), n = (long long)json_num(o, "count", 0);
        char b[96];
        cl_pf(b, sizeof b, "{\"bytes\":%lld,\"count\":%lld}", by, n);
        res = strdup(b);
        pthread_mutex_lock(&cl_mu);
        a->measured_bytes = by;
        a->measured_count = n;
        a->measured_at = now_ms();
        save_accounts();
        pthread_mutex_unlock(&cl_mu);
    } else res = strdup("{\"ok\":true}");
    json_free(&d);
    pthread_mutex_lock(&cl_mu);
    int req = a->rcjob_req;
    a->rcjob_kind = 0;
    pthread_mutex_unlock(&cl_mu);
    post_reply(req, res);
    if (kind == 't') about_acc(a);
    cl_changed();
}

static void do_job(Job *j) {
    switch (j->type) {
    case J_PROVIDERS: {
        char *p = ensure_daemon() ? NULL : load_providers();
        post_reply(j->req, p ? p : err_json(cl_running ? "cloud.failed" : dmn.error[0] ? dmn.error : "cloud.notRunning"));
        break;
    }
    case J_ADD: if (!ensure_daemon() && !nkinfo) free(load_providers()); job_add(j); break;
    case J_ANSWER: job_answer(j); break;
    case J_ADDLATE: {
        if (!ensure_daemon()) free(load_providers());
        int r2 = 0;
        char *res = nkinfo ? add_impl(j->d, j->a, j->c, j->b, &r2, j->req) : err_json(cl_running ? "cloud.notReady" : "cloud.notRunning");
        if (res) post_reply(j->req, res);
        break;
    }
    case J_RETRY:
        if (!naccounts() || ensure_daemon()) break;
        for (int i = 0; i < ACC_MAX; i++) {
            Acc *a = &cl_acc[i];
            if (!a->used || (j->id[0] && strcmp(a->id, j->id))) continue;
            if (a->reconnect) {
                /* made again from its options and secrets, then mounted: rclone takes the new known_hosts */
                a->reconnect = 0;
                if (a->mounted) unmount_acc(a);
                pthread_mutex_lock(&cl_mu);
                a->configured = 0;
                a->err[0] = a->message[0] = 0;
                pthread_mutex_unlock(&cl_mu);
                bring_up(a);
                continue;
            }
            if (a->configured && a->mounted) { a->check_at = 0; continue; }     /* listed again on the next tick */
            if (!strcmp(a->err, "cloud.addFailed") || a->pending_req || a->new_acc) continue;
            pthread_mutex_lock(&cl_mu);
            a->err[0] = a->message[0] = 0;
            pthread_mutex_unlock(&cl_mu);
            bring_up(a);
        }
        cl_changed();
        break;
    case J_TRASH: rcjob_start(j, 't'); break;
    case J_MEASURE: rcjob_start(j, 'm'); break;
    case J_CANCEL_MEASURE: {
        Acc *a;
        pthread_mutex_lock(&cl_mu);
        a = cl_find(j->id);
        long long jid = a && a->rcjob_kind == 'm' ? a->rcjob_id : -1;
        pthread_mutex_unlock(&cl_mu);
        if (jid >= 0) { char b[64]; cl_pf(b, sizeof b, "{\"jobid\":%lld}", jid); rcs(RCM_JOB_STOP, b, NULL, 0, RC_SHORT); }
        post_reply(j->req, strdup(jid >= 0 ? "{\"ok\":true}" : "{\"error\":\"cloud.notMeasuring\"}"));
        break;
    }
    case J_REMOVE: job_remove(j); break;
    case J_RENAME: job_rename(j); break;
    case J_ABOUT: { Acc *a; pthread_mutex_lock(&cl_mu); a = cl_find(j->id); pthread_mutex_unlock(&cl_mu); if (a && cl_running && a->configured) about_acc(a); break; }
    case J_LINK: job_link(j, 0); break;
    case J_UNLINK: job_link(j, 1); break;
    case J_RESTART:
        daemon_stop();
        dmn.error[0] = 0;
        memset(dmn.crash, 0, sizeof dmn.crash);
        if (naccounts() && !daemon_start()) { dmn.restarts++; for (int i = 0; i < ACC_MAX; i++) if (cl_acc[i].used) bring_up(&cl_acc[i]); }
        cl_changed();
        break;
    case J_OPTIONS: {
        char r[64], b[128];
        if (cl_running && !cloud_bw_rate(opt.bw_up, opt.bw_down, r, sizeof r)) { cl_pf(b, sizeof b, "{\"rate\":\"%s\"}", r); rcs(RCM_BWLIMIT, b, NULL, 0, RC_SHORT); }
        cf_kick();
        break;
    }
    case J_SIGNIN: if (!ensure_daemon() && !nkinfo) free(load_providers()); break;
    default: break;
    }
}

static void *worker(void *arg) {
    (void)arg;
    if (naccounts() && !ensure_daemon()) {
        free(load_providers());
        for (int i = 0; i < ACC_MAX; i++) if (cl_acc[i].used) bring_up(&cl_acc[i]);
    }
    long long next_tick = 0, next_slow = 0;
    int ticks = 0;
    for (;;) {
        struct pollfd pf[3];
        int n = 0;
        pf[n++] = (struct pollfd){ efd_job, POLLIN, 0 };
        pthread_mutex_lock(&cl_mu);
        int sfo = si.on ? si.fo : -1, sfe = si.on ? si.fe : -1, son = si.on && !strcmp(si.phase, "waiting") && si.pid > 0;
        pthread_mutex_unlock(&cl_mu);
        if (son && sfo >= 0) pf[n++] = (struct pollfd){ sfo, POLLIN, 0 };
        if (son && sfe >= 0) pf[n++] = (struct pollfd){ sfe, POLLIN, 0 };
        long long now = mono_ms();
        int active = cl_running || son;
        int timeout = active ? (int)(next_tick > now ? next_tick - now : 0) : -1;
        if (timeout > 1000) timeout = 1000;
        if (poll(pf, (nfds_t)n, timeout) < 0 && errno != EINTR) break;
        if (pf[0].revents) { uint64_t v; ssize_t r = read(efd_job, &v, sizeof v); (void)r; }
        /* the sign-in */
        if (son) {
            int ended = signin_io();
            long long left = si.expires_ms - now_ms();
            if (ended == 1) signin_token();
            else if (ended == 2) { pthread_mutex_lock(&cl_mu); signin_end("failed", "cloud.signinFailed"); pthread_mutex_unlock(&cl_mu); cl_changed(); }
            else if (left <= 0) { pthread_mutex_lock(&cl_mu); signin_end("failed", "cloud.signinTimeout"); pthread_mutex_unlock(&cl_mu); cl_changed(); }
        }
        /* the jobs */
        for (;;) {
            pthread_mutex_lock(&cl_mu);
            Job *j = jhead;
            if (j) { jhead = j->next; if (!jhead) jtail = NULL; }
            pthread_mutex_unlock(&cl_mu);
            if (!j) break;
            if (j->type == J_STOP) { free(j); daemon_stop(); worker_done = 1; return NULL; }
            do_job(j);
            if (j->a) { explicit_bzero(j->a, strlen(j->a)); free(j->a); }
            if (j->b) explicit_bzero(j->b, strlen(j->b));   /* an answer may be a code or a password */
            free(j->b);
            if (j->c) { explicit_bzero(j->c, strlen(j->c)); free(j->c); }   /* a deferred add's fields */
            free(j->d);
            free(j);
            dmn.idle_since = time(NULL);
        }
        now = mono_ms();
        if (now >= next_tick) {
            int anymount = 0;
            ticks++;
            check_daemon();
            if (cl_running) {
                record_transfers();
                if (ticks % 2 == 0) record_done();
                for (int i = 0; i < ACC_MAX; i++) {
                    Acc *a = &cl_acc[i];
                    if (!a->used) continue;
                    if (a->mounted) { anymount = 1; poll_acc(a); }
                    time_t t = time(NULL);
                    if ((!a->configured || !a->mounted) && t >= a->retry_at && strcmp(a->err, "cloud.addFailed")) bring_up(a);
                    if (a->pending_secret && t >= a->retry_at) {
                        char *s = strdup(a->pending_secret);
                        a->retry_at = t + 30;
                        if (s) store_secret(a, s);
                    }
                    if (a->configured && t - a->tok_at >= 300) token_check(a);
                    if (a->mounted && t - a->about_at >= 1800) about_acc(a);
                    if (a->rcjob_kind) rcjob_poll(a);
                    /* B14: a new account whose question nobody answered in 5 minutes goes away */
                    if (a->new_acc && a->pending_req && a->pending_since && t - a->pending_since > 300) { add_failed(a, 0, "cloud.signinTimeout", ""); continue; }
                    /* B6: a mount can be up while the provider refuses every listing (a certificate, a password that
                     * changed): the top folder is listed after mounting and every 10 minutes (2 while it fails) */
                    if (a->mounted && t >= a->check_at && !a->pending_req) check_acc(a);
                }
            }
            if (now >= next_slow) {                 /* every minute */
                next_slow = now + 60000;
                int b = on_battery();
                if (b != on_batt) { on_batt = b; cf_kick(); }
                if (cl_running) cf_measure();
                if (cl_running && opt.auto_free && ticks % 5 == 0) cf_auto_free(opt.free_below);
                if (ticks % 5 == 0) cf_kick();
            }
            /* nothing to serve: rclone goes away until it's needed */
            if (cl_running && !naccounts() && !si.on && time(NULL) - dmn.idle_since > IDLE_STOP_S) daemon_stop();
            next_tick = now + (anymount ? 1000 : 3000);
        }
        long long again = maybe_push();
        if (again > 0 && next_tick - mono_ms() > again) next_tick = mono_ms() + again;
    }
    return NULL;
}

/* ---- the API (main thread) ---- */

int cloud_start(const char *home_dir, const CloudSecrets *sec) {
    if (started) return 0;
    if (!home_dir || home_dir[0] != '/' || strlen(home_dir) > PATH_MAX - 64) return -1;
    started = 1;
    secrets = sec;
    cpy(cl_home, sizeof cl_home, home_dir);
    cl_pf(cl_root, sizeof cl_root, "%s/Cloud", cl_home);
    cl_pf(cl_cache, sizeof cl_cache, "%s/.cache/lordos/cloud", cl_home);
    cl_pf(cl_cfg, sizeof cl_cfg, "%s/.config/lordos/cloud", cl_home);
    mkdirs(cl_cfg, 0700);
    si.fo = si.fe = -1;
    load_all();
    efd_push = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    efd_job = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd_push < 0 || efd_job < 0) return -1;
    on_batt = on_battery();
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    int r = pthread_create(&th, &at, worker, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&at);
    if (r) return -1;
    cf_start();
    return 0;
}

void cloud_stop(void) {
    if (!started || worker_done) return;
    Job *j = new_job(J_STOP, "", NULL, NULL);
    if (j) push_job(j);
    /* the thread unmounts every account (each busy one lazily after 1.5 s) and waits for rclone: up to 15 s */
    for (int i = 0; i < 300 && !worker_done; i++) { struct timespec ts = { 0, 50 * 1000 * 1000 }; nanosleep(&ts, NULL); }
    /* whatever is still there (the thread was stuck in a call): detached here */
    for (int i = 0; i < ACC_MAX; i++) if (cl_acc[i].used) { char d[PATH_MAX]; mount_dir(&cl_acc[i], d, sizeof d); if (is_mounted(d)) fusermount_u(d); }
}

int cloud_fd(void) { return efd_push; }

void cloud_dispatch(void (*push)(const char *), void (*reply)(int, const char *), int (*open_url)(const char *)) {
    uint64_t v;
    ssize_t r = read(efd_push, &v, sizeof v);
    (void)r;
    reply_fn = reply;
    pthread_mutex_lock(&cl_mu);
    Reply *rs = rhead;
    rhead = NULL;
    char url[200] = "";
    if (si.on && si.url[0] && !si.opened && !strcmp(si.phase, "waiting")) { si.opened = 1; cpy(url, sizeof url, si.url); }
    pthread_mutex_unlock(&cl_mu);
    if (url[0] && open_url && open_url(url)) fprintf(stderr, "cloud: no default browser opened the sign-in page\n");
    /* oldest first */
    Reply *rev = NULL;
    while (rs) { Reply *n = rs->next; rs->next = rev; rev = rs; rs = n; }
    for (Reply *x = rev; x; ) {
        Reply *n = x->next;
        if (reply && x->json) reply(x->req, x->json);
        free(x->json);
        free(x);
        x = n;
    }
    char *s = state_json(0);
    if (s && push) push(s);
    free(s);
}

static int next_req(void) { static int seq; pthread_mutex_lock(&cl_mu); int r = ++seq; pthread_mutex_unlock(&cl_mu); return r; }

/* a job whose answer comes through reply(req, …) */
static char *async_job(int type, const char *id, const char *a, const char *b, int *req) {
    Job *j = new_job(type, id, a, b);
    if (!j) { *req = 0; return err_json("cloud.failed"); }
    j->req = *req = next_req();
    push_job(j);
    return NULL;
}

void cl_reply(int req, char *json) { post_reply(req, json); }
int  cl_next_req(void) { return next_req(); }

char *cloud_providers(int *req) {
    *req = 0;
    if (!cloud_available()) return err_json("cloud.unavailable");
    return async_job(J_PROVIDERS, "", NULL, NULL, req);
}

char *cloud_answer(const char *state, const char *value, int apply_others, int *req) {
    char id[16];
    const char *st;
    *req = 0;
    if (cloud_answer_state(state, id, sizeof id, &st)) return err_json("cloud.badArgument");
    if (!value || !cloud_value_ok(value)) return err_json("cloud.badValue");
    pthread_mutex_lock(&cl_mu);
    Acc *a = cl_find(id);
    int ok = a && a->pending_req;
    pthread_mutex_unlock(&cl_mu);
    if (!ok) return err_json(a ? "cloud.noQuestion" : "cloud.notFound");
    char *r = async_job(J_ANSWER, id, st, value, req);
    if (!r && !apply_others) {
        /* the job is queued: mark it (the queue's tail is ours) */
        pthread_mutex_lock(&cl_mu);
        if (jtail && jtail->req == *req) jtail->flag = 2;
        pthread_mutex_unlock(&cl_mu);
    }
    return r;
}

static char *acc_job(int type, const char *id, int *req) {
    *req = 0;
    pthread_mutex_lock(&cl_mu);
    Acc *a = cl_find(id);
    int tr = a ? a->trash_supported : -1;
    pthread_mutex_unlock(&cl_mu);
    if (!a) return err_json("cloud.notFound");
    if (type == J_TRASH && tr == 0) return err_json("unsupported");
    return async_job(type, id, NULL, NULL, req);
}
char *cloud_empty_trash(const char *id, int *req) { return acc_job(J_TRASH, id, req); }
char *cloud_measure(const char *id, int *req) { return acc_job(J_MEASURE, id, req); }
char *cloud_cancel_measure(const char *id, int *req) { return acc_job(J_CANCEL_MEASURE, id, req); }
char *cloud_delete(const char *const *paths, int n, int *req) { return cf_delete(paths, n, req); }

static int name_free(const char *name, const Acc *self) {
    for (int i = 0; i < ACC_MAX; i++) if (cl_acc[i].used && &cl_acc[i] != self && !strcasecmp(cl_acc[i].name, name)) return 0;
    if (si.on && !strcmp(si.phase, "waiting") && !strcasecmp(si.name, name)) return 0;
    return 1;
}

/* which existing account an add or a sign-in updates (with cl_mu held): the one named by its id (Sign In Again from the
 * account's page), else one of the same kind with that name, whatever its case (17.1); a name of another kind's account
 * is taken. NULL and *err "": a new account. Kinds compare by name: the overlay's entry and rclone's are the same kind. */
static Acc *account_for(const char *account_id, const char *name, const CloudKind *k, const char **err) {
    *err = "";
    if (account_id && account_id[0]) {
        Acc *a = cl_find(account_id);
        if (!a) { *err = "cloud.notFound"; return NULL; }
        if (strcmp(a->k->kind, k->kind)) { *err = "cloud.badKind"; return NULL; }
        return a;
    }
    for (int i = 0; name && i < ACC_MAX; i++)
        if (cl_acc[i].used && !strcasecmp(cl_acc[i].name, name)) {
            if (strcmp(cl_acc[i].k->kind, k->kind)) { *err = "cloud.nameTaken"; return NULL; }
            return &cl_acc[i];
        }
    return NULL;
}

char *cloud_begin_signin(const char *kind, const char *name, const char *cid, const char *csec, const char *account_id) {
    pthread_mutex_lock(&cl_mu);
    const CloudKind *k = kind_get(kind);
    pthread_mutex_unlock(&cl_mu);
    if (!cloud_available()) return err_json("cloud.unavailable");
    if (!k || strcmp(k->group, "oauth")) return err_json("cloud.badKind");
    int by_id = account_id && account_id[0];
    if (!by_id && !cloud_name_ok(name)) return err_json("cloud.badName");
    if ((cid && cid[0] && (strlen(cid) > 200 || !cloud_value_ok(cid))) || (csec && csec[0] && (strlen(csec) > 200 || !cloud_value_ok(csec)))) return err_json("cloud.badValue");
    pthread_mutex_lock(&cl_mu);
    if (si.on && !strcmp(si.phase, "waiting")) { pthread_mutex_unlock(&cl_mu); return err_json("cloud.busy"); }
    /* the name of an account of the same kind: Sign In Again for that account */
    char upd[16] = "", acc_name[CLOUD_NAME_MAX + 1];
    const char *why;
    Acc *ua = account_for(account_id, name, k, &why);
    if (why[0]) { pthread_mutex_unlock(&cl_mu); return err_json(why); }
    if (ua) { cpy(upd, sizeof upd, ua->id); cpy(acc_name, sizeof acc_name, ua->name); name = acc_name; }
    if (!upd[0] && !name_free(name, NULL)) { pthread_mutex_unlock(&cl_mu); return err_json("cloud.nameTaken"); }
    if (!upd[0] && naccounts() >= ACC_MAX) { pthread_mutex_unlock(&cl_mu); return err_json("cloud.tooMany"); }
    pthread_mutex_unlock(&cl_mu);
    int fo, fe;
    pid_t p = spawn_authorize(k, cid, csec, &fo, &fe);
    if (p < 0) return err_json("cloud.signinFailed");
    /* its first lines come within a few milliseconds: wait for the address up to 2 s, then the thread takes over */
    char url[200] = "", line[4096];
    size_t ll = 0;
    long long end = mono_ms() + 2000;
    while (!url[0] && mono_ms() < end) {
        struct pollfd pf = { fe, POLLIN, 0 };
        if (poll(&pf, 1, (int)(end - mono_ms())) <= 0) break;
        char b[1024];
        ssize_t n = read(fe, b, sizeof b);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n && !url[0]; i++) {
            if (b[i] == '\n' || ll + 1 >= sizeof line) { line[ll] = 0; cloud_auth_url(line, url, sizeof url); ll = 0; }
            else line[ll++] = b[i];
        }
    }
    pthread_mutex_lock(&cl_mu);
    memset(&si, 0, sizeof si);
    si.on = 1;
    si.id[0] = 's';
    rand_hex(si.id + 1, 4);
    cpy(si.kind, sizeof si.kind, k->kind);
    cpy(si.name, sizeof si.name, name);
    cpy(si.phase, sizeof si.phase, "waiting");
    cpy(si.url, sizeof si.url, url);
    cpy(si.cid, sizeof si.cid, cid);
    cpy(si.csec, sizeof si.csec, csec);
    cpy(si.update, sizeof si.update, upd);
    if (upd[0]) cpy(si.account, sizeof si.account, upd);
    si.expires_ms = now_ms() + SIGNIN_S * 1000LL;
    si.pid = p;
    si.fo = fo;
    si.fe = fe;
    si.opened = 0;
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"id\":");
    jb_str(&b, si.id);
    jb_raw(&b, ",\"url\":");
    if (url[0]) jb_str(&b, url); else jb_raw(&b, "null");
    jb_printf(&b, ",\"expiresAt\":%lld}", si.expires_ms);
    pthread_mutex_unlock(&cl_mu);
    Job *j = new_job(J_SIGNIN, "", NULL, NULL);     /* rclone itself, for config/create when the token comes */
    if (j) push_job(j);
    wake(efd_push);                                 /* the main loop opens the address */
    return b.oom ? (jb_free(&b), NULL) : b.p;
}

int cloud_signin_url(const char *id, char *url, size_t n) {
    pthread_mutex_lock(&cl_mu);
    int ok = si.on && id && !strcmp(si.id, id) && si.url[0] && !strcmp(si.phase, "waiting");
    if (ok) cpy(url, n, si.url);
    pthread_mutex_unlock(&cl_mu);
    return ok ? 0 : -1;
}

void cloud_cancel_signin(const char *id) {
    pthread_mutex_lock(&cl_mu);
    if (si.on && id && !strcmp(si.id, id)) {
        if (!strcmp(si.phase, "waiting") && si.pid <= 0) { pthread_mutex_unlock(&cl_mu); return; }   /* the token came: setting it up */
        signin_end("failed", "cloud.signinCancelled");
        si.on = 0;
    }
    pthread_mutex_unlock(&cl_mu);
    cl_changed();
}

char *cloud_add(const char *kind, const char *name, const char *fields, const char *account_id, int *req) {
    return add_impl(kind, name, fields, account_id, req, 0);
}

/* use_req > 0: called by the thread for an add that came before rclone's list of providers; the answer goes there */
static char *add_impl(const char *kind, const char *name, const char *fields, const char *account_id, int *req, int use_req) {
    *req = 0;
    pthread_mutex_lock(&cl_mu);
    int known = nkinfo > 0;
    pthread_mutex_unlock(&cl_mu);
    if (!known && !use_req && cloud_available() && kind && name && fields) {
        /* add before providers(): the thread asks rclone for the list first, then does it (no cloud.notReady) */
        Job *j = new_job(J_ADDLATE, "", name, account_id);
        if (!j) return err_json("cloud.failed");
        j->c = strdup(fields);
        j->d = strdup(kind);
        j->req = *req = next_req();
        push_job(j);
        return NULL;
    }
    pthread_mutex_lock(&cl_mu);
    const CloudKind *k = kind_get(kind);
    pthread_mutex_unlock(&cl_mu);
    if (!cloud_available()) return err_json("cloud.unavailable");
    if (!k || !strcmp(k->group, "oauth")) return err_json("cloud.badKind");
    int by_id = account_id && account_id[0];
    if (!by_id && !cloud_name_ok(name)) return err_json("cloud.badName");
    JDoc d;
    if (!fields || json_parse(fields, strlen(fields), &d, NULL, 0)) return err_json("cloud.badField");
    if (d.root->type != J_OBJ) { json_free(&d); return err_json("cloud.badField"); }
    pthread_mutex_lock(&cl_mu);
    const KindInfo *ki = kind_info(k->kind);
    char *res = NULL;
    Acc *a = NULL, *upd = NULL;
    JBuf sec;
    jb_init(&sec);
    jb_raw(&sec, "{");
    Params np = { .n = 0 };
    if (!ki) { res = err_json("cloud.notReady"); goto out; }
    /* the name of an account of the same kind: new credentials or settings for that account (same id, same folder) */
    const char *why;
    upd = account_for(account_id, name, k, &why);
    if (why[0]) { res = err_json(why); goto out; }
    if (!upd && !name_free(name, NULL)) { res = err_json("cloud.nameTaken"); goto out; }
    a = upd;
    for (int i = 0; i < ACC_MAX && !a; i++) if (!cl_acc[i].used) a = &cl_acc[i];
    if (!a) { res = err_json("cloud.tooMany"); goto out; }
    for (const JVal *x = d.root->kid; x; x = x->next) {
        int f = -1;
        for (int i = 0; i < ki->n; i++) if (!strcmp(ki->name[i], x->key)) f = i;
        const char *v = x->type == J_STR ? x->str : NULL;
        if (f < 0 || !v || !cloud_value_ok(v)) {
            JBuf e;
            jb_init(&e);
            jb_raw(&e, f < 0 ? "{\"error\":\"cloud.badField\",\"field\":" : "{\"error\":\"cloud.badValue\",\"field\":");
            jb_str(&e, x->key);
            jb_raw(&e, "}");
            res = e.oom ? (jb_free(&e), NULL) : e.p;
            a = NULL;
            goto out;
        }
        if (!v[0]) continue;
        /* an encrypted folder sits on one of this person's accounts, nowhere else */
        if (!strcmp(k->kind, "crypt") && !strcmp(x->key, "remote")) {
            char id[16];
            size_t l = strcspn(v, ":");
            if (l >= sizeof id || v[l] != ':' || !cloud_rel_ok(v + l + 1)) { res = err_json("cloud.badValue"); a = NULL; goto out; }
            memcpy(id, v, l);
            id[l] = 0;
            if (!cl_find(id)) { res = err_json("cloud.badValue"); a = NULL; goto out; }
        }
        if (ki->secret[f]) {
            if (sec.n > 1) jb_raw(&sec, ",");
            jb_str(&sec, x->key);
            jb_raw(&sec, ":");
            jb_str(&sec, v);
        } else kv_set(&np, x->key, v);
    }
    for (int i = 0; i < ki->n && !upd; i++) {
        if (!ki->required[i]) continue;
        const JVal *x = json_get(d.root, ki->name[i]);
        if (!x || x->type != J_STR || !x->str[0]) {
            JBuf e;
            jb_init(&e);
            jb_raw(&e, "{\"error\":\"cloud.missingField\",\"field\":");
            jb_str(&e, ki->name[i]);
            jb_raw(&e, "}");
            res = e.oom ? (jb_free(&e), NULL) : e.p;
            a = NULL;
            goto out;
        }
    }
    jb_raw(&sec, "}");
    if (upd) {
        for (int i = 0; i < np.n; i++) kv_set(&a->p, np.kv[i].key, np.kv[i].val);
        goto made;
    }
    memset(a, 0, sizeof *a);
    a->used = 1;
    a->k = k;
    a->links_supported = -1;
    a->created = time(NULL);
    a->id[0] = 'c';
    rand_hex(a->id + 1, 4);
    cpy(a->name, sizeof a->name, name);
    a->p = np;
    np.n = 0;
made:
    a->retry_at = time(NULL) + 3600;            /* the add job brings it up, not the tick */
    if (!upd) a->new_acc = 1;
out:
    pthread_mutex_unlock(&cl_mu);
    json_free(&d);
    kv_clear(&np);
    if (a && !res && !sec.oom) {
        /* the answer, { id } or one of rclone's questions, comes through reply(req) */
        Job *j = new_job(J_ADD, a->id, sec.p, NULL);
        if (j) { j->flag = upd != NULL; j->req = *req = use_req ? use_req : next_req(); push_job(j); }
        else res = err_json("cloud.failed");
    }
    if (!sec.oom) explicit_bzero(sec.p, sec.n);
    jb_free(&sec);
    cl_changed();
    return res;
}

char *cloud_remove(const char *id, int discard) {
    pthread_mutex_lock(&cl_mu);
    Acc *a = cl_find(id);
    if (!a) { pthread_mutex_unlock(&cl_mu); return err_json("cloud.notFound"); }
    if (a->nq && !discard) {
        char b[64];
        cl_pf(b, sizeof b, "{\"error\":\"pending\",\"files\":%d}", a->nq);
        pthread_mutex_unlock(&cl_mu);
        return strdup(b);
    }
    pthread_mutex_unlock(&cl_mu);
    Job *j = new_job(J_REMOVE, id, NULL, NULL);
    if (j) push_job(j);
    return strdup("{\"ok\":true}");
}

char *cloud_rename(const char *id, const char *name) {
    if (!cloud_name_ok(name)) return err_json("cloud.badName");
    pthread_mutex_lock(&cl_mu);
    Acc *a = cl_find(id);
    int bad = !a ? 1 : !name_free(name, a) ? 2 : 0;
    pthread_mutex_unlock(&cl_mu);
    if (bad) return err_json(bad == 1 ? "cloud.notFound" : "cloud.nameTaken");
    Job *j = new_job(J_RENAME, id, name, NULL);
    if (j) push_job(j);
    return strdup("{\"ok\":true}");
}

void cloud_refresh_about(const char *id) {
    pthread_mutex_lock(&cl_mu);
    Acc *a = cl_find(id);
    int down = a && (!a->configured || !a->mounted);
    pthread_mutex_unlock(&cl_mu);
    if (down) cloud_retry(id);              /* a cheap moment to try a waiting account again */
    Job *j = new_job(J_ABOUT, id, NULL, NULL);
    if (j) push_job(j);
}

/* the accounts that aren't connected (keyring was locked, the network was down…) try again now: after the keyring's
 * unlock event, or when the interface asks (cloud.retry). id NULL: all of them. */
void cloud_retry(const char *id) {
    if (!started) return;
    Job *j = new_job(J_RETRY, id ? id : "", NULL, NULL);
    if (j) push_job(j);
}

char *cloud_website(const char *id) {
    char out[400];
    pthread_mutex_lock(&cl_mu);
    Acc *a = cl_find(id);
    int ok = a && cloud_site_url(a->k, a->k->webfield ? kv_get(&a->p, a->k->webfield) : NULL, out, sizeof out);
    pthread_mutex_unlock(&cl_mu);
    return ok ? strdup(out) : NULL;
}

char *cloud_status(const char *const *paths, int n) { return cf_status(paths, n); }
void  cloud_browse(const char *path) { if (started) cf_browse(path); }
char *cloud_free(const char *const *paths, int n) { return cf_free(paths, n); }

void cloud_keep(const char *const *paths, int n, int keep) {
    int changed = 0;
    pthread_mutex_lock(&cl_mu);
    for (int i = 0; i < n; i++) {
        char name[CLOUD_NAME_MAX + 1], rel[CLOUD_REL_MAX + 1];
        if (cloud_split(cl_root, paths[i], name, sizeof name, rel, sizeof rel)) continue;
        Acc *a = cl_find_name(name);
        if (!a) continue;
        int at = -1;
        for (int k = 0; k < a->nkeep; k++) if (!strcmp(a->keep[k], rel)) at = k;
        /* a new kept entry is coming down until the keeper's first pass over it ends (#71) */
        if (keep && at < 0 && a->nkeep < KEEP_MAX) { char *d = strdup(rel); if (d) { a->keep_busy[a->nkeep] = 1; a->keep[a->nkeep++] = d; changed = 1; } }
        else if (!keep && at >= 0) {
            free(a->keep[at]);
            a->keep[at] = a->keep[--a->nkeep];
            a->keep_busy[at] = a->keep_busy[a->nkeep];
            a->keep_busy[a->nkeep] = 0;
            changed = 1;
        }
    }
    if (changed) cl_save_keep();
    pthread_mutex_unlock(&cl_mu);
    if (changed) { cf_kick(); cl_changed(); }
}

static char *link_req(const char *path, const char *expire, int *req, int unlink) {
    char name[CLOUD_NAME_MAX + 1], rel[CLOUD_REL_MAX + 1];
    *req = 0;
    if (cloud_split(cl_root, path, name, sizeof name, rel, sizeof rel) || !rel[0]) return err_json("cloud.badPath");
    char arg[16];
    long long secs;
    if (!unlink && cloud_expire(expire, arg, sizeof arg, &secs)) return err_json("cloud.badExpire");
    pthread_mutex_lock(&cl_mu);
    Acc *a = cl_find_name(name);
    int sup = a ? a->links_supported : 0;
    char id[16] = "";
    if (a) cpy(id, sizeof id, a->id);
    pthread_mutex_unlock(&cl_mu);
    if (!a) return err_json("cloud.notFound");
    if (sup == 0) return err_json("unsupported");
    Job *j = new_job(unlink ? J_UNLINK : J_LINK, id, rel, expire);
    if (!j) return err_json("cloud.failed");
    j->req = *req = next_req();
    push_job(j);
    return NULL;
}

char *cloud_link(const char *path, const char *expire, int *req) { return link_req(path, expire, req, 0); }
char *cloud_unlink(const char *path, int *req) { return link_req(path, NULL, req, 1); }

char *cloud_links(const char *id) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "[");
    int first = 1;
    long long now = now_ms();
    pthread_mutex_lock(&cl_mu);
    Acc *a = cl_find(id);
    for (Link *l = a ? links : NULL; l; l = l->next) {
        if (strcmp(l->id, a->id) || (l->expires && l->expires < now)) continue;
        char p[PATH_MAX * 2];
        cl_pf(p, sizeof p, "%s/%s/%s", cl_root, a->name, l->rel);
        jb_raw(&b, first ? "{\"path\":" : ",{\"path\":");
        first = 0;
        jb_str(&b, p);
        jb_raw(&b, ",\"url\":");
        jb_str(&b, l->url);
        jb_raw(&b, ",\"expires\":");
        if (l->expires) jb_printf(&b, "%lld", l->expires); else jb_raw(&b, "null");
        jb_printf(&b, ",\"made\":%lld}", l->made);
    }
    pthread_mutex_unlock(&cl_mu);
    jb_raw(&b, "]");
    return b.oom ? (jb_free(&b), NULL) : b.p;
}

void cloud_pause(const char *id, int pause) {
    pthread_mutex_lock(&cl_mu);
    if (!id) {
        opt.pause_all = pause;
        if (!pause) for (int i = 0; i < ACC_MAX; i++) cl_acc[i].paused = 0;
    } else {
        Acc *a = cl_find(id);
        if (a) a->paused = pause;
    }
    save_accounts();
    pthread_mutex_unlock(&cl_mu);
    cf_kick();
    cl_changed();
}

char *cloud_set_options(const char *json) {
    JDoc d;
    if (!json || json_parse(json, strlen(json), &d, NULL, 0) || d.root->type != J_OBJ) { if (json && d.root) json_free(&d); return err_json("cloud.badOption"); }
    pthread_mutex_lock(&cl_mu);
    long up = (long)json_num(d.root, "bwUp", (double)opt.bw_up), down = (long)json_num(d.root, "bwDown", (double)opt.bw_down);
    int fb = (int)json_num(d.root, "freeBelowGb", opt.free_below);
    char r[64];
    char *res;
    if (cloud_bw_rate(up, down, r, sizeof r) || fb < 5 || fb > 100) res = err_json("cloud.badOption");
    else {
        opt.bw_up = up;
        opt.bw_down = down;
        opt.free_below = fb;
        opt.pause_batt = json_bool(d.root, "pauseOnBattery", opt.pause_batt);
        opt.pause_metered = json_bool(d.root, "pauseOnMetered", opt.pause_metered);
        opt.auto_free = json_bool(d.root, "autoFree", opt.auto_free);
        save_accounts();
        res = strdup("{\"ok\":true}");
    }
    pthread_mutex_unlock(&cl_mu);
    json_free(&d);
    Job *j = new_job(J_OPTIONS, "", NULL, NULL);
    if (j) push_job(j);
    cl_changed();
    return res;
}

void cloud_restart(void) { Job *j = new_job(J_RESTART, "", NULL, NULL); if (j) push_job(j); }
