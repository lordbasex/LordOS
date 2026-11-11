/* webreg.h — the web app registry (docs/WEBAPPS.md, "The registry"): one file per installed web app,
 * ~/.config/lordos/web/apps/<app>.json, written only by lord-server's lord.webapps calls (in fsops_protected)
 * and read, read-only, by lord-web, which refuses to start on anything it doesn't expect. One module for both
 * sides, so what lord-server writes is exactly what lord-web accepts.
 *
 * Pure: parsing, checking and writing the JSON text, the URL and name rules, the probe's answers and the icon
 * check. The file system only in webreg_load, webreg_read_file and webreg_write_file. Tests: tests/webreg_test.c.
 *
 *   - ids (app and profile): ^[a-z0-9][a-z0-9-]{0,39}$
 *   - names (the app's, each profile's): UTF-8 of 1..WEBREG_NAME_MAX bytes, no control, bidi override or
 *     zero-width character, no space at either end
 *   - startUrl and scope: https: URLs (http: only for localhost, 127.0.0.1 and [::1]) of the same origin, in
 *     lower case, printable ASCII only (a host in punycode), no user or password; origin is exactly
 *     "scheme://host[:port]" (no default port); scope has no query or fragment and is a prefix of startUrl
 *   - icon: "icons/<id>.png" or "icons/<id>.svg"; themeColor "#rrggbb"
 *   - a permission is allow, ask or deny; source is catalog or url; background a boolean
 *   - optional: tray, a boolean (false when missing: the tray icon of an app in the background); notifySound, one of
 *     auto (when missing), app, system or none (LordOS's sound for the page's notifications, webpage.h)
 *   - schema, id, name, startUrl, scope, origin, source and profiles (1..16, ids unique) are required;
 *     unknown keys (also in a profile or in permissions) and repeated keys are refused; at most 64 KB */
#ifndef LORD_WEB_WEBREG_H
#define LORD_WEB_WEBREG_H

#include <stddef.h>

#define WEBREG_ID_MAX         40
#define WEBREG_NAME_MAX       64             /* bytes of UTF-8 */
#define WEBREG_URL_MAX        2048
#define WEBREG_PROFILES_MAX   16
#define WEBREG_FILE_MAX       (64 * 1024)
#define WEBREG_ICON_PATH_MAX  (WEBREG_ID_MAX + 15)      /* "icons/<id>.png" */
#define WEBREG_ICON_BYTES     (1024 * 1024)             /* an icon file */
#define WEBREG_SCHEMA         1

/* what a page may ask for; the registry's "permissions" keys */
enum { WEBREG_PERM_NOTIFICATIONS, WEBREG_PERM_CAMERA, WEBREG_PERM_MICROPHONE, WEBREG_PERM_SCREEN, WEBREG_PERM_LOCATION, WEBREG_PERM_N };
/* a permission's value */
enum { WEBREG_ALLOW, WEBREG_ASK, WEBREG_DENY };
/* "source" */
enum { WEBREG_SRC_CATALOG, WEBREG_SRC_URL };
/* "notifySound" */
enum { WEBREG_SOUND_AUTO, WEBREG_SOUND_APP, WEBREG_SOUND_SYSTEM, WEBREG_SOUND_NONE, WEBREG_SOUND_N };

typedef struct {
    char id[WEBREG_ID_MAX + 1];
    char name[WEBREG_NAME_MAX + 1];
} WebProfile;

typedef struct {
    char id[WEBREG_ID_MAX + 1];
    char name[WEBREG_NAME_MAX + 1];
    char start_url[WEBREG_URL_MAX + 1], scope[WEBREG_URL_MAX + 1], origin[WEBREG_URL_MAX + 1];
    char icon[WEBREG_ICON_PATH_MAX + 1];  /* relative to ~/.config/lordos/web, or "" */
    char theme_color[8];                  /* "#rrggbb", or "" */
    int  source;                          /* WEBREG_SRC_* */
    int  background;
    int  tray;                            /* the tray icon while in the background (written only when true) */
    int  notify_sound;                    /* WEBREG_SOUND_* (written only when not auto) */
    int  nprofiles;
    WebProfile profiles[WEBREG_PROFILES_MAX];
    int  perms[WEBREG_PERM_N];            /* WEBREG_ALLOW / WEBREG_ASK / WEBREG_DENY */
} WebApp;

/* ---- the rules ---- */
int  webreg_id_ok(const char *s);
int  webreg_name_ok(const char *s);
/* An https: URL (http: only for localhost, 127.0.0.1 and [::1]); origin (optional) gets its origin
 * ("https://host[:port]", without a default port). 0 ok, -1 not. */
int  webreg_url_origin(const char *url, char *origin, size_t n);
/* 1 when startUrl, scope and origin agree: both URLs valid, of that origin, scope a prefix of startUrl. */
int  webreg_urls_ok(const char *start_url, const char *scope, const char *origin);
/* The folder of url ("https://a.example/x/y?q" → "https://a.example/x/"): a scope when none was given. */
int  webreg_default_scope(const char *url, char *out, size_t n);

const char *webreg_perm_name(int kind);        /* "notifications" … ; NULL out of range */
int  webreg_perm_kind(const char *name);       /* -1 if unknown */
const char *webreg_perm_value_name(int v);     /* "allow" / "ask" / "deny" */
int  webreg_perm_value(const char *name);      /* -1 if unknown */
const char *webreg_sound_name(int v);          /* "auto" / "app" / "system" / "none"; NULL out of range */
int  webreg_sound_value(const char *name);     /* -1 if unknown */

/* ---- the file ---- */
/* Parses and checks one registry file. 0 ok; -1 refused (a zeroed), with why (optional) saying what is wrong. */
int  webreg_parse(const char *json, size_t len, WebApp *a, char *why, size_t wn);
/* Checks a filled-in struct by the same rules (what webreg_write writes is always parseable). */
int  webreg_check(const WebApp *a, char *why, size_t wn);
/* The file's text (NUL-terminated) into out; its length, or 0 if a is not valid or does not fit. */
size_t webreg_write(const WebApp *a, char *out, size_t cap);
/* The defaults for a new app: no profiles, source url, notifications/camera/microphone/screen ask, location deny. */
void webreg_init(WebApp *a);

/* ---- profiles ---- */
int  webreg_profile_index(const WebApp *a, const char *profile);    /* -1 if not there */
const WebProfile *webreg_profile(const WebApp *a, const char *id);  /* NULL if not there */
/* A profile id from its name ("Work" → "work", "Mi trabajo" → "mi-trabajo"), unique in a ("work-2"); "profile"
 * when the name has no ASCII letters or digits. 0 ok, -1 when a is full or nothing fits. */
int  webreg_profile_new_id(const WebApp *a, const char *name, char *id, size_t n);
/* An app id from a name, the same way, unique against taken(id) (NULL: anything goes). */
int  webreg_slug(const char *name, char *id, size_t n, int (*taken)(const char *id, void *ctx), void *ctx);

/* ---- lord-web's side ---- */
/* Everything lord-web checks before it starts for (app, profile): both ids valid, the file's id is app, profile one of
 * its profiles. 0, or -1 with the reason in why. */
int  webreg_check_launch(const WebApp *a, const char *app, const char *profile, char *why, size_t wn);
/* Reads <home>/.config/lordos/web/apps/<app>.json (a regular file, opened without following a link, at most
 * WEBREG_FILE_MAX bytes) and parses it. 0, or -1 with the reason in why. */
int  webreg_load(const char *home, const char *app, WebApp *a, char *why, size_t wn);

/* The bus name a running lord-web serves org.lordos.WebApp1.Show at (/org/lordos/WebApp1):
 * "org.lordos.WebApp.app_<app>.profile_<profile>", '-' as '_' (ids never hold a '_'). lord-server calls it to bring a
 * running app forward; a second lord-web for the same profile would too. 0, or -1 for a bad id or a short buffer. */
#define WEBREG_SHOW_PATH  "/org/lordos/WebApp1"
#define WEBREG_SHOW_IFACE "org.lordos.WebApp1"
int  webreg_bus_name(const char *app, const char *profile, char *out, size_t n);

/* ---- lord-web --probe's answers (docs/WEBAPPS.md, "Adding an app from an address") ---- */
typedef struct {
    int  ok, manifest;
    char name[WEBREG_NAME_MAX + 1];
    char start_url[WEBREG_URL_MAX + 1], scope[WEBREG_URL_MAX + 1], origin[WEBREG_URL_MAX + 1];
    char theme_color[8];
    char icon_url[WEBREG_URL_MAX + 1];  /* the icon chosen from "icons" for --probe-icon, or "" */
    char error[48];                     /* {"ok":false,"error":"web.probe.…"}: the runtime's code, or "" */
} WebProbe;
/* The --probe line: { ok, name, startUrl, scope, origin, themeColor, icons: [{ src, sizes, type }], manifest }. The
 * page wrote most of it, so it is made safe here: a name with control characters is cleaned and cut, an unusable one
 * becomes the host; a scope that does not fit startUrl becomes startUrl's folder; the origin is startUrl's; the icon
 * is the best PNG (the largest up to 512 px, else the smallest above) or else an SVG, https: only.
 * 0 when it is a usable answer (ok true and a valid startUrl); -1 otherwise, with p->error set to the runtime's code
 * when it answered {"ok":false,"error":"web.probe.<code>"} with one of the codes it documents. */
int  webreg_probe_parse(const char *line, size_t len, WebProbe *p);
/* 1 when code is one of the probe's documented error codes (web.probe.request, .load, .tls, .notHttps, .page,
 * .timeout, .iconType, .iconTooBig). */
int  webreg_probe_code_ok(const char *code);
/* The --probe-icon header line: {"ok":true,"type":"image/png"|"image/svg+xml","size":N} (1 ≤ N ≤ 1 MB).
 * 1 an icon of *size bytes follows (*svg set for an SVG); 0 the runtime refused ({"ok":false,"error":…}, the code
 * into err when it is a documented one); -1 not that line. */
int  webreg_icon_header(const char *line, size_t len, size_t *size, int *svg, char *err, size_t en);
/* Is it a PNG (signature, IHDR, 1..4096 px) or an SVG (an <svg> element, no script), at most WEBREG_ICON_BYTES?
 * 1 PNG, 2 SVG, 0 neither. */
int  webreg_icon_kind(const unsigned char *b, size_t n);

/* ---- files ---- */
/* A regular file opened without following a link, at most max bytes (malloc'd, NUL-terminated). NULL if not. */
char *webreg_read_file(const char *path, size_t max, size_t *len);
/* Writes path atomically (a .tmp beside it, fsync, rename), mode 0600. 0 ok. */
int  webreg_write_file(const char *path, const void *data, size_t len);

#endif
