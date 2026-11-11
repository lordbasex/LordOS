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

int webpage_user_agent(const char *machine, const char *version, char *out, size_t n) {
    char plat[64], tail[64] = "";
    if (all_of(machine, "_")) snprintf(plat, sizeof plat, "X11; Linux %s", machine);
    else snprintf(plat, sizeof plat, "X11; Linux");
    if (all_of(version, ".+~-")) snprintf(tail, sizeof tail, " LordOS/%s", version);
    int k = snprintf(out, n, "Mozilla/5.0 (%s) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15%s", plat, tail);
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

void webpage_state_parse(const char *json, size_t len, WebPageState *s) {
    memset(s, 0, sizeof *s);
    JDoc d;
    if (!json || len > 4096 || json_parse(json, len, &d, NULL, 0)) return;
    if (d.root && d.root->type == J_OBJ) s->own_sound = json_bool(d.root, "ownSound", 0) != 0;
    json_free(&d);
}

size_t webpage_state_write(const WebPageState *s, char *out, size_t n) {
    int k = snprintf(out, n, "{ \"ownSound\": %s }\n", s->own_sound ? "true" : "false");
    return k < 0 || (size_t)k >= n ? 0 : (size_t)k;
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
