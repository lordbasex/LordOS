/* store.c — the local mail cache: folders, message index, conversations and the lists the UI
 * shows (see maild.h). Everything is in memory and mirrored to small text files, so a folder
 * with 10,000 messages opens instantly and offline. Callers hold G. */
#include "maild.h"
#include "mime.h"
#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

typedef struct { char id[40]; FolderList fl; int idx; } Store;
static Store stores[MAIL_MAX_ACCOUNTS * 2];
static int nstores;

static int acct_index(const Account *a) { for (int i = 0; i < naccounts; i++) if (accounts[i] == a) return i; return -1; }

static Store *store_of(const Account *a, int create) {
    for (int i = 0; i < nstores; i++) if (!strcmp(stores[i].id, a->id)) { stores[i].idx = acct_index(a); return &stores[i]; }
    if (!create || nstores >= (int)(sizeof stores / sizeof stores[0])) return NULL;
    Store *s = &stores[nstores++];
    memset(s, 0, sizeof *s);
    snprintf(s->id, sizeof s->id, "%s", a->id);
    s->idx = acct_index(a);
    return s;
}

FolderList *store_folders(Account *a) { Store *s = store_of(a, 1); return s ? &s->fl : NULL; }

/* ---- escaping for the index files ---- */

static void put_field(JBuf *b, const char *s) {
    for (; s && *s; s++) {
        if (*s == '\t') jb_raw(b, "\\t");
        else if (*s == '\n') jb_raw(b, "\\n");
        else if (*s == '\r') continue;
        else if (*s == '\\') jb_raw(b, "\\\\");
        else { char c[2] = { *s, 0 }; jb_raw(b, c); }
    }
}

/* One field of a line (the line already NUL-terminated): unescapes in place and moves *p past
 * the tab. Writing the terminator over the tab is fine: lines are split before parsing. */
static char *get_field(char **p) {
    char *s = *p, *o = s, *r = s;
    while (*s && *s != '\t') {
        if (*s == '\\' && s[1]) { s++; *o++ = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s; s++; continue; }
        *o++ = *s++;
    }
    int tab = *s == '\t';
    *o = 0;
    *p = tab ? s + 1 : s;
    return r;
}

static char *sdup(const char *s) { return strdup(s ? s : ""); }

static void msg_free(Msg *m) {
    free(m->from_name); free(m->from_email); free(m->to); free(m->cc); free(m->subject);
    free(m->msgid); free(m->inreplyto); free(m->refs); free(m->preview); free(m->text);
}

/* ---- folders ---- */

static void folder_path(Account *a, Folder *f, const char *file, char *out, size_t cap) { snprintf(out, cap, "%s/f/%s/%s", a->id, f->dir, file); }

Folder *store_folder(Account *a, const char *name, int create) {
    FolderList *fl = store_folders(a);
    if (!fl || !name) return NULL;
    for (int i = 0; i < fl->n; i++) if (!strcmp(fl->f[i]->name, name)) return fl->f[i];
    if (!create) return NULL;
    Folder **nf = realloc(fl->f, (size_t)(fl->n + 1) * sizeof *nf);
    if (!nf) return NULL;
    fl->f = nf;
    Folder *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    snprintf(f->name, sizeof f->name, "%s", name);
    snprintf(f->display, sizeof f->display, "%s", name);
    fs_hash(name, f->dir);
    fl->f[fl->n++] = f;
    return f;
}

Msg *store_msg(Folder *f, uint32_t uid) {
    /* messages are kept sorted by uid */
    int lo = 0, hi = f->n;
    while (lo < hi) { int mid = (lo + hi) / 2; if (f->msgs[mid].uid < uid) lo = mid + 1; else hi = mid; }
    return lo < f->n && f->msgs[lo].uid == uid ? &f->msgs[lo] : NULL;
}

void store_msg_text(Msg *m, const char *body) {
    size_t n = strlen(m->subject ? m->subject : "") + strlen(m->from_name ? m->from_name : "") + strlen(m->from_email ? m->from_email : "") +
               strlen(m->to ? m->to : "") + strlen(m->cc ? m->cc : "") + strlen(m->preview ? m->preview : "") + (body ? strlen(body) : 0) + 16;
    if (n > 24000) n = 24000;
    char *raw = malloc(n);
    if (!raw) return;
    snprintf(raw, n, "%s %s %s %s %s %s %s", m->subject ? m->subject : "", m->from_name ? m->from_name : "", m->from_email ? m->from_email : "",
             m->to ? m->to : "", m->cc ? m->cc : "", m->preview ? m->preview : "", body ? body : "");
    char *fold = malloc(n + 8);
    if (fold) search_fold(raw, fold, n + 8);
    free(raw);
    free(m->text);
    m->text = fold;
}

static void index_msg(Account *a, Folder *f, Msg *m) {
    if (m->doc > 0) search_remove(m->doc - 1);
    int d = search_add(m->text ? m->text : "", acct_index(a), f, m->uid);
    m->doc = d + 1;
}

void store_reindex(Account *a, Folder *f, Msg *m) { index_msg(a, f, m); }

Msg *store_add(Account *a, Folder *f, const Msg *src) {
    Msg *ex = store_msg(f, src->uid);
    if (ex) {
        uint32_t old = ex->flags;
        ex->flags = src->flags;
        if ((old ^ ex->flags) & F_SEEN) f->unread += (ex->flags & F_SEEN) ? -1 : 1;
        f->dirty = 1; f->version++;
        return ex;
    }
    if (f->n == f->cap) {
        int nc = f->cap ? f->cap * 2 : 64;
        Msg *nm = realloc(f->msgs, (size_t)nc * sizeof *nm);
        if (!nm) return NULL;
        f->msgs = nm;
        f->cap = nc;
    }
    int pos = f->n;
    while (pos > 0 && f->msgs[pos - 1].uid > src->uid) pos--;
    memmove(&f->msgs[pos + 1], &f->msgs[pos], (size_t)(f->n - pos) * sizeof *f->msgs);
    Msg *m = &f->msgs[pos];
    *m = *src;
    m->from_name = sdup(src->from_name); m->from_email = sdup(src->from_email); m->to = sdup(src->to); m->cc = sdup(src->cc);
    m->subject = sdup(src->subject); m->msgid = sdup(src->msgid); m->inreplyto = sdup(src->inreplyto); m->refs = sdup(src->refs);
    m->preview = sdup(src->preview);
    m->text = NULL;
    m->doc = 0;
    f->n++;
    if (!(m->flags & F_SEEN)) f->unread++;
    store_msg_text(m, NULL);
    /* search docs point at (folder, uid): re-adding is enough */
    index_msg(a, f, m);
    f->dirty = 1;
    f->version++;
    return m;
}

void store_remove(Account *a, Folder *f, uint32_t uid) {
    Msg *m = store_msg(f, uid);
    if (!m) return;
    if (!(m->flags & F_SEEN)) f->unread--;
    if (m->doc > 0) search_remove(m->doc - 1);
    char rel[700];
    char name[32];
    snprintf(name, sizeof name, "m/%u", uid);
    folder_path(a, f, name, rel, sizeof rel);
    fs_unlink(rel);
    msg_free(m);
    int pos = (int)(m - f->msgs);
    memmove(&f->msgs[pos], &f->msgs[pos + 1], (size_t)(f->n - pos - 1) * sizeof *f->msgs);
    f->n--;
    f->dirty = 1;
    f->version++;
}

void store_clear(Account *a, Folder *f) {
    for (int i = 0; i < f->n; i++) { if (f->msgs[i].doc > 0) search_remove(f->msgs[i].doc - 1); msg_free(&f->msgs[i]); }
    f->n = 0;
    f->unread = 0;
    char rel[700];
    folder_path(a, f, "m", rel, sizeof rel);
    fs_rmtree(rel);
    f->uidnext = 0;
    f->modseq = 0;
    f->dirty = 1;
    f->version++;
}

char *store_body_path(Account *a, Folder *f, uint32_t uid, char *out, size_t cap) {
    char name[32];
    snprintf(name, sizeof name, "m/%u", uid);
    folder_path(a, f, name, out, cap);
    return out;
}

void store_save(Account *a, Folder *f) {
    if (!f->dirty) return;
    JBuf b;
    jb_init(&b);
    for (int i = 0; i < f->n; i++) {
        Msg *m = &f->msgs[i];
        jb_printf(&b, "%u\t%u\t%lld\t%u\t%u\t%u\t", m->uid, m->flags, (long long)m->date, m->size, m->att, m->body);
        put_field(&b, m->from_name); jb_raw(&b, "\t");
        put_field(&b, m->from_email); jb_raw(&b, "\t");
        put_field(&b, m->to); jb_raw(&b, "\t");
        put_field(&b, m->cc); jb_raw(&b, "\t");
        put_field(&b, m->subject); jb_raw(&b, "\t");
        put_field(&b, m->msgid); jb_raw(&b, "\t");
        put_field(&b, m->inreplyto); jb_raw(&b, "\t");
        put_field(&b, m->refs); jb_raw(&b, "\t");
        put_field(&b, m->preview); jb_raw(&b, "\n");
    }
    char rel[700];
    folder_path(a, f, "index", rel, sizeof rel);
    if (!b.oom) fs_write(rel, b.p ? b.p : "", b.n);
    jb_free(&b);
    char st[96];
    snprintf(st, sizeof st, "%u %u %llu\n", f->uidvalidity, f->uidnext, (unsigned long long)f->modseq);
    folder_path(a, f, "state", rel, sizeof rel);
    fs_write(rel, st, strlen(st));
    f->dirty = 0;
}

void store_save_folders(Account *a) {
    FolderList *fl = store_folders(a);
    if (!fl) return;
    JBuf b;
    jb_init(&b);
    for (int i = 0; i < fl->n; i++) {
        Folder *f = fl->f[i];
        put_field(&b, f->name); jb_raw(&b, "\t");
        put_field(&b, f->display); jb_raw(&b, "\t");
        put_field(&b, f->special);
        jb_printf(&b, "\t%d\t%d\n", (int)(unsigned char)f->delim, f->noselect);
    }
    char rel[100];
    snprintf(rel, sizeof rel, "%s/folders", a->id);
    if (!b.oom) fs_write(rel, b.p ? b.p : "", b.n);
    jb_free(&b);
}

static void load_folder(Account *a, Folder *f) {
    char rel[700];
    size_t n;
    folder_path(a, f, "state", rel, sizeof rel);
    char *st = fs_read(rel, &n, 256);
    if (st) {
        unsigned v = 0, nx = 0;
        unsigned long long ms = 0;
        if (sscanf(st, "%u %u %llu", &v, &nx, &ms) >= 2) { f->uidvalidity = v; f->uidnext = nx; f->modseq = ms; }
        free(st);
    }
    folder_path(a, f, "index", rel, sizeof rel);
    char *ix = fs_read(rel, &n, (size_t)512 << 20);
    if (!ix) return;
    for (char *line = ix; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *p = line;
        line = nl ? nl + 1 : NULL;
        Msg m;
        memset(&m, 0, sizeof m);
        unsigned long long date;
        unsigned uid, flags, size, att, body;
        if (sscanf(p, "%u\t%u\t%llu\t%u\t%u\t%u\t", &uid, &flags, &date, &size, &att, &body) != 6) continue;
        int bad = 0;
        for (int k = 0; k < 6; k++) { char *t = strchr(p, '\t'); if (!t) { bad = 1; break; } p = t + 1; }
        if (bad) continue;
        m.uid = uid; m.flags = flags; m.date = (int64_t)date; m.size = size; m.att = (uint16_t)att; m.body = (uint16_t)body;
        m.from_name = get_field(&p); m.from_email = get_field(&p); m.to = get_field(&p); m.cc = get_field(&p);
        m.subject = get_field(&p); m.msgid = get_field(&p); m.inreplyto = get_field(&p); m.refs = get_field(&p); m.preview = get_field(&p);
        if (uid) store_add(a, f, &m);
    }
    free(ix);
    /* the body text of downloaded messages goes into the search index too */
    for (int i = 0; i < f->n; i++) {
        Msg *m = &f->msgs[i];
        if (!m->body) continue;
        char tr[700], name[32];
        snprintf(name, sizeof name, "t/%u", m->uid);
        folder_path(a, f, name, tr, sizeof tr);
        size_t tl;
        char *t = fs_read(tr, &tl, 64 * 1024);
        if (t) { store_msg_text(m, t); index_msg(a, f, m); free(t); }
    }
    f->dirty = 0;
}

void store_load(Account *a) {
    char rel[100];
    snprintf(rel, sizeof rel, "%s/folders", a->id);
    size_t n;
    char *list = fs_read(rel, &n, 4 << 20);
    if (!list) return;
    for (char *line = list; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *p = line;
        line = nl ? nl + 1 : NULL;
        char *name = get_field(&p), *disp = get_field(&p), *sp = get_field(&p);
        int delim = 0, nosel = 0;
        sscanf(p, "%d\t%d", &delim, &nosel);
        if (!*name) continue;
        Folder *f = store_folder(a, name, 1);
        if (!f) continue;
        snprintf(f->display, sizeof f->display, "%s", disp);
        snprintf(f->special, sizeof f->special, "%s", sp);
        f->delim = (char)delim;
        f->noselect = nosel;
        load_folder(a, f);
    }
    free(list);
}

void store_drop_account(const char *id) {
    for (int i = 0; i < nstores; i++) {
        if (strcmp(stores[i].id, id)) continue;
        FolderList *fl = &stores[i].fl;
        for (int k = 0; k < fl->n; k++) {
            Folder *f = fl->f[k];
            for (int m = 0; m < f->n; m++) { if (f->msgs[m].doc > 0) search_remove(f->msgs[m].doc - 1); msg_free(&f->msgs[m]); }
            free(f->msgs);
            free(f);
        }
        free(fl->f);
        char rel[64];
        snprintf(rel, sizeof rel, "%s", id);
        fs_rmtree(rel);
        stores[i] = stores[--nstores];
        return;
    }
}

/* ---- JSON ---- */

static int special_rank(const char *sp) {
    static const char *order[] = { "inbox", "drafts", "sent", "archive", "junk", "trash", "all", "flagged" };
    for (int i = 0; i < 8; i++) if (!strcmp(sp, order[i])) return i;
    return 20;
}

static int cmp_folder(const void *x, const void *y) {
    const Folder *a = *(Folder *const *)x, *b = *(Folder *const *)y;
    int ra = special_rank(a->special), rb = special_rank(b->special);
    if (ra != rb) return ra - rb;
    return strcasecmp(a->display, b->display);
}

void store_folders_json(JBuf *b) {
    jb_raw(b, "{\"accounts\":[");
    int first = 1;
    for (int i = 0; i < naccounts; i++) {
        Account *a = accounts[i];
        if (!a || a->stop || strcmp(a->kind, "mail")) continue;
        FolderList *fl = store_folders(a);
        if (!first) jb_raw(b, ",");
        first = 0;
        jb_raw(b, "{\"id\":"); jb_str(b, a->id);
        jb_raw(b, ",\"name\":"); jb_str(b, a->name[0] ? a->name : a->email);
        jb_raw(b, ",\"email\":"); jb_str(b, a->email);
        jb_raw(b, ",\"status\":"); jb_str(b, a->status);
        jb_raw(b, ",\"folders\":[");
        Folder **sorted = fl && fl->n ? malloc((size_t)fl->n * sizeof *sorted) : NULL;
        int n = 0;
        if (sorted) { memcpy(sorted, fl->f, (size_t)fl->n * sizeof *sorted); n = fl->n; qsort(sorted, (size_t)n, sizeof *sorted, cmp_folder); }
        for (int k = 0; k < n; k++) {
            Folder *f = sorted[k];
            if (k) jb_raw(b, ",");
            jb_raw(b, "{\"name\":"); jb_str(b, f->name);
            jb_raw(b, ",\"display\":"); jb_str(b, f->display);
            jb_raw(b, ",\"special\":"); jb_str(b, f->special);
            char d[2] = { f->delim ? f->delim : '/', 0 };
            jb_raw(b, ",\"delim\":"); jb_str(b, d);
            jb_printf(b, ",\"total\":%d,\"unread\":%d,\"noselect\":%s}", f->n, f->unread < 0 ? 0 : f->unread, f->noselect ? "true" : "false");
        }
        free(sorted);
        jb_raw(b, "]}");
    }
    jb_raw(b, "]}");
}

void store_counts_json(JBuf *b) {
    int total = 0;
    jb_raw(b, "{\"inbox\":[");
    int first = 1;
    for (int i = 0; i < naccounts; i++) {
        Account *a = accounts[i];
        if (!a || a->stop || strcmp(a->kind, "mail")) continue;
        Folder *f = store_folder(a, "INBOX", 0);
        int u = f && f->unread > 0 ? f->unread : 0;
        total += u;
        if (!first) jb_raw(b, ",");
        first = 0;
        jb_raw(b, "{\"acct\":"); jb_str(b, a->id);
        jb_printf(b, ",\"unread\":%d}", u);
    }
    jb_printf(b, "],\"unread\":%d}", total);
}

/* ---- lists and conversations ---- */

typedef struct { int acct; Folder *f; Msg *m; uint64_t key; } Ent;
typedef struct { int first; int n, unread, flagged, att; uint64_t key; } Grp;

static uint64_t h64(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

/* conversation key: the first Message-ID of References (the root), else In-Reply-To, else
 * the message's own id; replies without headers fall back to the subject without "Re:" */
static uint64_t thread_key(const Msg *m) {
    const char *r = m->refs && *m->refs ? m->refs : m->inreplyto && *m->inreplyto ? m->inreplyto : NULL;
    if (r) {
        const char *a = strchr(r, '<');
        const char *e = a ? strchr(a, '>') : NULL;
        if (a && e) return h64(a + 1, (size_t)(e - a - 1));
    }
    char base[512];
    mime_base_subject(m->subject, base, sizeof base);
    if (m->subject && strcmp(base, m->subject) && base[0]) {
        char low[512];
        search_fold(base, low, sizeof low);
        uint64_t k = h64(low, strlen(low));
        return k ^ 0x5bd1e995ull;
    }
    if (m->msgid && *m->msgid) {
        const char *a = strchr(m->msgid, '<');
        const char *e = a ? strchr(a, '>') : NULL;
        if (a && e) return h64(a + 1, (size_t)(e - a - 1));
        return h64(m->msgid, strlen(m->msgid));
    }
    return h64((const char *)&m, sizeof m);          /* unique: alone */
}

/* the subject-only key of an original message, so "Re: X" without headers joins "X" */
static uint64_t subject_key(const Msg *m) {
    char base[512], low[512];
    mime_base_subject(m->subject, base, sizeof base);
    search_fold(base, low, sizeof low);
    return h64(low, strlen(low)) ^ 0x5bd1e995ull;
}

static int cmp_ent(const void *x, const void *y) {
    const Ent *a = x, *b = y;
    if (a->m->date != b->m->date) return a->m->date > b->m->date ? -1 : 1;
    return a->m->uid > b->m->uid ? -1 : a->m->uid < b->m->uid ? 1 : 0;
}

static void flags_json(JBuf *b, uint32_t f) {
    jb_printf(b, ",\"seen\":%s,\"flagged\":%s,\"answered\":%s,\"forwarded\":%s,\"draft\":%s", f & F_SEEN ? "true" : "false",
              f & F_FLAGGED ? "true" : "false", f & F_ANSWERED ? "true" : "false", f & F_FORWARDED ? "true" : "false", f & F_DRAFT ? "true" : "false");
    jb_raw(b, ",\"labels\":[");
    int first = 1;
    for (int i = 0; i < 6; i++) if (f & (F_LABEL1 << i)) { jb_printf(b, "%s%d", first ? "" : ",", i + 1); first = 0; }
    jb_raw(b, "]");
}

static void item_json(JBuf *b, const Ent *e, const Grp *g, const Ent *all) {
    const Msg *m = e->m;
    const Account *a = accounts[e->acct];
    jb_raw(b, "{\"acct\":"); jb_str(b, a->id);
    jb_raw(b, ",\"folder\":"); jb_str(b, e->f->name);
    jb_printf(b, ",\"uid\":%u,\"date\":%lld", m->uid, (long long)m->date * 1000);
    jb_raw(b, ",\"from\":"); jb_str(b, m->from_name && *m->from_name ? m->from_name : m->from_email);
    jb_raw(b, ",\"email\":"); jb_str(b, m->from_email);
    jb_raw(b, ",\"to\":"); jb_str(b, m->to);
    jb_raw(b, ",\"subject\":"); jb_str(b, m->subject);
    jb_raw(b, ",\"preview\":"); jb_str(b, m->preview);
    flags_json(b, m->flags);
    jb_printf(b, ",\"att\":%s", m->att ? "true" : "false");
    if (g) {
        jb_printf(b, ",\"n\":%d,\"unreadN\":%d,\"anyFlag\":%s,\"tk\":\"%016llx\"", g->n, g->unread, g->flagged ? "true" : "false", (unsigned long long)g->key);
        if (g->n > 1) {
            /* the conversation's messages in this list, newest first (for actions and expanding) */
            jb_raw(b, ",\"thread\":[");
            int k = 0;
            for (int i = g->first; all && k < g->n && k < 200; i++) {
                if (all[i].key != g->key) continue;
                if (k) jb_raw(b, ",");
                jb_raw(b, "{\"acct\":"); jb_str(b, accounts[all[i].acct]->id);
                jb_raw(b, ",\"folder\":"); jb_str(b, all[i].f->name);
                jb_printf(b, ",\"uid\":%u,\"seen\":%s}", all[i].m->uid, all[i].m->flags & F_SEEN ? "true" : "false");
                k++;
            }
            jb_raw(b, "]");
        }
    }
    jb_raw(b, "}");
}

static int filter_ok(const Msg *m, const char *filter, int label) {
    if (m->flags & F_DELETED) return 0;
    if (label && !(m->flags & (F_LABEL1 << (label - 1)))) return 0;
    if (!filter || !*filter || !strcmp(filter, "all")) return 1;
    if (!strcmp(filter, "unread")) return !(m->flags & F_SEEN);
    if (!strcmp(filter, "flagged")) return (m->flags & F_FLAGGED) != 0;
    if (!strcmp(filter, "att")) return m->att != 0;
    return 1;
}

/* cache of the last built list (sorting 10,000 rows on every scroll would be wasteful) */
static struct { char key[700]; uint64_t ver; Ent *e; int n; Grp *g; int ng; } view;

static uint64_t versions(const char *acct, const char *folder, int unified) {
    uint64_t v = 0;
    for (int i = 0; i < naccounts; i++) {
        Account *a = accounts[i];
        if (!a || a->stop || strcmp(a->kind, "mail")) continue;
        if (!unified && strcmp(a->id, acct)) continue;
        FolderList *fl = store_folders(a);
        for (int k = 0; fl && k < fl->n; k++) {
            Folder *f = fl->f[k];
            if (unified ? !strcmp(f->special, "inbox") : !strcmp(f->name, folder)) v = v * 31 + f->version + (uint64_t)(uintptr_t)f;
        }
    }
    return v;
}

static int build_view(const char *acct, const char *folder, int unified, int threaded, const char *filter, int label) {
    char key[700];
    snprintf(key, sizeof key, "%s|%s|%d|%d|%s|%d", acct ? acct : "", folder ? folder : "", unified, threaded, filter ? filter : "", label);
    uint64_t ver = versions(acct ? acct : "", folder ? folder : "", unified);
    if (!strcmp(view.key, key) && view.ver == ver && view.e) return 0;
    free(view.e); free(view.g);
    memset(&view, 0, sizeof view);
    int cap = 0;
    for (int i = 0; i < naccounts; i++) {
        Account *a = accounts[i];
        if (!a || a->stop || strcmp(a->kind, "mail") || (!unified && strcmp(a->id, acct))) continue;
        FolderList *fl = store_folders(a);
        for (int k = 0; fl && k < fl->n; k++) {
            Folder *f = fl->f[k];
            if (unified ? strcmp(f->special, "inbox") : strcmp(f->name, folder)) continue;
            cap += f->n;
        }
    }
    view.e = malloc((size_t)(cap ? cap : 1) * sizeof *view.e);
    if (!view.e) return -1;
    for (int i = 0; i < naccounts; i++) {
        Account *a = accounts[i];
        if (!a || a->stop || strcmp(a->kind, "mail") || (!unified && strcmp(a->id, acct))) continue;
        FolderList *fl = store_folders(a);
        for (int k = 0; fl && k < fl->n; k++) {
            Folder *f = fl->f[k];
            if (unified ? strcmp(f->special, "inbox") : strcmp(f->name, folder)) continue;
            for (int m = 0; m < f->n; m++) if (filter_ok(&f->msgs[m], filter, label)) view.e[view.n++] = (Ent){ i, f, &f->msgs[m], 0 };
        }
    }
    qsort(view.e, (size_t)view.n, sizeof *view.e, cmp_ent);
    view.g = malloc((size_t)(view.n ? view.n : 1) * sizeof *view.g);
    if (!view.g) return -1;
    if (threaded) {
        /* hash table key → group */
        size_t hc = 16;
        while (hc < (size_t)view.n * 2) hc *= 2;
        int *tab = malloc(hc * sizeof *tab);
        if (!tab) return -1;
        for (size_t i = 0; i < hc; i++) tab[i] = -1;
        /* subject keys of originals, so header-less replies can join them */
        for (int i = 0; i < view.n; i++) {
            Ent *e = &view.e[i];
            uint64_t k = thread_key(e->m) ^ ((uint64_t)e->acct << 56);
            e->key = k;
        }
        for (int i = 0; i < view.n; i++) {
            Ent *e = &view.e[i];
            size_t h = (size_t)e->key & (hc - 1);
            while (tab[h] >= 0 && view.g[tab[h]].key != e->key) h = (h + 1) & (hc - 1);
            if (tab[h] < 0) {
                tab[h] = view.ng;
                view.g[view.ng++] = (Grp){ i, 0, 0, 0, 0, e->key };
            }
            Grp *g = &view.g[tab[h]];
            g->n++;
            if (!(e->m->flags & F_SEEN)) g->unread++;
            if (e->m->flags & F_FLAGGED) g->flagged = 1;
            if (e->m->att) g->att = 1;
        }
        free(tab);
        (void)subject_key;
    } else {
        for (int i = 0; i < view.n; i++) view.g[view.ng++] = (Grp){ i, 1, !(view.e[i].m->flags & F_SEEN), (view.e[i].m->flags & F_FLAGGED) != 0, view.e[i].m->att, 0 };
    }
    snprintf(view.key, sizeof view.key, "%s", key);
    view.ver = ver;
    return 0;
}

void store_list_json(JBuf *b, const JVal *args) {
    const char *acct = json_str(args, "acct"), *folder = json_str(args, "folder");
    int unified = json_bool(args, "unified", 0) || (folder && !strcmp(folder, "*inbox"));
    int threaded = json_bool(args, "threaded", 1);
    const char *filter = json_str(args, "filter");
    int label = (int)json_num(args, "label", 0);
    if (label < 0 || label > 6) label = 0;
    int off = (int)json_num(args, "offset", 0), cnt = (int)json_num(args, "count", 60);
    if (off < 0) off = 0;
    if (cnt < 1) cnt = 1;
    if (cnt > 500) cnt = 500;
    if (!unified && (!acct || !folder)) { jb_raw(b, "{\"total\":0,\"items\":[]}"); return; }
    if (build_view(acct, folder, unified, threaded, filter, label)) { jb_raw(b, "{\"total\":0,\"items\":[],\"error\":\"memory\"}"); return; }
    jb_printf(b, "{\"total\":%d,\"messages\":%d,\"version\":%llu,\"items\":[", view.ng, view.n, (unsigned long long)view.ver);
    for (int i = off; i < view.ng && i < off + cnt; i++) {
        if (i > off) jb_raw(b, ",");
        item_json(b, &view.e[view.g[i].first], threaded ? &view.g[i] : NULL, view.e);
    }
    jb_raw(b, "]}");
}

void store_search_json(JBuf *b, const JVal *args) {
    const char *q = json_str(args, "q");
    const char *acct = json_str(args, "acct");
    Hit *hits = malloc(sizeof *hits * 2000);
    int n = hits && q ? search_query(q, hits, 2000) : 0;
    Ent *ents = malloc(sizeof *ents * (size_t)(n ? n : 1));
    int ne = 0;
    for (int i = 0; ents && i < n; i++) {
        if (hits[i].acct < 0 || hits[i].acct >= naccounts || !accounts[hits[i].acct] || accounts[hits[i].acct]->stop) continue;
        if (acct && *acct && strcmp(accounts[hits[i].acct]->id, acct)) continue;
        Msg *m = store_msg(hits[i].f, hits[i].uid);
        if (!m || (m->flags & F_DELETED)) continue;
        ents[ne++] = (Ent){ hits[i].acct, hits[i].f, m, 0 };
    }
    if (ents) qsort(ents, (size_t)ne, sizeof *ents, cmp_ent);
    int max = (int)json_num(args, "count", 300);
    if (max < 1 || max > 2000) max = 300;
    jb_printf(b, "{\"total\":%d,\"items\":[", ne);
    for (int i = 0; i < ne && i < max; i++) {
        if (i) jb_raw(b, ",");
        item_json(b, &ents[i], NULL, NULL);
    }
    jb_raw(b, "]}");
    free(ents);
    free(hits);
}

void store_autocomplete(const char *q, JBuf *b, int *count, int max) {
    char fq[256];
    search_fold(q, fq, sizeof fq);
    if (strlen(fq) < 1) return;
    /* senders and recipients we have seen, newest first, without repeats */
    typedef struct { const char *name, *email; int64_t date; } Seen;
    int cap = 64, n = 0;
    Seen *s = malloc((size_t)cap * sizeof *s);
    if (!s) return;
    for (int i = 0; i < naccounts && n < 4000; i++) {
        Account *a = accounts[i];
        if (!a || a->stop || strcmp(a->kind, "mail")) continue;
        FolderList *fl = store_folders(a);
        for (int k = 0; fl && k < fl->n; k++) {
            Folder *f = fl->f[k];
            if (!strcmp(f->special, "junk") || !strcmp(f->special, "trash")) continue;
            for (int m = 0; m < f->n && n < 4000; m++) {
                Msg *x = &f->msgs[m];
                if (!x->from_email || !*x->from_email) continue;
                char fe[300], fnm[300];
                search_fold(x->from_email, fe, sizeof fe);
                search_fold(x->from_name, fnm, sizeof fnm);
                if (strncmp(fe, fq, strlen(fq)) && !strstr(fnm, fq)) continue;
                int dup = 0;
                for (int d = 0; d < n; d++) if (!strcasecmp(s[d].email, x->from_email)) { dup = 1; if (x->date > s[d].date) s[d].date = x->date; break; }
                if (dup) continue;
                if (n == cap) { cap *= 2; Seen *ns = realloc(s, (size_t)cap * sizeof *ns); if (!ns) break; s = ns; }
                s[n++] = (Seen){ x->from_name, x->from_email, x->date };
            }
        }
    }
    /* newest first */
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && s[j].date > s[j - 1].date; j--) { Seen t = s[j]; s[j] = s[j - 1]; s[j - 1] = t; }
    for (int i = 0; i < n && *count < max; i++) {
        if (*count) jb_raw(b, ",");
        jb_raw(b, "{\"name\":"); jb_str(b, s[i].name);
        jb_raw(b, ",\"email\":"); jb_str(b, s[i].email);
        jb_raw(b, ",\"src\":\"mail\"}");
        (*count)++;
    }
    free(s);
}

/* Every message of one conversation in the account, oldest first, from any folder except the
 * trash and junk (so the reading pane shows the replies we sent too). */
void store_thread_json(JBuf *b, const JVal *args) {
    Account *a = acct_get(json_str(args, "acct"));
    const char *tk = json_str(args, "tk");
    jb_raw(b, "{\"items\":[");
    if (!a || !tk) { jb_raw(b, "]}"); return; }
    unsigned long long want = strtoull(tk, NULL, 16);
    int ai = acct_index(a);
    FolderList *fl = store_folders(a);
    Ent *e = NULL;
    int n = 0, cap = 0;
    for (int k = 0; fl && k < fl->n; k++) {
        Folder *f = fl->f[k];
        if (!strcmp(f->special, "trash") || !strcmp(f->special, "junk") || !strcmp(f->special, "all")) continue;
        for (int m = 0; m < f->n; m++) {
            if (f->msgs[m].flags & F_DELETED) continue;
            uint64_t key = thread_key(&f->msgs[m]) ^ ((uint64_t)ai << 56);
            if (key != want) continue;
            if (n == cap) { cap = cap ? cap * 2 : 16; Ent *ne = realloc(e, (size_t)cap * sizeof *ne); if (!ne) break; e = ne; }
            e[n++] = (Ent){ ai, f, &f->msgs[m], key };
        }
    }
    if (e) qsort(e, (size_t)n, sizeof *e, cmp_ent);
    /* duplicates (the same Message-ID in two folders, e.g. Gmail labels): keep one */
    int out = 0;
    for (int i = n - 1; i >= 0 && out < 200; i--) {
        int dup = 0;
        for (int j = n - 1; j > i; j--) if (e[j].m->msgid && *e[j].m->msgid && e[i].m->msgid && !strcmp(e[j].m->msgid, e[i].m->msgid)) dup = 1;
        if (dup) continue;
        if (out) jb_raw(b, ",");
        item_json(b, &e[i], NULL, NULL);
        out++;
    }
    jb_raw(b, "]}");
    free(e);
}
