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
 *   webtray.c     the StatusNotifierItem of an app that keeps running in the background (when its "tray" is on)
 *   webdark.c     the desktop's light or dark mode: the page's prefers-color-scheme through matchMedia (webscheme.js),
 *                 the view's background while a page loads
 *   webselect.c   selecting text by dragging (a script in an isolated world: WPE 2.48 is built without drag support)
 *   webxfer.c     files dropped on the window and images pasted into the page (WebKit 2.48's WPE takes neither: a
 *                 script hands the page the bytes lord-web read itself), and the page's copies of an image
 *   webprobe.c    --probe and --probe-icon, headless, in a throwaway profile
 *   weblang.c     the few words lord-web shows itself (assets/locales/<lang>/webruntime.json)
 *   webargs.c, webreg.c, weburl.c, webmanifest.c, webdl.c, webaxis.c, webpage.c, webdrop.c   the pure parts
 *                 (tests/webapp_test.c)
 *
 * No LordOS API reaches the page: no lord.*, no broker, no script with privileges. The scripts of lord-web's the page
 * can see are the Async Clipboard API's write and writeText that WPE 2.48 lacks (webxfer.c): what it hands over
 * becomes the session's clipboard only right after the person's own key or click in the window; and matchMedia's
 * answers about prefers-color-scheme (webdark.c), which tell lord-web nothing. lord-web is a plain
 * Wayland and D-Bus client of the session. */
#ifndef LORD_WEB_WEB_H
#define LORD_WEB_WEB_H

#include "webargs.h"
#include "webreg.h"
#include "weburl.h"
#include "webdrop.h"
#include "webpage.h"
#include "webedit.h"
#include "webscheme.h"

#include <limits.h>
#include <sys/types.h>

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
    int badge;                          /* the unread count last reported to lord-server (SetBadge) */
    WebPageState state;                 /* <profile>/state.json: what lord-web learned about the page */
    double audio_ms;                    /* when the page last started playing audio (monotonic ms), or -1 */
    double note_ms;                     /* when its last notification went out with LordOS's sound, or -1 */
    pid_t server_pid;                   /* the lord-server that started this lord-web (its parent): Edit comes only from it */
    int menu_open;                      /* a context menu is waiting for the person (Menu on lord-server) */
    int scheme;                         /* the desktop's WEBSCHEME_LIGHT or WEBSCHEME_DARK (LORDOS_WEB_SCHEME, SetColorScheme) */
    char scheme_key[WEBSCHEME_KEY_LEN + 1]; /* webscheme.js's hook on window, random per process ("" none: no script) */
    WebKitUserScript *scheme_script;    /* the one added for the scheme now: replaced when it changes */
    int loading;                        /* between a load's start and its end (the dark background) */
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
double web_now_ms(void);                                 /* monotonic */
/* an editing command on the page (webedit.h's WEBEDIT_*): the Edit menu, the context menu */
void web_edit(Web *w, int cmd);
/* the person chose item (WEBMENU_*) in the page's context menu; link and image are what was under the pointer */
void web_menu_chosen(Web *w, int item, const char *link, const char *image);
/* a right click in the page (WEBMENU_AT_* of what is under the pointer, a link's and an image's address or NULL):
 * LordOS's menu with the items for it (webbus_menu); addresses of the wrong kind are dropped */
void web_context_menu(Web *w, unsigned at, const char *link, const char *image);
void web_state_save(Web *w);

/* webxfer.c */
void webxfer_init(Web *w);                               /* the clipboard script and its message handler */
/* files dropped at (x, y) in the window's surface coordinates (n of them, their fds taken over), else the dropped text
 * (a uri-list's lines or plain text; may be NULL) */
void webxfer_drop(Web *w, double x, double y, WebDropFile *files, int n, const char *uris, const char *text);
/* an image from the session's clipboard, pasted into the focused element (data taken over) */
void webxfer_paste_image(Web *w, const char *mime, GBytes *data);
/* the context menu's Copy Image: the page's image at uri (read by the page's own origin, in lord-web's isolated world,
 * made a PNG when it is in another format) goes to the session's clipboard */
void webxfer_copy_image(Web *w, const char *uri);

/* webdark.c */
void webdark_init(Web *w);                               /* the script and the background, before the first load */
void web_set_scheme(Web *w, int scheme);                 /* the desktop's mode changed (SetColorScheme) */
void webdark_load(Web *w, WebKitLoadEvent ev);           /* the view's load-changed */

/* webselect.c: dragging across text selects it (WPE 2.48 has no drag support, which in WebCore holds that too) */
void webselect_init(Web *w);

/* webbus.c */
void webbus_init(Web *w);
typedef void (*WebAskDone)(Web *w, int kind, const char *answer, void *data);
/* the registry's word for kind, asking lord-server (org.lordos.WebApps1.Ask) when it is "ask"; cb gets "allow",
 * "deny" or "once" (an error or no answer is "deny") */
void webbus_ask(Web *w, int kind, WebAskDone cb, void *data);
void webbus_open_uri(Web *w, const char *uri);
void webbus_notify(Web *w, WebKitNotification *n);
void webbus_notify_text(Web *w, const char *summary, const char *body);
void webbus_set_badge(Web *w, unsigned count);           /* org.lordos.WebApps1.SetBadge, no answer awaited */
void webbus_choose_files(Web *w, WebKitFileChooserRequest *req);
/* org.lordos.WebApp1.Edit(command) from sender: run only once the bus says sender is w->server_pid's */
void webbus_edit(Web *w, const char *sender, int cmd);
/* org.lordos.WebApp1.SetColorScheme(scheme) from sender: the same check, then web_set_scheme */
void webbus_scheme(Web *w, const char *sender, int scheme);
/* the page's context menu: org.lordos.WebApps1.Menu(app, profile, items) on lord-server, which shows it at the
 * pointer; the chosen item goes to web_menu_chosen with link and image (copied here) */
void webbus_menu(Web *w, const int *items, int n, const char *link, const char *image);

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
