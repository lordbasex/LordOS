/* imap.c — IMAP4rev1 client and the per-account worker of axia-maild (see maild.h).
 *
 * Per account: connect (implicit TLS on 993, or STARTTLS on 143), authenticate (AUTHENTICATE
 * PLAIN or LOGIN with an app password; XOAUTH2 is prepared but disabled until the user registers
 * a client ID, see docs/MAIL.md), list folders (SPECIAL-USE), sync each folder into the cache
 * (UIDVALIDITY, UID SEARCH, header + preview fetch in chunks, newest first; CONDSTORE for cheap
 * flag updates), then wait in IDLE on the inbox for new mail. Jobs from the UI (open a message,
 * flags, move, delete, drafts) interrupt the IDLE through an eventfd. */
#include "maild.h"
#include "imapp.h"
#include "imapio.h"
#include "mime.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define LINE_MAX_BYTES ((size_t)16 << 20)
#define CHUNK 250

typedef struct {
    Account *a;
    Conn *c;
    int tag;
    char caps[4096];
    int idle, condstore, move, uidplus, literalplus, special_use, sasl_ir, auth_plain;
    char selected[512];
    uint32_t exists;
    char text[512];                 /* text of the last tagged reply */
    char code[128];                 /* [CODE] of the last tagged reply */
} Imap;

static int has_cap(const Imap *im, const char *cap) {
    size_t n = strlen(cap);
    for (const char *p = im->caps; (p = strcasestr(p, cap)); p += n)
        if ((p == im->caps || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0 || p[n] == '\r' || p[n] == '\n')) return 1;
    return 0;
}

static void parse_caps(Imap *im, const char *line) {
    const char *c = strcasestr(line, "CAPABILITY");
    if (!c) return;
    c += 10;
    snprintf(im->caps, sizeof im->caps, "%s", c);
    for (char *p = im->caps; *p; p++) if (*p == ']' || *p == '\r' || *p == '\n') { *p = 0; break; }
    im->idle = has_cap(im, "IDLE");
    im->condstore = has_cap(im, "CONDSTORE") || has_cap(im, "QRESYNC");
    im->move = has_cap(im, "MOVE");
    im->uidplus = has_cap(im, "UIDPLUS");
    im->literalplus = has_cap(im, "LITERAL+");
    im->special_use = has_cap(im, "SPECIAL-USE");
    im->sasl_ir = has_cap(im, "SASL-IR");
    im->auth_plain = has_cap(im, "AUTH=PLAIN");
}

/* One complete response (line + literals) as raw bytes. NULL on I/O error. */
static char *read_raw(Imap *im, size_t *len) {
    char err[96];
    char *r = imap_read_response(im->c, len, LINE_MAX_BYTES, (size_t)64 << 20, err, sizeof err);
    if (!r) snprintf(im->text, sizeof im->text, "%s", err);
    return r;
}

typedef void (*Untagged)(Imap *im, IResp *r, void *u);

/* Waits for the tagged reply of `tag`, handing untagged responses to cb. 0 OK, 1 NO, 2 BAD, -1 I/O. */
static int wait_tag(Imap *im, int tag, Untagged cb, void *u) {
    char want[16];
    snprintf(want, sizeof want, "A%04d", tag);
    for (;;) {
        size_t len;
        char *raw = read_raw(im, &len);
        if (!raw) return -1;
        IResp r;
        if (imap_parse(raw, len, &r)) { dbg("unparsable IMAP response: %.120s", raw); free(raw); continue; }
        if (!strcmp(r.tag, "*")) {
            if (iv_is(r.first, "CAPABILITY") || (r.first && r.first->type == IV_ATOM && r.first->n > 12 && !strncasecmp(r.first->s, "[CAPABILITY", 11))) parse_caps(im, raw);
            if (r.first && r.first->next && iv_is(r.first->next, "EXISTS")) im->exists = (uint32_t)iv_num(r.first, 0);
            if (cb) cb(im, &r, u);
        } else if (!strcmp(r.tag, want)) {
            int st = iv_is(r.first, "OK") ? 0 : iv_is(r.first, "NO") ? 1 : 2;
            /* keep the human text and a response code */
            const char *t = raw + strlen(want) + 1;
            snprintf(im->text, sizeof im->text, "%s", t);
            for (char *p = im->text; *p; p++) if (*p == '\r' || *p == '\n') { *p = 0; break; }
            im->code[0] = 0;
            IVal *c = r.first ? r.first->next : NULL;
            if (c && c->type == IV_ATOM && c->n && c->s[0] == '[') snprintf(im->code, sizeof im->code, "%.*s", (int)(c->n < 127 ? c->n : 127), c->s);
            if (strcasestr(raw, "[CAPABILITY")) parse_caps(im, raw);
            imap_resp_free(&r);
            free(raw);
            return st;
        }
        imap_resp_free(&r);
        free(raw);
    }
}

static int cmd(Imap *im, Untagged cb, void *u, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int cmd(Imap *im, Untagged cb, void *u, const char *fmt, ...) {
    int tag = ++im->tag;
    char head[16];
    snprintf(head, sizeof head, "A%04d ", tag);
    va_list ap;
    va_start(ap, fmt);
    char small[8192];
    int k = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= sizeof small) return -1;
    dbg("%s> %.*s", im->a->id, strncasecmp(small, "LOGIN", 5) && strncasecmp(small, "AUTHENTICATE", 12) ? k : 12, small);
    if (conn_write(im->c, head, strlen(head)) || conn_write(im->c, small, (size_t)k) || conn_write(im->c, "\r\n", 2)) return -1;
    return wait_tag(im, tag, cb, u);
}

static void imap_close(Imap *im) {
    if (im->c) { conn_printf(im->c, "A%04d LOGOUT\r\n", ++im->tag); conn_close(im->c); }
    im->c = NULL;
    im->selected[0] = 0;
}

/* ---- connect + login ---- */

static void need_auth(Account *a) {
    if (a->need_auth_sent) return;
    a->need_auth_sent = 1;
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"ev\":\"need-auth\",\"acct\":"); jb_str(&b, a->id);
    jb_raw(&b, ",\"user\":"); jb_str(&b, a->user);
    jb_raw(&b, "}");
    send_event(&b);
}

static int imap_open(Imap *im, char *err, size_t ecap) {
    Account *a = im->a;
    memset((char *)im + sizeof(Account *), 0, sizeof *im - sizeof(Account *));
    pthread_mutex_lock(&G);
    char *pw = a->password ? strdup(a->password) : NULL;
    pthread_mutex_unlock(&G);
    if (!strcmp(a->auth, "oauth2")) { free(pw); snprintf(err, ecap, "oauth-disabled"); return -2; }
    if (!pw) { need_auth(a); snprintf(err, ecap, "no-password"); return -2; }
    TlsInfo ti;
    char why[64];
    im->c = open_conn(a->id, 0, a->imap.host, a->imap.port, a->imap.sec == SEC_SSL, a->pin_imap, &ti, why, sizeof why);
    if (!im->c) { free(pw); snprintf(err, ecap, "%s", why); return !strcmp(why, "cert") ? -3 : -1; }
    size_t len;
    char *greet = read_raw(im, &len);
    if (!greet) { free(pw); snprintf(err, ecap, "greeting"); imap_close(im); return -1; }
    parse_caps(im, greet);
    free(greet);
    if (a->imap.sec == SEC_STARTTLS) {
        if (!im->caps[0]) cmd(im, NULL, NULL, "CAPABILITY");
        if (!has_cap(im, "STARTTLS") || cmd(im, NULL, NULL, "STARTTLS")) { free(pw); snprintf(err, ecap, "no-starttls"); imap_close(im); return -1; }
        if (conn_tls(im->c, a->pin_imap, &ti)) {
            JBuf b;
            jb_init(&b);
            jb_raw(&b, "{\"ev\":\"cert\",\"acct\":"); jb_str(&b, a->id);
            jb_raw(&b, ",\"host\":"); jb_str(&b, a->imap.host);
            jb_printf(&b, ",\"port\":%d,\"reason\":", a->imap.port); jb_str(&b, ti.reason);
            jb_raw(&b, ",\"subject\":"); jb_str(&b, ti.subject);
            jb_raw(&b, ",\"issuer\":"); jb_str(&b, ti.issuer);
            jb_raw(&b, ",\"sha256\":"); jb_str(&b, ti.sha256);
            jb_raw(&b, "}");
            if (ti.code) send_event(&b); else jb_free(&b);
            free(pw);
            snprintf(err, ecap, "%s", ti.code ? "cert" : "tls");
            conn_close(im->c);
            im->c = NULL;
            return ti.code ? -3 : -1;
        }
        im->caps[0] = 0;
    } else if (a->imap.sec == SEC_NONE) {
        /* never send a password in the clear */
        free(pw);
        snprintf(err, ecap, "insecure");
        imap_close(im);
        return -1;
    }
    if (!im->caps[0] || !strcasestr(im->caps, "AUTH")) cmd(im, NULL, NULL, "CAPABILITY");
    int r;
    if (im->auth_plain) {
        size_t ul = strlen(a->user), pl = strlen(pw);
        unsigned char *raw = malloc(ul + pl + 3);
        if (!raw) { free(pw); imap_close(im); return -1; }
        raw[0] = 0;
        memcpy(raw + 1, a->user, ul);
        raw[1 + ul] = 0;
        memcpy(raw + 2 + ul, pw, pl);
        size_t el;
        char *e = b64_encode(raw, ul + pl + 2, 0, &el);
        explicit_bzero(raw, ul + pl + 2);
        free(raw);
        if (!e) { free(pw); imap_close(im); return -1; }
        if (im->sasl_ir) r = cmd(im, NULL, NULL, "AUTHENTICATE PLAIN %s", e);
        else {
            int tag = ++im->tag;
            conn_printf(im->c, "A%04d AUTHENTICATE PLAIN\r\n", tag);
            size_t ll;
            char *plus = read_raw(im, &ll);
            if (plus && plus[0] == '+') { conn_write(im->c, e, el); conn_write(im->c, "\r\n", 2); }
            free(plus);
            r = wait_tag(im, tag, NULL, NULL);
        }
        explicit_bzero(e, el);
        free(e);
    } else {
        int l1, l2;
        char *qu = imap_quote(a->user, im->literalplus, &l1), *qp = imap_quote(pw, im->literalplus, &l2);
        if ((l1 || l2) && !im->literalplus) r = 2;          /* non-ASCII without LITERAL+: use PLAIN */
        else r = qu && qp ? cmd(im, NULL, NULL, "LOGIN %s %s", qu, qp) : -1;
        if (qp) { explicit_bzero(qp, strlen(qp)); free(qp); }
        free(qu);
    }
    explicit_bzero(pw, strlen(pw));
    free(pw);
    if (r) {
        SNPF(err, ecap, r < 0 ? "io" : "auth: %s", im->text);
        imap_close(im);
        if (r > 0) {
            /* wrong password: ask again (from the keyring prompt) */
            pthread_mutex_lock(&G);
            if (a->password) { explicit_bzero(a->password, strlen(a->password)); free(a->password); a->password = NULL; }
            pthread_mutex_unlock(&G);
            a->need_auth_sent = 0;
            JBuf b;
            jb_init(&b);
            jb_raw(&b, "{\"ev\":\"auth-failed\",\"acct\":"); jb_str(&b, a->id);
            jb_raw(&b, ",\"detail\":"); jb_str(&b, im->text);
            jb_raw(&b, "}");
            send_event(&b);
            return -2;
        }
        return -1;
    }
    if (!strcasestr(im->caps, "IMAP4")) cmd(im, NULL, NULL, "CAPABILITY");
    if (im->condstore && has_cap(im, "ENABLE")) cmd(im, NULL, NULL, "ENABLE CONDSTORE");
    return 0;
}

/* ---- folders ---- */

typedef struct { Account *a; char seen[512][160]; int nseen; int gmail; } ListCtx;

static void on_list(Imap *im, IResp *r, void *u) {
    ListCtx *lc = u;
    if (!iv_is(r->first, "LIST") && !iv_is(r->first, "XLIST")) return;
    IVal *flags = iv_at(r->first, 1), *delim = iv_at(r->first, 2), *name = iv_at(r->first, 3);
    if (!flags || flags->type != IV_LIST || !name) return;
    char fl[512] = "", nm[512], d[8];
    size_t o = 0;
    for (IVal *k = flags->kid; k; k = k->next) { o += (size_t)snprintf(fl + o, sizeof fl - o, "%.*s ", (int)k->n, k->s); if (o >= sizeof fl) break; }
    iv_copy(name, nm, sizeof nm);
    iv_copy(delim, d, sizeof d);
    if (!nm[0]) return;
    if (!strncasecmp(nm, "[Gmail]", 7) || !strncasecmp(nm, "[Google Mail]", 13)) lc->gmail = 1;
    pthread_mutex_lock(&G);
    Folder *f = store_folder(im->a, !strcasecmp(nm, "INBOX") ? "INBOX" : nm, 1);
    if (f) {
        char disp[512];
        imap_utf7_decode(f->name, disp, sizeof disp);
        /* show the last level ("[Gmail]/Sent Mail" → "Sent Mail") */
        const char *last = d[0] ? strrchr(disp, d[0]) : NULL;
        snprintf(f->display, sizeof f->display, "%s", !strcasecmp(f->name, "INBOX") ? "INBOX" : last ? last + 1 : disp);
        snprintf(f->special, sizeof f->special, "%s", special_of(f->name, fl));
        f->delim = d[0];
        f->noselect = strcasestr(fl, "\\Noselect") || strcasestr(fl, "\\NonExistent");
    }
    pthread_mutex_unlock(&G);
    if (lc->nseen < 512) SNPF(lc->seen[lc->nseen++], sizeof lc->seen[0], "%s", !strcasecmp(nm, "INBOX") ? "INBOX" : nm);
}

static int sync_folder_list(Imap *im) {
    ListCtx *lc = calloc(1, sizeof *lc);
    if (!lc) return -1;
    lc->a = im->a;
    int r = im->special_use ? cmd(im, on_list, lc, "LIST \"\" \"*\" RETURN (SPECIAL-USE)") : cmd(im, on_list, lc, "LIST \"\" \"*\"");
    if (r == 0) {
        pthread_mutex_lock(&G);
        FolderList *fl = store_folders(im->a);
        for (int i = 0; fl && i < fl->n; i++) {
            int found = 0;
            for (int k = 0; k < lc->nseen; k++) if (!strcmp(lc->seen[k], fl->f[i]->name)) { found = 1; break; }
            if (!found && lc->nseen < 512) { store_clear(im->a, fl->f[i]); free(fl->f[i]->msgs); free(fl->f[i]); fl->f[i] = fl->f[--fl->n]; i--; }
        }
        /* Gmail files sent mail itself */
        if (lc->gmail) im->a->sent_saved = 1;
        store_save_folders(im->a);
        pthread_mutex_unlock(&G);
        JBuf b;
        jb_init(&b);
        jb_raw(&b, "{\"ev\":\"folders\",\"acct\":"); jb_str(&b, im->a->id); jb_raw(&b, "}");
        send_event(&b);
    }
    free(lc);
    return r;
}

/* ---- messages ---- */

static uint32_t parse_flags(const IVal *list) {
    uint32_t f = 0;
    for (const IVal *k = list && list->type == IV_LIST ? list->kid : NULL; k; k = k->next) {
        if (iv_is(k, "\\Seen")) f |= F_SEEN;
        else if (iv_is(k, "\\Flagged")) f |= F_FLAGGED;
        else if (iv_is(k, "\\Answered")) f |= F_ANSWERED;
        else if (iv_is(k, "\\Deleted")) f |= F_DELETED;
        else if (iv_is(k, "\\Draft")) f |= F_DRAFT;
        else if (iv_is(k, "$Forwarded") || iv_is(k, "Forwarded")) f |= F_FORWARDED;
        else if (iv_is(k, "$Junk") || iv_is(k, "Junk")) f |= F_JUNK;
        else if (k->type == IV_ATOM && k->n == 7 && !strncasecmp(k->s, "$label", 6) && k->s[6] >= '1' && k->s[6] <= '6') f |= F_LABEL1 << (k->s[6] - '1');
    }
    return f;
}

static time_t internaldate(const IVal *v) {
    char d[64];
    iv_copy(v, d, sizeof d);
    for (char *p = d; *p; p++) if (*p == '-' && p - d < 12) *p = ' ';
    return mime_date(d);
}

typedef struct { Account *a; Folder *f; uint32_t old_uidnext; int added, fresh; JBuf news; int nnews; } FetchCtx;

static void addr_list_text(const char *raw, char *out, size_t cap) {
    char dec[2048];
    out[0] = 0;
    MimeAddr ad[24];
    int n = mime_addrs(raw, ad, 24);
    size_t o = 0;
    for (int i = 0; i < n && o + 4 < cap; i++) {
        mime_decode_words(ad[i].name, dec, sizeof dec);
        o += (size_t)snprintf(out + o, cap - o, "%s%s <%s>", i ? ", " : "", dec[0] ? dec : "", ad[i].email);
    }
    if (o >= cap) out[cap - 1] = 0;
}

static void on_fetch(Imap *im, IResp *r, void *u) {
    (void)im;
    FetchCtx *fc = u;
    if (!r->first || !iv_is(r->first->next, "FETCH")) return;
    IVal *list = r->first->next->next;
    if (!list || list->type != IV_LIST) return;
    uint32_t uid = (uint32_t)iv_num(imap_fetch_item(list, "UID"), 0);
    if (!uid) return;
    IVal *hdr = imap_fetch_item(list, "BODY[HEADER");
    IVal *flags = imap_fetch_item(list, "FLAGS");
    IVal *ms = imap_fetch_item(list, "MODSEQ");
    (void)ms;
    if (!hdr) {
        /* flags only */
        pthread_mutex_lock(&G);
        Msg *m = store_msg(fc->f, uid);
        if (m && flags) {
            uint32_t nf = (m->flags & ~(uint32_t)(F_SEEN | F_FLAGGED | F_ANSWERED | F_DELETED | F_DRAFT | F_FORWARDED | F_JUNK | F_LABELS)) | parse_flags(flags);
            if (nf != m->flags) {
                if ((nf ^ m->flags) & F_SEEN) fc->f->unread += (nf & F_SEEN) ? -1 : 1;
                m->flags = nf;
                fc->f->dirty = 1;
                fc->f->version++;
                fc->added++;
            }
        }
        pthread_mutex_unlock(&G);
        return;
    }
    IVal *text = imap_fetch_item(list, "BODY[TEXT]");
    const char *h = hdr->type == IV_STR ? hdr->s : "";
    size_t hl = hdr->type == IV_STR ? hdr->n : 0;
    char v[4096], dec[2048];
    Msg m;
    memset(&m, 0, sizeof m);
    m.uid = uid;
    m.flags = parse_flags(flags);
    m.size = (uint32_t)iv_num(imap_fetch_item(list, "RFC822.SIZE"), 0);
    char from_name[512] = "", from_email[512] = "", to[1024] = "", cc[1024] = "", subject[1024] = "", msgid[512] = "", irt[512] = "";
    if (mime_header(h, hl, "From", v, sizeof v)) {
        MimeAddr ad[1];
        if (mime_addrs(v, ad, 1) == 1) { snprintf(from_name, sizeof from_name, "%s", ad[0].name); snprintf(from_email, sizeof from_email, "%s", ad[0].email); }
    }
    if (mime_header(h, hl, "To", v, sizeof v)) addr_list_text(v, to, sizeof to);
    if (mime_header(h, hl, "Cc", v, sizeof v)) addr_list_text(v, cc, sizeof cc);
    if (mime_header(h, hl, "Subject", v, sizeof v)) { mime_decode_words(v, dec, sizeof dec); SNPF(subject, sizeof subject, "%s", dec); }
    mime_header(h, hl, "Message-ID", msgid, sizeof msgid);
    mime_header(h, hl, "In-Reply-To", irt, sizeof irt);
    char *refs = mime_header_dup(h, hl, "References");
    if (refs && strlen(refs) > 2000) {
        /* keep the root and the last few ids */
        char *last = refs + strlen(refs) - 1200;
        char *lt = strchr(last, '<');
        char *e = strchr(refs, '>');
        if (lt && e && e < lt) { size_t a1 = (size_t)(e - refs + 1); memmove(refs + a1 + 1, lt, strlen(lt) + 1); refs[a1] = ' '; }
    }
    m.date = mime_header(h, hl, "Date", v, sizeof v) ? mime_date(v) : 0;
    if (!m.date) m.date = internaldate(imap_fetch_item(list, "INTERNALDATE"));
    if (!m.date) m.date = time(NULL);
    char preview[400] = "";
    int att = 0;
    msg_preview(h, hl, text && text->type == IV_STR ? text->s : "", text && text->type == IV_STR ? text->n : 0, preview, sizeof preview, &att);
    m.att = (uint16_t)att;
    m.from_name = from_name; m.from_email = from_email; m.to = to; m.cc = cc; m.subject = subject;
    m.msgid = msgid; m.inreplyto = irt; m.refs = refs ? refs : (char *)""; m.preview = preview;
    pthread_mutex_lock(&G);
    int is_new = !store_msg(fc->f, uid);
    store_add(fc->a, fc->f, &m);
    fc->added++;
    pthread_mutex_unlock(&G);
    /* new mail notification: new since the last sync, unread, recent */
    if (is_new && fc->fresh && uid >= fc->old_uidnext && !(m.flags & F_SEEN) && m.date > time(NULL) - 86400 && fc->nnews < 5) {
        if (fc->nnews) jb_raw(&fc->news, ",");
        jb_raw(&fc->news, "{\"from\":"); jb_str(&fc->news, from_name[0] ? from_name : from_email);
        jb_raw(&fc->news, ",\"subject\":"); jb_str(&fc->news, subject);
        jb_raw(&fc->news, ",\"preview\":"); jb_str(&fc->news, preview);
        jb_printf(&fc->news, ",\"uid\":%u}", uid);
        fc->nnews++;
    } else if (is_new && fc->fresh && uid >= fc->old_uidnext && !(m.flags & F_SEEN)) fc->nnews++;
    free(refs);
}

typedef struct { uint32_t *u; size_t n, cap; } Uids;

static void on_search(Imap *im, IResp *r, void *u) {
    (void)im;
    Uids *s = u;
    if (!iv_is(r->first, "SEARCH") && !iv_is(r->first, "ESEARCH")) return;
    for (IVal *k = r->first->next; k; k = k->next) {
        if (k->type == IV_ATOM && iv_is(k, "ALL") && k->next) {           /* ESEARCH ... ALL 1:5,7 */
            char set[1 << 16];
            iv_copy(k->next, set, sizeof set);
            uint32_t tmp[8192];
            size_t c = imap_parse_uidset(set, tmp, 8192);
            for (size_t i = 0; i < c; i++) { if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 1024; uint32_t *nu = realloc(s->u, s->cap * sizeof *nu); if (!nu) return; s->u = nu; } s->u[s->n++] = tmp[i]; }
            break;
        }
        long long x = iv_num(k, -1);
        if (x <= 0 || x > 4294967295LL) continue;
        if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 1024; uint32_t *nu = realloc(s->u, s->cap * sizeof *nu); if (!nu) return; s->u = nu; }
        s->u[s->n++] = (uint32_t)x;
    }
}

static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }

typedef struct { uint32_t uidvalidity, uidnext; uint64_t hms; int nomodseq; } SelInfo;

static void on_select(Imap *im, IResp *r, void *u) {
    (void)im;
    SelInfo *si = u;
    unsigned long long v;
    if (imap_code_num(r, "UIDVALIDITY", &v)) si->uidvalidity = (uint32_t)v;
    if (imap_code_num(r, "UIDNEXT", &v)) si->uidnext = (uint32_t)v;
    if (imap_code_num(r, "HIGHESTMODSEQ", &v)) si->hms = v;
    for (IVal *k = r->first; k; k = k->next) if (k->type == IV_ATOM && k->n >= 9 && !strncasecmp(k->s, "[NOMODSEQ", 9)) si->nomodseq = 1;
}

static int select_folder(Imap *im, const char *name, SelInfo *si, int readonly) {
    SelInfo tmp;
    if (!si) si = &tmp;
    memset(si, 0, sizeof *si);
    if (!strcmp(im->selected, name) && si == &tmp) return 0;
    int lit;
    char *q = imap_quote(name, im->literalplus, &lit);
    if (!q) return -1;
    im->exists = 0;
    int r = cmd(im, on_select, si, "%s %s%s", readonly ? "EXAMINE" : "SELECT", q, im->condstore ? " (CONDSTORE)" : "");
    free(q);
    if (r == 0) snprintf(im->selected, sizeof im->selected, "%s", name);
    else im->selected[0] = 0;
    return r;
}

static void folder_event(Account *a, Folder *f, const char *ev) {
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"ev\":\"%s\",\"acct\":", ev);
    jb_str(&b, a->id);
    jb_raw(&b, ",\"folder\":");
    jb_str(&b, f->name);
    jb_printf(&b, ",\"total\":%d,\"unread\":%d}", f->n, f->unread);
    send_event(&b);
}

static int sync_folder(Imap *im, Folder *f) {
    Account *a = im->a;
    if (f->noselect) return 0;
    SelInfo si;
    int r = select_folder(im, f->name, &si, 0);
    if (r) return r < 0 ? -1 : 0;
    pthread_mutex_lock(&G);
    uint32_t old_next = f->uidnext;
    int known = f->uidvalidity != 0;
    if (f->uidvalidity && si.uidvalidity && f->uidvalidity != si.uidvalidity) { store_clear(a, f); known = 0; }
    if (si.uidvalidity) f->uidvalidity = si.uidvalidity;
    int unchanged = f->synced && si.uidnext && f->uidnext == si.uidnext && si.hms && f->modseq == si.hms && (uint32_t)f->n == im->exists;
    int cached = f->n;
    pthread_mutex_unlock(&G);
    if (unchanged) return 0;
    /* which uids exist on the server */
    Uids s = { 0 };
    if (im->exists) {
        if (cmd(im, on_search, &s, "UID SEARCH ALL")) { free(s.u); return -1; }
        qsort(s.u, s.n, sizeof *s.u, cmp_u32);
    }
    /* gone from the server */
    pthread_mutex_lock(&G);
    for (int i = f->n - 1; i >= 0; i--) {
        uint32_t u = f->msgs[i].uid;
        if (!bsearch(&u, s.u, s.n, sizeof *s.u, cmp_u32)) store_remove(a, f, u);
    }
    /* new ones, newest first */
    uint32_t *want = malloc((s.n ? s.n : 1) * sizeof *want);
    size_t nw = 0;
    for (size_t i = s.n; want && i-- > 0;) if (!store_msg(f, s.u[i])) want[nw++] = s.u[i];
    pthread_mutex_unlock(&G);
    FetchCtx fc;
    memset(&fc, 0, sizeof fc);
    fc.a = a; fc.f = f; fc.old_uidnext = old_next; fc.fresh = known && !strcmp(f->special, "inbox");
    jb_init(&fc.news);
    int rc = 0;
    for (size_t off = 0; want && off < nw && !a->stop; off += CHUNK) {
        size_t n = nw - off < CHUNK ? nw - off : CHUNK;
        uint32_t chunk[CHUNK];
        for (size_t i = 0; i < n; i++) chunk[i] = want[off + n - 1 - i];      /* ascending for a compact set */
        char set[CHUNK * 12];
        imap_uidset(chunk, n, set, sizeof set);
        rc = cmd(im, on_fetch, &fc, "UID FETCH %s (UID FLAGS INTERNALDATE RFC822.SIZE BODY.PEEK[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID IN-REPLY-TO "
                                    "REFERENCES CONTENT-TYPE CONTENT-TRANSFER-ENCODING)] BODY.PEEK[TEXT]<0.2048>)", set);
        if (rc < 0) break;
        pthread_mutex_lock(&G);
        store_save(a, f);
        pthread_mutex_unlock(&G);
        folder_event(a, f, "changed");
        /* a waiting job (opening a message) goes first */
        pthread_mutex_lock(&a->qmu);
        int urgent = a->jobs && a->jobs->prio >= 5;
        pthread_mutex_unlock(&a->qmu);
        if (urgent) break;
    }
    int complete = !want || fc.added >= (int)nw || rc < 0 ? (rc >= 0) : 0;
    free(want);
    /* flags of what we already had */
    if (rc >= 0 && cached) {
        if (im->condstore && f->modseq && si.hms && !si.nomodseq)
            rc = cmd(im, on_fetch, &fc, "UID FETCH 1:* (UID FLAGS) (CHANGEDSINCE %llu)", (unsigned long long)f->modseq);
        else if (s.n <= 6000) rc = cmd(im, on_fetch, &fc, "UID FETCH 1:* (UID FLAGS)");
        else {
            char set[64];
            snprintf(set, sizeof set, "%u:*", s.u[s.n - 2000]);
            rc = cmd(im, on_fetch, &fc, "UID FETCH %s (UID FLAGS)", set);
        }
    }
    free(s.u);
    pthread_mutex_lock(&G);
    if (rc >= 0 && complete) {
        if (si.uidnext) f->uidnext = si.uidnext;
        if (si.hms) f->modseq = si.hms;
        f->synced = 1;
    }
    f->dirty = 1;
    store_save(a, f);
    pthread_mutex_unlock(&G);
    folder_event(a, f, "changed");
    if (fc.nnews) {
        JBuf b;
        jb_init(&b);
        jb_raw(&b, "{\"ev\":\"new\",\"acct\":"); jb_str(&b, a->id);
        jb_raw(&b, ",\"name\":"); jb_str(&b, a->name[0] ? a->name : a->email);
        jb_raw(&b, ",\"folder\":"); jb_str(&b, f->name);
        jb_printf(&b, ",\"count\":%d,\"msgs\":[", fc.nnews);
        if (fc.news.p) jb_raw(&b, fc.news.p);
        jb_raw(&b, "]}");
        send_event(&b);
    }
    jb_free(&fc.news);
    return rc < 0 ? -1 : 0;
}

/* ---- bodies ---- */

typedef struct { char *raw; size_t len; } BodyCtx;
static void on_body(Imap *im, IResp *r, void *u) {
    (void)im;
    BodyCtx *bc = u;
    if (!r->first || !iv_is(r->first->next, "FETCH")) return;
    IVal *list = r->first->next->next;
    IVal *b = imap_fetch_item(list, "BODY[]");
    if (b && b->type == IV_STR && !bc->raw) { bc->raw = malloc(b->n + 1); if (bc->raw) { memcpy(bc->raw, b->s, b->n); bc->raw[b->n] = 0; bc->len = b->n; } }
}

static int fetch_body(Imap *im, Folder *f, uint32_t uid) {
    if (select_folder(im, f->name, NULL, 0)) return -1;
    BodyCtx bc = { 0 };
    int r = cmd(im, on_body, &bc, "UID FETCH %u (BODY.PEEK[])", uid);
    if (r || !bc.raw) { free(bc.raw); return r < 0 ? -1 : 1; }
    char rel[800];
    store_body_path(im->a, f, uid, rel, sizeof rel);
    int ok = fs_write(rel, bc.raw, bc.len) == 0;
    char *text = msg_body_text(bc.raw, bc.len);
    if (text) {
        char tr[800];
        snprintf(tr, sizeof tr, "%s/f/%s/t/%u", im->a->id, f->dir, uid);
        size_t tl = strlen(text) > 16000 ? 16000 : strlen(text);
        fs_write(tr, text, tl);
    }
    pthread_mutex_lock(&G);
    Msg *m = store_msg(f, uid);
    if (m && ok) {
        m->body = 1;
        if (text) { text[strlen(text) > 16000 ? 16000 : strlen(text)] = 0; store_msg_text(m, text); store_reindex(im->a, f, m); }
        f->dirty = 1;
    }
    store_save(im->a, f);
    pthread_mutex_unlock(&G);
    free(text);
    free(bc.raw);
    return ok ? 0 : -1;
}

/* ---- jobs ---- */

static const char *flag_atom(const char *n) {
    static const struct { const char *n, *a; } t[] = {
        { "seen", "\\Seen" }, { "flagged", "\\Flagged" }, { "answered", "\\Answered" }, { "forwarded", "$Forwarded" }, { "junk", "$Junk" },
        { "label1", "$label1" }, { "label2", "$label2" }, { "label3", "$label3" }, { "label4", "$label4" }, { "label5", "$label5" }, { "label6", "$label6" },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(n, t[i].n)) return t[i].a;
    return NULL;
}

static void flags_text(const JVal *arr, char *out, size_t cap) {
    size_t o = 0;
    out[0] = 0;
    for (const JVal *x = arr && arr->type == J_ARR ? arr->kid : NULL; x; x = x->next) {
        const char *a = x->type == J_STR ? flag_atom(x->str) : NULL;
        if (a && o + strlen(a) + 2 < cap) o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? " " : "", a);
    }
}

static Folder *special_folder(Account *a, const char *sp) {
    FolderList *fl = store_folders(a);
    for (int i = 0; fl && i < fl->n; i++) if (!strcmp(fl->f[i]->special, sp)) return fl->f[i];
    return NULL;
}

static int do_expunge(Imap *im, const char *set) {
    return im->uidplus ? cmd(im, NULL, NULL, "UID EXPUNGE %s", set) : cmd(im, NULL, NULL, "EXPUNGE");
}

static void reply_ok(uint32_t req, const char *extra) {
    if (!req) return;
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"ok\":true");
    if (extra) jb_raw(&b, extra);
    jb_raw(&b, "}");
    reply_done(req, &b);
}

static int run_move(Imap *im, Job *j, const char *dest) {
    Account *a = im->a;
    pthread_mutex_lock(&G);
    Folder *f = store_folder(a, j->folder, 0);
    pthread_mutex_unlock(&G);
    if (!f) return 1;
    qsort(j->uids, (size_t)j->nuids, sizeof *j->uids, cmp_u32);
    char *set = malloc((size_t)j->nuids * 12 + 16);
    if (!set) return -1;
    imap_uidset(j->uids, (size_t)j->nuids, set, (size_t)j->nuids * 12 + 16);
    if (select_folder(im, f->name, NULL, 0)) { free(set); return -1; }
    int r;
    if (dest) {
        int lit;
        char *q = imap_quote(dest, im->literalplus, &lit);
        if (!q) { free(set); return -1; }
        if (im->move) r = cmd(im, NULL, NULL, "UID MOVE %s %s", set, q);
        else {
            r = cmd(im, NULL, NULL, "UID COPY %s %s", set, q);
            if (!r) r = cmd(im, NULL, NULL, "UID STORE %s +FLAGS.SILENT (\\Deleted)", set);
            if (!r) r = do_expunge(im, set);
        }
        free(q);
    } else {
        r = cmd(im, NULL, NULL, "UID STORE %s +FLAGS.SILENT (\\Deleted)", set);
        if (!r) r = do_expunge(im, set);
    }
    free(set);
    if (r == 0) {
        pthread_mutex_lock(&G);
        for (int i = 0; i < j->nuids; i++) store_remove(a, f, j->uids[i]);
        store_save(a, f);
        Folder *df = dest ? store_folder(a, dest, 0) : NULL;
        pthread_mutex_unlock(&G);
        folder_event(a, f, "changed");
        if (df) sync_folder(im, df);
    }
    return r;
}

static void run_job(Imap *im, Job *j) {
    Account *a = im->a;
    JDoc d;
    int have = j->arg && !json_parse(j->arg, j->arglen, &d, NULL, 0);
    const JVal *args = have ? d.root : NULL;
    int r = 0;
    switch (j->kind) {
    case J_SYNC_ALL: {
        r = sync_folder_list(im);
        if (r < 0) break;
        /* inbox first, then the rest (the "all mail" archive only when opened) */
        pthread_mutex_lock(&G);
        FolderList *fl = store_folders(a);
        int n = fl ? fl->n : 0;
        char (*names)[512] = n ? malloc((size_t)n * 512) : NULL;
        int k = 0;
        for (int pass = 0; names && pass < 2; pass++)
            for (int i = 0; i < n; i++) {
                Folder *f = fl->f[i];
                int inbox = !strcmp(f->special, "inbox");
                if ((pass == 0) != inbox || !strcmp(f->special, "all") || f->noselect) continue;
                snprintf(names[k++], 512, "%s", f->name);
            }
        pthread_mutex_unlock(&G);
        for (int i = 0; i < k && !a->stop; i++) {
            pthread_mutex_lock(&G);
            Folder *f = store_folder(a, names[i], 0);
            pthread_mutex_unlock(&G);
            if (f && sync_folder(im, f) < 0) { r = -1; break; }
            /* let urgent jobs through between folders */
            pthread_mutex_lock(&a->qmu);
            Job *urgent = a->jobs && a->jobs->prio >= 5 ? a->jobs : NULL;
            if (urgent) a->jobs = urgent->next;
            pthread_mutex_unlock(&a->qmu);
            if (urgent) { run_job(im, urgent); job_free(urgent); }
        }
        free(names);
        if (r >= 0) {
            a->last_sync = time(NULL);
            a->initial_done = 1;
            /* download the newest bodies of the inbox for offline reading and full-text search */
            pthread_mutex_lock(&G);
            Folder *in = store_folder(a, "INBOX", 0);
            uint32_t pre[40];
            int np = 0;
            for (int i = in ? in->n - 1 : -1; i >= 0 && np < 40; i--) if (!in->msgs[i].body && in->msgs[i].size < (2u << 20)) pre[np++] = in->msgs[i].uid;
            pthread_mutex_unlock(&G);
            for (int i = 0; i < np; i++) { Job *pj = job_new(J_FETCH_BODY, 0, 0); if (pj) { snprintf(pj->folder, sizeof pj->folder, "INBOX"); pj->uid = pre[i]; job_push(a, pj); } }
        }
        break;
    }
    case J_SYNC_FOLDER: {
        pthread_mutex_lock(&G);
        Folder *f = store_folder(a, j->folder, 0);
        pthread_mutex_unlock(&G);
        if (f) r = sync_folder(im, f);
        break;
    }
    case J_FETCH_BODY: {
        pthread_mutex_lock(&G);
        Folder *f = store_folder(a, j->folder, 0);
        Msg *m = f ? store_msg(f, j->uid) : NULL;
        int have_body = m && m->body;
        pthread_mutex_unlock(&G);
        if (!f || !m) { if (j->req) reply_err(j->req, "not-found", ""); break; }
        if (!have_body) r = fetch_body(im, f, j->uid);
        if (j->req) {
            if (r == 0) msg_open_cached(j->req, a, f, j->uid);
            else reply_err(j->req, r < 0 ? "offline" : "not-found", im->text);
        }
        if (r > 0) r = 0;
        break;
    }
    case J_FLAGS: {
        pthread_mutex_lock(&G);
        Folder *f = store_folder(a, j->folder, 0);
        pthread_mutex_unlock(&G);
        if (!f) { if (j->req) reply_err(j->req, "no-folder", ""); break; }
        char add[256], rem[256];
        flags_text(json_get(args, "add"), add, sizeof add);
        flags_text(json_get(args, "remove"), rem, sizeof rem);
        qsort(j->uids, (size_t)j->nuids, sizeof *j->uids, cmp_u32);
        char *set = malloc((size_t)j->nuids * 12 + 16);
        if (!set) break;
        imap_uidset(j->uids, (size_t)j->nuids, set, (size_t)j->nuids * 12 + 16);
        r = select_folder(im, f->name, NULL, 0);
        if (!r && add[0]) r = cmd(im, NULL, NULL, "UID STORE %s +FLAGS.SILENT (%s)", set, add);
        if (!r && rem[0]) r = cmd(im, NULL, NULL, "UID STORE %s -FLAGS.SILENT (%s)", set, rem);
        free(set);
        if (r > 0) { if (j->req) reply_err(j->req, "server", im->text); r = 0; break; }
        if (!r) reply_ok(j->req, NULL);
        break;
    }
    case J_MOVE: {
        const char *to = json_str(args, "to");
        if (!to) { reply_err(j->req, "no-target", ""); break; }
        r = run_move(im, j, to);
        if (r > 0) { reply_err(j->req, "server", im->text); r = 0; } else if (!r) reply_ok(j->req, NULL);
        break;
    }
    case J_DELETE: {
        pthread_mutex_lock(&G);
        Folder *f = store_folder(a, j->folder, 0);
        Folder *trash = special_folder(a, "trash");
        char tname[512] = "";
        if (trash && trash != f) snprintf(tname, sizeof tname, "%s", trash->name);
        pthread_mutex_unlock(&G);
        r = run_move(im, j, tname[0] && !json_bool(args, "permanent", 0) ? tname : NULL);
        if (r > 0) { reply_err(j->req, "server", im->text); r = 0; } else if (!r) reply_ok(j->req, NULL);
        break;
    }
    case J_CREATE_FOLDER: {
        char enc[600];
        imap_utf7_encode(j->folder, enc, sizeof enc);
        int lit;
        char *q = imap_quote(enc, im->literalplus, &lit);
        r = q ? cmd(im, NULL, NULL, "CREATE %s", q) : -1;
        if (!r) cmd(im, NULL, NULL, "SUBSCRIBE %s", q);
        free(q);
        if (!r) { sync_folder_list(im); reply_ok(j->req, NULL); }
        else if (r > 0) { reply_err(j->req, "server", im->text); r = 0; }
        break;
    }
    case J_APPEND_DRAFT: {
        /* a draft (from the UI) or a sent copy (from the sender): append the raw message */
        char msgid[300] = "", err[200] = "";
        size_t ml = 0;
        char *msg = NULL;
        const char *target = "drafts";
        if (args && json_get(args, "raw")) { target = json_str(args, "target") ? json_str(args, "target") : "sent"; }
        if (args && json_str(args, "raw")) { msg = strdup(json_str(args, "raw")); ml = msg ? strlen(msg) : 0; }
        else msg = compose_message(a, args, j->fds, j->nfds, &ml, msgid, sizeof msgid, err, sizeof err);
        if (!msg) { reply_err(j->req, "compose", err); break; }
        pthread_mutex_lock(&G);
        Folder *dest = special_folder(a, target);
        char dname[512] = "";
        if (dest) snprintf(dname, sizeof dname, "%s", dest->name);
        pthread_mutex_unlock(&G);
        if (!dname[0]) snprintf(dname, sizeof dname, "%s", !strcmp(target, "drafts") ? "Drafts" : "Sent");
        int lit;
        char *q = imap_quote(dname, im->literalplus, &lit);
        int tag = ++im->tag;
        const char *fl = !strcmp(target, "drafts") ? "(\\Draft \\Seen)" : "(\\Seen)";
        if (im->literalplus) conn_printf(im->c, "A%04d APPEND %s %s {%zu+}\r\n", tag, q ? q : "\"Drafts\"", fl, ml);
        else {
            conn_printf(im->c, "A%04d APPEND %s %s {%zu}\r\n", tag, q ? q : "\"Drafts\"", fl, ml);
            size_t ll;
            char *plus = read_raw(im, &ll);
            int go = plus && plus[0] == '+';
            free(plus);
            if (!go) { free(q); free(msg); r = wait_tag(im, tag, NULL, NULL); reply_err(j->req, "append", im->text); break; }
        }
        conn_write(im->c, msg, ml);
        conn_write(im->c, "\r\n", 2);
        r = wait_tag(im, tag, NULL, NULL);
        free(q);
        free(msg);
        if (r == 0) {
            unsigned long long nu = 0;
            const char *ap = strcasestr(im->code, "APPENDUID");
            if (ap) { unsigned long long v1; if (sscanf(ap, "APPENDUID %llu %llu", &v1, &nu) != 2) nu = 0; }
            /* replace the previous version of a draft */
            double old = json_num(args, "replace", 0);
            if (old >= 1 && old < 4294967296.0) {
                Job tmp = { 0 };
                uint32_t u = (uint32_t)old;
                tmp.uids = &u; tmp.nuids = 1;
                snprintf(tmp.folder, sizeof tmp.folder, "%s", dname);
                run_move(im, &tmp, NULL);
                tmp.uids = NULL;
            }
            pthread_mutex_lock(&G);
            Folder *df = store_folder(a, dname, 0);
            pthread_mutex_unlock(&G);
            if (df) sync_folder(im, df);
            char extra[64];
            snprintf(extra, sizeof extra, ",\"uid\":%llu", nu);
            reply_ok(j->req, extra);
        } else if (r > 0) { reply_err(j->req, "append", im->text); r = 0; }
        break;
    }
    default: break;
    }
    if (r < 0 && j->req && j->kind != J_FETCH_BODY) reply_err(j->req, "offline", im->c ? conn_error(im->c) : "connection lost");
    if (have) json_free(&d);
}

/* ---- the worker ---- */

static int idle_wait(Imap *im, int *need_sync) {
    Account *a = im->a;
    *need_sync = 0;
    if (strcmp(im->selected, "INBOX") && select_folder(im, "INBOX", NULL, 0)) return -1;
    if (!im->idle) {
        int w = conn_wait(im->c, a->wake, 5 * 60 * 1000);
        if (w < 0) return -1;
        if (w == 1) { size_t l; char *x = read_raw(im, &l); if (!x) return -1; free(x); }
        *need_sync = w != 2;
        return 0;
    }
    int tag = ++im->tag;
    if (conn_printf(im->c, "A%04d IDLE\r\n", tag)) return -1;
    size_t len;
    char *plus = read_raw(im, &len);
    if (!plus) return -1;
    int ok = plus[0] == '+';
    free(plus);
    if (!ok) return wait_tag(im, tag, NULL, NULL) < 0 ? -1 : 0;
    acct_status(a, "idle", "");
    for (;;) {
        int w = conn_wait(im->c, a->wake, 25 * 60 * 1000);
        if (w < 0) return -1;
        if (w == 1) {
            char *x = read_raw(im, &len);
            if (!x) return -1;
            if (strcasestr(x, "EXISTS") || strcasestr(x, "EXPUNGE") || strcasestr(x, "FETCH")) *need_sync = 1;
            int bye = !strncasecmp(x, "* BYE", 5);
            free(x);
            if (bye) return -1;
            if (!*need_sync) continue;
        }
        break;          /* new mail, a job, or the 25-minute refresh */
    }
    if (conn_write(im->c, "DONE\r\n", 6)) return -1;
    return wait_tag(im, tag, NULL, NULL) < 0 ? -1 : 0;
}

void *mail_worker(void *arg) {
    Account *a = arg;
    Imap im;
    memset(&im, 0, sizeof im);
    im.a = a;
    int backoff = 5;
    Job *first = job_new(J_SYNC_ALL, 0, 1);
    if (first) job_push(a, first);
    time_t last_full = 0;
    while (!a->stop) {
        if (!im.c) {
            char err[200];
            acct_status(a, "connecting", "");
            int r = imap_open(&im, err, sizeof err);
            if (r) {
                acct_status(a, r == -2 ? "auth" : r == -3 ? "cert" : "offline", err);
                /* fail the queued requests that need the server, keep the rest */
                Job *j;
                while ((j = job_pop(a))) { if (j->req) { if (j->kind == J_FETCH_BODY) reply_err(j->req, "offline", err); else reply_err(j->req, r == -2 ? "auth" : "offline", err); } job_free(j); }
                struct pollfd p = { a->wake, POLLIN, 0 };
                poll(&p, 1, r == -2 || r == -3 ? -1 : backoff * 1000);   /* auth/cert: until something changes */
                uint64_t v;
                if (read(a->wake, &v, sizeof v) < 0) { /* timeout */ }
                if (r == -1 && backoff < 300) backoff *= 2;
                /* offline: try again after the back-off; no password or a bad certificate: wait
                 * until the user does something (a new password or config wakes us up) */
                if (r == -1 && !a->jobs) { Job *s = job_new(J_SYNC_ALL, 0, 1); if (s) { pthread_mutex_lock(&a->qmu); s->next = a->jobs; a->jobs = s; pthread_mutex_unlock(&a->qmu); } }
                continue;
            }
            backoff = 5;
            acct_status(a, "sync", "");
            last_full = time(NULL);
        }
        uint64_t v;
        if (read(a->wake, &v, sizeof v) < 0) { /* nothing signalled */ }
        Job *j = job_pop(a);
        if (j) {
            if (j->kind == J_SYNC_ALL) acct_status(a, "sync", "");
            run_job(&im, j);
            job_free(j);
            if (im.c && conn_error(im.c)[0] && (strstr(conn_error(im.c), "closed") || strstr(conn_error(im.c), "timeout") || strstr(conn_error(im.c), "write"))) {
                conn_close(im.c); im.c = NULL;
                acct_status(a, "offline", "connection lost");
            }
            continue;
        }
        if (time(NULL) - last_full > 15 * 60) { Job *s = job_new(J_SYNC_ALL, 0, 1); if (s) job_push(a, s); last_full = time(NULL); continue; }
        int need_sync;
        if (idle_wait(&im, &need_sync)) { conn_close(im.c); im.c = NULL; continue; }
        if (need_sync) {
            pthread_mutex_lock(&G);
            Folder *in = store_folder(a, "INBOX", 0);
            pthread_mutex_unlock(&G);
            if (in && sync_folder(&im, in) < 0) { conn_close(im.c); im.c = NULL; }
        }
    }
    if (im.c) imap_close(&im);
    return NULL;
}

int imap_append_sent(Account *a, const char *msg, size_t len) {
    /* the sender hands the copy to the account's worker */
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"raw\":");
    jb_strn(&b, msg, len);
    jb_raw(&b, ",\"target\":\"sent\"}");
    if (b.oom) { jb_free(&b); return -1; }
    Job *j = job_new(J_APPEND_DRAFT, 0, 6);
    if (!j) { jb_free(&b); return -1; }
    j->arg = b.p;
    j->arglen = b.n;
    job_push(a, j);
    return 0;
}
