/* webua.h — Chrome's User-Agent Client Hints and navigator.userAgentData for a profile whose Browser identity is
 * "chrome" (docs/WEBAPPS.md, "Browser identity" and "Calls"), pure (tests/webua_test.c).
 *
 * Chrome tells a site what it is three ways: the User-Agent string (webpage_user_agent), the Sec-CH-UA-* request
 * headers and navigator.userAgentData. A site that reads the second or third and finds nothing (WebKit has neither)
 * sees "not Chrome", whatever the string says. Everything here comes from one table, the Chrome version in webpage.h
 * (WEBPAGE_CHROME_MAJOR, WEBPAGE_CHROME_FULL) and the machine, so the string, the headers and the script never
 * disagree. Other identities get none of it (Firefox sends no hints; Edge's and LordOS's are left as they were).
 *
 *   - the brand list, in Chromium's own order and with its "GREASE" brand, both worked out from the major version
 *     exactly as Chromium does (GetGreasedUserAgentBrandVersion): for 158, "Chromium" 158, "Not.A/Brand" 24,
 *     "Google Chrome" 158;
 *   - the low-entropy hints, on every request to a secure origin: Sec-CH-UA, Sec-CH-UA-Mobile ?0,
 *     Sec-CH-UA-Platform "Linux";
 *   - the high-entropy ones only after the site asked with Accept-CH, and only on that site's own requests:
 *     Sec-CH-UA-Full-Version-List, Sec-CH-UA-Full-Version, Sec-CH-UA-Platform-Version (""), Sec-CH-UA-Arch ("x86",
 *     "arm" on aarch64), Sec-CH-UA-Bitness ("64"), Sec-CH-UA-Model (""), Sec-CH-UA-WoW64 (?0),
 *     Sec-CH-UA-Form-Factors ("Desktop"), the values Chrome 158 on Linux sends (captured 2026-11-25);
 *   - navigator.userAgentData (webua.js, in the page's world with no privileges): brands, mobile, platform,
 *     getHighEntropyValues() and toJSON(), with the same values; navigator.vendor "Google Inc.".
 *
 * Nothing here comes from the page: the hints a site asked for are a closed set of bits (WEBUA_HINT_*). */
#ifndef LORD_WEB_WEBUA_H
#define LORD_WEB_WEBUA_H

#include <stddef.h>

/* the hints, as bits of a site's Accept-CH */
enum {
    WEBUA_HINT_FULL_VERSION_LIST = 1u << 0,
    WEBUA_HINT_FULL_VERSION      = 1u << 1,
    WEBUA_HINT_PLATFORM_VERSION  = 1u << 2,
    WEBUA_HINT_ARCH              = 1u << 3,
    WEBUA_HINT_BITNESS           = 1u << 4,
    WEBUA_HINT_MODEL             = 1u << 5,
    WEBUA_HINT_WOW64             = 1u << 6,
    WEBUA_HINT_FORM_FACTORS      = 1u << 7,
    WEBUA_HINT_ALL               = (1u << 8) - 1,
};
#define WEBUA_HINT_N 8
/* Sec-CH-UA, -Mobile and -Platform: always sent, never asked for (they are in Accept-CH lists too; ignored there) */
#define WEBUA_LOW_N 3

typedef struct { char brand[24]; char version[24]; } WebUaBrand;

/* The three brands in Chromium's order for this major version; full: versions "158.0.8127.36" and "24.0.0.0" (the
 * full version list) instead of "158" and "24". 0, or -1 for a bad major. */
int webua_brands(int major, const char *full_version, int full, WebUaBrand out[3]);
/* '"Chromium";v="158", "Not.A/Brand";v="24", "Google Chrome";v="158"' (full: the full version list's). Bytes, 0 when
 * out is too small. */
size_t webua_brand_header(int full, char *out, size_t n);
/* "x86" on x86_64 and i?86, "arm" on aarch64 and arm*, else "" (as Chrome) */
const char *webua_arch(const char *machine);
/* "64" or "32" (from the machine), "" when unknown */
const char *webua_bitness(const char *machine);

/* The bits a site's Accept-CH value asks for: a comma-separated list of names, case-insensitive, with spaces around
 * them; an unknown name is ignored. At most 4096 bytes are read. */
unsigned webua_accept_ch(const char *value);
/* the header of hint i (0..WEBUA_LOW_N + WEBUA_HINT_N - 1: the low ones first, then the bits in order) for a request
 * that may carry the high-entropy bits in mask: its name in *name and its value in out, 0 when that hint isn't sent
 * (a high one not in mask), -1 for a bad i or a short out. */
int webua_header(int i, unsigned mask, const char *machine, const char **name, char *out, size_t n);
/* "sec-ch-ua-arch" and the like for one bit, NULL for anything else */
const char *webua_hint_name(unsigned bit);

/* The user script (webua.js, a function expression) called with its data, all of it from the table above:
 * malloc'd, NULL on failure. */
char *webua_script(const char *js, const char *machine);

/* A site's address → its origin, "https://host[:port]" (lowercase; the default port left out) for https, or for
 * http on localhost (a secure context): 0, else -1 (no hints for it). */
int webua_origin(const char *uri, char *out, size_t n);

/* The hints each site asked for, remembered as Chrome does (the profile's state.json, "clientHints"): at most
 * WEBUA_SITES origins, the oldest dropped. */
#define WEBUA_SITES 16
typedef struct { char origin[256]; unsigned mask; } WebUaSite;
typedef struct { WebUaSite site[WEBUA_SITES]; int n; } WebUaSites;
unsigned webua_sites_get(const WebUaSites *s, const char *origin);
/* 1 when it changed. A mask of 0 forgets the origin (a site that sends an empty Accept-CH asks for nothing more). */
int      webua_sites_set(WebUaSites *s, const char *origin, unsigned mask);

#endif
