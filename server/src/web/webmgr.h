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
 *     LORDOS_WEB=1 (and LORDOS_SESSIOND=1 under lord-sessiond, so lord-web refuses to run without its scope). One process per app and profile; a second launch only says it is running (the shell
 *     raises its window).
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

/* The folder LordOS's assets are in (the catalog and its icons). Called once, before anything else. */
void webmgr_init(const char *assets_dir);
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

/* *raised = 1 when it was already running (nothing started). */
const char *webmgr_launch(const char *app, const char *profile, int *raised);
typedef struct { char app[WEBREG_ID_MAX + 1], profile[WEBREG_ID_MAX + 1]; pid_t pid; } WebRunning;
int  webmgr_running(WebRunning *out, int max);  /* the live processes (the ended ones are reaped first) */
/* The app and profile of the lord-web process that pid is, or descends from; 0 found, -1 not one of ours. */
int  webmgr_owner(pid_t pid, char *app, char *profile);

/* A probe of url: its id (> 0), or 0 with *err set. */
int  webmgr_probe(const char *url, const char **err);
/* Its answer as JSON (malloc'd), or NULL while it runs; *gone = 1 for an unknown id. */
char *webmgr_probe_result(int id, int *gone);

/* For tests: run this program instead of the lord-web next to lord-server. */
void webmgr_set_helper(const char *path);
/* For tests: the data folder of an app ("" for the whole app) and the registry folder. */
int  webmgr_data_dir(const char *app, const char *profile, char *out, size_t n);
const char *webmgr_config_dir(void);

#endif
