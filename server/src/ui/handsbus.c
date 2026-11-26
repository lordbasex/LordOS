/* handsbus.c — org.lordos.Hands1 for lordos-mcp (see handsbus.h, docs/HANDS.md) */
#include "handsbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char introspect[] =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">"
    "<node><interface name='" HANDS_BUS_IFACE "'>"
    "<method name='Hello'><arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='Status'><arg type='s' direction='out'/></method>"
    "<method name='Windows'><arg type='s' direction='out'/></method>"
    "<method name='ReadWindow'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='Find'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='Pointer'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='Scroll'><arg type='s' direction='in'/><arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='s' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='Key'><arg type='s' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='Type'><arg type='s' direction='in'/></method>"
    "<method name='SetValue'><arg type='s' direction='in'/><arg type='s' direction='in'/></method>"
    "<method name='Screenshot'><arg type='s' direction='in'/><arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='u' direction='in'/><arg type='ay' direction='out'/><arg type='s' direction='out'/></method>"
    "<method name='OpenApp'><arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='OpenPath'><arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='CloseWindow'><arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='AppLog'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='s' direction='out'/></method>"
    "</interface></node>";

/* ---- the arguments ---- */

static const struct { const char *member, *sig; int call; } METHODS[] = {
    { "Hello", "s", HANDS_C_HELLO }, { "Status", "", HANDS_C_STATUS }, { "Windows", "", HANDS_C_WINDOWS },
    { "ReadWindow", "ssu", HANDS_C_READ }, { "Find", "ss", HANDS_C_FIND }, { "Pointer", "ssiiii", HANDS_C_POINTER },
    { "Scroll", "siisu", HANDS_C_SCROLL }, { "Key", "su", HANDS_C_KEY }, { "Type", "s", HANDS_C_TYPE },
    { "SetValue", "ss", HANDS_C_SETVALUE }, { "Screenshot", "siiiiu", HANDS_C_SHOT }, { "OpenApp", "s", HANDS_C_OPENAPP },
    { "OpenPath", "s", HANDS_C_OPENPATH }, { "CloseWindow", "s", HANDS_C_CLOSE }, { "AppLog", "ssu", HANDS_C_APPLOG },
};

#define COORD_MAX 65536

static int coord_ok(int32_t v) { return v >= -1 && v < COORD_MAX; }

/* a window id: "" (the one in front) or hands_id_ok */
static int win_ok(const char *s) { return s && (!s[0] || hands_id_ok(s)); }

int handsbus_parse(const DMsg *m, HandsReq *r) {
    memset(r, 0, sizeof *r);
    r->call = -1;
    if (!m || !m->member) return -1;
    const char *sig = m->sig ? m->sig : "";
    for (size_t i = 0; i < sizeof METHODS / sizeof METHODS[0]; i++)
        if (!strcmp(m->member, METHODS[i].member)) {
            if (strcmp(sig, METHODS[i].sig)) return -1;
            r->call = METHODS[i].call;
        }
    if (r->call < 0) return -1;
    DRd d;
    dr_init(&d, m);
    uint32_t len = 0;
    const char *s1, *s2;
    int32_t x, y, x2, y2;
    uint32_t u;
    switch (r->call) {
    case HANDS_C_HELLO:
        s1 = dr_str(&d, &len);
        if (d.err || !hands_text_ok(s1, len, 256, 0)) return -1;
        r->text = (char *)s1;
        r->text_len = len;
        return 0;
    case HANDS_C_STATUS:
    case HANDS_C_WINDOWS:
        return 0;
    case HANDS_C_READ:
        s1 = dr_str(&d, NULL);
        s2 = dr_str(&d, NULL);
        u = dr_u32(&d);
        if (d.err || !win_ok(s1) || (r->filter = hands_filter(s2)) < 0 || u > HANDS_DEPTH_MAX) return -1;
        snprintf(r->window, sizeof r->window, "%s", s1);
        r->depth = u ? (int)u : 30;
        return 0;
    case HANDS_C_FIND:
        s1 = dr_str(&d, NULL);
        s2 = dr_str(&d, &len);
        if (d.err || !win_ok(s1) || !len || !hands_text_ok(s2, len, HANDS_QUERY_MAX, 0)) return -1;
        snprintf(r->window, sizeof r->window, "%s", s1);
        r->text = (char *)s2;
        r->text_len = len;
        return 0;
    case HANDS_C_POINTER:
        s1 = dr_str(&d, NULL);
        s2 = dr_str(&d, NULL);
        x = dr_i32(&d); y = dr_i32(&d); x2 = dr_i32(&d); y2 = dr_i32(&d);
        if (d.err || !(r->action = hands_pointer_action(s1)) || !hands_ref_ok(s2)) return -1;
        if (!coord_ok(x) || !coord_ok(y) || !coord_ok(x2) || !coord_ok(y2)) return -1;
        if (!s2[0] && (x < 0 || y < 0)) return -1;                 /* a ref or a point */
        if (r->action == HANDS_P_DRAG && (x2 < 0 || y2 < 0)) return -1;
        snprintf(r->ref, sizeof r->ref, "%s", s2);
        r->x = x; r->y = y; r->x2 = x2; r->y2 = y2;
        return 0;
    case HANDS_C_SCROLL:
        s1 = dr_str(&d, NULL);
        x = dr_i32(&d); y = dr_i32(&d);
        s2 = dr_str(&d, NULL);
        u = dr_u32(&d);
        if (d.err || !hands_ref_ok(s1) || !coord_ok(x) || !coord_ok(y) || (!s1[0] && (x < 0 || y < 0))) return -1;
        if (!(r->dir = hands_direction(s2)) || u > HANDS_SCROLL_MAX) return -1;
        snprintf(r->ref, sizeof r->ref, "%s", s1);
        r->x = x; r->y = y;
        r->amount = (int)u;
        return 0;
    case HANDS_C_KEY: {
        s1 = dr_str(&d, &len);
        u = dr_u32(&d);
        HandsChord c[8];
        int n = 0;
        if (d.err || u > HANDS_REPEAT_MAX || hands_chords(s1, c, 8, &n)) return -1;
        r->text = (char *)s1;
        r->text_len = len;
        r->repeat = (int)u;
        return 0;
    }
    case HANDS_C_TYPE:
        s1 = dr_str(&d, &len);
        if (d.err || !len || !hands_text_ok(s1, len, HANDS_TEXT_MAX, 1)) return -1;
        r->text = (char *)s1;
        r->text_len = len;
        return 0;
    case HANDS_C_SETVALUE:
        s1 = dr_str(&d, NULL);
        s2 = dr_str(&d, &len);
        if (d.err || !s1[0] || !hands_ref_ok(s1) || !hands_text_ok(s2, len, HANDS_VALUE_MAX, 1)) return -1;
        snprintf(r->ref, sizeof r->ref, "%s", s1);
        r->text = (char *)s2;
        r->text_len = len;
        return 0;
    case HANDS_C_SHOT:
        s1 = dr_str(&d, NULL);
        x = dr_i32(&d); y = dr_i32(&d); x2 = dr_i32(&d); y2 = dr_i32(&d);
        u = dr_u32(&d);
        if (d.err || !(r->shot = hands_shot_target(s1, &r->display, r->window, sizeof r->window))) return -1;
        if (x < 0 || y < 0 || x2 < 0 || y2 < 0 || x >= COORD_MAX || y >= COORD_MAX || x2 >= COORD_MAX || y2 >= COORD_MAX) return -1;
        if (r->shot == HANDS_SHOT_REGION && (x2 < 1 || y2 < 1)) return -1;
        if (u && (u < 64 || u > 2560)) return -1;
        r->x = x; r->y = y; r->w = x2; r->h = y2;
        r->maxdim = u ? (int)u : HANDS_SHOT_MAXDIM;
        return 0;
    case HANDS_C_OPENAPP:
        s1 = dr_str(&d, NULL);
        if (d.err || !hands_name_ok(s1)) return -1;
        snprintf(r->app, sizeof r->app, "%s", s1);
        return 0;
    case HANDS_C_OPENPATH:
        s1 = dr_str(&d, &len);
        if (d.err || !hands_path_ok(s1)) return -1;
        r->text = (char *)s1;
        r->text_len = len;
        return 0;
    case HANDS_C_CLOSE:
        s1 = dr_str(&d, NULL);
        if (d.err || !hands_id_ok(s1)) return -1;
        snprintf(r->window, sizeof r->window, "%s", s1);
        return 0;
    case HANDS_C_APPLOG:
        s1 = dr_str(&d, NULL);
        s2 = dr_str(&d, &len);
        u = dr_u32(&d);
        if (d.err || !hands_name_ok(s1) || !hands_text_ok(s2, len, HANDS_QUERY_MAX, 0) || u > HANDS_LOG_MAX) return -1;
        snprintf(r->app, sizeof r->app, "%s", s1);
        r->text = (char *)s2;
        r->text_len = len;
        r->amount = u ? (int)u : 100;
        return 0;
    }
    return -1;
}

/* ---- the callers ---- */

typedef struct {
    char name[64];
    int st;                         /* 0 free, 1 asking the bus, 2 same user, 3 another user */
    uint32_t ask;
    DMsg pend;
    int has_pend;
} Caller;
static Caller callers[HANDSBUS_CALLERS];

static const char *err_name(int rc) {
    switch (rc) {
    case HANDS_E_OFF: return HANDS_ERR_OFF;
    case HANDS_E_ASKING: return HANDS_ERR_ASKING;
    case HANDS_E_DENIED: return HANDS_ERR_DENIED;
    case HANDS_E_STOPPED: return HANDS_ERR_STOPPED;
    case HANDS_E_ACTIVE: return HANDS_ERR_ACTIVE;
    case HANDS_E_REFUSED: return HANDS_ERR_REFUSED;
    case HANDS_E_RATE: return HANDS_ERR_RATE;
    case HANDS_E_NOTFOUND: return HANDS_ERR_NOTFOUND;
    case HANDS_E_ARGS: return HANDS_ERR_ARGS;
    case HANDS_E_BUSY: return HANDS_ERR_BUSY;
    default: return HANDS_ERR_FAILED;
    }
}

static void serve(DConn *c, const DMsg *m) {
    HandsReq r;
    if (handsbus_parse(m, &r)) {
        int known = 0;
        for (size_t i = 0; i < sizeof METHODS / sizeof METHODS[0]; i++) if (!strcmp(m->member, METHODS[i].member)) known = 1;
        dbus_error(c, m, known ? HANDS_ERR_ARGS : "org.freedesktop.DBus.Error.UnknownMethod", known ? "arguments" : "unknown method");
        return;
    }
    DBuf b;
    dw_init(&b);
    if (r.call == HANDS_C_HELLO) {
        int rc = hands_hello(m->sender, r.text);
        if (rc == HANDS_OK || rc == HANDS_E_ASKING) {
            dw_str(&b, rc == HANDS_OK ? "granted" : "asking");
            dbus_reply(c, m, "s", &b);
        } else dbus_error(c, m, err_name(rc), "hello");
        dw_free(&b);
        return;
    }
    char *out = NULL;
    uint8_t *png = NULL;
    size_t plen = 0;
    int why = 0;
    int rc = hands_call(m->sender, &r, &out, &png, &plen, &why);
    if (rc) dbus_error(c, m, err_name(rc), rc == HANDS_E_REFUSED ? hands_why_word(why) : hands_call_word(r.call));
    else if (r.call == HANDS_C_SHOT) {
        dw_bytes(&b, png, plen);
        dw_str(&b, out ? out : "{}");
        dbus_reply(c, m, "ays", &b);
    } else if (r.call == HANDS_C_SCROLL || r.call == HANDS_C_KEY || r.call == HANDS_C_TYPE || r.call == HANDS_C_SETVALUE) {
        dbus_reply(c, m, NULL, NULL);
    } else {
        dw_str(&b, out ? out : "{}");
        dbus_reply(c, m, "s", &b);
    }
    dw_free(&b);
    free(out);
    free(png);
}

static Caller *caller_of(const char *name) {
    for (int i = 0; i < HANDSBUS_CALLERS; i++) if (callers[i].st && !strcmp(callers[i].name, name)) return &callers[i];
    return NULL;
}

static void caller_drop(Caller *k) {
    if (k->has_pend) dbus_msg_free(&k->pend);
    memset(k, 0, sizeof *k);
}

void handsbus_call(DConn *c, const DMsg *m) {
    if (m->member && !strcmp(m->member, "Introspect") && m->iface && !strcmp(m->iface, "org.freedesktop.DBus.Introspectable")) {
        DBuf b;
        dw_init(&b);
        dw_str(&b, introspect);
        dbus_reply(c, m, "s", &b);
        dw_free(&b);
        return;
    }
    if (!m->member || (m->iface && strcmp(m->iface, HANDS_BUS_IFACE))) { dbus_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method"); return; }
    if (!m->sender || m->sender[0] != ':' || strlen(m->sender) >= sizeof callers[0].name || (m->flags & DBUS_NO_REPLY)) {
        dbus_error(c, m, HANDS_ERR_ARGS, "caller");
        return;
    }
    Caller *k = caller_of(m->sender);
    if (k && k->st == 2) { serve(c, m); return; }
    if (k && k->st == 3) { dbus_error(c, m, HANDS_ERR_ACCESS, "another user"); return; }
    if (k) { dbus_error(c, m, HANDS_ERR_BUSY, "one call at a time"); return; }   /* still asking the bus */
    for (int i = 0; i < HANDSBUS_CALLERS && !k; i++) if (!callers[i].st) k = &callers[i];
    if (!k) { dbus_error(c, m, HANDS_ERR_BUSY, "too many callers"); return; }
    /* the caller's uid, from the bus, before its first call is served */
    DBuf b;
    dw_init(&b);
    dw_str(&b, m->sender);
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus",
               .member = "GetConnectionUnixUser", .dest = "org.freedesktop.DBus", .sig = "s" };
    uint32_t serial = dbus_send(c, &h, &b);
    dw_free(&b);
    if (!serial || dbus_msg_parse(m->raw, m->raw_len, &k->pend)) { dbus_error(c, m, HANDS_ERR_FAILED, "no answer from the bus"); return; }
    k->has_pend = 1;
    k->st = 1;
    k->ask = serial;
    snprintf(k->name, sizeof k->name, "%s", m->sender);
}

int handsbus_reply(DConn *c, const DMsg *m) {
    for (int i = 0; i < HANDSBUS_CALLERS; i++) {
        Caller *k = &callers[i];
        if (k->st != 1 || k->ask != m->reply_serial) continue;
        uint32_t uid = (uint32_t)-1;
        if (m->type == DBUS_RETURN && m->sig && !strcmp(m->sig, "u")) { DRd r; dr_init(&r, m); uid = dr_u32(&r); if (r.err) uid = (uint32_t)-1; }
        k->st = uid == (uint32_t)getuid() ? 2 : 3;
        if (k->has_pend) {
            if (k->st == 2) serve(c, &k->pend);
            else dbus_error(c, &k->pend, HANDS_ERR_ACCESS, "another user");
            dbus_msg_free(&k->pend);
            k->has_pend = 0;
        }
        return 1;
    }
    return 0;
}

void handsbus_gone(const char *name) {
    Caller *k = caller_of(name);
    if (k) caller_drop(k);
    hands_gone(name);
}

void handsbus_lost(void) {
    for (int i = 0; i < HANDSBUS_CALLERS; i++) caller_drop(&callers[i]);
    hands_lost();
}
