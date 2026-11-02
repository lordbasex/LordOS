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
#endif
