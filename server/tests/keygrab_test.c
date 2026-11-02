/* keygrab_test.c — the fixed tables of keys the desktop keeps from Wayland apps (src/wl/keygrab.c). */
#include "../src/wl/keygrab.h"
#include <linux/input-event-codes.h>
#include <stdio.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

int main(void) {
    /* the media row stays with LordOS; the F keys and Esc are the app's */
    static const int kept[] = { KEY_MUTE, KEY_VOLUMEDOWN, KEY_VOLUMEUP, KEY_BRIGHTNESSDOWN, KEY_BRIGHTNESSUP, KEY_KBDILLUMDOWN,
                                KEY_KBDILLUMUP, KEY_PLAYPAUSE, KEY_NEXTSONG, KEY_PREVIOUSSONG, KEY_SYSRQ };
    for (size_t i = 0; i < sizeof kept / sizeof kept[0]; i++) CHECK(keygrab_system_key(kept[i]));
    CHECK(!keygrab_system_key(KEY_ESC));
    CHECK(!keygrab_system_key(KEY_F11));
    CHECK(!keygrab_system_key(KEY_A));

    /* no table: nothing is taken */
    CHECK(keygrab_match(KEY_TAB, 0, KG_ALT) == KG_PASS);
    CHECK(!keygrab_is_remap('c', KG_META));

    /* only the fixed names, and a refused name keeps the table */
    /* Desktops: ⌃ arrows are kept only with the option on, and never with another modifier */
    CHECK(keygrab_set("flow"));
    CHECK(keygrab_match(KEY_LEFT, 0, KG_CTRL) == KG_PASS);
    CHECK(keygrab_match(KEY_UP, 0, KG_CTRL) == KG_PASS);
    CHECK(keygrab_match(KEY_LEFT, 0, KG_META | KG_CTRL) == KG_TAKE);
    CHECK(keygrab_set("flow-desktops"));
    CHECK(keygrab_match(KEY_LEFT, 0, KG_CTRL) == KG_TAKE);
    CHECK(keygrab_match(KEY_RIGHT, 0, KG_CTRL) == KG_TAKE);
    CHECK(keygrab_match(KEY_UP, 0, KG_CTRL) == KG_TAKE);
    CHECK(keygrab_match(KEY_DOWN, 0, KG_CTRL) == KG_TAKE);
    CHECK(keygrab_match(KEY_LEFT, 0, KG_CTRL | KG_SHIFT) == KG_PASS);      /* selecting by word */
    CHECK(keygrab_match(KEY_TAB, 0, KG_META) == KG_HOLD);
    CHECK(keygrab_is_remap('c', KG_META));
    CHECK(keygrab_set("classic"));
    CHECK(keygrab_match(KEY_LEFT, 0, KG_META | KG_CTRL) == KG_TAKE);
    CHECK(keygrab_match(KEY_F4, 0, KG_META | KG_CTRL) == KG_TAKE);
    CHECK(keygrab_match(KEY_D, 'd', KG_META | KG_CTRL) == KG_TAKE);
    CHECK(keygrab_match(KEY_LEFT, 0, KG_CTRL) == KG_PASS);
    /* the switcher's keys, from Settings → Keyboard */
    CHECK(keygrab_set("flow+tab-alt"));
    CHECK(keygrab_match(KEY_TAB, 0, KG_ALT) == KG_HOLD);
    CHECK(keygrab_match(KEY_TAB, 0, KG_ALT | KG_SHIFT) == KG_HOLD);
    CHECK(keygrab_match(KEY_TAB, 0, KG_META) == KG_PASS);
    CHECK(keygrab_set("classic+tab-both"));
    CHECK(keygrab_match(KEY_TAB, 0, KG_META) == KG_HOLD);
    CHECK(keygrab_match(KEY_TAB, 0, KG_ALT) == KG_HOLD);
    CHECK(keygrab_set("classic+tab-meta"));
    CHECK(keygrab_match(KEY_TAB, 0, KG_ALT) == KG_PASS);
    CHECK(keygrab_match(0, 'w', KG_META) == KG_TAKE);
    CHECK(!keygrab_set("flow+tab-x"));
    CHECK(!keygrab_set("flow+x"));
    CHECK(keygrab_set("classic"));
    CHECK(!keygrab_set("flow "));
    CHECK(!keygrab_set("alt+x"));
    CHECK(!keygrab_set(NULL));
    CHECK(keygrab_match(KEY_TAB, 0, KG_ALT) == KG_HOLD);

    /* Lord Classic */
    CHECK(keygrab_match(KEY_TAB, 0, KG_ALT | KG_SHIFT) == KG_HOLD);
    CHECK(keygrab_match(KEY_TAB, 0, KG_CTRL) == KG_PASS);                  /* Ctrl+Tab stays with the apps */
    CHECK(keygrab_match(KEY_F4, 0, KG_ALT) == KG_TAKE);
    CHECK(keygrab_match(KEY_F4, 0, 0) == KG_PASS);
    CHECK(keygrab_match(KEY_ESC, 0, KG_CTRL | KG_SHIFT) == KG_TAKE);
    CHECK(keygrab_match(KEY_ESC, 0, KG_CTRL) == KG_PASS);
    CHECK(keygrab_match(KEY_A, 'a', KG_META) == KG_TAKE);
    CHECK(keygrab_match(KEY_A, 'a', KG_CTRL) == KG_PASS);
    CHECK(keygrab_match(KEY_S, 's', KG_META | KG_SHIFT) == KG_TAKE);
    CHECK(keygrab_match(KEY_S, 's', KG_META) == KG_PASS);
    CHECK(keygrab_match(KEY_5, 0, KG_META) == KG_TAKE);
    CHECK(keygrab_match(KEY_0, 0, KG_META) == KG_PASS);
    CHECK(keygrab_match(KEY_Q, 'q', KG_CTRL) == KG_PASS);                  /* Ctrl+Q is never global */
    CHECK(!keygrab_is_remap('c', KG_META));                                /* no ❖ → Ctrl here */

    /* Lord Flow */
    CHECK(keygrab_set("flow"));
    CHECK(keygrab_match(KEY_TAB, 0, KG_ALT) == KG_PASS);
    CHECK(keygrab_match(KEY_TAB, 0, KG_META) == KG_HOLD);
    CHECK(keygrab_match(KEY_M, 'm', KG_META) == KG_TAKE);
    CHECK(keygrab_match(KEY_H, 'h', KG_META | KG_ALT) == KG_TAKE);
    CHECK(keygrab_match(KEY_COMMA, 0, KG_META) == KG_TAKE);
    for (const char *p = "cvxazftwnlrps"; *p; p++) CHECK(keygrab_is_remap(*p, KG_META));
    CHECK(!keygrab_is_remap('q', KG_META));
    CHECK(!keygrab_is_remap('m', KG_META));
    CHECK(!keygrab_is_remap('v', KG_META | KG_SHIFT));
    CHECK(!keygrab_is_remap('c', KG_CTRL));
    CHECK(!keygrab_is_remap(0, KG_META));

    /* the letter follows the layout, and the position only where the layout has no Latin letters */
    CHECK(keygrab_letter(KEY_A, "a") == 'a');
    CHECK(keygrab_letter(KEY_Q, "a") == 'a');                              /* the A of an AZERTY keyboard */
    CHECK(keygrab_letter(KEY_A, "A") == 'a');
    CHECK(keygrab_letter(KEY_C, "\xd1\x81") == 'c');                       /* Cyrillic */
    CHECK(keygrab_letter(KEY_1, "1") == 0);
    CHECK(keygrab_letter(KEY_1, "&") == 0);
    CHECK(keygrab_letter(KEY_TAB, "") == 0);
    CHECK(keygrab_letter(KEY_A, "") == 'a');
    CHECK(keygrab_letter(KEY_A, NULL) == 'a');

    /* a letter key never matches through its code: on Dvorak the V position is K */
    CHECK(keygrab_set("classic"));
    CHECK(keygrab_match(KEY_COMMA, 'w', KG_META) == KG_PASS);

    CHECK(keygrab_set(""));
    CHECK(keygrab_match(KEY_TAB, 0, KG_ALT) == KG_PASS);
    printf(fails ? "keygrab: %d failed\n" : "keygrab: ok\n", fails);
    return fails != 0;
}
