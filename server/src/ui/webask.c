/* webask.c — see webask.h. */
#include "webask.h"
#include "webbadge.h"
#include "../hw/hw.h"
#include "../web/webmgr.h"
#include "../web/webedit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int used, id, shown;
    uint32_t serial;                 /* the Ask call, answered later */
    uint32_t pid_serial;             /* our GetConnectionUnixProcessID about its sender */
    char sender[256];
    char app[WEBREG_ID_MAX + 1], profile[WEBREG_ID_MAX + 1];
    int kind;
    int menu, nitems;                /* a context menu (Menu) rather than a permission question (Ask) */
    int items[WEBMENU_N];
} Ask;

static Ask asks[WEBASK_MAX];
static int next_id = 1;
static DConn *bus;
static WebBadges badges;

static const char introspect[] =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"" WEBASK_IFACE "\">\n"
    "    <method name=\"Ask\"><arg type=\"s\" name=\"app\" direction=\"in\"/><arg type=\"s\" name=\"profile\" direction=\"in\"/>"
    "<arg type=\"s\" name=\"kind\" direction=\"in\"/><arg type=\"s\" name=\"answer\" direction=\"out\"/></method>\n"
    "    <method name=\"SetBadge\"><arg type=\"s\" name=\"app\" direction=\"in\"/><arg type=\"s\" name=\"profile\" direction=\"in\"/>"
    "<arg type=\"u\" name=\"count\" direction=\"in\"/></method>\n"
    "    <method name=\"Menu\"><arg type=\"s\" name=\"app\" direction=\"in\"/><arg type=\"s\" name=\"profile\" direction=\"in\"/>"
    "<arg type=\"as\" name=\"items\" direction=\"in\"/><arg type=\"s\" name=\"item\" direction=\"out\"/></method>\n"
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

/* the menu for the shell: { id, app, profile, items: [names] } (names from webedit.c's list only) */
static void menu_event(const char *kind, const Ask *a) {
    char j[512];
    int o = snprintf(j, sizeof j, "{\"id\":%d,\"app\":\"%s\",\"profile\":\"%s\",\"items\":[", a->id, a->app, a->profile);
    for (int i = 0; i < a->nitems && o > 0 && (size_t)o < sizeof j - 32; i++) o += snprintf(j + o, sizeof j - (size_t)o, "%s\"%s\"", i ? "," : "", webmenu_name(a->items[i]));
    if (o > 0 && (size_t)o < sizeof j - 3) { snprintf(j + o, sizeof j - (size_t)o, "]}"); hw_emit(kind, j); }
}

static void drop(Ask *a, int tell) {
    if (tell && a->shown) { if (a->menu) menu_event("webapps-menu-gone", a); else event("webapps-ask-gone", a); }
    memset(a, 0, sizeof *a);
}

/* asks the bus for sender's pid (its answer comes to webask_reply); the serial, 0 when it could not */
static uint32_t ask_pid(DConn *c, const char *sender) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, sender);
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus",
               .member = "GetConnectionUnixProcessID", .dest = "org.freedesktop.DBus", .sig = "s" };
    uint32_t serial = dbus_send(c, &h, &b);
    dw_free(&b);
    return serial;
}

static void badge_event(const char *app, const char *profile, uint32_t count) {
    char j[160];
    snprintf(j, sizeof j, "{\"app\":\"%s\",\"profile\":\"%s\",\"count\":%u}", app, profile, count);
    hw_emit("webapps-badge", j);
}

/* SetBadge(s app, s profile, u count): shown only once the caller is known to be that app and profile's lord-web */
static void set_badge(DConn *c, const DMsg *m) {
    if (!m->sig || strcmp(m->sig, "ssu") || !m->sender) { dbus_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "arguments"); return; }
    DRd r;
    dr_init(&r, m);
    const char *app = dr_str(&r, NULL), *profile = dr_str(&r, NULL);
    uint32_t count = dr_u32(&r);
    int slot = -1;
    int what = r.err ? WB_REFUSE : webbadge_call(&badges, m->sender, app, profile, count, &slot);
    if (what == WB_REFUSE) { dbus_error(c, m, "org.freedesktop.DBus.Error.AccessDenied", "not this app's process"); return; }
    if (what == WB_ASK_PID) {
        uint32_t serial = ask_pid(c, m->sender);
        webbadge_asked(&badges, slot, serial);
        if (!serial) { dbus_error(c, m, "org.freedesktop.DBus.Error.Failed", "no answer from the bus"); return; }
    } else if (what == WB_EMIT) badge_event(badges.b[slot].app, badges.b[slot].profile, badges.b[slot].count);
    dbus_reply(c, m, NULL, NULL);
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
    if (m->iface && !strcmp(m->iface, WEBASK_IFACE) && !strcmp(m->member, "SetBadge")) { set_badge(c, m); return; }
    int menu = m->iface && !strcmp(m->iface, WEBASK_IFACE) && !strcmp(m->member, "Menu");
    if (!menu && (!m->iface || strcmp(m->iface, WEBASK_IFACE) || strcmp(m->member, "Ask"))) {
        dbus_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
        return;
    }
    if (!m->sig || strcmp(m->sig, menu ? "ssas" : "sss") || !m->sender || strlen(m->sender) >= sizeof asks[0].sender || (m->flags & DBUS_NO_REPLY)) {
        dbus_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "arguments");
        return;
    }
    DRd r;
    dr_init(&r, m);
    const char *app = dr_str(&r, NULL), *profile = dr_str(&r, NULL);
    int k = 0, items[WEBMENU_N], nitems = 0;
    if (menu) {
        /* the items: names from the closed list (webedit.h), none twice; never text to show */
        const char *names[WEBMENU_N + 1];
        int n = 0;
        size_t end = dr_arr(&r, 4);
        while (dr_more(&r, end) && n <= WEBMENU_N) names[n++] = dr_str(&r, NULL);
        nitems = r.err || dr_more(&r, end) ? -1 : webmenu_parse(names, n, items);
        k = nitems < 0 ? -1 : 0;
    } else {
        const char *kind = dr_str(&r, NULL);
        k = r.err ? -1 : webreg_perm_kind(kind);
    }
    if (k < 0 || !webreg_id_ok(app) || !webreg_id_ok(profile)) { dbus_error(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "arguments"); return; }
    Ask *a = NULL;
    int mine = 0;
    for (int i = 0; i < WEBASK_MAX; i++) {
        /* a new menu from a lord-web closes the one it had open (answered "") */
        if (menu && asks[i].used && asks[i].menu && !strcmp(asks[i].sender, m->sender)) { send_answer(&asks[i], ""); drop(&asks[i], 1); }
        if (asks[i].used && !strcmp(asks[i].sender, m->sender)) mine++;
        else if (!asks[i].used && !a) a = &asks[i];
    }
    if (!a || mine >= 4) { dbus_error(c, m, "org.freedesktop.DBus.Error.LimitsExceeded", "too many questions"); return; }
    a->used = 1;
    a->id = next_id++;
    a->serial = m->serial;
    a->kind = k;
    a->menu = menu;
    a->nitems = menu ? nitems : 0;
    for (int i = 0; i < a->nitems; i++) a->items[i] = items[i];
    snprintf(a->sender, sizeof a->sender, "%s", m->sender);
    snprintf(a->app, sizeof a->app, "%s", app);
    snprintf(a->profile, sizeof a->profile, "%s", profile);
    /* who asked: the bus answers from the socket's credentials, never the caller */
    a->pid_serial = ask_pid(c, m->sender);
    if (!a->pid_serial) { send_error(a, "org.freedesktop.DBus.Error.Failed", "no answer from the bus"); drop(a, 0); }
}

/* the bus's pid of a SetBadge caller: 1 if m was that answer */
static int badge_reply(const DMsg *m) {
    int slot = -1;
    for (int i = 0; i < WEBBADGE_MAX && slot < 0; i++) if (badges.b[i].state == WB_ASKING && badges.b[i].serial == m->reply_serial) slot = i;
    if (slot < 0) return 0;
    uint32_t pid = 0;
    if (m->type == DBUS_RETURN && m->sig && !strcmp(m->sig, "u")) { DRd r; dr_init(&r, m); pid = dr_u32(&r); if (r.err) pid = 0; }
    char app[WEBREG_ID_MAX + 1], profile[WEBREG_ID_MAX + 1];
    const WebBadge *b = &badges.b[slot];
    int ok = pid && !webmgr_owner((pid_t)pid, app, profile) && !strcmp(app, b->app) && !strcmp(profile, b->profile);
    if (!ok) fprintf(stderr, "webapps: a badge for %s (%s) from a process that is not its lord-web: refused\n", b->app, b->profile);
    int show = 0;
    slot = webbadge_answer(&badges, m->reply_serial, ok, &show);
    if (slot >= 0 && show) badge_event(badges.b[slot].app, badges.b[slot].profile, badges.b[slot].count);
    return 1;
}

int webask_reply(DConn *c, const DMsg *m) {
    (void)c;
    if (!m->sender || strcmp(m->sender, "org.freedesktop.DBus")) return 0;
    if (badge_reply(m)) return 1;
    Ask *a = NULL;
    for (int i = 0; i < WEBASK_MAX; i++) if (asks[i].used && !asks[i].shown && asks[i].pid_serial == m->reply_serial) a = &asks[i];
    if (!a) return 0;
    uint32_t pid = 0;
    if (m->type == DBUS_RETURN && m->sig && !strcmp(m->sig, "u")) { DRd r; dr_init(&r, m); pid = dr_u32(&r); if (r.err) pid = 0; }
    char app[WEBREG_ID_MAX + 1], profile[WEBREG_ID_MAX + 1];
    if (!pid || webmgr_owner((pid_t)pid, app, profile) || strcmp(app, a->app) || strcmp(profile, a->profile)) {
        fprintf(stderr, "webapps: a %s for %s (%s) from a process that is not its lord-web: refused\n", a->menu ? "menu" : "question", a->app, a->profile);
        send_error(a, "org.freedesktop.DBus.Error.AccessDenied", "not this app's process");
        drop(a, 0);
        return 1;
    }
    if (a->menu) {
        a->shown = 1;
        menu_event("webapps-menu", a);
        return 1;
    }
    /* decided meanwhile (Settings, or the answer to an earlier question): no need to ask */
    WebApp *w = malloc(sizeof *w);
    int v = w && !webmgr_load(a->app, w) ? w->perms[a->kind] : -1;
    free(w);
    if (v < 0) { send_error(a, "org.freedesktop.DBus.Error.AccessDenied", "not installed"); drop(a, 0); return 1; }
    if (v != WEBREG_ASK) { send_answer(a, webreg_perm_value_name(v)); drop(a, 0); return 1; }
    a->shown = 1;
    event("webapps-ask", a);
    return 1;
}

int webask_answer(int id, const char *value) {
    /* "once": allowed this time; "deny-once": refused this time (the reply is "deny"); neither is kept */
    int v = !strcmp(value, "once") ? 3 : !strcmp(value, "deny-once") ? 4 : webreg_perm_value(value);
    Ask *a = NULL;
    for (int i = 0; i < WEBASK_MAX; i++) if (asks[i].used && asks[i].shown && !asks[i].menu && asks[i].id == id) a = &asks[i];
    if (!a || v < 0 || v == WEBREG_ASK) return -1;
    if (v >= 3) { send_answer(a, v == 3 ? "once" : "deny"); drop(a, 0); return 0; }
    const char *e = webmgr_set_permission(a->app, webreg_perm_name(a->kind), value);
    if (e) fprintf(stderr, "webapps: the answer for %s could not be kept: %s\n", a->app, e);
    /* the same question from the app's other windows has the same answer now */
    char app[WEBREG_ID_MAX + 1];
    int kind = a->kind;
    snprintf(app, sizeof app, "%s", a->app);
    send_answer(a, value);
    drop(a, 0);
    for (int i = 0; i < WEBASK_MAX; i++)
        if (asks[i].used && asks[i].shown && asks[i].kind == kind && !strcmp(asks[i].app, app)) { send_answer(&asks[i], value); drop(&asks[i], 1); }
    return 0;
}

int webask_menu_answer(int id, const char *item) {
    Ask *a = NULL;
    for (int i = 0; i < WEBASK_MAX; i++) if (asks[i].used && asks[i].shown && asks[i].menu && asks[i].id == id) a = &asks[i];
    if (!a || !item) return -1;
    int k = item[0] ? webmenu_item(item) : -1, offered = 0;
    for (int i = 0; i < a->nitems; i++) if (a->items[i] == k) offered = 1;
    if (item[0] && !offered) return -1;            /* only an item this menu had */
    send_answer(a, item);
    drop(a, 0);
    return 0;
}

void webask_attach(DConn *c) { bus = c; }

int webask_edit(const char *app, const char *profile, const char *command) {
    char name[160];
    if (!bus || webedit_command(command) < 0 || webreg_bus_name(app, profile, name, sizeof name)) return -1;
    DBuf b;
    dw_init(&b);
    dw_str(&b, command);
    DHdr h = { .type = DBUS_CALL, .flags = DBUS_NO_REPLY | DBUS_NO_AUTOSTART, .path = WEBREG_SHOW_PATH, .iface = WEBREG_SHOW_IFACE,
               .member = "Edit", .dest = name, .sig = "s" };
    uint32_t serial = dbus_send(bus, &h, &b);
    dw_free(&b);
    if (serial) dbus_flush(bus);
    return serial ? 0 : -1;
}

int webask_show(const char *app, const char *profile, const char *token) {
    char name[160];
    if (!bus || webreg_bus_name(app, profile, name, sizeof name)) return -1;
    DBuf b;
    dw_init(&b);
    dw_str(&b, token ? token : "");
    DHdr h = { .type = DBUS_CALL, .flags = DBUS_NO_REPLY | DBUS_NO_AUTOSTART, .path = WEBREG_SHOW_PATH, .iface = WEBREG_SHOW_IFACE,
               .member = "Show", .dest = name, .sig = "s" };
    uint32_t serial = dbus_send(bus, &h, &b);
    dw_free(&b);
    if (serial) dbus_flush(bus);
    return serial ? 0 : -1;
}

/* that connection's count goes: its lord-web ended (or the bus did) */
static void badge_gone(const char *name) {
    char app[WEBBADGE_ID_MAX + 1], profile[WEBBADGE_ID_MAX + 1];
    if (webbadge_gone(&badges, name, app, profile)) badge_event(app, profile, 0);
}

void webask_gone(const char *name) {
    for (int i = 0; i < WEBASK_MAX; i++) if (asks[i].used && !strcmp(asks[i].sender, name)) drop(&asks[i], 1);
    badge_gone(name);
}

void webask_lost(void) {
    for (int i = 0; i < WEBASK_MAX; i++) if (asks[i].used) drop(&asks[i], 1);
    for (int i = 0; i < WEBBADGE_MAX; i++) {
        if (badges.b[i].state == WB_NEW) continue;
        char s[WEBBADGE_NAME_MAX + 1];
        snprintf(s, sizeof s, "%s", badges.b[i].sender);
        badge_gone(s);
    }
    bus = NULL;
}

char *webask_badges(void) {
    size_t cap = 128 * WEBBADGE_MAX + 4, o = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[o++] = '[';
    for (int i = 0; i < WEBBADGE_MAX; i++) {
        const WebBadge *b = &badges.b[i];
        if (b->state != WB_OK) continue;
        int w = snprintf(out + o, cap - o, "%s{\"app\":\"%s\",\"profile\":\"%s\",\"count\":%u}", o > 1 ? "," : "", b->app, b->profile, b->count);
        if (w > 0 && o + (size_t)w < cap - 2) o += (size_t)w;
    }
    out[o++] = ']';
    out[o] = 0;
    return out;
}

char *webask_pending(void) {
    size_t cap = 256 * WEBASK_MAX + 4, o = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[o++] = '[';
    for (int i = 0; i < WEBASK_MAX; i++) {
        if (!asks[i].used || !asks[i].shown || asks[i].menu) continue;
        int w = snprintf(out + o, cap - o, "%s{\"id\":%d,\"app\":\"%s\",\"profile\":\"%s\",\"kind\":\"%s\"}", o > 1 ? "," : "",
                         asks[i].id, asks[i].app, asks[i].profile, webreg_perm_name(asks[i].kind));
        if (w > 0 && o + (size_t)w < cap - 2) o += (size_t)w;
    }
    out[o++] = ']';
    out[o] = 0;
    return out;
}
