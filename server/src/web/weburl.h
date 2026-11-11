/* weburl.h — the addresses of a web app (docs/WEBAPPS.md): a strict reader of absolute http(s) URLs, their origin and
 * the app's scope. Pure: no allocation, no I/O; used by lord-web and its tests (tests/webapp_test.c).
 *
 * What is accepted: "https://" (or "http://" for localhost, 127.0.0.1 and [::1] only), a host of letters, digits, dots
 * and hyphens (international names arrive in their xn-- form) or a bracketed IPv6 address, an optional port, then a
 * path. No user or password in the address, no byte below 0x21 or above 0x7e, no backslash, at most WEBURL_MAX bytes.
 * The scheme and the host compare without case; the default port (443, 80) is the same as no port. */
#ifndef LORD_WEB_WEBURL_H
#define LORD_WEB_WEBURL_H

#include <stddef.h>

#define WEBURL_MAX 2048
#define WEBURL_HOST_MAX 253
#define WEBURL_ORIGIN_MAX (8 + WEBURL_HOST_MAX + 8)

typedef struct {
    int https;                          /* 1 https, 0 http */
    char host[WEBURL_HOST_MAX + 1];     /* lower case; an IPv6 address keeps its brackets */
    int port;                           /* 0 = the scheme's default */
    const char *path;                   /* into the parsed text: "/..." with query and fragment, or "" */
    size_t path_len;                    /* up to the fragment ('#'), which is never part of a comparison */
} WebUrl;

/* 0 and *u filled when s is an address as above; -1 otherwise. */
int weburl_parse(const char *s, WebUrl *u);
/* 1 when the host is this computer: localhost, 127.0.0.1 or [::1] */
int weburl_is_local(const WebUrl *u);
/* 1 when s parses and is https, or http on this computer: what an app's startUrl, scope and icons may be */
int weburl_app_ok(const char *s);
/* "https://host[:port]" (no path), lower case. 0, or -1 when s doesn't parse or out is too small. */
int weburl_origin(const char *s, char *out, size_t n);
/* 1 when a and b have the same origin */
int weburl_same_origin(const char *a, const char *b);
/* 1 when url is inside scope: the same origin and url's path (query included, fragment not) begins with scope's. */
int weburl_in_scope(const char *url, const char *scope);
/* 1 when a page may use the app's permissions (docs/WEBAPPS.md, "Permissions"): the window shows view_uri, an address
 * inside scope and of app_origin, and the origin that asks (asker, "https://host[:port]"; NULL when WebKit's API doesn't
 * say, as for a permission request) is app_origin too. Anything else (another site in the window after a redirect or a
 * navigation, a frame of another origin, an opaque origin, no address yet) gets every permission denied and nothing is
 * asked. */
int weburl_perm_allowed(const char *view_uri, const char *asker, const char *scope, const char *app_origin);
/* The folder of url: its origin and its path up to the last '/' before any query ("https://a.example/b/c?d" →
 * "https://a.example/b/"), the default scope of a manifest without one. 0, or -1. */
int weburl_dir(const char *url, char *out, size_t n);

#endif
