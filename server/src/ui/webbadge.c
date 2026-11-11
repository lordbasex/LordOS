/* webbadge.c — see webbadge.h. */
#include "webbadge.h"

#include <stdio.h>
#include <string.h>

static int id_ok(const char *s) {
    if (!s || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9'))) return 0;
    size_t n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
    }
    return n <= WEBBADGE_ID_MAX;
}

uint32_t webbadge_clamp(uint32_t count) { return count > WEBBADGE_COUNT_MAX ? WEBBADGE_COUNT_MAX : count; }

int webbadge_call(WebBadges *t, const char *sender, const char *app, const char *profile, uint32_t count, int *slot) {
    *slot = -1;
    if (!sender || !sender[0] || strlen(sender) > WEBBADGE_NAME_MAX || !id_ok(app) || !id_ok(profile)) return WB_REFUSE;
    int free_slot = -1;
    for (int i = 0; i < WEBBADGE_MAX; i++) {
        WebBadge *b = &t->b[i];
        if (b->state == WB_NEW) { if (free_slot < 0) free_slot = i; continue; }
        if (strcmp(b->sender, sender)) continue;
        /* one sender is one lord-web: one app and one profile, for as long as it is on the bus */
        if (b->state == WB_REFUSED || strcmp(b->app, app) || strcmp(b->profile, profile)) return WB_REFUSE;
        b->count = webbadge_clamp(count);
        *slot = i;
        return b->state == WB_OK ? WB_EMIT : WB_WAIT;
    }
    /* full: a sender refused before gives its place (it is asked about again if it calls again) */
    for (int i = 0; i < WEBBADGE_MAX && free_slot < 0; i++) if (t->b[i].state == WB_REFUSED) free_slot = i;
    if (free_slot < 0) return WB_REFUSE;
    WebBadge *b = &t->b[free_slot];
    memset(b, 0, sizeof *b);
    b->state = WB_ASKING;
    snprintf(b->sender, sizeof b->sender, "%s", sender);
    snprintf(b->app, sizeof b->app, "%s", app);
    snprintf(b->profile, sizeof b->profile, "%s", profile);
    b->count = webbadge_clamp(count);
    *slot = free_slot;
    return WB_ASK_PID;
}

void webbadge_asked(WebBadges *t, int slot, uint32_t serial) {
    if (slot < 0 || slot >= WEBBADGE_MAX || t->b[slot].state != WB_ASKING) return;
    if (!serial) memset(&t->b[slot], 0, sizeof t->b[slot]);
    else t->b[slot].serial = serial;
}

int webbadge_answer(WebBadges *t, uint32_t serial, int owner_ok, int *show) {
    *show = 0;
    if (!serial) return -1;
    for (int i = 0; i < WEBBADGE_MAX; i++) {
        WebBadge *b = &t->b[i];
        if (b->state != WB_ASKING || b->serial != serial) continue;
        b->serial = 0;
        b->state = owner_ok ? WB_OK : WB_REFUSED;
        *show = owner_ok;
        return i;
    }
    return -1;
}

int webbadge_gone(WebBadges *t, const char *sender, char *app, char *profile) {
    int shown = 0;
    for (int i = 0; sender && i < WEBBADGE_MAX; i++) {
        WebBadge *b = &t->b[i];
        if (b->state == WB_NEW || strcmp(b->sender, sender)) continue;
        if (b->state == WB_OK) {
            shown = 1;
            snprintf(app, WEBBADGE_ID_MAX + 1, "%s", b->app);
            snprintf(profile, WEBBADGE_ID_MAX + 1, "%s", b->profile);
        }
        memset(b, 0, sizeof *b);
    }
    return shown;
}
