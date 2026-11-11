/* webtray.c — the tray icon of a web app that keeps running in the background (docs/WEBAPPS.md, "background"): an
 * org.kde.StatusNotifierItem with the app's icon (its PNG as IconPixmap: LordOS's tray never reads icon paths) and a
 * com.canonical.dbusmenu menu with Open and Quit, registered with lord-server's StatusNotifierWatcher (ui/sni.c).
 * A click on the icon opens the window. */
#include "web.h"
#include "webwin.h"

#include "stb_image.h"            /* lord-server's own stb object (ui/stb.c): PNG and JPEG only */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define ITEM_PATH "/StatusNotifierItem"
#define MENU_PATH "/MenuBar"
enum { MENU_OPEN = 1, MENU_QUIT = 2 };

struct WebTray {
    Web *w;
    guint item_reg, menu_reg, name_id;
    GVariant *pixmaps;                  /* a(iiay), floating ref sunk */
    char name[64];
};

static const char ITEM_XML[] =
    "<node><interface name='org.kde.StatusNotifierItem'>"
    "<property name='Category' type='s' access='read'/><property name='Id' type='s' access='read'/>"
    "<property name='Title' type='s' access='read'/><property name='Status' type='s' access='read'/>"
    "<property name='IconName' type='s' access='read'/><property name='IconPixmap' type='a(iiay)' access='read'/>"
    "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "<property name='ItemIsMenu' type='b' access='read'/><property name='Menu' type='o' access='read'/>"
    "<method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
    "<signal name='NewIcon'/><signal name='NewTitle'/><signal name='NewStatus'><arg type='s'/></signal>"
    "</interface></node>";

static const char MENU_XML[] =
    "<node><interface name='com.canonical.dbusmenu'>"
    "<property name='Version' type='u' access='read'/><property name='TextDirection' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/><property name='IconThemePath' type='as' access='read'/>"
    "<method name='GetLayout'><arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='as' direction='in'/>"
    "<arg type='u' direction='out'/><arg type='(ia{sv}av)' direction='out'/></method>"
    "<method name='GetGroupProperties'><arg type='ai' direction='in'/><arg type='as' direction='in'/><arg type='a(ia{sv})' direction='out'/></method>"
    "<method name='GetProperty'><arg type='i' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='out'/></method>"
    "<method name='Event'><arg type='i' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='EventGroup'><arg type='a(isvu)' direction='in'/><arg type='ai' direction='out'/></method>"
    "<method name='AboutToShow'><arg type='i' direction='in'/><arg type='b' direction='out'/></method>"
    "<method name='AboutToShowGroup'><arg type='ai' direction='in'/><arg type='ai' direction='out'/><arg type='ai' direction='out'/></method>"
    "<signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
    "<signal name='ItemsPropertiesUpdated'><arg type='a(ia{sv})'/><arg type='a(ias)'/></signal>"
    "</interface></node>";

/* one side x side network-order ARGB32 pixmap, the PNG's pixels averaged into it */
static GVariant *pixmap(const unsigned char *rgba, int w, int h, int side) {
    guint8 *out = g_malloc0((gsize)side * side * 4);
    int box = w > h ? w : h;
    int ox = (box - w) / 2, oy = (box - h) / 2;
    for (int y = 0; y < side; y++) for (int x = 0; x < side; x++) {
        int x0 = x * box / side - ox, x1 = (x + 1) * box / side - ox, y0 = y * box / side - oy, y1 = (y + 1) * box / side - oy;
        if (x1 <= x0) x1 = x0 + 1;
        if (y1 <= y0) y1 = y0 + 1;
        unsigned long sum[4] = { 0 }, n = 0;
        for (int sy = y0; sy < y1; sy++) for (int sx = x0; sx < x1; sx++) {
            if (sx < 0 || sy < 0 || sx >= w || sy >= h) { n++; continue; }
            const unsigned char *p = rgba + ((size_t)sy * w + sx) * 4;
            unsigned a = p[3];
            sum[0] += p[0] * a; sum[1] += p[1] * a; sum[2] += p[2] * a; sum[3] += a;
            n++;
        }
        guint8 *o = out + ((size_t)y * side + x) * 4;
        unsigned long a = n ? sum[3] / n : 0;
        o[0] = (guint8)a;
        o[1] = (guint8)(sum[3] ? sum[0] / sum[3] : 0);
        o[2] = (guint8)(sum[3] ? sum[1] / sum[3] : 0);
        o[3] = (guint8)(sum[3] ? sum[2] / sum[3] : 0);
    }
    GVariant *bytes = g_variant_new_from_data(G_VARIANT_TYPE("ay"), out, (gsize)side * side * 4, TRUE, g_free, out);
    return g_variant_new("(ii@ay)", side, side, bytes);
}

static GVariant *load_pixmaps(Web *w) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a(iiay)"));
    gchar *data = NULL;
    gsize len = 0;
    size_t n = strlen(w->icon_path);
    if (n > 4 && !strcmp(w->icon_path + n - 4, ".png") && g_file_get_contents(w->icon_path, &data, &len, NULL) && len <= 1024 * 1024) {
        int iw, ih, ch;
        unsigned char *px = stbi_load_from_memory((const unsigned char *)data, (int)len, &iw, &ih, &ch, 4);
        if (px) {
            g_variant_builder_add_value(&b, pixmap(px, iw, ih, 32));
            g_variant_builder_add_value(&b, pixmap(px, iw, ih, 64));
            stbi_image_free(px);
        }
    }
    g_free(data);
    return g_variant_ref_sink(g_variant_builder_end(&b));
}

static void item_call(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *method, GVariant *p, GDBusMethodInvocation *inv, gpointer d) {
    (void)c; (void)sender; (void)path; (void)iface; (void)p;
    WebTray *t = d;
    if (!strcmp(method, "Activate") || !strcmp(method, "SecondaryActivate")) web_show(t->w, NULL);
    g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *item_prop(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *prop, GError **e, gpointer d) {
    (void)c; (void)sender; (void)path; (void)iface; (void)e;
    WebTray *t = d;
    Web *w = t->w;
    char title[2 * WEBREG_NAME_MAX + 8];
    if (w->app.nprofiles > 1) snprintf(title, sizeof title, "%s (%s)", w->app.name, w->profile->name);
    else snprintf(title, sizeof title, "%s", w->app.name);
    if (!strcmp(prop, "Category")) return g_variant_new_string("Communications");
    if (!strcmp(prop, "Id")) return g_variant_new_string(w->window_id);
    if (!strcmp(prop, "Title")) return g_variant_new_string(title);
    if (!strcmp(prop, "Status")) return g_variant_new_string("Active");
    if (!strcmp(prop, "IconName")) return g_variant_new_string(g_variant_n_children(t->pixmaps) ? "" : "applications-internet");
    if (!strcmp(prop, "IconPixmap")) return g_variant_ref(t->pixmaps);
    if (!strcmp(prop, "ToolTip")) return g_variant_new("(s@a(iiay)ss)", "", g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0), title, "");
    if (!strcmp(prop, "ItemIsMenu")) return g_variant_new_boolean(FALSE);
    if (!strcmp(prop, "Menu")) return g_variant_new_object_path(MENU_PATH);
    return NULL;
}

static GVariant *menu_entry(int id, const char *label) {
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "label", g_variant_new_string(label));
    g_variant_builder_add(&props, "{sv}", "enabled", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(TRUE));
    return g_variant_new("(i@a{sv}@av)", id, g_variant_builder_end(&props), g_variant_new_array(G_VARIANT_TYPE_VARIANT, NULL, 0));
}

static GVariant *menu_layout(void) {
    GVariantBuilder props, kids;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "children-display", g_variant_new_string("submenu"));
    g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
    g_variant_builder_add(&kids, "v", menu_entry(MENU_OPEN, weblang("tray.open")));
    g_variant_builder_add(&kids, "v", menu_entry(MENU_QUIT, weblang("tray.quit")));
    return g_variant_new("(i@a{sv}@av)", 0, g_variant_builder_end(&props), g_variant_builder_end(&kids));
}

static void menu_event(WebTray *t, int id, const char *ev) {
    if (strcmp(ev, "clicked")) return;
    if (id == MENU_OPEN) web_show(t->w, NULL);
    else if (id == MENU_QUIT) web_quit(t->w, 0);
}

static void menu_call(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *method, GVariant *p, GDBusMethodInvocation *inv, gpointer d) {
    (void)c; (void)sender; (void)path; (void)iface;
    WebTray *t = d;
    if (!strcmp(method, "GetLayout")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u@(ia{sv}av))", 1u, menu_layout()));
    } else if (!strcmp(method, "GetGroupProperties")) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(ia{sv})"));
        g_dbus_method_invocation_return_value(inv, g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&b)));
    } else if (!strcmp(method, "GetProperty")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", g_variant_new_string("")));
    } else if (!strcmp(method, "Event")) {
        gint32 id;
        const char *ev;
        GVariant *v;
        guint32 ts;
        g_variant_get(p, "(i&svu)", &id, &ev, &v, &ts);
        g_variant_unref(v);
        g_dbus_method_invocation_return_value(inv, NULL);
        menu_event(t, id, ev);
        return;
    } else if (!strcmp(method, "EventGroup")) {
        GVariantIter *it;
        gint32 id;
        const char *ev;
        GVariant *v;
        guint32 ts;
        g_variant_get(p, "(a(isvu))", &it);
        int quit = 0, open = 0;
        while (g_variant_iter_next(it, "(i&svu)", &id, &ev, &v, &ts)) {
            if (!strcmp(ev, "clicked")) { if (id == MENU_QUIT) quit = 1; if (id == MENU_OPEN) open = 1; }
            g_variant_unref(v);
        }
        g_variant_iter_free(it);
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(inv, g_variant_new("(@ai)", g_variant_builder_end(&b)));
        if (open) menu_event(t, MENU_OPEN, "clicked");
        if (quit) menu_event(t, MENU_QUIT, "clicked");
        return;
    } else if (!strcmp(method, "AboutToShow")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", FALSE));
    } else if (!strcmp(method, "AboutToShowGroup")) {
        GVariantBuilder a, b;
        g_variant_builder_init(&a, G_VARIANT_TYPE("ai"));
        g_variant_builder_init(&b, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(inv, g_variant_new("(@ai@ai)", g_variant_builder_end(&a), g_variant_builder_end(&b)));
    } else g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
}

static GVariant *menu_prop(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *prop, GError **e, gpointer d) {
    (void)c; (void)sender; (void)path; (void)iface; (void)e; (void)d;
    if (!strcmp(prop, "Version")) return g_variant_new_uint32(3);
    if (!strcmp(prop, "TextDirection")) return g_variant_new_string("ltr");
    if (!strcmp(prop, "Status")) return g_variant_new_string("normal");
    if (!strcmp(prop, "IconThemePath")) return g_variant_new_strv(NULL, 0);
    return NULL;
}

static const GDBusInterfaceVTable item_vt = { item_call, item_prop, NULL, { 0 } };
static const GDBusInterfaceVTable menu_vt = { menu_call, menu_prop, NULL, { 0 } };

static void on_registered(GObject *src, GAsyncResult *res, gpointer d) {
    (void)d;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (!r) { fprintf(stderr, "lord-web: no tray (%s)\n", e ? e->message : "?"); g_clear_error(&e); return; }
    g_variant_unref(r);
}

static void on_name(GDBusConnection *c, const char *name, gpointer d) {
    (void)d;
    g_dbus_connection_call(c, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                           g_variant_new("(s)", name), NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, on_registered, NULL);
}

WebTray *webtray_new(Web *w) {
    if (!w->bus) return NULL;
    WebTray *t = g_new0(WebTray, 1);
    t->w = w;
    t->pixmaps = load_pixmaps(w);
    GDBusNodeInfo *item = g_dbus_node_info_new_for_xml(ITEM_XML, NULL), *menu = g_dbus_node_info_new_for_xml(MENU_XML, NULL);
    if (item && menu) {
        t->item_reg = g_dbus_connection_register_object(w->bus, ITEM_PATH, item->interfaces[0], &item_vt, t, NULL, NULL);
        t->menu_reg = g_dbus_connection_register_object(w->bus, MENU_PATH, menu->interfaces[0], &menu_vt, t, NULL, NULL);
    }
    if (item) g_dbus_node_info_unref(item);
    if (menu) g_dbus_node_info_unref(menu);
    snprintf(t->name, sizeof t->name, "org.kde.StatusNotifierItem-%d-1", (int)getpid());
    t->name_id = g_bus_own_name_on_connection(w->bus, t->name, G_BUS_NAME_OWNER_FLAGS_NONE, on_name, NULL, t, NULL);
    return t;
}

void webtray_free(WebTray *t) {
    if (!t) return;
    if (t->name_id) g_bus_unown_name(t->name_id);
    if (t->item_reg) g_dbus_connection_unregister_object(t->w->bus, t->item_reg);
    if (t->menu_reg) g_dbus_connection_unregister_object(t->w->bus, t->menu_reg);
    if (t->pixmaps) g_variant_unref(t->pixmaps);
    g_free(t);
}
