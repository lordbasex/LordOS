/* webua.c — see webua.h. */
#include "webua.h"
#include "webpage.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Chromium's GREASE brand (components/embedder_support/user_agent_utils.cc, GetGreasedUserAgentBrandVersion): the
 * seed is the major version; the brand is "Not" <c1> "A" <c2> "Brand" with c1, c2 from this list, its version one of
 * three, and the three brands are ordered by one of the six permutations */
static const char GREASE_CHARS[] = { ' ', '(', ':', '-', '.', '/', ')', ';', '=', '?', '_' };
static const char *const GREASE_VERSIONS[] = { "8", "99", "24" };
static const int ORDERS[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };

int webua_brands(int major, const char *full_version, int full, WebUaBrand out[3]) {
    if (major <= 0 || major > 9999 || !full_version) return -1;
    WebUaBrand grease, chromium, chrome;
    snprintf(grease.brand, sizeof grease.brand, "Not%cA%cBrand", GREASE_CHARS[major % 11], GREASE_CHARS[(major + 1) % 11]);
    snprintf(grease.version, sizeof grease.version, full ? "%s.0.0.0" : "%s", GREASE_VERSIONS[major % 3]);
    snprintf(chromium.brand, sizeof chromium.brand, "Chromium");
    snprintf(chrome.brand, sizeof chrome.brand, "Google Chrome");
    if (full) {
        snprintf(chromium.version, sizeof chromium.version, "%s", full_version);
        snprintf(chrome.version, sizeof chrome.version, "%s", full_version);
    } else {
        snprintf(chromium.version, sizeof chromium.version, "%d", major);
        snprintf(chrome.version, sizeof chrome.version, "%d", major);
    }
    const int *o = ORDERS[major % 6];
    out[o[0]] = grease;
    out[o[1]] = chromium;
    out[o[2]] = chrome;
    return 0;
}

size_t webua_brand_header(int full, char *out, size_t n) {
    WebUaBrand b[3];
    if (!out || !n || webua_brands(WEBPAGE_CHROME_MAJOR, WEBPAGE_CHROME_FULL, full, b)) return 0;
    int k = snprintf(out, n, "\"%s\";v=\"%s\", \"%s\";v=\"%s\", \"%s\";v=\"%s\"", b[0].brand, b[0].version, b[1].brand,
                     b[1].version, b[2].brand, b[2].version);
    return k < 0 || (size_t)k >= n ? 0 : (size_t)k;
}

const char *webua_arch(const char *m) {
    if (!m) return "";
    if (!strcmp(m, "x86_64") || !strcmp(m, "amd64") || (m[0] == 'i' && m[1] && !strcmp(m + 2, "86"))) return "x86";
    if (!strcmp(m, "aarch64") || !strcmp(m, "arm64") || !strncmp(m, "arm", 3)) return "arm";
    return "";
}

const char *webua_bitness(const char *m) {
    if (!m) return "";
    if (!strcmp(m, "x86_64") || !strcmp(m, "amd64") || !strcmp(m, "aarch64") || !strcmp(m, "arm64")) return "64";
    if ((m[0] == 'i' && m[1] && !strcmp(m + 2, "86")) || !strncmp(m, "arm", 3)) return "32";
    return "";
}

static const char *const HINT_NAMES[WEBUA_HINT_N] = {
    "sec-ch-ua-full-version-list", "sec-ch-ua-full-version", "sec-ch-ua-platform-version", "sec-ch-ua-arch",
    "sec-ch-ua-bitness", "sec-ch-ua-model", "sec-ch-ua-wow64", "sec-ch-ua-form-factors",
};
static const char *const LOW_NAMES[WEBUA_LOW_N] = { "sec-ch-ua", "sec-ch-ua-mobile", "sec-ch-ua-platform" };

const char *webua_hint_name(unsigned bit) {
    for (int i = 0; i < WEBUA_HINT_N; i++) if (bit == 1u << i) return HINT_NAMES[i];
    return NULL;
}

unsigned webua_accept_ch(const char *v) {
    unsigned mask = 0;
    if (!v) return 0;
    size_t len = strnlen(v, 4097);
    if (len > 4096) len = 4096;
    for (size_t i = 0; i < len;) {
        while (i < len && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        size_t s = i;
        while (i < len && v[i] != ',') i++;
        size_t e = i;
        while (e > s && (v[e - 1] == ' ' || v[e - 1] == '\t')) e--;
        for (int h = 0; h < WEBUA_HINT_N; h++)
            if (e - s == strlen(HINT_NAMES[h]) && !strncasecmp(v + s, HINT_NAMES[h], e - s)) mask |= 1u << h;
    }
    return mask;
}

static int put(char *out, size_t n, const char *fmt, const char *a) {
    int k = snprintf(out, n, fmt, a);
    return k < 0 || (size_t)k >= n ? -1 : 1;
}

int webua_header(int i, unsigned mask, const char *machine, const char **name, char *out, size_t n) {
    if (i < 0 || i >= WEBUA_LOW_N + WEBUA_HINT_N || !name || !out || !n) return -1;
    if (i < WEBUA_LOW_N) {
        *name = LOW_NAMES[i];
        if (i == 0) return webua_brand_header(0, out, n) ? 1 : -1;
        return put(out, n, "%s", i == 1 ? "?0" : "\"Linux\"");
    }
    unsigned bit = 1u << (i - WEBUA_LOW_N);
    *name = HINT_NAMES[i - WEBUA_LOW_N];
    if (!(mask & bit)) return 0;
    switch (bit) {
    case WEBUA_HINT_FULL_VERSION_LIST: return webua_brand_header(1, out, n) ? 1 : -1;
    case WEBUA_HINT_FULL_VERSION: return put(out, n, "\"%s\"", WEBPAGE_CHROME_FULL);
    case WEBUA_HINT_PLATFORM_VERSION: return put(out, n, "%s", "\"\"");
    case WEBUA_HINT_ARCH: return put(out, n, "\"%s\"", webua_arch(machine));
    case WEBUA_HINT_BITNESS: return put(out, n, "\"%s\"", webua_bitness(machine));
    case WEBUA_HINT_MODEL: return put(out, n, "%s", "\"\"");
    case WEBUA_HINT_WOW64: return put(out, n, "%s", "?0");
    case WEBUA_HINT_FORM_FACTORS: return put(out, n, "%s", "\"Desktop\"");
    default: return -1;
    }
}

/* the brands as a JSON array: [{"brand":"Chromium","version":"158"},...] */
static int brands_json(int full, char *out, size_t n) {
    WebUaBrand b[3];
    if (webua_brands(WEBPAGE_CHROME_MAJOR, WEBPAGE_CHROME_FULL, full, b)) return -1;
    int k = snprintf(out, n, "[{\"brand\":\"%s\",\"version\":\"%s\"},{\"brand\":\"%s\",\"version\":\"%s\"},{\"brand\":\"%s\",\"version\":\"%s\"}]",
                     b[0].brand, b[0].version, b[1].brand, b[1].version, b[2].brand, b[2].version);
    return k < 0 || (size_t)k >= n ? -1 : 0;
}

char *webua_script(const char *js, const char *machine) {
    char brands[512], full[512];
    if (!js || brands_json(0, brands, sizeof brands) || brands_json(1, full, sizeof full)) return NULL;
    /* every value is from the table: the brands (letters, digits and Chromium's GREASE characters, none of them a
     * quote or a backslash), the version, and the two closed words for the machine */
    const char *fmt = "%s({\"brands\":%s,\"fullVersionList\":%s,\"mobile\":false,\"platform\":\"Linux\",\"platformVersion\":\"\","
                      "\"architecture\":\"%s\",\"bitness\":\"%s\",\"model\":\"\",\"wow64\":false,\"formFactors\":[\"Desktop\"],"
                      "\"uaFullVersion\":\"%s\",\"vendor\":\"Google Inc.\"});\n";
    const char *arch = webua_arch(machine), *bits = webua_bitness(machine);
    int k = snprintf(NULL, 0, fmt, js, brands, full, arch, bits, WEBPAGE_CHROME_FULL);
    if (k < 0) return NULL;
    char *out = malloc((size_t)k + 1);
    if (out) snprintf(out, (size_t)k + 1, fmt, js, brands, full, arch, bits, WEBPAGE_CHROME_FULL);
    return out;
}

int webua_origin(const char *uri, char *out, size_t n) {
    if (!uri || !out || !n) return -1;
    int https = !strncasecmp(uri, "https://", 8);
    if (!https && strncasecmp(uri, "http://", 7)) return -1;
    const char *h = uri + (https ? 8 : 7);
    size_t len = strcspn(h, "/?#");
    if (!len || len > 253 || memchr(h, '@', len)) return -1;
    char host[256];
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)h[i];
        if (!(isalnum(c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']' || c == '_')) return -1;
        host[i] = (char)tolower(c);
    }
    host[len] = 0;
    /* the default port left out */
    size_t hl = len;
    if (https && hl > 4 && !strcmp(host + hl - 4, ":443")) host[hl - 4] = 0;
    if (!https && hl > 3 && !strcmp(host + hl - 3, ":80")) host[hl - 3] = 0;
    if (!https) {
        /* http only on localhost, a secure context */
        char name[256];
        snprintf(name, sizeof name, "%s", host);
        char *colon = name[0] == '[' ? strchr(name, ']') : strchr(name, ':');
        if (colon && name[0] == '[') colon[1] = 0;
        else if (colon) *colon = 0;
        if (strcmp(name, "localhost") && strcmp(name, "127.0.0.1") && strcmp(name, "[::1]")) return -1;
    }
    int k = snprintf(out, n, "%s://%s", https ? "https" : "http", host);
    return k < 0 || (size_t)k >= n ? -1 : 0;
}

unsigned webua_sites_get(const WebUaSites *s, const char *origin) {
    for (int i = 0; s && origin && i < s->n; i++) if (!strcmp(s->site[i].origin, origin)) return s->site[i].mask;
    return 0;
}

int webua_sites_set(WebUaSites *s, const char *origin, unsigned mask) {
    if (!s || !origin || !origin[0] || strlen(origin) >= sizeof s->site[0].origin) return 0;
    mask &= WEBUA_HINT_ALL;
    for (int i = 0; i < s->n; i++) {
        if (strcmp(s->site[i].origin, origin)) continue;
        if (s->site[i].mask == mask) return 0;
        if (!mask) {
            memmove(&s->site[i], &s->site[i + 1], (size_t)(s->n - i - 1) * sizeof s->site[0]);
            s->n--;
        } else s->site[i].mask = mask;
        return 1;
    }
    if (!mask) return 0;
    if (s->n == WEBUA_SITES) {         /* the oldest goes */
        memmove(&s->site[0], &s->site[1], (WEBUA_SITES - 1) * sizeof s->site[0]);
        s->n--;
    }
    snprintf(s->site[s->n].origin, sizeof s->site[0].origin, "%s", origin);
    s->site[s->n].mask = mask;
    s->n++;
    return 1;
}
