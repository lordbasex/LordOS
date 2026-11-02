/* osk_test.c — the on-screen keyboard's C side: which devices count as a keyboard, which keys a tap
 * may press, the queue, and the labels of real XKB layouts (skipped without xkb-data). */
#include "../src/keyboard.h"
#include "../src/osk.h"

#include <linux/input.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAILED line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define LBITS (8 * sizeof(unsigned long))
#define NL ((KEY_MAX + 1) / LBITS + 1)
static void set(unsigned long *k, unsigned c) { k[c / LBITS] |= 1UL << (c % LBITS); }
static void clear_bit(unsigned long *k, unsigned c) { k[c / LBITS] &= ~(1UL << (c % LBITS)); }

static void full(unsigned long *k) {
    memset(k, 0, NL * sizeof *k);
    for (unsigned c = KEY_ESC; c <= KEY_KPDOT; c++) set(k, c);
}

static void test_devices(void) {
    unsigned long k[NL];
    full(k);
    CHECK(osk_full_keyboard(k, NL, "USB Keyboard"));
    CHECK(osk_full_keyboard(k, NL, ""));
    CHECK(osk_full_keyboard(k, NL, NULL));
    CHECK(!osk_full_keyboard(k, NL, "Yubico YubiKey OTP+FIDO+CCID"));
    clear_bit(k, KEY_Q);
    CHECK(!osk_full_keyboard(k, NL, "remote with no Q"));
    full(k);
    clear_bit(k, KEY_LEFTSHIFT);
    CHECK(!osk_full_keyboard(k, NL, "no Shift"));
    /* a power button, media keys */
    memset(k, 0, sizeof k);
    set(k, KEY_POWER);
    set(k, KEY_VOLUMEUP);
    CHECK(!osk_full_keyboard(k, NL, "Power Button"));
    /* a capability bitmap shorter than the key it asks for */
    full(k);
    CHECK(!osk_full_keyboard(k, 0, "short"));
}

static void test_counts(void) {
    /* USB and Bluetooth always; the PS/2 controller of a desktop only after typing */
    CHECK(osk_keyboard_counts(BUS_USB, 0, 0));
    CHECK(osk_keyboard_counts(BUS_BLUETOOTH, 0, 0));
    CHECK(!osk_keyboard_counts(BUS_I8042, 0, 0));
    CHECK(osk_keyboard_counts(BUS_I8042, 0, 1));
    CHECK(osk_keyboard_counts(BUS_I8042, 1, 0));
    CHECK(osk_chassis_portable(9) && osk_chassis_portable(10) && osk_chassis_portable(31));
    CHECK(!osk_chassis_portable(3) && !osk_chassis_portable(30) && !osk_chassis_portable(0) && !osk_chassis_portable(1));
}

static void test_keys(void) {
    static const int yes[] = { KEY_A, KEY_Z, KEY_1, KEY_SPACE, KEY_ENTER, KEY_BACKSPACE, KEY_TAB, KEY_ESC, KEY_LEFT, KEY_DOWN, KEY_102ND, KEY_SEMICOLON };
    static const int no[] = { 0, -1, KEY_LEFTMETA, KEY_RIGHTMETA, KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTSHIFT,
                              KEY_RIGHTSHIFT, KEY_F1, KEY_F12, KEY_SYSRQ, KEY_POWER, KEY_DELETE, KEY_PLAYPAUSE, BTN_LEFT, 100000 };
    for (size_t i = 0; i < sizeof yes / sizeof yes[0]; i++) CHECK(osk_key_allowed(yes[i]));
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++) CHECK(!osk_key_allowed(no[i]));
    CHECK(osk_level_valid(0) && osk_level_valid(3) && !osk_level_valid(4) && !osk_level_valid(-1));
}

static void test_queue(void) {
    OskTap t;
    osk_clear();
    CHECK(!osk_pop(&t));
    CHECK(!osk_push(KEY_LEFTMETA, 0));
    CHECK(!osk_push(KEY_A, 4));
    CHECK(!osk_pop(&t));
    for (int i = 0; i < OSK_QUEUE_MAX; i++) CHECK(osk_push(KEY_A + i % 3, 1));
    CHECK(!osk_push(KEY_A, 0));
    for (int i = 0; i < OSK_QUEUE_MAX; i++) { CHECK(osk_pop(&t)); CHECK(t.code == KEY_A + i % 3 && t.level == 1); }
    CHECK(!osk_pop(&t));
    /* it wraps around */
    for (int r = 0; r < OSK_QUEUE_MAX * 3; r++) { CHECK(osk_push(KEY_B, 0)); CHECK(osk_pop(&t) && t.code == KEY_B); }
    CHECK(osk_push(KEY_C, 0));
    osk_clear();
    CHECK(!osk_pop(&t));
}

static void label(int code, int level, char *out, size_t n) { kb_label(code, level, out, n); }

static void test_labels(void) {
    char s[32];
    if (kb_set("us", "") || !kb_labels_begin()) { printf("  labels skipped: needs xkb-data\n"); return; }
    label(KEY_A, 0, s, sizeof s); CHECK(!strcmp(s, "a"));
    label(KEY_A, 1, s, sizeof s); CHECK(!strcmp(s, "A"));
    label(KEY_2, 1, s, sizeof s); CHECK(!strcmp(s, "@"));
    label(KEY_SPACE, 0, s, sizeof s); CHECK(!strcmp(s, " "));
    label(KEY_ENTER, 0, s, sizeof s); CHECK(!s[0]);          /* a control character is no label */
    label(KEY_A, 9, s, sizeof s); CHECK(!s[0]);
    CHECK(!kb_has_altgr());
    kb_labels_end();

    CHECK(!kb_set("latam", ""));
    CHECK(kb_labels_begin());
    CHECK(kb_has_altgr());
    label(KEY_SEMICOLON, 0, s, sizeof s); CHECK(!strcmp(s, "ñ"));
    label(KEY_LEFTBRACE, 0, s, sizeof s); CHECK(!strcmp(s, "dead:´"));
    label(KEY_Q, 2, s, sizeof s); CHECK(!strcmp(s, "@"));
    kb_labels_end();

    CHECK(!kb_set("us,ara", ""));
    kb_set_group(1);
    CHECK(kb_labels_begin());
    label(KEY_H, 0, s, sizeof s); CHECK(!strcmp(s, "ا"));     /* the active layout, not the first */
    kb_labels_end();
    /* the scratch states did not press anything in the real one */
    KeyInfo k;
    kb_key(KEY_H, 1, &k);
    CHECK(!k.shift && !k.altgr);
    kb_key(KEY_H, 0, &k);
}

int main(void) {
    test_devices();
    test_counts();
    test_keys();
    test_queue();
    test_labels();
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("osk-test: ok\n");
    return 0;
}
