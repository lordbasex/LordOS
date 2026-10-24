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
} InputDevice;

typedef struct {
    InputDevice dev[INPUT_MAX_DEVICES];
    int count;
} Input;

typedef void (*InputHandler)(void *ud, const InputDevice *dev, const struct input_event *ev);

int  input_open(Input *in);     /* opens and grabs (EVIOCGRAB) the useful devices */
int  input_watch_fd(void);      /* inotify: standalone session hotplug */
int  input_watch_dispatch(Input *in, int active);
/* Same, with fds already opened and passed by lord-sessiond (useless ones are closed). */
int  input_open_fds(Input *in, const int *fds, int n);
void input_close(Input *in);
/* Plugged in later (lord-sessiond, RP_INPUT_ADD): added to the existing ones. Returns how many were usable. */
int  input_add_fds(Input *in, const int *fds, int n);
/* A device that disappeared (ENODEV, unplugged): only that one is closed. */
void input_remove(Input *in, int idx);
void input_dispatch(Input *in, int idx, InputHandler cb, void *ud);

#endif
