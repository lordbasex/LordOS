/* web.h — lord-web, the LordOS Web Runtime (docs/WEBAPPS.md): one process per installed web app and profile, the only
 * binary that links WPE WebKit. What its parts share:
 *
 *   webmain.c     argv, the registry check, isolation, then the app or a probe
 *   webengine.c   WebKit: the context and network session of the profile, the view, navigation, permissions,
 *                 notifications, downloads, the file chooser, the page of an app that is offline (weboffline.h)
 *   webwin.c      the Wayland window: xdg-shell, the frame's decoration, scale, the frames WebKit draws (dmabuf or
 *                 shared memory), keyboard, pointer, touch, the clipboard, the input method
 *   webback.h     the seam to WPE WebKit's view: webback_legacy.c (libwpe + wpebackend-fdo, WPE 2.48; webshm.c reads
 *                 wpebackend-fdo's own buffers) or webback_platform.c (WPEPlatform, WPE 2.50 and later), one of them
 *                 built (mk/web.mk)
 *   webbus.c      the session bus: notifications, the OpenURI and FileChooser portals, org.lordos.WebApps1.Ask
 *   webmain.c     also --inspect: WebKit's remote inspector on 127.0.0.1 for this window only (webinspect.h), behind
 *                 webproxy.c's gate (webguard.h: a one-time address, then a cookie; Host and Origin checked)
 *   webtray.c     the StatusNotifierItem of an app that keeps running in the background (when its "tray" is on)
 *   webdark.c     the desktop's light or dark mode: the page's prefers-color-scheme through matchMedia (webscheme.js),
 *                 the view's background while a page loads
 *   webselect.c   selecting text by dragging (a script in an isolated world: WPE 2.48 is built without drag support)
 *   webxfer.c     files dropped on the window and images pasted into the page (WebKit 2.48's WPE takes neither: a
 *                 script hands the page the bytes lord-web read itself), and the page's copies of an image
 *   webprobe.c    --probe and --probe-icon, headless, in a throwaway profile
 *   weblang.c     the few words lord-web shows itself (assets/locales/<lang>/webruntime.json)
 *   webargs.c, webreg.c, weburl.c, webmanifest.c, webdl.c, webaxis.c, webpage.c, webdrop.c, weboffline.h   the pure parts
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
#include "webinspect.h"
#include "webguard.h"

#include <limits.h>
#include <sys/types.h>

#include <wpe/webkit.h>

typedef struct WebWin WebWin;
typedef struct WebBack WebBack;
typedef struct WebTray WebTray;
typedef struct WebProxy WebProxy;

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
    WebBack *back;                      /* the view's backend (webback.h) */
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
    int user_agent;                     /* the WEBREG_UA_* the view has now (the registry's at start, SetUserAgent) */
    int scheme;                         /* the desktop's WEBSCHEME_LIGHT or WEBSCHEME_DARK (LORDOS_WEB_SCHEME, SetColorScheme) */
    char scheme_key[WEBSCHEME_KEY_LEN + 1]; /* webscheme.js's hook on window, random per process ("" none: no script) */
    WebKitUserScript *scheme_script;    /* the one added for the scheme now: replaced when it changes */
    int loading;                        /* between a load's start and its end (the dark background) */
    char *offline_uri;                  /* the app's address that failed for lack of network (weboffline.h), loaded again until it works, or NULL */
    unsigned offline_failures;          /* its failures in a row: the next try's delay (weboffline_delay_s) */
    guint offline_timer;                /* the next try, or 0 */
    int offline_retrying;               /* a try's load is under way */
    int offline_page;                   /* lord-web's own "No connection" page is in the window */
    unsigned inspect_port;              /* --inspect: WebKit's inspector on 127.0.0.1:<port>, a port browsers refuse (webguard.h), else 0 */
    guint inspect_timer;                /* waiting for that port to listen before the browser opens it, or 0 */
    int inspect_tries;
    WebProxy *inspect_proxy;            /* the gate the browser goes through (webproxy.c), once WebKit's port listens */
    guint32 inspect_note;               /* the "Developer tools are on" notification (its action opens them again), or 0 */
    int quitting;
    int exit_code;
} Web;

/* webengine.c */
WebKitNetworkSession *web_session_new(Web *w);           /* the profile's own folders, cookies in cookies.sqlite */
WebKitWebContext *web_context_new(void);                 /* memory pressure, no prewarmed process, cache model */
WebKitSettings *web_settings_new(int ua_kind);           /* the user agent (WEBREG_UA_*), developer extras off, no automatic windows */
/* the profile's Browser identity changed (SetUserAgent): the view's user agent, then the app's page loads again */
void web_set_user_agent(Web *w, int kind);
int web_engine_start(Web *w);                            /* the view on w->back, signals, first load */
void web_quit(Web *w, int code);
/* the window was closed: hidden behind the tray when the app runs in the background, otherwise quit */
void web_close_requested(Web *w);
/* the window is shown again (Show, the tray): an app whose page failed for lack of network tries again at once */
void web_show(Web *w, const char *activation_token);
/* Ctrl+R, F5: the page again, or, on lord-web's "No connection" page, the address that failed (inside the app's scope) */
void web_reload(Web *w);
/* the desktop's mode changed: lord-web's "No connection" page, when it is in the window, again in the new colors */
void web_offline_restyle(Web *w);
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
int  webxfer_in_scope(Web *w, const char *href);         /* href, the main frame's committed document and the view's address are in the app's scope, and the window isn't outside it */
/* files dropped at (x, y) in the window's surface coordinates (n of them, their fds taken over), else the dropped text
 * (a uri-list's lines or plain text; may be NULL) */
void webxfer_drop(Web *w, double x, double y, WebDropFile *files, int n, const char *uris, const char *text);
/* a drag over the window at (x, y) (surface coordinates): dragenter, dragover and dragleave reach the page as the
 * pointer moves, before any drop, so a page that shows its drop area only after dragenter has it there by the drop;
 * files is 1 for a drag of files (its types say "Files", its data comes only with the drop) */
void webxfer_drag(Web *w, double x, double y, int files);
/* the drag left the window without dropping: dragleave on the element it was over */
void webxfer_drag_leave(Web *w);
/* an image from the session's clipboard, pasted into the focused element (data taken over) */
void webxfer_paste_image(Web *w, const char *mime, GBytes *data);
/* local files from the session's clipboard (n of them, their fds taken over), pasted into the focused element: a
 * paste event whose DataTransfer holds them as Files */
void webxfer_paste_files(Web *w, WebDropFile *files, int n);
/* text (UTF-8) pasted into the focused element: a paste event with it as text/plain, and when no listener takes it
 * the text inserted at the caret (WebKit's InsertText). 0 when it can't be now (another transfer runs, another site). */
int webxfer_paste_text(Web *w, const char *text);
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
/* the developer tools' notification: its action ("default") calls web_inspect_open; its id in w->inspect_note */
void webbus_notify_inspect(Web *w, const char *summary, const char *body);
void webbus_set_badge(Web *w, unsigned count);           /* org.lordos.WebApps1.SetBadge, no answer awaited */
void webbus_choose_files(Web *w, WebKitFileChooserRequest *req);
/* org.lordos.WebApp1.Edit(command) from sender: run only once the bus says sender is w->server_pid's */
void webbus_edit(Web *w, const char *sender, int cmd);
/* org.lordos.WebApp1.SetColorScheme(scheme) from sender: the same check, then web_set_scheme */
void webbus_scheme(Web *w, const char *sender, int scheme);
/* org.lordos.WebApp1.SetUserAgent(chrome|edge|firefox|lordos) from sender: the same check, then web_set_user_agent */
void webbus_user_agent(Web *w, const char *sender, int kind);
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
/* --inspect: once WebKit's inspector listens on w->inspect_port from this process, the person's browser opens it */
void web_inspect_watch(Web *w);
/* opens the developer tools in the person's browser through OpenURI with a new one-time address (webguard.h) */
void web_inspect_open(Web *w);

/* webproxy.c: the gate in front of WebKit's inspector on 127.0.0.1:<backend> (webguard.h); NULL when it can't listen */
WebProxy *webproxy_start(unsigned backend);
/* a new one-time secret and its address, http://127.0.0.1:<proxy port>/<secret>/; 0, or -1 */
int webproxy_open_url(WebProxy *px, char *out, size_t n);
const char *web_self_dir(void);                          /* the folder of /proc/self/exe */
/* reads the app's registry file again (lord-server stores a permission's answer there); kept as it was when the new
 * file isn't valid or is for another app or profile */
void web_registry_refresh(Web *w);

#endif
