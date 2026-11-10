/* sni.c — the tray of Linux apps (see sni.h): lord-server owns org.kde.StatusNotifierWatcher on the session bus, the
 * name Qt, KDE, Ayatana's AppIndicator and Chromium register their StatusNotifierItem with, and is its own host. Each
 * item's properties are read with GetAll and read again on its New* signals; the theme draws it next to LordOS's
 * status items (sdk/trayitems.jsx). Its menu is com.canonical.dbusmenu's layout, shown with the SDK's menu.
 *
 * Nothing here waits for an app: every call is sent and its answer handled when it comes, so an app that hangs only
 * loses its own icon or menu. An item leaves when its connection (or the name it registered) leaves the bus, or when
 * its first GetAll fails. What an app sends is bounded and cleaned (ui/snimodel.c); an icon name is looked up in the
 * installed themes (apps_status_icon), never as a path, and IconThemePath is not read. Uses Passwords' own D-Bus
 * client (vault/dbus.c). See docs/WAYLAND.md, "Tray icons". */
#include "sni.h"
#include "snimodel.h"
#include "../apps.h"
#include "../hw/hw.h"
#include "../vault/dbus.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WATCHER_NAME  "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH  "/StatusNotifierWatcher"
#define WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define ITEM_IFACE    "org.kde.StatusNotifierItem"
#define MENU_IFACE    "com.canonical.dbusmenu"
#define PROPS_IFACE   "org.freedesktop.DBus.Properties"
#define MAX_ITEMS     32
#define MAX_PER_APP   4                 /* items one connection may register */
#define MAX_HOSTS     8
#define MAX_CALLS     64                /* calls waiting for an answer; past it the oldest is forgotten */

static DConn conn = { .fd = -1 };
static int up, owned;

typedef struct {
    int id;                             /* ours, > 0; 0 = a free slot */
    char owner[SNI_NAME_MAX];           /* the connection that registered it */
    char dest[SNI_NAME_MAX], path[SNI_NAME_MAX];
    char key[2 * SNI_NAME_MAX];         /* dest + path, as RegisteredStatusNotifierItems lists it */
    char menu[SNI_NAME_MAX];            /* its dbusmenu object, or "" */
    char title[SNI_TEXT_MAX], tooltip[SNI_TEXT_MAX], status[16];
    char icon[PATH_MAX], attn[PATH_MAX];  /* the theme's files for IconName and AttentionIconName, or "" */
    SniPix pix, attn_pix;
    int is_menu, ready;
    unsigned rev;                       /* goes up with every new pixmap */
    uint32_t fetching;                  /* the GetAll on its way */
    int again;                          /* a New* signal came while it was */
    int32_t ids[SNI_MENU_ITEMS];        /* the entries of the last menu shown that can be clicked */
    int nids;
} Item;

static Item items[MAX_ITEMS];
static int item_seq;
static char hosts[MAX_HOSTS][SNI_NAME_MAX];
static int nhosts;

enum { C_NAME = 1, C_GETALL, C_LAYOUT, C_ACTIVATE };
static struct { uint32_t serial; int kind, item; } calls[MAX_CALLS];
static int ncalls;

static Item *item_by_id(int id);

static void call_add(uint32_t serial, int kind, int item) {
    if (!serial) return;
    if (ncalls == MAX_CALLS) {
        Item *it = calls[0].kind == C_GETALL ? item_by_id(calls[0].item) : NULL;
        if (it && it->fetching == calls[0].serial) it->fetching = 0;   /* forgotten: the next New* reads it again */
        memmove(&calls[0], &calls[1], sizeof calls[0] * (MAX_CALLS - 1));
        ncalls--;
    }
    calls[ncalls].serial = serial;
    calls[ncalls].kind = kind;
    calls[ncalls].item = item;
    ncalls++;
}

static Item *item_by_id(int id) {
    for (int i = 0; id > 0 && i < MAX_ITEMS; i++) if (items[i].id == id) return &items[i];
    return NULL;
}

/* a method call to an item's app; with_reply 0: the app need not answer */
static uint32_t call(const char *dest, const char *path, const char *iface, const char *member, const char *sig, const DBuf *b, int with_reply) {
    DHdr h = { .type = DBUS_CALL, .flags = with_reply ? 0 : DBUS_NO_REPLY, .path = path, .iface = iface, .member = member, .dest = dest, .sig = sig };
    return dbus_send(&conn, &h, b);
}

static void signal_str(const char *member, const char *arg) {
    DBuf b;
    dw_init(&b);
    if (arg) dw_str(&b, arg);
    dbus_signal(&conn, WATCHER_PATH, WATCHER_IFACE, member, arg ? "s" : NULL, &b);
    dw_free(&b);
}

/* ---- what the theme sees ---- */

/* What the bar shows: the attention icon while the app asks for it, the theme's file before the app's pixmap. *file
 * gets the file ("" for none); the pixmap, if that is what shows, is returned. */
static const SniPix *shown(const Item *it, const char **file) {
    int attention = !strcmp(it->status, "NeedsAttention");
    *file = "";
    if (attention && it->attn[0]) { *file = it->attn; return NULL; }
    if (attention && it->attn_pix.rgba) return &it->attn_pix;
    if (it->icon[0]) { *file = it->icon; return NULL; }
    return it->pix.rgba ? &it->pix : NULL;
}

static void item_json(HBuf *b, const Item *it) {
    const char *file;
    const SniPix *px = shown(it, &file);
    hb_printf(b, "{\"id\":%d,\"title\":", it->id);
    hb_str(b, it->title);
    hb_printf(b, ",\"tooltip\":");
    hb_str(b, it->tooltip);
    hb_printf(b, ",\"status\":\"%s\",\"icon\":", it->status);
    hb_str(b, file);
    hb_printf(b, ",\"pix\":%u,\"pw\":%d,\"ph\":%d,\"menu\":%s,\"isMenu\":%s}", px ? it->rev : 0u, px ? px->w : 0, px ? px->h : 0,
              it->menu[0] ? "true" : "false", it->is_menu ? "true" : "false");
}

static void emit_item(const Item *it) {
    HBuf b = { 0 };
    item_json(&b, it);
    char *j = hb_take(&b);
    if (j) hw_emit("sni", j);
    free(j);
}

void sni_list(HBuf *b) {
    hb_printf(b, "[");
    int comma = 0;
    for (int i = 0; i < MAX_ITEMS; i++) {
        if (!items[i].id || !items[i].ready) continue;
        if (comma) hb_printf(b, ",");
        comma = 1;
        item_json(b, &items[i]);
    }
    hb_printf(b, "]");
}

const uint8_t *sni_pixels(int id, int *w, int *h) {
    Item *it = item_by_id(id);
    const char *file;
    const SniPix *px = it && it->ready ? shown(it, &file) : NULL;
    if (!px) return NULL;
    *w = px->w;
    *h = px->h;
    return px->rgba;
}

static void item_drop(Item *it) {
    if (owned) signal_str("StatusNotifierItemUnregistered", it->key);
    if (it->ready) {
        char j[48];
        snprintf(j, sizeof j, "{\"id\":%d}", it->id);
        hw_emit("sni-gone", j);
    }
    sni_pix_free(&it->pix);
    sni_pix_free(&it->attn_pix);
    memset(it, 0, sizeof *it);
}

/* ---- an item's properties ---- */

static void fetch(Item *it) {
    if (it->fetching) { it->again = 1; return; }
    DBuf b;
    dw_init(&b);
    dw_str(&b, ITEM_IFACE);
    it->fetching = call(it->dest, it->path, PROPS_IFACE, "GetAll", "s", &b, 1);
    dw_free(&b);
    call_add(it->fetching, C_GETALL, it->id);
}

/* GetAll's a{sv}: what is not there keeps no old value, as each answer has them all */
static void got_props(Item *it, const DMsg *m) {
    DRd r;
    dr_init(&r, m);
    char title[SNI_TEXT_MAX] = "", id[SNI_TEXT_MAX] = "", tip[SNI_TEXT_MAX] = "", status[16] = "Active";
    char icon_name[SNI_TEXT_MAX] = "", attn_name[SNI_TEXT_MAX] = "", menu[SNI_NAME_MAX] = "";
    SniPix pix = { 0 }, attn = { 0 };
    int is_menu = 0;
    size_t end = dr_arr(&r, 8);
    while (dr_more(&r, end)) {
        dr_struct(&r);
        const char *key = dr_str(&r, NULL), *sig = dr_variant(&r);
        if (r.err) break;
        if (!strcmp(key, "Title") && !strcmp(sig, "s")) sni_text(dr_str(&r, NULL), title, sizeof title, 0);
        else if (!strcmp(key, "Id") && !strcmp(sig, "s")) sni_text(dr_str(&r, NULL), id, sizeof id, 0);
        else if (!strcmp(key, "Status") && !strcmp(sig, "s")) {
            const char *s = dr_str(&r, NULL);
            if (!strcmp(s, "Passive") || !strcmp(s, "NeedsAttention")) snprintf(status, sizeof status, "%s", s);
        } else if (!strcmp(key, "IconName") && !strcmp(sig, "s")) snprintf(icon_name, sizeof icon_name, "%s", dr_str(&r, NULL));
        else if (!strcmp(key, "AttentionIconName") && !strcmp(sig, "s")) snprintf(attn_name, sizeof attn_name, "%s", dr_str(&r, NULL));
        else if (!strcmp(key, "IconPixmap") && !strcmp(sig, "a(iiay)")) { sni_pix_free(&pix); sni_pixmap(&r, &pix); }
        else if (!strcmp(key, "AttentionIconPixmap") && !strcmp(sig, "a(iiay)")) { sni_pix_free(&attn); sni_pixmap(&r, &attn); }
        else if (!strcmp(key, "ToolTip") && !strcmp(sig, "(sa(iiay)ss)")) {
            dr_struct(&r);
            dr_str(&r, NULL);                       /* its icon: not shown */
            dr_skip(&r, "a(iiay)");
            const char *t = dr_str(&r, NULL), *body = dr_str(&r, NULL);
            sni_text(t[0] ? t : body, tip, sizeof tip, 0);
        } else if (!strcmp(key, "Menu") && !strcmp(sig, "o")) {
            const char *p = dr_str(&r, NULL);
            if (strcmp(p, "/") && strlen(p) < sizeof menu) snprintf(menu, sizeof menu, "%s", p);
        } else if (!strcmp(key, "ItemIsMenu") && !strcmp(sig, "b")) is_menu = dr_bool(&r);
        else dr_skip(&r, sig);
    }
    if (r.err) { sni_pix_free(&pix); sni_pix_free(&attn); return; }
    snprintf(it->title, sizeof it->title, "%s", title[0] ? title : id);
    snprintf(it->tooltip, sizeof it->tooltip, "%s", tip);
    snprintf(it->status, sizeof it->status, "%s", status);
    snprintf(it->menu, sizeof it->menu, "%s", menu);
    it->is_menu = is_menu;
    apps_status_icon(icon_name, it->icon, sizeof it->icon);
    apps_status_icon(attn_name, it->attn, sizeof it->attn);
    sni_pix_free(&it->pix);
    sni_pix_free(&it->attn_pix);
    it->pix = pix;
    it->attn_pix = attn;
    it->rev++;
    it->ready = 1;
    emit_item(it);
}

/* ---- the watcher ---- */

static void reply_empty(const DMsg *m) { dbus_reply(&conn, m, NULL, NULL); }

static void register_item(const DMsg *m) {
    DRd r;
    dr_init(&r, m);
    const char *service = dr_str(&r, NULL);
    char dest[SNI_NAME_MAX], path[SNI_NAME_MAX];
    if (r.err || sni_service(service, m->sender, dest, path)) {
        dbus_error(&conn, m, "org.freedesktop.DBus.Error.InvalidArgs", "service");
        return;
    }
    char key[2 * SNI_NAME_MAX];
    snprintf(key, sizeof key, "%s%s", dest, path);
    int mine = 0, slot = -1;
    for (int i = 0; i < MAX_ITEMS; i++) {
        if (!items[i].id) { if (slot < 0) slot = i; continue; }
        if (!strcmp(items[i].key, key)) { reply_empty(m); fetch(&items[i]); return; }   /* again: read it again */
        if (!strcmp(items[i].owner, m->sender)) mine++;
    }
    if (slot < 0 || mine >= MAX_PER_APP) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.LimitsExceeded", "too many items"); return; }
    Item *it = &items[slot];
    memset(it, 0, sizeof *it);
    if (++item_seq <= 0) item_seq = 1;
    it->id = item_seq;
    snprintf(it->owner, sizeof it->owner, "%s", m->sender);
    snprintf(it->dest, sizeof it->dest, "%s", dest);
    snprintf(it->path, sizeof it->path, "%s", path);
    snprintf(it->key, sizeof it->key, "%s", key);
    snprintf(it->status, sizeof it->status, "Active");
    reply_empty(m);
    signal_str("StatusNotifierItemRegistered", key);
    fetch(it);
}

static void register_host(const DMsg *m) {
    reply_empty(m);
    for (int i = 0; i < nhosts; i++) if (!strcmp(hosts[i], m->sender)) return;
    if (nhosts == MAX_HOSTS || strlen(m->sender) >= SNI_NAME_MAX) return;
    snprintf(hosts[nhosts++], SNI_NAME_MAX, "%s", m->sender);
    signal_str("StatusNotifierHostRegistered", NULL);
}

static void put_prop(DBuf *b, const char *name) {
    if (!strcmp(name, "RegisteredStatusNotifierItems")) {
        dw_variant(b, "as");
        size_t a = dw_arr_begin(b, 4);
        for (int i = 0; i < MAX_ITEMS; i++) if (items[i].id) dw_str(b, items[i].key);
        dw_arr_end(b, a);
    } else if (!strcmp(name, "IsStatusNotifierHostRegistered")) {
        dw_variant(b, "b");
        dw_bool(b, 1);                              /* lord-server is the host */
    } else {
        dw_variant(b, "i");
        dw_i32(b, 0);                               /* ProtocolVersion */
    }
}

static const char *const props[] = { "RegisteredStatusNotifierItems", "IsStatusNotifierHostRegistered", "ProtocolVersion" };

static void properties(const DMsg *m) {
    DRd r;
    dr_init(&r, m);
    const char *iface = dr_str(&r, NULL);
    if (r.err || (strcmp(iface, WATCHER_IFACE) && iface[0])) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownInterface", "interface"); return; }
    DBuf b;
    dw_init(&b);
    if (!strcmp(m->member, "Get") && m->sig && !strcmp(m->sig, "ss")) {
        const char *name = dr_str(&r, NULL);
        int known = 0;
        for (size_t i = 0; i < sizeof props / sizeof props[0]; i++) known |= !strcmp(name, props[i]);
        if (r.err || !known) dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownProperty", "property");
        else { put_prop(&b, name); dbus_reply(&conn, m, "v", &b); }
    } else if (!strcmp(m->member, "GetAll")) {
        size_t a = dw_arr_begin(&b, 8);
        for (size_t i = 0; i < sizeof props / sizeof props[0]; i++) {
            dw_struct(&b);
            dw_str(&b, props[i]);
            put_prop(&b, props[i]);
        }
        dw_arr_end(&b, a);
        dbus_reply(&conn, m, "a{sv}", &b);
    } else dbus_error(&conn, m, "org.freedesktop.DBus.Error.PropertyReadOnly", "read-only");
    dw_free(&b);
}

static const char introspect_xml[] =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    " <interface name=\"org.kde.StatusNotifierWatcher\">\n"
    "  <method name=\"RegisterStatusNotifierItem\"><arg type=\"s\" name=\"service\" direction=\"in\"/></method>\n"
    "  <method name=\"RegisterStatusNotifierHost\"><arg type=\"s\" name=\"service\" direction=\"in\"/></method>\n"
    "  <property name=\"RegisteredStatusNotifierItems\" type=\"as\" access=\"read\"/>\n"
    "  <property name=\"IsStatusNotifierHostRegistered\" type=\"b\" access=\"read\"/>\n"
    "  <property name=\"ProtocolVersion\" type=\"i\" access=\"read\"/>\n"
    "  <signal name=\"StatusNotifierItemRegistered\"><arg type=\"s\"/></signal>\n"
    "  <signal name=\"StatusNotifierItemUnregistered\"><arg type=\"s\"/></signal>\n"
    "  <signal name=\"StatusNotifierHostRegistered\"/>\n"
    "  <signal name=\"StatusNotifierHostUnregistered\"/>\n"
    " </interface>\n"
    " <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "  <method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>\n"
    "  <method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>\n"
    " </interface>\n"
    " <interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\"><arg type=\"s\" direction=\"out\"/></method></interface>\n"
    " <interface name=\"org.freedesktop.DBus.Peer\"><method name=\"Ping\"/></interface>\n"
    "</node>\n";

static void method(const DMsg *m) {
    const char *iface = m->iface ? m->iface : "", *mem = m->member;
    if (!strcmp(mem, "Ping") && (!iface[0] || !strcmp(iface, "org.freedesktop.DBus.Peer"))) { reply_empty(m); return; }
    if (!strcmp(mem, "Introspect")) {
        DBuf b;
        dw_init(&b);
        dw_str(&b, m->path && !strcmp(m->path, WATCHER_PATH) ? introspect_xml : "<node><node name=\"StatusNotifierWatcher\"/></node>");
        dbus_reply(&conn, m, "s", &b);
        dw_free(&b);
        return;
    }
    if (!m->path || strcmp(m->path, WATCHER_PATH)) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownObject", "no such object"); return; }
    if (!strcmp(iface, PROPS_IFACE)) { properties(m); return; }
    if ((!iface[0] || !strcmp(iface, WATCHER_IFACE)) && m->sig && !strcmp(m->sig, "s") && m->sender) {
        if (!strcmp(mem, "RegisterStatusNotifierItem")) { register_item(m); return; }
        if (!strcmp(mem, "RegisterStatusNotifierHost")) { register_host(m); return; }
    }
    dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
}

/* ---- answers and signals ---- */

static void open_menu(Item *it);

static void on_reply(const DMsg *m) {
    int k = -1;
    for (int i = 0; i < ncalls && k < 0; i++) if (calls[i].serial == m->reply_serial) k = i;
    if (k < 0) return;
    int kind = calls[k].kind, id = calls[k].item;
    memmove(&calls[k], &calls[k + 1], sizeof calls[0] * (size_t)(ncalls - k - 1));
    ncalls--;
    if (kind == C_NAME) {
        uint32_t res = 0;
        if (m->type == DBUS_RETURN && m->sig && !strcmp(m->sig, "u")) { DRd r; dr_init(&r, m); res = dr_u32(&r); if (r.err) res = 0; }
        if (res == 1 || res == 4) {
            owned = 1;
            signal_str("StatusNotifierHostRegistered", NULL);
            fprintf(stderr, "sni: %s on the session bus (tray icons of Linux apps)\n", WATCHER_NAME);
        } else fprintf(stderr, "sni: %s is already served by another program; no tray icons for Linux apps\n", WATCHER_NAME);
        return;
    }
    Item *it = item_by_id(id);
    if (!it) return;
    if (kind == C_GETALL) {
        if (it->fetching != m->reply_serial) return;
        it->fetching = 0;
        if (m->type == DBUS_RETURN && m->sig && !strcmp(m->sig, "a{sv}")) got_props(it, m);
        else if (!it->ready) { item_drop(it); return; }    /* nothing there: not an item */
        if (it->again) { it->again = 0; fetch(it); }
    } else if (kind == C_LAYOUT) {
        if (m->type != DBUS_RETURN || !m->sig || strcmp(m->sig, "u(ia{sv}av)")) return;
        DRd r;
        dr_init(&r, m);
        dr_u32(&r);                                 /* the layout's revision */
        HBuf b = { 0 };
        hb_printf(&b, "{\"id\":%d,\"items\":", it->id);
        int n = 0;
        if (sni_menu_json(&r, &b, it->ids, SNI_MENU_ITEMS, &n)) { free(hb_take(&b)); it->nids = 0; return; }
        it->nids = n;
        hb_printf(&b, "}");
        char *j = hb_take(&b);
        if (j) hw_emit("sni-menu", j);
        free(j);
    } else if (kind == C_ACTIVATE && m->type == DBUS_ERROR && it->menu[0]) open_menu(it);   /* no Activate: Ayatana's items */
}

static int is_bus(const DMsg *m) { return m->sender && !strcmp(m->sender, "org.freedesktop.DBus"); }

static void on_signal(const DMsg *m) {
    if (!m->member) return;
    if (is_bus(m) && !strcmp(m->member, "NameOwnerChanged") && m->sig && !strcmp(m->sig, "sss")) {
        DRd r;
        dr_init(&r, m);
        const char *name = dr_str(&r, NULL), *old = dr_str(&r, NULL);
        dr_str(&r, NULL);
        if (r.err || !old[0]) return;
        /* the connection left, or the name it registered went away (or to another program) */
        for (int i = 0; i < MAX_ITEMS; i++)
            if (items[i].id && (!strcmp(items[i].owner, name) || !strcmp(items[i].dest, name))) item_drop(&items[i]);
        for (int i = nhosts; i-- > 0;)
            if (!strcmp(hosts[i], name)) {
                memmove(hosts[i], hosts[i + 1], sizeof hosts[0] * (size_t)(nhosts - i - 1));
                nhosts--;
                if (owned) signal_str("StatusNotifierHostUnregistered", NULL);
            }
        return;
    }
    if (!m->sender || !m->path || !m->iface || strcmp(m->iface, ITEM_IFACE) || strncmp(m->member, "New", 3)) return;
    for (int i = 0; i < MAX_ITEMS; i++)
        if (items[i].id && !strcmp(items[i].owner, m->sender) && !strcmp(items[i].path, m->path)) fetch(&items[i]);
}

static void handle(const DMsg *m) {
    if ((m->type == DBUS_RETURN || m->type == DBUS_ERROR) && m->reply_serial) { on_reply(m); return; }
    if (m->type == DBUS_SIGNAL) { on_signal(m); return; }
    if (m->type == DBUS_CALL && m->member) method(m);
}

/* ---- from the theme ---- */

static void open_menu(Item *it) {
    DBuf b;
    dw_init(&b);
    dw_i32(&b, 0);
    call(it->dest, it->menu, MENU_IFACE, "AboutToShow", "i", &b, 0);   /* the app may fill the menu now; GetLayout comes after */
    dw_free(&b);
    dw_init(&b);
    dw_i32(&b, 0);                                  /* from the root */
    dw_i32(&b, SNI_MENU_DEPTH);
    size_t a = dw_arr_begin(&b, 4);                 /* every property */
    dw_arr_end(&b, a);
    call_add(call(it->dest, it->menu, MENU_IFACE, "GetLayout", "iias", &b, 1), C_LAYOUT, it->id);
    dw_free(&b);
    dbus_flush(&conn);
}

static void send_xy(Item *it, const char *member, int x, int y, int kind) {
    DBuf b;
    dw_init(&b);
    dw_i32(&b, x);
    dw_i32(&b, y);
    uint32_t s = call(it->dest, it->path, ITEM_IFACE, member, "ii", &b, kind != 0);
    dw_free(&b);
    if (kind) call_add(s, kind, it->id);
    dbus_flush(&conn);
}

int sni_activate(int id, int x, int y, int secondary) {
    Item *it = item_by_id(id);
    if (!up || !it || !it->ready) return -1;
    if (it->is_menu && it->menu[0] && !secondary) { open_menu(it); return 0; }
    send_xy(it, secondary ? "SecondaryActivate" : "Activate", x, y, secondary ? 0 : C_ACTIVATE);
    return 0;
}

int sni_menu(int id, int x, int y) {
    Item *it = item_by_id(id);
    if (!up || !it || !it->ready) return -1;
    if (it->menu[0]) { open_menu(it); return 1; }
    send_xy(it, "ContextMenu", x, y, 0);
    return 0;
}

int sni_menu_clicked(int id, int32_t entry) {
    Item *it = item_by_id(id);
    if (!up || !it || !it->menu[0]) return -1;
    int found = 0;
    for (int i = 0; i < it->nids && !found; i++) found = it->ids[i] == entry;
    if (!found) return -1;
    DBuf b;
    dw_init(&b);
    dw_i32(&b, entry);
    dw_str(&b, "clicked");
    dw_variant(&b, "i");
    dw_i32(&b, 0);
    dw_u32(&b, 0);                                  /* timestamp */
    call(it->dest, it->menu, MENU_IFACE, "Event", "isvu", &b, 0);
    dw_free(&b);
    dbus_flush(&conn);
    it->nids = 0;                                   /* one choice per menu shown */
    return 0;
}

/* ---- the connection ---- */

static void add_match(const char *rule) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, rule);
    call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "AddMatch", "s", &b, 0);
    dw_free(&b);
}

int sni_start(void) {
    int fd = dbus_connect_session();
    if (fd < 0 || dbus_open(&conn, fd, 1000)) { if (fd >= 0) dbus_close(&conn); conn.fd = -1; return -1; }
    up = 1;
    /* the name is asked for without waiting: the answer comes in sni_dispatch */
    DBuf b;
    dw_init(&b);
    dw_str(&b, WATCHER_NAME);
    dw_u32(&b, 4);                                  /* DBUS_NAME_FLAG_DO_NOT_QUEUE */
    call_add(call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", "su", &b, 1), C_NAME, 0);
    dw_free(&b);
    add_match("type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'");
    add_match("type='signal',interface='" ITEM_IFACE "'");
    return 0;
}

int sni_fd(void) { return up ? conn.fd : -1; }
int sni_wants_write(void) { return up && dbus_pending_out(&conn); }

void sni_dispatch(void) {
    if (!up) return;
    DMsg m;
    int r;
    while ((r = dbus_next(&conn, &m)) == 1) { handle(&m); dbus_msg_free(&m); }
    if (r < 0 || dbus_flush(&conn) < 0) {
        fprintf(stderr, "sni: the session bus went away\n");
        owned = 0;                                  /* nobody to tell */
        for (int i = 0; i < MAX_ITEMS; i++) if (items[i].id) item_drop(&items[i]);
        dbus_close(&conn);
        conn.fd = -1;
        up = 0;
        ncalls = nhosts = 0;
    }
}
