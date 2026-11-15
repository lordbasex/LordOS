/* inputkind.h — what makes an absolute device a pointer. X and Y alone don't: a laptop's motion sensor (applesmc, on
 * Intel MacBooks) and a joystick report them too, and read as a tablet they pull the pointer to the middle of the screen
 * at every sample. A pointer has a button or a touch, and isn't marked as an accelerometer. */
#ifndef LORD_INPUTKIND_H
#define LORD_INPUTKIND_H
#include <linux/input-event-codes.h>
#include <limits.h>

#define IK_BIT(bits, n) (((bits)[(n) / (sizeof(unsigned long) * CHAR_BIT)] >> ((n) % (sizeof(unsigned long) * CHAR_BIT))) & 1UL)

static inline int input_is_abs_pointer(const unsigned long *ev, const unsigned long *abs, const unsigned long *keys, const unsigned long *props) {
    if (!IK_BIT(ev, EV_ABS) || !IK_BIT(abs, ABS_X) || !IK_BIT(abs, ABS_Y)) return 0;
    if (IK_BIT(props, INPUT_PROP_ACCELEROMETER)) return 0;
    return IK_BIT(ev, EV_KEY) && (IK_BIT(keys, BTN_LEFT) || IK_BIT(keys, BTN_TOUCH) || IK_BIT(keys, BTN_STYLUS) || IK_BIT(keys, BTN_TOOL_PEN) || IK_BIT(keys, BTN_TOOL_FINGER));
}

/* Player, sound, screen and keyboard light, and Print Screen: the keys the desktop acts on even with an app in front
 * (keygrab_system_key in wl/keygrab.c), and the only ones a system-keys device passes on. */
static inline int input_system_key(int code) {
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

/* A device with sound or player keys and no letters or pointer buttons (issue #131): a tablet's volume buttons
 * (gpio-keys from soc_button_array, on a Chuwi Hi12 or a Teclast X98 Pro) or a keyboard's separate "Consumer
 * Control" node. It is NOT grabbed: the same gpio-keys node often carries KEY_POWER, which logind must keep
 * reading; and only input_system_key codes are passed on, so the power key never reaches the desktop twice. A
 * "Power Button" (KEY_POWER only) or a "Video Bus" (brightness only, which the kernel's ACPI video driver may
 * already act on) isn't one. Keyboards, mice and tablets are checked first: a device that is one of them stays one. */
static inline int input_is_system_keys(const unsigned long *ev, const unsigned long *keys) {
    if (!IK_BIT(ev, EV_KEY) || IK_BIT(keys, KEY_A) || IK_BIT(keys, BTN_LEFT) || IK_BIT(keys, BTN_TOUCH)) return 0;
    return IK_BIT(keys, KEY_VOLUMEUP) || IK_BIT(keys, KEY_VOLUMEDOWN) || IK_BIT(keys, KEY_MUTE) ||
           IK_BIT(keys, KEY_PLAYPAUSE) || IK_BIT(keys, KEY_NEXTSONG) || IK_BIT(keys, KEY_PREVIOUSSONG);
}

/* What a system-keys device passes on: reports and input_system_key keys, never KEY_POWER (logind's) or a tablet's
 * Windows button. */
static inline int input_syskeys_event(int type, int code) {
    return type == EV_SYN || (type == EV_KEY && input_system_key(code));
}

/* Pointer buttons in the DOM's numbering, the one the desktop's events use: 0 left, 1 middle, 2 right, 3 back, 4
 * forward; -1 for any other code. Mice send back and forward as BTN_SIDE and BTN_EXTRA (an MX Master's thumb
 * buttons, issue #159), a few as BTN_BACK and BTN_FORWARD. */
static inline int input_dom_button(int code) {
    switch (code) {
    case BTN_LEFT: return 0;
    case BTN_MIDDLE: return 1;
    case BTN_RIGHT: return 2;
    case BTN_SIDE: case BTN_BACK: return 3;
    case BTN_EXTRA: case BTN_FORWARD: return 4;
    default: return -1;
    }
}

/* What a Wayland app gets for a DOM button: BTN_SIDE and BTN_EXTRA for back and forward, as libinput sends them
 * (GTK, Qt and the browsers go back and forward on them; Xwayland makes them X buttons 8 and 9). */
static inline unsigned input_evdev_button(int dom) {
    static const unsigned code[5] = { BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE, BTN_EXTRA };
    return dom >= 0 && dom < 5 ? code[dom] : BTN_LEFT;
}
#endif
