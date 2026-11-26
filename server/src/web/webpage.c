/* webpage.c — the page's unread count, the user agent, the notification sound, the state file (see webpage.h). */
#include "webpage.h"
#include "webreg.h"
#include "../sec/json.h"

#include <stdio.h>
#include <string.h>

int webpage_title_badge(const char *t) {
    if (!t || t[0] != '(' || t[1] < '1' || t[1] > '9') return 0;
    int n = 0, i = 1;
    for (; t[i] >= '0' && t[i] <= '9'; i++) {
        if (i > 4) return 0;
        n = n * 10 + (t[i] - '0');
    }
    if (t[i] == '+') i++;
    if (t[i] != ')' || (t[i + 1] && t[i + 1] != ' ')) return 0;
    return n <= WEBPAGE_BADGE_MAX ? n : 0;
}

static int all_of(const char *s, const char *extra) {
    if (!s || !s[0] || strlen(s) > 32) return 0;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr(extra, c))) return 0;
    }
    return 1;
}

int webpage_user_agent(int kind, const char *machine, const char *version, char *out, size_t n) {
    char plat[64], tail[64] = "";
    int k;
    /* LordOS's own token on its WebKit string: " LordOS/<version>", left out for a strange version */
    if (all_of(version, ".+~-")) snprintf(tail, sizeof tail, " LordOS/%s", version);
    if (kind == WEBREG_UA_CHROME) {
        /* Chrome's reduced user agent exactly as Chrome sends it, nothing added: the same platform on every Linux */
        k = snprintf(out, n, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/%d.0.0.0 Safari/537.36",
                     WEBPAGE_CHROME_MAJOR);
    } else if (kind == WEBREG_UA_EDGE) {
        /* desktop Edge on Linux: Chrome's reduced string with Edge's own token */
        k = snprintf(out, n, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/%d.0.0.0 Safari/537.36 Edg/%d.0.0.0",
                     WEBPAGE_EDGE_MAJOR, WEBPAGE_EDGE_MAJOR);
    } else if (kind == WEBREG_UA_FIREFOX) {
        /* Firefox names the processor it runs on */
        k = snprintf(out, n, "Mozilla/5.0 (X11; Linux %s; rv:%d.0) Gecko/20100101 Firefox/%d.0", all_of(machine, "_") ? machine : "x86_64",
                     WEBPAGE_FIREFOX_MAJOR, WEBPAGE_FIREFOX_MAJOR);
    } else if (kind == WEBREG_UA_LORDOS) {
        if (all_of(machine, "_")) snprintf(plat, sizeof plat, "X11; Linux %s", machine);
        else snprintf(plat, sizeof plat, "X11; Linux");
        k = snprintf(out, n, "Mozilla/5.0 (%s) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15%s", plat, tail);
    } else return -1;
    return k < 0 || (size_t)k >= n ? -1 : 0;
}

int webpage_sound_suppress(int mode, int learned, int silent, double last_audio_ms, double now_ms) {
    if (mode == WEBREG_SOUND_SYSTEM) return 0;
    if (mode == WEBREG_SOUND_APP || mode == WEBREG_SOUND_NONE || silent) return 1;
    return learned || webpage_sound_learns(mode, now_ms, last_audio_ms);
}

int webpage_sound_learns(int mode, double note_ms, double audio_ms) {
    if (mode != WEBREG_SOUND_AUTO || audio_ms < 0 || note_ms < 0) return 0;
    double d = audio_ms - note_ms;
    return d >= -WEBPAGE_SOUND_WINDOW_MS && d <= WEBPAGE_SOUND_WINDOW_MS;
}

void webpage_tag_hash(const char *tag, unsigned long long salt, char out[9]) {
    /* FNV-1a, 64 bits, over the salt's 8 bytes and then the tag; the low 32 bits are written */
    unsigned long long h = 1469598103934665603ULL;
    for (int i = 0; i < 8; i++) { h ^= (salt >> (8 * i)) & 0xff; h *= 1099511628211ULL; }
    for (const unsigned char *p = (const unsigned char *)(tag ? tag : ""); *p; p++) { h ^= *p; h *= 1099511628211ULL; }
    for (int i = 0; i < 2; i++) { h ^= h >> 29; h *= 0xbf58476d1ce4e5b9ULL; }
    snprintf(out, 9, "%08x", (unsigned)(h & 0xffffffffu));
}

static const char *tri(int v) { return v < 0 ? "unknown" : v ? "yes" : "no"; }

size_t webpage_notify_line(const char *tag, unsigned long long salt, int silent, int renotify, int replaced, char *out, size_t n) {
    if (!out || !n) return 0;
    char h[9] = "none";
    if (tag && tag[0]) webpage_tag_hash(tag, salt, h);
    int k = snprintf(out, n, "page tag %s silent %s renotify %s %s", h, tri(silent), tri(renotify), replaced ? "replaced" : "shown");
    if (k < 0 || (size_t)k >= n) { out[0] = 0; return 0; }
    return (size_t)k;
}

void webpage_state_parse(const char *json, size_t len, WebPageState *s) {
    memset(s, 0, sizeof *s);
    JDoc d;
    if (!json || len > WEBPAGE_STATE_MAX || json_parse(json, len, &d, NULL, 0)) return;
    if (d.root && d.root->type == J_OBJ) {
        s->own_sound = json_bool(d.root, "ownSound", 0) != 0;
        const JVal *ch = json_get(d.root, "clientHints");
        for (const JVal *o = ch && ch->type == J_OBJ ? ch->kid : NULL; o; o = o->next) {
            char origin[256];
            if (o->type != J_ARR || webua_origin(o->key, origin, sizeof origin) || strcmp(origin, o->key)) continue;
            unsigned mask = 0;
            for (const JVal *h = o->kid; h; h = h->next)
                if (h->type == J_STR) for (unsigned b = 1; b & WEBUA_HINT_ALL; b <<= 1)
                    if (webua_hint_name(b) && !strcmp(h->str, webua_hint_name(b))) mask |= b;
            webua_sites_set(&s->hints, origin, mask);
        }
    }
    json_free(&d);
}

size_t webpage_state_write(const WebPageState *s, char *out, size_t n) {
    size_t at = 0;
    int k = snprintf(out, n, "{ \"ownSound\": %s", s->own_sound ? "true" : "false");
    if (k < 0 || (size_t)k >= n) return 0;
    at = (size_t)k;
    if (s->hints.n) {
        k = snprintf(out + at, n - at, ", \"clientHints\": {");
        if (k < 0 || (size_t)k >= n - at) return 0;
        at += (size_t)k;
        for (int i = 0; i < s->hints.n; i++) {
            char q[600];
            /* an origin is only [a-z0-9.:[]_-] after webua_origin, so quoting changes nothing; quoted all the same */
            if (!json_quote(q, sizeof q, s->hints.site[i].origin)) return 0;
            k = snprintf(out + at, n - at, "%s %s: [", i ? "," : "", q);
            if (k < 0 || (size_t)k >= n - at) return 0;
            at += (size_t)k;
            int first = 1;
            for (unsigned b = 1; b & WEBUA_HINT_ALL; b <<= 1) {
                if (!(s->hints.site[i].mask & b)) continue;
                k = snprintf(out + at, n - at, "%s\"%s\"", first ? "" : ", ", webua_hint_name(b));
                if (k < 0 || (size_t)k >= n - at) return 0;
                at += (size_t)k;
                first = 0;
            }
            k = snprintf(out + at, n - at, "]");
            if (k < 0 || (size_t)k >= n - at) return 0;
            at += (size_t)k;
        }
        k = snprintf(out + at, n - at, " }");
        if (k < 0 || (size_t)k >= n - at) return 0;
        at += (size_t)k;
    }
    k = snprintf(out + at, n - at, " }\n");
    return k < 0 || (size_t)k >= n - at ? 0 : at + (size_t)k;
}

int webpage_view_visible(int shown, int configured, int suspended) { return shown && configured && !suspended; }

int webpage_toplevel_suspended(const unsigned *states, size_t n) {
    for (size_t i = 0; states && i < n; i++) if (states[i] == 9) return 1;
    return 0;
}

WebScopeLimits webpage_scope_limits(long ram_mb, int ncpu) {
    WebScopeLimits l;
    if (ram_mb <= 0) ram_mb = 4096;
    long max = ram_mb / 4 * 3;
    if (max < 1024) max = 1024;
    if (max > 16384) max = 16384;
    l.mem_max_mb = max;
    l.mem_high_mb = max - max / 8;
    int cpu = (ncpu > 0 ? ncpu : 1) * 50;
    if (cpu < 200) cpu = 200;
    if (cpu > 6400) cpu = 6400;
    l.cpu_percent = cpu;
    return l;
}
