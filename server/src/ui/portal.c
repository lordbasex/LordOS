/* portal.c — our own org.freedesktop.portal.Settings, FileChooser, OpenURI, Inhibit and ScreenCast (without
 * xdg-desktop-portal), and org.freedesktop.ScreenSaver: GTK, GNOME (libadwaita) and Qt apps read from
 * here the dark/light mode, the accent color, the order of the title bar buttons and the GTK 3 theme,
 * and follow LordOS's changes live (SettingChanged); they open links and files the way LordOS does,
 * keep the screen on while a video plays and share a screen or a window the person picks. Uses Passwords' own D-Bus client (vault/dbus.c). See
 * docs/WAYLAND.md, "GTK and GNOME apps", and docs/FLATPAK.md, "Portals".
 *
 * GTK 3 outside Flatpak doesn't read the portal: it reads GSettings. lord-server gives the apps
 * GSETTINGS_BACKEND=keyfile and writes those same keys to ~/.config/glib-2.0/settings/keyfile, which
 * GLib watches: GTK 3 also changes live (without dconf or running gsettings). */
#include "portal.h"
#include "portalcheck.h"
#include "../hw/hw.h"
#include "../hw/jbuf.h"
#include "../fsops.h"
#include "../sec/appid.h"
#include "../sec/restrict.h"
#include "../fw/rules.h"
#include "../vault/dbus.h"
#include "../wl/wl.h"
#include "../cast/cast.h"
#include "../cast/castsm.h"
#include "../cast/castproto.h"

#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUS_NAME "org.freedesktop.portal.Desktop"
#define OBJ_PATH "/org/freedesktop/portal/desktop"
#define IFACE    "org.freedesktop.portal.Settings"

static DConn conn;
static int up;

/* valores actuales */
static int dark = 1;
static double acc[3] = { 0.04, 0.52, 1.0 };
static char acc_name[16] = "blue", buttons[64] = "close,minimize,maximize:", gtk_theme[64] = "Adwaita-dark";

enum { V_U, V_S, V_RGB };
typedef struct { const char *ns, *key; int type; } Key;
static const Key keys[] = {
    { "org.freedesktop.appearance", "color-scheme", V_U },
    { "org.freedesktop.appearance", "accent-color", V_RGB },
    { "org.freedesktop.appearance", "contrast", V_U },
    { "org.gnome.desktop.interface", "color-scheme", V_S },
    { "org.gnome.desktop.interface", "accent-color", V_S },
    { "org.gnome.desktop.interface", "gtk-theme", V_S },
    { "org.gnome.desktop.wm.preferences", "button-layout", V_S },
};
#define NKEYS (sizeof keys / sizeof keys[0])

static const char *str_of(int i) {
    switch (i) {
    case 3: return dark ? "prefer-dark" : "default";
    case 4: return acc_name;
    case 5: return gtk_theme;
    case 6: return buttons;
    }
    return "";
}

static const char *sig_of(int type) { return type == V_U ? "u" : type == V_S ? "s" : "(ddd)"; }

static void put_double(DBuf *w, double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    dw_u64(w, u);
}

/* the value (already inside a variant) */
static void put_value(DBuf *w, int i) {
    switch (keys[i].type) {
    case V_U: dw_u32(w, i == 0 ? (dark ? 1 : 2) : 0); break;
    case V_S: dw_str(w, str_of(i)); break;
    default: dw_struct(w); put_double(w, acc[0]); put_double(w, acc[1]); put_double(w, acc[2]); break;
    }
}

static void put_variant(DBuf *w, int i) {
    dw_variant(w, sig_of(keys[i].type));
    put_value(w, i);
}

static int find_key(const char *ns, const char *key) {
    for (size_t i = 0; i < NKEYS; i++) if (!strcmp(keys[i].ns, ns) && !strcmp(keys[i].key, key)) return (int)i;
    return -1;
}

/* ReadAll(as namespaces) → a{sa{sv}}: each pattern may end in * (empty = all) */
static void read_all(const DMsg *m) {
    DRd r;
    dr_init(&r, m);
    const char *pat[16];
    int np = 0;
    size_t end = dr_arr(&r, 4);
    while (dr_more(&r, end) && np < 16) pat[np++] = dr_str(&r, NULL);
    if (r.err) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.InvalidArgs", "namespaces"); return; }
    DBuf b;
    dw_init(&b);
    size_t outer = dw_arr_begin(&b, 8);
    const char *seen[8];
    int ns = 0;
    for (size_t i = 0; i < NKEYS; i++) {
        int dup = 0;
        for (int k = 0; k < ns; k++) if (!strcmp(seen[k], keys[i].ns)) dup = 1;
        if (dup) continue;
        int want = np == 0;
        for (int k = 0; k < np && !want; k++) want = !pat[k][0] || fnmatch(pat[k], keys[i].ns, 0) == 0;
        if (!want) continue;
        seen[ns++] = keys[i].ns;
        dw_struct(&b);
        dw_str(&b, keys[i].ns);
        size_t inner = dw_arr_begin(&b, 8);
        for (size_t j = i; j < NKEYS; j++) {
            if (strcmp(keys[j].ns, keys[i].ns)) continue;
            dw_struct(&b);
            dw_str(&b, keys[j].key);
            put_variant(&b, (int)j);
        }
        dw_arr_end(&b, inner);
    }
    dw_arr_end(&b, outer);
    dbus_reply(&conn, m, "a{sa{sv}}", &b);
    dw_free(&b);
}

/* Read (the old one: v with another v inside) and ReadOne (v) */
static void read_one(const DMsg *m, int nested) {
    DRd r;
    dr_init(&r, m);
    const char *ns = dr_str(&r, NULL), *key = dr_str(&r, NULL);
    int i = r.err ? -1 : find_key(ns, key);
    if (i < 0) { dbus_error(&conn, m, "org.freedesktop.portal.Error.NotFound", "no such key"); return; }
    DBuf b;
    dw_init(&b);
    if (nested) dw_variant(&b, "v");
    put_variant(&b, i);
    dbus_reply(&conn, m, "v", &b);
    dw_free(&b);
}

static const char introspect_xml[] =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    " <interface name=\"org.freedesktop.portal.Settings\">\n"
    "  <method name=\"ReadAll\"><arg type=\"as\" name=\"namespaces\" direction=\"in\"/><arg type=\"a{sa{sv}}\" name=\"value\" direction=\"out\"/></method>\n"
    "  <method name=\"Read\"><arg type=\"s\" name=\"namespace\" direction=\"in\"/><arg type=\"s\" name=\"key\" direction=\"in\"/><arg type=\"v\" name=\"value\" direction=\"out\"/></method>\n"
    "  <method name=\"ReadOne\"><arg type=\"s\" name=\"namespace\" direction=\"in\"/><arg type=\"s\" name=\"key\" direction=\"in\"/><arg type=\"v\" name=\"value\" direction=\"out\"/></method>\n"
    "  <signal name=\"SettingChanged\"><arg type=\"s\" name=\"namespace\"/><arg type=\"s\" name=\"key\"/><arg type=\"v\" name=\"value\"/></signal>\n"
    "  <property name=\"version\" type=\"u\" access=\"read\"/>\n"
    " </interface>\n"
    " <interface name=\"org.freedesktop.portal.FileChooser\">\n"
    "  <method name=\"OpenFile\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>\n"
    "  <method name=\"SaveFile\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>\n"
    "  <property name=\"version\" type=\"u\" access=\"read\"/>\n"
    " </interface>\n"
    " <interface name=\"org.freedesktop.portal.OpenURI\">\n"
    "  <method name=\"OpenURI\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>\n"
    "  <method name=\"OpenFile\"><arg type=\"s\" direction=\"in\"/><arg type=\"h\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>\n"
    "  <method name=\"OpenDirectory\"><arg type=\"s\" direction=\"in\"/><arg type=\"h\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>\n"
    "  <property name=\"version\" type=\"u\" access=\"read\"/>\n"
    " </interface>\n"
    " <interface name=\"org.freedesktop.portal.Inhibit\">\n"
    "  <method name=\"Inhibit\"><arg type=\"s\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>\n"
    "  <property name=\"version\" type=\"u\" access=\"read\"/>\n"
    " </interface>\n"
    " <interface name=\"org.freedesktop.portal.ScreenCast\">\n"
    "  <method name=\"CreateSession\"><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>\n"
    "  <method name=\"SelectSources\"><arg type=\"o\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>\n"
    "  <method name=\"Start\"><arg type=\"o\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>\n"
    "  <method name=\"OpenPipeWireRemote\"><arg type=\"o\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"h\" direction=\"out\"/></method>\n"
    "  <property name=\"AvailableSourceTypes\" type=\"u\" access=\"read\"/>\n"
    "  <property name=\"AvailableCursorModes\" type=\"u\" access=\"read\"/>\n"
    "  <property name=\"version\" type=\"u\" access=\"read\"/>\n"
    " </interface>\n"
    " <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "  <method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>\n"
    "  <method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>\n"
    " </interface>\n"
    " <interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\"><arg type=\"s\" direction=\"out\"/></method></interface>\n"
    " <interface name=\"org.freedesktop.DBus.Peer\"><method name=\"Ping\"/></interface>\n"
    "</node>\n";

/* ---- org.freedesktop.portal.FileChooser ----
 * For apps that have no file dialog of their own and ask the desktop for one. The shell draws it
 * (hw event "portal-file", sdk/filepicker.jsx) and answers with lord.portal.chosen. The answer goes
 * only to the app that asked, on the Request object it was given. It says version 2 on purpose: the
 * browsers want 3 before they leave their own dialog, so they keep it. */
#define FC_IFACE  "org.freedesktop.portal.FileChooser"
#define REQ_IFACE "org.freedesktop.portal.Request"
#define MAX_PICKS 8
#define MAX_PATTERNS 64

typedef struct { int id; char sender[256], handle[400]; char *json; } Pick;
static Pick picks[MAX_PICKS];
static int npicks, pick_seq;

/* The Request object a call gets: OBJ_PATH/request/SENDER/TOKEN, the form the spec gives, so an app
 * can listen for its Response before it calls. A token with other characters is cut to the allowed
 * ones; with none left it is ours. */
static void request_path(const char *sender, const char *token, int id, char *out, size_t n) {
    char who[256] = "", tok[65] = "";
    size_t w = 0;
    for (const char *s = sender + (sender[0] == ':'); *s && w + 1 < sizeof who; s++) who[w++] = *s == '.' ? '_' : *s;
    size_t t = 0;
    for (const char *s = token; *s && t + 1 < sizeof tok; s++)
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_') tok[t++] = *s;
    if (!t) snprintf(tok, sizeof tok, "lordos%d", id);
    snprintf(out, n, "%s/request/%s/%s", OBJ_PATH, who, tok);
}

/* Response(u code, a{sv} results) with no results: 0 done, 1 cancelled, 2 refused */
static void respond(const char *sender, const char *handle, uint32_t code) {
    if (!up) return;
    DBuf b;
    dw_init(&b);
    dw_u32(&b, code);
    size_t a = dw_arr_begin(&b, 8);
    dw_arr_end(&b, a);
    DHdr h = { .type = DBUS_SIGNAL, .path = handle, .iface = REQ_IFACE, .member = "Response", .dest = sender, .sig = "ua{sv}" };
    dbus_send(&conn, &h, &b);
    dw_free(&b);
    dbus_flush(&conn);
}

static void pick_show(void) { hw_emit("portal-file", npicks ? picks[0].json : "null"); }

static void pick_drop(int i) {
    free(picks[i].json);
    memmove(&picks[i], &picks[i + 1], sizeof picks[0] * (size_t)(npicks - i - 1));
    npicks--;
    if (i == 0) pick_show();
}

static void file_chooser(const DMsg *m, const char *mode) {
    if (!m->sig || strcmp(m->sig, "ssa{sv}") || !m->sender) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.InvalidArgs", "arguments"); return; }
    if (npicks >= MAX_PICKS) { dbus_error(&conn, m, "org.freedesktop.portal.Error.Failed", "too many requests"); return; }
    DRd r;
    dr_init(&r, m);
    /* the parent window: the dialog is the system's, over everything, and goes over that window when it
     * is a Wayland one the app exported (xdg-foreign-v2, "wayland:<handle>") */
    const char *parent = dr_str(&r, NULL);
    int over = parent && !strncmp(parent, "wayland:", 8) ? wl_foreign_surface(parent + 8) : 0;
    const char *title = dr_str(&r, NULL), *name = "", *token = "", *accept = "";
    char folder[1024] = "";
    int multiple = 0, directory = 0, every = 0, npat = 0;
    const char *pat[MAX_PATTERNS];
    size_t end = dr_arr(&r, 8);
    while (!r.err && dr_more(&r, end)) {
        dr_struct(&r);
        const char *k = dr_str(&r, NULL), *sig = dr_variant(&r);
        if (r.err) break;
        if (!strcmp(k, "handle_token") && !strcmp(sig, "s")) token = dr_str(&r, NULL);
        else if (!strcmp(k, "current_name") && !strcmp(sig, "s")) name = dr_str(&r, NULL);
        else if (!strcmp(k, "accept_label") && !strcmp(sig, "s")) accept = dr_str(&r, NULL);
        else if (!strcmp(k, "multiple") && !strcmp(sig, "b")) multiple = dr_bool(&r);
        else if (!strcmp(k, "directory") && !strcmp(sig, "b")) directory = dr_bool(&r);
        else if (!strcmp(k, "current_folder") && !strcmp(sig, "ay")) {
            uint32_t n = 0;
            const uint8_t *p = dr_bytes(&r, &n);
            if (p && n < sizeof folder) { memcpy(folder, p, n); folder[n] = 0; }
        } else if (!strcmp(k, "filters") && !strcmp(sig, "a(sa(us))")) {
            size_t fe = dr_arr(&r, 8);
            while (!r.err && dr_more(&r, fe)) {
                dr_struct(&r);
                dr_str(&r, NULL);
                size_t pe = dr_arr(&r, 8);
                while (!r.err && dr_more(&r, pe)) {
                    dr_struct(&r);
                    uint32_t kind = dr_u32(&r);
                    const char *g = dr_str(&r, NULL);
                    if (kind != 0) every = 1;       /* a type name: the dialog doesn't know types, it shows all */
                    else if (npat < MAX_PATTERNS) pat[npat++] = g;
                }
            }
        } else dr_skip(&r, sig);
    }
    if (r.err) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.InvalidArgs", "options"); return; }
    Pick *p = &picks[npicks];
    memset(p, 0, sizeof *p);
    p->id = ++pick_seq;
    snprintf(p->sender, sizeof p->sender, "%s", m->sender);
    request_path(m->sender, token, p->id, p->handle, sizeof p->handle);
    HBuf j = { 0 };
    hb_printf(&j, "{\"id\":%d,\"mode\":\"%s\",\"multiple\":%s,\"parent\":%d,\"title\":", p->id,
              directory ? "folder" : mode, multiple ? "true" : "false", over);
    hb_str(&j, title ? title : "");
    hb_printf(&j, ",\"name\":");   hb_str(&j, name);
    hb_printf(&j, ",\"accept\":"); hb_str(&j, accept);
    hb_printf(&j, ",\"folder\":"); hb_str(&j, folder);
    hb_printf(&j, ",\"patterns\":[");
    if (!every) for (int i = 0; i < npat; i++) { if (i) hb_printf(&j, ","); hb_str(&j, pat[i]); }
    hb_printf(&j, "]}");
    p->json = hb_take(&j);
    if (!p->json) { dbus_error(&conn, m, "org.freedesktop.portal.Error.Failed", "memory"); return; }
    npicks++;
    DBuf b;
    dw_init(&b);
    dw_str(&b, p->handle);
    dbus_reply(&conn, m, "o", &b);
    dw_free(&b);
    if (npicks == 1) pick_show();
}

static void uri_of(DBuf *b, const char *path) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(path), o = 7;
    char *u = malloc(n * 3 + 8);
    if (!u) return;
    memcpy(u, "file://", 7);
    for (const unsigned char *s = (const unsigned char *)path; *s; s++) {
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr("/-._~", *s)) u[o++] = (char)*s;
        else { u[o++] = '%'; u[o++] = hex[*s >> 4]; u[o++] = hex[*s & 15]; }
    }
    u[o] = 0;
    dw_str(b, u);
    free(u);
}

void portal_file_chosen(int id, const char *const *paths, int n) {
    int i = 0;
    while (i < npicks && picks[i].id != id) i++;
    if (i == npicks) return;
    if (up) {
        DBuf b;
        dw_init(&b);
        dw_u32(&b, n > 0 ? 0 : 1);          /* 0 success, 1 cancelled */
        size_t a = dw_arr_begin(&b, 8);
        if (n > 0) {
            dw_struct(&b);
            dw_str(&b, "uris");
            dw_variant(&b, "as");
            size_t u = dw_arr_begin(&b, 4);
            for (int k = 0; k < n; k++) uri_of(&b, paths[k]);
            dw_arr_end(&b, u);
        }
        dw_arr_end(&b, a);
        DHdr h = { .type = DBUS_SIGNAL, .path = picks[i].handle, .iface = REQ_IFACE, .member = "Response",
                   .dest = picks[i].sender, .sig = "ua{sv}" };
        dbus_send(&conn, &h, &b);
        dw_free(&b);
        dbus_flush(&conn);
    }
    pick_drop(i);
}

/* ---- org.freedesktop.portal.OpenURI ----
 * An app asks the desktop to open a web address or a file, or to show a file in Files. The shell opens
 * it the way LordOS opens everything else (hw event "portal-open", sdk/portalopenmodel.js: the default
 * browser, our own apps, the default app for the type) and answers with lord.portal.opened. When the
 * app that asks is not the one with the keyboard, the person is asked first, as other desktops do: an
 * app in the background can't open things on its own. OpenFile and OpenDirectory get the file as an
 * fd: only its name is used (nothing is read or written through it, and it is closed with the
 * message), and that name must still lead to the same file. The file is opened by its name later, after
 * the person answers, so the shell asks lord.portal.still(id) right before: the same device, inode and
 * type, the same real path, still allowed; a file swapped in the meantime is refused. A web address is
 * handed on only when every URL parser reads its host the same way (portalcheck.c, portal_web_host), so
 * the question names the host the browser goes to. */
#define OU_IFACE  "org.freedesktop.portal.OpenURI"
#define MAX_OPENS 8
#define URI_MAX   4096

enum { O_URI = PORTAL_URI, O_FILE = PORTAL_FILE, O_FOLDER = PORTAL_FOLDER, O_REVEAL = PORTAL_REVEAL };
static const char *const open_kinds[] = { "uri", "file", "folder", "reveal" };
/* seen: the local file as it was when the app asked, checked again before the shell opens it (portal_open_still) */
typedef struct { int id, kind, shown; uint32_t serial; char sender[256], handle[400]; char *target; PortalSeen seen; } Open;
static Open opens[MAX_OPENS];
static int nopens, open_seq;

static void open_drop(int i, int tell) {
    if (tell && opens[i].shown) {
        char j[32];
        snprintf(j, sizeof j, "{\"id\":%d}", opens[i].id);
        hw_emit("portal-open-gone", j);
    }
    free(opens[i].target);
    memmove(&opens[i], &opens[i + 1], sizeof opens[0] * (size_t)(nopens - i - 1));
    nopens--;
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* file:///path or file://localhost/path, %XX decoded (a NUL is refused) */
static int file_uri_kind(const char *uri, char *real, PortalSeen *seen) {
    const char *p = uri + 7;
    if (!strncasecmp(p, "localhost/", 10)) p += 9;
    if (*p != '/') return -1;
    char name[PATH_MAX];
    size_t o = 0;
    for (; *p && *p != '?' && *p != '#'; p++) {
        if (o + 1 >= sizeof name) return -1;
        if (*p != '%') { name[o++] = *p; continue; }
        int hi = hexval(p[1]), lo = hi < 0 ? -1 : hexval(p[2]);
        if (lo < 0 || (hi | lo) == 0) return -1;
        name[o++] = (char)(hi * 16 + lo);
        p += 2;
    }
    name[o] = 0;
    return portal_file_kind(name, O_FILE, real, seen);
}

/* a{sv} options: only handle_token is read (writable, ask and activation_token are not needed here) */
static const char *opt_token(DRd *r) {
    const char *token = "";
    size_t end = dr_arr(r, 8);
    while (!r->err && dr_more(r, end)) {
        dr_struct(r);
        const char *k = dr_str(r, NULL), *sig = dr_variant(r);
        if (r->err) break;
        if (!strcmp(k, "handle_token") && !strcmp(sig, "s")) token = dr_str(r, NULL);
        else dr_skip(r, sig);
    }
    return r->err ? "" : token;
}

/* The bus said which process asked (GetConnectionUnixProcessID): it is in front when it is the process
 * with the keyboard or in the same app scope. Only now does the shell hear of it. */
static void open_show(Open *o, unsigned pid) {
    int fp = wl_focus_pid();
    char who[128] = "", front_app[128] = "", comm[32] = "";
    int front = pid && fp > 0 && (int)pid == fp;
    if (pid && appid_of_pid(pid, who, sizeof who) != 0) who[0] = 0;
    if (!front && who[0] && fp > 0 && appid_of_pid((unsigned)fp, front_app, sizeof front_app) == 0) front = !strcmp(who, front_app);
    if (pid) {
        char path[48];
        snprintf(path, sizeof path, "/proc/%u/comm", pid);
        FILE *f = fopen(path, "re");
        if (f) { if (!fgets(comm, sizeof comm, f)) comm[0] = 0; fclose(f); }
        comm[strcspn(comm, "\n")] = 0;
    }
    HBuf j = { 0 };
    hb_printf(&j, "{\"id\":%d,\"kind\":\"%s\",\"front\":%s,\"target\":", o->id, open_kinds[o->kind], front ? "true" : "false");
    hb_str(&j, o->target);
    hb_printf(&j, ",\"who\":");  hb_str(&j, who);
    hb_printf(&j, ",\"comm\":"); hb_str(&j, comm);
    hb_printf(&j, "}");
    char *json = hb_take(&j);
    o->shown = 1;
    if (json) hw_emit("portal-open", json);
    else { respond(o->sender, o->handle, 2); open_drop((int)(o - opens), 0); }
    free(json);
}

static void open_call(const DMsg *m, int by_fd, int reveal) {
    if (!m->sig || strcmp(m->sig, by_fd ? "sha{sv}" : "ssa{sv}") || !m->sender) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.InvalidArgs", "arguments"); return; }
    if (nopens >= MAX_OPENS) { dbus_error(&conn, m, "org.freedesktop.portal.Error.Failed", "too many requests"); return; }
    DRd r;
    dr_init(&r, m);
    dr_str(&r, NULL);                       /* the parent window: the question is the system's, over everything */
    char real[PATH_MAX] = "", host[256];
    const char *target = real;
    int kind = O_URI;
    PortalSeen seen = { 0 };
    if (by_fd) {
        uint32_t i = dr_u32(&r);
        if (r.err || i >= (uint32_t)m->n_fds) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.InvalidArgs", "fd"); return; }
        kind = portal_fd_kind(m->fds[i], reveal ? O_REVEAL : O_FILE, real, &seen);
    } else {
        uint32_t n = 0;
        const char *uri = dr_str(&r, &n);
        int ok = !r.err && n > 0 && n <= URI_MAX;
        for (uint32_t i = 0; ok && i < n; i++) ok = (unsigned char)uri[i] > 0x20 && uri[i] != 0x7f;
        if (!ok) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.InvalidArgs", "uri"); return; }
        if (!strncasecmp(uri, "file://", 7)) kind = file_uri_kind(uri, real, &seen);
        else if (portal_web_host(uri, host, sizeof host)) { dbus_error(&conn, m, "org.freedesktop.portal.Error.NotAllowed", "not a web address apps may open"); return; }
        else target = uri;
    }
    const char *token = opt_token(&r);
    if (r.err) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.InvalidArgs", "options"); return; }
    if (kind < 0) { dbus_error(&conn, m, "org.freedesktop.portal.Error.NotAllowed", "not a file apps may open"); return; }
    Open *o = &opens[nopens];
    memset(o, 0, sizeof *o);
    o->id = ++open_seq;
    o->kind = kind;
    o->seen = seen;
    snprintf(o->sender, sizeof o->sender, "%s", m->sender);
    request_path(m->sender, token, o->id, o->handle, sizeof o->handle);
    if (!(o->target = strdup(target))) { dbus_error(&conn, m, "org.freedesktop.portal.Error.Failed", "memory"); return; }
    nopens++;
    DBuf b;
    dw_init(&b);
    dw_str(&b, o->handle);
    dbus_reply(&conn, m, "o", &b);
    dw_free(&b);
    /* who asked: the bus answers from the socket's credentials, never the app */
    dw_init(&b);
    dw_str(&b, m->sender);
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus",
               .member = "GetConnectionUnixProcessID", .dest = "org.freedesktop.DBus", .sig = "s" };
    o->serial = dbus_send(&conn, &h, &b);
    dw_free(&b);
    if (!o->serial) open_show(o, 0);        /* unknown: treated as an app in the background */
}

int portal_open_still(int id) {
    for (int i = 0; i < nopens; i++)
        if (opens[i].id == id && opens[i].shown)
            return opens[i].kind == O_URI || portal_file_still(opens[i].target, opens[i].kind, &opens[i].seen);
    return 0;
}

void portal_open_done(int id, int code) {
    for (int i = 0; i < nopens; i++)
        if (opens[i].id == id && opens[i].shown) {
            respond(opens[i].sender, opens[i].handle, code == 0 ? 0 : code == 1 ? 1 : 2);
            open_drop(i, 0);
            return;
        }
}

/* ---- org.freedesktop.portal.Inhibit and org.freedesktop.ScreenSaver ----
 * A video player, a presentation or a long copy asks for the screen to stay on and the computer not to
 * sleep: while one of these holds is on, it counts as the compositor's own idle inhibition
 * (zwp_idle_inhibit_v1, main.c), which keeps the screen-off, lock and sleep timers from running. The
 * portal's hold ends with Request.Close on its handle, the ScreenSaver one with UnInhibit(cookie),
 * and both when the app's connection leaves the bus (NameOwnerChanged). Logout and user-switch holds
 * are accepted and do nothing. */
#define INH_IFACE     "org.freedesktop.portal.Inhibit"
#define SS_NAME       "org.freedesktop.ScreenSaver"
#define SS_IFACE      "org.freedesktop.ScreenSaver"
#define MAX_HOLDS     64
#define HOLDS_PER_APP 8
enum { INH_SUSPEND = 4, INH_IDLE = 8 };

typedef struct { char sender[256], handle[400]; uint32_t cookie, flags; } Hold;
static Hold holds[MAX_HOLDS];
static int nholds;
static uint32_t cookie_seq;

static void hold_drop(int i) {
    memmove(&holds[i], &holds[i + 1], sizeof holds[0] * (size_t)(nholds - i - 1));
    nholds--;
}

static Hold *hold_add(const char *sender, uint32_t flags) {
    int mine = 0;
    for (int i = 0; i < nholds; i++) mine += !strcmp(holds[i].sender, sender);
    if (nholds >= MAX_HOLDS || mine >= HOLDS_PER_APP) return NULL;
    Hold *h = &holds[nholds++];
    memset(h, 0, sizeof *h);
    snprintf(h->sender, sizeof h->sender, "%s", sender);
    h->flags = flags;
    return h;
}

int portal_inhibited(void) {
    for (int i = 0; i < nholds; i++) if (holds[i].flags & (INH_SUSPEND | INH_IDLE)) return 1;
    return 0;
}

static void inhibit(const DMsg *m) {
    if (!m->sig || strcmp(m->sig, "sua{sv}") || !m->sender) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.InvalidArgs", "arguments"); return; }
    DRd r;
    dr_init(&r, m);
    dr_str(&r, NULL);                       /* the window: the hold is the app's, shown or not */
    uint32_t flags = dr_u32(&r);
    const char *token = opt_token(&r);
    if (r.err) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.InvalidArgs", "options"); return; }
    char handle[400];
    request_path(m->sender, token, (int)++cookie_seq, handle, sizeof handle);
    Hold *h = NULL;
    for (int i = 0; i < nholds && !h; i++)  /* the same token twice: one hold */
        if (!strcmp(holds[i].handle, handle) && !strcmp(holds[i].sender, m->sender)) { h = &holds[i]; h->flags = flags; }
    if (!h && (h = hold_add(m->sender, flags))) snprintf(h->handle, sizeof h->handle, "%s", handle);
    if (!h) { dbus_error(&conn, m, "org.freedesktop.portal.Error.Failed", "too many requests"); return; }
    DBuf b;
    dw_init(&b);
    dw_str(&b, h->handle);
    dbus_reply(&conn, m, "o", &b);
    dw_free(&b);
    respond(h->sender, h->handle, 0);
}

static const char ss_xml[] =
    "<node><interface name=\"org.freedesktop.ScreenSaver\">"
    "<method name=\"Inhibit\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/></method>"
    "<method name=\"UnInhibit\"><arg type=\"u\" direction=\"in\"/></method>"
    "<method name=\"GetActive\"><arg type=\"b\" direction=\"out\"/></method>"
    "</interface></node>";

static void screensaver(const DMsg *m) {
    const char *iface = m->iface ? m->iface : SS_IFACE, *sig = m->sig ? m->sig : "";
    if (strcmp(iface, SS_IFACE) || !m->sender) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method"); return; }
    DBuf b;
    dw_init(&b);
    if (!strcmp(m->member, "Inhibit") && !strcmp(sig, "ss")) {
        Hold *h = hold_add(m->sender, INH_IDLE);
        if (h) {
            if (!++cookie_seq) ++cookie_seq;
            h->cookie = cookie_seq;
            dw_u32(&b, h->cookie);
            dbus_reply(&conn, m, "u", &b);
        } else dbus_error(&conn, m, "org.freedesktop.DBus.Error.LimitsExceeded", "too many holds");
    } else if (!strcmp(m->member, "UnInhibit") && !strcmp(sig, "u")) {
        DRd r;
        dr_init(&r, m);
        uint32_t cookie = dr_u32(&r);
        for (int i = 0; i < nholds; i++)    /* only the app that took it can let it go */
            if (cookie && holds[i].cookie == cookie && !strcmp(holds[i].sender, m->sender)) { hold_drop(i); break; }
        dbus_reply(&conn, m, NULL, NULL);
    } else if (!strcmp(m->member, "GetActive") && !sig[0]) {
        dw_bool(&b, 0);
        dbus_reply(&conn, m, "b", &b);
    } else dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
    dw_free(&b);
}

/* ---- org.freedesktop.portal.ScreenCast ----
 * A browser's video call, a recorder or a chat app asks to share "a window or the entire screen". The sessions follow
 * cast/castsm.c: CreateSession, SelectSources (monitor and window, one or several, the pointer hidden or drawn in),
 * Start and OpenPipeWireRemote. Start shows LordOS's own chooser (hw event "portal-cast", sdk/screencast.jsx): the
 * displays and the open windows, with pictures, and the name of the app asking; nothing is shared until the person
 * picks, and Cancel answers 1. Then lord-screencast (cast/cast.c) makes one PipeWire stream per source and the app gets
 * their nodes; OpenPipeWireRemote gives it a PipeWire connection that sees only those. While a session runs the shell
 * shows the sharing indicator with its Stop (hw event "portal-casts"); no frame goes before it is on screen
 * (lord.portal.castShown). Stop, a lock, the app's Close or the app leaving the bus end it, and the app hears
 * Session.Closed when it wasn't the one closing. persist_mode is accepted and nothing is kept: no restore_token is
 * ever given, so the person picks every time. The cursor as metadata isn't offered. */
#define SC_IFACE   "org.freedesktop.portal.ScreenCast"
#define SESS_IFACE "org.freedesktop.portal.Session"
#define SC_VERSION 4
#define MAX_RCALLS 8
#define THUMB_W    320
_Static_assert((int)CSM_TYPE_MONITOR == (int)CAST_SRC_MONITOR && (int)CSM_TYPE_WINDOW == (int)CAST_SRC_WINDOW, "the portal's types are the helper's");
_Static_assert((int)CSM_MAX_SOURCES == CAST_MAX_STREAMS, "one stream per source");

static CastSm csm;
/* per session (the same index as csm.s): who asked, and the bus's answer we wait for */
static struct { char who[128], comm[32]; uint32_t serial; } cast_app[CSM_MAX];
/* OpenPipeWireRemote calls answered when the helper hands the connection back */
typedef struct { int sess; uint32_t req, serial; char sender[256]; } RCall;
static RCall rcalls[MAX_RCALLS];
static int nrcalls;
static uint32_t rcall_seq;

static int cast_idx(const CsSess *s) { return (int)(s - csm.s); }

static void cast_error(const DMsg *m, int e) {
    const char *name = e == CSM_E_ARGS ? "org.freedesktop.DBus.Error.InvalidArgs" : e == CSM_E_NOTFOUND ? "org.freedesktop.portal.Error.NotFound"
                     : "org.freedesktop.portal.Error.Failed";
    const char *text = e == CSM_E_STATE ? "not now in this session" : e == CSM_E_FULL ? "too many sessions" : e == CSM_E_BUSY ? "another app is asking"
                     : e == CSM_E_NOTFOUND ? "no such session" : "arguments";
    dbus_error(&conn, m, name, text);
}

/* the options the ScreenCast calls take */
typedef struct { const char *token, *session_token; uint32_t types, cursor, persist; int multiple; } CastOpts;

static int cast_opts(DRd *r, CastOpts *o) {
    memset(o, 0, sizeof *o);
    o->token = o->session_token = "";
    size_t end = dr_arr(r, 8);
    while (!r->err && dr_more(r, end)) {
        dr_struct(r);
        const char *k = dr_str(r, NULL), *sig = dr_variant(r);
        if (r->err) break;
        int is_s = !strcmp(sig, "s"), is_u = !strcmp(sig, "u"), is_b = !strcmp(sig, "b");
        if (!strcmp(k, "handle_token") && is_s) o->token = dr_str(r, NULL);
        else if (!strcmp(k, "session_handle_token") && is_s) o->session_token = dr_str(r, NULL);
        else if (!strcmp(k, "types") && is_u) o->types = dr_u32(r);
        else if (!strcmp(k, "cursor_mode") && is_u) o->cursor = dr_u32(r);
        else if (!strcmp(k, "persist_mode") && is_u) o->persist = dr_u32(r);
        else if (!strcmp(k, "multiple") && is_b) o->multiple = dr_bool(r);
        else if (!strcmp(k, "types") || !strcmp(k, "cursor_mode") || !strcmp(k, "persist_mode") || !strcmp(k, "multiple")) return -1;
        else dr_skip(r, sig);           /* restore_token and the rest: nothing is restored */
    }
    return r->err ? -1 : 0;
}

static void reply_handle(const DMsg *m, const char *handle) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, handle);
    dbus_reply(&conn, m, "o", &b);
    dw_free(&b);
}

/* The shell's indicator: the sessions sharing now and who they are for */
static void casts_changed(void) {
    HBuf j = { 0 };
    hb_printf(&j, "[");
    int first = 1;
    for (int i = 0; i < CSM_MAX; i++) {
        if (csm.s[i].st != CS_STARTED) continue;
        hb_printf(&j, "%s{\"id\":%d,\"who\":", first ? "" : ",", csm.s[i].id);
        hb_str(&j, cast_app[i].who);
        hb_printf(&j, ",\"comm\":");
        hb_str(&j, cast_app[i].comm);
        hb_printf(&j, "}");
        first = 0;
    }
    hb_printf(&j, "]");
    char *json = hb_take(&j);
    if (json) hw_emit("portal-casts", json);
    free(json);
}

static void chooser_gone(int id) {
    char j[32];
    snprintf(j, sizeof j, "{\"id\":%d}", id);
    hw_emit("portal-cast-gone", j);
}

static void rcall_drop(int i, int fd) {
    RCall *c = &rcalls[i];
    if (up) {
        if (fd >= 0) {
            DBuf b;
            dw_init(&b);
            dw_u32(&b, 0);              /* the fd's index */
            DHdr h = { .type = DBUS_RETURN, .reply_serial = c->serial, .dest = c->sender, .sig = "h" };
            if (!dbus_send_fd(&conn, &h, &b, fd)) fd = -1;
            dw_free(&b);
        }
        if (fd < 0) {
            DBuf b;
            dw_init(&b);
            dw_str(&b, "no connection to share through");
            DHdr h = { .type = DBUS_ERROR, .reply_serial = c->serial, .dest = c->sender, .error = "org.freedesktop.portal.Error.Failed", .sig = "s" };
            dbus_send(&conn, &h, &b);
            dw_free(&b);
        }
        dbus_flush(&conn);
    }
    memmove(&rcalls[i], &rcalls[i + 1], sizeof rcalls[0] * (size_t)(nrcalls - i - 1));
    nrcalls--;
}

/* A session ends here. closed: the app didn't close it itself, so it hears Session.Closed (and a Start it waits for
 * hears response 2). */
static void cast_close(CsSess *s, int closed) {
    int id = s->id;
    if (s->st == CS_CHOOSING) chooser_gone(id);
    if (closed && (s->st == CS_CHOOSING || s->st == CS_STARTING)) respond(s->sender, s->req, 2);
    int was = s->st == CS_STARTED;
    cast_end(id);
    for (int i = nrcalls; i-- > 0;) if (rcalls[i].sess == id) rcall_drop(i, -1);
    if (closed && up) {
        DBuf b;
        dw_init(&b);
        size_t a = dw_arr_begin(&b, 8);
        dw_arr_end(&b, a);
        DHdr h = { .type = DBUS_SIGNAL, .path = s->path, .iface = SESS_IFACE, .member = "Closed", .dest = s->sender, .sig = "a{sv}" };
        dbus_send(&conn, &h, &b);
        dw_free(&b);
        dbus_flush(&conn);
    }
    memset(&cast_app[cast_idx(s)], 0, sizeof cast_app[0]);
    csm_end(s);
    if (was) casts_changed();
}

static void sc_create(const DMsg *m) {
    if (!m->sig || strcmp(m->sig, "a{sv}") || !m->sender) { cast_error(m, CSM_E_ARGS); return; }
    DRd r;
    dr_init(&r, m);
    CastOpts o;
    if (cast_opts(&r, &o)) { cast_error(m, CSM_E_ARGS); return; }
    CsSess *s = NULL;
    int e = csm_create(&csm, m->sender, o.session_token, &s);
    if (e) { cast_error(m, e); return; }
    memset(&cast_app[cast_idx(s)], 0, sizeof cast_app[0]);
    char handle[400];
    request_path(m->sender, o.token, s->id, handle, sizeof handle);
    reply_handle(m, handle);
    DBuf b;
    dw_init(&b);
    dw_u32(&b, 0);
    size_t a = dw_arr_begin(&b, 8);
    dw_struct(&b);
    dw_str(&b, "session_handle");
    dw_variant(&b, "s");
    dw_str(&b, s->path);
    dw_arr_end(&b, a);
    DHdr h = { .type = DBUS_SIGNAL, .path = handle, .iface = REQ_IFACE, .member = "Response", .dest = m->sender, .sig = "ua{sv}" };
    dbus_send(&conn, &h, &b);
    dw_free(&b);
    dbus_flush(&conn);
}

/* SelectSources(o, a{sv}), Start(o, s, a{sv}) and OpenPipeWireRemote(o, a{sv}): the session, the sender's, and the
 * options */
static CsSess *sc_session(const DMsg *m, const char *sig, DRd *r, CastOpts *o) {
    if (!m->sig || strcmp(m->sig, sig) || !m->sender) { cast_error(m, CSM_E_ARGS); return NULL; }
    dr_init(r, m);
    const char *path = dr_str(r, NULL);
    if (sig[1] == 's') dr_str(r, NULL);     /* Start's parent window: the chooser is the system's, over everything */
    if (r->err || cast_opts(r, o)) { cast_error(m, CSM_E_ARGS); return NULL; }
    CsSess *s = csm_find(&csm, m->sender, path);
    if (!s) cast_error(m, CSM_E_NOTFOUND);
    return s;
}

static void sc_select(const DMsg *m) {
    DRd r;
    CastOpts o;
    CsSess *s = sc_session(m, "oa{sv}", &r, &o);
    if (!s) return;
    int e = csm_select(&csm, s, o.types, o.multiple, o.cursor, o.persist);
    if (e) { cast_error(m, e); return; }
    char handle[400];
    request_path(m->sender, o.token, s->id, handle, sizeof handle);
    reply_handle(m, handle);
    respond(m->sender, handle, 0);
}

static void sc_start(const DMsg *m) {
    DRd r;
    CastOpts o;
    CsSess *s = sc_session(m, "osa{sv}", &r, &o);
    if (!s) return;
    char handle[400];
    request_path(m->sender, o.token, s->id, handle, sizeof handle);
    int e = csm_start(&csm, s, handle);
    if (e) { cast_error(m, e); return; }
    reply_handle(m, handle);
    if (!cast_available()) {                /* no helper: nothing can be shared */
        respond(m->sender, handle, 2);
        csm_chosen(&csm, s, NULL, 0);
        return;
    }
    /* who asked: the bus answers from the socket's credentials, never the app; the chooser waits for it */
    DBuf b;
    dw_init(&b);
    dw_str(&b, m->sender);
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus",
               .member = "GetConnectionUnixProcessID", .dest = "org.freedesktop.DBus", .sig = "s" };
    cast_app[cast_idx(s)].serial = dbus_send(&conn, &h, &b);
    dw_free(&b);
    if (!cast_app[cast_idx(s)].serial) { respond(m->sender, handle, 2); csm_chosen(&csm, s, NULL, 0); }
}

/* The bus said which process asked: the chooser goes up, unless the firewall keeps this app from the screen. */
static void cast_ask(CsSess *s, unsigned pid) {
    int i = cast_idx(s);
    cast_app[i].serial = 0;
    char *who = cast_app[i].who, *comm = cast_app[i].comm;
    who[0] = comm[0] = 0;
    if (pid && appid_of_pid(pid, who, sizeof cast_app[i].who) != 0) who[0] = 0;
    if (pid) {
        char path[48];
        snprintf(path, sizeof path, "/proc/%u/comm", pid);
        FILE *f = fopen(path, "re");
        if (f) { if (!fgets(comm, sizeof cast_app[i].comm, f)) comm[0] = 0; fclose(f); }
        comm[strcspn(comm, "\n")] = 0;
    }
    if (who[0] && restrict_blocks(who, FW_R_NO_SCREEN)) {
        fprintf(stderr, "portal: %s may not share the screen (its restrictions)\n", who);
        respond(s->sender, s->req, 2);
        csm_chosen(&csm, s, NULL, 0);
        return;
    }
    HBuf j = { 0 };
    hb_printf(&j, "{\"id\":%d,\"monitor\":%s,\"window\":%s,\"multiple\":%s,\"who\":", s->id, s->types & CSM_TYPE_MONITOR ? "true" : "false",
              s->types & CSM_TYPE_WINDOW ? "true" : "false", s->multiple ? "true" : "false");
    hb_str(&j, who);
    hb_printf(&j, ",\"comm\":");
    hb_str(&j, comm);
    hb_printf(&j, "}");
    char *json = hb_take(&j);
    if (json) hw_emit("portal-cast", json);
    else { respond(s->sender, s->req, 2); csm_chosen(&csm, s, NULL, 0); }
    free(json);
}

static void sc_remote(const DMsg *m) {
    DRd r;
    CastOpts o;
    CsSess *s = sc_session(m, "oa{sv}", &r, &o);
    if (!s) return;
    if (!csm_may_remote(s)) { cast_error(m, CSM_E_STATE); return; }
    if (nrcalls >= MAX_RCALLS || !conn.unix_fds) { cast_error(m, CSM_E_FULL); return; }
    if (!++rcall_seq) ++rcall_seq;
    if (cast_remote(s->id, rcall_seq)) { dbus_error(&conn, m, "org.freedesktop.portal.Error.Failed", "no connection to share through"); return; }
    RCall *c = &rcalls[nrcalls++];
    c->sess = s->id;
    c->req = rcall_seq;
    c->serial = m->serial;
    snprintf(c->sender, sizeof c->sender, "%s", m->sender);
}

/* Session.Close, and its version, on the session's own object */
static void sc_session_call(const DMsg *m) {
    const char *iface = m->iface ? m->iface : "", *mem = m->member;
    CsSess *s = csm_find(&csm, m->sender, m->path);
    if (!s) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownObject", "no such session"); return; }
    if ((!strcmp(iface, SESS_IFACE) || !iface[0]) && !strcmp(mem, "Close")) {
        cast_close(s, 0);
        dbus_reply(&conn, m, NULL, NULL);
        return;
    }
    if (!strcmp(iface, "org.freedesktop.DBus.Properties") && (!strcmp(mem, "Get") || !strcmp(mem, "GetAll"))) {
        DBuf b;
        dw_init(&b);
        if (!strcmp(mem, "Get")) {
            dw_variant(&b, "u");
            dw_u32(&b, 1);
            dbus_reply(&conn, m, "v", &b);
        } else {
            size_t a = dw_arr_begin(&b, 8);
            dw_struct(&b);
            dw_str(&b, "version");
            dw_variant(&b, "u");
            dw_u32(&b, 1);
            dw_arr_end(&b, a);
            dbus_reply(&conn, m, "a{sv}", &b);
        }
        dw_free(&b);
        return;
    }
    dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
}

static int is_session_path(const char *p) { return p && !strncmp(p, OBJ_PATH "/session/", sizeof OBJ_PATH "/session/" - 1); }

/* ScreenCast's properties: the source types only when the helper is there */
static void sc_prop(DBuf *b, const char *name) {
    uint32_t v = !strcmp(name, "AvailableSourceTypes") ? (cast_available() && conn.unix_fds ? CSM_TYPES : 0)
               : !strcmp(name, "AvailableCursorModes") ? CSM_CURSORS : SC_VERSION;
    dw_variant(b, "u");
    dw_u32(b, v);
}

void portal_cast_chosen(int id, const CastSource *src, int n) {
    CsSess *s = csm_by_id(&csm, id);
    if (!s || s->st != CS_CHOOSING) return;
    char req[CSM_PATH_MAX];
    snprintf(req, sizeof req, "%s", s->req);
    uint32_t kinds[CSM_MAX_SOURCES];
    if (n < 0) n = 0;
    for (int i = 0; i < n && i < CSM_MAX_SOURCES; i++) kinds[i] = (uint32_t)src[i].kind;
    int e = csm_chosen(&csm, s, kinds, n);
    if (e) { respond(s->sender, req, 2); csm_chosen(&csm, s, NULL, 0); return; }
    if (n == 0) { respond(s->sender, req, 1); return; }        /* Cancel */
    if (cast_begin(id, src, n, s->cursor == CSM_CURSOR_EMBEDDED)) { respond(s->sender, req, 2); csm_failed(s); }
}

void portal_cast_streams(int id, const CastStreamInfo *info, int n) {
    CsSess *s = csm_by_id(&csm, id);
    if (!s || s->st != CS_STARTING || !up) { cast_end(id); return; }
    char req[CSM_PATH_MAX];
    snprintf(req, sizeof req, "%s", s->req);
    csm_started(s);
    DBuf b;
    dw_init(&b);
    dw_u32(&b, 0);
    size_t a = dw_arr_begin(&b, 8);
    dw_struct(&b);
    dw_str(&b, "streams");
    dw_variant(&b, "a(ua{sv})");
    size_t sa = dw_arr_begin(&b, 8);
    for (int i = 0; i < n; i++) {
        dw_struct(&b);
        dw_u32(&b, info[i].node);
        size_t pa = dw_arr_begin(&b, 8);
        if (info[i].kind == CAST_SRC_MONITOR) {     /* where a display sits; a window's place isn't the app's business */
            dw_struct(&b);
            dw_str(&b, "position");
            dw_variant(&b, "(ii)");
            dw_struct(&b);
            dw_i32(&b, info[i].x);
            dw_i32(&b, info[i].y);
        }
        dw_struct(&b);
        dw_str(&b, "size");
        dw_variant(&b, "(ii)");
        dw_struct(&b);
        dw_i32(&b, info[i].w);
        dw_i32(&b, info[i].h);
        dw_struct(&b);
        dw_str(&b, "source_type");
        dw_variant(&b, "u");
        dw_u32(&b, (uint32_t)info[i].kind);
        dw_arr_end(&b, pa);
    }
    dw_arr_end(&b, sa);
    dw_arr_end(&b, a);
    DHdr h = { .type = DBUS_SIGNAL, .path = req, .iface = REQ_IFACE, .member = "Response", .dest = s->sender, .sig = "ua{sv}" };
    dbus_send(&conn, &h, &b);
    dw_free(&b);
    dbus_flush(&conn);
    int i = cast_idx(s);
    fprintf(stderr, "portal: %s is sharing %d source%s\n", cast_app[i].who[0] ? cast_app[i].who : cast_app[i].comm, n, n == 1 ? "" : "s");
    casts_changed();
}

void portal_cast_failed(int id) {
    CsSess *s = csm_by_id(&csm, id);
    if (!s || s->st != CS_STARTING) return;
    respond(s->sender, s->req, 2);
    csm_failed(s);
}

void portal_cast_lost(int id) {
    CsSess *s = csm_by_id(&csm, id);
    if (s) cast_close(s, 1);
}

void portal_cast_remote(int id, uint32_t req, int fd) {
    for (int i = 0; i < nrcalls; i++)
        if (rcalls[i].sess == id && rcalls[i].req == req) { rcall_drop(i, fd); break; }
    if (fd >= 0) close(fd);
}

void portal_cast_stop(int id) {
    CsSess *s = csm_by_id(&csm, id);
    if (s && s->st == CS_STARTED) cast_close(s, 1);
}

void portal_cast_shown(int id) {
    CsSess *s = csm_by_id(&csm, id);
    if (s && s->st == CS_STARTED) cast_shown(id);
}

int portal_cast_thumb(int id, const CastSource *src) {
    CsSess *s = csm_by_id(&csm, id);
    return s && s->st == CS_CHOOSING ? cast_thumb(src, THUMB_W) : 0;
}

/* ---- the bus: who asked, and who left ---- */
static void on_reply(const DMsg *m) {
    if (!m->sender || strcmp(m->sender, "org.freedesktop.DBus")) return;
    for (int i = 0; i < CSM_MAX; i++) {
        if (csm.s[i].st != CS_CHOOSING || !cast_app[i].serial || cast_app[i].serial != m->reply_serial) continue;
        uint32_t pid = 0;
        if (m->type == DBUS_RETURN && m->sig && !strcmp(m->sig, "u")) { DRd r; dr_init(&r, m); pid = dr_u32(&r); if (r.err) pid = 0; }
        cast_ask(&csm.s[i], pid);
        return;
    }
    for (int i = 0; i < nopens; i++) {
        if (opens[i].shown || opens[i].serial != m->reply_serial) continue;
        uint32_t pid = 0;
        if (m->type == DBUS_RETURN && m->sig && !strcmp(m->sig, "u")) { DRd r; dr_init(&r, m); pid = dr_u32(&r); if (r.err) pid = 0; }
        open_show(&opens[i], pid);
        return;
    }
}

/* NameOwnerChanged(name, old, new) with no new owner: that connection is gone, and so are its holds,
 * its open requests, its file dialogs and its screen casts */
static void on_signal(const DMsg *m) {
    if (!m->sender || strcmp(m->sender, "org.freedesktop.DBus") || !m->member || strcmp(m->member, "NameOwnerChanged")
        || !m->sig || strcmp(m->sig, "sss")) return;
    DRd r;
    dr_init(&r, m);
    const char *name = dr_str(&r, NULL);
    dr_str(&r, NULL);
    const char *now = dr_str(&r, NULL);
    if (r.err || name[0] != ':' || now[0]) return;
    for (int i = nholds; i-- > 0;) if (!strcmp(holds[i].sender, name)) hold_drop(i);
    for (int i = nopens; i-- > 0;) if (!strcmp(opens[i].sender, name)) open_drop(i, 1);
    for (int i = npicks; i-- > 0;) if (!strcmp(picks[i].sender, name)) pick_drop(i);
    int ids[CSM_MAX], n = csm_gone(&csm, name, ids, CSM_MAX);
    for (int i = 0; i < n; i++) { CsSess *s = csm_by_id(&csm, ids[i]); if (s) cast_close(s, 0); }
}

/* Request.Close: the app no longer wants it (a dialog, an open, a hold or a screen cast's Start); only the app that
 * asked */
static void request_close(const DMsg *m) {
    int found = 0;
    for (int i = 0; m->sender && !found && i < npicks; i++)
        if (!strcmp(picks[i].handle, m->path) && !strcmp(picks[i].sender, m->sender)) { pick_drop(i); found = 1; }
    for (int i = 0; m->sender && !found && i < nopens; i++)
        if (!strcmp(opens[i].handle, m->path) && !strcmp(opens[i].sender, m->sender)) { open_drop(i, 1); found = 1; }
    for (int i = 0; m->sender && !found && i < nholds; i++)
        if (!strcmp(holds[i].handle, m->path) && !strcmp(holds[i].sender, m->sender)) { hold_drop(i); found = 1; }
    /* a screen cast's Start: no Response, and the session stays as it was before Start */
    int was_choosing = 0;
    for (int i = 0; m->sender && i < CSM_MAX; i++)
        if (csm.s[i].st == CS_CHOOSING && !strcmp(csm.s[i].req, m->path) && !strcmp(csm.s[i].sender, m->sender)) was_choosing = 1;
    CsSess *cs = NULL;
    if (!found && csm_request_close(&csm, m->sender, m->path, &cs) == CSM_OK) {
        if (was_choosing) chooser_gone(cs->id);
        cast_end(cs->id);
        found = 1;
    }
    if (found) { dbus_reply(&conn, m, NULL, NULL); return; }
    dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownObject", "no such request");
}

/* the version property of each interface served here */
static int version_of(const char *iface) {
    return !strcmp(iface, IFACE) || !strcmp(iface, FC_IFACE) ? 2 : !strcmp(iface, OU_IFACE) ? 3 : !strcmp(iface, INH_IFACE) ? 1
         : !strcmp(iface, SC_IFACE) ? SC_VERSION : -1;
}

static int screensaver_path(const char *p) { return p && (!strcmp(p, "/ScreenSaver") || !strcmp(p, "/org/freedesktop/ScreenSaver")); }

static void handle(const DMsg *m) {
    if ((m->type == DBUS_RETURN || m->type == DBUS_ERROR) && m->reply_serial) { on_reply(m); return; }
    if (m->type == DBUS_SIGNAL) { on_signal(m); return; }
    if (m->type != DBUS_CALL || !m->member) return;
    const char *iface = m->iface ? m->iface : "", *mem = m->member;
    if (!strcmp(iface, "org.freedesktop.DBus.Peer") || (!iface[0] && !strcmp(mem, "Ping"))) {
        if (!strcmp(mem, "Ping")) dbus_reply(&conn, m, NULL, NULL);
        else {
            DBuf b;
            dw_init(&b);
            char id[64] = "";
            FILE *f = fopen("/etc/machine-id", "r");
            if (f) { if (!fgets(id, sizeof id, f)) id[0] = 0; fclose(f); }
            id[strcspn(id, "\n")] = 0;
            dw_str(&b, id);
            dbus_reply(&conn, m, "s", &b);
            dw_free(&b);
        }
        return;
    }
    if (!strcmp(mem, "Introspect")) {
        DBuf b;
        dw_init(&b);
        dw_str(&b, m->path && !strcmp(m->path, OBJ_PATH) ? introspect_xml : screensaver_path(m->path) ? ss_xml
                   : "<node><node name=\"org\"/></node>");
        dbus_reply(&conn, m, "s", &b);
        dw_free(&b);
        return;
    }
    if (!strcmp(iface, REQ_IFACE) && !strcmp(mem, "Close") && m->path) { request_close(m); return; }
    if (screensaver_path(m->path)) { screensaver(m); return; }
    if (is_session_path(m->path)) { sc_session_call(m); return; }
    if (!m->path || strcmp(m->path, OBJ_PATH)) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownObject", "no existe"); return; }
    if (!strcmp(iface, "org.freedesktop.DBus.Properties")) {
        DRd r;
        dr_init(&r, m);
        const char *pi = dr_str(&r, NULL);
        int version = r.err ? -1 : version_of(pi);
        if (version < 0) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownInterface", "interfaz"); return; }
        DBuf b;
        dw_init(&b);
        int sc = !strcmp(pi, SC_IFACE);
        if (!strcmp(mem, "Get")) {
            const char *prop = dr_str(&r, NULL);
            int known = !r.err && (!strcmp(prop, "version") || (sc && (!strcmp(prop, "AvailableSourceTypes") || !strcmp(prop, "AvailableCursorModes"))));
            if (!known) { dw_free(&b); dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownProperty", "propiedad"); return; }
            if (sc) sc_prop(&b, prop);
            else { dw_variant(&b, "u"); dw_u32(&b, (uint32_t)version); }
            dbus_reply(&conn, m, "v", &b);
        } else if (!strcmp(mem, "GetAll")) {
            static const char *const sc_props[] = { "version", "AvailableSourceTypes", "AvailableCursorModes" };
            size_t a = dw_arr_begin(&b, 8);
            for (int k = 0; k < (sc ? 3 : 1); k++) {
                dw_struct(&b);
                dw_str(&b, sc_props[k]);
                if (sc) sc_prop(&b, sc_props[k]);
                else { dw_variant(&b, "u"); dw_u32(&b, (uint32_t)version); }
            }
            dw_arr_end(&b, a);
            dbus_reply(&conn, m, "a{sv}", &b);
        } else dbus_error(&conn, m, "org.freedesktop.DBus.Error.PropertyReadOnly", "read-only");
        dw_free(&b);
        return;
    }
    if (!strcmp(iface, FC_IFACE) && !strcmp(mem, "OpenFile")) { file_chooser(m, "open"); return; }
    if (!strcmp(iface, FC_IFACE) && !strcmp(mem, "SaveFile")) { file_chooser(m, "save"); return; }
    if (!strcmp(iface, OU_IFACE) && !strcmp(mem, "OpenURI")) { open_call(m, 0, 0); return; }
    if (!strcmp(iface, OU_IFACE) && !strcmp(mem, "OpenFile")) { open_call(m, 1, 0); return; }
    if (!strcmp(iface, OU_IFACE) && !strcmp(mem, "OpenDirectory")) { open_call(m, 1, 1); return; }
    if (!strcmp(iface, INH_IFACE) && !strcmp(mem, "Inhibit")) { inhibit(m); return; }
    if (!strcmp(iface, SC_IFACE) && !strcmp(mem, "CreateSession")) { sc_create(m); return; }
    if (!strcmp(iface, SC_IFACE) && !strcmp(mem, "SelectSources")) { sc_select(m); return; }
    if (!strcmp(iface, SC_IFACE) && !strcmp(mem, "Start")) { sc_start(m); return; }
    if (!strcmp(iface, SC_IFACE) && !strcmp(mem, "OpenPipeWireRemote")) { sc_remote(m); return; }
    if (!strcmp(iface, IFACE) || !iface[0]) {
        if (!strcmp(mem, "ReadAll") && m->sig && !strcmp(m->sig, "as")) { read_all(m); return; }
        if (!strcmp(mem, "Read") && m->sig && !strcmp(m->sig, "ss")) { read_one(m, 1); return; }
        if (!strcmp(mem, "ReadOne") && m->sig && !strcmp(m->sig, "ss")) { read_one(m, 0); return; }
    }
    dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
}

/* RequestName at startup: 1 if the name is ours (1 new owner, 4 it was already ours) */
static int own_name(const char *name) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, name);
    dw_u32(&b, 4);                          /* DBUS_NAME_FLAG_DO_NOT_QUEUE */
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus", .member = "RequestName",
               .dest = "org.freedesktop.DBus", .sig = "su" };
    uint32_t s = dbus_send(&conn, &h, &b);
    dw_free(&b);
    DMsg m;
    int ok = s && dbus_wait_reply(&conn, s, &m, 2000) == 1;
    uint32_t res = 0;
    if (ok) { DRd r; dr_init(&r, &m); res = dr_u32(&r); }
    if (s) dbus_msg_free(&m);
    return ok && (res == 1 || res == 4);
}

int portal_start(void) {
    /* apps launched by this session (GTK 3) read GSettings from the keyfile that portal_set writes */
    if (!getenv("GSETTINGS_BACKEND")) setenv("GSETTINGS_BACKEND", "keyfile", 1);
    int fd = dbus_connect_session();
    /* with fds: OpenFile and OpenDirectory hand the file over as one */
    if (fd < 0 || dbus_open_fds(&conn, fd, 2000)) { if (fd >= 0) dbus_close(&conn); return -1; }
    if (!own_name(BUS_NAME)) {
        fprintf(stderr, "portal: %s is already served by another program (xdg-desktop-portal); not serving Settings\n", BUS_NAME);
        dbus_close(&conn);
        return -1;
    }
    int ss = own_name(SS_NAME);
    /* the connections that leave the bus: their holds and requests go with them */
    DBuf b;
    dw_init(&b);
    dw_str(&b, "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'");
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus", .member = "AddMatch",
               .dest = "org.freedesktop.DBus", .sig = "s" };
    dbus_send(&conn, &h, &b);
    dw_free(&b);
    up = 1;
    csm_init(&csm);
    fprintf(stderr, "portal: Settings, FileChooser, OpenURI, Inhibit and ScreenCast on the session bus%s\n",
            ss ? ", and " SS_NAME : " (" SS_NAME " is another program's)");
    return 0;
}

int portal_fd(void) { return up ? conn.fd : -1; }
int portal_wants_write(void) { return up && dbus_pending_out(&conn); }

void portal_dispatch(void) {
    if (!up) return;
    DMsg m;
    int r;
    while ((r = dbus_next(&conn, &m)) == 1) { handle(&m); dbus_msg_free(&m); }
    if (r < 0 || dbus_flush(&conn) < 0) {
        fprintf(stderr, "portal: the session bus went away\n");
        dbus_close(&conn);
        up = 0;
        nholds = 0;                         /* nobody can let them go any more */
        while (nopens) open_drop(nopens - 1, 1);
        for (int i = 0; i < CSM_MAX; i++) if (csm.s[i].st != CS_FREE) cast_close(&csm.s[i], 0);
        nrcalls = 0;
    }
}

static void changed(int i) {
    if (!up) return;
    DBuf b;
    dw_init(&b);
    dw_str(&b, keys[i].ns);
    dw_str(&b, keys[i].key);
    put_variant(&b, i);
    dbus_signal(&conn, OBJ_PATH, IFACE, "SettingChanged", "ssv", &b);
    dw_free(&b);
}

/* ---- ~/.config/glib-2.0/settings/keyfile: [group] key=value (GVariant text) ----
 * Only our keys are replaced and whatever the apps saved is kept. */
static void keyfile_set(char **buf, const char *group, const char *key, const char *val) {
    char head[96], line[256];
    snprintf(head, sizeof head, "[%s]", group);
    snprintf(line, sizeof line, "%s=%s\n", key, val);
    char *b = *buf;
    size_t n = strlen(b);
    char *g = NULL;
    for (char *p = b; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncmp(p, head, strlen(head)) && (p[strlen(head)] == '\n' || !p[strlen(head)])) { g = p; break; }
    char *out = malloc(n + strlen(head) + strlen(line) + 4);
    if (!out) return;
    if (!g) {                                   /* new group at the end */
        sprintf(out, "%s%s%s\n%s", b, n && b[n - 1] != '\n' ? "\n" : "", head, line);
    } else {
        char *body = strchr(g, '\n');
        body = body ? body + 1 : g + strlen(g);
        char *end = body;                       /* up to the next group */
        while (*end && *end != '[') { char *nl = strchr(end, '\n'); end = nl ? nl + 1 : end + strlen(end); }
        size_t kl = strlen(key);
        char *k = NULL;
        for (char *p = body; p < end; ) {
            char *nl = strchr(p, '\n');
            if (!strncmp(p, key, kl) && p[kl] == '=') { k = p; break; }
            p = nl ? nl + 1 : end;
        }
        if (k) {
            char *nl = strchr(k, '\n');
            char *after = nl ? nl + 1 : k + strlen(k);
            sprintf(out, "%.*s%s%s", (int)(k - b), b, line, after);
        } else sprintf(out, "%.*s%s%s", (int)(body - b), b, line, body);
    }
    free(b);
    *buf = out;
}

static void keyfile_write(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char dir[512], path[600], tmp[620];
    snprintf(dir, sizeof dir, "%s/.config", home);
    mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/.config/glib-2.0", home);
    mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/.config/glib-2.0/settings", home);
    mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/keyfile", dir);
    snprintf(tmp, sizeof tmp, "%s/.keyfile-lordos", dir);
    char *buf = NULL;
    FILE *f = fopen(path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n >= 0 && n < (1 << 20) && (buf = malloc((size_t)n + 1))) { buf[fread(buf, 1, (size_t)n, f)] = 0; }
        fclose(f);
    }
    if (!buf && !(buf = calloc(1, 1))) return;
    char v[96];
    snprintf(v, sizeof v, "'%s'", gtk_theme);
    keyfile_set(&buf, "org/gnome/desktop/interface", "gtk-theme", v);
    keyfile_set(&buf, "org/gnome/desktop/interface", "color-scheme", dark ? "'prefer-dark'" : "'default'");
    snprintf(v, sizeof v, "'%s'", acc_name);
    keyfile_set(&buf, "org/gnome/desktop/interface", "accent-color", v);
    snprintf(v, sizeof v, "'%s'", buttons);
    keyfile_set(&buf, "org/gnome/desktop/wm/preferences", "button-layout", v);
    f = fopen(tmp, "w");
    if (!f) { free(buf); return; }
    int ok = fputs(buf, f) >= 0;
    ok = fclose(f) == 0 && ok;
    if (ok) rename(tmp, path); else unlink(tmp);
    free(buf);
}

static int theme_installed(const char *name) {
    char p[256];
    struct stat st;
    snprintf(p, sizeof p, "/usr/share/themes/%s/gtk-3.0/gtk.css", name);
    if (!stat(p, &st)) return 1;
    const char *home = getenv("HOME");
    if (!home) return 0;
    snprintf(p, sizeof p, "%s/.local/share/themes/%s/gtk-3.0/gtk.css", home, name);
    return !stat(p, &st);
}

void portal_set(int is_dark, const double rgb[3], const char *accent_name, const char *button_layout) {
    int old_dark = dark;
    double old[3] = { acc[0], acc[1], acc[2] };
    char old_name[16], old_buttons[64], old_theme[64];
    snprintf(old_name, sizeof old_name, "%s", acc_name);
    snprintf(old_buttons, sizeof old_buttons, "%s", buttons);
    snprintf(old_theme, sizeof old_theme, "%s", gtk_theme);
    dark = !!is_dark;
    for (int i = 0; i < 3; i++) acc[i] = rgb[i] < 0 ? 0 : rgb[i] > 1 ? 1 : rgb[i];
    if (accent_name) snprintf(acc_name, sizeof acc_name, "%s", accent_name);
    if (button_layout) snprintf(buttons, sizeof buttons, "%s", button_layout);
    /* GTK 3: our own theme if installed (assets/gtk), otherwise Adwaita */
    const char *t = dark ? "LordOS-dark" : "LordOS";
    snprintf(gtk_theme, sizeof gtk_theme, "%s", theme_installed(t) ? t : dark ? "Adwaita-dark" : "Adwaita");
    if (old_dark != dark) { changed(0); changed(3); }
    if (memcmp(old, acc, sizeof old)) changed(1);
    if (strcmp(old_name, acc_name)) changed(4);
    if (strcmp(old_theme, gtk_theme)) changed(5);
    if (strcmp(old_buttons, buttons)) changed(6);
    if (up) dbus_flush(&conn);
    static int written;
    if (!written || old_dark != dark || strcmp(old_name, acc_name) || strcmp(old_theme, gtk_theme) || strcmp(old_buttons, buttons)) {
        written = 1;
        keyfile_write();
    }
}
