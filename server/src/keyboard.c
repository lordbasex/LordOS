/* keyboard.c — keyboard with libxkbcommon: all XKB layouts (xkb-data), with
 * their variants, AltGr, dead keys and compose sequences according to the user's region.
 *
 * It is the same library GNOME and KDE use (and that Wayland apps will require), so
 * "latam", "us", "br(abnt2)", "pt", "fr(oss)"… behave as on any Linux.
 * If xkb-data is not installed, kb_ready() is 0 and the UI uses its fallback tables. */
#include "keyboard.h"

#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

static struct xkb_context *ctx;
static struct xkb_keymap *keymap;
static struct xkb_state *state;
static struct xkb_compose_table *compose_table;
static struct xkb_compose_state *compose;
static char cur_layouts[128], cur_variants[128];
static int altgr_down;

static int valid(const char *s) {
    if (strlen(s) > 120) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == ',' || *s == '_' || *s == '-')) return 0;
    return 1;
}

static void setup_compose(void) {
    if (compose) xkb_compose_state_unref(compose);
    if (compose_table) xkb_compose_table_unref(compose_table);
    compose = NULL;
    const char *loc = getenv("LC_ALL");
    if (!loc || !*loc) loc = getenv("LC_CTYPE");
    if (!loc || !*loc) loc = getenv("LANG");
    if (!loc || !*loc) loc = "C";
    compose_table = xkb_compose_table_new_from_locale(ctx, loc, XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (!compose_table) compose_table = xkb_compose_table_new_from_locale(ctx, "en_US.UTF-8", XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (compose_table) compose = xkb_compose_state_new(compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
}

/* layouts: "latam" or "us,latam" (up to 4); variants in the same order ("", "abnt2"…). */
int kb_set(const char *layouts, const char *variants) {
    if (!valid(layouts) || !valid(variants) || !layouts[0]) return -1;
    if (!ctx && !(ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) return -1;
    struct xkb_rule_names names = { "evdev", "pc105", layouts, variants, "" };
    struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) return -1;
    if (state) xkb_state_unref(state);
    if (keymap) xkb_keymap_unref(keymap);
    keymap = km;
    state = xkb_state_new(keymap);
    snprintf(cur_layouts, sizeof cur_layouts, "%s", layouts);
    snprintf(cur_variants, sizeof cur_variants, "%s", variants);
    setup_compose();
    return state ? 0 : -1;
}

/* App mode (appclient.c): the keymap the compositor sends over wl_keyboard, already fully
 * expanded, so compiling it reads no files (the sandbox blocks /usr/share/X11/xkb). The compose
 * table is kept from the kb_set() made before the sandbox. */
int kb_set_keymap_string(const char *text) {
    if (!text || !text[0]) return -1;
    if (!ctx && !(ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) return -1;
    struct xkb_keymap *km = xkb_keymap_new_from_string(ctx, text, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) return -1;
    struct xkb_state *st = xkb_state_new(km);
    if (!st) { xkb_keymap_unref(km); return -1; }
    if (state) xkb_state_unref(state);
    if (keymap) xkb_keymap_unref(keymap);
    keymap = km;
    state = st;
    return 0;
}

/* Forget the keys held down (app mode: the keyboard focus left or came back, and the releases
 * that happened meanwhile were never seen). Keeps the active layout. */
void kb_reset_state(void) {
    if (!keymap || !state) return;
    int g = kb_group();
    struct xkb_state *st = xkb_state_new(keymap);
    if (!st) return;
    xkb_state_unref(state);
    state = st;
    altgr_down = 0;
    kb_set_group(g);
}

/* The computer's layouts, as Debian's installer, keyboard-configuration and the LordOS installer write
 * them (XKBLAYOUT="us,latam", XKBVARIANT=",deadtilde"). Without the file, or with a value keyboard.c
 * would refuse, it is "us": never a layout nobody chose (#55). */
void kb_system_from(const char *path, char *layouts, size_t ln, char *variants, size_t vn) {
    snprintf(layouts, ln, "us");
    snprintf(variants, vn, "%s", "");
    FILE *f = fopen(path, "re");
    if (!f) return;
    char line[256], l[128] = "", v[128] = "";
    while (fgets(line, sizeof line, f)) {
        char *out = !strncmp(line, "XKBLAYOUT=", 10) ? l : !strncmp(line, "XKBVARIANT=", 11) ? v : NULL;
        if (!out) continue;
        char *s = strchr(line, '=') + 1;
        if (*s == '"' || *s == '\'') s++;
        size_t k = strcspn(s, "\"'\r\n");
        if (k >= sizeof l) continue;
        memcpy(out, s, k);
        out[k] = 0;
    }
    fclose(f);
    if (!l[0] || !valid(l)) return;
    snprintf(layouts, ln, "%s", l);
    if (valid(v)) snprintf(variants, vn, "%s", v);
}

int kb_ready(void) { return state != NULL; }
const char *kb_layouts(void) { return cur_layouts; }
const char *kb_variants(void) { return cur_variants; }
void kb_reload_compose(void) { if (ctx) setup_compose(); }

int kb_group(void) {
    if (!state) return 0;
    for (xkb_layout_index_t i = 0; i < xkb_keymap_num_layouts(keymap); i++)
        if (xkb_state_layout_index_is_active(state, i, XKB_STATE_LAYOUT_EFFECTIVE) > 0) return (int)i;
    return 0;
}

void kb_set_group(int g) {
    if (!state || g < 0 || g >= (int)xkb_keymap_num_layouts(keymap)) return;
    xkb_mod_mask_t dep = (xkb_mod_mask_t)xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED);
    xkb_mod_mask_t lat = (xkb_mod_mask_t)xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED);
    xkb_mod_mask_t lock = (xkb_mod_mask_t)xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED);
    xkb_state_update_mask(state, dep, lat, lock, 0, 0, (xkb_layout_index_t)g);
}

/* The locked modifiers as the compositor has them (wl_keyboard.modifiers): a window that gets the keyboard starts
 * from a fresh state (kb_reset_state), so without this Caps Lock turned on elsewhere would read as off here. Only
 * the bits the keymap has; the keys held and the layout stay as they are. */
void kb_set_locked(uint32_t locked) {
    if (!state) return;
    xkb_mod_index_t n = xkb_keymap_num_mods(keymap);
    if (n < 32) locked &= (1u << n) - 1;
    xkb_mod_mask_t dep = (xkb_mod_mask_t)xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED);
    xkb_mod_mask_t lat = (xkb_mod_mask_t)xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED);
    xkb_layout_index_t g = xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_LOCKED);
    xkb_state_update_mask(state, dep, lat, (xkb_mod_mask_t)locked, 0, 0, g);
}

/* ---- labels of the on-screen keyboard: a scratch state per level ---- */

static struct xkb_state *level_state[4];

void kb_labels_end(void) {
    for (int i = 0; i < 4; i++) { if (level_state[i]) xkb_state_unref(level_state[i]); level_state[i] = NULL; }
}

int kb_labels_begin(void) {
    kb_labels_end();
    if (!state) return 0;
    xkb_layout_index_t group = (xkb_layout_index_t)kb_group();
    for (int i = 0; i < 4; i++) {
        if (!(level_state[i] = xkb_state_new(keymap))) { kb_labels_end(); return 0; }
        if (i & 1) xkb_state_update_key(level_state[i], KEY_LEFTSHIFT + 8, XKB_KEY_DOWN);
        if (i & 2) xkb_state_update_key(level_state[i], KEY_RIGHTALT + 8, XKB_KEY_DOWN);
        xkb_mod_mask_t dep = (xkb_mod_mask_t)xkb_state_serialize_mods(level_state[i], XKB_STATE_MODS_DEPRESSED);
        xkb_state_update_mask(level_state[i], dep, 0, 0, 0, 0, group);
    }
    return 1;
}

/* A dead key's own symbol has no text: the mark it adds. */
static const char *dead_mark(xkb_keysym_t s) {
    switch (s) {
    case XKB_KEY_dead_acute:       return "´";
    case XKB_KEY_dead_grave:       return "`";
    case XKB_KEY_dead_circumflex:  return "^";
    case XKB_KEY_dead_tilde:       return "~";
    case XKB_KEY_dead_diaeresis:   return "¨";
    case XKB_KEY_dead_cedilla:     return "¸";
    case XKB_KEY_dead_abovering:   return "°";
    case XKB_KEY_dead_macron:      return "¯";
    case XKB_KEY_dead_caron:       return "ˇ";
    case XKB_KEY_dead_breve:       return "˘";
    case XKB_KEY_dead_abovedot:    return "˙";
    case XKB_KEY_dead_ogonek:      return "˛";
    case XKB_KEY_dead_doubleacute: return "˝";
    default:                       return NULL;
    }
}

void kb_label(int code, int level, char *out, size_t n) {
    out[0] = 0;
    if (level < 0 || level > 3 || !level_state[level]) return;
    xkb_keysym_t s = xkb_state_key_get_one_sym(level_state[level], (xkb_keycode_t)code + 8);
    const char *mark = dead_mark(s);
    if (mark) snprintf(out, n, "dead:%s", mark);
    else if (s != XKB_KEY_NoSymbol && xkb_keysym_to_utf8(s, out, n) <= 0) out[0] = 0;
    if ((unsigned char)out[0] < 0x20 || out[0] == 0x7f) out[0] = 0;    /* control characters: Tab, Enter */
}

int kb_has_altgr(void) {
    if (!keymap) return 0;
    const xkb_keysym_t *syms;
    xkb_layout_index_t group = (xkb_layout_index_t)kb_group();
    int n = xkb_keymap_key_get_syms_by_level(keymap, KEY_RIGHTALT + 8, group, 0, &syms);
    return n == 1 && syms[0] == XKB_KEY_ISO_Level3_Shift;
}

/* Key names as on the web (KeyboardEvent.key) for keys that do not type. */
static const char *named(xkb_keysym_t s) {
    switch (s) {
    case XKB_KEY_Escape: return "Escape";
    case XKB_KEY_BackSpace: return "Backspace";
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: return "Tab";
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: return "Enter";
    case XKB_KEY_Up: case XKB_KEY_KP_Up: return "ArrowUp";
    case XKB_KEY_Down: case XKB_KEY_KP_Down: return "ArrowDown";
    case XKB_KEY_Left: case XKB_KEY_KP_Left: return "ArrowLeft";
    case XKB_KEY_Right: case XKB_KEY_KP_Right: return "ArrowRight";
    case XKB_KEY_Home: case XKB_KEY_KP_Home: return "Home";
    case XKB_KEY_End: case XKB_KEY_KP_End: return "End";
    case XKB_KEY_Page_Up: case XKB_KEY_KP_Page_Up: return "PageUp";
    case XKB_KEY_Page_Down: case XKB_KEY_KP_Page_Down: return "PageDown";
    case XKB_KEY_Insert: case XKB_KEY_KP_Insert: return "Insert";
    case XKB_KEY_Delete: case XKB_KEY_KP_Delete: return "Delete";
    case XKB_KEY_Super_L: case XKB_KEY_Super_R: return "Meta";
    default: break;
    }
    if (s >= XKB_KEY_F1 && s <= XKB_KEY_F12) {
        static const char *const f[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
        return f[s - XKB_KEY_F1];
    }
    return NULL;
}

static void utf8(uint32_t cp, char *out) {
    if (!cp || cp < 0x20 || cp == 0x7f) { out[0] = 0; return; }
    if (cp < 0x80) { out[0] = (char)cp; out[1] = 0; }
    else if (cp < 0x800) { out[0] = (char)(0xc0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3f)); out[2] = 0; }
    else if (cp < 0x10000) { out[0] = (char)(0xe0 | cp >> 12); out[1] = (char)(0x80 | (cp >> 6 & 0x3f)); out[2] = (char)(0x80 | (cp & 0x3f)); out[3] = 0; }
    else { out[0] = (char)(0xf0 | cp >> 18); out[1] = (char)(0x80 | (cp >> 12 & 0x3f)); out[2] = (char)(0x80 | (cp >> 6 & 0x3f)); out[3] = (char)(0x80 | (cp & 0x3f)); out[4] = 0; }
}

void kb_key(int code, int value, KeyInfo *k) {
    memset(k, 0, sizeof *k);
    if (!state) return;
    xkb_keycode_t kc = (xkb_keycode_t)code + 8;
    int down = value != 0;
    if (code == 100) altgr_down = down;
    if (value != 2) xkb_state_update_key(state, kc, down ? XKB_KEY_DOWN : XKB_KEY_UP);
    k->shift = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;
    k->ctrl = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
    k->alt = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;
    k->meta = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0;
    k->caps = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) > 0;
    k->altgr = altgr_down;
    if (!down) return;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(state, kc);
    const char *name = named(sym);
    if (name) snprintf(k->key, sizeof k->key, "%s", name);
    /* base: what the key produces with Shift/AltGr but without the effect of Ctrl/Alt */
    utf8(xkb_keysym_to_utf32(sym), k->base);
    if (k->ctrl || k->alt || k->meta || name) {
        if (!name) snprintf(k->key, sizeof k->key, "%s", k->base);
        return;
    }
    if (compose) {
        xkb_compose_state_feed(compose, sym);
        switch (xkb_compose_state_get_status(compose)) {
        case XKB_COMPOSE_COMPOSING: k->dead = 1; return;             /* dead key: waits for the next one */
        case XKB_COMPOSE_COMPOSED:
            xkb_compose_state_get_utf8(compose, k->chr, sizeof k->chr);
            xkb_compose_state_reset(compose);
            snprintf(k->key, sizeof k->key, "%s", k->chr);
            return;
        case XKB_COMPOSE_CANCELLED: xkb_compose_state_reset(compose); return;
        default: break;
        }
    }
    xkb_state_key_get_utf8(state, kc, k->chr, sizeof k->chr);
    if ((unsigned char)k->chr[0] < 0x20 || k->chr[0] == 0x7f) k->chr[0] = 0;
    snprintf(k->key, sizeof k->key, "%s", k->chr);
}

char *kb_keymap_string(void) { return keymap ? xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL; }

int kb_leds(void) {
    if (!state) return 0;
    return (xkb_state_led_name_is_active(state, XKB_LED_NAME_NUM) > 0) << LED_NUML |
           (xkb_state_led_name_is_active(state, XKB_LED_NAME_CAPS) > 0) << LED_CAPSL |
           (xkb_state_led_name_is_active(state, XKB_LED_NAME_SCROLL) > 0) << LED_SCROLLL;
}

void kb_mods(uint32_t *depressed, uint32_t *latched, uint32_t *locked, uint32_t *group) {
    *depressed = *latched = *locked = *group = 0;
    if (!state) return;
    *depressed = (uint32_t)xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED);
    *latched = (uint32_t)xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED);
    *locked = (uint32_t)xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED);
    *group = (uint32_t)xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_EFFECTIVE);
}
