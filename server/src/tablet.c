/* tablet.c — graphics tablets from evdev: see tablet.h. */
#include "tablet.h"

#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define LONG_BITS (8 * sizeof(long))
#define NLONGS(n) (((n) + LONG_BITS - 1) / LONG_BITS)
#define HAS(bits, n) (((bits)[(n) / LONG_BITS] >> ((n) % LONG_BITS)) & 1UL)

enum { AX_X, AX_Y, AX_PRESSURE, AX_DISTANCE, AX_TILT_X, AX_TILT_Y, AX_ROTATION, AX_SLIDER, AX_N };
/* the stylus buttons a pen frame reports, bit i = stylus_code[i] */
static const unsigned short stylus_code[3] = { BTN_STYLUS, BTN_STYLUS2, BTN_STYLUS3 };

struct TabDev {
    TabCaps c;
    void *user;
    int announced;                      /* tablet_open's: the host was told */
    int dropped;                        /* SYN_DROPPED: everything up to the next SYN_REPORT is skipped */
    /* pen */
    unsigned tool_keys;                 /* TAB_TOOL_BIT of the BTN_TOOL_* held */
    unsigned tool;                      /* the tool in proximity (BTN_TOOL_*), 0 none */
    int touch;                          /* BTN_TOUCH held */
    int down;                           /* the tip as the tool was last told */
    unsigned held, sent;                /* stylus buttons held, and as the tool was last told (bits of stylus_code) */
    int v[AX_N];
    unsigned dirty;                     /* bit AX_*: changed since the last frame */
    uint64_t serial;
    unsigned tool_id;
    int wheel;                          /* the tool's wheel since the last frame, 1/120 of a click */
    /* pad */
    uint32_t pad_held, pad_sent;        /* bit i: pad button i */
    int ring_v[TAB_PAD_RINGS], strip_v[TAB_PAD_STRIPS];
    unsigned ring_dirty, strip_dirty;
    int dial[TAB_PAD_DIALS];
};

static const TabHost *host;
void tablet_set_host(const TabHost *h) { host = h; }
void tablet_set_user(TabDev *d, void *u) { if (d) d->user = u; }
void *tablet_user(const TabDev *d) { return d ? d->user : NULL; }
const TabCaps *tablet_caps(const TabDev *d) { return &d->c; }

/* ---- what a device is ---- */

/* A pad's buttons in the order the kernel numbers them (wacom_report_numbered_buttons): BTN_0…BTN_9, BTN_A…BTN_Z,
 * BTN_BASE…BTN_BASE6, then BTN_LEFT…BTN_TASK. */
static int pad_codes(unsigned short *out, int max, const unsigned long *keys) {
    static const unsigned short range[][2] = {
        { BTN_0, BTN_9 }, { BTN_A, BTN_Z }, { BTN_BASE, BTN_BASE6 }, { BTN_LEFT, BTN_TASK },
    };
    int n = 0;
    for (size_t r = 0; r < sizeof range / sizeof range[0]; r++)
        for (unsigned c = range[r][0]; c <= range[r][1]; c++)
            if (HAS(keys, c) && n < max) out[n++] = (unsigned short)c;
    return n;
}

int tablet_kind(const unsigned long *ev, const unsigned long *abs, const unsigned long *keys, const unsigned long *props,
                const unsigned long *rel) {
    if (!HAS(ev, EV_KEY)) return TAB_KIND_NONE;
    if (HAS(ev, EV_ABS) && HAS(abs, ABS_MT_SLOT)) return TAB_KIND_NONE;           /* a touch screen or a touchpad */
    if (HAS(keys, KEY_A)) return TAB_KIND_NONE;                                   /* a keyboard */
    if (HAS(ev, EV_REL) && HAS(rel, REL_X) && HAS(keys, BTN_LEFT)) return TAB_KIND_NONE;   /* a mouse */
    if (HAS(props, INPUT_PROP_ACCELEROMETER)) return TAB_KIND_NONE;
    unsigned short b[1];
    int pen_tool = HAS(keys, BTN_TOOL_PEN) || HAS(keys, BTN_TOOL_RUBBER) || HAS(keys, BTN_TOOL_BRUSH) ||
                   HAS(keys, BTN_TOOL_PENCIL) || HAS(keys, BTN_TOOL_AIRBRUSH);
    int numbered = HAS(keys, BTN_0) || HAS(keys, BTN_1);
    if (numbered && !pen_tool && !HAS(keys, BTN_TOUCH) && pad_codes(b, 1, keys)) return TAB_KIND_PAD;
    int xy = HAS(ev, EV_ABS) && HAS(abs, ABS_X) && HAS(abs, ABS_Y);
    if (xy && (pen_tool || HAS(keys, BTN_STYLUS))) return TAB_KIND_PEN;
    return TAB_KIND_NONE;
}

TabDev *tablet_new(const TabCaps *c) {
    if (!c || (c->kind != TAB_KIND_PEN && c->kind != TAB_KIND_PAD)) return NULL;
    TabDev *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->c = *c;
    d->v[AX_X] = c->x.value;
    d->v[AX_Y] = c->y.value;
    d->v[AX_PRESSURE] = c->pressure.value;
    d->v[AX_DISTANCE] = c->distance.value;
    d->v[AX_TILT_X] = c->tilt_x.value;
    d->v[AX_TILT_Y] = c->tilt_y.value;
    d->v[AX_ROTATION] = c->rotation.value;
    d->v[AX_SLIDER] = c->slider.value;
    return d;
}

void tablet_free(TabDev *d) {
    if (!d) return;
    if (d->announced && host && host->removed) host->removed(d);
    free(d);
}

TabDev *tablet_open(int fd, const char *name) {
    unsigned long ev[NLONGS(EV_MAX + 1)] = {0}, keys[NLONGS(KEY_MAX + 1)] = {0}, abs[NLONGS(ABS_MAX + 1)] = {0};
    unsigned long rel[NLONGS(REL_MAX + 1)] = {0}, props[NLONGS(INPUT_PROP_MAX + 1)] = {0};
    ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
    ioctl(fd, EVIOCGPROP(sizeof props), props);
    int kind = tablet_kind(ev, abs, keys, props, rel);
    if (kind == TAB_KIND_NONE) return NULL;
    TabCaps c;
    memset(&c, 0, sizeof c);
    c.kind = kind;
    snprintf(c.name, sizeof c.name, "%s", name ? name : "");
    struct input_id id;
    memset(&id, 0, sizeof id);
    ioctl(fd, EVIOCGID, &id);
    c.bus = id.bustype;
    c.vendor = id.vendor;
    c.product = id.product;
    c.direct = HAS(props, INPUT_PROP_DIRECT);
    char link[32], dev[sizeof c.path];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, dev, sizeof dev - 1);
    if (n > 0 && (size_t)n < sizeof dev - 1) { dev[n] = 0; if (!strncmp(dev, "/dev/input/event", 16)) snprintf(c.path, sizeof c.path, "%s", dev); }
    char phys[sizeof c.phys];
    memset(phys, 0, sizeof phys);
    if (ioctl(fd, EVIOCGPHYS(sizeof phys - 1), phys) > 0) {
        char *slash = strrchr(phys, '/');
        if (slash) *slash = 0;              /* "usb-0000:00:14.0-1/input0" → the USB device the pen and pad share */
        snprintf(c.phys, sizeof c.phys, "%s", phys);
    }
#define ABS(code, field, cap) do { if (HAS(abs, code)) { ioctl(fd, EVIOCGABS(code), &c.field); if (c.field.maximum > c.field.minimum) c.caps |= cap; } } while (0)
    if (kind == TAB_KIND_PEN) {
        ioctl(fd, EVIOCGABS(ABS_X), &c.x);
        ioctl(fd, EVIOCGABS(ABS_Y), &c.y);
        ABS(ABS_PRESSURE, pressure, TAB_CAP_PRESSURE);
        ABS(ABS_DISTANCE, distance, TAB_CAP_DISTANCE);
        if (HAS(abs, ABS_TILT_X) && HAS(abs, ABS_TILT_Y)) { ABS(ABS_TILT_X, tilt_x, TAB_CAP_TILT); ABS(ABS_TILT_Y, tilt_y, 0); }
        ABS(ABS_Z, rotation, TAB_CAP_ROTATION);
        ABS(ABS_WHEEL, slider, TAB_CAP_SLIDER);
        for (unsigned t = TAB_TOOL_FIRST; t <= TAB_TOOL_LAST; t++)
            if (t != BTN_TOOL_FINGER && HAS(keys, t)) c.tools |= TAB_TOOL_BIT(t);
        c.has_touch = HAS(keys, BTN_TOUCH);
        if (HAS(ev, EV_REL) && HAS(rel, REL_WHEEL)) { c.wheel = 1; c.wheel_hires = HAS(rel, REL_WHEEL_HI_RES); }
    } else {
        c.nbuttons = pad_codes(c.button, TAB_PAD_BUTTONS, keys);
        static const unsigned short rings[] = { ABS_WHEEL, ABS_THROTTLE }, strips[] = { ABS_RX, ABS_RY };
        for (int i = 0; i < 2; i++) {
            if (HAS(abs, rings[i])) { c.ring_code[c.nrings] = rings[i]; ioctl(fd, EVIOCGABS(rings[i]), &c.ring[c.nrings]); c.nrings++; }
            if (HAS(abs, strips[i])) { c.strip_code[c.nstrips] = strips[i]; ioctl(fd, EVIOCGABS(strips[i]), &c.strip[c.nstrips]); c.nstrips++; }
        }
        if (HAS(ev, EV_REL)) {
            c.dial_hires = HAS(rel, REL_WHEEL_HI_RES);
            if (HAS(rel, REL_WHEEL)) c.dial_code[c.ndials++] = REL_WHEEL;
            if (HAS(rel, REL_HWHEEL)) c.dial_code[c.ndials++] = REL_HWHEEL;
        }
    }
#undef ABS
    TabDev *d = tablet_new(&c);
    if (!d) return NULL;
    fprintf(stderr, "tablet: %s: %s %04x:%04x%s, %s\n", c.name, kind == TAB_KIND_PEN ? "pen" : "pad", c.vendor, c.product,
            c.direct ? " (display)" : "", c.path[0] ? c.path : "no path");
    tablet_announce(d);
    return d;
}

void tablet_announce(TabDev *d) {
    if (!d || d->announced) return;
    d->announced = 1;
    if (host && host->added) host->added(d);
}

unsigned tablet_tool_caps(const TabDev *d, unsigned tool) {
    unsigned caps = d->c.caps;
    if (tool == BTN_TOOL_MOUSE || tool == BTN_TOOL_LENS) caps &= ~(unsigned)(TAB_CAP_PRESSURE | TAB_CAP_TILT | TAB_CAP_SLIDER);
    if (tool != BTN_TOOL_AIRBRUSH) caps &= ~(unsigned)TAB_CAP_SLIDER;
    return caps;
}

/* ---- values ---- */

static double unit(int v, const struct input_absinfo *a) {
    if (a->maximum <= a->minimum) return 0;
    double u = (double)(v - a->minimum) / (a->maximum - a->minimum);
    return u < 0 ? 0 : u > 1 ? 1 : u;
}
/* As libinput: with a resolution (units per radian) and a range around 0, the angle itself; otherwise the range is
 * taken as Wacom's physical -64…64 degrees. */
static double tilt(int v, const struct input_absinfo *a) {
    double deg;
    if (a->resolution > 0 && a->minimum < 0 && a->maximum > 0) deg = 180.0 / M_PI * v / a->resolution;
    else deg = unit(v, a) * 128 - 64;
    return deg < -90 ? -90 : deg > 90 ? 90 : deg;
}

static int axis_of(unsigned code) {
    switch (code) {
    case ABS_X: return AX_X;
    case ABS_Y: return AX_Y;
    case ABS_PRESSURE: return AX_PRESSURE;
    case ABS_DISTANCE: return AX_DISTANCE;
    case ABS_TILT_X: return AX_TILT_X;
    case ABS_TILT_Y: return AX_TILT_Y;
    case ABS_Z: return AX_ROTATION;
    case ABS_WHEEL: return AX_SLIDER;
    default: return -1;
    }
}

static int tip(const TabDev *d) {
    if (d->c.has_touch) return d->touch;
    return (d->c.caps & TAB_CAP_PRESSURE) && unit(d->v[AX_PRESSURE], &d->c.pressure) > 0.05;
}

/* the tool to report: the one in proximity while its key is still held, else the first one held */
static unsigned wanted_tool(const TabDev *d) {
    if (!d->c.tools) return d->tool ? d->tool : (d->dirty & (1u << AX_X | 1u << AX_Y)) ? BTN_TOOL_PEN : 0;
    if (d->tool && (d->tool_keys & TAB_TOOL_BIT(d->tool))) return d->tool;
    for (unsigned t = TAB_TOOL_FIRST; t <= TAB_TOOL_LAST; t++)
        if (t != BTN_TOOL_FINGER && (d->tool_keys & TAB_TOOL_BIT(t))) return t;
    return 0;
}

static void fill_axes(const TabDev *d, TabFrame *f, unsigned which) {
    unsigned caps = tablet_tool_caps(d, f->tool);
    if (which & (1u << AX_X | 1u << AX_Y)) f->flags |= TF_MOTION;
    f->x = unit(d->v[AX_X], &d->c.x);
    f->y = unit(d->v[AX_Y], &d->c.y);
    f->pressure = (caps & TAB_CAP_PRESSURE) ? unit(d->v[AX_PRESSURE], &d->c.pressure) : 0;
    f->distance = (caps & TAB_CAP_DISTANCE) ? unit(d->v[AX_DISTANCE], &d->c.distance) : 0;
    f->tilt_x = (caps & TAB_CAP_TILT) ? tilt(d->v[AX_TILT_X], &d->c.tilt_x) : 0;
    f->tilt_y = (caps & TAB_CAP_TILT) ? tilt(d->v[AX_TILT_Y], &d->c.tilt_y) : 0;
    f->rotation = 0;
    if (caps & TAB_CAP_ROTATION) {
        const struct input_absinfo *a = &d->c.rotation;
        f->rotation = (double)(d->v[AX_ROTATION] - a->minimum) * 360.0 / (a->maximum - a->minimum + 1);
    }
    f->slider = (caps & TAB_CAP_SLIDER) ? unit(d->v[AX_SLIDER], &d->c.slider) * 2 - 1 : 0;
    if ((caps & TAB_CAP_PRESSURE) && (which & 1u << AX_PRESSURE)) f->flags |= TF_PRESSURE;
    if ((caps & TAB_CAP_DISTANCE) && (which & 1u << AX_DISTANCE)) f->flags |= TF_DISTANCE;
    if ((caps & TAB_CAP_TILT) && (which & (1u << AX_TILT_X | 1u << AX_TILT_Y))) f->flags |= TF_TILT;
    if ((caps & TAB_CAP_ROTATION) && (which & 1u << AX_ROTATION)) f->flags |= TF_ROTATION;
    if ((caps & TAB_CAP_SLIDER) && (which & 1u << AX_SLIDER)) f->flags |= TF_SLIDER;
}

static void frame_buttons(TabDev *d, TabFrame *f, unsigned want) {
    for (int i = 0; i < 3; i++) {
        unsigned bit = 1u << i;
        if ((want & bit) == (d->sent & bit) || f->nbtn >= TAB_FRAME_BUTTONS) continue;
        f->btn[f->nbtn].code = stylus_code[i];
        f->btn[f->nbtn].down = (want & bit) != 0;
        f->nbtn++;
        f->flags |= TF_BUTTONS;
    }
    d->sent = want;
}

/* the tool leaves proximity: what it held is released first */
static int leave_frame(TabDev *d, TabFrame *f) {
    if (!d->tool) return 0;
    memset(f, 0, sizeof *f);
    f->tool = d->tool;
    f->serial = d->serial;
    f->tool_id = d->tool_id;
    fill_axes(d, f, 0);
    f->flags = TF_PROX_OUT;
    frame_buttons(d, f, 0);
    if (d->down) f->flags |= TF_UP;
    d->down = 0;
    d->tool = 0;
    f->down = 0;
    return 1;
}

int tablet_leave(TabDev *d, TabFrame *out) { return d && d->c.kind == TAB_KIND_PEN ? leave_frame(d, out) : 0; }

static int pen_frame(TabDev *d, TabFrame out[2]) {
    int n = 0;
    unsigned want = wanted_tool(d);
    if (d->tool && want != d->tool) n += leave_frame(d, &out[n]);
    if (want) {
        TabFrame *f = &out[n];
        memset(f, 0, sizeof *f);
        int in = !d->tool;
        d->tool = want;
        f->tool = want;
        f->serial = d->serial;
        f->tool_id = d->tool_id;
        fill_axes(d, f, in ? ~0u : d->dirty);
        if (in) f->flags |= TF_PROX_IN | TF_MOTION;
        int t = tip(d);
        if (t != d->down) f->flags |= t ? TF_DOWN : TF_UP;
        d->down = t;
        f->down = t;
        frame_buttons(d, f, d->held);
        if (d->wheel && !in) { f->wheel = d->wheel; f->flags |= TF_WHEEL; }
        if (f->flags) n++;
    }
    d->dirty = 0;
    d->wheel = 0;
    return n;
}

/* ---- pads ---- */

static int pad_frame(TabDev *d, TabPadFrame *p) {
    memset(p, 0, sizeof *p);
    uint32_t change = d->pad_held ^ d->pad_sent;
    for (int i = 0; i < d->c.nbuttons && change; i++)
        if (change & (1u << i)) { p->btn[p->nbtn].index = (unsigned char)i; p->btn[p->nbtn].down = (d->pad_held >> i) & 1; p->nbtn++; }
    d->pad_sent = d->pad_held;
    for (int i = 0; i < d->c.nrings; i++) {
        if (!(d->ring_dirty & (1u << i))) continue;
        const struct input_absinfo *a = &d->c.ring[i];
        int v = d->ring_v[i];
        /* 0 is sent when the finger lifts (libinput reads it so too); 0 on the ring is north, a quarter turn past
         * the kernel's 0, which is the ring's left */
        if (v == 0) { p->ring_stop |= 1u << i; continue; }
        double u = (double)(v - a->minimum) / (a->maximum - a->minimum + 1) - 0.25;
        if (u < 0) u += 1;
        p->ring[i] = u * 360;
        p->ring_set |= 1u << i;
    }
    d->ring_dirty = 0;
    for (int i = 0; i < d->c.nstrips; i++) {
        if (!(d->strip_dirty & (1u << i))) continue;
        int v = d->strip_v[i], max = d->c.strip[i].maximum;
        /* a strip shifts one bit per position (1, 2, 4, …); 0 only when the finger lifts */
        if (v <= 0) { p->strip_stop |= 1u << i; continue; }
        double u = max > 1 ? log2((double)v) / log2((double)max) : 0;
        p->strip[i] = u < 0 ? 0 : u > 1 ? 1 : u;
        p->strip_set |= 1u << i;
    }
    d->strip_dirty = 0;
    for (int i = 0; i < d->c.ndials; i++) { p->dial[i] = d->dial[i]; d->dial[i] = 0; }
    int any = p->nbtn || p->ring_set || p->ring_stop || p->strip_set || p->strip_stop;
    for (int i = 0; i < d->c.ndials; i++) any |= p->dial[i] != 0;
    return any;
}

static void pad_event(TabDev *d, const struct input_event *ev) {
    if (ev->type == EV_KEY) {
        for (int i = 0; i < d->c.nbuttons; i++)
            if (d->c.button[i] == ev->code) {
                if (ev->value) d->pad_held |= 1u << i; else d->pad_held &= ~(1u << i);
                return;
            }
    } else if (ev->type == EV_ABS) {
        for (int i = 0; i < d->c.nrings; i++) if (d->c.ring_code[i] == ev->code) { d->ring_v[i] = ev->value; d->ring_dirty |= 1u << i; }
        for (int i = 0; i < d->c.nstrips; i++) if (d->c.strip_code[i] == ev->code) { d->strip_v[i] = ev->value; d->strip_dirty |= 1u << i; }
    } else if (ev->type == EV_REL) {
        for (int i = 0; i < d->c.ndials; i++) {
            unsigned code = d->c.dial_code[i];
            if (d->c.dial_hires && code == REL_WHEEL) {
                if (ev->code == REL_WHEEL_HI_RES) d->dial[i] += ev->value;
            } else if (ev->code == code) d->dial[i] += ev->value * 120;
        }
    }
}

int tablet_event(TabDev *d, const struct input_event *ev, TabFrame out[2], TabPadFrame *pad) {
    if (!d) return 0;
    if (ev->type == EV_SYN) {
        if (ev->code == SYN_DROPPED) { d->dropped = 1; return 0; }
        if (ev->code != SYN_REPORT) return 0;
        if (d->dropped) { d->dropped = 0; d->dirty = 0; d->ring_dirty = d->strip_dirty = 0; return 0; }
        if (d->c.kind == TAB_KIND_PAD) return pad && pad_frame(d, pad) ? 1 : 0;
        return pen_frame(d, out);
    }
    if (d->dropped) return 0;
    if (d->c.kind == TAB_KIND_PAD) { pad_event(d, ev); return 0; }
    switch (ev->type) {
    case EV_KEY:
        if (ev->code >= TAB_TOOL_FIRST && ev->code <= TAB_TOOL_LAST && ev->code != BTN_TOOL_FINGER) {
            if (ev->value) d->tool_keys |= TAB_TOOL_BIT(ev->code); else d->tool_keys &= ~TAB_TOOL_BIT(ev->code);
        } else if (ev->code == BTN_TOUCH) d->touch = ev->value != 0;
        else for (int i = 0; i < 3; i++)
            if (ev->code == stylus_code[i]) { if (ev->value) d->held |= 1u << i; else d->held &= ~(1u << i); }
        break;
    case EV_ABS: {
        if (ev->code == ABS_MISC) { if (ev->value) d->tool_id = (unsigned)ev->value; break; }
        int a = axis_of(ev->code);
        if (a < 0) break;
        if (d->v[a] != ev->value) { d->v[a] = ev->value; d->dirty |= 1u << a; }
        else if (a == AX_X || a == AX_Y) d->dirty |= 1u << a;      /* a report at all brings a tool with no BTN_TOOL in */
        break;
    }
    case EV_MSC:
        if (ev->code == MSC_SERIAL && ev->value) d->serial = (uint32_t)ev->value;
        break;
    case EV_REL:                        /* the tool's wheel: hi-res when the device has it, else whole clicks */
        if (!d->c.wheel) break;
        if (d->c.wheel_hires ? ev->code == REL_WHEEL_HI_RES : ev->code == REL_WHEEL) {
            long w = (long)d->wheel + (long)ev->value * (d->c.wheel_hires ? 1 : 120);
            d->wheel = w > 1200000 ? 1200000 : w < -1200000 ? -1200000 : (int)w;
        }
        break;
    default:
        break;
    }
    return 0;
}
