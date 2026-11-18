/* streamurl.c — see streamurl.h. */
#include "streamurl.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int scheme_in(const char *url, const char *const *list) {
    if (!url) return 0;
    for (int i = 0; list[i]; i++) {
        size_t n = strlen(list[i]);
        if (!strncmp(url, list[i], n) && !strncmp(url + n, "://", 3)) return 1;
    }
    return 0;
}

static int text_ok(const char *url) {
    size_t n = strlen(url);
    if (n < 8 || n > STREAM_URL_MAX) return 0;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p <= 0x20 || *p >= 0x7f || *p == '"' || *p == '\'' || *p == '\\' || *p == '|' || *p == '`') return 0;
    return 1;
}

int stream_url_ok(const char *url) {
    static const char *const s[] = STREAM_SCHEMES;
    if (!url || !text_ok(url) || !scheme_in(url, s)) return 0;
    const char *host = strstr(url, "://") + 3;
    return host[0] && host[0] != '/';               /* a host (or @multicast) comes first */
}

int stream_m3u_url_ok(const char *url) {
    static const char *const s[] = { "http", "https", NULL };
    return stream_url_ok(url) && scheme_in(url, s);
}

/* copies src[0..n) as printable UTF-8 text without control characters, cut on a whole character */
static void clean_copy(char *dst, size_t cap, const char *src, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x20 || c == 0x7f) continue;
        if (c >= 0x80) {                           /* a whole UTF-8 sequence or nothing */
            size_t k = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 0;
            if (!k || i + k > n || o + k >= cap) break;
            int ok = 1;
            for (size_t j = 1; j < k; j++) if (((unsigned char)src[i + j] & 0xc0) != 0x80) ok = 0;
            if (!ok) continue;
            memcpy(dst + o, src + i, k);
            o += k;
            i += k - 1;
            continue;
        }
        dst[o++] = (char)c;
    }
    while (o && dst[o - 1] == ' ') o--;
    dst[o] = 0;
}

/* the value of attr="…" in an #EXTINF line */
static void attr_of(const char *line, size_t n, const char *attr, char *out, size_t cap) {
    out[0] = 0;
    size_t al = strlen(attr);
    for (size_t i = 0; i + al + 2 <= n; i++) {
        if (memcmp(line + i, attr, al) || line[i + al] != '=' || line[i + al + 1] != '"') continue;
        size_t s = i + al + 2, e = s;
        while (e < n && line[e] != '"') e++;
        clean_copy(out, cap, line + s, e - s);
        return;
    }
}

int stream_m3u_parse(const char *text, size_t len, int max, void (*fn)(const StreamEntry *e, void *ud), void *ud) {
    if (!text || max <= 0) return 0;
    StreamEntry e;
    memset(&e, 0, sizeof e);
    int have_inf = 0, count = 0;
    size_t i = 0;
    if (len >= 3 && (unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf) i = 3;   /* BOM */
    while (i < len && count < max) {
        size_t s = i;
        while (i < len && text[i] != '\n' && text[i] != '\r') i++;
        size_t n = i - s;
        while (i < len && (text[i] == '\n' || text[i] == '\r')) i++;
        const char *l = text + s;
        while (n && (*l == ' ' || *l == '\t')) { l++; n--; }
        while (n && (l[n - 1] == ' ' || l[n - 1] == '\t')) n--;
        if (!n) continue;
        if (n > 8 && !memcmp(l, "#EXTINF:", 8)) {
            memset(&e, 0, sizeof e);
            /* the name: after the last comma outside quotes */
            size_t comma = 0;
            int q = 0;
            for (size_t k = 8; k < n; k++) { if (l[k] == '"') q = !q; else if (l[k] == ',' && !q) comma = k; }
            size_t ns = comma + 1;
            while (comma && ns < n && l[ns] == ' ') ns++;
            if (comma) clean_copy(e.name, sizeof e.name, l + ns, n - ns);
            attr_of(l, n, "group-title", e.group, sizeof e.group);
            char logo[512];
            attr_of(l, n, "tvg-logo", logo, sizeof logo);
            if (stream_m3u_url_ok(logo)) snprintf(e.logo, sizeof e.logo, "%s", logo);
            have_inf = 1;
            continue;
        }
        if (l[0] == '#') continue;
        if (n > STREAM_URL_MAX) { have_inf = 0; continue; }
        memcpy(e.url, l, n);
        e.url[n] = 0;
        if (stream_url_ok(e.url)) {
            if (!have_inf || !e.name[0]) clean_copy(e.name, sizeof e.name, e.url, strlen(e.url) > STREAM_NAME_MAX ? STREAM_NAME_MAX : strlen(e.url));
            fn(&e, ud);
            count++;
        }
        memset(&e, 0, sizeof e);
        have_inf = 0;
    }
    return count;
}
