/* weboffline.h — an app's own page that couldn't load for lack of network (#144), pure (tests/webapp_test.c).
 *
 * WebKit reports a failed main frame load with a GError. Its domain and code say whether the network was the cause:
 * GResolver's errors (no DNS: "Error resolving “host”: Name or service not known"), GIO's connection errors
 * (unreachable, refused, timed out, closed) and WebKit's own transport error. A cancelled load, a policy decision
 * (a download), a TLS error or an HTTP answer is not one: those keep WebKit's own handling.
 *
 * For a network error lord-web shows its own page (weboffline_html: no script, nothing from the site) and loads the
 * failed address again after weboffline_delay_s (2, 4, 8, 16, 32, then every 60 s), at once when the network comes
 * back, and when the window is shown again (webengine.c). The codes are mirrored here so this header needs no GLib;
 * webengine.c checks them against GIO's at compile time. */
#ifndef LORD_WEB_WEBOFFLINE_H
#define LORD_WEB_WEBOFFLINE_H

#include <stddef.h>
#include <string.h>

#define WEBOFFLINE_FIRST_S 2
#define WEBOFFLINE_MAX_S 60

/* the domains, as g_quark_to_string gives them */
#define WEBOFFLINE_DOMAIN_RESOLVER "g-resolver-error-quark"
#define WEBOFFLINE_DOMAIN_IO "g-io-error-quark"
#define WEBOFFLINE_DOMAIN_WEBKIT "WebKitNetworkError"

/* GIOErrorEnum's connection errors, and WEBKIT_NETWORK_ERROR_TRANSPORT */
enum {
    WEBOFFLINE_IO_TIMED_OUT = 24,
    WEBOFFLINE_IO_HOST_NOT_FOUND = 28,
    WEBOFFLINE_IO_HOST_UNREACHABLE = 37,
    WEBOFFLINE_IO_NETWORK_UNREACHABLE = 38,
    WEBOFFLINE_IO_CONNECTION_REFUSED = 39,
    WEBOFFLINE_IO_PROXY_FAILED = 40,
    WEBOFFLINE_IO_CONNECTION_CLOSED = 44,
    WEBOFFLINE_IO_NOT_CONNECTED = 45,
    WEBOFFLINE_WEBKIT_TRANSPORT = 300,
};

/* 1 when a load that failed with (domain, code) failed for lack of network */
static inline int weboffline_is_network_error(const char *domain, int code) {
    if (!domain) return 0;
    if (!strcmp(domain, WEBOFFLINE_DOMAIN_RESOLVER)) return 1;     /* not found, temporary failure, internal */
    if (!strcmp(domain, WEBOFFLINE_DOMAIN_WEBKIT)) return code == WEBOFFLINE_WEBKIT_TRANSPORT;
    if (!strcmp(domain, WEBOFFLINE_DOMAIN_IO))
        switch (code) {
        case WEBOFFLINE_IO_TIMED_OUT: case WEBOFFLINE_IO_HOST_NOT_FOUND: case WEBOFFLINE_IO_HOST_UNREACHABLE:
        case WEBOFFLINE_IO_NETWORK_UNREACHABLE: case WEBOFFLINE_IO_CONNECTION_REFUSED: case WEBOFFLINE_IO_PROXY_FAILED:
        case WEBOFFLINE_IO_CONNECTION_CLOSED: case WEBOFFLINE_IO_NOT_CONNECTED:
            return 1;
        default:
            return 0;
        }
    return 0;
}

/* seconds before the next try after the failures-th failure in a row (1 the first): 2, 4, 8, 16, 32, 60, 60... */
static inline unsigned weboffline_delay_s(unsigned failures) {
    if (failures < 1) failures = 1;
    if (failures > 6) return WEBOFFLINE_MAX_S;
    unsigned s = WEBOFFLINE_FIRST_S << (failures - 1);
    return s < WEBOFFLINE_MAX_S ? s : WEBOFFLINE_MAX_S;
}

/* appends s to out (at *len, n bytes in all) with &, <, >, " and ' escaped; 0, or -1 when it doesn't fit */
static inline int weboffline_put(char *out, size_t n, size_t *len, const char *s, int escape) {
    for (; s && *s; s++) {
        const char *e = NULL;
        if (escape)
            switch (*s) {
            case '&': e = "&amp;"; break;
            case '<': e = "&lt;"; break;
            case '>': e = "&gt;"; break;
            case '"': e = "&quot;"; break;
            case '\'': e = "&#39;"; break;
            default: break;
            }
        size_t k = e ? strlen(e) : 1;
        if (*len + k >= n) return -1;
        memcpy(out + *len, e ? e : s, k);
        *len += k;
    }
    out[*len] = 0;
    return 0;
}

/* lord-web's page for an app that is offline: title (the app's name), heading, body and hint (lord-web's words in the
 * person's language) and host (the site it couldn't reach, may be "") as text, in the desktop's light or dark colors.
 * No script runs in it (its Content-Security-Policy allows only its own style) and each line takes its direction from
 * its own text. The length written to out (n bytes), or 0 when it doesn't fit. */
static inline size_t weboffline_html(const char *title, const char *heading, const char *body, const char *hint, const char *host,
                                     int dark, char *out, size_t n) {
    size_t len = 0;
    if (!out || !n) return 0;
    out[0] = 0;
    const char *fg = dark ? "#e8e8ea" : "#1f1f23", *bg = dark ? "#1f1f23" : "#ffffff", *muted = dark ? "#a0a0a8" : "#5f5f66";
    int bad = weboffline_put(out, n, &len, "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                                           "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'\">"
                                           "<meta name=\"viewport\" content=\"width=device-width\"><title dir=\"auto\">", 0)
           || weboffline_put(out, n, &len, title, 1)
           || weboffline_put(out, n, &len, "</title><style>html,body{height:100%;margin:0}body{display:flex;align-items:center;"
                                           "justify-content:center;font-family:sans-serif;background:", 0)
           || weboffline_put(out, n, &len, bg, 0)
           || weboffline_put(out, n, &len, ";color:", 0)
           || weboffline_put(out, n, &len, fg, 0)
           || weboffline_put(out, n, &len, "}main{max-width:32em;padding:24px;text-align:center}h1{font-size:1.5em;font-weight:600;"
                                           "margin:0 0 12px}p{margin:0 0 8px;line-height:1.5}.m{color:", 0)
           || weboffline_put(out, n, &len, muted, 0)
           || weboffline_put(out, n, &len, ";font-size:.9em}</style></head><body><main><h1 dir=\"auto\">", 0)
           || weboffline_put(out, n, &len, heading, 1)
           || weboffline_put(out, n, &len, "</h1><p dir=\"auto\">", 0)
           || weboffline_put(out, n, &len, body, 1)
           || weboffline_put(out, n, &len, "</p>", 0);
    if (!bad && host && host[0])
        bad = weboffline_put(out, n, &len, "<p class=\"m\" dir=\"ltr\">", 0) || weboffline_put(out, n, &len, host, 1)
              || weboffline_put(out, n, &len, "</p>", 0);
    if (!bad)
        bad = weboffline_put(out, n, &len, "<p class=\"m\" dir=\"auto\">", 0) || weboffline_put(out, n, &len, hint, 1)
              || weboffline_put(out, n, &len, "</p></main></body></html>", 0);
    if (bad) { out[0] = 0; return 0; }
    return len;
}

#endif
