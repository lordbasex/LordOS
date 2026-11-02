/* maild.c — axia-maild: the helper that speaks IMAP, SMTP, CalDAV and CardDAV for Mail,
 * Calendar, Contacts and Reminders (see maild.h, proto.h and docs/MAIL.md).
 *
 * axia-server starts it with a fixed path, no arguments, a minimal environment and the channel on
 * fd 3. Before reading anything from the network it:
 *   - dies with axia-server (PR_SET_PDEATHSIG) and never dumps core;
 *   - loads the system CA bundle and the local time zone;
 *   - receives the cache folder as an open fd (MQ_INIT) and locks itself in with the shared
 *     helper sandbox (sandbox_apply_helper): Landlock with read-only /usr and the zoneinfo, write
 *     access only below the cache folder; seccomp without socket/connect/bind/exec/fork; no
 *     capabilities. It cannot open a single socket: every connection comes from axia-server,
 *     which only connects to the servers of the configured accounts. */
#include "maild.h"
#include "proto.h"
#include "ical.h"
#include "mime.h"
#include "../doc/proto.h"
#include "../sec/sandbox.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define SOCK 3

pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
int cache_fd = -1;
Account *accounts[MAIL_MAX_ACCOUNTS * 2];
int naccounts;
static pthread_mutex_t send_mu = PTHREAD_MUTEX_INITIALIZER;
static int debug;

void dbg(const char *fmt, ...) {
    if (!debug) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "axia-maild: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ---- channel ---- */

static void send_raw(uint32_t op, uint32_t id, const char *json, size_t len, const int *fds, int nfds) {
    pthread_mutex_lock(&send_mu);
    if (doc_send(SOCK, op, id, json, len, fds, nfds)) dbg("send failed: %s", strerror(errno));
    pthread_mutex_unlock(&send_mu);
}

void reply_done(uint32_t id, JBuf *b) {
    if (b->oom) { jb_free(b); reply_err(id, "too-big", ""); return; }
    send_raw(MR_DONE, id, b->p ? b->p : "{}", b->p ? b->n : 2, NULL, 0);
    jb_free(b);
}

void reply_err(uint32_t id, const char *code, const char *detail) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"error\":");
    jb_str(&b, code);
    jb_raw(&b, ",\"detail\":");
    jb_str(&b, detail ? detail : "");
    jb_raw(&b, "}");
    send_raw(MR_ERR, id, b.p, b.n, NULL, 0);
    jb_free(&b);
}

void send_event(JBuf *b) {
    if (!b->oom && b->p) send_raw(MR_EVT, 0, b->p, b->n, NULL, 0);
    jb_free(b);
}

void reply_image(uint32_t id, const uint32_t *argb, int w, int h) {
    size_t bytes = (size_t)w * (size_t)h * 4;
    int fd = doc_memfd("axia-maild-img", argb, bytes);
    if (fd < 0) { reply_err(id, "image", "memfd"); return; }
    char j[64];
    snprintf(j, sizeof j, "{\"w\":%d,\"h\":%d}", w, h);
    send_raw(MR_IMG, id, j, strlen(j), &fd, 1);
    close(fd);
}

/* ---- connections through axia-server ---- */

typedef struct { uint32_t cid; int done, fd; char why[24]; } Pend;
static Pend pend[64];
static pthread_mutex_t pend_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pend_cv = PTHREAD_COND_INITIALIZER;
static uint32_t next_cid = 1;

int net_connect(const char *host, int port, char *why, size_t wcap) {
    pthread_mutex_lock(&pend_mu);
    int slot = -1;
    for (int i = 0; i < 64; i++) if (!pend[i].cid) { slot = i; break; }
    if (slot < 0) { pthread_mutex_unlock(&pend_mu); snprintf(why, wcap, "busy"); return -1; }
    uint32_t cid = next_cid++;
    pend[slot] = (Pend){ cid, 0, -1, "" };
    pthread_mutex_unlock(&pend_mu);
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"host\":");
    jb_str(&b, host);
    jb_printf(&b, ",\"port\":%d}", port);
    send_raw(MR_CONNECT, cid, b.p, b.n, NULL, 0);
    jb_free(&b);
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += 40;
    pthread_mutex_lock(&pend_mu);
    while (!pend[slot].done) if (pthread_cond_timedwait(&pend_cv, &pend_mu, &dl) == ETIMEDOUT) break;
    int fd = pend[slot].done ? pend[slot].fd : -1;
    snprintf(why, wcap, "%s", pend[slot].done ? pend[slot].why : "timeout");
    pend[slot].cid = 0;
    pthread_mutex_unlock(&pend_mu);
    return fd;
}

static void on_sock(DocMsg *m) {
    pthread_mutex_lock(&pend_mu);
    for (int i = 0; i < 64; i++) {
        if (pend[i].cid != m->h.id) continue;
        pend[i].done = 1;
        if (m->nfds >= 1) { pend[i].fd = m->fds[0]; m->fds[0] = -1; }
        else {
            JDoc d;
            snprintf(pend[i].why, sizeof pend[i].why, "connect");
            if (m->json && !json_parse(m->json, m->len, &d, NULL, 0)) {
                const char *e = json_str(d.root, "error");
                if (e) snprintf(pend[i].why, sizeof pend[i].why, "%s", e);
                json_free(&d);
            }
        }
    }
    pthread_cond_broadcast(&pend_cv);
    pthread_mutex_unlock(&pend_mu);
}

Conn *open_conn(const char *acct, int smtp, const char *host, int port, int tls_now, const char *pin, TlsInfo *ti, char *why, size_t wcap) {
    int fd = net_connect(host, port, why, wcap);
    if (fd < 0) return NULL;
    Conn *c = conn_new(fd, host, 60000);
    if (!c) { close(fd); snprintf(why, wcap, "memory"); return NULL; }
    if (tls_now) {
        if (conn_tls(c, pin, ti)) {
            snprintf(why, wcap, "%s", ti->code ? "cert" : "tls");
            if (ti->code) {
                JBuf b;
                jb_init(&b);
                jb_raw(&b, "{\"ev\":\"cert\",\"acct\":"); jb_str(&b, acct);
                jb_raw(&b, ",\"host\":"); jb_str(&b, host);
                jb_printf(&b, ",\"port\":%d,%s\"reason\":", port, smtp ? "\"smtp\":true," : ""); jb_str(&b, ti->reason);
                jb_raw(&b, ",\"subject\":"); jb_str(&b, ti->subject);
                jb_raw(&b, ",\"issuer\":"); jb_str(&b, ti->issuer);
                jb_raw(&b, ",\"sha256\":"); jb_str(&b, ti->sha256);
                jb_raw(&b, ",\"notAfter\":"); jb_str(&b, ti->not_after);
                jb_raw(&b, "}");
                send_event(&b);
            }
            conn_close(c);
            return NULL;
        }
    }
    return c;
}

/* ---- files ---- */

int fs_mkdirs(const char *rel) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", rel);
    for (char *s = tmp + 1; *s; s++) if (*s == '/') { *s = 0; mkdirat(cache_fd, tmp, 0700); *s = '/'; }
    return mkdirat(cache_fd, tmp, 0700) && errno != EEXIST ? -1 : 0;
}

char *fs_read(const char *rel, size_t *len, size_t max) {
    *len = 0;
    int fd = openat(cache_fd, rel, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size > max) { close(fd); return NULL; }
    char *b = malloc((size_t)st.st_size + 1);
    if (!b) { close(fd); return NULL; }
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t r = read(fd, b + got, (size_t)st.st_size - got);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
        got += (size_t)r;
    }
    close(fd);
    b[got] = 0;
    *len = got;
    return b;
}

int fs_write(const char *rel, const void *data, size_t len) {
    char tmp[1100];
    static unsigned seq;
    snprintf(tmp, sizeof tmp, "%s.%u.%u.tmp", rel, (unsigned)getpid(), __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED));
    int fd = openat(cache_fd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        /* parent folder missing: create it */
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", rel);
        char *sl = strrchr(dir, '/');
        if (sl) { *sl = 0; fs_mkdirs(dir); }
        fd = openat(cache_fd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) return -1;
    }
    const char *p = data;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; close(fd); unlinkat(cache_fd, tmp, 0); return -1; }
        off += (size_t)w;
    }
    close(fd);
    if (renameat(cache_fd, tmp, cache_fd, rel)) { unlinkat(cache_fd, tmp, 0); return -1; }
    return 0;
}

int fs_exists(const char *rel) { struct stat st; return fstatat(cache_fd, rel, &st, AT_SYMLINK_NOFOLLOW) == 0; }
int fs_unlink(const char *rel) { return unlinkat(cache_fd, rel, 0); }

/* Files only: the sandbox lets us create folders but not remove them (no REMOVE_DIR). */
int fs_rmtree(const char *rel) {
    int dfd = openat(cache_fd, rel, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dfd < 0) return -1;
    DIR *d = fdopendir(dfd);
    if (!d) { close(dfd); return -1; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[1200];
        snprintf(sub, sizeof sub, "%s/%s", rel, e->d_name);
        if (e->d_type == DT_DIR) fs_rmtree(sub);
        else unlinkat(cache_fd, sub, 0);
    }
    closedir(d);
    return 0;
}

void fs_safe_name(const char *in, char *out, size_t cap) { mime_safe_filename(in, out, cap); }

void fs_hash(const char *s, char *out) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    snprintf(out, 17, "%016llx", (unsigned long long)h);
}

/* ---- jobs ---- */

Job *job_new(int kind, uint32_t req, int prio) {
    Job *j = calloc(1, sizeof *j);
    if (j) { j->kind = kind; j->req = req; j->prio = prio; }
    return j;
}

void job_free(Job *j) {
    if (!j) return;
    for (int i = 0; i < j->nfds; i++) if (j->fds[i] >= 0) close(j->fds[i]);
    free(j->uids);
    free(j->arg);
    free(j);
}

void job_push(Account *a, Job *j) {
    pthread_mutex_lock(&a->qmu);
    /* higher priority first; same priority in order */
    Job **p = &a->jobs;
    while (*p && (*p)->prio >= j->prio) p = &(*p)->next;
    j->next = *p;
    *p = j;
    pthread_mutex_unlock(&a->qmu);
    uint64_t one = 1;
    if (write(a->wake, &one, sizeof one) < 0) { /* already signalled */ }
}

Job *job_pop(Account *a) {
    pthread_mutex_lock(&a->qmu);
    Job *j = a->jobs;
    if (j) a->jobs = j->next;
    pthread_mutex_unlock(&a->qmu);
    return j;
}

/* ---- accounts ---- */

Account *acct_get(const char *id) {
    if (!id) return NULL;
    for (int i = 0; i < naccounts; i++) if (accounts[i] && !accounts[i]->stop && !strcmp(accounts[i]->id, id)) return accounts[i];
    return NULL;
}

void acct_status(Account *a, const char *status, const char *err) {
    pthread_mutex_lock(&G);
    snprintf(a->status, sizeof a->status, "%s", status);
    if (err) snprintf(a->last_error, sizeof a->last_error, "%s", err);
    pthread_mutex_unlock(&G);
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"ev\":\"status\",\"acct\":"); jb_str(&b, a->id);
    jb_raw(&b, ",\"status\":"); jb_str(&b, status);
    jb_raw(&b, ",\"error\":"); jb_str(&b, err ? err : "");
    jb_raw(&b, "}");
    send_event(&b);
}

void acct_set_password(Account *a, const char *pw) {
    pthread_mutex_lock(&G);
    if (a->password) { explicit_bzero(a->password, strlen(a->password)); free(a->password); }
    a->password = pw ? strdup(pw) : NULL;
    a->need_auth_sent = 0;
    pthread_mutex_unlock(&G);
    Job *j = job_new(a->kind[0] == 'd' ? J_DAV_SYNC : J_SYNC_ALL, 0, 1);
    if (j) job_push(a, j);
}

static int valid_id(const char *s) {
    if (!s || !*s || strlen(s) > 32) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return 0;
    return 1;
}

static void server_from(const JVal *o, Server *s, int defport) {
    memset(s, 0, sizeof *s);
    if (!o) return;
    const char *h = json_str(o, "host");
    if (h) snprintf(s->host, sizeof s->host, "%s", h);
    s->port = (int)json_num(o, "port", defport);
    if (s->port <= 0 || s->port > 65535) s->port = defport;
    const char *sec = json_str(o, "security");
    s->sec = sec && !strcmp(sec, "starttls") ? SEC_STARTTLS : sec && !strcmp(sec, "none") ? SEC_NONE : SEC_SSL;
}

#define CPY(dst, src) snprintf(dst, sizeof dst, "%s", (src) ? (src) : "")

void accounts_apply_config(const JVal *cfg) {
    const JVal *acc = json_get(cfg, "accounts");
    int seen[MAIL_MAX_ACCOUNTS * 2] = { 0 };
    for (const JVal *a = acc && acc->type == J_ARR ? acc->kid : NULL; a; a = a->next) {
        const char *id = json_str(a, "id");
        if (!valid_id(id)) continue;
        const char *kind = json_str(a, "kind");
        Account tmp;
        memset(&tmp, 0, sizeof tmp);
        CPY(tmp.id, id);
        CPY(tmp.kind, kind && !strcmp(kind, "dav") ? "dav" : "mail");
        CPY(tmp.name, json_str(a, "name"));
        CPY(tmp.email, json_str(a, "email"));
        CPY(tmp.full_name, json_str(a, "fullName"));
        CPY(tmp.user, json_str(a, "user"));
        CPY(tmp.auth, json_str(a, "auth") ? json_str(a, "auth") : "password");
        server_from(json_get(a, "imap"), &tmp.imap, 993);
        server_from(json_get(a, "smtp"), &tmp.smtp, 465);
        tmp.sent_saved = json_bool(a, "sentSaved", 0);
        const JVal *trust = json_get(a, "trust");
        CPY(tmp.pin_imap, json_str(trust, "imap"));
        CPY(tmp.pin_smtp, json_str(trust, "smtp"));
        CPY(tmp.pin_dav, json_str(trust, "dav"));
        CPY(tmp.url, json_str(a, "url"));
        tmp.cal = json_bool(a, "cal", 1);
        tmp.card = json_bool(a, "card", 1);
        Account *cur = acct_get(id);
        int same = cur && !strcmp(cur->kind, tmp.kind) && !strcmp(cur->user, tmp.user) && !strcmp(cur->imap.host, tmp.imap.host) &&
                   cur->imap.port == tmp.imap.port && cur->imap.sec == tmp.imap.sec && !strcmp(cur->url, tmp.url) &&
                   !strcmp(cur->pin_imap, tmp.pin_imap) && !strcmp(cur->pin_dav, tmp.pin_dav);
        if (cur && same) {
            /* light changes (name, signature, SMTP): in place */
            pthread_mutex_lock(&G);
            CPY(cur->name, tmp.name); CPY(cur->email, tmp.email); CPY(cur->full_name, tmp.full_name);
            cur->smtp = tmp.smtp; cur->sent_saved = tmp.sent_saved; CPY(cur->pin_smtp, tmp.pin_smtp);
            cur->cal = tmp.cal; cur->card = tmp.card;
            pthread_mutex_unlock(&G);
            for (int i = 0; i < naccounts; i++) if (accounts[i] == cur) seen[i] = 1;
            continue;
        }
        char *keep_pw = NULL;
        if (cur) {
            /* server settings changed: retire the old worker (it exits on its own) */
            if (cur->password && !strcmp(cur->user, tmp.user)) keep_pw = strdup(cur->password);
            cur->stop = 1;
            uint64_t one = 1;
            if (write(cur->wake, &one, sizeof one) < 0) { /* signalled */ }
        }
        if (naccounts >= (int)(sizeof accounts / sizeof accounts[0])) { free(keep_pw); continue; }
        Account *n = calloc(1, sizeof *n);
        if (!n) { free(keep_pw); continue; }
        *n = tmp;
        n->password = keep_pw;
        n->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        pthread_mutex_init(&n->qmu, NULL);
        snprintf(n->status, sizeof n->status, "idle");
        pthread_mutex_lock(&G);
        accounts[naccounts] = n;
        seen[naccounts] = 1;
        naccounts++;
        if (!strcmp(n->kind, "mail")) store_load(n);
        pthread_mutex_unlock(&G);
        if (pthread_create(&n->thread, NULL, !strcmp(n->kind, "mail") ? mail_worker : dav_worker, n) == 0) { n->started = 1; pthread_detach(n->thread); }
    }
    /* accounts no longer in the file */
    for (int i = 0; i < naccounts; i++) {
        if (seen[i] || !accounts[i] || accounts[i]->stop) continue;
        Account *a = accounts[i];
        a->stop = 1;
        uint64_t one = 1;
        if (write(a->wake, &one, sizeof one) < 0) { /* signalled */ }
        pthread_mutex_lock(&G);
        if (a->password) { explicit_bzero(a->password, strlen(a->password)); free(a->password); a->password = NULL; }
        pthread_mutex_unlock(&G);
        store_drop_account(a->id);
        pim_account_removed(a->id);
    }
}

static void status_json(JBuf *b) {
    jb_raw(b, "{\"accounts\":[");
    int first = 1;
    pthread_mutex_lock(&G);
    for (int i = 0; i < naccounts; i++) {
        Account *a = accounts[i];
        if (!a || a->stop) continue;
        if (!first) jb_raw(b, ",");
        first = 0;
        jb_raw(b, "{\"id\":"); jb_str(b, a->id);
        jb_raw(b, ",\"kind\":"); jb_str(b, a->kind);
        jb_raw(b, ",\"status\":"); jb_str(b, a->status);
        jb_raw(b, ",\"error\":"); jb_str(b, a->last_error);
        jb_printf(b, ",\"hasPassword\":%s,\"lastSync\":%lld}", a->password ? "true" : "false", (long long)a->last_sync * 1000);
    }
    pthread_mutex_unlock(&G);
    jb_raw(b, "]}");
}

/* ---- requests ---- */

static Account *req_acct(uint32_t id, const JVal *args) {
    Account *a = acct_get(json_str(args, "acct"));
    if (!a) reply_err(id, "no-account", json_str(args, "acct"));
    return a;
}

static uint32_t *uids_of(const JVal *args, int *n) {
    *n = 0;
    const JVal *u = json_get(args, "uids");
    if (!u || u->type != J_ARR || !u->len || u->len > 100000) return NULL;
    uint32_t *out = malloc(u->len * sizeof *out);
    if (!out) return NULL;
    for (const JVal *k = u->kid; k; k = k->next) if (k->type == J_NUM && k->num >= 1 && k->num <= 4294967295.0) out[(*n)++] = (uint32_t)k->num;
    return out;
}

/* Requests that may wait on the network run on their own thread: the main thread is the one
 * that receives axia-server's answers (connections included), so it must never block. */
typedef struct { uint32_t id; char *json; size_t len; } Bg;
static void *bg_image(void *arg) {
    Bg *b = arg;
    JDoc d;
    if (!json_parse(b->json, b->len, &d, NULL, 0)) { msg_image(b->id, d.root); json_free(&d); }
    else reply_err(b->id, "bad-json", "");
    free(b->json);
    free(b);
    return NULL;
}

static void handle_request(uint32_t id, const char *json, size_t len, int *fds, int nfds) {
    JDoc d;
    char perr[96];
    if (!json || json_parse(json, len, &d, perr, sizeof perr)) { reply_err(id, "bad-json", perr); return; }
    const JVal *args = d.root;
    const char *op = json_str(args, "op");
    if (!op) op = "";
    JBuf b;
    jb_init(&b);
    if (!strcmp(op, "ping")) { jb_raw(&b, "{\"ok\":true}"); reply_done(id, &b); }
    else if (!strcmp(op, "status")) { status_json(&b); reply_done(id, &b); }
    else if (!strcmp(op, "auth")) {
        Account *a = req_acct(id, args);
        const char *pw = json_str(args, "password");
        if (a) { acct_set_password(a, pw); jb_raw(&b, "{\"ok\":true}"); reply_done(id, &b); }
    } else if (!strcmp(op, "folders")) { pthread_mutex_lock(&G); store_folders_json(&b); pthread_mutex_unlock(&G); reply_done(id, &b); }
    else if (!strcmp(op, "counts")) { pthread_mutex_lock(&G); store_counts_json(&b); pthread_mutex_unlock(&G); reply_done(id, &b); }
    else if (!strcmp(op, "list")) { pthread_mutex_lock(&G); store_list_json(&b, args); pthread_mutex_unlock(&G); reply_done(id, &b); }
    else if (!strcmp(op, "thread")) { pthread_mutex_lock(&G); store_thread_json(&b, args); pthread_mutex_unlock(&G); reply_done(id, &b); }
    else if (!strcmp(op, "search")) { pthread_mutex_lock(&G); store_search_json(&b, args); pthread_mutex_unlock(&G); reply_done(id, &b); }
    else if (!strcmp(op, "open")) msg_open_json(id, args);
    else if (!strcmp(op, "attachment")) msg_attachment(id, args);
    else if (!strcmp(op, "image")) {
        Bg *bg = malloc(sizeof *bg);
        pthread_t th;
        if (bg && (bg->json = strndup(json, len))) {
            bg->id = id;
            bg->len = len;
            if (!pthread_create(&th, NULL, bg_image, bg)) pthread_detach(th);
            else { free(bg->json); free(bg); reply_err(id, "busy", ""); }
        } else { free(bg); reply_err(id, "memory", ""); }
    }
    else if (!strcmp(op, "invite")) msg_invite(id, args);
    else if (!strcmp(op, "invite-respond")) msg_invite_respond(id, args);
    else if (!strcmp(op, "autocomplete")) {
        const char *q = json_str(args, "q");
        int count = 0;
        jb_raw(&b, "{\"items\":[");
        pthread_mutex_lock(&G);
        pim_autocomplete(q ? q : "", &b, &count, 12);
        store_autocomplete(q ? q : "", &b, &count, 12);
        pthread_mutex_unlock(&G);
        jb_raw(&b, "]}");
        reply_done(id, &b);
    } else if (!strcmp(op, "sync")) {
        const char *acct = json_str(args, "acct");
        const char *folder = json_str(args, "folder");
        for (int i = 0; i < naccounts; i++) {
            Account *a = accounts[i];
            if (!a || a->stop || (acct && strcmp(acct, a->id))) continue;
            Job *j = job_new(a->kind[0] == 'd' ? J_DAV_SYNC : folder ? J_SYNC_FOLDER : J_SYNC_ALL, 0, folder ? 5 : 1);
            if (!j) continue;
            if (folder) snprintf(j->folder, sizeof j->folder, "%s", folder);
            job_push(a, j);
        }
        jb_raw(&b, "{\"ok\":true}");
        reply_done(id, &b);
    } else if (!strcmp(op, "flag") || !strcmp(op, "move") || !strcmp(op, "delete")) {
        Account *a = req_acct(id, args);
        const char *folder = json_str(args, "folder");
        if (a && folder) {
            int n;
            uint32_t *uids = uids_of(args, &n);
            if (!uids || !n) { free(uids); reply_err(id, "no-messages", ""); json_free(&d); return; }
            Job *j = job_new(!strcmp(op, "flag") ? J_FLAGS : !strcmp(op, "move") ? J_MOVE : J_DELETE, id, 8);
            if (!j) { free(uids); reply_err(id, "memory", ""); json_free(&d); return; }
            snprintf(j->folder, sizeof j->folder, "%s", folder);
            j->uids = uids;
            j->nuids = n;
            j->arg = strndup(json, len);
            j->arglen = len;
            /* flags change locally right away: the list updates without waiting for the server */
            if (j->kind == J_FLAGS) {
                pthread_mutex_lock(&G);
                Folder *f = store_folder(a, folder, 0);
                const JVal *add = json_get(args, "add"), *rem = json_get(args, "remove");
                static const struct { const char *n; uint32_t bit; } fl[] = {
                    { "seen", F_SEEN }, { "flagged", F_FLAGGED }, { "answered", F_ANSWERED }, { "forwarded", F_FORWARDED }, { "junk", F_JUNK },
                    { "label1", F_LABEL1 }, { "label2", F_LABEL1 << 1 }, { "label3", F_LABEL1 << 2 }, { "label4", F_LABEL1 << 3 }, { "label5", F_LABEL1 << 4 }, { "label6", F_LABEL1 << 5 } };
                for (int k = 0; f && k < n; k++) {
                    Msg *m = store_msg(f, uids[k]);
                    if (!m) continue;
                    uint32_t old = m->flags;
                    for (const JVal *x = add && add->type == J_ARR ? add->kid : NULL; x; x = x->next)
                        for (size_t q = 0; q < sizeof fl / sizeof fl[0]; q++) if (x->type == J_STR && !strcmp(x->str, fl[q].n)) m->flags |= fl[q].bit;
                    for (const JVal *x = rem && rem->type == J_ARR ? rem->kid : NULL; x; x = x->next)
                        for (size_t q = 0; q < sizeof fl / sizeof fl[0]; q++) if (x->type == J_STR && !strcmp(x->str, fl[q].n)) m->flags &= ~fl[q].bit;
                    if ((old ^ m->flags) & F_SEEN) f->unread += (m->flags & F_SEEN) ? -1 : 1;
                    if (old != m->flags) { f->dirty = 1; f->version++; }
                }
                if (f) store_save(a, f);
                pthread_mutex_unlock(&G);
                JBuf e;
                jb_init(&e);
                jb_raw(&e, "{\"ev\":\"changed\",\"acct\":"); jb_str(&e, a->id);
                jb_raw(&e, ",\"folder\":"); jb_str(&e, folder);
                jb_raw(&e, "}");
                send_event(&e);
            }
            job_push(a, j);
        } else if (a) reply_err(id, "no-folder", "");
    } else if (!strcmp(op, "send") || !strcmp(op, "draft")) {
        Account *a = req_acct(id, args);
        if (a) {
            Job *j = job_new(!strcmp(op, "send") ? J_SEND : J_APPEND_DRAFT, id, 9);
            if (!j) { reply_err(id, "memory", ""); }
            else {
                j->arg = strndup(json, len);
                j->arglen = len;
                for (int i = 0; i < nfds && i < 15; i++) { j->fds[j->nfds++] = fds[i]; fds[i] = -1; }
                snprintf(j->folder, sizeof j->folder, "%s", a->id);
                if (j->kind == J_SEND) sender_queue(j); else job_push(a, j);
            }
        }
    } else if (!strcmp(op, "create-folder")) {
        Account *a = req_acct(id, args);
        const char *name = json_str(args, "name");
        if (a && name && *name && strlen(name) < 200) {
            Job *j = job_new(J_CREATE_FOLDER, id, 7);
            if (j) { snprintf(j->folder, sizeof j->folder, "%s", name); job_push(a, j); }
        } else if (a) reply_err(id, "bad-name", "");
    } else if (!strcmp(op, "alarms")) {
        double from = json_num(args, "from", 0) / 1000, to = json_num(args, "to", 0) / 1000;
        pim_alarms_json(&b, (int64_t)from, (int64_t)to);
        reply_done(id, &b);
    } else if (!strncmp(op, "cal-", 4) || !strncmp(op, "ev-", 3) || !strncmp(op, "todo-", 5) || !strncmp(op, "book-", 5) || !strncmp(op, "contact-", 8)) {
        pim_handle(id, op, args, fds, nfds);
    } else reply_err(id, "unknown-op", op);
    jb_free(&b);
    json_free(&d);
}

/* ---- start ---- */

static void enter_sandbox(void) {
    static const char *ro[] = { "/usr/share/zoneinfo", NULL };
    int rw[] = { cache_fd, -1 };
    SandboxHelperCfg cfg = {
        .name = "lordos-maild",          /* cgroup scope lordos-maild.scope (lord-sessiond creates it) */
        .ro_paths = ro,
        .extra_syscalls = (int[]){ SYS_getcpu, SYS_membarrier, -1 },
        .allow_ioctl_classes = 0,
        .mem_max_mb = 768,
        .ro_dir_fds = NULL,
        .rw_dir_fds = rw,
    };
    /* syncing is background work: it must not compete with the desktop and its sound right after
     * login (0.8.1; the first full sync starts at once). nice 19 and idle I/O, set before the
     * sandbox, whose seccomp filter doesn't allow ioprio_set */
    setpriority(PRIO_PROCESS, 0, 19);
    syscall(SYS_ioprio_set, 1, 0, 3 << 13);         /* IOPRIO_WHO_PROCESS, this process, IOPRIO_CLASS_IDLE */
    int mask = sandbox_apply_helper(&cfg);
    dbg("sandbox mask %#x", (unsigned)mask);
}

int main(void) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    prctl(PR_SET_DUMPABLE, 0);
    struct rlimit nocore = { 0, 0 };
    setrlimit(RLIMIT_CORE, &nocore);
    signal(SIGPIPE, SIG_IGN);
    debug = getenv("LORD_MAIL_DEBUG") != NULL;
    struct stat st;
    if (fstat(SOCK, &st) || !S_ISSOCK(st.st_mode)) { fprintf(stderr, "axia-maild: started by axia-server only\n"); return 2; }
    if (net_init()) { fprintf(stderr, "axia-maild: TLS init failed\n"); return 3; }
    tzset();
    DocMsg m;
    if (doc_recv(SOCK, &m) || m.h.op != MQ_INIT || m.nfds != 1) { fprintf(stderr, "axia-maild: bad start\n"); return 4; }
    cache_fd = m.fds[0];
    m.fds[0] = -1;
    JDoc init;
    int have_init = m.json && !json_parse(m.json, m.len, &init, NULL, 0);
    if (have_init) ic_set_local_zone(json_str(init.root, "zone"));
    enter_sandbox();
    pim_init();
    sender_start();
    if (have_init) {
        const JVal *cfg = json_get(init.root, "config");
        if (cfg) accounts_apply_config(cfg);
        json_free(&init);
    }
    doc_msg_free(&m);
    JBuf ready;
    jb_init(&ready);
    jb_raw(&ready, "{\"ev\":\"ready\"}");
    send_event(&ready);
    for (;;) {
        DocMsg q;
        int r = doc_recv(SOCK, &q);
        if (r) break;
        switch (q.h.op) {
        case MQ_REQ: handle_request(q.h.id, q.json, q.len, q.fds, q.nfds); break;
        case MQ_SOCK: on_sock(&q); break;
        case MQ_CONFIG: {
            JDoc d;
            if (q.json && !json_parse(q.json, q.len, &d, NULL, 0)) {
                const JVal *cfg = json_get(d.root, "config");
                if (cfg) accounts_apply_config(cfg);
                json_free(&d);
            }
            break;
        }
        default: break;
        }
        doc_msg_free(&q);
    }
    return 0;
}
