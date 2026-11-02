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
/* Which modifier holds the app switcher, from Settings → Keyboard: a profile's "+tab-meta", "+tab-alt" or
 * "+tab-both". No suffix: what the table says. */
static int tab_set, tab_meta, tab_alt;

int keygrab_set(const char *name) {
    if (!name) return 0;
    char profile[32];
    const char *plus = strchr(name, '+');
    size_t len = plus ? (size_t)(plus - name) : strlen(name);
    if (len >= sizeof profile) return 0;
    memcpy(profile, name, len);
    profile[len] = 0;
    int set = 0, meta = 0, alt = 0;
    if (plus) {
        set = 1;
        if (!strcmp(plus, "+tab-meta")) meta = 1;
        else if (!strcmp(plus, "+tab-alt")) alt = 1;
        else if (!strcmp(plus, "+tab-both")) meta = alt = 1;
        else return 0;
    }
    if (strcmp(profile, "flow") && strcmp(profile, "flow-desktops") && strcmp(profile, "classic") && profile[0]) return 0;
    tab_set = set; tab_meta = meta; tab_alt = alt;
    if (!profile[0]) { table = NULL; count = 0; remaps = 0; tab_set = 0; return 1; }
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
    if (tab_set && code == KEY_TAB) {
        unsigned m = mods & (unsigned)~KG_SHIFT;
        if (m == KG_META) return tab_meta ? KG_HOLD : KG_PASS;
        if (m == KG_ALT) return tab_alt ? KG_HOLD : KG_PASS;
    }
    if (tab_set && tab_meta && table == classic && letter == 'w' && mods == KG_META) return KG_TAKE;   /* the Overview moves here */
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

/* Player, sound, screen and keyboard light, and Print Screen (Screenshots). A laptop sends these from its
 * top row with no modifier, so an app in front must not swallow them. */
int keygrab_system_key(int code) {
    switch (code) {
    case KEY_PLAYPAUSE: case KEY_NEXTSONG: case KEY_PREVIOUSSONG: case KEY_STOPCD: case KEY_PLAYCD: case KEY_PAUSECD:
    case KEY_MUTE: case KEY_VOLUMEDOWN: case KEY_VOLUMEUP:
    case KEY_BRIGHTNESSDOWN: case KEY_BRIGHTNESSUP: case KEY_KBDILLUMDOWN: case KEY_KBDILLUMUP:
    case KEY_SYSRQ:
        return 1;
    default:
        return 0;
    }
}
