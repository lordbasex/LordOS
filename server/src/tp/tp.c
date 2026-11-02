/* tp.c — touchpads and touchscreens read from evdev (multitouch, protocol B), without libinput.
 *
 * On each SYN_REPORT the state of the contacts (slots) is looked at in millimeters (with the
 * absinfo resolution) and a state machine decides what it is:
 *
 *   1 finger         move the pointer (smoothed and with tp_accel's acceleration curve)
 *   2 fingers        scroll (if they move together) or pinch (if they spread, close or rotate)
 *   3 fingers        swipe (desktop actions) or three-finger drag
 *   4 fingers        swipe (desktop or apps, depending on the setting)
 *   tap              click (1 finger left, 2 right, 3 middle), tap and drag
 *   physical click   clickpad: depending on how many fingers there are or the bottom corner
 *
 * Palms are discarded (large contact, high pressure, touches that start at the edges and don't
 * leave them) as are touches that start while typing. On a clickpad, a finger that
 * rests at the bottom (the thumb that presses) doesn't count while there are others.
 *
 * When released, a fast scroll keeps going on its own (inertia) with phase TP_MOMENTUM; Wayland apps do their
 * own inertia (they receive axis_stop), so the one here is for the LordOS UI.
 *
 * A touchscreen (INPUT_PROP_DIRECT) sends touches to the Wayland app underneath
 * (wl_touch); if there is none, it emulates the pointer: tap = click, drag = drag, two fingers =
 * scroll and pinch. */
#include "tp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

#define MAX_SLOTS 10
#define MAX_DEVS 16

/* times (ms) and distances (mm) */
#define TAP_TIME        200.0
#define TAP_DRAG_TIME   180.0
#define TAP_MOVE        1.5
#define EDGE_TIME       300.0
#define DWT_TIME        500.0
#define DRAG3_RELEASE   500.0
#define DECIDE_MOVE     1.0      /* each finger, to decide scroll or pinch */
#define SWIPE_START     3.0
#define KINETIC_MIN     200.0    /* px/s */
#define KINETIC_STOP    25.0
#define KINETIC_TAU     325.0    /* ms: deceleration constant */
#define TS_PRESS_TIME   90.0     /* touchscreen: the button is pressed if the finger stays */

typedef struct {
    int id, x, y, major, pressure;
    int down, was;
    double mx, my, pmx, pmy, sx, sy, t0;
    int palm, edge, thumb, dwt;
} Slot;

enum { G_NONE, G_POINTER, G_UND2, G_SCROLL, G_PINCH, G_UNDN, G_SWIPE, G_DRAG3 };
enum { T_IDLE, T_TOUCH, T_TAPPED, T_DRAG_OR_DOUBLE, T_DRAGGING, T_DEAD };

struct TpDevice {
    TpCaps c;
    int fd;
    double rx, ry, wmm, hmm;        /* units per mm, size in mm */
    Slot s[MAX_SLOTS];
    int cur, tool, dropped;
    int phys, phys_prev, phys_btn;  /* clickpad physical click and the button that was sent */
    int btn_pending[3], btn_state[3];   /* physical buttons of a touchpad with buttons */
    double now, prev_t;
    /* gestures */
    int g, gn;
    double g_t0;
    int bi, bj;
    double b1x, b1y, b2x, b2y, ccx, ccy;    /* start of the gesture (mm) */
    double dist0, last_ang, rot, pscale;
    double vel, sdx, sdy;
    int hold;
    double hold_move;
    double ht[8], hx[8], hy[8];
    int hn, hi;
    int kin;
    double kvx, kvy, kt;
    int d3_down;
    double d3_release;
    /* tap */
    int tap, tap_max, tap_down;
    double tap_t, tap_deadline;
    /* latency */
    int lat_n, lat_kind;
    double lat_sum, lat_max;
    /* touchscreen */
    int ts_app, ts_multi, ts_pending, ts_btn, ts_prev_n;
    double ts_t0;
};

static TpConfig cfg = { .speed = 0.5, .mouse_speed = 0.5, .tap = 1, .tap_drag = 1, .natural = 1, .secondary = TP_CLICK_TWO_FINGERS,
                        .three_drag = 0, .dwt = 1, .scroll = 1, .kinetic = 1, .pinch = 1, .swipe3 = 1, .swipe4 = 0 };
static const TpSink *sink;
static void *sink_ud;
static int scr_w = 1920, scr_h = 1080;
static double last_key = -1e12;
static TpDevice *devs[MAX_DEVS];
static int ndevs;

const TpConfig *tp_config(void) { return &cfg; }
void tp_set_config(const TpConfig *c) {
    cfg = *c;
    if (cfg.speed < 0) cfg.speed = 0;
    if (cfg.speed > 1) cfg.speed = 1;
    if (cfg.secondary < 0 || cfg.secondary > 2) cfg.secondary = 0;
}
void tp_set_sink(const TpSink *s, void *ud) { sink = s; sink_ud = ud; }
void tp_set_screen(int w, int h) { if (w > 0 && h > 0) { scr_w = w; scr_h = h; } }

static double mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---- output ---- */

static void out_motion(double dx, double dy, double rx, double ry) { if (sink && sink->motion) sink->motion(sink_ud, dx, dy, rx, ry); }
static void out_button(int b, int down) { if (sink && sink->button) sink->button(sink_ud, b, down); }
static void out_scroll(int ph, double dx, double dy) { if (sink && sink->scroll) sink->scroll(sink_ud, ph, dx, dy); }
static void out_pinch(int ph, int n, double dx, double dy, double sc, double rot) { if (sink && sink->pinch) sink->pinch(sink_ud, ph, n, dx, dy, sc, rot); }
static void out_swipe(int ph, int n, double dx, double dy) { if (sink && sink->swipe) sink->swipe(sink_ud, ph, n, dx, dy); }
static void out_hold(int ph, int n) { if (sink && sink->hold) sink->hold(sink_ud, ph, n); }
static int  out_touch(int ph, int id, double x, double y) { return sink && sink->touch ? sink->touch(sink_ud, ph, id, x, y) : 0; }
static void out_touch_frame(void) { if (sink && sink->touch_frame) sink->touch_frame(sink_ud); }
static void out_warp(double x, double y) { if (sink && sink->warp) sink->warp(sink_ud, x, y); }

/* ---- curves ---- */

/* px per mm. Slow (< 12 mm/s) is precise: ~2.4 px/mm (one pixel every 0.4 mm); fast (≥ 170 mm/s)
 * reaches ~15 px/mm, so 8 cm of touchpad cross a 1920 screen. The setting multiplies everything
 * between ×0.5 and ×2 (0.5 = ×1). */
double tp_accel(double v, double setting) {
    double factor = 0.5 * pow(4.0, setting < 0 ? 0 : setting > 1 ? 1 : setting);
    double lo = 2.4, hi = 15.0, v0 = 12.0, v1 = 170.0;
    double t = (v - v0) / (v1 - v0);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    t = t * t * (3 - 2 * t);
    return factor * (lo + (hi - lo) * t);
}

/* scroll: ~5 px per mm, a bit faster if the fingers move fast (up to ×3) */
static double scroll_gain(TpDevice *d, double v) {
    if (d->c.kind == TP_KIND_TOUCHSCREEN) return (double)scr_h / (d->hmm > 1 ? d->hmm : 1);
    double k = 1 + v / 180.0;
    return 5.0 * (k > 3 ? 3 : k);
}

/* ---- devices ---- */

TpDevice *tp_new(const TpCaps *caps) {
    if (ndevs >= MAX_DEVS) return NULL;
    TpDevice *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->c = *caps;
    d->fd = -1;
    if (d->c.nslots < 1) d->c.nslots = 1;
    if (d->c.nslots > MAX_SLOTS) d->c.nslots = MAX_SLOTS;
    double w = d->c.x.maximum - d->c.x.minimum, h = d->c.y.maximum - d->c.y.minimum;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    /* no resolution: a 100 mm wide touchpad is assumed (or 300 mm for a screen) */
    d->rx = d->c.x.resolution > 0 ? d->c.x.resolution : w / (d->c.kind == TP_KIND_TOUCHSCREEN ? 300.0 : 100.0);
    d->ry = d->c.y.resolution > 0 ? d->c.y.resolution : d->rx;
    d->wmm = w / d->rx;
    d->hmm = h / d->ry;
    for (int i = 0; i < MAX_SLOTS; i++) d->s[i].id = -1;
    devs[ndevs++] = d;
    return d;
}

void tp_free(TpDevice *d) {
    if (!d) return;
    for (int i = 0; i < ndevs; i++)
        if (devs[i] == d) { devs[i] = devs[--ndevs]; break; }
    free(d);
}

int tp_kind(const TpDevice *d) { return d ? d->c.kind : 0; }
const TpCaps *tp_caps(const TpDevice *d) { return &d->c; }

#define LONG_BITS (8 * sizeof(long))
#define NLONGS(n) (((n) + LONG_BITS - 1) / LONG_BITS)
#define HAS(bits, n) (((bits)[(n) / LONG_BITS] >> ((n) % LONG_BITS)) & 1UL)

TpDevice *tp_open(int fd, const char *name) {
    unsigned long ev[NLONGS(EV_MAX + 1)] = {0}, keys[NLONGS(KEY_MAX + 1)] = {0}, abs[NLONGS(ABS_MAX + 1)] = {0};
    unsigned long props[NLONGS(INPUT_PROP_MAX + 1)] = {0};
    ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
    ioctl(fd, EVIOCGPROP(sizeof props), props);
    if (!HAS(ev, EV_ABS) || !HAS(abs, ABS_MT_POSITION_X) || !HAS(abs, ABS_MT_POSITION_Y) || !HAS(abs, ABS_MT_SLOT)) return NULL;
    TpCaps c;
    memset(&c, 0, sizeof c);
    if (HAS(props, INPUT_PROP_DIRECT)) c.kind = TP_KIND_TOUCHSCREEN;
    else if (HAS(keys, BTN_TOOL_FINGER) && (HAS(props, INPUT_PROP_POINTER) || HAS(keys, BTN_LEFT) || HAS(props, INPUT_PROP_BUTTONPAD)))
        c.kind = TP_KIND_TOUCHPAD;
    else return NULL;
    c.clickpad = HAS(props, INPUT_PROP_BUTTONPAD);
    c.has_right = HAS(keys, BTN_RIGHT);
    struct input_absinfo slot;
    memset(&slot, 0, sizeof slot);
    ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &slot);
    c.nslots = slot.maximum + 1;
    ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &c.x);
    ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &c.y);
    if (HAS(abs, ABS_MT_PRESSURE)) { c.has_pressure = 1; ioctl(fd, EVIOCGABS(ABS_MT_PRESSURE), &c.pressure); }
    if (HAS(abs, ABS_MT_TOUCH_MAJOR)) { c.has_major = 1; ioctl(fd, EVIOCGABS(ABS_MT_TOUCH_MAJOR), &c.major); }
    c.max_fingers = HAS(keys, BTN_TOOL_QUINTTAP) ? 5 : HAS(keys, BTN_TOOL_QUADTAP) ? 4 : HAS(keys, BTN_TOOL_TRIPLETAP) ? 3
                  : HAS(keys, BTN_TOOL_DOUBLETAP) ? 2 : 1;
    if (c.max_fingers < c.nslots) c.max_fingers = c.nslots;
    snprintf(c.name, sizeof c.name, "%s", name ? name : "");
    TpDevice *d = tp_new(&c);
    if (!d) return NULL;
    d->fd = fd;
    int clk = CLOCK_MONOTONIC;
    ioctl(fd, EVIOCSCLOCKID, &clk);
    fprintf(stderr, "tp: %s: %s%s, %.0f×%.0f mm, %d contacts\n", c.name, c.kind == TP_KIND_TOUCHSCREEN ? "touchscreen" : "touchpad",
            c.clickpad ? " (clickpad)" : "", d->wmm, d->hmm, c.nslots);
    return d;
}

int tp_count(int kind) {
    int n = 0;
    for (int i = 0; i < ndevs; i++) if (!kind || devs[i]->c.kind == kind) n++;
    return n;
}

int tp_list_json(char *out, int size) {
    int o = snprintf(out, (size_t)size, "[");
    for (int i = 0; i < ndevs && o < size - 200; i++) {
        const TpDevice *d = devs[i];
        char name[80];
        int k = 0;
        for (const char *p = d->c.name; *p && k < 78; p++) if (*p != '"' && *p != '\\' && (unsigned char)*p >= 32) name[k++] = *p;
        name[k] = 0;
        o += snprintf(out + o, (size_t)(size - o), "%s{\"name\":\"%s\",\"kind\":\"%s\",\"clickpad\":%d,\"width\":%.0f,\"height\":%.0f,\"fingers\":%d}",
                      i ? "," : "", name, d->c.kind == TP_KIND_TOUCHSCREEN ? "touchscreen" : "touchpad", d->c.clickpad, d->wmm, d->hmm, d->c.max_fingers);
    }
    if (o < size - 1) o += snprintf(out + o, (size_t)(size - o), "]");
    return o;
}

void tp_key_activity(int code, double now) {
    switch (code) {
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: case KEY_LEFTALT:
    case KEY_RIGHTALT: case KEY_LEFTMETA: case KEY_RIGHTMETA: case KEY_CAPSLOCK: case KEY_FN:
        return;
    }
    last_key = now;
}

/* ---- contacts ---- */

static void slot_mm(TpDevice *d, Slot *s) {
    s->mx = (s->x - d->c.x.minimum) / d->rx;
    s->my = (s->y - d->c.y.minimum) / d->ry;
}

static double edge_zone(TpDevice *d) { double z = d->wmm * 0.07; return z > 7 ? 7 : z; }
static double bottom_zone(TpDevice *d) { double z = d->hmm * 0.15; return z > 12 ? 12 : z; }
static int in_edge(TpDevice *d, const Slot *s) { double z = edge_zone(d); return s->mx < z || s->mx > d->wmm - z; }

static int is_palm_size(TpDevice *d, const Slot *s) {
    if (d->c.has_major && s->major > 0 && s->major / d->rx > 22.0) return 1;
    if (d->c.has_pressure && d->c.pressure.maximum > d->c.pressure.minimum && !d->phys &&
        s->pressure > d->c.pressure.minimum + 0.85 * (d->c.pressure.maximum - d->c.pressure.minimum)) return 1;
    return 0;
}

static void begin_touch(TpDevice *d, Slot *s) {
    slot_mm(d, s);
    s->pmx = s->sx = s->mx;
    s->pmy = s->sy = s->my;
    s->t0 = d->now;
    s->palm = s->edge = s->thumb = s->dwt = 0;
    if (d->c.kind != TP_KIND_TOUCHPAD) return;
    s->palm = is_palm_size(d, s);
    s->edge = in_edge(d, s);
    s->thumb = d->c.clickpad && s->my > d->hmm - bottom_zone(d);
    s->dwt = cfg.dwt && d->now - last_key < DWT_TIME;
}

static void update_touch(TpDevice *d, Slot *s) {
    slot_mm(d, s);
    if (d->c.kind != TP_KIND_TOUCHPAD) return;
    if (!s->palm && is_palm_size(d, s) && d->now - s->t0 < 150) s->palm = 1;
    if (s->edge) {
        if (!in_edge(d, s) && d->now - s->t0 < EDGE_TIME) s->edge = 0;        /* left the edge right away: finger */
        else if (d->now - s->t0 >= EDGE_TIME) { s->edge = 0; s->palm = 1; }  /* stayed on the edge: palm */
    }
    if (s->thumb && hypot(s->mx - s->sx, s->my - s->sy) > 4.0) s->thumb = 0;
}

/* Active fingers (without palms or touches while typing); bottom thumbs count only if there are no others. */
static int active(TpDevice *d, int *idx) {
    int n = 0, nt = 0, th[MAX_SLOTS];
    for (int i = 0; i < d->c.nslots; i++) {
        Slot *s = &d->s[i];
        if (!s->down || s->palm || s->edge || s->dwt) continue;
        if (s->thumb) th[nt++] = i;
        else idx[n++] = i;
    }
    if (!n) for (int i = 0; i < nt; i++) idx[n++] = th[i];
    return n;
}

static int count_down(TpDevice *d) {
    int n = 0;
    for (int i = 0; i < d->c.nslots; i++) if (d->s[i].down) n++;
    return n;
}

/* ---- pointer ---- */

static void pointer_move(TpDevice *d, double dx, double dy, double dt) {
    double dist = hypot(dx, dy);
    double v = dist / dt;
    d->vel = d->vel <= 0 ? v : d->vel * 0.6 + v * 0.4;
    /* smoothing (exponential moving average) against sensor jitter */
    d->sdx = 0.7 * dx + 0.3 * d->sdx;
    d->sdy = 0.7 * dy + 0.3 * d->sdy;
    double g = tp_accel(d->vel, cfg.speed);
    if (d->sdx == 0 && d->sdy == 0) return;
    out_motion(d->sdx * g, d->sdy * g, dx * 4.0, dy * 4.0);
}

/* average of how much each finger moved since the previous frame */
static void mean_delta(TpDevice *d, const int *idx, int n, double *dx, double *dy) {
    *dx = *dy = 0;
    for (int i = 0; i < n; i++) { *dx += d->s[idx[i]].mx - d->s[idx[i]].pmx; *dy += d->s[idx[i]].my - d->s[idx[i]].pmy; }
    if (n) { *dx /= n; *dy /= n; }
}

static void centroid(TpDevice *d, const int *idx, int n, double *x, double *y) {
    *x = *y = 0;
    for (int i = 0; i < n; i++) { *x += d->s[idx[i]].mx; *y += d->s[idx[i]].my; }
    if (n) { *x /= n; *y /= n; }
}

static int direct(TpDevice *d) { return d->c.kind == TP_KIND_TOUCHSCREEN; }

/* touchscreen mm → position 0..1 */
static void ts_pos(TpDevice *d, double mx, double my, double *x, double *y) {
    *x = mx / (d->wmm > 0 ? d->wmm : 1);
    *y = my / (d->hmm > 0 ? d->hmm : 1);
}

/* ---- scroll and inertia ---- */

static void scroll_out(TpDevice *d, double dx, double dy, double dt, int phase) {
    double v = hypot(dx, dy) / dt;
    double g = scroll_gain(d, v);
    double px = dx * g, py = dy * g;
    if (direct(d)) { px = dx * (double)scr_w / (d->wmm > 1 ? d->wmm : 1); py = dy * g; }
    /* scroll: positive = move the content down (like the DOM's deltaY and Wayland's axis). Natural: the
     * content follows the fingers (fingers up = move down in the document) */
    int nat = direct(d) || cfg.natural;
    if (nat) { px = -px; py = -py; }
    /* soft lock to an axis: a bit of sideways motion while going straight doesn't shift the view */
    if (fabs(px) < 0.15 * fabs(py)) px = 0;
    else if (fabs(py) < 0.15 * fabs(px)) py = 0;
    if (phase == TP_UPDATE && px == 0 && py == 0) return;
    d->ht[d->hi] = d->now;
    d->hx[d->hi] = px;
    d->hy[d->hi] = py;
    d->hi = (d->hi + 1) % 8;
    if (d->hn < 8) d->hn++;
    out_scroll(phase, px, py);
}

static void kinetic_start(TpDevice *d) {
    if (!cfg.kinetic) return;
    double sx = 0, sy = 0, first = d->now, last = -1;
    for (int i = 0; i < d->hn; i++) {
        if (d->now - d->ht[i] > 80) continue;
        sx += d->hx[i];
        sy += d->hy[i];
        if (d->ht[i] < first) first = d->ht[i];
        if (d->ht[i] > last) last = d->ht[i];
    }
    if (last < 0 || d->now - last > 60) return;          /* the fingers were already still */
    double span = (last - first) + 12.0;                   /* + one frame */
    double vx = sx / span * 1000, vy = sy / span * 1000;   /* px/s */
    if (hypot(vx, vy) < KINETIC_MIN) return;
    double vmax = 9000;
    if (hypot(vx, vy) > vmax) { double k = vmax / hypot(vx, vy); vx *= k; vy *= k; }
    d->kin = 1;
    d->kvx = vx;
    d->kvy = vy;
    d->kt = d->now;
}

static void kinetic_stop(TpDevice *d) {
    if (!d->kin) return;
    d->kin = 0;
    out_scroll(TP_MOMENTUM_END, 0, 0);
}

/* ---- gestures ---- */

static void hold_end(TpDevice *d, int cancel) {
    if (!d->hold) return;
    d->hold = 0;
    out_hold(cancel ? TP_CANCEL : TP_END, d->gn);
}

static void gesture_end(TpDevice *d, int next) {
    switch (d->g) {
    case G_SCROLL:
        out_scroll(TP_END, 0, 0);
        if (next < 2) kinetic_start(d);
        break;
    case G_PINCH: out_pinch(TP_END, 2, 0, 0, d->pscale, d->rot); break;
    case G_SWIPE: out_swipe(TP_END, d->gn, 0, 0); break;
    case G_DRAG3:
        if (!d->d3_down) break;
        if (next == 0) d->d3_release = d->now + DRAG3_RELEASE;     /* the fingers can be put down again to continue */
        else { out_button(0, 0); d->d3_down = 0; }
        break;
    }
    if (d->hold) hold_end(d, next != 0);
}

static void gesture_begin(TpDevice *d, int n, const int *idx, int npos) {
    d->gn = n;
    d->g_t0 = d->now;
    d->vel = 0;
    d->sdx = d->sdy = 0;
    d->hold_move = 0;
    d->hn = d->hi = 0;
    if (n == 0) { d->g = G_NONE; return; }
    if (n == 1) {
        d->g = G_POINTER;
        if (!direct(d)) { d->hold = 1; out_hold(TP_BEGIN, 1); }
        return;
    }
    centroid(d, idx, npos, &d->ccx, &d->ccy);
    if (n == 2 && npos < 2) { d->g = G_NONE; return; }
    if (n == 2) {
        d->g = G_UND2;
        d->bi = idx[0];
        d->bj = idx[1];
        d->b1x = d->s[idx[0]].mx; d->b1y = d->s[idx[0]].my;
        d->b2x = d->s[idx[1]].mx; d->b2y = d->s[idx[1]].my;
        if (!direct(d) && (cfg.scroll || cfg.pinch)) { d->hold = 1; out_hold(TP_BEGIN, 2); }
        return;
    }
    if (direct(d)) { d->g = G_NONE; return; }               /* touchscreen: 3+ fingers do nothing */
    if (n == 3 && cfg.three_drag) {
        d->g = G_DRAG3;
        if (d->d3_down) d->d3_release = 0;                   /* put down again: the drag continues */
        return;
    }
    d->g = G_UNDN;
    d->hold = 1;
    out_hold(TP_BEGIN, n);
}

static double angle_of(double x1, double y1, double x2, double y2) { return atan2(y2 - y1, x2 - x1) * 180.0 / M_PI; }

static void pinch_update(TpDevice *d, double dt) {
    Slot *a = &d->s[d->bi], *b = &d->s[d->bj];
    double dist = hypot(b->mx - a->mx, b->my - a->my);
    double ang = angle_of(a->mx, a->my, b->mx, b->my);
    double da = ang - d->last_ang;
    while (da > 180) da -= 360;
    while (da < -180) da += 360;
    d->last_ang = ang;
    d->rot += da;
    d->pscale = d->dist0 > 0.5 ? dist / d->dist0 : 1;
    double dx = ((a->mx - a->pmx) + (b->mx - b->pmx)) / 2, dy = ((a->my - a->pmy) + (b->my - b->pmy)) / 2;
    double g = direct(d) ? (double)scr_w / (d->wmm > 1 ? d->wmm : 1) : tp_accel(0, cfg.speed);
    (void)dt;
    out_pinch(TP_UPDATE, 2, dx * g, dy * g, d->pscale, d->rot);
    if (direct(d)) { double x, y; ts_pos(d, (a->mx + b->mx) / 2, (a->my + b->my) / 2, &x, &y); out_warp(x, y); }
}

static void decide2(TpDevice *d, double dt) {
    Slot *a = &d->s[d->bi], *b = &d->s[d->bj];
    double v1x = a->mx - d->b1x, v1y = a->my - d->b1y, v2x = b->mx - d->b2x, v2y = b->my - d->b2y;
    double l1 = hypot(v1x, v1y), l2 = hypot(v2x, v2y);
    double dist0 = hypot(d->b2x - d->b1x, d->b2y - d->b1y), dist = hypot(b->mx - a->mx, b->my - a->my);
    double dd = fabs(dist - dist0);
    double cm = hypot((v1x + v2x) / 2, (v1y + v2y) / 2);
    int pinch = 0, scroll = 0;
    if (l1 > DECIDE_MOVE && l2 > DECIDE_MOVE) {
        double cosang = (v1x * v2x + v1y * v2y) / (l1 * l2);
        if (cosang > 0.3) scroll = 1;
        else pinch = 1;                         /* opposite (spread/close) or crossed (rotate) */
    } else if ((l1 > 3 * DECIDE_MOVE || l2 > 3 * DECIDE_MOVE)) {
        /* one finger still and the other moving: pinch if the distance changes, otherwise scroll */
        if (dd > 0.7 * (l1 > l2 ? l1 : l2)) pinch = 1;
        else scroll = 1;
    } else if (d->now - d->g_t0 > 400 && cm > 1.5) scroll = 1;
    if (pinch && !cfg.pinch) { pinch = 0; scroll = cfg.scroll; }
    if (scroll && !cfg.scroll) scroll = 0;
    if (!pinch && !scroll) return;
    hold_end(d, 1);
    if (direct(d)) {
        double x, y;
        ts_pos(d, (a->mx + b->mx) / 2, (a->my + b->my) / 2, &x, &y);
        out_warp(x, y);
    }
    if (scroll) {
        d->g = G_SCROLL;
        kinetic_stop(d);
        double mdx = (v1x + v2x) / 2, mdy = (v1y + v2y) / 2;
        scroll_out(d, 0, 0, dt, TP_BEGIN);
        scroll_out(d, mdx, mdy, d->now - d->g_t0 > 1 ? (d->now - d->g_t0) / 1000.0 : dt, TP_UPDATE);
    } else {
        d->g = G_PINCH;
        d->dist0 = dist0;
        d->last_ang = angle_of(d->b1x, d->b1y, d->b2x, d->b2y);
        d->rot = 0;
        d->pscale = 1;
        out_pinch(TP_BEGIN, 2, 0, 0, 1, 0);
        pinch_update(d, dt);
    }
}

static void gesture_frame(TpDevice *d, const int *idx, int n, double dt) {
    double dx, dy;
    switch (d->g) {
    case G_POINTER: {
        int k = idx[0];
        if (d->phys && n > 1) {                  /* click held with one finger while moving another */
            double best = -1;
            for (int i = 0; i < n; i++) {
                Slot *s = &d->s[idx[i]];
                double m = hypot(s->mx - s->pmx, s->my - s->pmy);
                if (m > best) { best = m; k = idx[i]; }
            }
        }
        Slot *s = &d->s[k];
        dx = s->mx - s->pmx;
        dy = s->my - s->pmy;
        if (direct(d)) return;
        d->hold_move += hypot(dx, dy);
        if (d->hold && d->hold_move > 0.6) hold_end(d, 1);
        if (dx != 0 || dy != 0) pointer_move(d, dx, dy, dt);
        break;
    }
    case G_UND2: decide2(d, dt); break;
    case G_SCROLL:
        mean_delta(d, idx, n, &dx, &dy);
        scroll_out(d, dx, dy, dt, TP_UPDATE);
        break;
    case G_PINCH: pinch_update(d, dt); break;
    case G_UNDN: {
        double cx, cy;
        centroid(d, idx, n, &cx, &cy);
        if (hypot(cx - d->ccx, cy - d->ccy) < SWIPE_START) break;
        hold_end(d, 1);
        d->g = G_SWIPE;
        double g = tp_accel(60, 0.5);
        out_swipe(TP_BEGIN, n, 0, 0);
        out_swipe(TP_UPDATE, n, (cx - d->ccx) * g, (cy - d->ccy) * g);
        break;
    }
    case G_SWIPE: {
        mean_delta(d, idx, n, &dx, &dy);
        double g = tp_accel(60, 0.5);
        if (dx != 0 || dy != 0) out_swipe(TP_UPDATE, n, dx * g, dy * g);
        break;
    }
    case G_DRAG3: {
        double cx, cy;
        mean_delta(d, idx, n, &dx, &dy);
        centroid(d, idx, n, &cx, &cy);
        if (!d->d3_down && hypot(cx - d->ccx, cy - d->ccy) > 1.5) { d->d3_down = 1; d->d3_release = 0; out_button(0, 1); }
        if (d->d3_down && (dx != 0 || dy != 0)) pointer_move(d, dx, dy, dt);
        break;
    }
    }
}

/* ---- tap to click ---- */

static int tap_moved(TpDevice *d) {
    for (int i = 0; i < d->c.nslots; i++) {
        Slot *s = &d->s[i];
        if (s->down && !s->palm && !s->edge && !s->dwt && hypot(s->mx - s->sx, s->my - s->sy) > TAP_MOVE) return 1;
    }
    return 0;
}

static void tap_release(TpDevice *d) {
    if (d->tap_down) { out_button(0, 0); d->tap_down = 0; }
}

static void do_tap(TpDevice *d) {
    int b = d->tap_max == 1 ? 0 : d->tap_max == 2 ? 2 : d->tap_max == 3 ? 1 : -1;
    d->tap = T_IDLE;
    if (b < 0) return;
    if (b == 0 && cfg.tap_drag) {
        out_button(0, 1);
        d->tap_down = 1;
        d->tap = T_TAPPED;
        d->tap_deadline = d->now + TAP_DRAG_TIME;
        return;
    }
    out_button(b, 1);
    out_button(b, 0);
}

static void tap_frame(TpDevice *d, int nf) {
    int moved = tap_moved(d);
    switch (d->tap) {
    case T_IDLE:
        if (cfg.tap && nf > 0 && !d->phys && !d->d3_down) { d->tap = T_TOUCH; d->tap_t = d->now; d->tap_max = nf; }
        break;
    case T_TOUCH:
        if (nf > d->tap_max) d->tap_max = nf;
        if (nf == 0) { if (d->now - d->tap_t <= TAP_TIME) do_tap(d); else d->tap = T_IDLE; }
        else if (d->phys || moved || d->now - d->tap_t > TAP_TIME || d->g == G_SCROLL || d->g == G_PINCH || d->g == G_SWIPE) d->tap = T_DEAD;
        break;
    case T_TAPPED:
        if (nf > 0) { d->tap = T_DRAG_OR_DOUBLE; d->tap_t = d->now; }
        break;
    case T_DRAG_OR_DOUBLE:
        if (nf > 1) { tap_release(d); d->tap = T_DEAD; }
        else if (nf == 0) {                      /* double tap: releases and clicks again */
            tap_release(d);
            out_button(0, 1);
            d->tap_down = 1;
            d->tap = T_TAPPED;
            d->tap_deadline = d->now + TAP_DRAG_TIME;
        } else if (moved || d->now - d->tap_t > TAP_TIME) d->tap = T_DRAGGING;
        break;
    case T_DRAGGING:
        if (nf == 0) { tap_release(d); d->tap = T_IDLE; }
        break;
    case T_DEAD:
        if (nf == 0) d->tap = T_IDLE;
        break;
    }
}

/* ---- physical click ---- */

static int click_button(TpDevice *d) {
    if (!d->c.clickpad) return 0;
    int all = 0, fingers = 0, bottom_l = 0, bottom_r = 0;
    for (int i = 0; i < d->c.nslots; i++) {
        Slot *s = &d->s[i];
        if (!s->down || s->palm) continue;
        all++;
        if (!s->thumb) fingers++;
        if (s->my > d->hmm - bottom_zone(d)) { if (s->mx > d->wmm / 2) bottom_r = 1; else bottom_l = 1; }
    }
    if (cfg.secondary == TP_CLICK_BOTTOM_RIGHT) return bottom_r ? 2 : 0;
    if (cfg.secondary == TP_CLICK_BOTTOM_LEFT) return bottom_l ? 2 : 0;
    int n = fingers ? fingers : all;
    if (d->tool > n && d->tool > d->c.nslots) n = d->tool;
    return n >= 3 ? 1 : n == 2 ? 2 : 0;
}

/* ---- complete frame ---- */

static void touchpad_frame(TpDevice *d, double dt) {
    int idx[MAX_SLOTS];
    int n = active(d, idx);
    /* devices with fewer contacts than fingers (BTN_TOOL_TRIPLETAP with 2 slots) */
    if (d->tool > d->c.nslots && count_down(d) >= d->c.nslots && n == count_down(d)) n = d->tool;
    int npos = n > count_down(d) ? count_down(d) : n;      /* fingers with a position */

    if (n > 0 && d->kin) kinetic_stop(d);
    if (n > 0 && d->d3_down && d->d3_release > 0 && n != 3) { out_button(0, 0); d->d3_down = 0; d->d3_release = 0; }

    /* physical click */
    if (d->phys != d->phys_prev) {
        if (d->phys) {
            if (d->tap_down) tap_release(d);
            if (d->tap != T_IDLE) d->tap = T_DEAD;
            d->phys_btn = click_button(d);
            out_button(d->phys_btn, 1);
        } else out_button(d->phys_btn, 0);
        d->phys_prev = d->phys;
    }
    for (int b = 0; b < 3; b++)
        if (d->btn_pending[b] != d->btn_state[b]) { d->btn_state[b] = d->btn_pending[b]; out_button(b, d->btn_state[b]); }

    tap_frame(d, n);

    int want = d->phys ? (n > 0 ? 1 : 0) : n;
    if (want != d->gn) {
        gesture_end(d, want);
        gesture_begin(d, want, idx, npos);
    } else if (want > 0 && npos > 0) gesture_frame(d, idx, npos, dt);
}

static void touchscreen_frame(TpDevice *d, double dt) {
    int idx[MAX_SLOTS], n = 0;
    for (int i = 0; i < d->c.nslots; i++) if (d->s[i].down) idx[n++] = i;
    double x, y;
    /* a new sequence: does a Wayland app take it? */
    if (d->ts_prev_n == 0 && n > 0) {
        Slot *s = &d->s[idx[0]];
        ts_pos(d, s->mx, s->my, &x, &y);
        kinetic_stop(d);
        d->ts_app = out_touch(TP_BEGIN, idx[0], x, y);
        d->ts_multi = 0;
        if (d->ts_app) {
            for (int i = 1; i < n; i++) { ts_pos(d, d->s[idx[i]].mx, d->s[idx[i]].my, &x, &y); out_touch(TP_BEGIN, idx[i], x, y); }
            out_touch_frame();
            d->ts_prev_n = n;
            return;
        }
    } else if (d->ts_app) {
        for (int i = 0; i < d->c.nslots; i++) {
            Slot *s = &d->s[i];
            ts_pos(d, s->mx, s->my, &x, &y);
            if (s->down && !s->was) out_touch(TP_BEGIN, i, x, y);
            else if (s->down && (s->mx != s->pmx || s->my != s->pmy)) out_touch(TP_UPDATE, i, x, y);
            else if (!s->down && s->was) out_touch(TP_END, i, x, y);
        }
        out_touch_frame();
        if (n == 0) d->ts_app = 0;
        d->ts_prev_n = n;
        return;
    }
    /* pointer emulation */
    if (n >= 2 && !d->ts_multi) {
        d->ts_multi = 1;
        if (d->ts_btn) { out_button(0, 0); d->ts_btn = 0; }
        d->ts_pending = 0;
    }
    if (n == 1 && !d->ts_multi) {
        Slot *s = &d->s[idx[0]];
        ts_pos(d, s->mx, s->my, &x, &y);
        if (d->ts_prev_n == 0) { out_warp(x, y); d->ts_pending = 1; d->ts_t0 = d->now; }
        else if (s->mx != s->pmx || s->my != s->pmy) out_warp(x, y);
        if (d->ts_pending && (hypot(s->mx - s->sx, s->my - s->sy) > 2.0 || d->now - d->ts_t0 > TS_PRESS_TIME)) {
            d->ts_pending = 0;
            d->ts_btn = 1;
            out_button(0, 1);
        }
    }
    if (n == 0) {
        if (d->ts_pending) { out_button(0, 1); out_button(0, 0); }
        else if (d->ts_btn) out_button(0, 0);
        d->ts_pending = d->ts_btn = 0;
        d->ts_multi = 0;
    }
    int want = n >= 2 ? n : 0;
    if (want != d->gn) { gesture_end(d, want); gesture_begin(d, want, idx, n); }
    else if (want) gesture_frame(d, idx, n, dt);
    d->ts_prev_n = n;
}

static void resync(TpDevice *d) {
    if (d->fd < 0) return;
    struct { unsigned code; int v[MAX_SLOTS]; } req;
    int codes[3] = { ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y };
    for (int c = 0; c < 3; c++) {
        memset(&req, 0, sizeof req);
        req.code = (unsigned)codes[c];
        if (ioctl(d->fd, EVIOCGMTSLOTS(sizeof req), &req) < 0) return;
        for (int i = 0; i < d->c.nslots; i++) {
            if (c == 0) { d->s[i].id = req.v[i]; d->s[i].down = req.v[i] >= 0; }
            else if (c == 1) d->s[i].x = req.v[i];
            else d->s[i].y = req.v[i];
        }
    }
    struct input_absinfo a;
    if (ioctl(d->fd, EVIOCGABS(ABS_MT_SLOT), &a) == 0 && a.value >= 0 && a.value < MAX_SLOTS) d->cur = a.value;
}

static void frame(TpDevice *d) {
    double dt = d->prev_t > 0 ? (d->now - d->prev_t) / 1000.0 : 0.012;
    if (dt < 0.004) dt = 0.004;
    if (dt > 0.05) dt = 0.05;
    for (int i = 0; i < d->c.nslots; i++) {
        Slot *s = &d->s[i];
        if (s->down && !s->was) begin_touch(d, s);
        else if (s->down) update_touch(d, s);
    }
    if (d->c.kind == TP_KIND_TOUCHSCREEN) touchscreen_frame(d, dt);
    else touchpad_frame(d, dt);
    for (int i = 0; i < d->c.nslots; i++) {
        Slot *s = &d->s[i];
        s->pmx = s->mx;
        s->pmy = s->my;
        s->was = s->down;
    }
    d->prev_t = d->now;
}

void tp_event_at(TpDevice *d, const struct input_event *ev, double now) {
    if (d->dropped) {
        if (ev->type == EV_SYN && ev->code == SYN_REPORT) { d->dropped = 0; resync(d); d->now = now; frame(d); }
        return;
    }
    Slot *s = &d->s[d->cur];
    switch (ev->type) {
    case EV_ABS:
        switch (ev->code) {
        case ABS_MT_SLOT: if (ev->value >= 0 && ev->value < d->c.nslots) d->cur = ev->value; break;
        case ABS_MT_TRACKING_ID: s->id = ev->value; s->down = ev->value >= 0; break;
        case ABS_MT_POSITION_X: s->x = ev->value; break;
        case ABS_MT_POSITION_Y: s->y = ev->value; break;
        case ABS_MT_TOUCH_MAJOR: s->major = ev->value; break;
        case ABS_MT_PRESSURE: s->pressure = ev->value; break;
        }
        break;
    case EV_KEY: {
        int n = ev->code == BTN_TOOL_FINGER ? 1 : ev->code == BTN_TOOL_DOUBLETAP ? 2 : ev->code == BTN_TOOL_TRIPLETAP ? 3
              : ev->code == BTN_TOOL_QUADTAP ? 4 : ev->code == BTN_TOOL_QUINTTAP ? 5 : 0;
        if (n) { if (ev->value) d->tool = n; else if (d->tool == n) d->tool = 0; break; }
        if (ev->value == 2) break;
        if (ev->code == BTN_LEFT && d->c.clickpad) d->phys = ev->value != 0;
        else if (ev->code == BTN_LEFT) d->btn_pending[0] = ev->value != 0;
        else if (ev->code == BTN_MIDDLE) d->btn_pending[1] = ev->value != 0;
        else if (ev->code == BTN_RIGHT) d->btn_pending[2] = ev->value != 0;
        break;
    }
    case EV_SYN:
        if (ev->code == SYN_DROPPED) { d->dropped = 1; break; }
        if (ev->code == SYN_REPORT) { d->now = now; frame(d); }
        break;
    }
}

void tp_event(TpDevice *d, const struct input_event *ev) {
    double t = ev->input_event_sec * 1e3 + ev->input_event_usec / 1e3;
    if (t <= 0) t = mono_ms();
    tp_event_at(d, ev, t);
}

/* ---- latency (log) ---- */

void tp_latency(TpDevice *d, const struct input_event *syn) {
    static const char *const names[] = { "tap", "pointer", "two fingers", "scroll", "pinch", "several fingers", "swipe", "3-finger drag" };
    double t = syn->input_event_sec * 1e3 + syn->input_event_usec / 1e3;
    double lat = mono_ms() - t;
    if (t > 0 && lat >= 0 && lat < 1000) {
        d->lat_sum += lat;
        if (lat > d->lat_max) d->lat_max = lat;
        d->lat_n++;
    }
    if (d->g > G_NONE && d->g > d->lat_kind) d->lat_kind = d->g;      /* the "highest" gesture of the sequence */
    if (count_down(d) == 0 && d->lat_n > 0) {
        fprintf(stderr, "tp: %s: %d frames, average latency %.2f ms, max %.2f ms (read → JS/Wayland)\n",
                names[d->lat_kind], d->lat_n, d->lat_sum / d->lat_n, d->lat_max);
        d->lat_n = 0;
        d->lat_sum = d->lat_max = 0;
        d->lat_kind = 0;
    }
}

/* ---- timers ---- */

double tp_timeout(double now) {
    double best = -1;
#define CONSIDER(t) do { double w_ = (t) - now; if (w_ < 0) w_ = 0; if (best < 0 || w_ < best) best = w_; } while (0)
    for (int i = 0; i < ndevs; i++) {
        TpDevice *d = devs[i];
        if (d->kin) CONSIDER(d->kt + 16);
        if (d->tap == T_TAPPED) CONSIDER(d->tap_deadline);
        if (d->tap == T_DRAG_OR_DOUBLE || d->tap == T_TOUCH) CONSIDER(d->tap_t + TAP_TIME + 1);
        if (d->d3_down && d->d3_release > 0) CONSIDER(d->d3_release);
        if (d->ts_pending) CONSIDER(d->ts_t0 + TS_PRESS_TIME + 1);
        for (int k = 0; k < d->c.nslots; k++) if (d->s[k].down && d->s[k].edge) CONSIDER(d->s[k].t0 + EDGE_TIME);
    }
#undef CONSIDER
    return best;
}

void tp_tick(double now) {
    for (int i = 0; i < ndevs; i++) {
        TpDevice *d = devs[i];
        int out = 0;
        if (d->kin && now - d->kt >= 15) {
            double dt = now - d->kt;
            if (dt > 50) dt = 50;
            d->kt = now;
            double dx = d->kvx * dt / 1000, dy = d->kvy * dt / 1000;
            double k = exp(-dt / KINETIC_TAU);
            d->kvx *= k;
            d->kvy *= k;
            if (hypot(d->kvx, d->kvy) < KINETIC_STOP) { d->kin = 0; out_scroll(TP_MOMENTUM_END, dx, dy); }
            else out_scroll(TP_MOMENTUM, dx, dy);
            out = 1;
        }
        if (d->tap == T_TAPPED && now >= d->tap_deadline) { d->now = now; tap_release(d); d->tap = T_IDLE; out = 1; }
        if (d->tap == T_DRAG_OR_DOUBLE && now - d->tap_t > TAP_TIME) d->tap = T_DRAGGING;
        if (d->tap == T_TOUCH && now - d->tap_t > TAP_TIME) d->tap = T_DEAD;
        if (d->d3_down && d->d3_release > 0 && now >= d->d3_release) { out_button(0, 0); d->d3_down = 0; d->d3_release = 0; out = 1; }
        if (d->ts_pending && now - d->ts_t0 > TS_PRESS_TIME) { d->ts_pending = 0; d->ts_btn = 1; out_button(0, 1); out = 1; }
        for (int k = 0; k < d->c.nslots; k++) {     /* a still touch on the edge: palm */
            Slot *s = &d->s[k];
            if (s->down && s->edge && now - s->t0 >= EDGE_TIME) { s->edge = 0; s->palm = 1; }
        }
        if (out && sink && sink->frame) sink->frame(sink_ud);
    }
}
