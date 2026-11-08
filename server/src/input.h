/* input.h — keyboard and mouse read straight from evdev (/dev/input/event*), without libinput. */
#ifndef LORD_INPUT_H
#define LORD_INPUT_H

#include <linux/input.h>

#define INPUT_MAX_DEVICES 32

typedef struct {
    int fd;
    int has_abs;                  /* tablet / touch: absolute coordinates */
    struct input_absinfo ax, ay;
    char name[80];
    struct TpDevice *tp;          /* touchpad or touchscreen (tp/tp.c), or NULL */
    int wheel_hires, hwheel_hires;  /* high-resolution wheel (REL_*WHEEL_HI_RES): that one is sent */
    int keyboard;                 /* a full keyboard (osk.h), not a power button or a token */
    int bus;                      /* input_id.bustype */
    int typed;                    /* a letter key was pressed on it since it appeared */
    int leds;                     /* the lock lights it has (bits LED_NUML, LED_CAPSL, LED_SCROLLL) */
    int led_state;                /* the lights last written to it, -1 none yet */
} InputDevice;

typedef struct {
    InputDevice dev[INPUT_MAX_DEVICES];
    int count;
} Input;

typedef void (*InputHandler)(void *ud, const InputDevice *dev, const struct input_event *ev);

int  input_open(Input *in);     /* opens and grabs (EVIOCGRAB) the useful devices */
/* Same, with fds already opened and passed by lord-sessiond (useless ones are closed). */
int  input_open_fds(Input *in, const int *fds, int n);
void input_close(Input *in);
/* Plugged in later (lord-sessiond, RP_INPUT_ADD): added to the existing ones. Returns how many were usable. */
int  input_add_fds(Input *in, const int *fds, int n);
/* A device that disappeared (ENODEV, unplugged): only that one is closed. */
void input_remove(Input *in, int idx);
void input_dispatch(Input *in, int idx, InputHandler cb, void *ud);
/* Is a keyboard someone can type on attached? (the on-screen keyboard's Automatic mode, osk.h) */
int  input_has_keyboard(const Input *in);
/* Lights Caps Lock, Num Lock and Scroll Lock (bits LED_*) on every keyboard that has them, writing only to the ones
 * whose lights differ. We hold the devices with EVIOCGRAB, so the kernel's console no longer lights them. */
void input_set_leds(Input *in, int mask);

#endif
