/* osk.c — see osk.h. */
#include "osk.h"

#include <linux/input.h>
#include <string.h>

#define LONG_BITS (8 * sizeof(long))

/* Top row (digits and the keys around them), the letter rows with their punctuation and the key next
 * to Shift on international keyboards, then space. sdk/oskmodel.js builds its rows from these. */
const unsigned short osk_label_codes[] = {
    KEY_GRAVE, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_MINUS, KEY_EQUAL,
    KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P, KEY_LEFTBRACE, KEY_RIGHTBRACE,
    KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_BACKSLASH,
    KEY_102ND, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_SPACE,
};
const size_t osk_label_count = sizeof osk_label_codes / sizeof osk_label_codes[0];

int osk_key_allowed(int code) {
    for (size_t i = 0; i < osk_label_count; i++) if (osk_label_codes[i] == code) return 1;
    switch (code) {
    case KEY_BACKSPACE: case KEY_ENTER: case KEY_TAB: case KEY_ESC:
    case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN:
        return 1;
    default:
        return 0;
    }
}

int osk_level_valid(int level) { return level >= 0 && level <= (OSK_LEVEL_SHIFT | OSK_LEVEL_ALTGR); }

static OskTap queue[OSK_QUEUE_MAX];
static int qhead, qlen;

int osk_push(int code, int level) {
    if (!osk_key_allowed(code) || !osk_level_valid(level) || qlen >= OSK_QUEUE_MAX) return 0;
    queue[(qhead + qlen++) % OSK_QUEUE_MAX] = (OskTap){ code, level };
    return 1;
}

int osk_pop(OskTap *t) {
    if (!qlen) return 0;
    *t = queue[qhead];
    queue[qhead] = (OskTap){ 0, 0 };
    qhead = (qhead + 1) % OSK_QUEUE_MAX;
    qlen--;
    return 1;
}

void osk_clear(void) {
    memset(queue, 0, sizeof queue);
    qhead = qlen = 0;
}

static int has(const unsigned long *keys, size_t nlongs, unsigned code) {
    size_t i = code / LONG_BITS;
    return i < nlongs && ((keys[i] >> (code % LONG_BITS)) & 1UL);
}

/* Security keys and similar tokens type by themselves and declare a full keyboard, with nobody's
 * fingers on it. Matched on the name the kernel reports. */
static int token_name(const char *name) {
    static const char *const tokens[] = { "YubiKey", "Yubico", "Nitrokey", "SoloKey", "OnlyKey", "Token" };
    for (size_t i = 0; i < sizeof tokens / sizeof tokens[0]; i++) if (strstr(name, tokens[i])) return 1;
    return 0;
}

int osk_full_keyboard(const unsigned long *keys, size_t nlongs, const char *name) {
    static const unsigned short need[] = { KEY_SPACE, KEY_ENTER, KEY_BACKSPACE, KEY_LEFTSHIFT,
        KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P, KEY_A, KEY_S, KEY_D, KEY_F, KEY_G,
        KEY_H, KEY_J, KEY_K, KEY_L, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M };
    for (size_t i = 0; i < sizeof need / sizeof need[0]; i++) if (!has(keys, nlongs, need[i])) return 0;
    return !token_name(name ? name : "");
}

int osk_keyboard_counts(int bus, int portable, int typed) {
    return bus != BUS_I8042 || portable || typed;
}

int osk_chassis_portable(int type) {
    /* SMBIOS: 8 portable, 9 laptop, 10 notebook, 14 sub notebook, 31 convertible, 32 detachable.
     * 30 (tablet) and 11 (hand held) have no keyboard of their own. */
    return type == 8 || type == 9 || type == 10 || type == 14 || type == 31 || type == 32;
}
