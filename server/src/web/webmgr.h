/* webmgr.h — lord-server's side of web apps (docs/WEBAPPS.md): the registry's writes, the probe and the
 * lord-web processes. lord.webapps (ui/js_webapps.c) is its only caller; isolated apps never reach it
 * (not in the broker).
 *
 *   - Writes: only here, each one a whole registry file checked by webreg_check and written atomically
 *     (~/.config/lordos/web/apps/<app>.json, 0600). The folder is in fsops_protected.
 *   - Installing never takes a URL from JS: a catalog app comes from LordOS's own catalog
 *     (<assets>/webapps/catalog.json, installed root-owned with LordOS), an app from an address from a
 *     finished probe (what lord-web --probe read from the page the person typed).
 *   - Starting lord-web: a fixed path next to the lord-server binary (owned by root or the user, not writable by
 *     others), argv exactly "lord-web --app <app> --profile <profile>" (both looked up in the registry first),
 *     and only the session's WAYLAND_DISPLAY, XDG_RUNTIME_DIR, DBUS_SESSION_BUS_ADDRESS, HOME and LANG plus
 *     LORDOS_WEB=1 (and LORDOS_SESSIOND=1 under lord-sessiond, so lord-web refuses to run without its scope), and
 *     LORDOS_WEB_SCHEME=light|dark once the theme has told the desktop's mode (webscheme.h).
 *     One process per app and profile; a second launch only says it is running (the shell
 *     raises its window, and ui/webalive.c checks it still answers).
 *   - Developer tools (docs/WEBAPPS.md, "Developer tools"; webinspect.h): developer mode is one switch,
 *     ~/.config/lordos/web/developer.json, written only here. webmgr_inspect ends that app and profile's lord-web and
 *     starts it again as "lord-web --app <app> --profile <profile> --inspect", only while the switch is on (lord-web
 *     checks the file too); turning the switch off ends every lord-web started that way.
 *   - The probe, the runtime's two steps (docs/WEBAPPS.md): "lord-web --probe", the address as one line on fd 3
 *     (https: only, under 2 KB), one JSON line back ({"ok":false,"error":"web.probe.…"} passes its code on to JS);
 *     then, when the page listed one, "lord-web --probe-icon" with the icon webreg_probe_parse chose, which answers
 *     {"ok":true,"type","size"} and the bytes (a refused or bad icon only means no icon). Each step is killed
 *     after 20 s (WEB_PROBE_MS).
 *
 * Every call returns NULL on success or an error code for js_throw_code ("webapps.notInstalled", …). */
#ifndef LORD_WEB_WEBMGR_H
#define LORD_WEB_WEBMGR_H

#include "webreg.h"

#include <stddef.h>
#include <sys/types.h>

#define WEB_MAX_RUNNING   32
#define WEB_PROBE_MS      20000
#define WEB_PROBE_SLOTS   8
#define WEB_PROBE_RESERVED 2        /* slots a background probe (the Store's icons) never takes: the person's own probes */
#define WEB_PROBE_BG_KEEP_MS 60000  /* a finished background probe nobody read in this long is abandoned: its slot is free */

/* The folder LordOS's assets are in (the catalog and its icons). Called once, before anything else. */
void webmgr_init(const char *assets_dir);
void webmgr_reap(void);                         /* wait for the lord-web processes that ended (main loop) */
int  webmgr_available(void);                    /* 1 when lord-web is installed next to lord-server and safe to run */

/* Each valid installed app, sorted by id: fn gets the checked struct, its file's text and the icon's
 * absolute path ("" if none). Files that fail the check are skipped (and logged once). */
void webmgr_each(void (*fn)(const WebApp *a, const char *text, size_t len, const char *icon, void *ctx), void *ctx);
const char *webmgr_load(const char *app, WebApp *a);

/* entry: {"source":"catalog","id":…,"profile":<name>} or {"source":"url","probe":<id>,"name":…,"profile":<name>}.
 * app (out) gets the installed id. */
const char *webmgr_install(const char *entry, size_t len, char *app, size_t n);
const char *webmgr_add_profile(const char *app, const char *name, char *id, size_t n);
const char *webmgr_rename_profile(const char *app, const char *profile, const char *name);
const char *webmgr_remove_profile(const char *app, const char *profile, int delete_data);
const char *webmgr_set_permission(const char *app, const char *kind, const char *value);
const char *webmgr_set_background(const char *app, int on);
const char *webmgr_uninstall(const char *app, int delete_data);

/* The desktop's color scheme (WEBSCHEME_LIGHT or WEBSCHEME_DARK; anything else: not known) for the next lord-web's
 * environment; webask_scheme tells the running ones. */
void webmgr_set_scheme(int scheme);
int  webmgr_scheme(void);                       /* -1 until set */
/* The scheme app's lord-web is told: its Appearance (registry colorScheme) light or dark, else the desktop's (-1 not
 * known yet, or an app that can't be read and the desktop's not known). Until WPEPlatform always the desktop's
 * (webscheme.h, WEBSCHEME_PER_APP). */
int  webmgr_scheme_of(const char *app);
/* A profile's Browser identity (registry userAgent, WEBREG_UA_*), or -1 when the app or the profile can't be read. */
int  webmgr_user_agent_of(const char *app, const char *profile);

/* *raised = 1 when it was already running (nothing started). */
const char *webmgr_launch(const char *app, const char *profile, int *raised);
typedef struct { char app[WEBREG_ID_MAX + 1], profile[WEBREG_ID_MAX + 1]; pid_t pid; int inspect; } WebRunning;
int  webmgr_running(WebRunning *out, int max);  /* the live processes (the ended ones are reaped first) */
/* The app and profile of the lord-web process that pid is, or descends from; 0 found, -1 not one of ours. */
int  webmgr_owner(pid_t pid, char *app, char *profile);

/* Force quit and restart (docs/WEBAPPS.md, "When a web app stops answering"): Settings → Web apps, the window's menu,
 * and ui/webalive.c when a lord-web stops answering its ping. Only a lord-web this lord-server started for that app and
 * profile (the kids table), never a pid from JS: its process group gets TERM, then KILL after 2 s, then each WebKit
 * process that was under it and is still the same process (pid and start time, webmem.h) gets KILL.
 * webmgr_quit: *was = 1 when it ran. webmgr_restart: ends it when it runs (*was = 1), then starts it again (with
 * developer tools again only while developer mode is on). webmgr_restart_all: every running one; how many started again. */
const char *webmgr_quit(const char *app, const char *profile, int *was);
const char *webmgr_restart(const char *app, const char *profile, int *was);
int  webmgr_restart_all(void);

/* Developer mode (Settings → Web Apps): 1 on, 0 off (no file, or one that isn't valid). Turning it off ends every
 * lord-web started with --inspect. */
int  webmgr_developer(void);
/* Settings → Web apps → Search the web with: the engine (websearch.h's WEBSEARCH_*; Google without a valid file), and
 * a new one by name (google, duckduckgo, bing, startpage) into ~/.config/lordos/web/search.json */
int  webmgr_search_engine(void);
const char *webmgr_set_search_engine(const char *name);
const char *webmgr_set_developer(int on);
/* That app and profile's window with developer tools on: its lord-web, if it runs without them, is ended, then started
 * again with --inspect. "webapps.developerOff" while developer mode is off; *already = 1 when it runs with them
 * (nothing started). */
const char *webmgr_inspect(const char *app, const char *profile, int *already);

/* A probe of url: its id (> 0), or 0 with *err set ("webapps.tooMany" when every slot is taken). The person's own
 * (Add from an address, Refresh icon) may use every slot, and take one from a finished background probe; a
 * background one (the Store's icon of a catalog app) never takes the last WEB_PROBE_RESERVED, so the person is
 * never kept waiting behind them (QA 0.8.9.6.2, BUG-E). webmgr_probe is webmgr_probe_ex(url, 0, err). */
int  webmgr_probe(const char *url, const char **err);
int  webmgr_probe_ex(const char *url, int background, const char **err);
/* Its answer as JSON (malloc'd), or NULL while it runs; *gone = 1 for an unknown id. */
char *webmgr_probe_result(int id, int *gone);
/* A finished probe whose answer was read and won't be installed (an icon fetch): its slot and icon file go now
 * instead of after the 15 minutes kept for an Install. Nothing while it runs. */
void webmgr_probe_release(int id);
/* -1: unknown id; 0: running; else the milliseconds since it finished (at least 1). */
long webmgr_probe_age(int id);

/* For tests: run this program instead of the lord-web next to lord-server. */
void webmgr_set_helper(const char *path);
/* For tests: the data folder of an app ("" for the whole app) and the registry folder. */
int  webmgr_data_dir(const char *app, const char *profile, char *out, size_t n);
const char *webmgr_config_dir(void);

#endif
