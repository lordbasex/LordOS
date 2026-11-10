/* castsm.c — the ScreenCast portal's sessions (see castsm.h). */
#include "castsm.h"

#include <stdio.h>
#include <string.h>

void csm_init(CastSm *m) { memset(m, 0, sizeof *m); }

void csm_path(const char *obj, const char *what, const char *sender, const char *token, int id, char *out, unsigned n) {
    char who[CSM_SENDER_MAX] = "", tok[65] = "";
    size_t w = 0;
    for (const char *s = sender + (sender[0] == ':'); *s && w + 1 < sizeof who; s++) who[w++] = *s == '.' ? '_' : *s;
    size_t t = 0;
    for (const char *s = token ? token : ""; *s && t + 1 < sizeof tok; s++)
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_') tok[t++] = *s;
    if (!t) snprintf(tok, sizeof tok, "lordos%d", id);
    snprintf(out, n, "%s/%s/%s/%s", obj, what, who, tok);
}

CsSess *csm_by_id(CastSm *m, int id) {
    for (int i = 0; id > 0 && i < CSM_MAX; i++) if (m->s[i].st != CS_FREE && m->s[i].id == id) return &m->s[i];
    return NULL;
}

CsSess *csm_find(CastSm *m, const char *sender, const char *path) {
    if (!sender || !path) return NULL;
    for (int i = 0; i < CSM_MAX; i++)
        if (m->s[i].st != CS_FREE && !strcmp(m->s[i].path, path) && !strcmp(m->s[i].sender, sender)) return &m->s[i];
    return NULL;
}

CsSess *csm_choosing(CastSm *m) {
    for (int i = 0; i < CSM_MAX; i++) if (m->s[i].st == CS_CHOOSING) return &m->s[i];
    return NULL;
}

int csm_create(CastSm *m, const char *sender, const char *token, CsSess **out) {
    if (!sender || !sender[0] || strlen(sender) >= CSM_SENDER_MAX) return CSM_E_ARGS;
    int mine = 0;
    CsSess *free_one = NULL;
    for (int i = 0; i < CSM_MAX; i++) {
        if (m->s[i].st == CS_FREE) { if (!free_one) free_one = &m->s[i]; }
        else mine += !strcmp(m->s[i].sender, sender);
    }
    if (!free_one || mine >= CSM_PER_APP) return CSM_E_FULL;
    if (++m->seq <= 0) m->seq = 1;
    char path[CSM_PATH_MAX];
    csm_path("/org/freedesktop/portal/desktop", "session", sender, token, m->seq, path, sizeof path);
    for (int i = 0; i < CSM_MAX; i++)       /* the same token twice: one object can't be two sessions */
        if (m->s[i].st != CS_FREE && !strcmp(m->s[i].path, path)) return CSM_E_ARGS;
    memset(free_one, 0, sizeof *free_one);
    free_one->id = m->seq;
    free_one->st = CS_CREATED;
    snprintf(free_one->sender, sizeof free_one->sender, "%s", sender);
    snprintf(free_one->path, sizeof free_one->path, "%s", path);
    *out = free_one;
    return CSM_OK;
}

int csm_select(CastSm *m, CsSess *s, uint32_t types, int multiple, uint32_t cursor, uint32_t persist) {
    (void)m;
    if (s->st != CS_CREATED) return CSM_E_STATE;            /* sources are chosen once, before Start */
    if (!types) types = CSM_TYPE_MONITOR;
    if (types & ~(uint32_t)CSM_TYPES) return CSM_E_ARGS;
    if (!cursor) cursor = CSM_CURSOR_HIDDEN;
    /* one mode, and one we offer (AvailableCursorModes) */
    if ((cursor & (cursor - 1)) || (cursor & ~(uint32_t)CSM_CURSORS)) return CSM_E_ARGS;
    if (persist > 2) return CSM_E_ARGS;
    s->types = types;
    s->multiple = !!multiple;
    s->cursor = cursor;
    s->persist = persist;
    s->st = CS_SELECTED;
    return CSM_OK;
}

int csm_start(CastSm *m, CsSess *s, const char *req) {
    if (s->st != CS_SELECTED) return CSM_E_STATE;
    if (!req || !req[0] || strlen(req) >= CSM_PATH_MAX) return CSM_E_ARGS;
    if (csm_choosing(m)) return CSM_E_BUSY;                  /* one question on screen at a time */
    snprintf(s->req, sizeof s->req, "%s", req);
    s->st = CS_CHOOSING;
    return CSM_OK;
}

int csm_chosen(CastSm *m, CsSess *s, const uint32_t *kinds, int n) {
    (void)m;
    if (s->st != CS_CHOOSING) return CSM_E_STATE;
    if (n == 0) { s->st = CS_SELECTED; s->req[0] = 0; return CSM_OK; }
    if (n < 0 || n > CSM_MAX_SOURCES || (n > 1 && !s->multiple)) return CSM_E_ARGS;
    for (int i = 0; i < n; i++)
        if ((kinds[i] != CSM_TYPE_MONITOR && kinds[i] != CSM_TYPE_WINDOW) || !(kinds[i] & s->types)) return CSM_E_ARGS;
    s->st = CS_STARTING;
    return CSM_OK;
}

int csm_started(CsSess *s) {
    if (s->st != CS_STARTING) return CSM_E_STATE;
    s->st = CS_STARTED;
    s->req[0] = 0;
    return CSM_OK;
}

int csm_failed(CsSess *s) {
    if (s->st != CS_STARTING) return CSM_E_STATE;
    s->st = CS_SELECTED;
    s->req[0] = 0;
    return CSM_OK;
}

int csm_request_close(CastSm *m, const char *sender, const char *req, CsSess **out) {
    if (!sender || !req) return CSM_E_NOTFOUND;
    for (int i = 0; i < CSM_MAX; i++) {
        CsSess *s = &m->s[i];
        if ((s->st != CS_CHOOSING && s->st != CS_STARTING) || strcmp(s->req, req) || strcmp(s->sender, sender)) continue;
        s->st = CS_SELECTED;
        s->req[0] = 0;
        *out = s;
        return CSM_OK;
    }
    return CSM_E_NOTFOUND;
}

int csm_may_remote(const CsSess *s) { return s && s->st == CS_STARTED; }

void csm_end(CsSess *s) { memset(s, 0, sizeof *s); }

int csm_gone(CastSm *m, const char *sender, int *ids, int max) {
    int n = 0;
    for (int i = 0; sender && i < CSM_MAX && n < max; i++)
        if (m->s[i].st != CS_FREE && !strcmp(m->s[i].sender, sender)) ids[n++] = m->s[i].id;
    return n;
}
