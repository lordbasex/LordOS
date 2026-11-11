/* webask.c — see webask.h. */
#include "webask.h"
#include "../hw/hw.h"
#include "../web/webmgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int used, id, shown;
    uint32_t serial;                 /* the Ask call, answered later */
    uint32_t pid_serial;             /* our GetConnectionUnixProcessID about its sender */
    char sender[256];
    char app[WEB_ID_MAX + 1], profile[WEB_ID_MAX + 1];
    int kind;
} Ask;

static Ask asks[WEBASK_MAX];
static int next_id = 1;
static DConn *bus;

static const char introspect[] =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"" WEBASK_IFACE "\">\n"
    "    <method name=\"Ask\"><arg type=\"s\" name=\"app\" direction=\"in\"/><arg type=\"s\" name=\"profile\" direction=\"in\"/>"
    "<arg type=\"s\" name=\"kind\" direction=\"in\"/><arg type=\"s\" name=\"answer\" direction=\"out\"/></method>\n"
    "  </interface>\n"
    "</node>\n";

static void send_answer(Ask *a, const char *value) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, value);
    DHdr h = { .type = DBUS_RETURN, .reply_serial = a->serial, .dest = a->sender, .sig = "s" };
    if (bus) { dbus_send(bus, &h, &b); dbus_flush(bus); }
    dw_free(&b);
}

static void send_error(Ask *a, const char *name, const char *text) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, text);
    DHdr h = { .type = DBUS_ERROR, .reply_serial = a->serial, .dest = a->sender, .error = name, .sig = "s" };
    if (bus) { dbus_send(bus, &h, &b); dbus_flush(bus); }
    dw_free(&b);
}

static void event(const char *kind, const Ask *a) {
    char j[256];
    snprintf(j, sizeof j, "{\"id\":%d,\"app\":\"%s\",\"profile\":\"%s\",\"kind\":\"%s\"}", a->id, a->app, a->profile, webreg_perm_name(a->kind));
    hw_emit(kind, j);
}

static void drop(Ask *a, int tell) {
    if (tell && a->shown) event("webapps-ask-gone", a);
    memset(a, 0, sizeof *a);
}

void webask_call(DConn *c, const DMsg *m) {
    bus = c;
    if (!strcmp(m->member, "Introspect")) {
        DBuf b;
        dw_init(&b);
        dw_str(&b, introspect);
        dbus_reply(c, m, "s", &b);
        dw_free(&b);
        return;
    }
    if (!m->iface || strcmp(m->iface, WEBASK_IFACE) || strcmp(m->member, "Ask")) {
        dbus_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
        return;
    }
    if (!m->sig || strcmp(m->sig, "sss") || !m->sender || strlen(m->sender) >= sizeof asks[0].sender || (m->flags & DBUS_NO_REPLY)) {
        dbus_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "arguments");
        return;
    }
    DRd r;
    dr_init(&r, m);
    const char *app = dr_str(&r, NULL), *profile = dr_str(&r, NULL), *kind = dr_str(&r, NULL);
    int k = r.err ? -1 : webreg_perm_kind(kind);
    if (k < 0 || !webreg_id_ok(app) || !webreg_id_ok(profile)) { dbus_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "arguments"); return; }
    Ask *a = NULL;
    int mine = 0;
    for (int i = 0; i < WEBASK_MAX; i++) {
        if (asks[i].used && !strcmp(asks[i].sender, m->sender)) mine++;
        else if (!asks[i].used && !a) a = &asks[i];
    }
    if (!a || mine >= 4) { dbus_error(c, m, "org.freedesktop.DBus.Error.LimitsExceeded", "too many questions"); return; }
    a->used = 1;
    a->id = next_id++;
    a->serial = m->serial;
    a->kind = k;
    snprintf(a->sender, sizeof a->sender, "%s", m->sender);
    snprintf(a->app, sizeof a->app, "%s", app);
    snprintf(a->profile, sizeof a->profile, "%s", profile);
    /* who asked: the bus answers from the socket's credentials, never the caller */
    DBuf b;
    dw_init(&b);
    dw_str(&b, m->sender);
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus",
               .member = "GetConnectionUnixProcessID", .dest = "org.freedesktop.DBus", .sig = "s" };
    a->pid_serial = dbus_send(c, &h, &b);
    dw_free(&b);
    if (!a->pid_serial) { send_error(a, "org.freedesktop.DBus.Error.Failed", "no answer from the bus"); drop(a, 0); }
}

int webask_reply(DConn *c, const DMsg *m) {
    (void)c;
    if (!m->sender || strcmp(m->sender, "org.freedesktop.DBus")) return 0;
    Ask *a = NULL;
    for (int i = 0; i < WEBASK_MAX; i++) if (asks[i].used && !asks[i].shown && asks[i].pid_serial == m->reply_serial) a = &asks[i];
    if (!a) return 0;
    uint32_t pid = 0;
    if (m->type == DBUS_RETURN && m->sig && !strcmp(m->sig, "u")) { DRd r; dr_init(&r, m); pid = dr_u32(&r); if (r.err) pid = 0; }
    char app[WEB_ID_MAX + 1], profile[WEB_ID_MAX + 1];
    if (!pid || webmgr_owner((pid_t)pid, app, profile) || strcmp(app, a->app) || strcmp(profile, a->profile)) {
        fprintf(stderr, "webapps: a question for %s (%s) from a process that is not its lord-web: refused\n", a->app, a->profile);
        send_error(a, "org.freedesktop.DBus.Error.AccessDenied", "not this app's process");
        drop(a, 0);
        return 1;
    }
    /* decided meanwhile (Settings, or the answer to an earlier question): no need to ask */
    WebApp *w = malloc(sizeof *w);
    int v = w && !webmgr_load(a->app, w) ? w->perms[a->kind] : -1;
    free(w);
    if (v < 0) { send_error(a, "org.freedesktop.DBus.Error.AccessDenied", "not installed"); drop(a, 0); return 1; }
    if (v != WEB_ASK) { send_answer(a, webreg_perm_value_name(v)); drop(a, 0); return 1; }
    a->shown = 1;
    event("webapps-ask", a);
    return 1;
}

int webask_answer(int id, const char *value) {
    /* "once": allowed this time; "deny-once": refused this time (the reply is "deny"); neither is kept */
    int v = !strcmp(value, "once") ? 3 : !strcmp(value, "deny-once") ? 4 : webreg_perm_value(value);
    Ask *a = NULL;
    for (int i = 0; i < WEBASK_MAX; i++) if (asks[i].used && asks[i].shown && asks[i].id == id) a = &asks[i];
    if (!a || v < 0 || v == WEB_ASK) return -1;
    if (v >= 3) { send_answer(a, v == 3 ? "once" : "deny"); drop(a, 0); return 0; }
    const char *e = webmgr_set_permission(a->app, webreg_perm_name(a->kind), value);
    if (e) fprintf(stderr, "webapps: the answer for %s could not be kept: %s\n", a->app, e);
    /* the same question from the app's other windows has the same answer now */
    char app[WEB_ID_MAX + 1];
    int kind = a->kind;
    snprintf(app, sizeof app, "%s", a->app);
    send_answer(a, value);
    drop(a, 0);
    for (int i = 0; i < WEBASK_MAX; i++)
        if (asks[i].used && asks[i].shown && asks[i].kind == kind && !strcmp(asks[i].app, app)) { send_answer(&asks[i], value); drop(&asks[i], 1); }
    return 0;
}

void webask_gone(const char *name) {
    for (int i = 0; i < WEBASK_MAX; i++) if (asks[i].used && !strcmp(asks[i].sender, name)) drop(&asks[i], 1);
}

void webask_lost(void) {
    for (int i = 0; i < WEBASK_MAX; i++) if (asks[i].used) drop(&asks[i], 1);
    bus = NULL;
}

char *webask_pending(void) {
    size_t cap = 256 * WEBASK_MAX + 4, o = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[o++] = '[';
    for (int i = 0; i < WEBASK_MAX; i++) {
        if (!asks[i].used || !asks[i].shown) continue;
        int w = snprintf(out + o, cap - o, "%s{\"id\":%d,\"app\":\"%s\",\"profile\":\"%s\",\"kind\":\"%s\"}", o > 1 ? "," : "",
                         asks[i].id, asks[i].app, asks[i].profile, webreg_perm_name(asks[i].kind));
        if (w > 0 && o + (size_t)w < cap - 2) o += (size_t)w;
    }
    out[o++] = ']';
    out[o] = 0;
    return out;
}
