/* inputkind_test.c — an absolute device is a pointer only with a button or a touch (src/inputkind.h, issue #2). */
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
    puts(fails ? "inputkind: FAILED" : "inputkind: ok");
    return fails != 0;
}
