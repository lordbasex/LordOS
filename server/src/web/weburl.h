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
/* The user-script allow-list pattern (WebKit's "scheme://host/path*") of the addresses in scope, so a script is not even
 * put into another site's page: 0 and out filled, -1 when scope doesn't parse or out is too small. WebKit's patterns
 * have no port, so a scope with one gives its host on every port, and a '*' in scope's path ends the pattern there:
 * a pattern may be wider than the scope, never narrower (weburl_in_scope still decides). */
int weburl_scope_pattern(const char *scope, char *out, size_t n);
/* Where a host is: WEBURL_PUBLIC, or WEBURL_PRIVATE for this computer and the local networks: localhost and *.localhost,
 * *.local, 127.0.0.0/8, 0.0.0.0/8, 10/8, 100.64/10, 172.16/12, 192.168/16, 169.254/16, ::, ::1, fc00::/7, fe80::/10, and
 * an IPv4-mapped IPv6 address of those. An IPv4 address is read the way browsers do (shortened, hex or octal parts:
 * "127.1", "0x7f.1"). Names: localhost, a single label ("router") and the local networks' suffixes (.localhost, .local,
 * .lan, .home, .home.arpa, .internal, .intranet, .localdomain, .corp). What a name resolves to is not seen here (no DNS
 * lookups: weburl_addrs_class, fed by the probe). */
enum { WEBURL_PUBLIC, WEBURL_PRIVATE };
int weburl_host_class(const WebUrl *u);
/* 1 when the host is an address (IPv6 in brackets, or IPv4 the way browsers read one), not a name to resolve */
int weburl_host_is_literal(const WebUrl *u);
/* The addresses a name resolved to (family 4 or 6, network order): WEBURL_PRIVATE when any of them is private by the
 * rules above, or when there are none; WEBURL_PUBLIC only when all of them are public. The probe resolves a name
 * itself (webprobe.c) and classifies every address, so a public-looking name that leads here (/etc/hosts, a LAN name,
 * DNS rebinding) is private too. */
typedef struct { int family; unsigned char a[16]; } WebAddr;
int weburl_addrs_class(const WebAddr *a, int n);
/* The probe (webprobe.c): 1 when a redirect or main-frame navigation from `from` to `to` keeps the request's scheme and
 * place: never https to http, never from a public host to a private one. */
int weburl_probe_hop_ok(const char *from, const char *to);
/* 1 when the address the probe ended on may be read for the address it was asked for: https (or http on this
 * computer, for an http request on it), and private only when the request was private too. */
int weburl_probe_final_ok(const char *typed, const char *final);
/* One hop of the probe, by the class its address resolved to (WEBURL_PUBLIC, WEBURL_PRIVATE, or -1 when it did not
 * resolve). *seen_public says a hop so far was public; then this one must be public too. 1 allowed (and *seen_public
 * updated: a public or unknown hop sets it), 0 refused. */
int weburl_probe_step(int *seen_public, int cls);
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
