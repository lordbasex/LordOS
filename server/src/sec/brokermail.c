/* brokermail.c — axia-maild for the isolated system apps (BOP_MAIL, BOP_SYS_OPEN).
 *
 * Mail, Calendar, Contacts and Reminders run as their own sandboxed process
 * (`axia-server --app <id>`, the system package in assets/sysapps). Their process has no network
 * and cannot read the mail cache or the accounts file; it reaches axia-maild only through here, and
 * axia-server keeps every rule it applied when these apps ran in-process (js_mail.c):
 *
 *   - only a system package (brokersrv_conn_system) named mail, calendar, contacts or reminders;
 *   - each app may send only its own requests: Mail all of them, Calendar `cal-*`/`ev-*`/`todo-*`,
 *     Reminders `cal-*`/`todo-*`, Contacts `book-*`/`contact-*`/`autocomplete` (so a compromised
 *     Calendar cannot read mail);
 *   - files reach axia-maild only as fds the app already holds (from the system file dialog), and
 *     only regular files of at most 64 MB, only for the requests that take files;
 *   - replies, decoded images and events belong to that app's owner queue in mailc.c; Calendar,
 *     Contacts and Reminders get only the PIM events (no senders or subjects);
 *   - the accounts file (which servers axia-maild may reach) and remote-image grants: Mail only,
 *     with the same validation as lord.mail.saveConfig / allowImages;
 *   - attachments: Mail gets a read-only fd of one cached attachment (to save it through the
 *     system dialog) or asks the session to open it (the session opens it with the user's apps).
 *
 * Replies and events go back as "mail" events in chunks of at most CHUNK bytes ('C' = more follow,
 * 'F' = last), queued per app and sent without ever blocking the compositor. */
#include "broker_int.h"
#include "json.h"
#include "../mail/mailc.h"
#include "../mail/proto.h"
#include "../errcode.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_CONN 32                 /* brokersrv.c MAX_APPS */
#define CHUNK    (60 * 1024)        /* below the default SEQPACKET send buffer */
#define REQ_MAX  (24u << 20)        /* js_mail.c MAIL_REQ_MAX */
#define FILE_MAX ((off_t)64 << 20)

enum { ROLE_NONE = 0, ROLE_MAIL, ROLE_CALENDAR, ROLE_REMINDERS, ROLE_CONTACTS };

typedef struct {
    int owner;                      /* mailc owner, 0 = not opened yet */
    int fd;                         /* broker socket (to send events) */
    char *out;                      /* the batch being sent */
    size_t len, off;
} MailConn;
static MailConn mc[MAX_CONN];

static int role_of(int conn, const char *app) {
    if (!brokersrv_conn_system(conn) || !app) return ROLE_NONE;
    if (!strcmp(app, "mail")) return ROLE_MAIL;
    if (!strcmp(app, "calendar")) return ROLE_CALENDAR;
    if (!strcmp(app, "reminders")) return ROLE_REMINDERS;
    if (!strcmp(app, "contacts")) return ROLE_CONTACTS;
    return ROLE_NONE;
}

static int prefix(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

/* May `role` send request `op`? */
static int op_allowed(int role, const char *op) {
    if (!strcmp(op, "ping") || !strcmp(op, "status")) return 1;
    switch (role) {
    case ROLE_MAIL: return 1;
    case ROLE_CALENDAR: return prefix(op, "cal-") || prefix(op, "ev-") || prefix(op, "todo-") || !strcmp(op, "alarms");
    case ROLE_REMINDERS: return prefix(op, "cal-") || prefix(op, "todo-") || !strcmp(op, "alarms");
    case ROLE_CONTACTS: return prefix(op, "book-") || prefix(op, "contact-") || !strcmp(op, "autocomplete");
    default: return 0;
    }
}

/* Requests that take files (attachments, a contact photo, an .ics/.vcf to import). */
static int op_takes_files(const char *op) {
    return !strcmp(op, "send") || !strcmp(op, "draft") || !strcmp(op, "contact-save") || !strcmp(op, "contact-import") || !strcmp(op, "ev-import");
}

static void reply(int fd, uint32_t reqid, int st, const char *s) { brokersrv_reply(fd, reqid, st, s, s ? (uint32_t)strlen(s) : 0, -1); }
static void reply_code(int fd, uint32_t reqid, const char *code) { reply(fd, reqid, BST_ERROR, code); }

static void close_fds(int *fds, int n) { for (int i = 0; i < n; i++) if (fds[i] >= 0) { close(fds[i]); fds[i] = -1; } }

/* A file the app passes on: a readable regular file of at most 64 MB, read from the start. */
static int file_ok(int fd) {
    struct stat st;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || (fl & O_ACCMODE) == O_WRONLY || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > FILE_MAX) return 0;
    lseek(fd, 0, SEEK_SET);
    return 1;
}

static int ensure_owner(int conn, int fd, int role) {
    if (conn < 0 || conn >= MAX_CONN) return -1;
    if (mc[conn].owner > 0) return mc[conn].owner;
    int o = mail_owner_open(role == ROLE_MAIL ? MAIL_EV_ALL : MAIL_EV_PIM);
    if (o > 0) { mc[conn].owner = o; mc[conn].fd = fd; }
    return o;
}

/* "verb\0rest": rest (may hold NULs) and its length */
static const char *rest_of(const char *payload, uint32_t len, uint32_t *rl) {
    const char *nul = memchr(payload, 0, len);
    if (!nul) { *rl = 0; return payload + len; }
    *rl = len - (uint32_t)(nul + 1 - payload);
    return nul + 1;
}

/* JSON with a short "op" (as js_mail.c f_request); op copied out. 0 if valid. */
static int check_request(const char *j, size_t len, char *op, size_t on) {
    JDoc d;
    if (!len || len > REQ_MAX || json_parse(j, len, &d, NULL, 0)) return -1;
    const char *o = d.root && d.root->type == J_OBJ ? json_str(d.root, "op") : NULL;
    int ok = o && o[0] && strlen(o) <= 32;
    if (ok) snprintf(op, on, "%s", o);
    json_free(&d);
    return ok ? 0 : -1;
}

static char *read_all(int fd, size_t max, size_t *out) {
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (size_t)st.st_size > max) return NULL;
    char *b = malloc((size_t)st.st_size + 1);
    if (!b) return NULL;
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t r = pread(fd, b + got, (size_t)st.st_size - got, (off_t)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (got != (size_t)st.st_size) { free(b); return NULL; }
    b[got] = 0;
    *out = got;
    return b;
}

/* A cached attachment: "att/<acct>/<uid>-<idx>/<name>" inside the mail cache, no "..", no links
 * out of it. Writes the real path. 0 if valid. */
static int attachment_path(const char *rel, size_t rl, char *real, size_t n) {
    if (rl < 5 || rl > 1024 || memchr(rel, 0, rl) || strncmp(rel, "att/", 4) || strstr(rel, "..") || strstr(rel, "//")) return -1;
    char full[PATH_MAX * 2], base[PATH_MAX], cache[PATH_MAX];
    if (!realpath(mail_cache_path(), cache)) return -1;
    if ((size_t)snprintf(base, sizeof base, "%s/att/", cache) >= sizeof base) return -1;
    if ((size_t)snprintf(full, sizeof full, "%s/%.*s", cache, (int)rl, rel) >= sizeof full) return -1;
    char tmp[PATH_MAX];
    if (!realpath(full, tmp) || strncmp(tmp, base, strlen(base))) return -1;
    if ((size_t)snprintf(real, n, "%s", tmp) >= n) return -1;
    return 0;
}

void brokermail_handle(int conn, int fd, uint32_t reqid, const char *app, char *payload, uint32_t len, int *fds, int nfds) {
    int role = role_of(conn, app);
    if (role == ROLE_NONE) { close_fds(fds, nfds); reply(fd, reqid, BST_DENIED, NULL); return; }
    uint32_t rl = 0;
    const char *verb = payload, *rest = rest_of(payload, len, &rl);

    if (!strcmp(verb, "req") || !strcmp(verb, "reqm")) {
        int first = 0;
        char *mem = NULL;
        const char *j = rest;
        size_t jl = rl;
        if (!strcmp(verb, "reqm")) {             /* a large request: its JSON in a memfd (the first fd) */
            if (nfds < 1 || !(mem = read_all(fds[0], REQ_MAX, &jl))) { close_fds(fds, nfds); reply_code(fd, reqid, "mail.requestTooLarge"); return; }
            close(fds[0]);
            fds[0] = -1;
            j = mem;
            first = 1;
        }
        char op[40];
        if (check_request(j, jl, op, sizeof op)) { free(mem); close_fds(fds, nfds); reply_code(fd, reqid, "sys.badArgument"); return; }
        if (!op_allowed(role, op)) {
            fprintf(stderr, "broker[%s]: mail request '%s' refused (not this app's)\n", app, op);
            free(mem); close_fds(fds, nfds); reply(fd, reqid, BST_DENIED, NULL); return;
        }
        int files[15], nf = 0;
        for (int i = first; i < nfds; i++) {
            if (fds[i] < 0) continue;
            if (nf >= 15 || !op_takes_files(op) || !file_ok(fds[i])) {
                free(mem); close_fds(fds, nfds);
                for (int k = 0; k < nf; k++) close(files[k]);
                reply_code(fd, reqid, "fs.outsideHome");
                return;
            }
            files[nf++] = fds[i];
            fds[i] = -1;
        }
        int owner = ensure_owner(conn, fd, role);
        int id = owner > 0 ? mail_request_as(owner, j, jl, files, nf) : -1;   /* takes the files */
        if (owner <= 0) for (int k = 0; k < nf; k++) close(files[k]);
        free(mem);
        char s[16];
        snprintf(s, sizeof s, "%d", id);
        reply(fd, reqid, id > 0 ? BST_OK : BST_ERROR, s);
        return;
    }
    /* the other verbs take no files */
    close_fds(fds, nfds);
    if (!strcmp(verb, "available")) { reply(fd, reqid, BST_OK, mail_available() == 0 ? "1" : "0"); return; }
    if (!strcmp(verb, "start")) { ensure_owner(conn, fd, role); reply(fd, reqid, BST_OK, mail_start() == 0 ? "1" : "0"); return; }
    if (!strcmp(verb, "img")) {
        int w = 0, h = 0, f = mc[conn].owner > 0 ? mail_image_fd(mc[conn].owner, atoi(rest), &w, &h) : -1;
        if (f < 0) { reply(fd, reqid, BST_ERROR, NULL); return; }
        char s[32];
        snprintf(s, sizeof s, "%d %d", w, h);
        brokersrv_reply(fd, reqid, BST_OK, s, (uint32_t)strlen(s), f);
        close(f);
        return;
    }
    if (!strcmp(verb, "release")) { if (mc[conn].owner > 0) mail_image_release_as(mc[conn].owner, atoi(rest)); reply(fd, reqid, BST_OK, NULL); return; }
    /* ---- Mail only from here ---- */
    if (role != ROLE_MAIL) { reply(fd, reqid, BST_DENIED, NULL); return; }
    if (!strcmp(verb, "config")) {
        size_t n = 0;
        int f = open(mail_config_path(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        char *b = f >= 0 ? read_all(f, MAIL_CONFIG_MAX, &n) : NULL;
        if (f >= 0) close(f);
        brokersrv_reply(fd, reqid, BST_OK, b ? b : "", b ? (uint32_t)n : 0, -1);
        free(b);
        return;
    }
    if (!strcmp(verb, "save")) {
        int r = rl ? mail_save_config(rest, rl) : 1;
        if (!r) { reply(fd, reqid, BST_OK, NULL); return; }
        char line[PATH_MAX + 64];
        if (r == 1) lerr_set(line, sizeof line, "mail.badAccounts", NULL);
        else lerr_set(line, sizeof line, "sys.writeFailed", "path", mail_config_path(), NULL);
        reply_code(fd, reqid, line);
        return;
    }
    if (!strcmp(verb, "images")) {               /* "url\0url\0…": the remote images of one message */
        const char *urls[200];
        int n = 0;
        const char *end = rest + rl;
        for (const char *p = rest; p < end && n < 200;) {
            size_t l = strnlen(p, (size_t)(end - p));
            if (p + l == end) break;             /* each one NUL-terminated */
            if (l && l < 2048) urls[n++] = p;
            p += l + 1;
        }
        mail_allow_hosts(urls, n, 600);
        reply(fd, reqid, BST_OK, NULL);
        return;
    }
    if (!strcmp(verb, "att") || !strcmp(verb, "open")) {
        /* "att": rest = relative path → read-only fd. "open": rest = "path\0name" → the session opens it */
        size_t pl = strnlen(rest, rl);
        char real[PATH_MAX];
        if (attachment_path(rest, pl, real, sizeof real)) { reply(fd, reqid, BST_DENIED, NULL); return; }
        if (!strcmp(verb, "att")) {
            int f = open(real, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            struct stat st;
            if (f < 0 || fstat(f, &st) || !S_ISREG(st.st_mode)) { if (f >= 0) close(f); reply(fd, reqid, BST_ERROR, NULL); return; }
            brokersrv_reply(fd, reqid, BST_OK, NULL, 0, f);
            close(f);
            return;
        }
        const char *name = pl < rl ? rest + pl + 1 : "";
        char qp[PATH_MAX * 2 + 2], qn[600], ev[PATH_MAX * 2 + 700];
        char nm[256];
        snprintf(nm, sizeof nm, "%.*s", (int)strnlen(name, rl - (pl < rl ? pl + 1 : rl)), name);
        json_quote(qp, sizeof qp, real);
        json_quote(qn, sizeof qn, nm);
        snprintf(ev, sizeof ev, "{\"app\":\"mail\",\"path\":%s,\"name\":%s}", qp, qn);
        brokersrv_shell_event("mailopen", ev);
        reply(fd, reqid, BST_OK, NULL);
        return;
    }
    reply(fd, reqid, BST_ERROR, NULL);
}

/* "app\0new\0json": Mail, Calendar, Contacts and Reminders may open one another with args. */
void brokermail_sys_open(int conn, int fd, uint32_t reqid, const char *app, char *payload, uint32_t len) {
    if (role_of(conn, app) == ROLE_NONE) { reply(fd, reqid, BST_DENIED, NULL); return; }
    const char *target = payload;
    uint32_t rl = 0;
    const char *nw = rest_of(payload, len, &rl);
    uint32_t jl = 0;
    const char *json = rest_of(nw, rl, &jl);
    int ok = (!strcmp(target, "mail") || !strcmp(target, "calendar") || !strcmp(target, "contacts") || !strcmp(target, "reminders"));
    JDoc d;
    /* args only: a small JSON object, never a path (these apps never open files by path) */
    ok = ok && jl > 0 && jl <= 1500 && !memchr(json, 0, jl) && !json_parse(json, jl, &d, NULL, 0);
    if (ok) { ok = d.root && d.root->type == J_OBJ && !json_get(d.root, "path") && !json_get(d.root, "paths"); json_free(&d); }
    if (!ok) { reply(fd, reqid, BST_DENIED, NULL); return; }
    char ev[1700];
    snprintf(ev, sizeof ev, "{\"app\":\"%s\",\"new\":%s,\"args\":%.*s}", target, nw[0] == '1' ? "true" : "false", (int)jl, json);
    brokersrv_shell_event("launch", ev);
    reply(fd, reqid, BST_OK, NULL);
}

void brokermail_conn_closed(int conn) {
    if (conn < 0 || conn >= MAX_CONN) return;
    if (mc[conn].owner > 0) mail_owner_close(mc[conn].owner);
    free(mc[conn].out);
    memset(&mc[conn], 0, sizeof mc[conn]);
}

int brokermail_pollfd(void) { return mail_owner_pollfd(); }
int brokermail_wants_write(int conn) { return conn >= 0 && conn < MAX_CONN && mc[conn].out != NULL; }

/* Sends what it can of every app's queue; never blocks. */
void brokermail_tick(void) {
    mail_owner_clear_wake();
    for (int i = 0; i < MAX_CONN; i++) {
        MailConn *m = &mc[i];
        if (m->owner <= 0 || !brokersrv_conn_app(i)) continue;
        for (;;) {
            if (!m->out) {
                m->out = mail_poll_owner(m->owner);
                if (!m->out) break;
                m->len = strlen(m->out);
                m->off = 0;
            }
            size_t n = m->len - m->off;
            int last = n <= CHUNK;
            if (!last) n = CHUNK;
            char *buf = malloc(n + 1);
            if (!buf) break;
            buf[0] = last ? 'F' : 'C';
            memcpy(buf + 1, m->out + m->off, n);
            int r = brokersrv_conn_event(i, "mail", buf, n + 1);
            free(buf);
            if (r) break;                        /* the socket is full: the rest goes on a later tick */
            m->off += n;
            if (last) { free(m->out); m->out = NULL; }
        }
    }
}
