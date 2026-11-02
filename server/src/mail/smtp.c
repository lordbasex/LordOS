/* smtp.c — composing (MIME) and sending (SMTP submission) for axia-maild (see maild.h).
 *
 * Messages are built here, never by JS: headers are encoded (RFC 2047/2231), bodies go out as
 * quoted-printable, attachments as base64 read from the fds axia-server opened for the files the
 * user picked. Submission is on 465 (implicit TLS) or 587 (STARTTLS, required), with AUTH PLAIN
 * or LOGIN. One sender thread serves every account, plus the "send later" outbox. */
#include "maild.h"
#include "mime.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ATT_MAX   ((size_t)25 << 20)
#define TOTAL_MAX ((size_t)35 << 20)

static void rand_hex(char *out, size_t bytes) {
    unsigned char r[32];
    if (bytes > sizeof r) bytes = sizeof r;
    if (getrandom(r, bytes, 0) != (ssize_t)bytes) for (size_t i = 0; i < bytes; i++) r[i] = (unsigned char)(rand() ^ (int)time(NULL));
    for (size_t i = 0; i < bytes; i++) sprintf(out + i * 2, "%02x", r[i]);
}

static void hdr(JBuf *b, const char *name, const char *value) {
    if (!value || !*value) return;
    char *e = mime_encode_header(value);
    if (!e) return;
    jb_raw(b, name);
    jb_raw(b, ": ");
    jb_raw(b, e);
    jb_raw(b, "\r\n");
    free(e);
}

static int valid_email(const char *e) {
    if (!e || !*e || strlen(e) > 254 || !strchr(e, '@')) return 0;
    for (const char *c = e; *c; c++) if ((unsigned char)*c <= 0x20 || *c == '<' || *c == '>' || *c == ',' || *c == ';' || *c == '"' || *c == '\\' || *c == 0x7f) return 0;
    return 1;
}

static void addr_hdr(JBuf *b, const char *name, const JVal *list) {
    if (!list || list->type != J_ARR || !list->kid) return;
    jb_raw(b, name);
    jb_raw(b, ": ");
    int first = 1;
    for (const JVal *x = list->kid; x; x = x->next) {
        const char *e = json_str(x, "email");
        if (!valid_email(e)) continue;
        char *f = mime_format_addr(json_str(x, "name"), e);
        if (!f) continue;
        if (!first) jb_raw(b, ",\r\n ");
        jb_raw(b, f);
        free(f);
        first = 0;
    }
    jb_raw(b, "\r\n");
}

static void text_part(JBuf *b, const char *type, const char *text, const char *extra_params) {
    jb_printf(b, "Content-Type: %s; charset=utf-8%s\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n", type, extra_params ? extra_params : "");
    size_t ql;
    char *q = qp_encode(text ? text : "", text ? strlen(text) : 0, &ql);
    if (q) { jb_raw(b, q); free(q); }
    jb_raw(b, "\r\n");
}

static char *read_fd(int fd, size_t *n, size_t max) {
    struct stat st;
    *n = 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size > max) return NULL;
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) return NULL;
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t r = pread(fd, buf + got, (size_t)st.st_size - got, (off_t)got);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
        got += (size_t)r;
    }
    *n = got;
    return buf;
}

static void pct(JBuf *b, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || strchr("!#$&+-.^_`|~", c)) { char t[2] = { (char)c, 0 }; jb_raw(b, t); }
        else jb_printf(b, "%%%02X", c);
    }
}

char *compose_message(Account *a, const JVal *m, int *fds, int nfds, size_t *olen, char *msgid, size_t mcap, char *err, size_t ecap) {
    JBuf b;
    jb_init(&b);
    char date[64];
    mime_format_date(time(NULL), date, sizeof date);
    jb_printf(&b, "Date: %s\r\n", date);
    char *from = mime_format_addr(a->full_name, a->email);
    if (!from || !valid_email(a->email)) { free(from); jb_free(&b); snprintf(err, ecap, "bad sender address"); return NULL; }
    jb_printf(&b, "From: %s\r\n", from);
    free(from);
    addr_hdr(&b, "To", json_get(m, "to"));
    addr_hdr(&b, "Cc", json_get(m, "cc"));
    hdr(&b, "Subject", json_str(m, "subject"));
    const char *dom = strchr(a->email, '@') + 1;
    char rnd[40];
    rand_hex(rnd, 16);
    snprintf(msgid, mcap, "<%s@%s>", rnd, dom);
    jb_printf(&b, "Message-ID: %s\r\n", msgid);
    const char *irt = json_str(m, "inReplyTo"), *refs = json_str(m, "references");
    /* threading headers: ids only (no line breaks smuggled in) */
    if (irt && *irt && !strpbrk(irt, "\r\n")) jb_printf(&b, "In-Reply-To: %.*s\r\n", 998, irt);
    if (refs && *refs && !strpbrk(refs, "\r\n")) jb_printf(&b, "References: %.*s\r\n", 3000, refs);
    jb_raw(&b, "MIME-Version: 1.0\r\nX-Mailer: Axia Mail\r\n");
    const char *text = json_str(m, "text"), *html = json_str(m, "html");
    const char *ics = json_str(m, "calendar"), *icsm = json_str(m, "calendarMethod");
    const JVal *atts = json_get(m, "attachments");
    int natt = nfds;
    char bm[48], ba[48];
    rand_hex(rnd, 12);
    SNPF(bm, sizeof bm, "=_lordos_m_%s", rnd);
    rand_hex(rnd, 12);
    SNPF(ba, sizeof ba, "=_lordos_a_%s", rnd);
    int mixed = natt > 0;
    int alt = (html && *html) || (ics && *ics);
    if (mixed) jb_printf(&b, "Content-Type: multipart/mixed; boundary=\"%s\"\r\n\r\nThis is a multi-part message in MIME format.\r\n--%s\r\n", bm, bm);
    if (alt) {
        jb_printf(&b, "Content-Type: multipart/alternative; boundary=\"%s\"\r\n\r\n", ba);
        jb_printf(&b, "--%s\r\n", ba);
        text_part(&b, "text/plain", text, NULL);
        if (html && *html) { jb_printf(&b, "--%s\r\n", ba); text_part(&b, "text/html", html, NULL); }
        if (ics && *ics) {
            char mp[48];
            snprintf(mp, sizeof mp, "; method=%s", icsm && !strcmp(icsm, "REPLY") ? "REPLY" : icsm && !strcmp(icsm, "CANCEL") ? "CANCEL" : "REQUEST");
            jb_printf(&b, "--%s\r\n", ba);
            text_part(&b, "text/calendar", ics, mp);
        }
        jb_printf(&b, "--%s--\r\n", ba);
    } else text_part(&b, "text/plain", text, NULL);
    size_t total = b.n;
    int i = 0;
    for (const JVal *x = atts && atts->type == J_ARR ? atts->kid : NULL; i < natt; i++, x = x ? x->next : NULL) {
        size_t n;
        char *data = read_fd(fds[i], &n, ATT_MAX);
        if (!data) { jb_free(&b); snprintf(err, ecap, "attachment %d is too big or unreadable", i + 1); return NULL; }
        total += n * 4 / 3;
        if (total > TOTAL_MAX) { free(data); jb_free(&b); snprintf(err, ecap, "the message is too big"); return NULL; }
        char name[256];
        fs_safe_name(x && json_str(x, "name") ? json_str(x, "name") : "attachment", name, sizeof name);
        const char *type = x && json_str(x, "type") ? json_str(x, "type") : "application/octet-stream";
        int type_ok = strlen(type) < 100 && strchr(type, '/') && !strpbrk(type, "\r\n;\"");
        jb_printf(&b, "--%s\r\nContent-Type: %s; name=\"", bm, type_ok ? type : "application/octet-stream");
        char *en = mime_encode_header(name);
        if (en) { jb_raw(&b, en); free(en); }
        jb_raw(&b, "\"\r\nContent-Disposition: attachment; filename*=UTF-8''");
        pct(&b, name);
        jb_raw(&b, "\r\nContent-Transfer-Encoding: base64\r\n\r\n");
        size_t el;
        char *e64 = b64_encode((const unsigned char *)data, n, 1, &el);
        free(data);
        if (!e64) { jb_free(&b); snprintf(err, ecap, "memory"); return NULL; }
        jb_raw(&b, e64);
        free(e64);
    }
    if (mixed) jb_printf(&b, "--%s--\r\n", bm);
    if (b.oom) { jb_free(&b); snprintf(err, ecap, "memory"); return NULL; }
    *olen = b.n;
    return b.p;
}

/* ---- SMTP ---- */

static int smtp_reply(Conn *c, char *text, size_t tcap, char *caps, size_t ccap) {
    int code = -1;
    if (caps && ccap) caps[0] = 0;
    for (int n = 0; n < 100; n++) {
        size_t l;
        char *line = conn_line(c, &l, 4096);
        if (!line) return -1;
        if (l < 4) { free(line); return -1; }
        code = atoi(line);
        if (caps && ccap && strlen(caps) + l + 1 < ccap) strcat(caps, line + 4);
        int more = line[3] == '-';
        if (!more && text) { snprintf(text, tcap, "%s", line + 4); for (char *p = text; *p; p++) if (*p == '\r' || *p == '\n') *p = 0; }
        free(line);
        if (!more) break;
    }
    return code;
}

static int smtp_cmd(Conn *c, char *text, size_t tcap, const char *fmt, const char *arg) {
    if (conn_printf(c, fmt, arg ? arg : "") || conn_write(c, "\r\n", 2)) return -1;
    return smtp_reply(c, text, tcap, NULL, 0);
}

int smtp_send(Account *a, const char *from, const char **rcpt, int nrcpt, const char *msg, size_t len, char *err, size_t ecap) {
    pthread_mutex_lock(&G);
    char *pw = a->password ? strdup(a->password) : NULL;
    Server s = a->smtp;
    char pin[96];
    snprintf(pin, sizeof pin, "%s", a->pin_smtp);
    pthread_mutex_unlock(&G);
    if (!pw) { snprintf(err, ecap, "no-password"); return -2; }
    if (s.sec == SEC_NONE || !s.host[0]) { free(pw); snprintf(err, ecap, s.host[0] ? "insecure" : "no SMTP server"); return -1; }
    TlsInfo ti;
    char why[64], text[512], caps[4096];
    Conn *c = open_conn(a->id, 1, s.host, s.port, s.sec == SEC_SSL, pin, &ti, why, sizeof why);
    if (!c) { free(pw); snprintf(err, ecap, "%s", why); return -1; }
    int r = -1;
    if (smtp_reply(c, text, sizeof text, NULL, 0) != 220) { snprintf(err, ecap, "greeting: %s", text); goto out; }
    if (smtp_cmd(c, text, sizeof text, "EHLO %s", "lordos") != 250) { snprintf(err, ecap, "EHLO: %s", text); goto out; }
    /* EHLO again to read the capabilities (the first reply already went by) */
    conn_printf(c, "EHLO lordos\r\n");
    if (smtp_reply(c, text, sizeof text, caps, sizeof caps) != 250) { snprintf(err, ecap, "EHLO: %s", text); goto out; }
    if (s.sec == SEC_STARTTLS) {
        if (!strcasestr(caps, "STARTTLS")) { snprintf(err, ecap, "no-starttls"); goto out; }
        if (smtp_cmd(c, text, sizeof text, "STARTTLS%s", "") != 220) { snprintf(err, ecap, "STARTTLS: %s", text); goto out; }
        if (conn_tls(c, pin, &ti)) {
            snprintf(err, ecap, "%s", ti.code ? "cert" : "tls");
            if (ti.code) {
                JBuf b;
                jb_init(&b);
                jb_raw(&b, "{\"ev\":\"cert\",\"acct\":"); jb_str(&b, a->id);
                jb_raw(&b, ",\"host\":"); jb_str(&b, s.host);
                jb_printf(&b, ",\"port\":%d,\"smtp\":true,\"reason\":", s.port); jb_str(&b, ti.reason);
                jb_raw(&b, ",\"subject\":"); jb_str(&b, ti.subject);
                jb_raw(&b, ",\"issuer\":"); jb_str(&b, ti.issuer);
                jb_raw(&b, ",\"sha256\":"); jb_str(&b, ti.sha256);
                jb_raw(&b, "}");
                send_event(&b);
            }
            goto out;
        }
        conn_printf(c, "EHLO lordos\r\n");
        if (smtp_reply(c, text, sizeof text, caps, sizeof caps) != 250) { snprintf(err, ecap, "EHLO: %s", text); goto out; }
    }
    /* AUTH PLAIN (or LOGIN) */
    {
        size_t ul = strlen(a->user), pl = strlen(pw);
        int code;
        if (strcasestr(caps, "PLAIN") || !strcasestr(caps, "LOGIN")) {
            unsigned char *raw = malloc(ul + pl + 3);
            if (!raw) goto out;
            raw[0] = 0; memcpy(raw + 1, a->user, ul); raw[1 + ul] = 0; memcpy(raw + 2 + ul, pw, pl);
            size_t el;
            char *e = b64_encode(raw, ul + pl + 2, 0, &el);
            explicit_bzero(raw, ul + pl + 2);
            free(raw);
            if (!e) goto out;
            code = smtp_cmd(c, text, sizeof text, "AUTH PLAIN %s", e);
            explicit_bzero(e, el);
            free(e);
        } else {
            size_t el;
            code = smtp_cmd(c, text, sizeof text, "AUTH LOGIN%s", "");
            if (code == 334) { char *e = b64_encode((const unsigned char *)a->user, ul, 0, &el); code = e ? smtp_cmd(c, text, sizeof text, "%s", e) : -1; free(e); }
            if (code == 334) { char *e = b64_encode((const unsigned char *)pw, pl, 0, &el); code = e ? smtp_cmd(c, text, sizeof text, "%s", e) : -1; if (e) { explicit_bzero(e, el); free(e); } }
        }
        if (code != 235) { snprintf(err, ecap, "auth: %s", text); r = -2; goto out; }
    }
    if (!msg) { r = 0; goto out; }          /* a login test */
    {
        char line[600];
        snprintf(line, sizeof line, "MAIL FROM:<%s>%s", from, strcasestr(caps, "SIZE") ? "" : "");
        if (smtp_cmd(c, text, sizeof text, "%s", line) != 250) { snprintf(err, ecap, "MAIL FROM: %s", text); goto out; }
        int okr = 0;
        for (int i = 0; i < nrcpt; i++) {
            snprintf(line, sizeof line, "RCPT TO:<%s>", rcpt[i]);
            int code = smtp_cmd(c, text, sizeof text, "%s", line);
            if (code == 250 || code == 251) okr++;
            else { snprintf(err, ecap, "%s: %s", rcpt[i], text); goto out; }
        }
        if (!okr) { snprintf(err, ecap, "no recipients"); goto out; }
        if (smtp_cmd(c, text, sizeof text, "DATA%s", "") != 354) { snprintf(err, ecap, "DATA: %s", text); goto out; }
        /* dot-stuffing, CRLF line ends */
        JBuf d;
        jb_init(&d);
        int bol = 1;
        for (size_t i = 0; i < len; i++) {
            char ch = msg[i];
            if (bol && ch == '.') jb_raw(&d, ".");
            if (ch == '\n' && (i == 0 || msg[i - 1] != '\r')) jb_raw(&d, "\r");
            char t[2] = { ch, 0 };
            if (ch) jb_raw(&d, t);
            bol = ch == '\n';
        }
        if (!bol) jb_raw(&d, "\r\n");
        jb_raw(&d, ".\r\n");
        int w = d.oom ? -1 : conn_write(c, d.p, d.n);
        jb_free(&d);
        if (w || smtp_reply(c, text, sizeof text, NULL, 0) != 250) { snprintf(err, ecap, "send: %s", text); goto out; }
        r = 0;
    }
out:
    if (pw) { explicit_bzero(pw, strlen(pw)); free(pw); }
    conn_printf(c, "QUIT\r\n");
    conn_close(c);
    return r;
}

/* ---- the sender ---- */

static pthread_mutex_t smu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scv = PTHREAD_COND_INITIALIZER;
static Job *squeue;

void sender_queue(Job *j) {
    pthread_mutex_lock(&smu);
    Job **p = &squeue;
    while (*p) p = &(*p)->next;
    j->next = NULL;
    *p = j;
    pthread_cond_signal(&scv);
    pthread_mutex_unlock(&smu);
}

static void sent_event(const char *acct, const char *msgid, const char *subject, int later) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"ev\":\"sent\",\"acct\":"); jb_str(&b, acct);
    jb_raw(&b, ",\"msgid\":"); jb_str(&b, msgid);
    jb_raw(&b, ",\"subject\":"); jb_str(&b, subject ? subject : "");
    jb_printf(&b, ",\"later\":%s}", later ? "true" : "false");
    send_event(&b);
}

/* Envelope + raw message → SMTP, then the copy in Sent and the \Answered flag. */
static int deliver(Account *a, const JVal *m, const char *raw, size_t rl, char *err, size_t ecap) {
    const char *rcpt[300];
    int nr = 0;
    const char *lists[] = { "to", "cc", "bcc" };
    for (int l = 0; l < 3; l++) {
        const JVal *arr = json_get(m, lists[l]);
        for (const JVal *x = arr && arr->type == J_ARR ? arr->kid : NULL; x && nr < 300; x = x->next) {
            const char *e = json_str(x, "email");
            if (valid_email(e)) rcpt[nr++] = e;
        }
    }
    if (!nr) { snprintf(err, ecap, "no recipients"); return -1; }
    int r = smtp_send(a, a->email, rcpt, nr, raw, rl, err, ecap);
    if (r) return r;
    if (!a->sent_saved && !json_bool(m, "noSent", 0)) imap_append_sent(a, raw, rl);
    /* the message we answered or forwarded */
    const JVal *orig = json_get(m, "original");
    if (orig && json_str(orig, "folder") && json_num(orig, "uid", 0) >= 1) {
        JBuf fj;
        jb_init(&fj);
        jb_printf(&fj, "{\"add\":[\"%s\"]}", json_bool(orig, "forward", 0) ? "forwarded" : "answered");
        Job *j = job_new(J_FLAGS, 0, 6);
        if (j && !fj.oom) {
            snprintf(j->folder, sizeof j->folder, "%s", json_str(orig, "folder"));
            j->uids = malloc(sizeof *j->uids);
            if (j->uids) { j->uids[0] = (uint32_t)json_num(orig, "uid", 0); j->nuids = 1; }
            j->arg = fj.p; j->arglen = fj.n;
            Account *oa = acct_get(json_str(orig, "acct"));
            job_push(oa ? oa : a, j);
        } else { jb_free(&fj); job_free(j); }
    }
    /* the draft it came from */
    double du = json_num(m, "draftUid", 0);
    if (du >= 1 && json_str(m, "draftFolder")) {
        Job *j = job_new(J_DELETE, 0, 6);
        if (j) {
            snprintf(j->folder, sizeof j->folder, "%s", json_str(m, "draftFolder"));
            j->uids = malloc(sizeof *j->uids);
            if (j->uids) { j->uids[0] = (uint32_t)du; j->nuids = 1; }
            j->arg = strdup("{\"permanent\":true}");
            j->arglen = j->arg ? strlen(j->arg) : 0;
            job_push(a, j);
        }
    }
    return 0;
}

static void run_outbox(void) {
    int dfd = openat(cache_fd, "outbox", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return;
    close(dfd);
    /* outbox/<n>.json holds {"acct","sendAt","env":{…},"raw":"…"} */
    for (int k = 0; k < 1000; k++) {
        char rel[64];
        snprintf(rel, sizeof rel, "outbox/%d.json", k);
        size_t n;
        char *t = fs_read(rel, &n, 64 << 20);
        if (!t) continue;
        JDoc d;
        if (json_parse(t, n, &d, NULL, 0)) { free(t); fs_unlink(rel); continue; }
        double at = json_num(d.root, "sendAt", 0) / 1000;
        if (at > (double)time(NULL)) { json_free(&d); free(t); continue; }
        Account *a = acct_get(json_str(d.root, "acct"));
        const char *raw = json_str(d.root, "raw");
        const JVal *env = json_get(d.root, "env");
        char err[300] = "";
        if (a && raw && env) {
            int r = deliver(a, env, raw, strlen(raw), err, sizeof err);
            if (!r) { fs_unlink(rel); sent_event(a->id, json_str(d.root, "msgid"), json_str(env, "subject"), 1); }
            else if (r == -2) { /* no password yet: keep it */ }
            else {
                JBuf b;
                jb_init(&b);
                jb_raw(&b, "{\"ev\":\"send-failed\",\"acct\":"); jb_str(&b, a->id);
                jb_raw(&b, ",\"subject\":"); jb_str(&b, json_str(env, "subject"));
                jb_raw(&b, ",\"error\":"); jb_str(&b, err);
                jb_raw(&b, "}");
                send_event(&b);
                fs_unlink(rel);
            }
        } else fs_unlink(rel);
        json_free(&d);
        free(t);
    }
}

static int outbox_store(Account *a, const JVal *m, const char *raw, size_t rl, const char *msgid, double at) {
    fs_mkdirs("outbox");
    for (int k = 0; k < 1000; k++) {
        char rel[64];
        snprintf(rel, sizeof rel, "outbox/%d.json", k);
        if (fs_exists(rel)) continue;
        JBuf b;
        jb_init(&b);
        jb_raw(&b, "{\"acct\":"); jb_str(&b, a->id);
        jb_printf(&b, ",\"sendAt\":%.0f,\"msgid\":", at);
        jb_str(&b, msgid);
        jb_raw(&b, ",\"env\":{\"subject\":"); jb_str(&b, json_str(m, "subject"));
        const char *lists[] = { "to", "cc", "bcc" };
        for (int l = 0; l < 3; l++) {
            jb_printf(&b, ",\"%s\":[", lists[l]);
            const JVal *arr = json_get(m, lists[l]);
            int f = 1;
            for (const JVal *x = arr && arr->type == J_ARR ? arr->kid : NULL; x; x = x->next) {
                const char *e = json_str(x, "email");
                if (!valid_email(e)) continue;
                if (!f) jb_raw(&b, ",");
                jb_raw(&b, "{\"email\":"); jb_str(&b, e); jb_raw(&b, "}");
                f = 0;
            }
            jb_raw(&b, "]");
        }
        jb_raw(&b, "},\"raw\":");
        jb_strn(&b, raw, rl);
        jb_raw(&b, "}");
        int ok = !b.oom && fs_write(rel, b.p, b.n) == 0;
        jb_free(&b);
        return ok ? 0 : -1;
    }
    return -1;
}

static void *sender(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&smu);
        while (!squeue) {
            struct timespec dl;
            clock_gettime(CLOCK_REALTIME, &dl);
            dl.tv_sec += 30;
            if (pthread_cond_timedwait(&scv, &smu, &dl) == ETIMEDOUT) break;
        }
        Job *j = squeue;
        if (j) squeue = j->next;
        pthread_mutex_unlock(&smu);
        if (!j) { run_outbox(); continue; }
        JDoc d;
        Account *a = acct_get(j->folder);
        if (!a || !j->arg || json_parse(j->arg, j->arglen, &d, NULL, 0)) { if (j->req) reply_err(j->req, "bad-args", ""); job_free(j); continue; }
        const JVal *m = d.root;
        char err[300] = "", msgid[300] = "";
        if (json_bool(m, "test", 0)) {
            int r = smtp_send(a, NULL, NULL, 0, NULL, 0, err, sizeof err);
            if (r) reply_err(j->req, r == -2 ? "auth" : "smtp", err);
            else { JBuf b; jb_init(&b); jb_raw(&b, "{\"ok\":true}"); reply_done(j->req, &b); }
            json_free(&d);
            job_free(j);
            continue;
        }
        size_t rl;
        char *raw = compose_message(a, m, j->fds, j->nfds, &rl, msgid, sizeof msgid, err, sizeof err);
        if (!raw) { if (j->req) reply_err(j->req, "compose", err); json_free(&d); job_free(j); continue; }
        double at = json_num(m, "sendAt", 0);
        if (at / 1000 > (double)time(NULL) + 20) {
            int r = outbox_store(a, m, raw, rl, msgid, at);
            if (j->req) { if (r) reply_err(j->req, "outbox", ""); else { JBuf b; jb_init(&b); jb_raw(&b, "{\"ok\":true,\"scheduled\":true,\"msgid\":"); jb_str(&b, msgid); jb_raw(&b, "}"); reply_done(j->req, &b); } }
        } else {
            int r = deliver(a, m, raw, rl, err, sizeof err);
            if (j->req) {
                if (r) reply_err(j->req, r == -2 ? "auth" : "smtp", err);
                else { JBuf b; jb_init(&b); jb_raw(&b, "{\"ok\":true,\"msgid\":"); jb_str(&b, msgid); jb_raw(&b, "}"); reply_done(j->req, &b); }
            }
            if (!r) sent_event(a->id, msgid, json_str(m, "subject"), 0);
        }
        free(raw);
        json_free(&d);
        job_free(j);
    }
    return NULL;
}

void sender_start(void) {
    pthread_t t;
    if (!pthread_create(&t, NULL, sender, NULL)) pthread_detach(t);
}
