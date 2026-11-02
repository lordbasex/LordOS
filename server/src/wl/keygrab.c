/* keygrab.c — the keys the desktop keeps from a focused Wayland app.
 *
 * A focused app gets every key before the theme (wl_key in wl.c). The few combinations a theme needs
 * over an app live here, in fixed tables written at build time: keyboard keys only (no pointer, no
 * text), each one a modifier set plus a letter or an evdev code. The shell chooses a table by name
 * with lord.wayland.grabKeys; nothing can add an entry. lord.wayland does not exist in an isolated
 * app's process (js_broker_register), and that process has no compositor to change anyway.
 *
 * Letters are compared by what the layout produces, as sdk/shortcuts.js does, so ❖Z is the key
 * printed Z on any Latin layout; everything else by its position. */
#include "keygrab.h"
#include <linux/input-event-codes.h>
#include <string.h>

typedef struct { unsigned char mods; char letter; unsigned short code; unsigned char how; } Grab;

/* The grave key is ❖`; ❖Tab and ❖Esc, like ❖`, are already kept from isolated apps by
 * system_meta_key in wl.c: they are listed only when they need more than that. */
static const Grab flow[] = {
    { KG_META, 0, KEY_TAB, KG_HOLD },
    { KG_META | KG_SHIFT, 0, KEY_TAB, KG_HOLD },
    { KG_META, 'm', 0, KG_TAKE },
    { KG_META, 'h', 0, KG_TAKE },
    { KG_META | KG_ALT, 'h', 0, KG_TAKE },
    { KG_META | KG_ALT, 'd', 0, KG_TAKE },
    { KG_META, 0, KEY_COMMA, KG_TAKE },
    { KG_META | KG_CTRL, 0, KEY_LEFT, KG_TAKE }, { KG_META | KG_CTRL, 0, KEY_RIGHT, KG_TAKE },
};

/* Lord Flow with Settings → Keyboard's Desktops arrows on (the default): ⌃ arrows are word movement in
 * a Linux app, so the person chooses. Kept together with `flow` by being a second table. */
static const Grab flow_desktops[] = {
    { KG_META, 0, KEY_TAB, KG_HOLD },
    { KG_META | KG_SHIFT, 0, KEY_TAB, KG_HOLD },
    { KG_META, 'm', 0, KG_TAKE },
    { KG_META, 'h', 0, KG_TAKE },
    { KG_META | KG_ALT, 'h', 0, KG_TAKE },
    { KG_META | KG_ALT, 'd', 0, KG_TAKE },
    { KG_META, 0, KEY_COMMA, KG_TAKE },
    { KG_META | KG_CTRL, 0, KEY_LEFT, KG_TAKE }, { KG_META | KG_CTRL, 0, KEY_RIGHT, KG_TAKE },
    { KG_CTRL, 0, KEY_LEFT, KG_TAKE }, { KG_CTRL, 0, KEY_RIGHT, KG_TAKE },
    { KG_CTRL, 0, KEY_UP, KG_TAKE }, { KG_CTRL, 0, KEY_DOWN, KG_TAKE },
};

static const Grab classic[] = {
    { KG_ALT, 0, KEY_TAB, KG_HOLD },
    { KG_ALT | KG_SHIFT, 0, KEY_TAB, KG_HOLD },
    { KG_ALT, 0, KEY_F4, KG_TAKE },
    { KG_CTRL | KG_SHIFT, 0, KEY_ESC, KG_TAKE },
    { KG_META, 'a', 0, KG_TAKE },
    { KG_META, 'n', 0, KG_TAKE },
    { KG_META, 'd', 0, KG_TAKE },
    { KG_META, 'e', 0, KG_TAKE },
    { KG_META, 'i', 0, KG_TAKE },
    { KG_META, 'r', 0, KG_TAKE },
    { KG_META | KG_SHIFT, 's', 0, KG_TAKE },
    { KG_META, 0, KEY_LEFT, KG_TAKE }, { KG_META, 0, KEY_RIGHT, KG_TAKE },
    { KG_META, 0, KEY_UP, KG_TAKE }, { KG_META, 0, KEY_DOWN, KG_TAKE },
    { KG_META, 0, KEY_1, KG_TAKE }, { KG_META, 0, KEY_2, KG_TAKE }, { KG_META, 0, KEY_3, KG_TAKE },
    { KG_META, 0, KEY_4, KG_TAKE }, { KG_META, 0, KEY_5, KG_TAKE }, { KG_META, 0, KEY_6, KG_TAKE },
    { KG_META, 0, KEY_7, KG_TAKE }, { KG_META, 0, KEY_8, KG_TAKE }, { KG_META, 0, KEY_9, KG_TAKE },
    { KG_META | KG_CTRL, 'd', 0, KG_TAKE }, { KG_META | KG_CTRL, 0, KEY_F4, KG_TAKE },
    { KG_META | KG_CTRL, 0, KEY_LEFT, KG_TAKE }, { KG_META | KG_CTRL, 0, KEY_RIGHT, KG_TAKE },
};

static const Grab *table;
static size_t count;
static int remaps;

int keygrab_set(const char *profile) {
    if (!profile) return 0;
    if (!profile[0]) { table = NULL; count = 0; remaps = 0; return 1; }
    if (!strcmp(profile, "flow")) { table = flow; count = sizeof flow / sizeof flow[0]; remaps = 1; return 1; }
    if (!strcmp(profile, "flow-desktops")) { table = flow_desktops; count = sizeof flow_desktops / sizeof flow_desktops[0]; remaps = 1; return 1; }
    if (!strcmp(profile, "classic")) { table = classic; count = sizeof classic / sizeof classic[0]; remaps = 0; return 1; }
    return 0;
}

char keygrab_letter(int code, const char *base) {
    static const struct { unsigned short code; char letter; } us[] = {
        { KEY_Q, 'q' }, { KEY_W, 'w' }, { KEY_E, 'e' }, { KEY_R, 'r' }, { KEY_T, 't' }, { KEY_Y, 'y' }, { KEY_U, 'u' },
        { KEY_I, 'i' }, { KEY_O, 'o' }, { KEY_P, 'p' }, { KEY_A, 'a' }, { KEY_S, 's' }, { KEY_D, 'd' }, { KEY_F, 'f' },
        { KEY_G, 'g' }, { KEY_H, 'h' }, { KEY_J, 'j' }, { KEY_K, 'k' }, { KEY_L, 'l' }, { KEY_Z, 'z' }, { KEY_X, 'x' },
        { KEY_C, 'c' }, { KEY_V, 'v' }, { KEY_B, 'b' }, { KEY_N, 'n' }, { KEY_M, 'm' },
    };
    unsigned char b = base ? (unsigned char)base[0] : 0;
    if (b && !base[1] && b < 0x80) {
        if (b >= 'A' && b <= 'Z') return (char)(b - 'A' + 'a');
        return b >= 'a' && b <= 'z' ? (char)b : 0;      /* a Latin layout's digit or symbol */
    }
    for (size_t i = 0; i < sizeof us / sizeof us[0]; i++) if (us[i].code == code) return us[i].letter;
    return 0;
}

int keygrab_match(int code, char letter, unsigned mods) {
    for (size_t i = 0; i < count; i++) {
        const Grab *g = &table[i];
        if (g->mods != mods) continue;
        if (g->letter ? g->letter == letter : !letter && g->code == code) return g->how;
    }
    return KG_PASS;
}

int keygrab_is_remap(char letter, unsigned mods) {
    return remaps && mods == KG_META && letter && strchr("cvxazftwnlrps", letter) != NULL;
}
