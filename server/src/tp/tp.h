/* tp.h — our own driver for touchpads and touchscreens (evdev multitouch, protocol B), without
 * libinput. See docs/TRACKPAD.md.
 *
 * tp.c is pure logic: it receives a device's evdev events (with their time) and produces
 * pointer motion, buttons, pixel-exact scroll (and inertia), gestures (pinch,
 * 3/4-finger swipe, hold) and direct touches. What it produces goes to a TpSink: in
 * lord-server it's tp_route.c (which splits it between the UI and the Wayland apps); in the tests
 * (tests/tp_test.c) it's a recorder. */
#ifndef LORD_TP_H
#define LORD_TP_H

#include <linux/input.h>

enum { TP_KIND_NONE = 0, TP_KIND_TOUCHPAD = 1, TP_KIND_TOUCHSCREEN = 2 };

/* phases of a gesture or of a scroll */
enum { TP_BEGIN = 1, TP_UPDATE = 2, TP_END = 3, TP_CANCEL = 4, TP_MOMENTUM = 5, TP_MOMENTUM_END = 6 };

/* secondary click (physical click on a clickpad) */
enum { TP_CLICK_TWO_FINGERS = 0, TP_CLICK_BOTTOM_RIGHT = 1, TP_CLICK_BOTTOM_LEFT = 2 };

/* touch sensitivity: how firm a touch must be to count, and how eager palm rejection is */
enum { TP_SENS_LOW = 0, TP_SENS_MEDIUM = 1, TP_SENS_HIGH = 2 };

typedef struct {
    double speed;           /* 0..1, pointer speed (0.5 = normal) */
    double mouse_speed;     /* 0..1, a mouse's speed (0.5 = as it comes) */
    int mouse_natural;      /* a mouse's wheel the other way */
    int mouse_left_handed;  /* a mouse's primary button is the right one */
    double scroll_speed;    /* 0..1, two-finger scroll speed (0.5 = normal) */
    int sensitivity;        /* TP_SENS_* */
    int tap;                /* tap to click (1/2/3 fingers = left/right/middle) */
    int tap_drag;           /* tap and drag (tap, and put the finger back down right away) */
    int natural;            /* natural scrolling (the content follows the fingers) */
    int secondary;          /* TP_CLICK_* */
    int three_drag;         /* three-finger drag */
    int dwt;                /* ignore the touchpad while typing */
    int scroll;             /* two-finger scroll */
    int kinetic;            /* inertia on release */
    int pinch;              /* pinch to zoom (and rotate) */
    int swipe3;             /* 3-finger swipe: desktop actions */
    int swipe4;             /* 4-finger swipe: desktop actions (otherwise, for the apps) */
} TpConfig;

/* What the driver produces. dx/dy in screen pixels; scale relative to the start of the
 * pinch, rotation accumulated in degrees (clockwise +). x/y of direct touches in 0..1. */
typedef struct {
    void (*motion)(void *ud, double dx, double dy, double raw_dx, double raw_dy);
    void (*button)(void *ud, int button, int down);         /* 0 left, 1 middle, 2 right */
    void (*scroll)(void *ud, int phase, double dx, double dy);
    void (*pinch)(void *ud, int phase, int fingers, double dx, double dy, double scale, double rotation);
    void (*swipe)(void *ud, int phase, int fingers, double dx, double dy);
    void (*hold)(void *ud, int phase, int fingers);
    /* touchscreen: 1 if a Wayland app took the touch (TP_BEGIN); then the pointer isn't emulated */
    int  (*touch)(void *ud, int phase, int id, double x, double y);
    void (*touch_frame)(void *ud);
    void (*warp)(void *ud, double x, double y);              /* absolute pointer (0..1), touchscreen */
    void (*frame)(void *ud);                                 /* there was output from tp_tick */
} TpSink;

/* Description of a device (tp_open builds it with ioctl, or the tests by hand). */
typedef struct {
    int kind;
    int clickpad;                       /* INPUT_PROP_BUTTONPAD: the whole touchpad is the button */
    int has_right;                      /* has a physical right button */
    int nslots;
    int max_fingers;                    /* from BTN_TOOL_*TAP (can be more than nslots) */
    struct input_absinfo x, y, pressure, major;
    int has_pressure, has_major;
    int bus, vendor, product;           /* struct input_id (EVIOCGID) */
    char name[80];
} TpCaps;

typedef struct TpDevice TpDevice;

const TpConfig *tp_config(void);
void tp_set_config(const TpConfig *c);
void tp_set_sink(const TpSink *s, void *ud);
void tp_set_screen(int w, int h);       /* for touchscreens (mm → pixels) */

/* Looks at the fd: if it's a touchpad or touchscreen it returns the device (and sets the monotonic
 * clock on its events); otherwise, NULL. */
TpDevice *tp_open(int fd, const char *name);
TpDevice *tp_new(const TpCaps *caps);
void tp_free(TpDevice *d);
int  tp_kind(const TpDevice *d);
const TpCaps *tp_caps(const TpDevice *d);

void tp_event(TpDevice *d, const struct input_event *ev);
void tp_event_at(TpDevice *d, const struct input_event *ev, double now_ms);   /* tests */
/* Keyboard: for "ignore while typing" (modifier keys don't count). */
void tp_key_activity(int code, double now_ms);

/* After dispatching a frame (SYN_REPORT): measures from the event's time. When the fingers
 * lift it writes one line per gesture to the log (stderr): frames, average and maximum latency. */
void tp_latency(TpDevice *d, const struct input_event *syn);

/* Timers (tap, inertia, three-finger drag): ms until the next one, or -1. */
double tp_timeout(double now_ms);
void   tp_tick(double now_ms);

/* Open devices: how many of each type, and the list as JSON for lord.input. */
int tp_count(int kind);
int tp_list_json(char *out, int size);

/* Acceleration curve (px per mm depending on the speed in mm/s): used by tp.c and the test. */
double tp_accel(double speed_mm_s, double setting);

#endif
