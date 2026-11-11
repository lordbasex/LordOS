/* webreg.h — the web app registry (docs/WEBAPPS.md, "The registry"): one file per installed web app,
 * ~/.config/lordos/web/apps/<app>.json, written only by lord-server's lord.webapps calls (in
 * fsops_protected) and read, read-only, by lord-web.
 *
 * Pure: parsing, checking and writing the JSON text, the URL and name rules, the probe's answer and the
 * icon check. No file system or process here except webreg_read_file / webreg_write_file, so lord-web and
 * the tests link it alone (tests/webreg_test.c).
 *
 *   - ids (app and profile): ^[a-z0-9][a-z0-9-]{0,39}$
 *   - startUrl and scope: https: URLs (http: only for localhost) of the same origin, scope a prefix of
 *     startUrl; printable ASCII only (a host in punycode), no user or password in them
 *   - a permission is allow, ask or deny; source is catalog or url
 *   - unknown keys are refused, and the file is at most WEB_FILE_MAX bytes */
#ifndef LORD_WEB_WEBREG_H
#define LORD_WEB_WEBREG_H

#include <stddef.h>

#define WEB_ID_MAX        40
#define WEB_NAME_MAX      64            /* bytes of UTF-8 */
#define WEB_URL_MAX       2048
#define WEB_PROFILES_MAX  16
#define WEB_FILE_MAX      (64u << 10)
#define WEB_ICON_MAX      (1u << 20)
#define WEB_SCHEMA        1

enum { WEB_PERM_NOTIFICATIONS, WEB_PERM_CAMERA, WEB_PERM_MICROPHONE, WEB_PERM_SCREEN, WEB_PERM_LOCATION, WEB_PERM_N };
enum { WEB_ALLOW, WEB_ASK, WEB_DENY };
enum { WEB_SRC_CATALOG, WEB_SRC_URL };

typedef struct {
    char id[WEB_ID_MAX + 1];
    char name[WEB_NAME_MAX + 1];
} WebProfile;

typedef struct {
    char id[WEB_ID_MAX + 1];
    char name[WEB_NAME_MAX + 1];
    char start_url[WEB_URL_MAX + 1], scope[WEB_URL_MAX + 1], origin[WEB_URL_MAX + 1];
    char icon[WEB_ID_MAX + 16];        /* "icons/<id>.png" or "icons/<id>.svg", or "" */
    char theme_color[8];               /* "#rrggbb" or "" */
    int  source;                       /* WEB_SRC_* */
    int  background;
    int  nprofiles;
    WebProfile profiles[WEB_PROFILES_MAX];
    int  perms[WEB_PERM_N];            /* WEB_ALLOW / WEB_ASK / WEB_DENY */
} WebApp;

/* ---- the rules ---- */
int  webreg_id_ok(const char *s);
/* A name the person sees (an app's, a profile's): valid UTF-8, 1..WEB_NAME_MAX bytes, no control or bidi
 * override characters, no space at either end. */
int  webreg_name_ok(const char *s);
/* An https: URL (http: only for localhost, 127.0.0.1 and [::1]); origin (optional) gets its origin
 * ("https://host[:port]", without a default port). 0 ok, -1 not. */
int  webreg_url_origin(const char *url, char *origin, size_t n);
/* startUrl, scope and origin agree: both URLs valid, of that origin, and scope a prefix of startUrl. */
int  webreg_urls_ok(const char *start_url, const char *scope, const char *origin);
/* The folder of url ("https://a.example/x/y?q" → "https://a.example/x/"): a scope when none was given. */
int  webreg_default_scope(const char *url, char *out, size_t n);

const char *webreg_perm_name(int kind);        /* "notifications" … ; NULL out of range */
int  webreg_perm_kind(const char *name);       /* -1 if unknown */
const char *webreg_perm_value_name(int v);     /* "allow" / "ask" / "deny" */
int  webreg_perm_value(const char *name);      /* -1 if unknown */

/* ---- the file ---- */
/* Parses and checks one registry file. 0 ok; -1 refused, with why (optional) saying what is wrong. */
int  webreg_parse(const char *json, size_t len, WebApp *a, char *why, size_t wn);
/* Checks a filled-in struct by the same rules (what webreg_write writes is always parseable). */
int  webreg_check(const WebApp *a, char *why, size_t wn);
/* The file's text (NUL-terminated) into out; its length, or 0 if a is not valid or does not fit. */
size_t webreg_write(const WebApp *a, char *out, size_t cap);
/* The defaults for a new app: no profiles, notifications/camera/microphone/screen ask, location deny. */
void webreg_init(WebApp *a);

/* ---- profiles ---- */
int  webreg_profile_index(const WebApp *a, const char *profile);    /* -1 if not there */
/* A profile id from its name ("Work" → "work", "Mi trabajo" → "mi-trabajo"), unique in a ("work-2"); "profile"
 * when the name has no ASCII letters or digits. 0 ok, -1 when a is full or nothing fits. */
int  webreg_profile_new_id(const WebApp *a, const char *name, char *id, size_t n);
/* An app id from a name, the same way, unique against taken(id) (NULL: anything goes). */
int  webreg_slug(const char *name, char *id, size_t n, int (*taken)(const char *id, void *ctx), void *ctx);

/* ---- lord-web --probe's answers (docs/WEBAPPS.md, "Adding an app from an address") ---- */
typedef struct {
    int  ok, manifest;
    char name[WEB_NAME_MAX + 1];
    char start_url[WEB_URL_MAX + 1], scope[WEB_URL_MAX + 1], origin[WEB_URL_MAX + 1];
    char theme_color[8];
} WebProbe;
/* The first line: { ok, name, startUrl, scope, origin, themeColor, icons, manifest }. The page wrote most
 * of it, so it is made safe here: a name with control characters is cleaned and cut, an unusable one
 * becomes the host; a scope that does not fit startUrl becomes startUrl's folder; the origin is startUrl's.
 * 0 when it is a usable answer (ok true and a valid startUrl), -1 otherwise. */
int  webreg_probe_parse(const char *line, size_t len, WebProbe *p);
/* The icon step's header line: {"icon":{"type":"image/png"|"image/svg+xml","size":N}} or {"icon":null}.
 * 1 an icon of *size bytes follows (*svg set for an SVG), 0 none, -1 not that line. */
int  webreg_icon_header(const char *line, size_t len, size_t *size, int *svg);
/* Is it a PNG (signature and IHDR) or an SVG (an <svg> element, no script), at most WEB_ICON_MAX bytes?
 * 1 PNG, 2 SVG, 0 neither. */
int  webreg_icon_kind(const unsigned char *b, size_t n);

/* ---- files (lord-server and lord-web) ---- */
/* A regular file opened without following a link, at most max bytes (malloc'd, NUL-terminated). NULL if not. */
char *webreg_read_file(const char *path, size_t max, size_t *len);
/* Writes path atomically (a .tmp beside it, fsync, rename), mode 0600. 0 ok. */
int  webreg_write_file(const char *path, const void *data, size_t len);

#endif
