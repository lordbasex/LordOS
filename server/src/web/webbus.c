/* webbus.c — what lord-web asks of the session bus (see web.h), like any Linux app:
 *   - notifications: org.freedesktop.Notifications, with the app's name and icon (app_icon and "image-path": the
 *     registry's icon file) and "desktop-entry" lordos-web.<app>.<profile>, so LordOS shows it under the app; a click
 *     tells the page first (WebKit's notification "clicked": the page's own onclick runs, a messaging app opens that
 *     chat), then brings the window forward with the activation token LordOS sent. "suppress-sound" when LordOS
 *     shouldn't play its own sound (webpage_sound_suppress: the page plays one, or the registry's "notifySound");
 *   - the page's unread count: org.lordos.WebApps1.SetBadge(app, profile, count) on lord-server, only when it changes
 *     (0: none), not waiting for an answer;
 *   - links outside the app's scope: the OpenURI portal, so they open in the person's browser; with --inspect, the
 *     developer tools' one-time address too, and their notification, whose action opens them again (webguard.h: the
 *     address is never shown in the notification, which other programs may read);
 *   - the page's file chooser: the FileChooser portal;
 *   - a permission the registry says to ask about: org.lordos.WebApps1.Ask(app, profile, kind) on lord-server, which
 *     shows the dialog and stores the answer. Anything but "allow", "deny" or "once", an error or no lord-server is
 *     "deny";
 *   - the shell's Edit menu: org.lordos.WebApp1.Edit(command) from lord-server (webmain.c serves it), run only when
 *     the bus says the caller's pid is the lord-server that started this lord-web; the desktop's light or dark mode,
 *     org.lordos.WebApp1.SetColorScheme("light" | "dark"), with the same check (webscheme.h); the profile's Browser
 *     identity, org.lordos.WebApp1.SetUserAgent("chrome" | "edge" | "firefox" | "lordos"), with the same check (webpage.h);
 *   - (the page's context menu, org.lordos.WebApps1.Menu, and its save dialog are webmenu.c's)
 * Every call is asynchronous: nothing here waits for another program. */
#include "web.h"
#include "webwin.h"
#include "webcall.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define NOTIFY_NAME  "org.freedesktop.Notifications"
#define NOTIFY_PATH  "/org/freedesktop/Notifications"
#define PORTAL_NAME  "org.freedesktop.portal.Desktop"
#define PORTAL_PATH  "/org/freedesktop/portal/desktop"
#define ASK_NAME     "org.lordos.WebApps1"
#define ASK_PATH     "/org/lordos/WebApps1"
#define ASK_TIMEOUT_MS (5 * 60 * 1000)          /* the person may take a while to answer the dialog */
#define MAX_NOTES 32

typedef struct { guint32 id; WebKitNotification *n; char token[256]; } Note;
static Note notes[MAX_NOTES];
static Web *web;
/* the notify: log line's tag hash (webpage_notify_line): keyed with this, picked at start and never written */
static WebTagIds tag_ids;              /* this run's tags, in the order they came (the log's "tag N") */
/* the last page notification WebKit withdrew (its tag's hash) and when: one with the same tag right after replaced it */
#define REPLACE_MS 1000.0
static unsigned long long closed_tag;   /* the tag WebKit withdrew last (webpage_tag_key: memory only) */
static double closed_ms = -1;

static Note *note_by_id(guint32 id) {
    for (int i = 0; i < MAX_NOTES; i++) if (notes[i].id == id && notes[i].n) return &notes[i];
    return NULL;
}

static void note_drop(Note *nt) {
    if (nt->n) g_object_unref(nt->n);
    memset(nt, 0, sizeof *nt);
}

static void on_notify_signal(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *sig, GVariant *p, gpointer d) {
    (void)c; (void)sender; (void)path; (void)iface; (void)d;
    guint32 id = 0;
    if (!strcmp(sig, "ActivationToken") && g_variant_is_of_type(p, G_VARIANT_TYPE("(us)"))) {
        const char *tok = NULL;
        g_variant_get(p, "(u&s)", &id, &tok);
        Note *nt = note_by_id(id);
        if (nt && tok) snprintf(nt->token, sizeof nt->token, "%s", tok);
    } else if (!strcmp(sig, "ActionInvoked") && g_variant_is_of_type(p, G_VARIANT_TYPE("(us)"))) {
        const char *action = NULL;
        g_variant_get(p, "(u&s)", &id, &action);
        if (id && id == web->inspect_note) { web_inspect_open(web); return; }
        Note *nt = note_by_id(id);
        if (!nt) return;
        char token[sizeof nt->token];
        snprintf(token, sizeof token, "%s", nt->token);
        WebKitNotification *n = g_object_ref(nt->n);
        webkit_notification_clicked(n);             /* the page may close it (note_drop): token and n are ours */
        g_object_unref(n);
        web_show(web, token);
    } else if (!strcmp(sig, "NotificationClosed") && g_variant_is_of_type(p, G_VARIANT_TYPE("(uu)"))) {
        guint32 reason = 0;
        g_variant_get(p, "(uu)", &id, &reason);
        if (id && id == web->inspect_note) web->inspect_note = 0;
        Note *nt = note_by_id(id);
        if (nt) note_drop(nt);
    }
}

void webbus_init(Web *w) {
    web = w;
    GError *e = NULL;
    w->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
    if (!w->bus) {
        fprintf(stderr, "lord-web: no session bus (%s): no notifications, portals or permission dialogs\n", e ? e->message : "?");
        g_clear_error(&e);
        return;
    }
    g_dbus_connection_signal_subscribe(w->bus, NOTIFY_NAME, NOTIFY_NAME, NULL, NOTIFY_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_notify_signal, w, NULL);
}

/* ---- notifications ---- */

typedef struct { WebKitNotification *n; int inspect; } NotifyCall;

static void on_page_closed(WebKitNotification *n, gpointer d) {
    (void)d;
    const char *tag = webkit_notification_get_tag(n);
    if (tag && tag[0]) { closed_tag = webpage_tag_key(tag); closed_ms = web_now_ms(); }
    for (int i = 0; i < MAX_NOTES; i++) if (notes[i].n == n) {
        if (web->bus) g_dbus_connection_call(web->bus, NOTIFY_NAME, NOTIFY_PATH, NOTIFY_NAME, "CloseNotification", g_variant_new("(u)", notes[i].id),
                                             NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        note_drop(&notes[i]);
    }
}

static void on_notified(GObject *src, GAsyncResult *res, gpointer d) {
    NotifyCall *call = d;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (r && call->n) {
        guint32 id = 0;
        g_variant_get(r, "(u)", &id);
        Note *slot = NULL;
        for (int i = 0; i < MAX_NOTES && !slot; i++) if (!notes[i].n) slot = &notes[i];
        if (!slot) { slot = &notes[0]; note_drop(slot); }
        slot->id = id;
        slot->n = call->n;
        call->n = NULL;
        g_signal_connect(slot->n, "closed", G_CALLBACK(on_page_closed), NULL);
    }
    if (r && call->inspect) g_variant_get(r, "(u)", &web->inspect_note);
    if (r) g_variant_unref(r);
    if (call->n) g_object_unref(call->n);
    g_free(call);
}

static void send_notify(Web *w, const char *summary, const char *body, WebKitNotification *n, guint32 replaces, int quiet, int inspect) {
    if (!w->bus) return;
    GVariantBuilder actions, hints;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    if (n) { g_variant_builder_add(&actions, "s", "default"); g_variant_builder_add(&actions, "s", weblang("notify.open")); }
    if (inspect) { g_variant_builder_add(&actions, "s", "default"); g_variant_builder_add(&actions, "s", weblang("inspect.open")); }
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    if (w->icon_path[0]) g_variant_builder_add(&hints, "{sv}", "image-path", g_variant_new_string(w->icon_path));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(w->window_id));
    if (quiet) g_variant_builder_add(&hints, "{sv}", "suppress-sound", g_variant_new_boolean(TRUE));
    NotifyCall *call = g_new0(NotifyCall, 1);
    call->n = n ? g_object_ref(n) : NULL;
    call->inspect = inspect;
    char name[2 * WEBREG_NAME_MAX + 8];
    if (w->app.nprofiles > 1 && w->profile) snprintf(name, sizeof name, "%s (%s)", w->app.name, w->profile->name);
    else snprintf(name, sizeof name, "%s", w->app.name);
    g_dbus_connection_call(w->bus, NOTIFY_NAME, NOTIFY_PATH, NOTIFY_NAME, "Notify",
                           g_variant_new("(susssasa{sv}i)", name, replaces, w->icon_path, summary ? summary : "", body ? body : "", &actions, &hints, -1),
                           G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_notified, call);
}

void webbus_notify(Web *w, WebKitNotification *n) {
    /* a notification with the same tag replaces the one before it */
    guint32 replaces = 0;
    const char *tag = webkit_notification_get_tag(n);
    for (int i = 0; tag && tag[0] && i < MAX_NOTES; i++) {
        const char *t = notes[i].n ? webkit_notification_get_tag(notes[i].n) : NULL;
        if (t && !strcmp(t, tag)) { replaces = notes[i].id; note_drop(&notes[i]); break; }
    }
    /* WebKit (2.48 to 2.54) doesn't tell a notification's "silent" option: 0 */
    double now = web_now_ms();
    /* one line in the app's log, without any of its text (webpage_notify_line); "replaced" also when WebKit withdrew
     * the one with the same tag just before (it does so itself, before showing the new one) */
    int replaced = replaces != 0;
    if (!replaced && tag && tag[0] && closed_ms >= 0 && now - closed_ms <= REPLACE_MS) {
        replaced = webpage_tag_key(tag) == closed_tag;
    }
    char line[128];
    if (webpage_notify_line(webpage_tag_id(&tag_ids, tag), -1, -1, replaced, line, sizeof line)) webcall_log_as("notify", line);
    int quiet = webpage_sound_suppress(w->app.notify_sound, w->state.own_sound, 0, w->audio_ms, now);
    if (!w->state.own_sound && webpage_sound_learns(w->app.notify_sound, now, w->audio_ms)) {
        w->state.own_sound = 1;                     /* the page started its sound just before: it has its own */
        web_state_save(w);
    }
    w->note_ms = quiet ? -1 : now;                  /* audio right after this one teaches the same (webengine.c) */
    send_notify(w, webkit_notification_get_title(n), webkit_notification_get_body(n), n, replaces, quiet, 0);
}

void webbus_notify_text(Web *w, const char *summary, const char *body) { send_notify(w, summary, body, NULL, 0, 0, 0); }

void webbus_notify_inspect(Web *w, const char *summary, const char *body) { send_notify(w, summary, body, NULL, w->inspect_note, 0, 1); }

void webbus_set_badge(Web *w, unsigned count) {
    if (!w->bus || !w->profile) return;
    g_dbus_connection_call(w->bus, ASK_NAME, ASK_PATH, ASK_NAME, "SetBadge", g_variant_new("(ssu)", w->app.id, w->profile->id, count),
                           NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, NULL, NULL);
}

/* ---- OpenURI ---- */

void webbus_open_uri(Web *w, const char *uri) {
    if (!w->bus || !uri) return;
    if (g_ascii_strncasecmp(uri, "https://", 8) && g_ascii_strncasecmp(uri, "http://", 7) && g_ascii_strncasecmp(uri, "mailto:", 7) && g_ascii_strncasecmp(uri, "tel:", 4)) return;
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_dbus_connection_call(w->bus, PORTAL_NAME, PORTAL_PATH, "org.freedesktop.portal.OpenURI", "OpenURI",
                           g_variant_new("(ssa{sv})", "", uri, &opts), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* ---- the FileChooser portal ---- */

typedef struct { WebKitFileChooserRequest *req; guint sub; int done; } Choose;

static void choose_end(Choose *ch, GVariant *results) {
    if (ch->done) return;
    ch->done = 1;
    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    GVariant *uris = results ? g_variant_lookup_value(results, "uris", G_VARIANT_TYPE("as")) : NULL;
    if (uris) {
        GVariantIter it;
        const char *u;
        g_variant_iter_init(&it, uris);
        while (g_variant_iter_next(&it, "&s", &u)) {
            char *f = g_filename_from_uri(u, NULL, NULL);
            if (f) g_ptr_array_add(files, f);
        }
        g_variant_unref(uris);
    }
    if (files->len) {
        g_ptr_array_add(files, NULL);
        webkit_file_chooser_request_select_files(ch->req, (const gchar *const *)files->pdata);
    } else webkit_file_chooser_request_cancel(ch->req);
    g_ptr_array_unref(files);
    if (ch->sub && web->bus) g_dbus_connection_signal_unsubscribe(web->bus, ch->sub);
    g_object_unref(ch->req);
    g_free(ch);
}

static void on_response(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *sig, GVariant *p, gpointer d) {
    (void)c; (void)sender; (void)path; (void)iface; (void)sig;
    guint32 code = 2;
    GVariant *results = NULL;
    if (g_variant_is_of_type(p, G_VARIANT_TYPE("(ua{sv})"))) g_variant_get(p, "(u@a{sv})", &code, &results);
    choose_end(d, code == 0 ? results : NULL);
    if (results) g_variant_unref(results);
}

static void on_open_file(GObject *src, GAsyncResult *res, gpointer d) {
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (!r) { fprintf(stderr, "lord-web: no file chooser (%s)\n", e ? e->message : "?"); g_clear_error(&e); choose_end(d, NULL); return; }
    g_variant_unref(r);
}

void webbus_choose_files(Web *w, WebKitFileChooserRequest *req) {
    if (!w->bus) { webkit_file_chooser_request_cancel(req); return; }
    Choose *ch = g_new0(Choose, 1);
    ch->req = g_object_ref(req);
    static unsigned seq;
    char token[48], path[256];
    snprintf(token, sizeof token, "lordweb%u_%u", (unsigned)getpid(), ++seq);
    /* the request's object path follows from our unique name and the token: listen before asking */
    char sender[128];
    snprintf(sender, sizeof sender, "%s", g_dbus_connection_get_unique_name(w->bus) + 1);
    for (char *p = sender; *p; p++) if (*p == '.') *p = '_';
    snprintf(path, sizeof path, PORTAL_PATH "/request/%s/%s", sender, token);
    ch->sub = g_dbus_connection_signal_subscribe(w->bus, PORTAL_NAME, "org.freedesktop.portal.Request", "Response", path, NULL,
                                                 G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE, on_response, ch, NULL);
    int many = webkit_file_chooser_request_get_select_multiple(req);
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));
    g_variant_builder_add(&opts, "{sv}", "multiple", g_variant_new_boolean(many));
    g_variant_builder_add(&opts, "{sv}", "modal", g_variant_new_boolean(TRUE));
    const gchar *const *types = webkit_file_chooser_request_get_mime_types(req);
    if (types && types[0]) {
        GVariantBuilder filters, pats;
        g_variant_builder_init(&filters, G_VARIANT_TYPE("a(sa(us))"));
        g_variant_builder_init(&pats, G_VARIANT_TYPE("a(us)"));
        for (int i = 0; types[i] && i < 32; i++) g_variant_builder_add(&pats, "(us)", 1u, types[i]);
        g_variant_builder_add(&filters, "(s@a(us))", "", g_variant_builder_end(&pats));
        g_variant_builder_add(&opts, "{sv}", "filters", g_variant_builder_end(&filters));
    }
    g_dbus_connection_call(w->bus, PORTAL_NAME, PORTAL_PATH, "org.freedesktop.portal.FileChooser", "OpenFile",
                           g_variant_new("(ssa{sv})", "", weblang(many ? "chooser.openMany" : "chooser.open"), &opts),
                           G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, G_MAXINT, NULL, on_open_file, ch);
}

/* ---- permissions ---- */

typedef struct { Web *w; int kind; WebAskDone cb; void *data; } Ask;

static void on_ask(GObject *src, GAsyncResult *res, gpointer d) {
    Ask *a = d;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    const char *answer = "deny";
    if (r) {
        const char *s = NULL;
        g_variant_get(r, "(&s)", &s);
        if (s && (!strcmp(s, "allow") || !strcmp(s, "once"))) answer = s;
    } else {
        fprintf(stderr, "lord-web: %s not asked (%s): denied\n", webreg_perm_name(a->kind), e ? e->message : "?");
        g_clear_error(&e);
    }
    a->cb(a->w, a->kind, answer, a->data);
    if (r) g_variant_unref(r);
    g_free(a);
}

void webbus_ask(Web *w, int kind, WebAskDone cb, void *data) {
    web_registry_refresh(w);                        /* an answer stored since the app started counts */
    int p = kind >= 0 && kind < WEBREG_PERM_N ? w->app.perms[kind] : WEBREG_DENY;
    if (p != WEBREG_ASK || !w->bus) { cb(w, kind, p == WEBREG_ALLOW ? "allow" : "deny", data); return; }
    Ask *a = g_new0(Ask, 1);
    a->w = w;
    a->kind = kind;
    a->cb = cb;
    a->data = data;
    g_dbus_connection_call(w->bus, ASK_NAME, ASK_PATH, ASK_NAME, "Ask", g_variant_new("(sss)", w->app.id, w->profile->id, webreg_perm_name(kind)),
                           G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, ASK_TIMEOUT_MS, NULL, on_ask, a);
}

/* ---- editing: the shell's Edit menu and the page's context menu ---- */

/* cmd >= 0: Edit; CALL_SCHEME: SetColorScheme(arg); CALL_UA: SetUserAgent(arg) */
enum { CALL_SCHEME = -1, CALL_UA = -2, CALL_WRITE = -3 };
typedef struct { Web *w; int cmd, arg; char *text; } EditCall;

static void on_edit_pid(GObject *src, GAsyncResult *res, gpointer d) {
    EditCall *ec = d;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    guint32 pid = 0;
    if (r) { g_variant_get(r, "(u)", &pid); g_variant_unref(r); }
    if (pid && ec->w->server_pid > 1 && (pid_t)pid == ec->w->server_pid) {
        if (ec->cmd >= 0) web_edit(ec->w, ec->cmd);
        else if (ec->cmd == CALL_SCHEME) web_set_scheme(ec->w, ec->arg);
        else if (ec->cmd == CALL_UA) web_set_user_agent(ec->w, ec->arg);
        else if (ec->cmd == CALL_WRITE && ec->text) web_menu_ai_replace(ec->w, ec->text);
    } else fprintf(stderr, "lord-web: %s call from a process that is not lord-server: refused\n",
                   ec->cmd >= 0 ? "an Edit" : ec->cmd == CALL_SCHEME ? "a SetColorScheme" : ec->cmd == CALL_WRITE ? "a WriteReplace" : "a SetUserAgent");
    if (ec->text) { memset(ec->text, 0, strlen(ec->text)); g_free(ec->text); }
    g_free(ec);
}

/* the bus answers from the caller's socket credentials, never from the caller */
static void check_caller_text(Web *w, const char *sender, int cmd, int arg, const char *text) {
    if (!w->bus || !sender) return;
    EditCall *ec = g_new0(EditCall, 1);
    ec->w = w;
    ec->cmd = cmd;
    ec->arg = arg;
    ec->text = text ? g_strdup(text) : NULL;
    g_dbus_connection_call(w->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetConnectionUnixProcessID",
                           g_variant_new("(s)", sender), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_edit_pid, ec);
}

static void check_caller(Web *w, const char *sender, int cmd, int arg) { check_caller_text(w, sender, cmd, arg, NULL); }
void webbus_write_replace(Web *w, const char *sender, const char *text) { if (text && strlen(text) <= WEBAI_MAX) check_caller_text(w, sender, CALL_WRITE, 0, text); }
void webbus_edit(Web *w, const char *sender, int cmd) { if (cmd >= 0) check_caller(w, sender, cmd, 0); }
void webbus_scheme(Web *w, const char *sender, int scheme) { if (webscheme_name(scheme)) check_caller(w, sender, CALL_SCHEME, scheme); }
void webbus_user_agent(Web *w, const char *sender, int kind) { if (webreg_ua_name(kind)) check_caller(w, sender, CALL_UA, kind); }
