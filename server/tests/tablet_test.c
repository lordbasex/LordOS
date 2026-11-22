/* tablet_test.c — graphics tablets from evdev (src/tablet.c): which devices are pens and pads, and the frames that
 * synthetic event sequences give — proximity in and out, the tip, pressure, tilt, distance, stylus buttons, a switch
 * from pen to eraser, a pen with no BTN_TOOL_*, SYN_DROPPED; a pad's buttons, ring, strip and dial. The sequences are
 * the ones a Wacom Intuos (wacom.ko) and a hid-generic pen send. */
#include "../src/tablet.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); fails++; } } while (0)
#define NEAR(a, b) (fabs((a) - (b)) < 1e-6)

#define L(n) ((n) / (sizeof(unsigned long) * CHAR_BIT) + 1)
static unsigned long ev[L(EV_MAX)], ab[L(ABS_MAX)], keys[L(KEY_MAX)], props[L(INPUT_PROP_MAX)], rel[L(REL_MAX)];
static void set(unsigned long *b, int n) { b[n / (sizeof(unsigned long) * CHAR_BIT)] |= 1UL << (n % (sizeof(unsigned long) * CHAR_BIT)); }
static void reset(void) { memset(ev, 0, sizeof ev); memset(ab, 0, sizeof ab); memset(keys, 0, sizeof keys); memset(props, 0, sizeof props); memset(rel, 0, sizeof rel); }
#define KIND() tablet_kind(ev, ab, keys, props, rel)

static TabFrame fr[2];
static TabPadFrame pf;
static int nframes;
static int feed(TabDev *d, int type, int code, int value) {
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = (unsigned short)type;
    e.code = (unsigned short)code;
    e.value = value;
    return tablet_event(d, &e, fr, &pf);
}
static int syn(TabDev *d) { return nframes = feed(d, EV_SYN, SYN_REPORT, 0); }

static void kinds(void) {
    reset(); set(ev, EV_KEY); set(ev, EV_ABS); set(ab, ABS_X); set(ab, ABS_Y); set(keys, BTN_TOOL_PEN); set(keys, BTN_TOUCH);
    set(keys, BTN_STYLUS); set(ab, ABS_PRESSURE);
    CHECK(KIND() == TAB_KIND_PEN);                         /* an Intuos pen */
    set(props, INPUT_PROP_DIRECT); CHECK(KIND() == TAB_KIND_PEN);   /* a pen display */
    set(ab, ABS_MT_SLOT); CHECK(KIND() == TAB_KIND_NONE);  /* multitouch: tp/tp.c's */
    reset(); set(ev, EV_KEY); set(ev, EV_ABS); set(ab, ABS_X); set(ab, ABS_Y); set(keys, BTN_STYLUS);
    CHECK(KIND() == TAB_KIND_PEN);                         /* a pen with buttons and no BTN_TOOL */
    /* a Wacom pad: numbered buttons, a ring, and the old BTN_STYLUS with X and Y the X driver looked for */
    reset(); set(ev, EV_KEY); set(ev, EV_ABS); set(ab, ABS_X); set(ab, ABS_Y); set(ab, ABS_WHEEL); set(keys, BTN_0);
    set(keys, BTN_1); set(keys, BTN_STYLUS);
    CHECK(KIND() == TAB_KIND_PAD);
    reset(); set(ev, EV_KEY); set(keys, BTN_0); set(keys, BTN_1); CHECK(KIND() == TAB_KIND_PAD);   /* buttons only (ExpressKey Remote) */
    /* none of these is a tablet */
    reset(); set(ev, EV_KEY); set(ev, EV_ABS); set(ab, ABS_X); set(ab, ABS_Y); set(keys, BTN_LEFT);
    CHECK(KIND() == TAB_KIND_NONE);                        /* QEMU's USB tablet stays a pointer (inputkind.h) */
    reset(); set(ev, EV_KEY); set(ev, EV_REL); set(rel, REL_X); set(keys, BTN_LEFT); set(keys, BTN_0); CHECK(KIND() == TAB_KIND_NONE);
    reset(); set(ev, EV_KEY); set(keys, KEY_A); set(keys, BTN_0); CHECK(KIND() == TAB_KIND_NONE);
    reset(); set(ev, EV_KEY); set(ev, EV_ABS); set(ab, ABS_X); set(ab, ABS_Y); set(keys, BTN_TOOL_PEN);
    set(props, INPUT_PROP_ACCELEROMETER); CHECK(KIND() == TAB_KIND_NONE);
    reset(); set(ev, EV_ABS); set(ab, ABS_X); set(ab, ABS_Y); CHECK(KIND() == TAB_KIND_NONE);   /* applesmc */
    reset(); set(ev, EV_KEY); set(ev, EV_ABS); set(ab, ABS_X); set(ab, ABS_Y); set(keys, BTN_TRIGGER); CHECK(KIND() == TAB_KIND_NONE);
}

static TabCaps intuos(void) {
    TabCaps c;
    memset(&c, 0, sizeof c);
    c.kind = TAB_KIND_PEN;
    snprintf(c.name, sizeof c.name, "Wacom Intuos Pro M Pen");
    c.x = (struct input_absinfo){ .minimum = 0, .maximum = 44800, .resolution = 200 };
    c.y = (struct input_absinfo){ .minimum = 0, .maximum = 29600, .resolution = 200 };
    c.pressure = (struct input_absinfo){ .minimum = 0, .maximum = 8191 };
    c.distance = (struct input_absinfo){ .minimum = 0, .maximum = 63 };
    c.tilt_x = (struct input_absinfo){ .minimum = -64, .maximum = 63, .resolution = 57 };
    c.tilt_y = (struct input_absinfo){ .minimum = -64, .maximum = 63, .resolution = 57 };
    c.caps = TAB_CAP_PRESSURE | TAB_CAP_DISTANCE | TAB_CAP_TILT;
    c.tools = TAB_TOOL_BIT(BTN_TOOL_PEN) | TAB_TOOL_BIT(BTN_TOOL_RUBBER) | TAB_TOOL_BIT(BTN_TOOL_MOUSE);
    c.has_touch = 1;
    return c;
}

static void pen(void) {
    TabCaps c = intuos();
    TabDev *d = tablet_new(&c);
    CHECK(d != NULL);
    /* hovering in: the tool, its serial, the axes */
    feed(d, EV_ABS, ABS_X, 22400); feed(d, EV_ABS, ABS_Y, 14800); feed(d, EV_ABS, ABS_DISTANCE, 40);
    feed(d, EV_ABS, ABS_TILT_X, 57); feed(d, EV_ABS, ABS_TILT_Y, -57); feed(d, EV_ABS, ABS_MISC, 0x802);
    feed(d, EV_KEY, BTN_TOOL_PEN, 1); feed(d, EV_MSC, MSC_SERIAL, 0x1234567);
    CHECK(syn(d) == 1);
    CHECK(fr[0].flags & TF_PROX_IN);
    CHECK(fr[0].flags & TF_MOTION && fr[0].flags & TF_DISTANCE && fr[0].flags & TF_TILT && fr[0].flags & TF_PRESSURE);
    CHECK(!(fr[0].flags & (TF_DOWN | TF_UP | TF_PROX_OUT | TF_BUTTONS)));
    CHECK(fr[0].tool == BTN_TOOL_PEN && fr[0].serial == 0x1234567 && fr[0].tool_id == 0x802);
    CHECK(NEAR(fr[0].x, 0.5) && NEAR(fr[0].y, 0.5));
    CHECK(fabs(fr[0].tilt_x - 57.2958) < 0.01 && fabs(fr[0].tilt_y + 57.2958) < 0.01);   /* 1 radian each way */
    CHECK(NEAR(fr[0].distance, 40.0 / 63));
    /* nothing new: no frame */
    CHECK(syn(d) == 0);
    /* the tip touches with some pressure */
    feed(d, EV_KEY, BTN_TOUCH, 1); feed(d, EV_ABS, ABS_PRESSURE, 4096); feed(d, EV_ABS, ABS_DISTANCE, 0);
    CHECK(syn(d) == 1);
    CHECK(fr[0].flags & TF_DOWN && fr[0].flags & TF_PRESSURE && fr[0].flags & TF_DISTANCE && fr[0].down);
    CHECK(!(fr[0].flags & (TF_MOTION | TF_TILT | TF_PROX_IN)));
    CHECK(fabs(fr[0].pressure - 4096.0 / 8191) < 1e-9);
    /* drawing: motion and pressure only */
    feed(d, EV_ABS, ABS_X, 44800); feed(d, EV_ABS, ABS_PRESSURE, 8191);
    CHECK(syn(d) == 1 && fr[0].flags == (TF_MOTION | TF_PRESSURE) && NEAR(fr[0].x, 1) && NEAR(fr[0].pressure, 1));
    /* the lower stylus button, then the upper */
    feed(d, EV_KEY, BTN_STYLUS, 1);
    CHECK(syn(d) == 1 && fr[0].flags == TF_BUTTONS && fr[0].nbtn == 1 && fr[0].btn[0].code == BTN_STYLUS && fr[0].btn[0].down);
    feed(d, EV_KEY, BTN_STYLUS2, 1);
    CHECK(syn(d) == 1 && fr[0].nbtn == 1 && fr[0].btn[0].code == BTN_STYLUS2 && fr[0].btn[0].down);
    feed(d, EV_KEY, BTN_STYLUS2, 0);
    CHECK(syn(d) == 1 && fr[0].nbtn == 1 && fr[0].btn[0].code == BTN_STYLUS2 && !fr[0].btn[0].down);
    /* lifted from the tablet with the tip and a button still down: up, the button released, proximity out */
    feed(d, EV_KEY, BTN_TOOL_PEN, 0); feed(d, EV_ABS, ABS_PRESSURE, 0); feed(d, EV_ABS, ABS_X, 0);
    CHECK(syn(d) == 1);
    CHECK(fr[0].flags & TF_PROX_OUT && fr[0].flags & TF_UP && fr[0].flags & TF_BUTTONS);
    CHECK(!(fr[0].flags & (TF_MOTION | TF_PRESSURE)));     /* what came with the exit is not a move */
    CHECK(fr[0].nbtn == 1 && fr[0].btn[0].code == BTN_STYLUS && !fr[0].btn[0].down && fr[0].tool == BTN_TOOL_PEN);
    feed(d, EV_KEY, BTN_TOUCH, 0); feed(d, EV_KEY, BTN_STYLUS, 0);
    CHECK(syn(d) == 0);                                    /* nothing in proximity: nothing to say */
    /* back in with the button and the tip already down: they come with proximity_in */
    feed(d, EV_KEY, BTN_STYLUS, 1); feed(d, EV_KEY, BTN_TOUCH, 1); feed(d, EV_KEY, BTN_TOOL_PEN, 1); feed(d, EV_ABS, ABS_PRESSURE, 100);
    CHECK(syn(d) == 1 && fr[0].flags & TF_PROX_IN && fr[0].flags & TF_DOWN && fr[0].flags & TF_BUTTONS && fr[0].down);
    CHECK(fr[0].nbtn == 1 && fr[0].btn[0].code == BTN_STYLUS && fr[0].btn[0].down);
    /* turned over to the eraser in one report: the pen leaves, then the eraser comes in */
    feed(d, EV_KEY, BTN_TOOL_PEN, 0); feed(d, EV_KEY, BTN_TOOL_RUBBER, 1); feed(d, EV_KEY, BTN_STYLUS, 0);
    CHECK(syn(d) == 2);
    CHECK(fr[0].tool == BTN_TOOL_PEN && fr[0].flags & TF_PROX_OUT && fr[0].flags & TF_UP);
    CHECK(fr[1].tool == BTN_TOOL_RUBBER && fr[1].flags & TF_PROX_IN && fr[1].flags & TF_DOWN && fr[1].nbtn == 0);
    /* the session leaves the screen with the eraser down */
    CHECK(tablet_leave(d, &fr[0]) == 1 && fr[0].flags & TF_PROX_OUT && fr[0].flags & TF_UP && fr[0].tool == BTN_TOOL_RUBBER);
    CHECK(tablet_leave(d, &fr[0]) == 0);
    /* SYN_DROPPED: up to the next report, nothing counts */
    feed(d, EV_KEY, BTN_TOUCH, 0); feed(d, EV_KEY, BTN_TOOL_RUBBER, 0); syn(d);
    feed(d, EV_SYN, SYN_DROPPED, 0); feed(d, EV_KEY, BTN_TOOL_PEN, 1); feed(d, EV_ABS, ABS_X, 100);
    CHECK(syn(d) == 0);
    feed(d, EV_ABS, ABS_X, 200);
    CHECK(syn(d) == 0);                                    /* the BTN_TOOL_PEN press was lost with the drop */
    /* a mouse tool has no pressure or tilt here */
    CHECK(tablet_tool_caps(d, BTN_TOOL_MOUSE) == TAB_CAP_DISTANCE && tablet_tool_caps(d, BTN_TOOL_PEN) == c.caps);
    tablet_free(d);

    /* a pen with no BTN_TOOL_* and no BTN_TOUCH (a cheap hid-generic one): in on its first report, the tip by
     * pressure over 5 % */
    TabCaps g;
    memset(&g, 0, sizeof g);
    g.kind = TAB_KIND_PEN;
    g.x = (struct input_absinfo){ .minimum = 0, .maximum = 1000 };
    g.y = (struct input_absinfo){ .minimum = 0, .maximum = 1000 };
    g.pressure = (struct input_absinfo){ .minimum = 0, .maximum = 1023 };
    g.caps = TAB_CAP_PRESSURE;
    d = tablet_new(&g);
    feed(d, EV_ABS, ABS_X, 250);
    CHECK(syn(d) == 1 && fr[0].flags & TF_PROX_IN && fr[0].tool == BTN_TOOL_PEN && NEAR(fr[0].x, 0.25) && !fr[0].down);
    feed(d, EV_ABS, ABS_PRESSURE, 40);
    CHECK(syn(d) == 1 && fr[0].flags == TF_PRESSURE);      /* 4 %: not yet down */
    feed(d, EV_ABS, ABS_PRESSURE, 300);
    CHECK(syn(d) == 1 && fr[0].flags & TF_DOWN && fr[0].down);
    feed(d, EV_ABS, ABS_PRESSURE, 0);
    CHECK(syn(d) == 1 && fr[0].flags & TF_UP && !fr[0].down);
    tablet_free(d);

    /* tilt with no resolution: Wacom's -64…64 degrees over the range */
    TabCaps t = intuos();
    t.tilt_x = (struct input_absinfo){ .minimum = 0, .maximum = 127 };
    t.tilt_y = (struct input_absinfo){ .minimum = 0, .maximum = 127 };
    d = tablet_new(&t);
    feed(d, EV_KEY, BTN_TOOL_PEN, 1); feed(d, EV_ABS, ABS_TILT_X, 127); feed(d, EV_ABS, ABS_TILT_Y, 0);
    CHECK(syn(d) == 1 && NEAR(fr[0].tilt_x, 64) && NEAR(fr[0].tilt_y, -64));
    tablet_free(d);
    CHECK(tablet_new(NULL) == NULL);
}

static void pad(void) {
    TabCaps c;
    memset(&c, 0, sizeof c);
    c.kind = TAB_KIND_PAD;
    c.nbuttons = 3;
    c.button[0] = BTN_0; c.button[1] = BTN_1; c.button[2] = BTN_2;
    c.nrings = 1; c.ring_code[0] = ABS_WHEEL; c.ring[0] = (struct input_absinfo){ .minimum = 0, .maximum = 71 };
    c.nstrips = 1; c.strip_code[0] = ABS_RX; c.strip[0] = (struct input_absinfo){ .minimum = 0, .maximum = 4096 };
    c.ndials = 1; c.dial_code[0] = REL_WHEEL;
    TabDev *d = tablet_new(&c);
    feed(d, EV_KEY, BTN_1, 1);
    CHECK(syn(d) == 1 && pf.nbtn == 1 && pf.btn[0].index == 1 && pf.btn[0].down);
    feed(d, EV_KEY, BTN_1, 0); feed(d, EV_KEY, BTN_2, 1);
    CHECK(syn(d) == 1 && pf.nbtn == 2 && pf.btn[0].index == 1 && !pf.btn[0].down && pf.btn[1].index == 2 && pf.btn[1].down);
    feed(d, EV_KEY, BTN_9, 1);                             /* not one of its buttons */
    CHECK(syn(d) == 0);
    /* the ring: the kernel's 18 of 72 is a quarter turn from its left, which is north */
    feed(d, EV_ABS, ABS_WHEEL, 18);
    CHECK(syn(d) == 1 && pf.ring_set == 1 && NEAR(pf.ring[0], 0));
    feed(d, EV_ABS, ABS_WHEEL, 36);
    CHECK(syn(d) == 1 && NEAR(pf.ring[0], 90));
    feed(d, EV_ABS, ABS_WHEEL, 0);
    CHECK(syn(d) == 1 && pf.ring_stop == 1 && !pf.ring_set);
    /* the strip: one bit per position; 0 when the finger lifts */
    feed(d, EV_ABS, ABS_RX, 1);
    CHECK(syn(d) == 1 && pf.strip_set == 1 && NEAR(pf.strip[0], 0));
    feed(d, EV_ABS, ABS_RX, 4096);
    CHECK(syn(d) == 1 && NEAR(pf.strip[0], 1));
    feed(d, EV_ABS, ABS_RX, 64);
    CHECK(syn(d) == 1 && NEAR(pf.strip[0], 0.5));
    feed(d, EV_ABS, ABS_RX, 0);
    CHECK(syn(d) == 1 && pf.strip_stop == 1);
    /* the dial: detents in 1/120 */
    feed(d, EV_REL, REL_WHEEL, -2);
    CHECK(syn(d) == 1 && pf.dial[0] == -240);
    CHECK(syn(d) == 0);
    tablet_free(d);
    /* a high-resolution dial: those values, the whole detents ignored */
    c.dial_hires = 1;
    d = tablet_new(&c);
    feed(d, EV_REL, REL_WHEEL_HI_RES, 30); feed(d, EV_REL, REL_WHEEL, 1);
    CHECK(syn(d) == 1 && pf.dial[0] == 30);
    tablet_free(d);
}

int main(void) {
    kinds();
    pen();
    pad();
    puts(fails ? "tablet: FAILED" : "tablet: ok");
    return fails != 0;
}
