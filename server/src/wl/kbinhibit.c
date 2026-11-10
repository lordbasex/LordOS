/* kbinhibit.c — see kbinhibit.h. No Wayland here: wl.c keeps the objects and sends the events. */
#include "kbinhibit.h"
#include "keygrab.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

/* An app_id goes into permisos.json and in front of the person in a prompt: the characters a .desktop id uses. */
static int id_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+' || c == '@' || c == '~';
}
static int id_ok(const char *s, size_t room) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n >= room || s[0] == '.') return 0;
    for (; *s; s++) if (!id_char((unsigned char)*s)) return 0;
    return 1;
}
static int exe_ok(const char *e, size_t room) {
    if (!e || e[0] != '/' || strlen(e) >= room || strstr(e, "..")) return 0;
    for (const unsigned char *p = (const unsigned char *)e; *p; p++) if (!id_char(*p) && *p != '/') return 0;
    return 1;
}

int kbi_app_key(const char *app_id, const char *exe, char *out, size_t n) {
    if (!out || n < KBI_ID_MAX) return 0;
    out[0] = 0;
    if (id_ok(app_id, KBI_ID_MAX - 8)) snprintf(out, n, "desktop:%s", app_id);
    else if (exe_ok(exe, KBI_ID_MAX - 4)) snprintf(out, n, "exe:%s", exe);
    return out[0] != 0;
}

int kbi_key_valid(const char *key) {
    if (!key) return 0;
    if (!strncmp(key, "desktop:", 8)) return id_ok(key + 8, KBI_ID_MAX - 8);
    if (!strncmp(key, "exe:", 4)) return exe_ok(key + 4, KBI_ID_MAX - 4);
    return 0;
}

int kbi_decide(int state, int isolated, int greeter, int has_key) {
    if (isolated || greeter || !has_key || state == 2) return KBI_NEVER;
    return state == 1 ? KBI_GRANT : KBI_PROMPT;
}

static int update(KbiState *k) {
    int want = k->allowed && !k->off;
    if (want == k->active) return KBI_SAME;
    k->active = want;
    return want ? KBI_ACTIVE : KBI_INACTIVE;
}

int kbi_set_allowed(KbiState *k, int allowed) {
    k->allowed = !!allowed;
    if (!k->allowed) k->off = 0;        /* allowed again later: it holds again */
    return update(k);
}

int kbi_toggle(KbiState *k) {
    if (!k->allowed) return KBI_SAME;
    k->off = !k->off;
    return update(k);
}

int kbi_route(int code, char letter, unsigned mods, int active) {
    if (code == KEY_ESC && mods == KG_META) return KBI_KEY_TOGGLE;
    if (!active) return KBI_KEY_LORD;
    /* the screen lock is always reachable: a person leaving a VM in front can still lock */
    if (mods == KG_META && letter == 'l') return KBI_KEY_LORD;
    if (mods == (KG_META | KG_CTRL) && letter == 'q') return KBI_KEY_LORD;
    /* the console switch, as without an inhibitor */
    if ((mods & (KG_CTRL | KG_ALT)) == (KG_CTRL | KG_ALT) && !(mods & KG_META) && code >= KEY_F1 && code <= KEY_F12) return KBI_KEY_LORD;
    if (keygrab_system_key(code)) return KBI_KEY_LORD;
    return KBI_KEY_APP;
}
