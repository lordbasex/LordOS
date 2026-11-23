/* xsstore.c — sessions of xdg-session-management as plain data and text (xsstore.h). */
#include "xsstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int xs_id_ok(const char *id) {
    if (!id || strlen(id) != XS_ID_LEN) return 0;
    for (const char *p = id; *p; p++) if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return 0;
    return 1;
}

static int printable(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p < 0x20 || *p == 0x7f) return 0;
    return 1;
}

int xs_owner_ok(const char *o) {
    if (!o || !o[0] || strlen(o) >= XS_OWNER_MAX || !printable(o)) return 0;
    return !strncmp(o, "program:/", 9) || (!strncmp(o, "flatpak:", 8) && o[8]) || (!strncmp(o, "app:", 4) && o[4]);
}

int xs_name_ok(const char *n) { return n && strlen(n) <= XS_NAME_MAX && printable(n); }

void xs_new_id(const unsigned char rnd[16], char out[XS_ID_LEN + 1]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[2 * i] = hex[rnd[i] >> 4]; out[2 * i + 1] = hex[rnd[i] & 15]; }
    out[XS_ID_LEN] = 0;
}

XsSess *xs_find(XsStore *st, const char *id, const char *owner) {
    if (!owner || !owner[0] || !xs_id_ok(id)) return NULL;
    for (int i = 0; i < st->n; i++) if (st->s[i]->persist && !strcmp(st->s[i]->id, id) && !strcmp(st->s[i]->owner, owner)) return st->s[i];
    return NULL;
}

void xs_drop(XsStore *st, XsSess *s) {
    for (int i = 0; i < st->n; i++)
        if (st->s[i] == s) {
            free(s);
            memmove(&st->s[i], &st->s[i + 1], (size_t)(st->n - i - 1) * sizeof *st->s);
            st->n--;
            return;
        }
}

void xs_clear(XsStore *st) {
    for (int i = 0; i < st->n; i++) free(st->s[i]);
    st->n = 0;
}

/* The least recently used session not in use (of that owner when owner is given), or NULL. */
static XsSess *lru(XsStore *st, const char *owner) {
    XsSess *best = NULL;
    for (int i = 0; i < st->n; i++) {
        XsSess *s = st->s[i];
        if (s->inuse || (owner && strcmp(s->owner, owner))) continue;
        if (!best || s->used < best->used) best = s;
    }
    return best;
}

XsSess *xs_new(XsStore *st, const char *id, const char *owner, long long now) {
    if (!xs_id_ok(id)) return NULL;
    int persist = xs_owner_ok(owner);
    if (!persist) owner = "";           /* every app LordOS can't tell shares one owner's limit */
    int mine = 0;
    for (int i = 0; i < st->n; i++) if (!strcmp(st->s[i]->owner, owner)) mine++;
    if (mine >= XS_PER_OWNER) { XsSess *o = lru(st, owner); if (!o) return NULL; xs_drop(st, o); }
    if (st->n >= XS_SESSIONS) { XsSess *o = lru(st, NULL); if (!o) return NULL; xs_drop(st, o); }
    XsSess *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    memcpy(s->id, id, XS_ID_LEN + 1);
    snprintf(s->owner, sizeof s->owner, "%s", persist ? owner : "");
    s->used = now;
    s->persist = persist;
    st->s[st->n++] = s;
    return s;
}

XsTop *xs_top(XsSess *s, const char *name) {
    for (int i = 0; s && name && i < s->ntop; i++) if (!strcmp(s->top[i].name, name)) return &s->top[i];
    return NULL;
}
XsTop *xs_top_put(XsSess *s, const char *name) {
    if (!s || !xs_name_ok(name)) return NULL;
    XsTop *t = xs_top(s, name);
    if (t) return t;
    if (s->ntop >= XS_TOPS) return NULL;
    t = &s->top[s->ntop++];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    return t;
}
void xs_top_del(XsSess *s, const char *name) {
    XsTop *t = xs_top(s, name);
    if (!t) return;
    int i = (int)(t - s->top);
    memmove(&s->top[i], &s->top[i + 1], (size_t)(s->ntop - i - 1) * sizeof *s->top);
    s->ntop--;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
void xs_top_set(XsTop *t, const int r[4], int max) {
    t->r[0] = clampi(r[0], -XS_COORD, XS_COORD);
    t->r[1] = clampi(r[1], -XS_COORD, XS_COORD);
    t->r[2] = clampi(r[2], 1, XS_COORD);
    t->r[3] = clampi(r[3], 1, XS_COORD);
    t->max = !!max;
}
int xs_top_has(const XsTop *t) { return t && t->r[2] > 0 && t->r[3] > 0; }
int xs_top_note(XsTop *t, const int r[4], int maximized, int fullscreen) {
    if (!t) return 0;
    int was[4] = { t->r[0], t->r[1], t->r[2], t->r[3] }, wasmax = t->max;
    if (!fullscreen && !maximized) xs_top_set(t, r, 0);
    else {
        if (!xs_top_has(t)) xs_top_set(t, r, wasmax);
        if (maximized && !fullscreen) t->max = 1;
    }
    return memcmp(was, t->r, sizeof was) || wasmax != t->max;
}

char *xs_dump(const XsStore *st, size_t *len) {
    size_t cap = 64, o = 0;
    for (int i = 0; i < st->n; i++) cap += 96 + XS_OWNER_MAX + (size_t)st->s[i]->ntop * (XS_NAME_MAX + 80);
    char *b = malloc(cap);
    if (!b) return NULL;
    o += (size_t)snprintf(b + o, cap - o, "lordos-wl-sessions 1\n");
    for (int i = 0; i < st->n; i++) {
        const XsSess *s = st->s[i];
        if (!s->persist) continue;
        o += (size_t)snprintf(b + o, cap - o, "S %s %lld %s\n", s->id, s->used, s->owner);
        for (int k = 0; k < s->ntop; k++) {
            const XsTop *t = &s->top[k];
            if (!xs_top_has(t)) continue;
            o += (size_t)snprintf(b + o, cap - o, "T %d %d %d %d %d %s\n", t->r[0], t->r[1], t->r[2], t->r[3], t->max, t->name);
        }
    }
    *len = o;
    return b;
}

/* "<int> " at *p, within ±XS_COORD: 1 and *p past it */
static int num(const char **p, int *v) {
    char *e;
    long x = strtol(*p, &e, 10);
    if (e == *p || *e != ' ' || x < -XS_COORD || x > XS_COORD) return 0;
    *v = (int)x;
    *p = e + 1;
    return 1;
}

int xs_load(XsStore *st, const char *buf, size_t len) {
    xs_clear(st);
    if (!buf || len > XS_FILE_MAX) return 0;
    char *text = malloc(len + 1);
    if (!text) return 0;
    memcpy(text, buf, len);
    text[len] = 0;
    if (strlen(text) != len) { free(text); return 0; }       /* a NUL inside: not ours */
    char *save = NULL, *line = strtok_r(text, "\n", &save);
    if (!line || strcmp(line, "lordos-wl-sessions 1")) { free(text); return 0; }
    XsSess *cur = NULL;
    while ((line = strtok_r(NULL, "\n", &save))) {
        if (line[0] == 'S' && line[1] == ' ') {
            cur = NULL;
            char id[XS_ID_LEN + 1];
            const char *p = line + 2;
            if (strlen(p) < XS_ID_LEN + 2 || p[XS_ID_LEN] != ' ') continue;
            memcpy(id, p, XS_ID_LEN);
            id[XS_ID_LEN] = 0;
            p += XS_ID_LEN + 1;
            char *e;
            long long used = strtoll(p, &e, 10);
            if (e == p || *e != ' ' || used < 0) continue;
            const char *owner = e + 1;
            if (!xs_id_ok(id) || !xs_owner_ok(owner) || xs_find(st, id, owner)) continue;
            cur = xs_new(st, id, owner, used);
        } else if (line[0] == 'T' && line[1] == ' ' && cur) {
            const char *p = line + 2;
            int r[4], m;
            if (!num(&p, &r[0]) || !num(&p, &r[1]) || !num(&p, &r[2]) || !num(&p, &r[3]) || !num(&p, &m)) continue;
            if (r[2] <= 0 || r[3] <= 0 || (m != 0 && m != 1) || xs_top(cur, p)) continue;
            XsTop *t = xs_top_put(cur, p);
            if (t) xs_top_set(t, r, m);
        }
    }
    free(text);
    return st->n;
}
