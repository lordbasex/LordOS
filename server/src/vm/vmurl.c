/* vmurl.c — vm-image-get-user's checks (vmurl.h). */
#include "vmurl.h"
#include "vmcatalog.h"
#include "../pkg/pubhost.h"

#include <stdio.h>
#include <string.h>

static int hexd(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

/* RFC 3986 pchar without '%' (checked apart): unreserved, sub-delims, ':' and '@' */
static int pchar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("-._~!$&'()*+,;=:@", c));
}

int vmurl_check(const char *url, char *host, size_t hn, char *file, size_t fn) {
    if (!url || hn < 254 || fn < 128) return -1;
    size_t n = strlen(url);
    if (n < 12 || n > VMURL_MAX || strncmp(url, "https://", 8)) return -1;
    for (size_t i = 0; i < n; i++) if ((unsigned char)url[i] <= 0x20 || (unsigned char)url[i] >= 0x7f) return -1;
    const char *a = url + 8, *slash = strchr(a, '/');
    if (!slash) return -1;
    size_t al = (size_t)(slash - a);
    if (!al || memchr(a, '@', al) || memchr(a, '?', al) || memchr(a, '#', al)) return -1;
    const char *colon = memchr(a, ':', al);
    size_t hl = colon ? (size_t)(colon - a) : al;
    if (colon && (al - hl != 4 || memcmp(colon, ":443", 4))) return -1;
    if (!hl || hl > 253) return -1;
    char h[254];
    memcpy(h, a, hl);
    h[hl] = 0;
    for (size_t i = 0; i < hl; i++) if (h[i] >= 'A' && h[i] <= 'Z') return -1;   /* the app writes it in lower case */
    if (!pubhost_name_ok(h)) return -1;
    /* the path: segments, then an optional query */
    const char *p = slash, *q = strchr(slash, '?');
    const char *pend = q ? q : url + n;
    const char *seg = p + 1, *last = seg;
    for (const char *s = p + 1;; s++) {
        if (s == pend || *s == '/') {
            size_t sl = (size_t)(s - seg);
            if ((sl == 1 && seg[0] == '.') || (sl == 2 && seg[0] == '.' && seg[1] == '.')) return -1;
            last = seg;
            if (s == pend) { if (!sl) return -1; break; }   /* the last segment names a file */
            seg = s + 1;
            continue;
        }
        if (*s == '%') {
            if (s + 2 >= pend || !hexd(s[1]) || !hexd(s[2]) || (s[1] == '0' && s[2] == '0')) return -1;
            s += 2;
            continue;
        }
        if (!pchar(*s)) return -1;
    }
    /* a segment of dots written with %2e: refused like "." and ".." */
    for (const char *s = p + 1; s < pend;) {
        const char *e = s;
        while (e < pend && *e != '/') e++;
        int dots = 1, any = 0;
        for (const char *t = s; t < e;) {
            if (*t == '.') { t++; any++; continue; }
            if (e - t >= 3 && t[0] == '%' && t[1] == '2' && (t[2] == 'e' || t[2] == 'E')) { t += 3; any++; continue; }
            dots = 0;
            break;
        }
        if (dots && any && any <= 2) return -1;
        s = e < pend ? e + 1 : e;
    }
    if (q) {
        for (const char *s = q + 1; *s; s++) {
            if (*s == '%') {
                if (!hexd(s[1]) || !hexd(s[2]) || (s[1] == '0' && s[2] == '0')) return -1;
                s += 2;
                continue;
            }
            if (!pchar(*s) && *s != '/' && *s != '?') return -1;
        }
    }
    snprintf(host, hn, "%s", h);
    size_t fl = (size_t)(pend - last);
    char f[128];
    if (fl < sizeof f) { memcpy(f, last, fl); f[fl] = 0; }
    else f[0] = 0;
    snprintf(file, fn, "%s", f[0] && vmcat_name_ok(f) ? f : "image.iso");
    return 0;
}

void vmurl_redact(const char *url, char *out, size_t n) {
    if (!n) return;
    out[0] = 0;
    if (!url) return;
    const char *rest = url;
    char scheme[16] = "";
    const char *ss = strstr(url, "://");
    if (ss && ss - url < (long)sizeof scheme - 1) {
        memcpy(scheme, url, (size_t)(ss - url));
        scheme[ss - url] = 0;
        rest = ss + 3;
    }
    /* a user name or password in the authority: dropped */
    const char *slash = strchr(rest, '/'), *at = strchr(rest, '@');
    if (at && (!slash || at < slash)) rest = at + 1;
    size_t keep = strcspn(rest, "?#");
    int cut = rest[keep] != 0;
    char body[1100];
    size_t bl = 0;
    for (size_t i = 0; i < keep && bl + 1 < sizeof body; i++) {
        unsigned char c = (unsigned char)rest[i];
        body[bl++] = c < 0x20 || c == 0x7f ? '?' : (char)c;
    }
    body[bl] = 0;
    snprintf(out, n, "%s%s%s%s", scheme, scheme[0] ? "://" : "", body, cut ? "?…" : "");
}

int vmurl_size(const char *s, uint64_t *out) {
    *out = 0;
    if (!s || !*s) return -1;
    if (!strcmp(s, "-")) return 0;
    if (s[0] == '0' || strlen(s) > 12) return -1;
    uint64_t v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (uint64_t)(*p - '0');
    }
    if (!v || v > VMURL_IMAGE_MAX) return -1;
    *out = v;
    return 0;
}

uint64_t vmurl_room(uint64_t size, uint64_t part, uint64_t free_bytes, uint64_t *need) {
    uint64_t dummy;
    if (!need) need = &dummy;
    if (size > VMURL_IMAGE_MAX) { *need = size; return 0; }
    if (size) {
        if (part > size) part = 0;                       /* a part larger than the file starts again */
        *need = size - part + VMURL_RESERVE;
        return free_bytes >= *need ? size : 0;
    }
    /* size not known: as much as fits, never more than an image may be */
    *need = VMURL_RESERVE + (1u << 20);
    if (free_bytes < *need) return 0;
    uint64_t can = part + free_bytes - VMURL_RESERVE;
    return can < VMURL_IMAGE_MAX ? can : VMURL_IMAGE_MAX;
}
