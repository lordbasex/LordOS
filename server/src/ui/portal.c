/* portal.c — our own org.freedesktop.portal.Settings and FileChooser (without xdg-desktop-portal): GTK, GNOME
 * (libadwaita) and Qt apps read from here the dark/light mode, the accent color, the order of the
 * title bar buttons and the GTK 3 theme, and follow LordOS's changes live (SettingChanged).
 * Uses Passwords' own D-Bus client (vault/dbus.c). See docs/WAYLAND.md, "GTK and GNOME apps".
 *
 * GTK 3 outside Flatpak doesn't read the portal: it reads GSettings. lord-server gives the apps
 * GSETTINGS_BACKEND=keyfile and writes those same keys to ~/.config/glib-2.0/settings/keyfile, which
 * GLib watches: GTK 3 also changes live (without dconf or running gsettings). */
#include "portal.h"
#include "../hw/hw.h"
#include "../hw/jbuf.h"
#include "../vault/dbus.h"

#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    dr_str(&r, NULL);                       /* the parent window: the dialog is the system's, over everything */
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
    char who[256] = "", tok[65] = "";
    size_t w = 0;
    for (const char *s = m->sender + (m->sender[0] == ':'); *s && w + 1 < sizeof who; s++) who[w++] = *s == '.' ? '_' : *s;
    size_t t = 0;
    for (const char *s = token; *s && t + 1 < sizeof tok; s++)
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_') tok[t++] = *s;
    if (!t) snprintf(tok, sizeof tok, "lordos%d", p->id);
    snprintf(p->handle, sizeof p->handle, "%s/request/%s/%s", OBJ_PATH, who, tok);
    HBuf j = { 0 };
    hb_printf(&j, "{\"id\":%d,\"mode\":\"%s\",\"multiple\":%s,\"title\":", p->id,
              directory ? "folder" : mode, multiple ? "true" : "false");
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

static void request_close(const DMsg *m) {
    for (int i = 0; i < npicks; i++)
        if (!strcmp(picks[i].handle, m->path) && m->sender && !strcmp(picks[i].sender, m->sender)) {
            pick_drop(i);
            dbus_reply(&conn, m, NULL, NULL);
            return;
        }
    dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownObject", "no such request");
}

static void handle(const DMsg *m) {
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
        dw_str(&b, m->path && !strcmp(m->path, OBJ_PATH) ? introspect_xml
                   : "<node><node name=\"org\"/></node>");
        dbus_reply(&conn, m, "s", &b);
        dw_free(&b);
        return;
    }
    if (!strcmp(iface, REQ_IFACE) && !strcmp(mem, "Close") && m->path) { request_close(m); return; }
    if (!m->path || strcmp(m->path, OBJ_PATH)) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownObject", "no existe"); return; }
    if (!strcmp(iface, "org.freedesktop.DBus.Properties")) {
        DRd r;
        dr_init(&r, m);
        const char *pi = dr_str(&r, NULL);
        if (r.err || (strcmp(pi, IFACE) && strcmp(pi, FC_IFACE))) { dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownInterface", "interfaz"); return; }
        DBuf b;
        dw_init(&b);
        if (!strcmp(mem, "Get")) {
            const char *prop = dr_str(&r, NULL);
            if (r.err || strcmp(prop, "version")) { dw_free(&b); dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownProperty", "propiedad"); return; }
            dw_variant(&b, "u");
            dw_u32(&b, 2);
            dbus_reply(&conn, m, "v", &b);
        } else if (!strcmp(mem, "GetAll")) {
            size_t a = dw_arr_begin(&b, 8);
            dw_struct(&b);
            dw_str(&b, "version");
            dw_variant(&b, "u");
            dw_u32(&b, 2);
            dw_arr_end(&b, a);
            dbus_reply(&conn, m, "a{sv}", &b);
        } else dbus_error(&conn, m, "org.freedesktop.DBus.Error.PropertyReadOnly", "read-only");
        dw_free(&b);
        return;
    }
    if (!strcmp(iface, FC_IFACE) && !strcmp(mem, "OpenFile")) { file_chooser(m, "open"); return; }
    if (!strcmp(iface, FC_IFACE) && !strcmp(mem, "SaveFile")) { file_chooser(m, "save"); return; }
    if (!strcmp(iface, IFACE) || !iface[0]) {
        if (!strcmp(mem, "ReadAll") && m->sig && !strcmp(m->sig, "as")) { read_all(m); return; }
        if (!strcmp(mem, "Read") && m->sig && !strcmp(m->sig, "ss")) { read_one(m, 1); return; }
        if (!strcmp(mem, "ReadOne") && m->sig && !strcmp(m->sig, "ss")) { read_one(m, 0); return; }
    }
    dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
}

int portal_start(void) {
    /* apps launched by this session (GTK 3) read GSettings from the keyfile that portal_set writes */
    if (!getenv("GSETTINGS_BACKEND")) setenv("GSETTINGS_BACKEND", "keyfile", 1);
    int fd = dbus_connect_session();
    if (fd < 0 || dbus_open(&conn, fd, 2000)) { if (fd >= 0) dbus_close(&conn); return -1; }
    DBuf b;
    dw_init(&b);
    dw_str(&b, BUS_NAME);
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
    if (!ok || (res != 1 && res != 4)) {     /* 1 new owner, 4 it was already ours */
        fprintf(stderr, "portal: %s is already served by another program (xdg-desktop-portal); not serving Settings\n", BUS_NAME);
        dbus_close(&conn);
        return -1;
    }
    up = 1;
    fprintf(stderr, "portal: Settings and FileChooser on the session bus\n");
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
