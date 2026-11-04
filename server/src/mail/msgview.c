/* msgview.c — opening a message for the reading pane (see maild.h): headers, the plain text,
 * the HTML turned into our safe tree (html.c), attachments, inline and remote images, and
 * calendar invitations. The last few opened messages stay parsed for their images. */
#include "maild.h"
#include "mime.h"
#include "html.h"
#include "http.h"
#include "ical.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define OPEN_CACHE 6
#define IMG_MAX_BYTES (5u << 20)      /* one remote image */
#define IMG_DEADLINE_MS 20000

typedef struct {
    char acct[40], folder[512];
    uint32_t uid;
    char *raw;
    size_t len;
    MimeMsg *mm;
    HtmlData data[HTML_MAX_DATA];
    int ndata;
    char *remote[HTML_MAX_REMOTE];
    int nremote;
    uint64_t used;
} Open;

static Open opened[OPEN_CACHE];
static uint64_t open_seq;
static pthread_mutex_t open_mu = PTHREAD_MUTEX_INITIALIZER;

static void open_free(Open *o) {
    free(o->raw);
    free(o->mm);
    for (int i = 0; i < o->ndata; i++) free((void *)o->data[i].p);
    for (int i = 0; i < o->nremote; i++) free(o->remote[i]);
    memset(o, 0, sizeof *o);
}

static Open *open_find(const char *acct, const char *folder, uint32_t uid) {
    for (int i = 0; i < OPEN_CACHE; i++)
        if (opened[i].raw && opened[i].uid == uid && !strcmp(opened[i].acct, acct) && !strcmp(opened[i].folder, folder)) { opened[i].used = ++open_seq; return &opened[i]; }
    return NULL;
}

static Open *open_slot(void) {
    int s = 0;
    for (int i = 0; i < OPEN_CACHE; i++) { if (!opened[i].raw) { s = i; break; } if (opened[i].used < opened[s].used) s = i; }
    open_free(&opened[s]);
    opened[s].used = ++open_seq;
    return &opened[s];
}

/* ---- text helpers ---- */

static int is_attachment(const MimePart *p) {
    if (!strcmp(p->type, "multipart")) return 0;
    if (!strcmp(p->type, "message") && !strcmp(p->subtype, "rfc822")) return 1;
    if (!strcmp(p->disposition, "attachment")) return 1;
    if (!strcmp(p->type, "text") && (!strcmp(p->subtype, "plain") || !strcmp(p->subtype, "html")) && !p->filename[0]) return 0;
    if (!strcmp(p->type, "text") && !strcmp(p->subtype, "calendar")) return 1;
    return p->filename[0] || strcmp(p->type, "text");
}

static void collapse(const char *in, char *out, size_t cap) {
    size_t o = 0;
    int sp = 0, line_start = 1, skip_line = 0;
    for (const char *s = in; *s && o + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\n') { line_start = 1; skip_line = 0; sp = o > 0; continue; }
        if (line_start && c == '>') { skip_line = 1; }       /* quoted text does not preview well */
        if (line_start && c != ' ' && c != '\t' && c != '\r') line_start = 0;
        if (skip_line) continue;
        if (c == ' ' || c == '\t' || c == '\r' || c < 0x20) { sp = o > 0; continue; }
        if (sp) { out[o++] = ' '; sp = 0; }
        out[o++] = (char)c;
    }
    out[o] = 0;
    /* do not cut a UTF-8 sequence */
    while (o && ((unsigned char)out[o - 1] & 0xc0) == 0x80) o--;
    if (o && (unsigned char)out[o - 1] >= 0xc0) o--;
    out[o] = 0;
}

void msg_preview(const char *hdr, size_t hlen, const char *part, size_t plen, char *out, size_t cap, int *att) {
    out[0] = 0;
    *att = 0;
    size_t n = hlen + plen + 4;
    char *m = malloc(n);
    if (!m) return;
    memcpy(m, hdr, hlen);
    size_t k = hlen;
    if (k < 2 || m[k - 1] != '\n') { m[k++] = '\r'; m[k++] = '\n'; }
    m[k++] = '\r'; m[k++] = '\n';
    if (k + plen > n) plen = n - k;
    memcpy(m + k, part, plen);
    k += plen;
    MimeMsg *mm = malloc(sizeof *mm);
    if (!mm) { free(m); return; }
    mime_parse(m, k, mm);
    const MimePart *best = NULL;
    for (int i = 0; i < mm->nparts; i++) {
        const MimePart *p = &mm->parts[i];
        if (!strcmp(p->type, "multipart") && !strcmp(p->subtype, "mixed")) *att = 1;
        if (is_attachment(p) && strcmp(p->type, "multipart")) *att = 1;
        if (!strcmp(p->type, "text") && !is_attachment(p)) {
            if (!best || (!strcmp(p->subtype, "plain") && strcmp(best->subtype, "plain"))) best = p;
        }
    }
    /* "multipart/mixed" with only text inside is not really an attachment */
    if (*att) {
        int real = 0;
        for (int i = 0; i < mm->nparts; i++) if (is_attachment(&mm->parts[i])) real = 1;
        if (!real && mm->nparts > 1) *att = mm->truncated;
    }
    if (best) {
        size_t tl;
        char *t = mime_part_text(best, &tl);
        if (t) {
            if (!strcmp(best->subtype, "html")) { size_t xl; char *x = html_to_text(t, tl, &xl); free(t); t = x; }
            if (t) collapse(t, out, cap < 240 ? cap : 240);
            free(t);
        }
    }
    free(mm);
    free(m);
}

char *msg_body_text(const char *raw, size_t len) {
    MimeMsg *mm = malloc(sizeof *mm);
    if (!mm) return NULL;
    mime_parse(raw, len, mm);
    JBuf b;
    jb_init(&b);
    const MimePart *html = NULL;
    int plain = 0;
    for (int i = 0; i < mm->nparts; i++) {
        const MimePart *p = &mm->parts[i];
        if (is_attachment(p)) { if (p->filename[0]) { jb_raw(&b, " "); jb_raw(&b, p->filename); } continue; }
        if (strcmp(p->type, "text")) continue;
        if (!strcmp(p->subtype, "html")) { if (!html) html = p; continue; }
        size_t tl;
        char *t = mime_part_text(p, &tl);
        if (t) { jb_raw(&b, " "); jb_raw(&b, t); free(t); plain = 1; }
    }
    if (!plain && html) {
        size_t tl;
        char *t = mime_part_text(html, &tl);
        if (t) { size_t xl; char *x = html_to_text(t, tl, &xl); free(t); if (x) { jb_raw(&b, " "); jb_raw(&b, x); free(x); } }
    }
    free(mm);
    if (b.oom) { jb_free(&b); return NULL; }
    if (!b.p) { b.p = calloc(1, 1); }
    return b.p;
}

/* ---- opening ---- */

static void addrs_json(JBuf *b, const char *key, const char *hdr, size_t hl, const char *name) {
    char *v = mime_header_dup(hdr, hl, name);
    jb_printf(b, ",\"%s\":[", key);
    if (v) {
        MimeAddr *ad = malloc(sizeof *ad * 100);
        int n = ad ? mime_addrs(v, ad, 100) : 0;
        for (int i = 0; i < n; i++) {
            if (i) jb_raw(b, ",");
            jb_raw(b, "{\"name\":"); jb_str(b, ad[i].name);
            jb_raw(b, ",\"email\":"); jb_str(b, ad[i].email);
            jb_raw(b, "}");
        }
        free(ad);
        free(v);
    }
    jb_raw(b, "]");
}

static void header_str(JBuf *b, const char *key, const char *hdr, size_t hl, const char *name, int decode) {
    char *v = mime_header_dup(hdr, hl, name);
    jb_printf(b, ",\"%s\":", key);
    if (!v) { jb_raw(b, "\"\""); return; }
    if (strlen(v) > 4000) v[4000] = 0;
    if (decode) { char d[4096]; mime_decode_words(v, d, sizeof d); jb_str(b, d); }
    else jb_str(b, v);
    free(v);
}

static int find_parts(const MimeMsg *mm, int *html, int *plain, int *cal) {
    *html = *plain = *cal = -1;
    for (int i = 0; i < mm->nparts; i++) {
        const MimePart *p = &mm->parts[i];
        if (!strcmp(p->type, "text") && !strcmp(p->subtype, "calendar") && *cal < 0) *cal = i;
        if ((!strcmp(p->type, "application") && (!strcmp(p->subtype, "ics") || !strcmp(p->subtype, "x-ics"))) && *cal < 0) *cal = i;
        if (is_attachment(p) || strcmp(p->type, "text")) continue;
        if (!strcmp(p->subtype, "html") && *html < 0) *html = i;
        if (!strcmp(p->subtype, "plain") && *plain < 0) *plain = i;
    }
    if (*cal < 0) for (int i = 0; i < mm->nparts; i++) {
        const char *fn = mm->parts[i].filename;
        size_t l = strlen(fn);
        if (l > 4 && !strcasecmp(fn + l - 4, ".ics")) { *cal = i; break; }
    }
    return 0;
}

void msg_open_cached(uint32_t id, Account *a, Folder *f, uint32_t uid) {
    char rel[800];
    pthread_mutex_lock(&G);
    store_body_path(a, f, uid, rel, sizeof rel);
    Msg *meta = store_msg(f, uid);
    uint32_t flags = meta ? meta->flags : 0;
    char fname[512];
    snprintf(fname, sizeof fname, "%s", f->name);
    pthread_mutex_unlock(&G);
    size_t len;
    char *raw = fs_read(rel, &len, (size_t)64 << 20);
    if (!raw) { reply_err(id, "not-cached", ""); return; }
    pthread_mutex_lock(&open_mu);
    Open *o = open_find(a->id, fname, uid);
    if (!o) {
        o = open_slot();
        snprintf(o->acct, sizeof o->acct, "%s", a->id);
        snprintf(o->folder, sizeof o->folder, "%s", fname);
        o->uid = uid;
        o->raw = raw;
        o->len = len;
        o->mm = malloc(sizeof *o->mm);
        if (o->mm) mime_parse(o->raw, o->len, o->mm);
    } else free(raw);
    if (!o->mm) { pthread_mutex_unlock(&open_mu); reply_err(id, "memory", ""); return; }
    const char *h, *bd;
    size_t hl, bl;
    mime_split(o->raw, o->len, &h, &hl, &bd, &bl);
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"acct\":"); jb_str(&b, a->id);
    jb_raw(&b, ",\"folder\":"); jb_str(&b, fname);
    jb_printf(&b, ",\"uid\":%u", uid);
    header_str(&b, "subject", h, hl, "Subject", 1);
    char dv[200];
    time_t dt = mime_header(h, hl, "Date", dv, sizeof dv) ? mime_date(dv) : 0;
    jb_printf(&b, ",\"date\":%lld", (long long)dt * 1000);
    addrs_json(&b, "from", h, hl, "From");
    addrs_json(&b, "to", h, hl, "To");
    addrs_json(&b, "cc", h, hl, "Cc");
    addrs_json(&b, "replyTo", h, hl, "Reply-To");
    header_str(&b, "msgid", h, hl, "Message-ID", 0);
    header_str(&b, "refs", h, hl, "References", 0);
    header_str(&b, "inReplyTo", h, hl, "In-Reply-To", 0);
    header_str(&b, "unsubscribe", h, hl, "List-Unsubscribe", 0);
    jb_printf(&b, ",\"seen\":%s,\"flagged\":%s,\"answered\":%s,\"size\":%zu", flags & F_SEEN ? "true" : "false", flags & F_FLAGGED ? "true" : "false",
              flags & F_ANSWERED ? "true" : "false", o->len);
    int hi, pi, ci;
    find_parts(o->mm, &hi, &pi, &ci);
    if (pi >= 0) {
        size_t tl;
        char *t = mime_part_text(&o->mm->parts[pi], &tl);
        if (t && tl > (2u << 20)) tl = 2u << 20;
        /* by length: a NUL byte in the body must not cut the text short (jb_strn writes it as U+FFFD) */
        jb_raw(&b, ",\"text\":"); jb_strn(&b, t ? t : "", t ? tl : 0);
        jb_printf(&b, ",\"flowed\":%s", strcasestr(o->mm->parts[pi].hdr, "format=flowed") && o->mm->parts[pi].hlen ? "true" : "false");
        free(t);
    }
    if (hi >= 0) {
        size_t tl;
        char *t = mime_part_text(&o->mm->parts[hi], &tl);
        if (t) {
            HtmlStats st;
            for (int i = 0; i < o->ndata; i++) free((void *)o->data[i].p);
            for (int i = 0; i < o->nremote; i++) free(o->remote[i]);
            o->ndata = o->nremote = 0;
            size_t jl;
            char *j = html_sanitize(t, tl, &jl, &st, o->data, &o->ndata);
            if (j) {
                jb_raw(&b, ",\"html\":");
                jb_raw(&b, j);
                /* remember the remote URLs for the image requests */
                const char *r = strstr(j, ",\"remote\":[");
                if (r) {
                    r += 11;
                    while (*r == '"' && o->nremote < HTML_MAX_REMOTE) {
                        const char *e = r + 1;
                        while (*e && *e != '"') { if (*e == '\\' && e[1]) e++; e++; }
                        size_t ul = (size_t)(e - r - 1);
                        char *u = malloc(ul + 1);
                        if (u) {
                            size_t k = 0;
                            for (const char *x = r + 1; x < e; x++) { if (*x == '\\' && x + 1 < e) { x++; u[k++] = *x == '/' ? '/' : *x; continue; } u[k++] = *x; }
                            u[k] = 0;
                            o->remote[o->nremote++] = u;
                        }
                        r = *e ? e + 1 : e;
                        if (*r == ',') r++;
                    }
                }
                free(j);
            }
            if (pi < 0) {
                size_t xl;
                char *x = html_to_text(t, tl, &xl);
                jb_raw(&b, ",\"text\":"); jb_strn(&b, x ? x : "", x ? xl : 0);
                free(x);
            }
            free(t);
        }
    }
    jb_raw(&b, ",\"atts\":[");
    int first = 1;
    for (int i = 0; i < o->mm->nparts; i++) {
        const MimePart *p = &o->mm->parts[i];
        if (!is_attachment(p)) continue;
        if (!first) jb_raw(&b, ",");
        first = 0;
        char type[120];
        snprintf(type, sizeof type, "%s/%s", p->type, p->subtype);
        size_t est = !strcmp(p->encoding, "base64") ? p->blen * 3 / 4 : p->blen;
        jb_printf(&b, "{\"idx\":%d,\"name\":", i);
        if (p->filename[0]) jb_str(&b, p->filename);
        else if (!strcmp(p->type, "message")) jb_str(&b, "message.eml");
        else if (!strcmp(p->subtype, "calendar")) jb_str(&b, "invite.ics");
        else { char nm[64]; snprintf(nm, sizeof nm, "part-%d.%s", i, p->subtype); jb_str(&b, nm); }
        jb_raw(&b, ",\"type\":"); jb_str(&b, type);
        jb_printf(&b, ",\"size\":%zu", est);
        if (p->cid[0]) { jb_raw(&b, ",\"cid\":"); jb_str(&b, p->cid); }
        jb_printf(&b, ",\"inline\":%s}", !strcmp(p->disposition, "inline") || (p->cid[0] && strcmp(p->disposition, "attachment")) ? "true" : "false");
    }
    jb_raw(&b, "]");
    if (ci >= 0) {
        jb_printf(&b, ",\"invite\":{\"idx\":%d,\"method\":", ci);
        jb_str(&b, o->mm->parts[ci].method);
        jb_raw(&b, "}");
    }
    jb_raw(&b, "}");
    pthread_mutex_unlock(&open_mu);
    reply_done(id, &b);
}

void msg_open_json(uint32_t id, const JVal *args) {
    Account *a = acct_get(json_str(args, "acct"));
    const char *folder = json_str(args, "folder");
    double u = json_num(args, "uid", 0);
    if (!a || !folder || u < 1 || u > 4294967295.0) { reply_err(id, "bad-args", ""); return; }
    uint32_t uid = (uint32_t)u;
    pthread_mutex_lock(&G);
    Folder *f = store_folder(a, folder, 0);
    Msg *m = f ? store_msg(f, uid) : NULL;
    int cached = m && m->body;
    pthread_mutex_unlock(&G);
    if (!m) { reply_err(id, "not-found", ""); return; }
    if (cached) { msg_open_cached(id, a, f, uid); return; }
    Job *j = job_new(J_FETCH_BODY, id, 9);
    if (!j) { reply_err(id, "memory", ""); return; }
    snprintf(j->folder, sizeof j->folder, "%s", folder);
    j->uid = uid;
    job_push(a, j);
}

/* The parsed message for a follow-up request (attachments, images, invitation). open_mu held. */
static Open *open_for(uint32_t id, const JVal *args) {
    const char *acct = json_str(args, "acct"), *folder = json_str(args, "folder");
    double u = json_num(args, "uid", 0);
    if (!acct || !folder || u < 1 || u > 4294967295.0) { reply_err(id, "bad-args", ""); return NULL; }
    Open *o = open_find(acct, folder, (uint32_t)u);
    if (!o) {
        /* not among the last opened: load it again from the cache */
        Account *a = acct_get(acct);
        pthread_mutex_lock(&G);
        Folder *f = a ? store_folder(a, folder, 0) : NULL;
        char rel[800];
        if (f) store_body_path(a, f, (uint32_t)u, rel, sizeof rel);
        pthread_mutex_unlock(&G);
        if (!f) { reply_err(id, "not-found", ""); return NULL; }
        size_t len;
        char *raw = fs_read(rel, &len, (size_t)64 << 20);
        if (!raw) { reply_err(id, "not-cached", ""); return NULL; }
        o = open_slot();
        snprintf(o->acct, sizeof o->acct, "%s", acct);
        snprintf(o->folder, sizeof o->folder, "%s", folder);
        o->uid = (uint32_t)u;
        o->raw = raw;
        o->len = len;
        o->mm = malloc(sizeof *o->mm);
        if (!o->mm) { reply_err(id, "memory", ""); return NULL; }
        mime_parse(o->raw, o->len, o->mm);
    }
    return o;
}

void msg_attachment(uint32_t id, const JVal *args) {
    pthread_mutex_lock(&open_mu);
    Open *o = open_for(id, args);
    if (!o) { pthread_mutex_unlock(&open_mu); return; }
    int idx = (int)json_num(args, "idx", -1);
    if (idx < 0 || idx >= o->mm->nparts) { pthread_mutex_unlock(&open_mu); reply_err(id, "bad-index", ""); return; }
    const MimePart *p = &o->mm->parts[idx];
    size_t n = 0;
    char *data = mime_decode_body(p, &n);
    char name[256];
    fs_safe_name(p->filename[0] ? p->filename : !strcmp(p->type, "message") ? "message.eml" : !strcmp(p->subtype, "calendar") ? "invite.ics" : "attachment", name, sizeof name);
    char rel[900];
    snprintf(rel, sizeof rel, "att/%s/%u-%d/%s", o->acct, o->uid, idx, name);
    pthread_mutex_unlock(&open_mu);
    if (!data) { reply_err(id, "memory", ""); return; }
    char dir[900];
    snprintf(dir, sizeof dir, "%s", rel);
    *strrchr(dir, '/') = 0;
    fs_mkdirs(dir);
    int ok = fs_write(rel, data, n) == 0;
    free(data);
    if (!ok) { reply_err(id, "write", rel); return; }
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"path\":"); jb_str(&b, rel);
    jb_raw(&b, ",\"name\":"); jb_str(&b, name);
    jb_printf(&b, ",\"size\":%zu}", n);
    reply_done(id, &b);
}

/* ---- remote images ---- */

int https_get(const char *url, char **body, size_t *blen, size_t max, char *err, size_t ecap) {
    char cur[2048];
    snprintf(cur, sizeof cur, "%s", url);
    for (int hop = 0; hop < 4; hop++) {                 /* at most 3 redirects */
        int tls, port;
        char host[256], path[1800];
        if (http_split_url(cur, &tls, host, sizeof host, &port, path, sizeof path)) { snprintf(err, ecap, "url"); return -1; }
        if (port != 80 && port != 443) { snprintf(err, ecap, "port"); return -1; }
        char why[32];
        int fd = net_connect(host, port, why, sizeof why);      /* lord-server: granted host, public address only */
        if (fd < 0) { snprintf(err, ecap, "%s", why); return -1; }
        Conn *c = conn_new(fd, host, 10000);
        if (!c) { snprintf(err, ecap, "memory"); return -1; }
        TlsInfo ti;
        if (tls && conn_tls(c, NULL, &ti)) { snprintf(err, ecap, "%s", ti.code ? "cert" : "tls"); conn_close(c); return -1; }
        HttpResp r;
        int rc = http_request(c, "GET", host, path, "Accept: image/png,image/jpeg,image/gif,image/bmp\r\nConnection: close\r\n", NULL, 0, max, IMG_DEADLINE_MS, &r);
        conn_close(c);
        if (rc) { snprintf(err, ecap, rc == -2 ? "too-big" : "http"); return -1; }
        if (r.status >= 300 && r.status < 400 && r.location[0]) {
            if (!strncasecmp(r.location, "http://", 7) || !strncasecmp(r.location, "https://", 8)) snprintf(cur, sizeof cur, "%s", r.location);
            else if (r.location[0] == '/') snprintf(cur, sizeof cur, "%s://%s%s", tls ? "https" : "http", host, r.location);
            else { http_resp_free(&r); snprintf(err, ecap, "redirect"); return -1; }
            http_resp_free(&r);
            continue;
        }
        if (r.status != 200) { http_resp_free(&r); snprintf(err, ecap, "status %d", r.status); return -1; }
        *body = r.body;
        *blen = r.blen;
        r.body = NULL;
        return 0;
    }
    snprintf(err, ecap, "redirects");
    return -1;
}

void msg_image(uint32_t id, const JVal *args) {
    const char *src = json_str(args, "src");
    int maxw = (int)json_num(args, "maxw", 1200);
    if (maxw < 16 || maxw > 4096) maxw = 1200;
    if (!src) { reply_err(id, "bad-args", ""); return; }
    unsigned char *bytes = NULL;
    size_t n = 0;
    char url[2048] = "";
    pthread_mutex_lock(&open_mu);
    Open *o = open_for(id, args);
    if (!o) { pthread_mutex_unlock(&open_mu); return; }
    if (!strncasecmp(src, "cid:", 4)) {
        for (int i = 0; i < o->mm->nparts; i++) {
            const MimePart *p = &o->mm->parts[i];
            if (p->cid[0] && !strcasecmp(p->cid, src + 4)) { bytes = (unsigned char *)mime_decode_body(p, &n); break; }
        }
    } else if (!strncmp(src, "d:", 2)) {
        int k = atoi(src + 2);
        if (k >= 0 && k < o->ndata && o->data[k].p) { bytes = malloc(o->data[k].n * 3 / 4 + 4); if (bytes) n = b64_decode(o->data[k].p, o->data[k].n, bytes); }
    } else if (!strncmp(src, "r:", 2)) {
        int k = atoi(src + 2);
        if (k >= 0 && k < o->nremote) snprintf(url, sizeof url, "%s", o->remote[k]);
    }
    pthread_mutex_unlock(&open_mu);
    if (url[0]) {
        /* remote: our cache first, then the network (only after the user chose to load images) */
        char h[17], rel[64];
        fs_hash(url, h);
        snprintf(rel, sizeof rel, "img/%s", h);
        bytes = (unsigned char *)fs_read(rel, &n, IMG_MAX_BYTES);
        if (!bytes) {
            char err[64];
            char *body;
            if (https_get(url, &body, &n, IMG_MAX_BYTES, err, sizeof err)) { reply_err(id, "fetch", err); return; }
            bytes = (unsigned char *)body;
            fs_mkdirs("img");
            fs_write(rel, bytes, n);
        }
    }
    if (!bytes || !n) { free(bytes); reply_err(id, "no-image", src); return; }
    int w, hh;
    uint32_t *px = img_decode(bytes, n, maxw, 4096, &w, &hh);
    free(bytes);
    if (!px) { reply_err(id, "decode", ""); return; }
    reply_image(id, px, w, hh);
    free(px);
}

/* ---- invitations ---- */

static char *invite_text(uint32_t id, const JVal *args, size_t *len) {
    pthread_mutex_lock(&open_mu);
    Open *o = open_for(id, args);
    if (!o) { pthread_mutex_unlock(&open_mu); return NULL; }
    int hi, pi, ci;
    find_parts(o->mm, &hi, &pi, &ci);
    char *t = ci >= 0 ? mime_part_text(&o->mm->parts[ci], len) : NULL;
    pthread_mutex_unlock(&open_mu);
    if (!t) reply_err(id, "no-invite", "");
    return t;
}

void msg_invite(uint32_t id, const JVal *args) {
    size_t len;
    char *t = invite_text(id, args, &len);
    if (!t) return;
    IcEvent *ev;
    char method[24];
    int n = ical_events(t, len, &ev, method, sizeof method);
    free(t);
    if (n <= 0) { reply_err(id, "bad-invite", ""); return; }
    Account *a = acct_get(json_str(args, "acct"));
    const char *me = a ? a->email : "";
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"method\":"); jb_str(&b, method);
    jb_raw(&b, ",\"event\":");
    ical_event_json(&b, &ev[0], 0);
    char cal[64] = "", ps[24] = "";
    pthread_mutex_lock(&G);
    int have = pim_has_event(ev[0].uid, cal, sizeof cal, ps, sizeof ps, me);
    pthread_mutex_unlock(&G);
    jb_printf(&b, ",\"inCalendar\":%s", have ? "true" : "false");
    jb_raw(&b, ",\"calendar\":"); jb_str(&b, cal);
    jb_raw(&b, ",\"myStatus\":"); jb_str(&b, ps);
    /* my partstat in the invitation itself */
    for (int i = 0; i < ev[0].natt; i++) if (!strcasecmp(ev[0].att[i].email, me)) { jb_raw(&b, ",\"invited\":"); jb_str(&b, ev[0].att[i].partstat); }
    jb_raw(&b, "}");
    ical_events_free(ev, n);
    reply_done(id, &b);
}

/* Accept / decline / maybe: put it in the calendar with my answer and send the REPLY to the
 * organizer (iTIP over iMIP, RFC 6047). */
void msg_invite_respond(uint32_t id, const JVal *args) {
    const char *ps = json_str(args, "partstat");
    if (!ps || (strcmp(ps, "ACCEPTED") && strcmp(ps, "DECLINED") && strcmp(ps, "TENTATIVE"))) { reply_err(id, "bad-partstat", ""); return; }
    Account *a = acct_get(json_str(args, "acct"));
    if (!a) { reply_err(id, "no-account", ""); return; }
    size_t len;
    char *t = invite_text(id, args, &len);
    if (!t) return;
    IcEvent *ev;
    char method[24];
    int n = ical_events(t, len, &ev, method, sizeof method);
    if (n <= 0) { free(t); reply_err(id, "bad-invite", ""); return; }
    char err[200] = "";
    const char *cal = json_str(args, "calendar");
    int ok = 0;
    if (strcmp(ps, "DECLINED") || json_bool(args, "keepDeclined", 0)) {
        pthread_mutex_lock(&G);
        ok = pim_add_invite(cal ? cal : "", t, len, ps, a->email, err, sizeof err) == 0;
        pthread_mutex_unlock(&G);
    } else ok = 1;
    free(t);
    if (!ok) { ical_events_free(ev, n); reply_err(id, "calendar", err); return; }
    int sent = 0;
    if (json_bool(args, "reply", 1) && ev[0].organizer[0] && strcasecmp(ev[0].organizer, a->email)) {
        size_t rl;
        char *ics = ical_reply(&ev[0], a->email, a->full_name, ps, &rl);
        if (ics) {
            /* the reply goes out as a normal message through the sender */
            JBuf m;
            jb_init(&m);
            const char *verb = !strcmp(ps, "ACCEPTED") ? "Accepted" : !strcmp(ps, "DECLINED") ? "Declined" : "Tentative";
            char subj[700];
            snprintf(subj, sizeof subj, "%s: %s", verb, ev[0].summary);
            jb_raw(&m, "{\"op\":\"send\",\"acct\":"); jb_str(&m, a->id);
            jb_raw(&m, ",\"to\":[{\"name\":"); jb_str(&m, ev[0].organizer_name);
            jb_raw(&m, ",\"email\":"); jb_str(&m, ev[0].organizer);
            jb_raw(&m, "}],\"subject\":"); jb_str(&m, subj);
            jb_raw(&m, ",\"text\":"); jb_str(&m, subj);
            jb_raw(&m, ",\"calendar\":"); jb_strn(&m, ics, rl);
            jb_raw(&m, ",\"calendarMethod\":\"REPLY\",\"noSent\":true}");
            free(ics);
            Job *j = job_new(J_SEND, 0, 9);
            if (j && !m.oom) { j->arg = m.p; j->arglen = m.n; snprintf(j->folder, sizeof j->folder, "%s", a->id); sender_queue(j); sent = 1; }
            else { jb_free(&m); job_free(j); }
        }
    }
    ical_events_free(ev, n);
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"ok\":true,\"replied\":%s}", sent ? "true" : "false");
    reply_done(id, &b);
}
