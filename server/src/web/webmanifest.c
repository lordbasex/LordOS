/* webmanifest.c — see webmanifest.h. */
#include "webmanifest.h"
#include "webreg.h"
#include "weburl.h"
#include "../sec/json.h"

#include <stdio.h>
#include <string.h>

typedef struct { char *p; size_t n, o; int over; } Out;

static void put(Out *w, const char *s) {
    size_t l = strlen(s);
    if (w->over || w->o + l + 1 > w->n) { w->over = 1; return; }
    memcpy(w->p + w->o, s, l + 1);
    w->o += l;
}
static void putq(Out *w, const char *s) {
    char q[WEBURL_MAX * 6 + 8];
    size_t k = json_quote(q, sizeof q, s ? s : "");
    if (k < 2) { w->over = 1; return; }
    put(w, q);
}

static int hexd(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

int webmanifest_color(const char *s, char out[8]) {
    if (!s) return -1;
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strcspn(s, " \t");
    if (s[n] && s[n + strspn(s + n, " \t")]) return -1;
    if (s[0] != '#' || (n != 4 && n != 7)) return -1;
    for (size_t i = 1; i < n; i++) if (!hexd((unsigned char)s[i])) return -1;
    out[0] = '#';
    for (int i = 0; i < 6; i++) out[1 + i] = (char)lower((unsigned char)(n == 7 ? s[1 + i] : s[1 + i / 2]));
    out[7] = 0;
    return 0;
}

/* a name as the Dock shows it: control characters and invalid UTF-8 dropped, spaces folded and trimmed, at most
 * WEBMANIFEST_NAME_MAX bytes cut on a character boundary */
static void clean_name(const char *s, char *out, size_t n) {
    size_t o = 0;
    int space = 0;
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    while (*p && o + 1 < n && o < WEBMANIFEST_NAME_MAX) {
        unsigned c = *p;
        int k = c < 0x80 ? 0 : c >= 0xc2 && c < 0xe0 ? 1 : c >= 0xe0 && c < 0xf0 ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : -1;
        int ok = k >= 0;
        for (int j = 1; ok && j <= k; j++) if ((p[j] & 0xc0) != 0x80) ok = 0;
        if (!ok) { p++; continue; }
        if (k == 0 && (c < 0x20 || c == 0x7f || c == ' ')) { space = o > 0; p++; continue; }
        if (k == 1 && c == 0xc2 && p[1] < 0xa0) { p += 2; continue; }       /* C1 controls */
        if (space) {
            if (o + 2 >= n || o + 1 > WEBMANIFEST_NAME_MAX) break;
            out[o++] = ' ';
            space = 0;
        }
        if (o + (size_t)k + 1 >= n || o + (size_t)k + 1 > WEBMANIFEST_NAME_MAX) break;
        memcpy(out + o, p, (size_t)k + 1);
        o += (size_t)k + 1;
        p += k + 1;
    }
    out[o] = 0;
}

/* sizes and type of an icon: printable ASCII, at most 63 bytes, else "" */
static void clean_token(const char *s, char out[64]) {
    out[0] = 0;
    if (!s) return;
    size_t l = strlen(s);
    if (l > 63) return;
    for (size_t i = 0; i < l; i++) if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) return;
    memcpy(out, s, l + 1);
}

static int put_icons(Out *w, const JVal *list) {
    int count = 0;
    for (const JVal *i = list && list->type == J_ARR ? list->kid : NULL; i && count < WEBMANIFEST_ICONS; i = i->next) {
        const char *src = json_str(i, "src");
        if (!src || !weburl_app_ok(src)) continue;
        char sizes[64], type[64];
        clean_token(json_str(i, "sizes"), sizes);
        clean_token(json_str(i, "type"), type);
        put(w, count ? ",{\"src\":" : "{\"src\":");
        putq(w, src);
        put(w, ",\"sizes\":");
        putq(w, sizes);
        put(w, ",\"type\":");
        putq(w, type);
        put(w, "}");
        count++;
    }
    return count;
}

int webmanifest_error(const char *code, char *out, size_t n) {
    Out w = { out, n, 0, 0 };
    if (!out || !n) return -1;
    out[0] = 0;
    put(&w, "{\"ok\":false,\"error\":");
    putq(&w, code);
    put(&w, "}");
    return w.over ? -1 : 0;
}

int webmanifest_answer(const char *doc_url, const char *page, size_t len, char *out, size_t n) {
    if (!out || !n) return -1;
    out[0] = 0;
    WebUrl doc;
    if (!doc_url || weburl_parse(doc_url, &doc) || !(doc.https || weburl_is_local(&doc))) return -1;
    if (!page || len > WEBMANIFEST_PAGE_MAX) return -1;
    JDoc d = { 0 };
    if (json_parse(page, len, &d, NULL, 0) || !d.root || d.root->type != J_OBJ) { json_free(&d); return -1; }
    const JVal *root = d.root;
    const JVal *m = json_get(root, "manifest");
    if (m && m->type != J_OBJ) m = NULL;

    char origin[WEBURL_ORIGIN_MAX + 1];
    weburl_origin(doc_url, origin, sizeof origin);
    /* the start address: the manifest's on the same origin, else the page itself (without its fragment) */
    char start[WEBURL_MAX + 1];
    const char *ms = m ? json_str(m, "start_url") : NULL;
    if (ms && weburl_app_ok(ms) && weburl_same_origin(ms, doc_url) && strlen(ms) <= WEBURL_MAX) snprintf(start, sizeof start, "%s", ms);
    else snprintf(start, sizeof start, "%s", doc_url);
    start[strcspn(start, "#")] = 0;
    /* the scope: the manifest's when it holds the start address, else the start address's folder */
    char scope[WEBURL_MAX + 1];
    const char *sc = m ? json_str(m, "scope") : NULL;
    if (sc && weburl_app_ok(sc) && !strchr(sc, '?') && !strchr(sc, '#') && strlen(sc) <= WEBURL_MAX && weburl_in_scope(start, sc)) snprintf(scope, sizeof scope, "%s", sc);
    else if (weburl_dir(start, scope, sizeof scope)) { json_free(&d); return -1; }

    char name[WEBMANIFEST_NAME_MAX + 1] = "";
    const char *names[] = { m ? json_str(m, "name") : NULL, m ? json_str(m, "short_name") : NULL, json_str(root, "title") };
    for (int i = 0; i < 3 && !name[0]; i++) clean_name(names[i], name, sizeof name);
    if (!name[0]) clean_name(doc.host, name, sizeof name);

    char color[8];
    int has_color = !webmanifest_color(m ? json_str(m, "theme_color") : NULL, color) || !webmanifest_color(json_str(root, "metaThemeColor"), color);

    Out w = { out, n, 0, 0 };
    put(&w, "{\"ok\":true,\"name\":");
    putq(&w, name);
    put(&w, ",\"startUrl\":");
    putq(&w, start);
    put(&w, ",\"scope\":");
    putq(&w, scope);
    put(&w, ",\"origin\":");
    putq(&w, origin);
    put(&w, ",\"themeColor\":");
    if (has_color) putq(&w, color); else put(&w, "null");
    put(&w, ",\"icons\":[");
    if (!put_icons(&w, m ? json_get(m, "icons") : NULL)) put_icons(&w, json_get(root, "linkIcons"));
    put(&w, "],\"manifest\":");
    put(&w, m ? "true}" : "false}");
    json_free(&d);
    if (w.over) { out[0] = 0; return -1; }
    return 0;
}

const char *webicon_type(int kind) { return kind == WEBICON_PNG ? "image/png" : kind == WEBICON_SVG ? "image/svg+xml" : NULL; }

/* the registry's own check (webreg.c), so lord-web never sends an icon lord-server would refuse */
int webicon_kind(const unsigned char *b, size_t n) {
    int k = webreg_icon_kind(b, n);
    return k == 1 ? WEBICON_PNG : k == 2 ? WEBICON_SVG : WEBICON_NONE;
}
