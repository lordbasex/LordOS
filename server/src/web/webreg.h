/* webreg.h — the web app registry as lord-web reads it (docs/WEBAPPS.md, "The registry"): one file per app,
 * ~/.config/lordos/web/apps/<app>.json, written only by lord.webapps in lord-server. lord-web reads its own app's file,
 * read-only, and refuses to start on anything it doesn't expect: an unknown key, a wrong type, an id or URL that
 * isn't valid, a scope that isn't a prefix of the start address, a file over WEBREG_FILE_MAX, an app or profile that
 * isn't the one it was started for. Pure apart from webreg_load (tests/webapp_test.c). */
#ifndef LORD_WEB_WEBREG_H
#define LORD_WEB_WEBREG_H

#include "weburl.h"

#include <stddef.h>

#define WEBREG_ID_MAX       40
#define WEBREG_NAME_MAX     128          /* bytes of UTF-8 */
#define WEBREG_PROFILES_MAX 16
#define WEBREG_ICON_MAX     128
#define WEBREG_FILE_MAX     (64 * 1024)

/* what a page may ask for, and the registry's word for each (its "permissions" keys) */
enum { WK_NOTIFICATIONS, WK_CAMERA, WK_MICROPHONE, WK_SCREEN, WK_LOCATION, WK_COUNT };
typedef enum { WP_DENY, WP_ASK, WP_ALLOW } WebPerm;

typedef struct {
    char id[WEBREG_ID_MAX + 1];
    char name[WEBREG_NAME_MAX + 1];
} WebProfile;

typedef struct {
    char id[WEBREG_ID_MAX + 1];
    char name[WEBREG_NAME_MAX + 1];
    char start_url[WEBURL_MAX + 1];
    char scope[WEBURL_MAX + 1];
    char origin[WEBURL_ORIGIN_MAX + 1];
    char icon[WEBREG_ICON_MAX + 1];      /* relative to ~/.config/lordos/web, or "" */
    char theme_color[8];                 /* "#rrggbb", or "" */
    int catalog;                         /* source: 1 "catalog", 0 "url" */
    int background;
    int nprofiles;
    WebProfile profiles[WEBREG_PROFILES_MAX];
    WebPerm perms[WK_COUNT];
} WebApp;

/* ^[a-z0-9][a-z0-9-]{0,39}$ : an app id and a profile id */
int webreg_id_ok(const char *s);
/* the "permissions" key of a kind ("notifications", "camera", ...); NULL past WK_COUNT */
const char *webreg_kind_name(int kind);
/* 1 when s is UTF-8 of 1..max bytes with no control character */
int webreg_text_ok(const char *s, size_t max);
/* Reads and checks one registry file's text. 0 and *out filled; -1 with the reason in err. */
int webreg_parse(const char *text, size_t len, WebApp *out, char *err, size_t errlen);
/* The profile with that id, or NULL. */
const WebProfile *webreg_profile(const WebApp *a, const char *id);
/* Everything lord-web checks before it starts for (app, profile): both ids valid, the file's id is app, profile one of
 * its profiles. 0, or -1 with the reason in err. */
int webreg_check_launch(const WebApp *a, const char *app, const char *profile, char *err, size_t errlen);
/* Reads <home>/.config/lordos/web/apps/<app>.json (a regular file, opened without following a link, at most
 * WEBREG_FILE_MAX bytes) and parses it. 0, or -1 with the reason in err. */
int webreg_load(const char *home, const char *app, WebApp *out, char *err, size_t errlen);

#endif
