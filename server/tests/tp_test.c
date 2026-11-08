/* tp_test.c — test of the touchpad driver (src/tp/tp.c) without a device: feeds it the same
 * evdev sequences the simulated touchpad replays (tests/touchpad-sim/gestures.c), with their
 * timing, and checks what it recognizes (pointer, clicks, scroll with momentum, pinch, swipe,
 * palms, disable-while-typing, touchscreen). It also measures how long it takes to recognize each
 * gesture (in gesture time) and how much processing a frame costs.
 *
 *   build/tp-test                 all tests
 *   build/tp-test --replay F.ev   replays a recording (tpsim record/dump) and shows what came out
 *   build/tp-test --dump NAME     prints one of the bcm5974 sequences below as text (for
 *                                 tpsim play-file --bcm5974, which replays it through uinput)
 *   build/tp-test --live DEV SECS opens DEV as lord-server does (tp_open), grabs it, reads it for
 *                                 SECS seconds and prints what the driver made of it; DEV can be
 *                                 the name of a device about to appear (it's grabbed on arrival) */
#include "../src/tp/tp.h"
#include "../../tests/touchpad-sim/gestures.h"

#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { O_MOTION, O_BUTTON, O_SCROLL, O_PINCH, O_SWIPE, O_HOLD, O_TOUCH, O_WARP };
typedef struct { int kind, phase, a, b; double t, dx, dy, scale, rot; } Out;

static Out outs[20000];
static int nout;
static double cur_t;
static int touch_consume;

static void put(int kind, int phase, int a, int b, double dx, double dy, double sc, double rot) {
    if (nout < (int)(sizeof outs / sizeof outs[0])) outs[nout++] = (Out){ kind, phase, a, b, cur_t, dx, dy, sc, rot };
}
static void r_motion(void *u, double dx, double dy, double rx, double ry) { (void)u; (void)rx; (void)ry; put(O_MOTION, 0, 0, 0, dx, dy, 0, 0); }
static void r_button(void *u, int b, int d) { (void)u; put(O_BUTTON, 0, b, d, 0, 0, 0, 0); }
static void r_scroll(void *u, int ph, double dx, double dy) { (void)u; put(O_SCROLL, ph, 0, 0, dx, dy, 0, 0); }
static void r_pinch(void *u, int ph, int n, double dx, double dy, double sc, double rot) { (void)u; put(O_PINCH, ph, n, 0, dx, dy, sc, rot); }
static void r_swipe(void *u, int ph, int n, double dx, double dy) { (void)u; put(O_SWIPE, ph, n, 0, dx, dy, 0, 0); }
static void r_hold(void *u, int ph, int n) { (void)u; put(O_HOLD, ph, n, 0, 0, 0, 0, 0); }
static int r_touch(void *u, int ph, int id, double x, double y) { (void)u; put(O_TOUCH, ph, id, 0, x, y, 0, 0); return touch_consume; }
static void r_warp(void *u, double x, double y) { (void)u; put(O_WARP, 0, 0, 0, x, y, 0, 0); }
static const TpSink sink = { r_motion, r_button, r_scroll, r_pinch, r_swipe, r_hold, r_touch, NULL, r_warp, NULL };

static TpDevice *dev;
static double last_tick;
static double proc_ns, proc_frames;

static double ns_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

/* the lord-server loop: timers run between events */
static void ticks_until(double t) {
    for (;;) {
        double w = tp_timeout(last_tick);
        if (w < 0 || last_tick + w > t) break;
        last_tick += w < 1 ? 1 : w;
        cur_t = last_tick;
        tp_tick(last_tick);
    }
    last_tick = t;
}

static void feed(void *ud, double t, int type, int code, int value) {
    (void)ud;
    ticks_until(t);
    cur_t = t;
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = value;
    double t0 = ns_now();
    tp_event_at(dev, &ev, t);
    if (type == EV_SYN) { proc_ns += ns_now() - t0; proc_frames++; }
}

static TpCaps sim_caps(int kind) {
    TpCaps c;
    memset(&c, 0, sizeof c);
    c.kind = kind;
    c.clickpad = kind == TP_KIND_TOUCHPAD;
    c.nslots = SIM_SLOTS;
    c.max_fingers = 5;
    c.x = (struct input_absinfo){ .minimum = SIM_X_MIN, .maximum = SIM_X_MAX, .resolution = SIM_RES };
    c.y = (struct input_absinfo){ .minimum = SIM_Y_MIN, .maximum = SIM_Y_MAX, .resolution = SIM_RES };
    c.pressure = (struct input_absinfo){ .minimum = 0, .maximum = SIM_PMAX };
    c.has_pressure = 1;
    snprintf(c.name, sizeof c.name, "%s", SIM_NAME);
    return c;
}

static TpConfig defaults(void) {
    return (TpConfig){ .speed = 0.5, .tap = 1, .tap_drag = 1, .natural = 1, .three_drag = 0, .dwt = 1, .scroll = 1,
                       .kinetic = 1, .pinch = 1, .swipe3 = 1, .swipe4 = 0, .scroll_speed = 0.5, .sensitivity = TP_SENS_MEDIUM };
}

static double base_t = 100000;      /* each test starts later (far from the last key) */

static void run(const char *name, int kind, const TpConfig *cfg) {
    TpConfig c = cfg ? *cfg : defaults();
    tp_set_config(&c);
    TpCaps caps = sim_caps(kind);
    dev = tp_new(&caps);
    nout = 0;
    SimGen g;
    sim_init(&g, feed, NULL);
    g.t = base_t;
    last_tick = base_t;
    const SimGesture *gs = sim_find(name);
    if (!gs) { fprintf(stderr, "no gesture %s\n", name); exit(2); }
    gs->play(&g);
    ticks_until(g.t + 3000);
    base_t = g.t + 10000;
    tp_free(dev);
    dev = NULL;
}

/* ---- summaries ---- */

typedef struct { double mx, my, sx, sy, wx, wy, scale, rot, swx, swy; int nmotion, nscroll, nmom, mom_end, pinch_b, pinch_e, swipe_b, swipe_e, fingers, holds;
                 int down[3], up[3]; char btns[64]; double t_first, t_scroll, t_pinch, t_swipe; } Sum;

static Sum summary(void) {
    Sum s;
    memset(&s, 0, sizeof s);
    s.scale = 1;
    s.t_first = nout ? outs[0].t : 0;
    int nb = 0;
    for (int i = 0; i < nout; i++) {
        Out *o = &outs[i];
        switch (o->kind) {
        case O_MOTION: s.mx += o->dx; s.my += o->dy; s.nmotion++; break;
        case O_BUTTON:
            if (o->b) s.down[o->a]++; else s.up[o->a]++;
            if (nb < 60) { s.btns[nb++] = (char)("LMR"[o->a] + (o->b ? 0 : 32)); s.btns[nb] = 0; }
            break;
        case O_SCROLL:
            if (o->phase == TP_BEGIN && !s.t_scroll) s.t_scroll = o->t;
            if (o->phase == TP_MOMENTUM || o->phase == TP_MOMENTUM_END) { s.nmom++; s.wx += o->dx; s.wy += o->dy; if (o->phase == TP_MOMENTUM_END) s.mom_end++; }
            else { s.sx += o->dx; s.sy += o->dy; if (o->phase == TP_UPDATE) s.nscroll++; }
            break;
        case O_PINCH:
            if (o->phase == TP_BEGIN) { s.pinch_b++; s.t_pinch = o->t; }
            if (o->phase == TP_END) s.pinch_e++;
            if (o->phase == TP_UPDATE || o->phase == TP_END) { s.scale = o->scale; s.rot = o->rot; }
            break;
        case O_SWIPE:
            if (o->phase == TP_BEGIN) { s.swipe_b++; s.fingers = o->a; s.t_swipe = o->t; }
            if (o->phase == TP_END) s.swipe_e++;
            s.swx += o->dx;
            s.swy += o->dy;
            break;
        case O_HOLD: if (o->phase == TP_BEGIN) s.holds++; break;
        case O_WARP: s.wx = o->dx; s.wy = o->dy; break;
        }
    }
    return s;
}

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void print_sum(const char *name, const Sum *s) {
    printf("%-13s pointer %+6.0f,%+5.0f  buttons %-8s scroll %+6.0f,%+6.0f (+momentum %+6.0f, %d)  pinch %.2f %+4.0f°  swipe %d fingers %+5.0f,%+5.0f\n",
           name, s->mx, s->my, s->btns[0] ? s->btns : "-", s->sx, s->sy, s->wy, s->nmom, s->scale, s->rot, s->fingers, s->swx, s->swy);
}

static Sum go(const char *name, const TpConfig *c) { run(name, TP_KIND_TOUCHPAD, c); Sum s = summary(); print_sum(name, &s); return s; }

static void t_pointer(void) {
    Sum s = go("move", NULL);
    CHECK(s.mx > 150 && s.mx < 1200, "45 mm to the right: %.0f px", s.mx);
    CHECK(fabs(s.my) < s.mx * 0.3, "almost horizontal: %.0f", s.my);
    CHECK(!s.btns[0] && !s.nscroll, "no clicks or scroll");
    Sum b = go("move-back", NULL);
    CHECK(b.mx < -150, "to the left: %.0f", b.mx);
    /* higher speed setting = more pixels */
    TpConfig fast = defaults();
    fast.speed = 1;
    Sum f = go("move", &fast);
    CHECK(f.mx > s.mx * 1.5, "max speed moves more: %.0f vs %.0f", f.mx, s.mx);
    CHECK(tp_accel(5, 0.5) < tp_accel(100, 0.5) && tp_accel(100, 0.5) < tp_accel(300, 0.5) + 1e-9, "increasing curve");
}

static void t_taps(void) {
    Sum s = go("tap", NULL);
    CHECK(!strcmp(s.btns, "Ll"), "tap = left click (%s)", s.btns);
    s = go("tap2", NULL);
    CHECK(!strcmp(s.btns, "Rr"), "2 fingers = right (%s)", s.btns);
    s = go("tap3", NULL);
    CHECK(!strcmp(s.btns, "Mm"), "3 fingers = middle (%s)", s.btns);
    s = go("doubletap", NULL);
    CHECK(!strcmp(s.btns, "LlLl"), "double tap (%s)", s.btns);
    s = go("tapdrag", NULL);
    CHECK(!strcmp(s.btns, "Ll") && s.mx > 100, "tap and drag: %s, %.0f px with the button held", s.btns, s.mx);
    /* the button stays held during the motion */
    int down_at = -1, up_at = -1, moved_between = 0;
    for (int i = 0; i < nout; i++) {
        if (outs[i].kind == O_BUTTON && outs[i].b && down_at < 0) down_at = i;
        else if (outs[i].kind == O_BUTTON && !outs[i].b) up_at = i;
        else if (outs[i].kind == O_MOTION && down_at >= 0 && up_at < 0) moved_between++;
    }
    CHECK(moved_between > 10, "motions with the button held: %d", moved_between);
    TpConfig off = defaults();
    off.tap = 0;
    s = go("tap", &off);
    CHECK(!s.btns[0], "tap off: no click (%s)", s.btns);
}

static void t_clicks(void) {
    Sum s = go("click", NULL);
    CHECK(!strcmp(s.btns, "Ll"), "physical click with 1 finger (%s)", s.btns);
    s = go("click2", NULL);
    CHECK(!strcmp(s.btns, "Rr"), "physical click with 2 fingers = right (%s)", s.btns);
    s = go("click3", NULL);
    CHECK(!strcmp(s.btns, "Mm"), "physical click with 3 fingers = middle (%s)", s.btns);
    s = go("thumbclick", NULL);
    CHECK(!strcmp(s.btns, "Ll"), "resting thumb + index = left (%s)", s.btns);
    TpConfig area = defaults();
    area.secondary = TP_CLICK_BOTTOM_RIGHT;
    s = go("click2", &area);
    CHECK(!strcmp(s.btns, "Ll"), "corner mode: 2 fingers up = left (%s)", s.btns);
}

static void t_scroll(void) {
    Sum s = go("scroll", NULL);
    CHECK(s.nscroll > 5 && s.sy > 100, "fingers up + natural = content scrolls down: %.0f px", s.sy);
    CHECK(fabs(s.sx) < 10, "no horizontal: %.0f", s.sx);
    CHECK(s.nmom > 5 && s.wy > 50 && s.mom_end == 1, "momentum: %d events, %.0f px, end %d", s.nmom, s.wy, s.mom_end);
    CHECK(!s.nmotion && !s.btns[0], "does not move the pointer or click");
    printf("  scroll recognized %.0f ms after touch-down\n", s.t_scroll - s.t_first);
    Sum d = go("scroll-down", NULL);
    CHECK(d.sy < -100, "downwards: %.0f", d.sy);
    TpConfig trad = defaults();
    trad.natural = 0;
    Sum t = go("scroll", &trad);
    CHECK(t.sy < -100, "without natural: reversed (%.0f)", t.sy);
    Sum slow = go("scroll-slow", NULL);
    CHECK(slow.sy > 30 && slow.nmom == 0, "slow and stopped: no momentum (%d)", slow.nmom);
    TpConfig nok = defaults();
    nok.kinetic = 0;
    Sum k = go("scroll", &nok);
    CHECK(k.nmom == 0, "momentum off");
}

static void t_pinch(void) {
    Sum s = go("pinch-out", NULL);
    CHECK(s.pinch_b == 1 && s.pinch_e == 1, "pinch begin/end: %d/%d", s.pinch_b, s.pinch_e);
    CHECK(s.scale > 2.0, "spread: scale %.2f", s.scale);
    CHECK(!s.nscroll && !s.nmotion, "neither scroll nor pointer");
    printf("  pinch recognized %.0f ms after touch-down\n", s.t_pinch - s.t_first);
    s = go("pinch-in", NULL);
    CHECK(s.scale < 0.5, "pinch in: scale %.2f", s.scale);
    s = go("rotate", NULL);
    CHECK(s.pinch_b == 1 && s.rot > 45 && s.rot < 75, "rotate 60°: %.0f°", s.rot);
    CHECK(fabs(s.scale - 1) < 0.15, "rotating does not change the scale: %.2f", s.scale);
    TpConfig off = defaults();
    off.pinch = 0;
    s = go("pinch-out", &off);
    CHECK(s.pinch_b == 0, "pinch off");
}

static void t_swipe(void) {
    Sum s = go("swipe3-up", NULL);
    CHECK(s.swipe_b == 1 && s.swipe_e == 1 && s.fingers == 3 && s.swy < -100, "3 fingers up: %d fingers, %.0f", s.fingers, s.swy);
    CHECK(!s.btns[0] && !s.nmotion && !s.nscroll, "only the gesture");
    printf("  swipe recognized %.0f ms after touch-down\n", s.t_swipe - s.t_first);
    s = go("swipe3-left", NULL);
    CHECK(s.swx < -100 && fabs(s.swy) < fabs(s.swx) / 2, "3 fingers left: %.0f", s.swx);
    s = go("swipe3-right", NULL);
    CHECK(s.swx > 100, "3 fingers right: %.0f", s.swx);
    s = go("swipe3-down", NULL);
    CHECK(s.swy > 100, "3 fingers down: %.0f", s.swy);
    s = go("swipe4-up", NULL);
    CHECK(s.fingers == 4 && s.swy < -100, "4 fingers: %d", s.fingers);
    TpConfig td = defaults();
    td.three_drag = 1;
    s = go("threedrag", &td);
    CHECK(!strcmp(s.btns, "Ll") && s.mx > 100 && !s.swipe_b, "3-finger drag: %s, %.0f px", s.btns, s.mx);
}

static void t_palm(void) {
    Sum s = go("palm", NULL);
    CHECK(!s.nmotion && !s.btns[0], "palm ignored: %d motions, %s", s.nmotion, s.btns);
    s = go("edge", NULL);
    CHECK(fabs(s.my) < 5 && !s.btns[0], "still touch on the edge ignored: %.0f", s.my);
    /* disable while typing: a key just before */
    tp_key_activity(KEY_A, base_t - 100 + 0);
    double kt = base_t;
    tp_key_activity(KEY_A, kt);
    Sum d = go("move", NULL);
    CHECK(!d.nmotion, "does not move while typing: %d", d.nmotion);
    tp_key_activity(KEY_LEFTCTRL, base_t);          /* modifiers do not count */
    d = go("move", NULL);
    CHECK(d.nmotion > 0, "Ctrl does not block");
    TpConfig nodwt = defaults();
    nodwt.dwt = 0;
    tp_key_activity(KEY_A, base_t);
    d = go("move", &nodwt);
    CHECK(d.nmotion > 0, "with dwt off it does move");
}

static void t_hold(void) {
    go("scroll-slow", NULL);
    int b = 0, c = 0;
    for (int i = 0; i < nout; i++) if (outs[i].kind == O_HOLD) { if (outs[i].phase == TP_BEGIN) b++; else if (outs[i].phase == TP_CANCEL) c++; }
    CHECK(b >= 1 && c >= 1, "hold starts on touch-down and is cancelled on scrolling: %d/%d", b, c);
}

static void t_touchscreen(void) {
    touch_consume = 0;
    run("tap", TP_KIND_TOUCHSCREEN, NULL);
    Sum s = summary();
    print_sum("pantalla:tap", &s);
    CHECK(!strcmp(s.btns, "Ll") && s.wx > 0.3 && s.wx < 0.5, "tap = click where touched (%s at %.2f)", s.btns, s.wx);
    run("move", TP_KIND_TOUCHSCREEN, NULL);
    s = summary();
    print_sum("pantalla:move", &s);
    CHECK(!strcmp(s.btns, "Ll") && s.wx > 0.65, "drag: button held and the pointer follows the finger (%.2f)", s.wx);
    run("scroll", TP_KIND_TOUCHSCREEN, NULL);
    s = summary();
    print_sum("pantalla:scroll", &s);
    CHECK(s.sy > 100 && !s.btns[0], "two fingers up = content scrolls down: %.0f", s.sy);
    run("pinch-out", TP_KIND_TOUCHSCREEN, NULL);
    s = summary();
    print_sum("pantalla:pinch", &s);
    CHECK(s.scale > 2, "pinch: %.2f", s.scale);
    touch_consume = 1;                       /* there is a Wayland app with wl_touch underneath */
    run("tap", TP_KIND_TOUCHSCREEN, NULL);
    s = summary();
    int down = 0, up = 0;
    for (int i = 0; i < nout; i++) if (outs[i].kind == O_TOUCH) { if (outs[i].phase == TP_BEGIN) down++; if (outs[i].phase == TP_END) up++; }
    CHECK(down == 1 && up == 1 && !s.btns[0], "to the app: touch without pointer emulation (%d/%d, %s)", down, up, s.btns);
    touch_consume = 0;
}

/* A turned panel (Settings → Displays or automatic rotation, issue #60): the touch lands where the
 * finger is in the picture, as display_unrotate_point maps a tablet's point, and scrolling follows. */
static void t_touchscreen_turned(void) {
    touch_consume = 0;
    tp_set_rotation(0);
    run("tap", TP_KIND_TOUCHSCREEN, NULL);
    Sum s0 = summary();
    run("scroll", TP_KIND_TOUCHSCREEN, NULL);
    Sum c0 = summary();
    for (int rot = 1; rot < 4; rot++) {
        tp_set_rotation(rot);
        run("tap", TP_KIND_TOUCHSCREEN, NULL);
        Sum s = summary();
        double px = s0.wx, py = s0.wy, ex = rot == 1 ? py : rot == 2 ? 1 - px : 1 - py, ey = rot == 1 ? 1 - px : rot == 2 ? 1 - py : px;
        CHECK(!strcmp(s.btns, "Ll") && fabs(s.wx - ex) < 1e-6 && fabs(s.wy - ey) < 1e-6,
              "rotation %d: tap at (%.3f, %.3f) of the picture, expected (%.3f, %.3f)", rot, s.wx, s.wy, ex, ey);
        run("scroll", TP_KIND_TOUCHSCREEN, NULL);
        Sum c = summary();
        /* two fingers toward the sensor's top: toward the picture's left at 90° (content moves right),
         * its bottom at 180° and its right at 270° */
        double along = rot & 1 ? c.sx : c.sy, across = rot & 1 ? c.sy : c.sx, want = rot == 1 ? 1 : -1;
        CHECK(fabs(along) > 100 && fabs(across) < 0.2 * fabs(along) && along * want > 0 && fabs(c0.sy) > 100,
              "rotation %d: scroll %.0f,%.0f (upright %.0f,%.0f)", rot, c.sx, c.sy, c0.sx, c0.sy);
    }
    tp_set_rotation(0);
    run("tap", TP_KIND_TOUCHSCREEN, NULL);
    Sum s = summary();
    CHECK(fabs(s.wx - s0.wx) < 1e-6 && fabs(s.wy - s0.wy) < 1e-6, "back to 0: the same point as before (%.3f, %.3f)", s.wx, s.wy);
}

/* ---- bcm5974 touchpad (a 2015 laptop): issue #34 ----
 *
 * What bcm5974 declares: 16 slots, x -4828..5345 and y -203..6803 without a resolution, a touch
 * size (ABS_MT_TOUCH_MAJOR) and no pressure; a clickpad. Its sensor reports a finger that is
 * only hovering (small touch size), and the kernel's slot assignment can hand a finger's slot to
 * another finger in the same frame. The sequences are written by hand from that description:
 * no real device was recorded. */

#define AP_XMIN -4828
#define AP_XMAX 5345
#define AP_YMIN -203
#define AP_YMAX 6803

static TpCaps bcm_caps(void) {
    TpCaps c;
    memset(&c, 0, sizeof c);
    c.kind = TP_KIND_TOUCHPAD;
    c.clickpad = 1;
    c.nslots = 16;
    c.max_fingers = 5;
    c.x = (struct input_absinfo){ .minimum = AP_XMIN, .maximum = AP_XMAX, .fuzz = 40 };
    c.y = (struct input_absinfo){ .minimum = AP_YMIN, .maximum = AP_YMAX, .fuzz = 28 };
    c.major = (struct input_absinfo){ .minimum = 0, .maximum = 2048 };
    c.has_major = 1;
    c.bus = BUS_USB;
    c.vendor = 0x05ac;
    c.product = 0x0273;
    snprintf(c.name, sizeof c.name, "bcm5974");
    return c;
}

typedef void (*Emit)(double t, int type, int code, int value);
typedef struct { int slot, id; double x, y; int major; } Ct;      /* mm from the top-left corner */
static Emit ap_emit;
static double ap_t;
static int ap_live[16], ap_btn, ap_tool;

static int ap_ux(double mm) { return (int)lround(AP_XMIN + mm * (AP_XMAX - AP_XMIN) / 104.0); }
static int ap_uy(double mm) { return (int)lround(AP_YMIN + mm * (AP_YMAX - AP_YMIN) / 75.0); }

/* one frame (~11 ms like bcm5974): the contacts listed are down, every other slot is lifted */
static void ap_frame(int n, const Ct *c, int button) {
    int seen[16] = {0};
    for (int i = 0; i < n; i++) {
        ap_emit(ap_t, EV_ABS, ABS_MT_SLOT, c[i].slot);
        if (ap_live[c[i].slot] != c[i].id) ap_emit(ap_t, EV_ABS, ABS_MT_TRACKING_ID, c[i].id);
        ap_live[c[i].slot] = c[i].id;
        ap_emit(ap_t, EV_ABS, ABS_MT_TOUCH_MAJOR, c[i].major);
        ap_emit(ap_t, EV_ABS, ABS_MT_POSITION_X, ap_ux(c[i].x));
        ap_emit(ap_t, EV_ABS, ABS_MT_POSITION_Y, ap_uy(c[i].y));
        seen[c[i].slot] = 1;
    }
    for (int k = 0; k < 16; k++)
        if (ap_live[k] > 0 && !seen[k]) { ap_emit(ap_t, EV_ABS, ABS_MT_SLOT, k); ap_emit(ap_t, EV_ABS, ABS_MT_TRACKING_ID, -1); ap_live[k] = 0; }
    static const int tools[] = { 0, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP };
    int tool = n > 5 ? 5 : n;
    if (tool != ap_tool) {
        if (ap_tool) ap_emit(ap_t, EV_KEY, tools[ap_tool], 0);
        if (!ap_tool) ap_emit(ap_t, EV_KEY, BTN_TOUCH, 1);
        if (tool) ap_emit(ap_t, EV_KEY, tools[tool], 1);
        else ap_emit(ap_t, EV_KEY, BTN_TOUCH, 0);
        ap_tool = tool;
    }
    if (button != ap_btn) { ap_emit(ap_t, EV_KEY, BTN_LEFT, button); ap_btn = button; }
    ap_emit(ap_t, EV_SYN, SYN_REPORT, 0);
    ap_t += 11;
}
static void ap_one(int id, double x, double y, int major, int button) { Ct c = { 0, id, x, y, major }; ap_frame(1, &c, button); }

/* A light tap: the finger hovers for 130 ms while it comes down (drifting 3 mm), touches for
 * 55 ms, and hovers 45 ms going up. The tap has to count, and the hover must not move the pointer. */
static void seq_hover_tap(void) {
    for (int k = 0; k < 12; k++) ap_one(1, 40 + k * 0.25, 30 + k * 0.1, 40 + k * 8, 0);
    for (int k = 0; k < 5; k++) ap_one(1, 43, 31.2, 420, 0);
    for (int k = 0; k < 4; k++) ap_one(1, 43 + k * 0.3, 31.2, 90 - k * 10, 0);
    ap_frame(0, NULL, 0);
    ap_t += 400;
}

/* A finger hovering above the touchpad, moving 25 mm, never touching. */
static void seq_hover_move(void) {
    for (int k = 0; k < 30; k++) ap_one(1, 30 + k * 0.85, 40, 90, 0);
    ap_frame(0, NULL, 0);
    ap_t += 200;
}

/* Index finger resting; it lifts as the thumb presses in the bottom-left corner to click, and the
 * kernel keeps the index's slot (and tracking id) for the thumb: the contact "moves" 50 mm in one
 * frame. That was the pointer going to the bottom-left corner on a click. */
static void seq_click_jump(void) {
    for (int k = 0; k < 10; k++) ap_one(1, 60 + k * 0.2, 25, 500, 0);
    ap_one(1, 14, 68, 700, 1);                              /* the same slot, now the thumb */
    for (int k = 0; k < 8; k++) ap_one(1, 14 + k * 0.05, 68, 900, 1);
    for (int k = 0; k < 3; k++) ap_one(1, 14.4, 68, 600, 0);
    ap_frame(0, NULL, 0);
    ap_t += 400;
}

/* Pressing the clickpad pushes the finger 1 mm; then the finger drags 20 mm with the button held. */
static void seq_click_push(void) {
    for (int k = 0; k < 6; k++) ap_one(1, 50, 35, 500, 0);
    for (int k = 0; k < 4; k++) ap_one(1, 50 + k * 0.25, 35 + k * 0.1, 900, 1);
    for (int k = 0; k < 20; k++) ap_one(1, 51 + k * 1.0, 35.4, 900, 1);
    ap_one(1, 71, 35.4, 600, 0);
    ap_frame(0, NULL, 0);
    ap_t += 400;
}

/* Space in Files (Quick Preview), Space again to close it, and the finger touches right after
 * and moves for 0.8 s without lifting. */
static double space_at;
static void seq_space_move(void) {
    space_at = ap_t;
    ap_t += 60;
    for (int k = 0; k < 70; k++) ap_one(1, 30 + k * 0.5, 40, 500, 0);
    ap_frame(0, NULL, 0);
    ap_t += 300;
}

/* A device with 2 slots that sends a third (some firmware does): its events must not land on
 * the finger in slot 1 (that was a jump of the pointer). */
static void seq_slot_overflow(void) {
    for (int k = 0; k < 10; k++) {
        ap_emit(ap_t, EV_ABS, ABS_MT_SLOT, 1);
        if (!k) ap_emit(ap_t, EV_ABS, ABS_MT_TRACKING_ID, 7);
        ap_emit(ap_t, EV_ABS, ABS_MT_TOUCH_MAJOR, 500);
        ap_emit(ap_t, EV_ABS, ABS_MT_POSITION_X, ap_ux(50 + k * 0.3));
        ap_emit(ap_t, EV_ABS, ABS_MT_POSITION_Y, ap_uy(30));
        if (k == 5) {
            ap_emit(ap_t, EV_ABS, ABS_MT_SLOT, 5);
            ap_emit(ap_t, EV_ABS, ABS_MT_TRACKING_ID, 9);
            ap_emit(ap_t, EV_ABS, ABS_MT_POSITION_X, ap_ux(2));
            ap_emit(ap_t, EV_ABS, ABS_MT_POSITION_Y, ap_uy(74));
        }
        if (!k) { ap_emit(ap_t, EV_KEY, BTN_TOUCH, 1); ap_emit(ap_t, EV_KEY, BTN_TOOL_FINGER, 1); }
        ap_emit(ap_t, EV_SYN, SYN_REPORT, 0);
        ap_t += 11;
    }
    ap_emit(ap_t, EV_ABS, ABS_MT_SLOT, 1);
    ap_emit(ap_t, EV_ABS, ABS_MT_TRACKING_ID, -1);
    ap_emit(ap_t, EV_KEY, BTN_TOOL_FINGER, 0);
    ap_emit(ap_t, EV_KEY, BTN_TOUCH, 0);
    ap_emit(ap_t, EV_SYN, SYN_REPORT, 0);
    ap_t += 300;
}

/* a light touch (size 200, between the "on" thresholds of medium and low) moving 20 mm */
static void seq_light_move(void) {
    for (int k = 0; k < 30; k++) ap_one(1, 30 + k * 0.7, 40, 200, 0);
    ap_frame(0, NULL, 0);
    ap_t += 200;
}

static const struct { const char *name; void (*play)(void); } ap_seqs[] = {
    { "hover-tap", seq_hover_tap }, { "hover-move", seq_hover_move }, { "click-jump", seq_click_jump },
    { "click-push", seq_click_push }, { "space-move", seq_space_move }, { "slot-overflow", seq_slot_overflow },
    { "light-move", seq_light_move },
};

static void ap_reset(double t);

static void print_emit(double t, int type, int code, int value) { printf("%.1f %d %d %d\n", t, type, code, value); }

static int dump(const char *name) {
    for (size_t i = 0; i < sizeof ap_seqs / sizeof ap_seqs[0]; i++)
        if (!strcmp(ap_seqs[i].name, name)) {
            TpCaps c = bcm_caps();
            printf("# tp-test %s: bcm5974-like (written by hand, not recorded)\n", name);
            printf("# caps x %d %d %d y %d %d %d\n# caps major %d %d\n# caps id %d %d %d\n# caps name %s\n", c.x.minimum, c.x.maximum, 0,
                   c.y.minimum, c.y.maximum, 0, c.major.minimum, c.major.maximum, c.bus, c.vendor, c.product, c.name);
            ap_reset(0);
            ap_emit = print_emit;
            ap_seqs[i].play();
            return 0;
        }
    fprintf(stderr, "no sequence %s\n", name);
    return 2;
}

static void feed_emit(double t, int type, int code, int value) { feed(NULL, t, type, code, value); }

static void ap_reset(double t) {
    ap_t = t;
    ap_btn = ap_tool = 0;
    memset(ap_live, 0, sizeof ap_live);
}

static Sum ap_go(const char *name, void (*play)(void), const TpCaps *caps, const TpConfig *cfg) {
    TpConfig c = cfg ? *cfg : defaults();
    tp_set_config(&c);
    TpCaps k = caps ? *caps : bcm_caps();
    dev = tp_new(&k);
    nout = 0;
    ap_reset(base_t);
    last_tick = base_t;
    ap_emit = feed_emit;
    play();
    ticks_until(ap_t + 3000);
    base_t = ap_t + 10000;
    tp_free(dev);
    dev = NULL;
    Sum s = summary();
    print_sum(name, &s);
    return s;
}

static void t_bcm5974(void) {
    Sum s = ap_go("bcm5974:hover-tap", seq_hover_tap, NULL, NULL);
    CHECK(!strcmp(s.btns, "Ll"), "a light tap after hovering clicks (%s)", s.btns);
    CHECK(fabs(s.mx) < 3 && fabs(s.my) < 3, "the hover does not move the pointer: %.1f,%.1f", s.mx, s.my);
    s = ap_go("bcm5974:hover-move", seq_hover_move, NULL, NULL);
    CHECK(!s.nmotion && !s.btns[0], "a hovering finger is not a touch: %d motions, %s", s.nmotion, s.btns);
    s = ap_go("bcm5974:click-jump", seq_click_jump, NULL, NULL);
    CHECK(!strcmp(s.btns, "Ll"), "the click is a left click (%s)", s.btns);
    CHECK(s.mx > -30 && s.my < 30, "no jump to the bottom-left on the click: %.0f,%+.0f px", s.mx, s.my);
    s = ap_go("bcm5974:click-push", seq_click_push, NULL, NULL);
    CHECK(!strcmp(s.btns, "Ll") && s.mx > 100, "press and drag: %s, %.0f px", s.btns, s.mx);
    /* Space (Quick Preview in Files) just before the touch, then no more keys */
    TpConfig c = defaults();
    tp_set_config(&c);
    TpCaps k = bcm_caps();
    dev = tp_new(&k);
    nout = 0;
    ap_reset(base_t);
    last_tick = base_t;
    ap_emit = feed_emit;
    tp_key_activity(KEY_SPACE, base_t);
    seq_space_move();
    ticks_until(ap_t + 3000);
    base_t = ap_t + 10000;
    tp_free(dev);
    dev = NULL;
    s = summary();
    print_sum("bcm5974:space", &s);
    double first = -1;
    for (int i = 0; i < nout; i++) if (outs[i].kind == O_MOTION) { first = outs[i].t; break; }
    CHECK(s.mx > 60, "after Space the pointer moves without lifting the finger: %.0f px", s.mx);
    CHECK(first < 0 || first - space_at >= 200, "...but not right after the key: %.0f ms", first - space_at);
    CHECK(!s.btns[0], "and the released touch does not tap (%s)", s.btns);
    /* while typing (keys 150 ms apart) a resting touch stays ignored */
    c = defaults();
    tp_set_config(&c);
    dev = tp_new(&k);
    nout = 0;
    ap_reset(base_t);
    last_tick = base_t;
    for (int i = 0; i < 6; i++) tp_key_activity(KEY_A, base_t + i * 150);
    ap_t = base_t + 760;
    for (int i = 0; i < 6; i++) {          /* the palm comes down between keys */
        for (int f = 0; f < 13; f++) ap_one(1, 20 + i * 0.1, 60, 500, 0);
        tp_key_activity(KEY_A, ap_t);
    }
    ap_frame(0, NULL, 0);
    ticks_until(ap_t + 3000);
    base_t = ap_t + 10000;
    tp_free(dev);
    dev = NULL;
    s = summary();
    CHECK(!s.nmotion && !s.btns[0], "a touch made while typing stays ignored while the keys go on: %d motions", s.nmotion);
    /* slots beyond the device's */
    k = bcm_caps();
    k.nslots = 2;
    s = ap_go("bcm5974:slot-overflow", seq_slot_overflow, &k, NULL);
    double travel = 0;
    for (int i = 0; i < nout; i++) if (outs[i].kind == O_MOTION) travel += fabs(outs[i].dx) + fabs(outs[i].dy);
    CHECK(travel < 40, "an unknown slot does not move the finger in slot 1: %.0f px of travel", travel);
}

/* the motion of the click-push sequence while the press pushes the finger: pinned */
static void t_pin(void) {
    TpConfig c = defaults();
    tp_set_config(&c);
    TpCaps k = bcm_caps();
    dev = tp_new(&k);
    nout = 0;
    ap_reset(base_t);
    last_tick = base_t;
    ap_emit = feed_emit;
    for (int i = 0; i < 6; i++) ap_one(1, 50, 35, 500, 0);
    double press = ap_t;
    for (int i = 0; i < 4; i++) ap_one(1, 50 + i * 0.25, 35 + i * 0.1, 900, 1);
    for (int i = 0; i < 4; i++) ap_one(1, 50.75 - i * 0.25, 35.3, 900, 0);     /* release: back */
    ap_frame(0, NULL, 0);
    ticks_until(ap_t + 3000);
    base_t = ap_t + 10000;
    tp_free(dev);
    dev = NULL;
    double moved = 0;
    for (int i = 0; i < nout; i++) if (outs[i].kind == O_MOTION && outs[i].t >= press) moved += fabs(outs[i].dx) + fabs(outs[i].dy);
    Sum s = summary();
    print_sum("bcm5974:pin", &s);
    CHECK(moved < 1, "pressing and releasing the clickpad does not nudge the pointer: %.1f px", moved);
    CHECK(!strcmp(s.btns, "Ll"), "click (%s)", s.btns);
}

/* ---- Alps I2C touchpad of the hid-alps driver (Dell Latitude 7480 and 5289): issues #43, #54, #67 ----
 *
 * What hid-alps declares for 044E:120B (U1): 5 slots, x 0..3072 and y 0..1536 with a resolution
 * (99×47 mm in the reporter's log), ABS_MT_PRESSURE 0..64, no touch size, INPUT_PROP_POINTER and
 * BTN_LEFT/RIGHT/MIDDLE (not a clickpad). Each report carries every slot; the pressure it sends is
 * the low 7 bits of the sensor (0..127), and the T4 pads send a fixed 62 for any finger. A firm finger
 * read as a palm: the pointer only moved after touching the very edge first, and taps did nothing.
 * The sequences follow the driver's u1_raw_event; no real device was recorded. */

static TpCaps alps_caps(int vendor) {
    TpCaps c;
    memset(&c, 0, sizeof c);
    c.kind = TP_KIND_TOUCHPAD;
    c.has_right = 1;
    c.nslots = 5;
    c.max_fingers = 5;
    c.x = (struct input_absinfo){ .minimum = 0, .maximum = 3072, .resolution = 31 };
    c.y = (struct input_absinfo){ .minimum = 0, .maximum = 1536, .resolution = 32 };
    c.pressure = (struct input_absinfo){ .minimum = 0, .maximum = 64 };
    c.has_pressure = 1;
    c.bus = BUS_I2C;
    c.vendor = vendor;
    c.product = 0x120b;
    snprintf(c.name, sizeof c.name, "DLL07A0:01 044E:120B");
    return c;
}

static int al_live[5], al_next = 100, al_tool, al_btn;

/* one report of u1_raw_event (~8 ms): pressure z per slot (0 = lifted), then the pointer emulation
 * of input_mt_sync_frame, the buttons and SYN_REPORT */
static void al_frame(const double *x, const double *y, const int *z, int button) {
    int n = 0;
    for (int i = 0; i < 5; i++) {
        feed(NULL, ap_t, EV_ABS, ABS_MT_SLOT, i);
        if (z[i]) {
            if (!al_live[i]) { al_live[i] = al_next++; feed(NULL, ap_t, EV_ABS, ABS_MT_TRACKING_ID, al_live[i]); }
            feed(NULL, ap_t, EV_ABS, ABS_MT_POSITION_X, (int)lround(x[i] * 31));
            feed(NULL, ap_t, EV_ABS, ABS_MT_POSITION_Y, (int)lround(y[i] * 32));
            feed(NULL, ap_t, EV_ABS, ABS_MT_PRESSURE, z[i]);
            n++;
        } else if (al_live[i]) { al_live[i] = 0; feed(NULL, ap_t, EV_ABS, ABS_MT_TRACKING_ID, -1); }
    }
    static const int tools[] = { 0, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP };
    if (n != al_tool) {
        if (al_tool) feed(NULL, ap_t, EV_KEY, tools[al_tool], 0);
        if (!!n != !!al_tool) feed(NULL, ap_t, EV_KEY, BTN_TOUCH, n > 0);
        if (n) feed(NULL, ap_t, EV_KEY, tools[n], 1);
        al_tool = n;
    }
    if (button != al_btn) { feed(NULL, ap_t, EV_KEY, BTN_LEFT, button); al_btn = button; }
    feed(NULL, ap_t, EV_SYN, SYN_REPORT, 0);
    ap_t += 8;
}

static void al_one(double x, double y, int z, int button) {
    double xs[5] = { x }, ys[5] = { y };
    int zs[5] = { z };
    al_frame(xs, ys, zs, button);
}

static int al_z0, al_zfixed;    /* pressure of the finger: ramps up from al_z0, or al_zfixed (T4) */
static int al_z(int k) { return al_zfixed ? al_zfixed : al_z0 + k * 6 > 110 ? 110 : al_z0 + k * 6; }

/* one finger in the middle of the pad, moving 30 mm to the right */
static void al_seq_move(void) {
    for (int k = 0; k < 40; k++) al_one(30 + k * 0.75, 25, al_z(k), 0);
    al_one(60, 25, 0, 0);
    ap_t += 300;
}

/* a tap in the middle: 60 ms down */
static void al_seq_tap(void) {
    for (int k = 0; k < 8; k++) al_one(50, 22, al_z(k), 0);
    al_one(50, 22, 0, 0);
    ap_t += 400;
}

/* the physical left button under the pad, no finger on it */
static void al_seq_button(void) {
    for (int k = 0; k < 6; k++) al_one(0, 0, 0, 1);
    al_one(0, 0, 0, 0);
    ap_t += 300;
}

static Sum al_go(const char *name, void (*play)(void), int vendor) {
    TpConfig c = defaults();
    tp_set_config(&c);
    TpCaps k = alps_caps(vendor);
    dev = tp_new(&k);
    nout = 0;
    ap_t = base_t;
    last_tick = base_t;
    memset(al_live, 0, sizeof al_live);
    al_tool = al_btn = 0;
    play();
    ticks_until(ap_t + 3000);
    base_t = ap_t + 10000;
    tp_free(dev);
    dev = NULL;
    Sum s = summary();
    print_sum(name, &s);
    return s;
}

static void t_alps(void) {
    al_z0 = 40;                                     /* U1: the pressure climbs past the declared 64 */
    al_zfixed = 0;
    Sum s = al_go("alps:move", al_seq_move, 0x044e);
    CHECK(s.mx > 100 && fabs(s.my) < 20, "one finger in the middle moves the pointer: %.0f,%.0f px", s.mx, s.my);
    al_z0 = 58;                                     /* a firm finger from the start, still within 0..64 */
    s = al_go("alps:firm", al_seq_move, 0x044e);
    CHECK(s.mx > 100, "a firm finger is not a palm: %.0f px", s.mx);
    s = al_go("alps:tap", al_seq_tap, 0x044e);
    CHECK(!strcmp(s.btns, "Ll"), "a tap clicks (%s)", s.btns);
    al_zfixed = 62;                                 /* T4: every finger reports 62 */
    s = al_go("alps-t4:move", al_seq_move, 0x044e);
    CHECK(s.mx > 100, "T4: one finger moves the pointer: %.0f px", s.mx);
    s = al_go("alps-t4:tap", al_seq_tap, 0x044e);
    CHECK(!strcmp(s.btns, "Ll"), "T4: a tap clicks (%s)", s.btns);
    s = al_go("alps:button", al_seq_button, 0x044e);
    CHECK(!strcmp(s.btns, "Ll"), "the physical button clicks (%s)", s.btns);
    /* other touchpads keep pressure palms: the same range from another maker, a heavy contact within it */
    al_zfixed = 60;
    s = al_go("other:heavy", al_seq_move, 0x06cb);
    CHECK(!s.nmotion && !s.btns[0], "a heavy contact on another touchpad is still a palm: %d motions", s.nmotion);
    /* ...unless it reports a pressure above its declared maximum: then the range can't be trusted */
    al_zfixed = 0;
    al_z0 = 70;
    s = al_go("other:range", al_seq_move, 0x06cb);
    CHECK(s.mx > 100, "a pressure above the maximum stops pressure palms: %.0f px", s.mx);
    al_zfixed = 0;
}

static void t_settings(void) {
    /* scroll speed: ×2 at the top of the slider, ×0.5 at the bottom */
    Sum n = go("scroll", NULL);
    TpConfig f = defaults();
    f.scroll_speed = 1;
    Sum fast = go("scroll", &f);
    f.scroll_speed = 0;
    Sum slow = go("scroll", &f);
    CHECK(fast.sy > n.sy * 1.7 && slow.sy < n.sy * 0.6, "scroll speed: %.0f / %.0f / %.0f px", slow.sy, n.sy, fast.sy);
    /* touch sensitivity on bcm5974: a touch of size 200 counts with medium, not with low */
    TpConfig lo = defaults();
    lo.sensitivity = TP_SENS_LOW;
    Sum a = ap_go("bcm5974:light", seq_light_move, NULL, NULL);
    Sum b = ap_go("bcm5974:light-low", seq_light_move, NULL, &lo);
    CHECK(a.nmotion > 0 && !b.nmotion, "light touch: medium %d motions, low %d", a.nmotion, b.nmotion);
    /* and on a pressure touchpad, low ignores a very light touch; medium keeps the old behaviour */
    Sum p = go("move", &lo);
    CHECK(p.mx > 150, "low sensitivity still takes a normal finger: %.0f px", p.mx);
}

static int replay(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 1; }
    TpCaps caps = sim_caps(TP_KIND_TOUCHPAD);
    char line[256];
    dev = NULL;
    nout = 0;
    TpConfig c = defaults();
    tp_set_config(&c);
    while (fgets(line, sizeof line, f)) {
        int a, b, cc, d, e, g;
        if (sscanf(line, "# caps x %d %d %d y %d %d %d", &a, &b, &cc, &d, &e, &g) == 6) {
            caps.x = (struct input_absinfo){ .minimum = a, .maximum = b, .resolution = cc };
            caps.y = (struct input_absinfo){ .minimum = d, .maximum = e, .resolution = g };
            continue;
        }
        if (sscanf(line, "# caps major %d %d", &a, &b) == 2) { caps.major = (struct input_absinfo){ .minimum = a, .maximum = b }; caps.has_major = 1; continue; }
        /* newer recordings list every capability after this line: start without the simulated ones */
        if (sscanf(line, "# caps id %d %d %d", &a, &b, &cc) == 3) { caps.bus = a; caps.vendor = b; caps.product = cc; caps.has_pressure = caps.has_major = 0; continue; }
        if (sscanf(line, "# caps slots %d", &a) == 1) { caps.nslots = a; continue; }
        if (sscanf(line, "# caps pressure %d %d", &a, &b) == 2) { caps.pressure = (struct input_absinfo){ .minimum = a, .maximum = b }; caps.has_pressure = 1; continue; }
        if (!strncmp(line, "# caps name ", 12)) {
            snprintf(caps.name, sizeof caps.name, "%.79s", line + 12);
            caps.name[strcspn(caps.name, "\r\n")] = 0;
            continue;
        }
        if (line[0] == '#') continue;
        double t;
        int type, code, value;
        if (sscanf(line, "%lf %d %d %d", &t, &type, &code, &value) != 4) continue;
        if (!dev) { dev = tp_new(&caps); last_tick = t; }
        feed(NULL, t + 1000, type, code, value);
    }
    fclose(f);
    if (!dev) return 1;
    ticks_until(last_tick + 3000);
    Sum s = summary();
    print_sum(path, &s);
    return 0;
}

/* Waits for a new /dev/input/eventN called name and grabs it at once, before a session on the
 * monitor (lord-sessiond hotplug) takes it: the replayed gestures must not reach a desktop. */
static int wait_device(const char *name, char *path, size_t size) {
    int in = inotify_init1(IN_CLOEXEC);
    if (in < 0 || inotify_add_watch(in, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) { perror("inotify"); return -1; }
    for (int tries = 0; tries < 200; tries++) {
        char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
        struct pollfd p = { in, POLLIN, 0 };
        if (poll(&p, 1, 100) <= 0) continue;
        ssize_t n = read(in, buf, sizeof buf);
        for (char *q = buf; n > 0 && q < buf + n;) {
            const struct inotify_event *e = (const struct inotify_event *)q;
            q += sizeof *e + e->len;
            if (!e->len || strncmp(e->name, "event", 5)) continue;
            snprintf(path, size, "/dev/input/%s", e->name);
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            char nm[80] = "";
            ioctl(fd, EVIOCGNAME(sizeof nm), nm);
            if (strcmp(nm, name)) { close(fd); continue; }
            if (ioctl(fd, EVIOCGRAB, 1)) perror("EVIOCGRAB: another reader got it first");
            close(in);
            return fd;
        }
    }
    close(in);
    fprintf(stderr, "no new device called %s\n", name);
    return -1;
}

/* A real (or uinput) device, opened like lord-server does: a path, or the name of a device that is
 * about to appear (tpsim play-file --bcm5974: "bcm5974"). */
static int live(const char *what, double secs) {
    char path[64];
    int fd;
    if (what[0] == '/') {
        snprintf(path, sizeof path, "%s", what);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0 && ioctl(fd, EVIOCGRAB, 1)) perror("EVIOCGRAB: another reader got it first");
    } else fd = wait_device(what, path, sizeof path);
    if (fd < 0) { perror(what); return 1; }
    TpConfig c = defaults();
    tp_set_config(&c);
    char name[80] = "";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    dev = tp_open(fd, name);
    if (!dev) { fprintf(stderr, "%s: not a touchpad or touchscreen\n", path); return 1; }
    nout = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec * 1e3 + ts.tv_nsec / 1e6, end = now + secs * 1000;
    last_tick = now;
    while (now < end) {
        double w = tp_timeout(now);
        struct pollfd p = { fd, POLLIN, 0 };
        poll(&p, 1, w < 0 || w > 50 ? 50 : (int)w + 1);
        struct input_event ev;
        while (read(fd, &ev, sizeof ev) == sizeof ev) {
            double t = ev.input_event_sec * 1e3 + ev.input_event_usec / 1e3;
            ticks_until(t);
            cur_t = t;
            tp_event(dev, &ev);
        }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
        ticks_until(now);
    }
    Sum s = summary();
    print_sum(name, &s);
    return 0;
}

int main(int argc, char **argv) {
    tp_set_sink(&sink, NULL);
    tp_set_screen(1280, 800);
    if (argc == 3 && !strcmp(argv[1], "--replay")) return replay(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--dump")) return dump(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "--live")) return live(argv[2], atof(argv[3]));
    t_pointer();
    t_taps();
    t_clicks();
    t_scroll();
    t_pinch();
    t_swipe();
    t_palm();
    t_hold();
    t_touchscreen();
    t_touchscreen_turned();
    t_bcm5974();
    t_pin();
    t_alps();
    t_settings();
    printf("processing a frame: %.2f µs (average over %.0f frames)\n", proc_ns / proc_frames / 1000, proc_frames);
    printf("tp-test: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
