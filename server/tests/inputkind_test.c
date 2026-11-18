/* inputkind_test.c — an absolute device is a pointer only with a button or a touch (src/inputkind.h, issue #2); a
 * tablet's volume buttons are a device of system keys that passes on only those keys (issue #131); a mouse's back
 * and forward buttons are DOM buttons 3 and 4 and reach a Wayland app as BTN_SIDE and BTN_EXTRA (issue #159). */
#include "../src/inputkind.h"
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#define L ((KEY_MAX + 1) / (sizeof(unsigned long) * CHAR_BIT) + 1)
static unsigned long ev[4], ab[8], keys[L], props[4];
static void set(unsigned long *b, int n) { b[n / (sizeof(unsigned long) * CHAR_BIT)] |= 1UL << (n % (sizeof(unsigned long) * CHAR_BIT)); }
static void reset(void) { memset(ev, 0, sizeof ev); memset(ab, 0, sizeof ab); memset(keys, 0, sizeof keys); memset(props, 0, sizeof props); set(ev, EV_ABS); set(ab, ABS_X); set(ab, ABS_Y); }
static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); fails++; } } while (0)
#define IS() input_is_abs_pointer(ev, ab, keys, props)
int main(void) {
    reset();                                   CHECK(!IS());   /* applesmc: X and Y, nothing else */
    reset(); set(ev, EV_KEY); set(keys, BTN_LEFT);  CHECK(IS());    /* a virtual machine's tablet */
    reset(); set(ev, EV_KEY); set(keys, BTN_TOUCH); CHECK(IS());    /* a touch panel with no multitouch */
    reset(); set(ev, EV_KEY); set(keys, BTN_TOOL_PEN); CHECK(IS()); /* a pen */
    reset(); set(ev, EV_KEY); set(keys, BTN_TRIGGER); CHECK(!IS()); /* a joystick */
    reset(); set(ev, EV_KEY); set(keys, BTN_LEFT); set(props, INPUT_PROP_ACCELEROMETER); CHECK(!IS());
    memset(ab, 0, sizeof ab); set(ev, EV_KEY); set(keys, BTN_LEFT); memset(props, 0, sizeof props); CHECK(!IS());

    /* system keys (issue #131) */
#define SK() input_is_system_keys(ev, keys)
    memset(ev, 0, sizeof ev); memset(keys, 0, sizeof keys);
    set(ev, EV_KEY); set(keys, KEY_VOLUMEUP); set(keys, KEY_VOLUMEDOWN); set(keys, KEY_POWER); set(keys, KEY_LEFTMETA);
    CHECK(SK());                                              /* gpio-keys on a Chuwi Hi12 */
    memset(keys, 0, sizeof keys); set(keys, KEY_POWER);       CHECK(!SK());   /* an ACPI "Power Button" */
    memset(keys, 0, sizeof keys); set(keys, KEY_BRIGHTNESSUP); set(keys, KEY_BRIGHTNESSDOWN); set(keys, KEY_SWITCHVIDEOMODE);
    CHECK(!SK());                                             /* a "Video Bus": the kernel may act on it itself */
    memset(keys, 0, sizeof keys); set(keys, KEY_MUTE); set(keys, KEY_PLAYPAUSE); set(keys, KEY_CALC);
    CHECK(SK());                                              /* a keyboard's "Consumer Control" node */
    set(keys, KEY_A); set(keys, KEY_ENTER);                   CHECK(!SK());   /* a full keyboard stays a keyboard */
    memset(keys, 0, sizeof keys); set(keys, BTN_LEFT); set(keys, KEY_VOLUMEUP); CHECK(!SK());   /* a mouse stays a mouse */
    memset(keys, 0, sizeof keys); set(keys, BTN_TOUCH); set(keys, KEY_VOLUMEUP); CHECK(!SK());  /* a touch panel too */
    memset(ev, 0, sizeof ev); memset(keys, 0, sizeof keys); set(keys, KEY_VOLUMEUP); CHECK(!SK());   /* no EV_KEY */
    CHECK(input_syskeys_event(EV_KEY, KEY_VOLUMEUP) && input_syskeys_event(EV_KEY, KEY_VOLUMEDOWN) && input_syskeys_event(EV_KEY, KEY_MUTE));
    CHECK(input_syskeys_event(EV_SYN, SYN_REPORT));
    CHECK(!input_syskeys_event(EV_KEY, KEY_POWER));           /* logind's: never handled twice */
    CHECK(!input_syskeys_event(EV_KEY, KEY_LEFTMETA) && !input_syskeys_event(EV_KEY, KEY_A) && !input_syskeys_event(EV_MSC, MSC_SCAN));
    CHECK(input_system_key(KEY_BRIGHTNESSUP) && input_system_key(KEY_SYSRQ) && !input_system_key(KEY_SLEEP));

    /* pointer buttons (issue #159) */
    CHECK(input_dom_button(BTN_LEFT) == 0 && input_dom_button(BTN_MIDDLE) == 1 && input_dom_button(BTN_RIGHT) == 2);
    CHECK(input_dom_button(BTN_SIDE) == 3 && input_dom_button(BTN_BACK) == 3);
    CHECK(input_dom_button(BTN_EXTRA) == 4 && input_dom_button(BTN_FORWARD) == 4);
    CHECK(input_dom_button(BTN_TASK) == -1 && input_dom_button(KEY_A) == -1 && input_dom_button(BTN_TOUCH) == -1);
    CHECK(input_evdev_button(0) == BTN_LEFT && input_evdev_button(1) == BTN_MIDDLE && input_evdev_button(2) == BTN_RIGHT);
    CHECK(input_evdev_button(3) == BTN_SIDE && input_evdev_button(4) == BTN_EXTRA);
    for (int b = 0; b < 5; b++) CHECK(input_dom_button((int)input_evdev_button(b)) == b);

    /* Ctrl+Alt+F1…F12: text consoles 1 to 12; nothing else is one */
    CHECK(input_vt_key(KEY_F1) == 1 && input_vt_key(KEY_F2) == 2 && input_vt_key(KEY_F10) == 10);
    CHECK(input_vt_key(KEY_F11) == 11 && input_vt_key(KEY_F12) == 12);
    CHECK(input_vt_key(KEY_F13) == 0 && input_vt_key(KEY_A) == 0 && input_vt_key(KEY_ESC) == 0 && input_vt_key(KEY_NUMLOCK) == 0);

    puts(fails ? "inputkind: FAILED" : "inputkind: ok");
    return fails != 0;
}
