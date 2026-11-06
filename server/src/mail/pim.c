/* pim.c — calendars, reminders and address books of lord-maild, local and synced with CalDAV /
 * CardDAV (see maild.h, dav.h and docs/MAIL.md).
 *
 * A collection is a calendar (events and/or to-dos) or an address book. Local ones live only
 * here; DAV ones mirror a server collection: every item keeps its href and ETag, local edits are
 * marked dirty and pushed with If-Match, and a sync compares ETags (after the CTag says
 * something changed) and fetches what changed with a multiget REPORT. On a conflict the server's
 * version wins and the local one is kept, with a new UID, in a collection on this computer
 * (keep_local_copy). The pure parts (hrefs, discovery answers, the plan) are in davutil.c.
 *
 * Files under the cache: pim/colls (one line per collection), pim/<id>/meta (one line per item:
 * file, uid, href, etag, dirty) and pim/<id>/<file> (the .ics or .vcf). Callers hold G, except
 * pim_dav_sync, which takes it only around in-memory changes. */
#include "maild.h"
#include "dav.h"
#include "ical.h"
#include "vcard.h"
#include "xml.h"
#include "davutil.h"
#include "mime.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#define MAX_COLLS 256

enum { CLEAN = 0, DIRTY_PUT = 1, DIRTY_DEL = 2 };

typedef struct {
    char uid[256], file[64], href[1024], etag[256];
    int dirty;
    IcEvent *ev;            /* calendars: master and overrides */
    int nev;
    char *json;             /* address books: the contact as JSON */
    char fn[256];
    char *emails;           /* "name\temail\n" lines */
    int photo;
} Item;

typedef struct {
    char id[80];
    char kind;              /* 'c' calendar, 'b' address book */
    char acct[40];          /* "" = local */
    char name[200], color[16], href[1024], ctag[256];
    int todos, events, hidden, seen;
    Item *items;
    int n, cap;
} Coll;

static Coll *colls[MAX_COLLS];
static int ncolls;

/* ---- helpers ---- */

static void new_uid(char *out, size_t cap) {
    unsigned char r[16];
    if (getrandom(r, sizeof r, 0) != (ssize_t)sizeof r) for (int i = 0; i < 16; i++) r[i] = (unsigned char)(rand() ^ (int)time(NULL));
    r[6] = (unsigned char)((r[6] & 0x0f) | 0x40);
    r[8] = (unsigned char)((r[8] & 0x3f) | 0x80);
    snprintf(out, cap, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9],
             r[10], r[11], r[12], r[13], r[14], r[15]);
}

static void put_field(JBuf *b, const char *s) {
    for (; s && *s; s++) {
        if (*s == '\t') jb_raw(b, "\\t");
        else if (*s == '\n') jb_raw(b, "\\n");
        else if (*s == '\\') jb_raw(b, "\\\\");
        else if (*s != '\r') { char c[2] = { *s, 0 }; jb_raw(b, c); }
    }
}

/* One field of a NUL-terminated line (lines are split before parsing), unescaped in place. */
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

/* Calls fn for every line of buf (modified in place). */
#define FOR_LINES(buf, line) for (char *line##_n, *line = (buf); line && *line; line = line##_n) \
    if ((line##_n = strchr(line, '\n')) ? (*line##_n++ = 0, 1) : 1)

static void scopy(char *d, size_t cap, const char *s) { if (!cap) return; size_t n = s ? strlen(s) : 0; if (n >= cap) n = cap - 1; if (n) memcpy(d, s, n); d[n] = 0; }

static Coll *coll_get(const char *id) {
    if (!id) return NULL;
    for (int i = 0; i < ncolls; i++) if (!strcmp(colls[i]->id, id)) return colls[i];
    return NULL;
}

static void item_clear(Item *it) {
    if (it->ev) ical_events_free(it->ev, it->nev);
    free(it->json);
    free(it->emails);
    it->ev = NULL; it->nev = 0; it->json = NULL; it->emails = NULL;
}

static void contact_emails_cb(void *u, const char *name, const char *email) {
    JBuf *b = u;
    put_field(b, name);
    jb_raw(b, "\t");
    put_field(b, email);
    jb_raw(b, "\n");
}

/* (Re)parse an item's text into the in-memory form. */
static int item_parse(Coll *c, Item *it, const char *text, size_t len) {
    item_clear(it);
    if (c->kind == 'c') {
        char method[24];
        int n = ical_events(text, len, &it->ev, method, sizeof method);
        if (n <= 0) { it->ev = NULL; return -1; }
        it->nev = n;
        if (!it->uid[0]) scopy(it->uid, sizeof it->uid, it->ev[0].uid);
        return 0;
    }
    JBuf b;
    jb_init(&b);
    if (vcard_json(text, len, &b) <= 0) { jb_free(&b); return -1; }
    it->json = b.p;
    JDoc d;
    if (!json_parse(it->json, b.n, &d, NULL, 0)) {
        scopy(it->fn, sizeof it->fn, json_str(d.root, "fn"));
        if (!it->uid[0]) scopy(it->uid, sizeof it->uid, json_str(d.root, "uid"));
        it->photo = json_bool(d.root, "photo", 0);
        json_free(&d);
    }
    JBuf e;
    jb_init(&e);
    vcard_emails(text, len, contact_emails_cb, &e);
    it->emails = e.p;
    return 0;
}

static void coll_dir(const Coll *c, const char *file, char *out, size_t cap) { snprintf(out, cap, "pim/%s/%s", c->id, file); }

static void save_meta(Coll *c) {
    JBuf b;
    jb_init(&b);
    for (int i = 0; i < c->n; i++) {
        Item *it = &c->items[i];
        put_field(&b, it->file); jb_raw(&b, "\t");
        put_field(&b, it->uid); jb_raw(&b, "\t");
        put_field(&b, it->href); jb_raw(&b, "\t");
        put_field(&b, it->etag);
        jb_printf(&b, "\t%d\n", it->dirty);
    }
    char rel[200];
    coll_dir(c, "meta", rel, sizeof rel);
    if (!b.oom) fs_write(rel, b.p ? b.p : "", b.n);
    jb_free(&b);
}

static void save_colls(void) {
    JBuf b;
    jb_init(&b);
    for (int i = 0; i < ncolls; i++) {
        Coll *c = colls[i];
        put_field(&b, c->id); jb_printf(&b, "\t%c\t", c->kind);
        put_field(&b, c->acct); jb_raw(&b, "\t");
        put_field(&b, c->name); jb_raw(&b, "\t");
        put_field(&b, c->color); jb_raw(&b, "\t");
        put_field(&b, c->href); jb_raw(&b, "\t");
        put_field(&b, c->ctag);
        jb_printf(&b, "\t%d\t%d\t%d\n", c->todos, c->events, c->hidden);
    }
    fs_mkdirs("pim");
    if (!b.oom) fs_write("pim/colls", b.p ? b.p : "", b.n);
    jb_free(&b);
}

static Coll *coll_new(const char *id, char kind, const char *acct, const char *name, const char *color) {
    if (ncolls >= MAX_COLLS) return NULL;
    Coll *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    scopy(c->id, sizeof c->id, id);
    c->kind = kind;
    scopy(c->acct, sizeof c->acct, acct);
    scopy(c->name, sizeof c->name, name);
    scopy(c->color, sizeof c->color, color && *color ? color : "#0a84ff");
    c->events = kind == 'c';
    colls[ncolls++] = c;
    char rel[160];
    snprintf(rel, sizeof rel, "pim/%s", c->id);
    fs_mkdirs(rel);
    return c;
}

static void coll_remove(Coll *c) {
    for (int i = 0; i < c->n; i++) item_clear(&c->items[i]);
    free(c->items);
    char rel[160];
    snprintf(rel, sizeof rel, "pim/%s", c->id);
    fs_rmtree(rel);
    for (int i = 0; i < ncolls; i++) if (colls[i] == c) { colls[i] = colls[--ncolls]; break; }
    free(c);
}

static Item *item_get(Coll *c, const char *uid) {
    for (int i = 0; uid && i < c->n; i++) if (!strcmp(c->items[i].uid, uid)) return &c->items[i];
    return NULL;
}

static Item *item_by_href(Coll *c, const char *href) {
    for (int i = 0; href && i < c->n; i++) if (!strcmp(c->items[i].href, href)) return &c->items[i];
    return NULL;
}

static Item *item_new(Coll *c, const char *uid) {
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 32;
        Item *ni = realloc(c->items, (size_t)nc * sizeof *ni);
        if (!ni) return NULL;
        c->items = ni;
        c->cap = nc;
    }
    Item *it = &c->items[c->n++];
    memset(it, 0, sizeof *it);
    scopy(it->uid, sizeof it->uid, uid);
    char h[17];
    fs_hash(uid && *uid ? uid : "x", h);
    snprintf(it->file, sizeof it->file, "%s.%s", h, c->kind == 'c' ? "ics" : "vcf");
    return it;
}

static void item_delete(Coll *c, Item *it) {
    char rel[200];
    coll_dir(c, it->file, rel, sizeof rel);
    fs_unlink(rel);
    item_clear(it);
    int pos = (int)(it - c->items);
    c->items[pos] = c->items[--c->n];
}

/* Writes the item's text, parses it, and marks it for upload when the collection syncs. */
static int item_write(Coll *c, Item *it, const char *text, size_t len, int dirty) {
    char rel[200];
    coll_dir(c, it->file, rel, sizeof rel);
    if (fs_write(rel, text, len)) return -1;
    if (item_parse(c, it, text, len)) return -1;
    if (dirty && c->acct[0]) {
        it->dirty = DIRTY_PUT;
        if (!it->href[0]) {
            char safe[200];
            fs_safe_name(it->uid, safe, sizeof safe);
            for (char *p = safe; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') *p = '_';
            SNPF(it->href, sizeof it->href, "%s%s%s.%s", c->href, c->href[0] && c->href[strlen(c->href) - 1] == '/' ? "" : "/", safe, c->kind == 'c' ? "ics" : "vcf");
        }
    }
    save_meta(c);
    return 0;
}

static void changed_event(const char *what) {
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"ev\":\"pim-changed\",\"what\":\"%s\"}", what);
    send_event(&b);
}

static void push_soon(Coll *c) {
    if (!c->acct[0]) return;
    Account *a = acct_get(c->acct);
    if (!a) return;
    Job *j = job_new(J_DAV_SYNC, 0, 5);
    if (j) job_push(a, j);
}

/* ---- loading ---- */

static void load_coll(Coll *c) {
    char rel[200];
    coll_dir(c, "meta", rel, sizeof rel);
    size_t n;
    char *meta = fs_read(rel, &n, 64 << 20);
    if (!meta) return;
    FOR_LINES(meta, line) {
        char *p = line;
        char *file = get_field(&p), *uid = get_field(&p), *href = get_field(&p), *etag = get_field(&p);
        int dirty = atoi(p);
        if (!file[0] || strchr(file, '/') || file[0] == '.') continue;
        Item *it = item_new(c, uid);
        if (!it) break;
        scopy(it->file, sizeof it->file, file);
        scopy(it->href, sizeof it->href, href);
        scopy(it->etag, sizeof it->etag, etag);
        it->dirty = dirty == DIRTY_PUT || dirty == DIRTY_DEL ? dirty : CLEAN;
        char fr[200];
        coll_dir(c, file, fr, sizeof fr);
        size_t tl;
        char *t = fs_read(fr, &tl, 16 << 20);
        if (!t || (item_parse(c, it, t, tl) && it->dirty != DIRTY_DEL)) { c->n--; free(t); continue; }
        free(t);
    }
    free(meta);
}

void pim_init(void) {
    fs_mkdirs("pim");
    size_t n;
    char *list = fs_read("pim/colls", &n, 4 << 20);
    if (list) {
        FOR_LINES(list, line) {
            char *p = line;
            char *id = get_field(&p), *kind = get_field(&p), *acct = get_field(&p), *name = get_field(&p), *color = get_field(&p),
                 *href = get_field(&p), *ctag = get_field(&p);
            int todos = 0, events = 0, hidden = 0;
            sscanf(p, "%d\t%d\t%d", &todos, &events, &hidden);
            if (!id[0] || strchr(id, '/') || id[0] == '.' || (kind[0] != 'c' && kind[0] != 'b')) continue;
            Coll *c = coll_new(id, kind[0], acct, name, color);
            if (!c) break;
            scopy(c->href, sizeof c->href, href);
            scopy(c->ctag, sizeof c->ctag, ctag);
            c->todos = todos; c->events = events; c->hidden = hidden;
            load_coll(c);
        }
        free(list);
    }
    /* first run: a calendar, a reminders list and an address book on this computer */
    int cal = 0, rem = 0, book = 0;
    for (int i = 0; i < ncolls; i++) { if (colls[i]->kind == 'b') book = 1; else if (colls[i]->todos && !colls[i]->events) rem = 1; else cal = 1; }
    if (!cal) coll_new("local-calendar", 'c', "", "Personal", "#0a84ff");
    if (!rem) { Coll *c = coll_new("local-reminders", 'c', "", "Reminders", "#ff9f0a"); if (c) { c->todos = 1; c->events = 0; } }
    if (!book) coll_new("local-contacts", 'b', "", "Contacts", "#8e8e93");
    if (!cal || !rem || !book) save_colls();
}

void pim_account_removed(const char *id) {
    pthread_mutex_lock(&G);
    for (int i = ncolls - 1; i >= 0; i--) if (!strcmp(colls[i]->acct, id)) coll_remove(colls[i]);
    save_colls();
    pthread_mutex_unlock(&G);
    changed_event("all");
}

/* ---- JSON ---- */

static void coll_json(JBuf *b, const Coll *c) {
    jb_raw(b, "{\"id\":"); jb_str(b, c->id);
    jb_raw(b, ",\"name\":"); jb_str(b, c->name);
    jb_raw(b, ",\"color\":"); jb_str(b, c->color);
    jb_raw(b, ",\"acct\":"); jb_str(b, c->acct);
    Account *a = c->acct[0] ? acct_get(c->acct) : NULL;
    jb_raw(b, ",\"acctName\":"); jb_str(b, a ? (a->name[0] ? a->name : a->user) : "");
    jb_printf(b, ",\"kind\":\"%s\",\"todos\":%s,\"events\":%s,\"hidden\":%s,\"count\":%d}", c->acct[0] ? "dav" : "local", c->todos ? "true" : "false",
              c->events ? "true" : "false", c->hidden ? "true" : "false", c->n);
}

typedef struct { JBuf *b; const Coll *c; const Item *it; const IcEvent *ev; int64_t *rids; int nrids; int count, max; } OccCtx;

static void occ_cb(void *u, int64_t start) {
    OccCtx *o = u;
    for (int i = 0; i < o->nrids; i++) if (o->rids[i] == start) return;      /* replaced by an override */
    if (o->count >= o->max) return;
    if (o->count++) jb_raw(o->b, ",");
    JBuf tmp;
    jb_init(&tmp);
    ical_event_json(&tmp, o->ev, o->ev->rrule[0] ? start : 0);
    if (tmp.p && tmp.n > 1) {
        tmp.p[tmp.n - 1] = 0;                 /* drop "}" to add the calendar */
        jb_raw(o->b, tmp.p);
        jb_raw(o->b, ",\"cal\":"); jb_str(o->b, o->c->id);
        jb_raw(o->b, ",\"color\":"); jb_str(o->b, o->c->color);
        jb_raw(o->b, "}");
    } else jb_raw(o->b, "{}");
    jb_free(&tmp);
}

static int cal_wanted(const Coll *c, const JVal *cals) {
    if (!cals || cals->type != J_ARR) return !c->hidden;
    for (const JVal *x = cals->kid; x; x = x->next) if (x->type == J_STR && !strcmp(x->str, c->id)) return 1;
    return 0;
}

static void events_json(JBuf *b, int64_t from, int64_t to, const JVal *cals) {
    jb_raw(b, "{\"events\":[");
    OccCtx o = { b, NULL, NULL, NULL, NULL, 0, 0, 20000 };
    for (int i = 0; i < ncolls; i++) {
        Coll *c = colls[i];
        if (c->kind != 'c' || !c->events || !cal_wanted(c, cals)) continue;
        for (int k = 0; k < c->n; k++) {
            Item *it = &c->items[k];
            if (it->dirty == DIRTY_DEL) continue;
            int64_t rids[64];
            int nr = 0;
            for (int e = 0; e < it->nev && nr < 64; e++) if (it->ev[e].recurrence_id) rids[nr++] = it->ev[e].recurrence_id;
            for (int e = 0; e < it->nev; e++) {
                IcEvent *ev = &it->ev[e];
                if (ev->is_todo) continue;
                o.c = c; o.it = it; o.ev = ev;
                o.rids = ev->recurrence_id ? NULL : rids;
                o.nrids = ev->recurrence_id ? 0 : nr;
                ical_expand(ev, from, to, 2000, occ_cb, &o);
            }
        }
    }
    jb_raw(b, "]}");
}

static void todos_json(JBuf *b, const JVal *lists) {
    jb_raw(b, "{\"todos\":[");
    int n = 0;
    for (int i = 0; i < ncolls; i++) {
        Coll *c = colls[i];
        if (c->kind != 'c' || !c->todos || !cal_wanted(c, lists)) continue;
        for (int k = 0; k < c->n; k++) {
            Item *it = &c->items[k];
            if (it->dirty == DIRTY_DEL) continue;
            for (int e = 0; e < it->nev; e++) {
                if (!it->ev[e].is_todo) continue;
                if (n++) jb_raw(b, ",");
                JBuf tmp;
                jb_init(&tmp);
                ical_event_json(&tmp, &it->ev[e], 0);
                if (tmp.p && tmp.n > 1) { tmp.p[tmp.n - 1] = 0; jb_raw(b, tmp.p); jb_raw(b, ",\"list\":"); jb_str(b, c->id); jb_raw(b, ",\"color\":"); jb_str(b, c->color); jb_raw(b, "}"); }
                else jb_raw(b, "{}");
                jb_free(&tmp);
            }
        }
    }
    jb_raw(b, "]}");
}

typedef struct { JBuf *b; int *count; int64_t from, to; const Coll *c; const IcEvent *ev; } AlarmCtx;

static void alarm_cb(void *u, int64_t start) {
    AlarmCtx *a = u;
    for (int i = 0; i < a->ev->nalarms; i++) {
        int64_t at = start - (int64_t)a->ev->alarms[i] * 60;
        if (at < a->from || at >= a->to) continue;
        if ((*a->count)++) jb_raw(a->b, ",");
        jb_printf(a->b, "{\"at\":%lld,\"start\":%lld,\"kind\":\"%s\",\"cal\":", (long long)at * 1000, (long long)start * 1000, a->ev->is_todo ? "todo" : "event");
        jb_str(a->b, a->c->id);
        jb_raw(a->b, ",\"uid\":"); jb_str(a->b, a->ev->uid);
        jb_raw(a->b, ",\"title\":"); jb_str(a->b, a->ev->summary);
        jb_raw(a->b, ",\"loc\":"); jb_str(a->b, a->ev->location);
        jb_printf(a->b, ",\"allDay\":%s}", a->ev->allday ? "true" : "false");
    }
}

/* Upcoming alarms of events and to-dos in [from, to): the SDK schedules them as notifications,
 * like the Clock alarms, so they ring with the apps closed. */
void pim_alarms_json(JBuf *b, int64_t from, int64_t to) {
    pthread_mutex_lock(&G);
    jb_raw(b, "{\"alarms\":[");
    int count = 0;
    for (int i = 0; i < ncolls; i++) {
        Coll *c = colls[i];
        if (c->kind != 'c' || c->hidden) continue;
        for (int k = 0; k < c->n; k++) {
            Item *it = &c->items[k];
            if (it->dirty == DIRTY_DEL) continue;
            for (int e = 0; e < it->nev; e++) {
                IcEvent *ev = &it->ev[e];
                if (ev->is_todo) {
                    if (ev->completed || !ev->due) continue;
                    /* a to-do with a due time and no alarm rings at the due time (all-day: 9:00) */
                    int64_t at = ev->due;
                    if (ev->due_allday) at += 9 * 3600;
                    IcEvent tmp = *ev;
                    int zero = 0;
                    if (!tmp.nalarms) { tmp.nalarms = 1; memcpy(tmp.alarms, &zero, sizeof zero); }
                    AlarmCtx ac = { b, &count, from, to, c, &tmp };
                    alarm_cb(&ac, at);
                    continue;
                }
                if (!ev->nalarms) continue;
                AlarmCtx ac = { b, &count, from, to, c, ev };
                /* occurrences that start up to a week after the window (alarms ring before) */
                ical_expand(ev, from, to + 7 * 86400, 500, alarm_cb, &ac);
            }
        }
    }
    jb_raw(b, "]}");
    pthread_mutex_unlock(&G);
}

void pim_autocomplete(const char *q, JBuf *b, int *count, int max) {
    char fq[256];
    search_fold(q, fq, sizeof fq);
    size_t ql = strlen(fq);
    if (!ql) return;
    for (int i = 0; i < ncolls && *count < max; i++) {
        Coll *c = colls[i];
        if (c->kind != 'b') continue;
        for (int k = 0; k < c->n && *count < max; k++) {
            Item *it = &c->items[k];
            if (!it->emails || it->dirty == DIRTY_DEL) continue;
            char *dup = strdup(it->emails);
            if (!dup) continue;
            FOR_LINES(dup, line) {
                if (*count >= max) break;
                char *p = line;
                char *name = get_field(&p);
                char *email = get_field(&p);
                char fn[300], fe[300];
                search_fold(name, fn, sizeof fn);
                search_fold(email, fe, sizeof fe);
                /* a word of the name or the address starts with the query */
                int hit = !strncmp(fe, fq, ql);
                for (char *w = fn; !hit && *w; ) { if (!strncmp(w, fq, ql)) hit = 1; while (*w && *w != ' ') w++; while (*w == ' ') w++; }
                if (!hit) continue;
                if (*count) jb_raw(b, ",");
                jb_raw(b, "{\"name\":"); jb_str(b, name);
                jb_raw(b, ",\"email\":"); jb_str(b, email);
                jb_raw(b, ",\"src\":\"contacts\"}");
                (*count)++;
            }
            free(dup);
        }
    }
}

int pim_has_event(const char *uid, char *cal, size_t cap, char *ps, size_t pcap, const char *me) {
    for (int i = 0; i < ncolls; i++) {
        Coll *c = colls[i];
        if (c->kind != 'c') continue;
        Item *it = item_get(c, uid);
        if (!it || it->dirty == DIRTY_DEL) continue;
        scopy(cal, cap, c->id);
        if (pcap) ps[0] = 0;
        for (int e = 0; e < it->nev; e++) for (int k = 0; k < it->ev[e].natt; k++) if (!strcasecmp(it->ev[e].att[k].email, me)) scopy(ps, pcap, it->ev[e].att[k].partstat);
        return 1;
    }
    return 0;
}

static Coll *default_calendar(void) {
    for (int i = 0; i < ncolls; i++) if (colls[i]->kind == 'c' && colls[i]->events && !colls[i]->hidden) return colls[i];
    return NULL;
}

/* An invitation into a calendar, with my answer in it. */
int pim_add_invite(const char *calid, const char *ics, size_t len, const char *partstat, const char *me, char *err, size_t ecap) {
    IcEvent *ev;
    char method[24];
    int n = ical_events(ics, len, &ev, method, sizeof method);
    if (n <= 0) { scopy(err, ecap, "bad invitation"); return -1; }
    Coll *c = coll_get(calid);
    if (!c || c->kind != 'c') {
        /* an existing copy keeps its calendar */
        for (int i = 0; i < ncolls && !c; i++) if (colls[i]->kind == 'c' && item_get(colls[i], ev[0].uid)) c = colls[i];
        if (!c) c = default_calendar();
    }
    if (!c) { ical_events_free(ev, n); scopy(err, ecap, "no calendar"); return -1; }
    IcEvent *e = &ev[0];
    int found = 0;
    for (int k = 0; k < e->natt; k++) if (!strcasecmp(e->att[k].email, me)) { scopy(e->att[k].partstat, sizeof e->att[k].partstat, partstat); found = 1; }
    if (!found && e->natt < IC_MAX_ATT) {
        IcAttendee *na = realloc(e->att, (size_t)(e->natt + 1) * sizeof *na);
        if (na) { e->att = na; memset(&e->att[e->natt], 0, sizeof *e->att); scopy(e->att[e->natt].email, sizeof e->att[0].email, me); scopy(e->att[e->natt].partstat, sizeof e->att[0].partstat, partstat); e->natt++; }
    }
    IcBuild b = { 0 };
    b.uid = e->uid; b.summary = e->summary; b.location = e->location; b.description = e->description; b.url = e->url;
    b.start = e->start; b.end = e->end; b.allday = e->allday; b.tzid = e->tzid; b.rrule = e->rrule;
    b.alarms = e->alarms; b.nalarms = e->nalarms; b.organizer = e->organizer; b.organizer_name = e->organizer_name;
    b.att = e->att; b.natt = e->natt; b.sequence = e->sequence; b.exdate = e->exdate; b.nexdate = e->nexdate;
    b.status = !strcmp(partstat, "TENTATIVE") ? "TENTATIVE" : "CONFIRMED";
    if (!b.nalarms) { static const int def[] = { 15 }; b.alarms = def; b.nalarms = 1; }
    size_t ol;
    char *text = ical_build(&b, &ol);
    int r = -1;
    if (text) {
        Item *it = item_get(c, e->uid);
        if (!it) it = item_new(c, e->uid);
        if (it) r = item_write(c, it, text, ol, 1);
        free(text);
    }
    ical_events_free(ev, n);
    if (!r) { push_soon(c); changed_event("events"); }
    else scopy(err, ecap, "write");
    return r;
}

/* ---- requests ---- */

static int64_t ms_arg(const JVal *o, const char *k) { double v = json_num(o, k, 0); return (int64_t)(v / 1000); }

static void reply_ok_id(uint32_t id, const char *uid) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"ok\":true");
    if (uid) { jb_raw(&b, ",\"uid\":"); jb_str(&b, uid); }
    jb_raw(&b, "}");
    reply_done(id, &b);
}

static int valid_color(const char *c) {
    if (!c || c[0] != '#' || strlen(c) != 7) return 0;
    for (int i = 1; i < 7; i++) if (!isxdigit((unsigned char)c[i])) return 0;
    return 1;
}

static void save_event(uint32_t id, const JVal *args, int todo) {
    Coll *c = coll_get(json_str(args, todo ? "list" : "cal"));
    const JVal *e = json_get(args, todo ? "todo" : "event");
    if (!c || c->kind != 'c' || !e || e->type != J_OBJ) { reply_err(id, "bad-args", ""); return; }
    char uid[256];
    const char *u = json_str(e, "uid");
    if (u && *u && strlen(u) < 200) scopy(uid, sizeof uid, u); else new_uid(uid, sizeof uid);
    /* moved from another calendar */
    const char *from = json_str(args, "from");
    Coll *fc = from && strcmp(from, c->id) ? coll_get(from) : NULL;
    Item *old = item_get(fc ? fc : c, uid);
    const IcEvent *prev = old && old->nev ? &old->ev[0] : NULL;
    for (int i = 0; old && i < old->nev; i++) if (!old->ev[i].recurrence_id) prev = &old->ev[i];
    IcBuild b = { 0 };
    b.is_todo = todo;
    b.uid = uid;
    b.summary = json_str(e, "title");
    b.location = json_str(e, "loc");
    b.description = json_str(e, "notes");
    b.url = json_str(e, "url");
    b.allday = json_bool(e, "allDay", 0);
    b.start = ms_arg(e, "start");
    b.end = ms_arg(e, "end");
    b.tzid = json_str(e, "tz");
    if (!b.tzid && !b.allday) b.tzid = ic_local_zone();
    const char *rr = json_str(e, "rrule");
    b.rrule = rr && strlen(rr) < 400 ? rr : NULL;
    int al[8], nal = 0;
    const JVal *alarms = json_get(e, "alarms");
    for (const JVal *x = alarms && alarms->type == J_ARR ? alarms->kid : NULL; x && nal < 8; x = x->next) if (x->type == J_NUM && x->num > -100000 && x->num < 1000000) al[nal++] = (int)x->num;
    b.alarms = al;
    b.nalarms = nal;
    b.status = json_str(e, "status");
    b.categories = json_str(e, "cats");
    IcAttendee att[IC_MAX_ATT];
    int natt = 0;
    const JVal *atl = json_get(e, "att");
    for (const JVal *x = atl && atl->type == J_ARR ? atl->kid : NULL; x && natt < IC_MAX_ATT; x = x->next) {
        const char *em = json_str(x, "email");
        if (!em || !strchr(em, '@') || strlen(em) > 250) continue;
        memset(&att[natt], 0, sizeof att[natt]);
        scopy(att[natt].email, sizeof att[0].email, em);
        scopy(att[natt].name, sizeof att[0].name, json_str(x, "name"));
        scopy(att[natt].partstat, sizeof att[0].partstat, json_str(x, "ps") ? json_str(x, "ps") : "NEEDS-ACTION");
        att[natt].rsvp = 1;
        natt++;
    }
    if (natt) { b.att = att; b.natt = natt; b.organizer = json_str(e, "orgEmail"); b.organizer_name = json_str(e, "orgName"); }
    else if (prev) { b.att = prev->att; b.natt = prev->natt; b.organizer = prev->organizer; b.organizer_name = prev->organizer_name; }
    b.sequence = prev ? prev->sequence + 1 : 0;
    if (prev && !json_get(e, "exdate")) { b.exdate = prev->exdate; b.nexdate = prev->nexdate; }
    if (todo) {
        b.due = ms_arg(e, "due");
        b.due_allday = json_bool(e, "dueAllDay", 0);
        b.priority = (int)json_num(e, "prio", 0);
        if (b.priority < 0 || b.priority > 9) b.priority = 0;
        b.flagged = json_bool(e, "flag", 0);
        b.completed = json_bool(e, "done", 0) ? (prev && prev->completed > 1 ? prev->completed : (int64_t)time(NULL)) : 0;
        b.parent_uid = json_str(e, "parent");
        b.start = 0;
    } else if (b.end <= b.start) b.end = b.start + (b.allday ? 86400 : 3600);
    size_t len;
    char *text = ical_build(&b, &len);
    if (!text) { reply_err(id, "memory", ""); return; }
    if (fc && old) {
        /* move: delete from the old calendar */
        if (fc->acct[0] && old->href[0]) { old->dirty = DIRTY_DEL; save_meta(fc); push_soon(fc); }
        else { item_delete(fc, old); save_meta(fc); }
        old = NULL;
    }
    Item *it = item_get(c, uid);
    if (!it) it = item_new(c, uid);
    int r = it ? item_write(c, it, text, len, 1) : -1;
    free(text);
    if (r) { reply_err(id, "write", ""); return; }
    push_soon(c);
    changed_event(todo ? "todos" : "events");
    reply_ok_id(id, uid);
}

static void delete_event(uint32_t id, const JVal *args, int todo) {
    Coll *c = coll_get(json_str(args, todo ? "list" : "cal"));
    Item *it = c ? item_get(c, json_str(args, "uid")) : NULL;
    if (!it) { reply_err(id, "not-found", ""); return; }
    int64_t occ = ms_arg(args, "occ");
    if (!todo && occ && it->nev && it->ev[0].rrule[0] && !json_bool(args, "all", 0)) {
        /* one occurrence of a series: EXDATE */
        IcEvent *e = NULL;
        for (int i = 0; i < it->nev; i++) if (!it->ev[i].recurrence_id) e = &it->ev[i];
        if (!e || e->nexdate >= 64) { reply_err(id, "too-many-exceptions", ""); return; }
        int64_t ex[64];
        memcpy(ex, e->exdate, sizeof ex);
        int nex = e->nexdate;
        ex[nex++] = occ;
        IcBuild b = { 0 };
        b.uid = e->uid; b.summary = e->summary; b.location = e->location; b.description = e->description; b.url = e->url;
        b.start = e->start; b.end = e->end; b.allday = e->allday; b.tzid = e->tzid; b.rrule = e->rrule; b.status = e->status;
        b.alarms = e->alarms; b.nalarms = e->nalarms; b.organizer = e->organizer; b.organizer_name = e->organizer_name;
        b.att = e->att; b.natt = e->natt; b.sequence = e->sequence + 1; b.exdate = ex; b.nexdate = nex;
        size_t len;
        char *text = ical_build(&b, &len);
        int r = text ? item_write(c, it, text, len, 1) : -1;
        free(text);
        if (r) { reply_err(id, "write", ""); return; }
    } else if (c->acct[0] && it->href[0]) { it->dirty = DIRTY_DEL; save_meta(c); }
    else { item_delete(c, it); save_meta(c); }
    push_soon(c);
    changed_event(todo ? "todos" : "events");
    reply_ok_id(id, NULL);
}

static void contacts_json(JBuf *b, const char *book) {
    jb_raw(b, "{\"contacts\":[");
    int n = 0;
    for (int i = 0; i < ncolls; i++) {
        Coll *c = colls[i];
        if (c->kind != 'b' || (book && *book && strcmp(book, c->id))) continue;
        for (int k = 0; k < c->n; k++) {
            Item *it = &c->items[k];
            if (!it->json || it->dirty == DIRTY_DEL) continue;
            if (n++) jb_raw(b, ",");
            size_t l = strlen(it->json);
            if (l > 1 && it->json[l - 1] == '}') {
                JBuf tmp;
                jb_init(&tmp);
                jb_raw(&tmp, it->json);
                tmp.p[tmp.n - 1] = 0;
                jb_raw(b, tmp.p);
                jb_free(&tmp);
                jb_raw(b, ",\"book\":"); jb_str(b, c->id);
                jb_raw(b, "}");
            } else jb_raw(b, "{}");
        }
    }
    jb_raw(b, "]}");
}

static void save_contact(uint32_t id, const JVal *args, int *fds, int nfds) {
    Coll *c = coll_get(json_str(args, "book"));
    const JVal *ct = json_get(args, "contact");
    if (!c || c->kind != 'b' || !ct || ct->type != J_OBJ) { reply_err(id, "bad-args", ""); return; }
    char uid[256];
    const char *u = json_str(ct, "uid");
    if (u && *u && strlen(u) < 200) scopy(uid, sizeof uid, u); else new_uid(uid, sizeof uid);
    /* JSON with the uid set (vcard_build reads it from the object) */
    JBuf cj;
    jb_init(&cj);
    {
        /* rebuild the object with our uid: copy the members except uid */
        jb_raw(&cj, "{\"uid\":");
        jb_str(&cj, uid);
        jb_raw(&cj, "}");
    }
    JDoc ud;
    if (cj.oom || json_parse(cj.p, cj.n, &ud, NULL, 0)) { jb_free(&cj); reply_err(id, "memory", ""); return; }
    /* photo: keep the old one, a new picture (fd), or none */
    const char *ph = json_str(args, "photo");
    unsigned char *photo = NULL;
    size_t plen = 0;
    Item *old = item_get(c, uid);
    if (ph && !strcmp(ph, "new") && nfds > 0) {
        char buf[1];
        (void)buf;
        size_t n = 0;
        off_t sz = lseek(fds[0], 0, SEEK_END);
        if (sz > 0 && sz < (off_t)(32 << 20)) {
            unsigned char *raw = malloc((size_t)sz);
            if (raw && pread(fds[0], raw, (size_t)sz, 0) == sz) n = (size_t)sz;
            if (n) photo = img_thumb_jpeg(raw, n, 320, &plen);
            free(raw);
        }
        if (!photo) { json_free(&ud); jb_free(&cj); reply_err(id, "photo", "unreadable image"); return; }
    } else if ((!ph || !strcmp(ph, "keep")) && old) {
        char rel[200];
        coll_dir(c, old->file, rel, sizeof rel);
        size_t tl;
        char *t = fs_read(rel, &tl, 16 << 20);
        if (t) { photo = vcard_photo(t, tl, &plen); free(t); }
    }
    /* vcard_build takes the members from `ct` but the uid from ours: patch through a copy */
    JVal tmp = *ct;
    JVal uidv = *json_get(ud.root, "uid");
    uidv.next = (JVal *)ct->kid;
    uidv.key = "uid";
    tmp.kid = &uidv;              /* json_get finds our "uid" first */
    size_t vl;
    char *v = vcard_build(&tmp, photo, plen, "jpeg", &vl);
    free(photo);
    json_free(&ud);
    jb_free(&cj);
    if (!v) { reply_err(id, "memory", ""); return; }
    Item *it = old ? old : item_new(c, uid);
    int r = it ? item_write(c, it, v, vl, 1) : -1;
    free(v);
    if (r) { reply_err(id, "write", ""); return; }
    push_soon(c);
    changed_event("contacts");
    reply_ok_id(id, uid);
}

static void import_contacts(uint32_t id, const JVal *args, int *fds, int nfds) {
    Coll *c = coll_get(json_str(args, "book"));
    if (!c || c->kind != 'b' || nfds < 1) { reply_err(id, "bad-args", ""); return; }
    off_t sz = lseek(fds[0], 0, SEEK_END);
    if (sz <= 0 || sz > (off_t)(64 << 20)) { reply_err(id, "too-big", ""); return; }
    char *t = malloc((size_t)sz + 1);
    if (!t || pread(fds[0], t, (size_t)sz, 0) != sz) { free(t); reply_err(id, "read", ""); return; }
    t[sz] = 0;
    /* one item per VCARD */
    int added = 0;
    const char *p = t;
    while ((p = strcasestr(p, "BEGIN:VCARD"))) {
        const char *e = strcasestr(p, "END:VCARD");
        if (!e) break;
        e += 9;
        size_t len = (size_t)(e - p);
        char uid[256];
        vcard_uid(p, len, uid, sizeof uid);
        int fresh = !uid[0];
        if (fresh) new_uid(uid, sizeof uid);
        Item *it = item_get(c, uid);
        if (!it) it = item_new(c, uid);
        if (it) {
            if (fresh) {
                /* add a UID line after BEGIN:VCARD */
                JBuf nb;
                jb_init(&nb);
                const char *after = strchr(p, '\n');
                if (after) { jb_strn(&nb, "", 0); nb.n = 0; char *head = strndup(p, (size_t)(after - p + 1)); if (head) { jb_raw(&nb, head); free(head); } jb_printf(&nb, "UID:%s\r\n", uid); char *rest = strndup(after + 1, (size_t)(e - after - 1)); if (rest) { jb_raw(&nb, rest); free(rest); } }
                if (!nb.oom && nb.p) item_write(c, it, nb.p, nb.n, 1);
                jb_free(&nb);
            } else item_write(c, it, p, len, 1);
            added++;
        }
        p = e;
    }
    free(t);
    push_soon(c);
    changed_event("contacts");
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"ok\":true,\"added\":%d}", added);
    reply_done(id, &b);
}

void pim_handle(uint32_t id, const char *op, const JVal *args, int *fds, int nfds) {
    JBuf b;
    jb_init(&b);
    pthread_mutex_lock(&G);
    if (!strcmp(op, "cal-list") || !strcmp(op, "book-list")) {
        char kind = op[0] == 'c' ? 'c' : 'b';
        jb_raw(&b, kind == 'c' ? "{\"cals\":[" : "{\"books\":[");
        int n = 0;
        for (int i = 0; i < ncolls; i++) if (colls[i]->kind == kind) { if (n++) jb_raw(&b, ","); coll_json(&b, colls[i]); }
        jb_raw(&b, "]}");
        pthread_mutex_unlock(&G);
        reply_done(id, &b);
        return;
    }
    if (!strcmp(op, "cal-create") || !strcmp(op, "book-create")) {
        const char *name = json_str(args, "name"), *color = json_str(args, "color");
        if (!name || !*name || strlen(name) > 150) { pthread_mutex_unlock(&G); reply_err(id, "bad-name", ""); return; }
        char cid[80], u[40];
        new_uid(u, sizeof u);
        snprintf(cid, sizeof cid, "local-%.8s", u);
        Coll *c = coll_new(cid, op[0] == 'c' ? 'c' : 'b', "", name, valid_color(color) ? color : "#30d158");
        if (c && op[0] == 'c') { c->todos = json_bool(args, "todos", 0); c->events = json_bool(args, "events", !c->todos); }
        save_colls();
        pthread_mutex_unlock(&G);
        if (!c) { reply_err(id, "full", ""); return; }
        changed_event("colls");
        reply_ok_id(id, cid);
        return;
    }
    if (!strcmp(op, "cal-update") || !strcmp(op, "book-update")) {
        Coll *c = coll_get(json_str(args, "id"));
        if (!c) { pthread_mutex_unlock(&G); reply_err(id, "not-found", ""); return; }
        const char *name = json_str(args, "name"), *color = json_str(args, "color");
        if (name && *name && strlen(name) <= 150) scopy(c->name, sizeof c->name, name);
        if (valid_color(color)) scopy(c->color, sizeof c->color, color);
        if (json_get(args, "hidden")) c->hidden = json_bool(args, "hidden", 0);
        save_colls();
        pthread_mutex_unlock(&G);
        changed_event("colls");
        reply_ok_id(id, NULL);
        return;
    }
    if (!strcmp(op, "cal-delete") || !strcmp(op, "book-delete")) {
        Coll *c = coll_get(json_str(args, "id"));
        if (!c || c->acct[0]) { pthread_mutex_unlock(&G); reply_err(id, c ? "synced" : "not-found", "remove the account to remove its calendars"); return; }
        coll_remove(c);
        save_colls();
        pthread_mutex_unlock(&G);
        changed_event("colls");
        reply_ok_id(id, NULL);
        return;
    }
    if (!strcmp(op, "ev-list")) {
        events_json(&b, ms_arg(args, "from"), ms_arg(args, "to"), json_get(args, "cals"));
        pthread_mutex_unlock(&G);
        reply_done(id, &b);
        return;
    }
    if (!strcmp(op, "todo-list")) {
        todos_json(&b, json_get(args, "lists"));
        pthread_mutex_unlock(&G);
        reply_done(id, &b);
        return;
    }
    if (!strcmp(op, "ev-save") || !strcmp(op, "todo-save")) { save_event(id, args, op[0] == 't'); pthread_mutex_unlock(&G); jb_free(&b); return; }
    if (!strcmp(op, "ev-delete") || !strcmp(op, "todo-delete")) { delete_event(id, args, op[0] == 't'); pthread_mutex_unlock(&G); jb_free(&b); return; }
    if (!strcmp(op, "ev-import")) {
        /* an .ics file (fd) into a calendar */
        Coll *c = coll_get(json_str(args, "cal"));
        if (!c) c = default_calendar();
        off_t sz = nfds > 0 ? lseek(fds[0], 0, SEEK_END) : -1;
        char *t = sz > 0 && sz < (off_t)(32 << 20) ? malloc((size_t)sz + 1) : NULL;
        int added = 0;
        if (c && t && pread(fds[0], t, (size_t)sz, 0) == sz) {
            t[sz] = 0;
            IcEvent *ev;
            char method[24];
            int n = ical_events(t, (size_t)sz, &ev, method, sizeof method);
            for (int i = 0; i < n; i++) {
                IcEvent *e = &ev[i];
                if (e->recurrence_id) continue;
                IcBuild bb = { 0 };
                bb.is_todo = e->is_todo; bb.uid = e->uid[0] ? e->uid : NULL; bb.summary = e->summary; bb.location = e->location; bb.description = e->description;
                bb.start = e->start; bb.end = e->end; bb.allday = e->allday; bb.tzid = e->tzid; bb.rrule = e->rrule; bb.alarms = e->alarms; bb.nalarms = e->nalarms;
                bb.due = e->due; bb.due_allday = e->due_allday; bb.priority = e->priority; bb.completed = e->completed; bb.flagged = e->flagged;
                bb.organizer = e->organizer; bb.organizer_name = e->organizer_name; bb.att = e->att; bb.natt = e->natt; bb.sequence = e->sequence;
                char uid[256];
                if (!bb.uid) { new_uid(uid, sizeof uid); bb.uid = uid; }
                size_t ol;
                char *text = ical_build(&bb, &ol);
                if (!text) continue;
                Item *it = item_get(c, bb.uid);
                if (!it) it = item_new(c, bb.uid);
                if (it && !item_write(c, it, text, ol, 1)) added++;
                free(text);
            }
            if (n > 0) ical_events_free(ev, n);
        }
        free(t);
        pthread_mutex_unlock(&G);
        if (c) push_soon(c);
        changed_event("events");
        jb_printf(&b, "{\"ok\":true,\"added\":%d}", added);
        reply_done(id, &b);
        return;
    }
    if (!strcmp(op, "contact-list")) {
        contacts_json(&b, json_str(args, "book"));
        pthread_mutex_unlock(&G);
        reply_done(id, &b);
        return;
    }
    if (!strcmp(op, "contact-save")) { save_contact(id, args, fds, nfds); pthread_mutex_unlock(&G); jb_free(&b); return; }
    if (!strcmp(op, "contact-import")) { import_contacts(id, args, fds, nfds); pthread_mutex_unlock(&G); jb_free(&b); return; }
    if (!strcmp(op, "contact-delete")) {
        Coll *c = coll_get(json_str(args, "book"));
        Item *it = c ? item_get(c, json_str(args, "uid")) : NULL;
        if (!it) { pthread_mutex_unlock(&G); reply_err(id, "not-found", ""); return; }
        if (c->acct[0] && it->href[0]) { it->dirty = DIRTY_DEL; save_meta(c); }
        else { item_delete(c, it); save_meta(c); }
        push_soon(c);
        pthread_mutex_unlock(&G);
        changed_event("contacts");
        reply_ok_id(id, NULL);
        return;
    }
    if (!strcmp(op, "contact-photo")) {
        Coll *c = coll_get(json_str(args, "book"));
        Item *it = c ? item_get(c, json_str(args, "uid")) : NULL;
        char rel[200] = "";
        if (it) coll_dir(c, it->file, rel, sizeof rel);
        pthread_mutex_unlock(&G);
        if (!rel[0]) { reply_err(id, "not-found", ""); return; }
        size_t tl, pl;
        char *t = fs_read(rel, &tl, 16 << 20);
        unsigned char *ph = t ? vcard_photo(t, tl, &pl) : NULL;
        free(t);
        int size = (int)json_num(args, "size", 128);
        if (size < 16 || size > 1024) size = 128;
        int w, h;
        uint32_t *px = ph ? img_decode(ph, pl, size, size, &w, &h) : NULL;
        free(ph);
        if (!px) { reply_err(id, "no-photo", ""); return; }
        reply_image(id, px, w, h);
        free(px);
        jb_free(&b);
        return;
    }
    pthread_mutex_unlock(&G);
    jb_free(&b);
    reply_err(id, "unknown-op", op);
}

/* ---- CalDAV / CardDAV sync ---- */

typedef struct { char href[1024], etag[256], name[200], color[16], ctag[256]; int cal, book, vevent, vtodo, ok; char *data; } DavItem;

static int status_ok(const MxNode *propstat) {
    char st[64];
    mx_child_text(propstat, MX_NS_DAV, "status", st, sizeof st);
    return !st[0] || strstr(st, " 200") != NULL;
}

/* Every <response> of a multistatus, with the 200 props flattened into a DavItem. */
static int parse_multistatus(const char *body, size_t len, DavItem **out, int with_data) {
    *out = NULL;
    const char *err;
    MxNode *root = mx_parse(body, len, &err);
    if (!root) { dbg("DAV XML: %s", err); return -1; }
    int n = 0, cap = 0;
    DavItem *items = NULL;
    for (MxNode *r = mx_child(root, MX_NS_DAV, "response"); r; r = mx_next(r, MX_NS_DAV, "response")) {
        if (n == cap) { cap = cap ? cap * 2 : 32; DavItem *ni = realloc(items, (size_t)cap * sizeof *ni); if (!ni) break; items = ni; }
        DavItem *it = &items[n];
        memset(it, 0, sizeof *it);
        mx_child_text(r, MX_NS_DAV, "href", it->href, sizeof it->href);
        if (!it->href[0]) continue;
        for (MxNode *ps = mx_child(r, MX_NS_DAV, "propstat"); ps; ps = mx_next(ps, MX_NS_DAV, "propstat")) {
            if (!status_ok(ps)) continue;
            MxNode *p = mx_child(ps, MX_NS_DAV, "prop");
            if (!p) continue;
            it->ok = 1;
            char t[1024];
            mx_child_text(p, MX_NS_DAV, "getetag", t, sizeof t);
            if (t[0]) scopy(it->etag, sizeof it->etag, t);
            mx_child_text(p, MX_NS_DAV, "displayname", t, sizeof t);
            if (t[0]) scopy(it->name, sizeof it->name, t);
            mx_child_text(p, MX_NS_APPLE, "calendar-color", t, sizeof t);
            if (t[0] == '#' && strlen(t) >= 7) { t[7] = 0; scopy(it->color, sizeof it->color, t); }
            mx_child_text(p, MX_NS_CS, "getctag", t, sizeof t);
            if (t[0]) scopy(it->ctag, sizeof it->ctag, t);
            else { mx_child_text(p, MX_NS_DAV, "sync-token", t, sizeof t); if (t[0]) scopy(it->ctag, sizeof it->ctag, t); }
            MxNode *rt = mx_child(p, MX_NS_DAV, "resourcetype");
            if (rt && mx_child(rt, MX_NS_CAL, "calendar")) it->cal = 1;
            if (rt && mx_child(rt, MX_NS_CARD, "addressbook")) it->book = 1;
            MxNode *comps = mx_child(p, MX_NS_CAL, "supported-calendar-component-set");
            if (comps) {
                /* the component names are attributes, which our reader skips: look at the raw text */
                it->vevent = it->vtodo = 0;
            }
            if (with_data) {
                MxNode *d = mx_child(p, MX_NS_CAL, "calendar-data");
                if (!d) d = mx_child(p, MX_NS_CARD, "address-data");
                if (d && d->text) it->data = strndup(d->text, d->tlen);
            }
        }
        n++;
    }
    mx_free(root);
    *out = items;
    return n;
}

static void free_items(DavItem *it, int n) { for (int i = 0; i < n; i++) free(it[i].data); free(it); }

/* supported components: the attribute names are not kept by our XML reader, so look at the
 * raw response for this collection's block */
static void comps_from_raw(const char *body, const char *href, int *vevent, int *vtodo) {
    *vevent = 1; *vtodo = 1;
    const char *h = strstr(body, href);
    if (!h) return;
    const char *end = strstr(h, "response>");
    const char *set = strstr(h, "supported-calendar-component-set");
    if (!set || (end && set > end)) return;
    const char *close = strstr(set + 10, "supported-calendar-component-set");
    if (!close) return;
    size_t n = (size_t)(close - set);
    char *blk = strndup(set, n);
    if (!blk) return;
    *vevent = strstr(blk, "VEVENT") != NULL;
    *vtodo = strstr(blk, "VTODO") != NULL;
    free(blk);
}

static const char *PF_HOMES =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\" "
    "xmlns:card=\"urn:ietf:params:xml:ns:carddav\"><d:prop><d:current-user-principal/><c:calendar-home-set/><card:addressbook-home-set/>"
    "<d:resourcetype/></d:prop></d:propfind>";
static const char *PF_COLLS =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\" "
    "xmlns:cs=\"http://calendarserver.org/ns/\" xmlns:x=\"http://apple.com/ns/ical/\"><d:prop><d:displayname/><d:resourcetype/>"
    "<x:calendar-color/><c:supported-calendar-component-set/><cs:getctag/><d:sync-token/></d:prop></d:propfind>";
static const char *PF_ETAGS =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:getetag/></d:prop></d:propfind>";

static void sync_failed(Account *a, Dav *d, const char *what) {
    if (d->auth_failed) {
        pthread_mutex_lock(&G);
        if (a->password) { explicit_bzero(a->password, strlen(a->password)); free(a->password); a->password = NULL; }
        pthread_mutex_unlock(&G);
        JBuf b;
        jb_init(&b);
        jb_raw(&b, "{\"ev\":\"auth-failed\",\"acct\":"); jb_str(&b, a->id); jb_raw(&b, "}");
        send_event(&b);
        acct_status(a, "auth", "wrong password");
    } else acct_status(a, !strcmp(d->err, "cert") ? "cert" : "offline", d->err[0] ? d->err : what);
}

/* Finds the home of the calendars (cal) or of the address books, starting from start: the URL
 * itself, the principal it names (current-user-principal), then /.well-known/caldav or
 * /.well-known/carddav on that server (RFC 6764). The other kind's home is noted too when the
 * server gives it. Homes are absolute URLs: a provider may put them on another host of its
 * domain (pNN-caldav.<domain>), which lord-server allows for this account. 0 ok. */
static int discover(Dav *d, const char *start, int cal, char *calhome, char *cardhome, size_t cap) {
    char target[DAV_URL_MAX], first[DAV_URL_MAX];
    if (dav_href_resolve(start, start, first, sizeof first)) { snprintf(d->err, sizeof d->err, "bad URL"); return -1; }
    scopy(target, sizeof target, first);
    char seen[4][DAV_URL_MAX];
    int nseen = 0, wellknown = 0, hard = 0;
    char *want = cal ? calhome : cardhome;
    for (int step = 0; step < 6; step++) {
        if (nseen < 4) scopy(seen[nseen++], DAV_URL_MAX, target);
        HttpResp r;
        if (dav_req(d, "PROPFIND", target, 0, PF_HOMES, "application/xml; charset=utf-8", NULL, &r)) return -1;
        int st = r.status;
        DavHomes h;
        int parsed = st == 207 ? dav_parse_homes(r.body, r.blen, d->url, &h) : -1;
        http_resp_free(&r);
        if (st == 401) return -1;
        if (st >= 500 && !hard) hard = st;
        if (!parsed) {
            /* a server may move the homes only within its own site (numbered hosts of its domain):
             * the account's credentials never go to another account's server */
            if (!dav_same_site(first, h.calhome)) h.calhome[0] = 0;
            if (!dav_same_site(first, h.cardhome)) h.cardhome[0] = 0;
            if (!dav_same_site(first, h.principal)) h.principal[0] = 0;
            if (h.calhome[0] && !calhome[0]) scopy(calhome, cap, h.calhome);
            if (h.cardhome[0] && !cardhome[0]) scopy(cardhome, cap, h.cardhome);
            if (want[0]) return 0;
            if (cal ? h.is_cal : h.is_book) { if (dav_href_resolve(d->url, d->url, want, cap)) scopy(want, cap, target); return 0; }
            int again = 0;
            for (int k = 0; k < nseen; k++) if (!strcmp(seen[k], h.principal)) again = 1;
            if (h.principal[0] && !again) { scopy(target, sizeof target, h.principal); continue; }
        }
        if (!wellknown) {
            wellknown = 1;
            if (!dav_href_resolve(first, cal ? "/.well-known/caldav" : "/.well-known/carddav", target, sizeof target)) continue;
        }
        break;
    }
    if (hard) { snprintf(d->err, sizeof d->err, "PROPFIND %d", hard); return -1; }
    /* nothing advertised: the URL itself is the home */
    scopy(want, cap, first);
    return 0;
}

/* The local collection that keeps a copy of a version the server refused (created if there is
 * none): an address book, a reminders list for to-dos, a calendar for events. Holds G. */
static Coll *local_coll(char kind, int todo) {
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < ncolls; i++) {
            Coll *c = colls[i];
            if (c->acct[0] || c->kind != kind || (pass == 0 && c->hidden)) continue;
            if (kind == 'b' || (todo ? c->todos : c->events)) return c;
        }
    const char *id = kind == 'b' ? "local-contacts" : todo ? "local-reminders" : "local-calendar";
    char cid[80];
    scopy(cid, sizeof cid, id);
    for (int n = 2; coll_get(cid) && n < 50; n++) snprintf(cid, sizeof cid, "%s-%d", id, n);
    if (coll_get(cid)) return NULL;
    Coll *c = coll_new(cid, kind, "", kind == 'b' ? "Contacts" : todo ? "Reminders" : "Personal", kind == 'b' ? "#8e8e93" : todo ? "#ff9f0a" : "#0a84ff");
    if (c && kind == 'c') { c->todos = todo; c->events = !todo; }
    if (c) save_colls();
    return c;
}

/* Conflict policy: the server's version wins in the synced collection, and the local version
 * that lost is kept, with a new UID, in a collection on this computer (never uploaded), so no
 * edit is lost. Holds G. */
static void keep_local_copy(Coll *from, const char *uid, const char *text, size_t len, int status) {
    int todo = from->kind == 'c' && dav_is_todo(text, len);
    Coll *dst = local_coll(from->kind, todo);
    if (!dst) return;
    char nu[64];
    new_uid(nu, sizeof nu);
    size_t nl;
    char *nt = dav_set_uid(text, len, nu, &nl);
    if (!nt) return;
    Item *it = item_new(dst, nu);
    if (it && item_write(dst, it, nt, nl, 0)) { item_delete(dst, it); it = NULL; }
    free(nt);
    if (!it) return;
    const char *title = from->kind == 'b' ? it->fn : it->nev > 0 ? it->ev[0].summary : "";
    JBuf ev;
    jb_init(&ev);
    jb_printf(&ev, "{\"ev\":\"dav-conflict\",\"status\":%d,\"kind\":\"%s\",\"acct\":", status, from->kind == 'b' ? "contact" : todo ? "todo" : "event");
    jb_str(&ev, from->acct);
    jb_raw(&ev, ",\"uid\":"); jb_str(&ev, uid);
    jb_raw(&ev, ",\"copy\":"); jb_str(&ev, nu);
    jb_raw(&ev, ",\"coll\":"); jb_str(&ev, dst->id);
    jb_raw(&ev, ",\"collName\":"); jb_str(&ev, dst->name);
    jb_raw(&ev, ",\"title\":"); jb_str(&ev, title);
    jb_raw(&ev, "}");
    send_event(&ev);
}

typedef struct { char href[DAV_URL_MAX], etag[256], file[64], uid[256]; int dirty; } PushItem;

static void push_changes(Dav *d, Coll *c) {
    pthread_mutex_lock(&G);
    int n = 0;
    PushItem *list = malloc(sizeof *list * (size_t)(c->n ? c->n : 1));
    for (int i = 0; list && i < c->n; i++) if (c->items[i].dirty) {
        PushItem *p = &list[n++];
        /* hrefs written by earlier versions may be paths: make them absolute on the collection */
        if (dav_href_resolve(c->href, c->items[i].href, p->href, sizeof p->href) || !dav_same_origin(c->href, p->href)) { n--; continue; }
        scopy(p->etag, sizeof p->etag, c->items[i].etag);
        scopy(p->file, sizeof p->file, c->items[i].file);
        scopy(p->uid, sizeof p->uid, c->items[i].uid);
        p->dirty = c->items[i].dirty;
    }
    char cid[80];
    scopy(cid, sizeof cid, c->id);
    char kind = c->kind;
    pthread_mutex_unlock(&G);
    for (int i = 0; i < n; i++) {
        PushItem *p = &list[i];
        int del = p->dirty == DIRTY_DEL;
        char hdr[400] = "";
        if (p->etag[0]) snprintf(hdr, sizeof hdr, "If-Match: %s\r\n", p->etag);
        else if (!del) snprintf(hdr, sizeof hdr, "If-None-Match: *\r\n");
        HttpResp r;
        int rc;
        char *t = NULL;
        size_t tl = 0;
        if (del) rc = dav_req(d, "DELETE", p->href, -1, NULL, NULL, hdr, &r);
        else {
            char rel[200];
            snprintf(rel, sizeof rel, "pim/%s/%s", cid, p->file);
            t = fs_read(rel, &tl, 16 << 20);
            if (!t) continue;
            rc = dav_req(d, "PUT", p->href, -1, t, kind == 'c' ? "text/calendar; charset=utf-8" : "text/vcard; charset=utf-8", hdr, &r);
        }
        if (rc) { free(t); break; }
        int out = dav_push_outcome(del, r.status);
        pthread_mutex_lock(&G);
        Coll *cc = coll_get(cid);
        Item *it = cc ? item_get(cc, p->uid) : NULL;
        if (it && it->dirty == p->dirty) {
            if (out == DAV_PUSH_DONE) {
                if (del) item_delete(cc, it);
                else { it->dirty = CLEAN; scopy(it->href, sizeof it->href, p->href); scopy(it->etag, sizeof it->etag, r.etag); }
            } else if (out == DAV_PUSH_CONFLICT || out == DAV_PUSH_REJECTED) {
                /* the server's version wins; it is fetched below */
                it->dirty = CLEAN;
                it->etag[0] = 0;
                if (dav_push_keeps_copy(del, out) && t) keep_local_copy(cc, p->uid, t, tl, r.status);
                if (out == DAV_PUSH_REJECTED) {
                    JBuf ev;
                    jb_init(&ev);
                    jb_printf(&ev, "{\"ev\":\"dav-rejected\",\"status\":%d,\"uid\":", r.status);
                    jb_str(&ev, p->uid);
                    jb_raw(&ev, "}");
                    send_event(&ev);
                }
            } else dbg("DAV %s %s: %d (again next sync)", del ? "DELETE" : "PUT", p->href, r.status);
            save_meta(cc);
        }
        pthread_mutex_unlock(&G);
        http_resp_free(&r);
        free(t);
    }
    free(list);
}

static void escape_href(JBuf *b, const char *h) {
    char e[2200];
    mx_escape(e, sizeof e, h);
    jb_raw(b, e);
}

/* hrefs of a multistatus made absolute on base (the URL that answered); unusable ones, and items
 * outside the collection's server, emptied */
static void resolve_items(DavItem *list, int n, const char *base, const char *coll) {
    for (int i = 0; i < n; i++) {
        char abs[DAV_URL_MAX];
        if (dav_href_resolve(base, list[i].href, abs, sizeof abs) || !dav_same_origin(coll, abs)) list[i].href[0] = 0;
        else scopy(list[i].href, sizeof list[i].href, abs);
    }
}

static void fetch_changed(Dav *d, Coll *c) {
    char href[DAV_URL_MAX], cid[80];
    pthread_mutex_lock(&G);
    scopy(href, sizeof href, c->href);
    scopy(cid, sizeof cid, c->id);
    char kind = c->kind;
    /* hrefs written by earlier versions may be paths: make them absolute once */
    for (int i = 0; i < c->n; i++) {
        char abs[DAV_URL_MAX];
        if (c->items[i].href[0] && !dav_href_resolve(href, c->items[i].href, abs, sizeof abs)) scopy(c->items[i].href, sizeof c->items[i].href, abs);
    }
    pthread_mutex_unlock(&G);
    HttpResp r;
    if (dav_req(d, "PROPFIND", href, 1, PF_ETAGS, "application/xml; charset=utf-8", NULL, &r)) return;
    if (r.status != 207) { http_resp_free(&r); return; }
    char base[sizeof d->url];
    scopy(base, sizeof base, d->url);
    DavItem *list;
    int n = parse_multistatus(r.body, r.blen, &list, 0);
    http_resp_free(&r);
    if (n < 0) return;
    resolve_items(list, n, base, href);
    /* what to download; what disappeared (davutil.c, dav_plan) */
    char **want = malloc(sizeof *want * (size_t)(n ? n : 1));
    DavRemote *rem = malloc(sizeof *rem * (size_t)(n ? n : 1));
    unsigned char *fetch = malloc((size_t)(n ? n : 1));
    int nw = 0;
    pthread_mutex_lock(&G);
    Coll *cc = coll_get(cid);
    DavLocal *loc = cc ? malloc(sizeof *loc * (size_t)(cc->n ? cc->n : 1)) : NULL;
    unsigned char *drop = cc ? malloc((size_t)(cc->n ? cc->n : 1)) : NULL;
    if (!cc || !want || !rem || !fetch || !loc || !drop) {
        pthread_mutex_unlock(&G);
        free(want); free(rem); free(fetch); free(loc); free(drop); free_items(list, n);
        return;
    }
    for (int i = 0; i < n; i++) { rem[i].href = list[i].href; rem[i].etag = list[i].etag; }
    int nl = cc->n;
    for (int k = 0; k < nl; k++) { loc[k].href = cc->items[k].href; loc[k].etag = cc->items[k].etag; loc[k].dirty = cc->items[k].dirty; }
    dav_plan(href, loc, nl, rem, n, fetch, drop);
    for (int i = 0; i < n; i++) if (fetch[i]) want[nw++] = list[i].href;
    for (int k = nl - 1; k >= 0; k--) if (drop[k]) item_delete(cc, &cc->items[k]);    /* descending: later swaps only touch done slots */
    save_meta(cc);
    pthread_mutex_unlock(&G);
    free(rem); free(fetch); free(loc); free(drop);
    for (int off = 0; off < nw; off += 50) {
        JBuf q;
        jb_init(&q);
        if (kind == 'c') jb_raw(&q, "<?xml version=\"1.0\" encoding=\"utf-8\"?><c:calendar-multiget xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:prop><d:getetag/><c:calendar-data/></d:prop>");
        else jb_raw(&q, "<?xml version=\"1.0\" encoding=\"utf-8\"?><card:addressbook-multiget xmlns:d=\"DAV:\" xmlns:card=\"urn:ietf:params:xml:ns:carddav\"><d:prop><d:getetag/><card:address-data/></d:prop>");
        for (int i = off; i < nw && i < off + 50; i++) { jb_raw(&q, "<d:href>"); escape_href(&q, dav_url_path(want[i])); jb_raw(&q, "</d:href>"); }
        jb_raw(&q, kind == 'c' ? "</c:calendar-multiget>" : "</card:addressbook-multiget>");
        if (q.oom) { jb_free(&q); break; }
        HttpResp mr;
        int rc = dav_req(d, "REPORT", href, 1, q.p, "application/xml; charset=utf-8", NULL, &mr);
        jb_free(&q);
        if (rc) break;
        if (mr.status != 207) { http_resp_free(&mr); break; }
        char mbase[sizeof d->url];
        scopy(mbase, sizeof mbase, d->url);
        DavItem *got;
        int ng = parse_multistatus(mr.body, mr.blen, &got, 1);
        http_resp_free(&mr);
        if (ng > 0) resolve_items(got, ng, mbase, href);
        pthread_mutex_lock(&G);
        cc = coll_get(cid);
        for (int i = 0; cc && i < ng; i++) {
            if (!got[i].data || !got[i].href[0]) continue;
            size_t dl = strlen(got[i].data);
            char uid[256] = "";
            if (kind == 'b') vcard_uid(got[i].data, dl, uid, sizeof uid);
            else { IcEvent *ev; char m[24]; int ne = ical_events(got[i].data, dl, &ev, m, sizeof m); if (ne > 0) { scopy(uid, sizeof uid, ev[0].uid); ical_events_free(ev, ne); } }
            if (!uid[0]) scopy(uid, sizeof uid, got[i].href);
            Item *it = item_by_href(cc, got[i].href);
            if (!it) it = item_get(cc, uid);
            if (it && it->dirty) continue;              /* changed here meanwhile: pushed next sync */
            if (!it) it = item_new(cc, uid);
            if (!it) continue;
            scopy(it->href, sizeof it->href, got[i].href);
            scopy(it->etag, sizeof it->etag, got[i].etag);
            if (item_write(cc, it, got[i].data, dl, 0)) item_delete(cc, it);
        }
        if (cc) save_meta(cc);
        pthread_mutex_unlock(&G);
        if (ng > 0) free_items(got, ng);
    }
    free(want);
    free_items(list, n);
}

void pim_dav_sync(Account *a) {
    Dav d;
    int r = dav_begin(&d, a);
    if (r == -2) {
        if (!a->need_auth_sent) {
            a->need_auth_sent = 1;
            JBuf b;
            jb_init(&b);
            jb_raw(&b, "{\"ev\":\"need-auth\",\"acct\":"); jb_str(&b, a->id);
            jb_raw(&b, ",\"user\":"); jb_str(&b, a->user);
            jb_raw(&b, "}");
            send_event(&b);
        }
        acct_status(a, "auth", "no password");
        dav_end(&d);
        return;
    }
    if (r) { sync_failed(a, &d, "connect"); dav_end(&d); return; }
    /* the homes: calendars from the account URL; contacts from their own URL when the provider
     * has one (card_url), else from the same discovery */
    char calhome[DAV_URL_MAX] = "", cardhome[DAV_URL_MAX] = "";
    if (a->cal && discover(&d, a->url, 1, calhome, cardhome, sizeof calhome)) { sync_failed(a, &d, "discovery"); dav_end(&d); return; }
    if (a->card && (a->card_url[0] || !cardhome[0])) {
        char other[DAV_URL_MAX] = "";
        cardhome[0] = 0;
        if (discover(&d, a->card_url[0] ? a->card_url : a->url, 0, other, cardhome, sizeof cardhome)) {
            if (d.auth_failed || !a->cal) { sync_failed(a, &d, "discovery"); dav_end(&d); return; }
            dbg("DAV %s: no contacts (%s)", a->id, d.err);
            cardhome[0] = 0;
            d.err[0] = 0;
        }
    }
    pthread_mutex_lock(&G);
    for (int i = 0; i < ncolls; i++) if (!strcmp(colls[i]->acct, a->id)) colls[i]->seen = 0;
    pthread_mutex_unlock(&G);
    int listed[2] = { 0, 0 };
    for (int pass = 0; pass < 2; pass++) {
        const char *home = pass ? cardhome : calhome;
        if ((pass == 0 && !a->cal) || (pass == 1 && !a->card) || !home[0]) continue;
        HttpResp hr;
        if (dav_req(&d, "PROPFIND", home, 1, PF_COLLS, "application/xml; charset=utf-8", NULL, &hr)) { sync_failed(a, &d, "list"); dav_end(&d); return; }
        if (hr.status != 207) { http_resp_free(&hr); continue; }
        char hbase[sizeof d.url];
        scopy(hbase, sizeof hbase, d.url);
        DavItem *it;
        int n = parse_multistatus(hr.body, hr.blen, &it, 0);
        listed[pass] = 1;
        pthread_mutex_lock(&G);
        for (int i = 0; i < n; i++) {
            if (pass == 0 ? !it[i].cal : !it[i].book) continue;
            char abs[DAV_URL_MAX];
            if (dav_href_resolve(hbase, it[i].href, abs, sizeof abs) || !dav_same_origin(hbase, abs)) { it[i].cal = it[i].book = 0; continue; }
            /* the id comes from the href as the server writes it (as in earlier versions) */
            char h[17], cid[80];
            fs_hash(it[i].href, h);
            snprintf(cid, sizeof cid, "%s-%s", a->id, h);
            Coll *c = coll_get(cid);
            int ve = 1, vt = 1;
            if (pass == 0) comps_from_raw(hr.body, it[i].href, &ve, &vt);
            if (!c) {
                c = coll_new(cid, pass == 0 ? 'c' : 'b', a->id, it[i].name[0] ? it[i].name : (pass ? "Contacts" : "Calendar"), it[i].color[0] ? it[i].color : (pass ? "#8e8e93" : "#0a84ff"));
                if (!c) continue;
                c->ctag[0] = 0;
            } else {
                if (it[i].name[0]) scopy(c->name, sizeof c->name, it[i].name);
                if (it[i].color[0]) scopy(c->color, sizeof c->color, it[i].color);
            }
            if (pass == 0) { c->events = ve; c->todos = vt; }
            c->seen = 1;
            scopy(c->href, sizeof c->href, abs);
            /* remember the server's ctag (or sync-token); compared after pushing */
            it[i].ok = strcmp(c->ctag, it[i].ctag) || !it[i].ctag[0];
            if (it[i].ok) scopy(c->ctag, sizeof c->ctag, it[i].ctag);
        }
        pthread_mutex_unlock(&G);
        http_resp_free(&hr);
        /* sync each collection */
        for (int i = 0; i < n; i++) {
            if (pass == 0 ? !it[i].cal : !it[i].book) continue;
            char h[17], cid[80];
            fs_hash(it[i].href, h);
            snprintf(cid, sizeof cid, "%s-%s", a->id, h);
            pthread_mutex_lock(&G);
            Coll *c = coll_get(cid);
            int dirty = 0;
            for (int k = 0; c && k < c->n; k++) if (c->items[k].dirty) dirty = 1;
            pthread_mutex_unlock(&G);
            if (!c) continue;
            if (dirty) push_changes(&d, c);
            if (it[i].ok || dirty) fetch_changed(&d, c);
        }
        if (n > 0) free_items(it, n);
    }
    /* collections gone from the server */
    pthread_mutex_lock(&G);
    for (int i = ncolls - 1; i >= 0; i--) {
        Coll *c = colls[i];
        if (strcmp(c->acct, a->id) || c->seen) continue;
        if ((c->kind == 'c' && listed[0]) || (c->kind == 'b' && listed[1])) coll_remove(c);
    }
    save_colls();
    pthread_mutex_unlock(&G);
    dav_end(&d);
    a->last_sync = time(NULL);
    acct_status(a, "idle", "");
    changed_event("all");
}
