/* webguard.h — the gate in front of WebKit's remote inspector (docs/WEBAPPS.md, "Developer tools"): the pure parts of
 * lord-web's inspector proxy (webproxy.c). Tests: tests/webapp_test.c.
 *
 * WebKit's inspector HTTP server (WEBKIT_INSPECTOR_HTTP_SERVER) has no authentication: whoever reaches its port reads
 * the page and runs script in it. A web page in the person's browser could reach a plain 127.0.0.1 port (a port scan,
 * a WebSocket, which no CORS rule stops, DNS rebinding). So:
 *
 *   - WebKit listens on one of WEBGUARD_BACKEND_PORTS: ports on the browsers' "bad port" list (WHATWG Fetch; Chrome,
 *     Chromium and Firefox refuse every request and WebSocket to them, whatever the page asks), so no page can reach
 *     it. Browsers are only ever pointed at the proxy.
 *   - lord-web's proxy listens on another, free port of 127.0.0.1 and forwards one HTTP request per connection, or one
 *     WebSocket upgrade, to WebKit's port only when (webguard_check):
 *       the head is at most WEBGUARD_HEAD_MAX bytes, well formed (CRLF lines, no control bytes, no folded lines), a
 *       GET of an origin-form target without a body;
 *       Host is exactly 127.0.0.1:<proxy port> or localhost:<proxy port> (DNS rebinding);
 *       Origin, when there is one, is http://127.0.0.1:<proxy port> or http://localhost:<proxy port> (cross-site
 *       fetch and WebSocket);
 *       Sec-Fetch-Site, when there is one, is same-origin or none (a page of another port of this computer is "same
 *       site" for the cookie: its frames and no-cors requests, which carry no Origin, are refused this way);
 *       and the request carries the cookie WEBGUARD_COOKIE "-<proxy port>" with this window's random value.
 *     The one way to get that cookie is /<secret> or /<secret>/ (exactly), the address OpenURI is handed: it answers a
 *     redirect to / that sets the cookie (HttpOnly, SameSite=Strict, Path=/). The secret works once; every opening
 *     makes a new one, so the address in the browser's history is dead. The cookie lasts as long as the window.
 *     Anything else is a 403 and the connection closes.
 *   - Why a cookie and not the secret in every path: the inspector's own pages build their WebSocket address from
 *     window.location.host and an absolute path ("Main.html?ws=" + location.host + "/socket/1/1/WebPage"), and the
 *     browser sends the cookie on that WebSocket too (same site).
 *
 * Secrets and cookie values are 32 random bytes (getrandom) in base64url, compared in constant time. */
#ifndef LORD_WEB_WEBGUARD_H
#define LORD_WEB_WEBGUARD_H

#include <stddef.h>

#define WEBGUARD_HEAD_MAX   (16 * 1024)
#define WEBGUARD_RAND_BYTES 32
#define WEBGUARD_TOKEN_LEN  43                  /* base64url of 32 bytes, no padding */
#define WEBGUARD_COOKIE     "lordos-inspect"    /* the cookie's name is "lordos-inspect-<proxy port>" */

/* the ports WebKit's inspector may listen on: on Chrome's and Firefox's bad port list, 1024 and up (no root needed),
 * not X11's 6000 nor NFS's 2049 */
extern const unsigned WEBGUARD_BACKEND_PORTS[];
extern const size_t WEBGUARD_BACKEND_NPORTS;
int webguard_backend_port_ok(unsigned port);

/* base64url (no padding) of n bytes into out (at least 4*n/3+2 bytes); its length, or 0 when out is too small */
size_t webguard_b64url(const unsigned char *in, size_t n, char *out, size_t outn);

/* the proxy's keys for one window */
typedef struct {
    unsigned port;                              /* the proxy's own port */
    char secret[WEBGUARD_TOKEN_LEN + 1];        /* the opening address's path; "" once used */
    char cookie[WEBGUARD_TOKEN_LEN + 1];        /* the cookie's value */
} WebGuardKeys;

/* 0, or -1 for a token that isn't WEBGUARD_TOKEN_LEN base64url characters */
int webguard_set_keys(WebGuardKeys *k, unsigned port, const char *secret, const char *cookie);

/* Where a request head ends: the length up to and with its "\r\n\r\n", 0 while it isn't complete, -1 when it's longer
 * than WEBGUARD_HEAD_MAX (or len is, with no end) */
long webguard_head_end(const char *buf, size_t len);

enum { WEBGUARD_DENY, WEBGUARD_OPEN, WEBGUARD_FORWARD, WEBGUARD_UPGRADE };
/* The verdict on one complete request head (head_len from webguard_head_end):
 *   WEBGUARD_OPEN     /<secret> or /<secret>/: answer webguard_open_reply (the caller then forgets the secret)
 *   WEBGUARD_FORWARD  a plain request with the cookie: forward webguard_rewrite's head, then only WebKit's answer
 *   WEBGUARD_UPGRADE  a WebSocket upgrade with the cookie: forward the head as it is, then both ways after a 101
 *   WEBGUARD_DENY     anything else */
int webguard_check(const char *head, size_t head_len, const WebGuardKeys *k);

/* the head forwarded for WEBGUARD_FORWARD: the same request with its Connection, Keep-Alive, Proxy-Connection and
 * Upgrade lines dropped and "Connection: close" added (one request per connection: a browser may reuse a connection
 * for another site's request, so every request is checked). Its length, or 0 when out is too small. */
size_t webguard_rewrite(const char *head, size_t head_len, char *out, size_t n);

/* the redirect that sets the cookie, and the refusal; their length, or 0 when out is too small */
size_t webguard_open_reply(const WebGuardKeys *k, char *out, size_t n);
size_t webguard_deny_reply(char *out, size_t n);

/* "http://127.0.0.1:<port>/<secret>/": what OpenURI opens; 0, or -1 */
int webguard_open_url(const WebGuardKeys *k, char *out, size_t n);

/* 1 when the first bytes of WebKit's answer (len of them) say "HTTP/1.1 101 ", 0 when they say something else, -1 while
 * there are too few to tell */
int webguard_switched(const char *buf, size_t len);

#endif
