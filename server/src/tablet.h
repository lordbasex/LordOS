/* tablet.h — graphics tablets read straight from evdev, without libinput or libwacom (docs/WAYLAND.md, "Graphics
 * tablets"). A pen device (BTN_TOOL_PEN or BTN_STYLUS with ABS_X/ABS_Y) reports its tools — pen, eraser, brush,
 * pencil, airbrush, mouse, lens — with pressure, distance, tilt, rotation and slider when it has those axes, and its
 * stylus buttons. A pad (BTN_0… with no pen) reports its buttons, rings (ABS_WHEEL, ABS_THROTTLE), strips (ABS_RX,
 * ABS_RY) and dials (REL_WHEEL, REL_HWHEEL).
 *
 * tablet.c is pure logic apart from tablet_open: it turns one device's evdev events into frames, one per SYN_REPORT.
 * lord-server sends them to the Wayland apps by tablet-v2 (wl/wl_tablet.c) and, where no app takes them, uses a pen as
 * a mouse (main.c); the tests (tests/tablet_test.c) feed it synthetic sequences. */
#ifndef LORD_TABLET_H
#define LORD_TABLET_H

#include <linux/input.h>
#include <stdint.h>

enum { TAB_KIND_NONE = 0, TAB_KIND_PEN = 1, TAB_KIND_PAD = 2 };

/* A tool's type is its BTN_TOOL_* code, as tablet-v2's zwp_tablet_tool_v2.type is. */
#define TAB_TOOL_FIRST BTN_TOOL_PEN
#define TAB_TOOL_LAST  BTN_TOOL_LENS
#define TAB_TOOL_BIT(code) (1u << ((code) - TAB_TOOL_FIRST))

/* what a pen device's tools may report beyond x, y and the tip */
enum { TAB_CAP_PRESSURE = 1, TAB_CAP_DISTANCE = 2, TAB_CAP_TILT = 4, TAB_CAP_ROTATION = 8, TAB_CAP_SLIDER = 16 };

#define TAB_PAD_BUTTONS 32
#define TAB_PAD_RINGS   2
#define TAB_PAD_STRIPS  2
#define TAB_PAD_DIALS   2

/* A device's description: tablet_open fills it with ioctl, the tests by hand. */
typedef struct {
    int kind;                           /* TAB_KIND_* */
    char name[80];
    char path[64];                      /* "/dev/input/eventN" when known, else "" */
    char phys[64];                      /* EVIOCGPHYS up to its last '/': a pen and its pad share it */
    unsigned bus, vendor, product;
    int direct;                         /* INPUT_PROP_DIRECT: a pen display (or a pen on a touch screen) */
    /* pen */
    struct input_absinfo x, y, pressure, distance, tilt_x, tilt_y, rotation, slider;
    unsigned caps;                      /* TAB_CAP_* */
    unsigned tools;                     /* TAB_TOOL_BIT of each BTN_TOOL_* it has; 0: none, the pen is always in */
    int has_touch;                      /* BTN_TOUCH (the tip); without it the tip is pressure over 5 % */
    /* pad */
    int nbuttons;
    unsigned short button[TAB_PAD_BUTTONS];    /* evdev codes, in the order of their pad index */
    int nrings, nstrips, ndials;
    unsigned short ring_code[TAB_PAD_RINGS], strip_code[TAB_PAD_STRIPS], dial_code[TAB_PAD_DIALS];
    struct input_absinfo ring[TAB_PAD_RINGS], strip[TAB_PAD_STRIPS];
    int dial_hires;                     /* REL_WHEEL_HI_RES: those are sent (1/120 of a detent) */
} TabCaps;

/* What changed in a pen frame (TabFrame.flags) */
enum {
    TF_PROX_IN = 1, TF_PROX_OUT = 2, TF_DOWN = 4, TF_UP = 8, TF_MOTION = 16, TF_PRESSURE = 32, TF_DISTANCE = 64,
    TF_TILT = 128, TF_ROTATION = 256, TF_SLIDER = 512, TF_BUTTONS = 1024,
};
#define TAB_FRAME_BUTTONS 6
/* One pen frame. On TF_PROX_IN every axis the tool has is set (and flagged), plus TF_DOWN and button presses for what
 * is already held; a TF_PROX_OUT frame releases what was held (TF_UP, button releases) first. */
typedef struct {
    unsigned flags;
    unsigned tool;                      /* BTN_TOOL_* */
    uint64_t serial;                    /* MSC_SERIAL (a Wacom tool's own), 0 if the device has none */
    unsigned tool_id;                   /* ABS_MISC (Wacom's tool id), 0 if none */
    double x, y;                        /* 0..1 across the tablet's area */
    double pressure, distance;          /* 0..1 */
    double tilt_x, tilt_y;              /* degrees, -90..90 */
    double rotation;                    /* degrees clockwise, 0..360 */
    double slider;                      /* -1..1 */
    int down;                           /* the tip touches, after this frame */
    int nbtn;
    struct { unsigned short code; unsigned char down; } btn[TAB_FRAME_BUTTONS];   /* BTN_STYLUS… changes */
} TabFrame;

/* One pad frame: button changes (pad indices), each ring's and strip's new value or its stop (the finger lifted),
 * each dial's turn in 1/120 of a detent. */
typedef struct {
    int nbtn;
    struct { unsigned char index, down; } btn[TAB_PAD_BUTTONS];
    unsigned ring_set, ring_stop, strip_set, strip_stop;   /* bit i: ring/strip i */
    double ring[TAB_PAD_RINGS];         /* degrees clockwise from north, 0..360 */
    double strip[TAB_PAD_STRIPS];       /* 0..1, top or left first */
    int dial[TAB_PAD_DIALS];
} TabPadFrame;

typedef struct TabDev TabDev;

/* Which kind of tablet device these capability bits describe (TAB_KIND_NONE: not one). A pad is checked first: an
 * old Wacom pad also announces BTN_STYLUS and ABS_X/ABS_Y. A device with multitouch slots is a touch screen or a
 * touchpad (tp/tp.c), never a pen; a mouse (REL_X with BTN_LEFT) or a keyboard (KEY_A) is never a pad. */
int tablet_kind(const unsigned long *ev, const unsigned long *abs, const unsigned long *keys, const unsigned long *props,
                const unsigned long *rel);

TabDev *tablet_new(const TabCaps *c);
/* A real device: NULL if it is no tablet. Its host's added() is told (tablet_announce); tablet_free tells removed(). */
TabDev *tablet_open(int fd, const char *name);
void tablet_announce(TabDev *d);        /* tells the host once (tablet_open does; the tests for tablet_new's) */
void tablet_free(TabDev *d);
const TabCaps *tablet_caps(const TabDev *d);
/* Feeds one event. On SYN_REPORT: a pen gives 0, 1 or 2 frames in out (2 when the tool changed: the old one leaves
 * proximity, the new one enters), a pad 1 if *pad holds something; other events give 0. */
int tablet_event(TabDev *d, const struct input_event *ev, TabFrame out[2], TabPadFrame *pad);
/* The device is going away or the session leaves the screen: the frame that takes the tool out of proximity (1), or
 * 0 if none was in. */
int tablet_leave(TabDev *d, TabFrame *out);
/* The capabilities a tool of this type has on this device (TAB_CAP_*): a mouse or a lens has no pressure or tilt. */
unsigned tablet_tool_caps(const TabDev *d, unsigned tool);

/* lord-server's: told when a device is opened and freed (wl/wl_tablet.c announces it to the apps). */
typedef struct {
    void (*added)(TabDev *d);
    void (*removed)(TabDev *d);
} TabHost;
void tablet_set_host(const TabHost *h);
/* an opaque value for the host (the Wayland side's record of the device) */
void  tablet_set_user(TabDev *d, void *u);
void *tablet_user(const TabDev *d);

#endif
