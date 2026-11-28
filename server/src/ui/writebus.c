/* writebus.c — org.lordos.Writing1 for the `lumo` command (see writebus.h) */
#include "writebus.h"
#include "../llm/engine.h"
#include "../llm/lumo.h"
#include "../llm/writing.h"
#include "../sec/json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char introspect[] =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">"
    "<node><interface name='" LUMO_BUS_IFACE "'>"
    "<method name='Status'><arg type='s' direction='out'/><arg type='s' direction='out'/><arg type='s' direction='out'/></method>"
    "<method name='Start'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='u' direction='out'/></method>"
    "<method name='Read'><arg type='u' direction='in'/><arg type='s' direction='out'/><arg type='s' direction='out'/></method>"
    "<method name='Cancel'><arg type='u' direction='in'/></method>"
    "</interface></node>";

typedef struct {
    char name[64];
    int st;                         /* 0 free, 1 asking the bus, 2 same user, 3 another user */
    uint32_t ask;                   /* GetConnectionUnixUser's serial */
    DMsg pend;                      /* the call waiting for that answer */
    int has_pend;
} Caller;
typedef struct { int id; char owner[64]; } Job;

static Caller callers[WRITEBUS_CALLERS];
static Job jobs[WRITEBUS_JOBS];
static int serve_known, serve_on;
static char serve_chosen[64];

void writebus_serve(int on, const char *chosen) {
    serve_known = 1;
    serve_on = !!on;
    snprintf(serve_chosen, sizeof serve_chosen, "%s", chosen && llm_model_id_ok(chosen) ? chosen : "");
}

/* ---- pure ---- */

int writebus_pick_model(const char *models_json, const char *chosen, char out[64]) {
    JDoc d;
    out[0] = 0;
    if (!models_json || json_parse(models_json, strlen(models_json), &d, NULL, 0)) return -1;
    const char *first = NULL;
    int found = 0;
    if (d.root && d.root->type == J_ARR)
        for (const JVal *m = d.root->kid; m; m = m->next) {
            const char *id = json_str(m, "id");
            if (!id || !llm_model_id_ok(id)) continue;
            if (!first) first = id;
            if (chosen && !strcmp(id, chosen)) found = 1;
        }
    if (found) snprintf(out, 64, "%s", chosen);
    else if (first) snprintf(out, 64, "%s", first);
    json_free(&d);
    return out[0] ? 0 : -1;
}

const char *writebus_state(const char *take_json, const char *peek_json, char **text) {
    *text = NULL;
    JDoc d;
    if (take_json) {
        if (json_parse(take_json, strlen(take_json), &d, NULL, 0)) { *text = strdup("llm.failed"); return "failed"; }
        const char *st = "failed";
        if (json_bool(d.root, "ok", 0) && json_str(d.root, "content")) { *text = strdup(json_str(d.root, "content")); st = "done"; }
        else {
            const char *e = json_str(d.root, "error");
            *text = strdup(e && strlen(e) < 48 ? e : "llm.failed");
        }
        json_free(&d);
        return st;
    }
    if (peek_json && !json_parse(peek_json, strlen(peek_json), &d, NULL, 0)) {
        const char *s = json_str(d.root, "state"), *st = "queued";
        if (s && !strcmp(s, "writing")) { const char *t = json_str(d.root, "text"); *text = strdup(t ? t : ""); st = "writing"; }
        else if (s && !strcmp(s, "starting")) st = "starting";
        json_free(&d);
        if (!*text) *text = strdup("");
        return st;
    }
    *text = strdup("");
    return "queued";
}

/* ---- the bus ---- */

static int model_now(char out[64], const char **why) {
    if (!serve_known || !serve_on) { *why = "off"; return -1; }
    if (!llm_available()) { *why = "no-engine"; return -1; }
    char *mj = llm_models_json();
    int r = writebus_pick_model(mj, serve_chosen[0] ? serve_chosen : NULL, out);
    free(mj);
    if (r) { *why = "no-model"; return -1; }
    *why = "ready";
    return 0;
}

static void status(DConn *c, const DMsg *m) {
    char model[64] = "";
    const char *why;
    model_now(model, &why);
    char *sj = llm_state_json();
    JDoc d;
    char engine[16] = "off";
    if (sj && !json_parse(sj, strlen(sj), &d, NULL, 0)) {
        const char *s = json_str(d.root, "state");
        if (s && strlen(s) < sizeof engine) snprintf(engine, sizeof engine, "%s", s);
        json_free(&d);
    }
    free(sj);
    DBuf b;
    dw_init(&b);
    dw_str(&b, why);
    dw_str(&b, model);
    dw_str(&b, engine);
    dbus_reply(c, m, "sss", &b);
    dw_free(&b);
}

static Job *job_of(const char *owner, uint32_t id) {
    for (int i = 0; i < WRITEBUS_JOBS; i++) if (jobs[i].id && (uint32_t)jobs[i].id == id && !strcmp(jobs[i].owner, owner)) return &jobs[i];
    return NULL;
}

static void start(DConn *c, const DMsg *m) {
    if (!m->sig || strcmp(m->sig, "sss")) { dbus_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "arguments"); return; }
    DRd r;
    dr_init(&r, m);
    const char *action = dr_str(&r, NULL), *target = dr_str(&r, NULL);
    uint32_t tl = 0;
    const char *text = dr_str(&r, &tl);
    int a = r.err ? 0 : wr_action(action);
    if (!a || (a == WR_TRANSLATE ? !wr_language(target) : target[0] != 0) || lumo_text_ok(text, tl)) {
        dbus_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "arguments");
        return;
    }
    char model[64];
    const char *why;
    if (model_now(model, &why)) {
        dbus_error(c, m, !strcmp(why, "off") ? LUMO_ERR_OFF : !strcmp(why, "no-engine") ? LUMO_ERR_NO_ENGINE : LUMO_ERR_NO_MODEL, why);
        return;
    }
    Job *slot = NULL;
    for (int i = 0; i < WRITEBUS_JOBS && !slot; i++) if (!jobs[i].id) slot = &jobs[i];
    if (!slot) { dbus_error(c, m, LUMO_ERR_BUSY, "busy"); return; }
    int id = llm_submit_write(model, action, a == WR_TRANSLATE ? target : "", text);
    if (id <= 0) { dbus_error(c, m, errno == EBUSY ? LUMO_ERR_BUSY : "org.freedesktop.DBus.Error.Failed", "not queued"); return; }
    slot->id = id;
    snprintf(slot->owner, sizeof slot->owner, "%s", m->sender);
    DBuf b;
    dw_init(&b);
    dw_u32(&b, (uint32_t)id);
    dbus_reply(c, m, "u", &b);
    dw_free(&b);
}

static void read_job(DConn *c, const DMsg *m) {
    if (!m->sig || strcmp(m->sig, "u")) { dbus_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "arguments"); return; }
    DRd r;
    dr_init(&r, m);
    uint32_t id = dr_u32(&r);
    Job *j = r.err ? NULL : job_of(m->sender, id);
    if (!j) { dbus_error(c, m, "org.freedesktop.DBus.Error.UnknownObject", "no such job"); return; }
    char *take = llm_take(j->id), *peek = take ? NULL : llm_peek(j->id), *text = NULL;
    const char *st = writebus_state(take, peek, &text);
    free(take);
    free(peek);
    if (!strcmp(st, "done") || !strcmp(st, "failed")) memset(j, 0, sizeof *j);
    DBuf b;
    dw_init(&b);
    dw_str(&b, st);
    dw_str(&b, text ? text : "");
    dbus_reply(c, m, "ss", &b);
    dw_free(&b);
    if (text) { memset(text, 0, strlen(text)); free(text); }
}

static void cancel(DConn *c, const DMsg *m) {
    if (!m->sig || strcmp(m->sig, "u")) { dbus_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "arguments"); return; }
    DRd r;
    dr_init(&r, m);
    uint32_t id = dr_u32(&r);
    Job *j = r.err ? NULL : job_of(m->sender, id);
    if (j) {
        llm_cancel(j->id);
        free(llm_take(j->id));
        memset(j, 0, sizeof *j);
    }
    dbus_reply(c, m, NULL, NULL);
}

static void serve(DConn *c, const DMsg *m) {
    const char *mem = m->member;
    if (!strcmp(mem, "Status")) status(c, m);
    else if (!strcmp(mem, "Start")) start(c, m);
    else if (!strcmp(mem, "Read")) read_job(c, m);
    else if (!strcmp(mem, "Cancel")) cancel(c, m);
    else dbus_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
}

static Caller *caller_of(const char *name) {
    for (int i = 0; i < WRITEBUS_CALLERS; i++) if (callers[i].st && !strcmp(callers[i].name, name)) return &callers[i];
    return NULL;
}

static void caller_drop(Caller *k) {
    if (k->has_pend) dbus_msg_free(&k->pend);
    memset(k, 0, sizeof *k);
}

void writebus_call(DConn *c, const DMsg *m) {
    if (m->member && !strcmp(m->member, "Introspect") && m->iface && !strcmp(m->iface, "org.freedesktop.DBus.Introspectable")) {
        DBuf b;
        dw_init(&b);
        dw_str(&b, introspect);
        dbus_reply(c, m, "s", &b);
        dw_free(&b);
        return;
    }
    if (!m->member || (m->iface && strcmp(m->iface, LUMO_BUS_IFACE))) { dbus_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method"); return; }
    if (!m->sender || m->sender[0] != ':' || strlen(m->sender) >= sizeof callers[0].name || (m->flags & DBUS_NO_REPLY)) {
        dbus_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "caller");
        return;
    }
    Caller *k = caller_of(m->sender);
    if (k && k->st == 2) { serve(c, m); return; }
    if (k && k->st == 3) { dbus_error(c, m, LUMO_ERR_DENIED, "another user"); return; }
    if (k) { dbus_error(c, m, LUMO_ERR_BUSY, "one call at a time"); return; }   /* still asking the bus */
    for (int i = 0; i < WRITEBUS_CALLERS && !k; i++) if (!callers[i].st) k = &callers[i];
    if (!k) { dbus_error(c, m, LUMO_ERR_BUSY, "too many callers"); return; }
    /* the caller's uid, from the bus, before its first call is served */
    DBuf b;
    dw_init(&b);
    dw_str(&b, m->sender);
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus",
               .member = "GetConnectionUnixUser", .dest = "org.freedesktop.DBus", .sig = "s" };
    uint32_t serial = dbus_send(c, &h, &b);
    dw_free(&b);
    if (!serial || dbus_msg_parse(m->raw, m->raw_len, &k->pend)) { dbus_error(c, m, "org.freedesktop.DBus.Error.Failed", "no answer from the bus"); return; }
    k->has_pend = 1;
    k->st = 1;
    k->ask = serial;
    snprintf(k->name, sizeof k->name, "%s", m->sender);
}

int writebus_reply(DConn *c, const DMsg *m) {
    for (int i = 0; i < WRITEBUS_CALLERS; i++) {
        Caller *k = &callers[i];
        if (k->st != 1 || k->ask != m->reply_serial) continue;
        uint32_t uid = (uint32_t)-1;
        if (m->type == DBUS_RETURN && m->sig && !strcmp(m->sig, "u")) { DRd r; dr_init(&r, m); uid = dr_u32(&r); if (r.err) uid = (uint32_t)-1; }
        k->st = uid == (uint32_t)getuid() ? 2 : 3;
        if (k->has_pend) {
            if (k->st == 2) serve(c, &k->pend);
            else dbus_error(c, &k->pend, LUMO_ERR_DENIED, "another user");
            dbus_msg_free(&k->pend);
            k->has_pend = 0;
        }
        return 1;
    }
    return 0;
}

void writebus_gone(const char *name) {
    for (int i = 0; i < WRITEBUS_JOBS; i++)
        if (jobs[i].id && !strcmp(jobs[i].owner, name)) { llm_cancel(jobs[i].id); free(llm_take(jobs[i].id)); memset(&jobs[i], 0, sizeof jobs[i]); }
    Caller *k = caller_of(name);
    if (k) caller_drop(k);
}

void writebus_lost(void) {
    for (int i = 0; i < WRITEBUS_JOBS; i++) if (jobs[i].id) { llm_cancel(jobs[i].id); memset(&jobs[i], 0, sizeof jobs[i]); }
    for (int i = 0; i < WRITEBUS_CALLERS; i++) caller_drop(&callers[i]);
}
