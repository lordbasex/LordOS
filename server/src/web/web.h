/* web.h — lord-web, the LordOS Web Runtime (docs/WEBAPPS.md): one process per installed web app and profile, the only
 * binary that links WPE WebKit. What its parts share:
 *
 *   webmain.c     argv, the registry check, isolation, then the app or a probe
 *   webengine.c   WebKit: the context and network session of the profile, the view, navigation, permissions,
 *                 notifications, downloads, the file chooser
 *   webwin.c      the Wayland window: xdg-shell, the frame's decoration, scale, the frames WebKit draws (dmabuf or
 *                 shared memory), keyboard, pointer, touch, the clipboard, the input method (webshm.c: the pieces that
 *                 read wpebackend-fdo's own buffers)
 *   webbus.c      the session bus: notifications, the OpenURI and FileChooser portals, org.lordos.WebApps1.Ask
 *   webtray.c     the StatusNotifierItem of an app that keeps running in the background
 *   webprobe.c    --probe and --probe-icon, headless, in a throwaway profile
 *   weblang.c     the few words lord-web shows itself (assets/locales/<lang>/webruntime.json)
 *   webargs.c, webreg.c, weburl.c, webmanifest.c, webdl.c   the pure checks (tests/webapp_test.c)
 *
 * No LordOS API reaches the page: no lord.*, no broker, no script with privileges. lord-web is a plain Wayland and
 * D-Bus client of the session. */
#ifndef LORD_WEB_WEB_H
#define LORD_WEB_WEB_H

#include "webargs.h"
#include "webreg.h"

#include <limits.h>

#include <wpe/webkit.h>
#include <wpe/fdo.h>

typedef struct WebWin WebWin;
typedef struct WebTray WebTray;

typedef struct {
    WebArgs args;
    WebApp app;
    const WebProfile *profile;
    char home[PATH_MAX];
    char config_dir[PATH_MAX];          /* ~/.config/lordos/web */
    char profile_dir[PATH_MAX];         /* ~/.local/share/lordos/web/<app>/<profile> */
    char icon_path[PATH_MAX];           /* the app's icon file, or "" */
    char window_id[3 * WEBREG_ID_MAX];  /* lordos-web.<app>.<profile>: the toplevel's app id */
    GMainLoop *loop;
    WebKitWebContext *context;
    WebKitNetworkSession *session;
    WebKitWebView *view;
    struct wpe_view_backend_exportable_fdo *exportable;
    WebWin *win;
    WebTray *tray;
    GDBusConnection *bus;
    int outside;                        /* the page is outside the app's scope (a sign-in on another site) */
    int quitting;
    int exit_code;
} Web;

/* webengine.c */
WebKitNetworkSession *web_session_new(Web *w);           /* the profile's own folders, cookies in cookies.sqlite */
WebKitWebContext *web_context_new(void);                 /* memory pressure, no prewarmed process, cache model */
WebKitSettings *web_settings_new(void);                  /* developer extras off, no automatic windows */
int web_engine_start(Web *w);                            /* the view on w->exportable, signals, first load */
void web_quit(Web *w, int code);
/* the window was closed: hidden behind the tray when the app runs in the background, otherwise quit */
void web_close_requested(Web *w);
void web_show(Web *w, const char *activation_token);

/* webbus.c */
void webbus_init(Web *w);
typedef void (*WebAskDone)(Web *w, int kind, const char *answer, void *data);
/* the registry's word for kind, asking lord-server (org.lordos.WebApps1.Ask) when it is "ask"; cb gets "allow",
 * "deny" or "once" (an error or no answer is "deny") */
void webbus_ask(Web *w, int kind, WebAskDone cb, void *data);
void webbus_open_uri(Web *w, const char *uri);
void webbus_notify(Web *w, WebKitNotification *n);
void webbus_notify_text(Web *w, const char *summary, const char *body);
void webbus_choose_files(Web *w, WebKitFileChooserRequest *req);

/* webtray.c */
WebTray *webtray_new(Web *w);
void webtray_free(WebTray *t);

/* weblang.c */
void weblang_init(void);
const char *weblang(const char *key);                    /* the text of webruntime.<key>, English if missing */

/* webprobe.c */
int webprobe_run(Web *w);
int webprobe_icon_run(Web *w);

/* webmain.c */
const char *web_self_dir(void);                          /* the folder of /proc/self/exe */
/* reads the app's registry file again (lord-server stores a permission's answer there); kept as it was when the new
 * file isn't valid or is for another app or profile */
void web_registry_refresh(Web *w);

#endif
