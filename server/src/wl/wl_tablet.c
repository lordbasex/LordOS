/* wl_tablet.c — tablet-v2 (zwp_tablet_manager_v2, version 2): see wl_tablet.h and docs/WAYLAND.md, "Graphics
 * tablets".
 *
 * Every seat a client asks for (get_tablet_seat) is told of each pen (tablet_added), each pad (pad_added, with one
 * group holding all of its buttons, rings, strips and dials, one mode) and each tool seen so far (tool_added). A tool
 * is announced the first time it comes into proximity; one with a hardware serial is the same tool on any tablet, one
 * without belongs to its tablet and is removed with it. Only the app under the tool, or with the keyboard for a pad,
 * gets events: the others never learn where the pen is. */
#include "wl_tablet.h"
#include "wl_input.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "tablet-v2-protocol.h"

typedef struct WtDev WtDev;
typedef struct WtTool WtTool;

/* A resource's user data: what it stands for (NULL once that is gone: the resource is inert) and the tablet seat it
 * was announced on (only compared, to pair a tool with the tablet of the same seat). */
typedef struct { void *obj; struct wl_resource *seat; } Ref;

struct WtDev {
    struct wl_list link;
    TabDev *dev;
    const TabCaps *c;
    struct wl_list res;                 /* zwp_tablet_v2 (a pen) or zwp_tablet_pad_v2 (a pad) */
    struct wl_resource *entered;        /* a pad: the surface its enter named, NULL if none */
    struct wl_listener entered_gone;
};

struct WtTool {
    struct wl_list link;
    unsigned type;                      /* BTN_TOOL_* = zwp_tablet_tool_v2.type */
    uint64_t serial;
    unsigned hwid;
    WtDev *owner;                       /* a tool with no serial belongs to its tablet */
    unsigned caps;
    struct wl_list res;                 /* zwp_tablet_tool_v2 */
    /* where it is */
    WtDev *tablet;                      /* the tablet it is in proximity of, NULL if none */
    struct wl_resource *surface;        /* the surface it is over (proximity_in was sent), NULL if none */
    struct wl_client *client;
    struct wl_listener surface_gone;
    uint32_t prox_serial;
    int grab;                           /* the tip pressed on that surface: it stays there until released */
    int pressed;                        /* that press counts as one for wl.c (wl__tool_press) */
    float px, py;                       /* where on screen */
    /* its state, for a surface it enters */
    TabFrame st;
    unsigned held;                      /* stylus buttons down, bit i = BTN_STYLUS + i */
};

static struct wl_display *dpy;
static const unsigned stylus_code[3] = { BTN_STYLUS, BTN_STYLUS2, BTN_STYLUS3 };   /* bit i of WtTool.held */
static struct wl_list devs, tools, seats, subs;
static int inited;

static uint32_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000);
}
static Ref *ref_new(void *obj, struct wl_resource *seat) {
    Ref *f = calloc(1, sizeof *f);
    if (f) { f->obj = obj; f->seat = seat; }
    return f;
}
static void *ref_obj(struct wl_resource *r) { Ref *f = wl_resource_get_user_data(r); return f ? f->obj : NULL; }
/* a listed resource that goes: out of its list, its Ref freed */
static void listed_gone(struct wl_resource *r) {
    wl_list_remove(wl_resource_get_link(r));
    free(wl_resource_get_user_data(r));
}
/* the thing it stood for is gone: the resource stays (the client destroys it) but nothing is sent to it again */
static void make_inert(struct wl_resource *r) {
    Ref *f = wl_resource_get_user_data(r);
    if (f) f->obj = NULL;
    wl_list_remove(wl_resource_get_link(r));
    wl_list_init(wl_resource_get_link(r));
}
static void res_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }

/* ---- pads' groups, rings, strips and dials: one list, each with the pad resource it was announced on ---- */
enum { SUB_GROUP, SUB_RING, SUB_STRIP, SUB_DIAL };
typedef struct { struct wl_list link; struct wl_resource *res, *pad; int kind, index; } PadSub;
static void sub_gone(struct wl_resource *r) {
    PadSub *s = wl_resource_get_user_data(r);
    if (!s) return;
    wl_list_remove(&s->link);
    free(s);
}
static void ring_feedback(struct wl_client *c, struct wl_resource *r, const char *d, uint32_t serial) { (void)c; (void)r; (void)d; (void)serial; }
static const struct zwp_tablet_pad_group_v2_interface group_impl = { .destroy = res_destroy };
static const struct zwp_tablet_pad_ring_v2_interface ring_impl = { .set_feedback = ring_feedback, .destroy = res_destroy };
static const struct zwp_tablet_pad_strip_v2_interface strip_impl = { .set_feedback = ring_feedback, .destroy = res_destroy };
static const struct zwp_tablet_pad_dial_v2_interface dial_impl = { .set_feedback = ring_feedback, .destroy = res_destroy };
static struct wl_resource *sub_new(struct wl_resource *pad, const struct wl_interface *iface, const void *impl, int kind, int index) {
    struct wl_client *c = wl_resource_get_client(pad);
    PadSub *s = calloc(1, sizeof *s);
    struct wl_resource *r = s ? wl_resource_create(c, iface, wl_resource_get_version(pad), 0) : NULL;
    if (!r) { free(s); wl_client_post_no_memory(c); return NULL; }
    s->res = r;
    s->pad = pad;
    s->kind = kind;
    s->index = index;
    wl_list_insert(&subs, &s->link);
    wl_resource_set_implementation(r, impl, s, sub_gone);
    return r;
}

/* ---- announcing ---- */

static void tablet_iface_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct zwp_tablet_v2_interface tablet_impl = { .destroy = tablet_iface_destroy };

static void announce_tablet(struct wl_resource *seat, WtDev *d) {
    struct wl_client *c = wl_resource_get_client(seat);
    Ref *f = ref_new(d, seat);
    struct wl_resource *r = f ? wl_resource_create(c, &zwp_tablet_v2_interface, wl_resource_get_version(seat), 0) : NULL;
    if (!r) { free(f); wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &tablet_impl, f, listed_gone);
    wl_list_insert(&d->res, wl_resource_get_link(r));
    zwp_tablet_seat_v2_send_tablet_added(seat, r);
    if (d->c->name[0]) zwp_tablet_v2_send_name(r, d->c->name);
    if (d->c->vendor || d->c->product) zwp_tablet_v2_send_id(r, d->c->vendor, d->c->product);
    if (d->c->bus && wl_resource_get_version(r) >= ZWP_TABLET_V2_BUSTYPE_SINCE_VERSION) zwp_tablet_v2_send_bustype(r, d->c->bus);
    if (d->c->path[0]) zwp_tablet_v2_send_path(r, d->c->path);
    zwp_tablet_v2_send_done(r);
}

static void pad_feedback(struct wl_client *c, struct wl_resource *r, uint32_t b, const char *d, uint32_t serial) { (void)c; (void)r; (void)b; (void)d; (void)serial; }
static void pad_gone(struct wl_resource *r) {
    PadSub *s;
    wl_list_for_each(s, &subs, link) if (s->pad == r) s->pad = NULL;    /* its group, rings, … live on, inert */
    listed_gone(r);
}
static const struct zwp_tablet_pad_v2_interface pad_impl = { .set_feedback = pad_feedback, .destroy = res_destroy };

static void announce_pad(struct wl_resource *seat, WtDev *d) {
    struct wl_client *c = wl_resource_get_client(seat);
    Ref *f = ref_new(d, seat);
    struct wl_resource *r = f ? wl_resource_create(c, &zwp_tablet_pad_v2_interface, wl_resource_get_version(seat), 0) : NULL;
    if (!r) { free(f); wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &pad_impl, f, pad_gone);
    wl_list_insert(&d->res, wl_resource_get_link(r));
    zwp_tablet_seat_v2_send_pad_added(seat, r);
    struct wl_resource *g = sub_new(r, &zwp_tablet_pad_group_v2_interface, &group_impl, SUB_GROUP, 0);
    if (g) {
        zwp_tablet_pad_v2_send_group(r, g);
        struct wl_array buttons;
        wl_array_init(&buttons);
        for (int i = 0; i < d->c->nbuttons; i++) {
            uint32_t *b = wl_array_add(&buttons, sizeof *b);
            if (b) *b = (uint32_t)i;
        }
        zwp_tablet_pad_group_v2_send_buttons(g, &buttons);
        wl_array_release(&buttons);
        for (int i = 0; i < d->c->nrings; i++) {
            struct wl_resource *x = sub_new(r, &zwp_tablet_pad_ring_v2_interface, &ring_impl, SUB_RING, i);
            if (x) zwp_tablet_pad_group_v2_send_ring(g, x);
        }
        for (int i = 0; i < d->c->nstrips; i++) {
            struct wl_resource *x = sub_new(r, &zwp_tablet_pad_strip_v2_interface, &strip_impl, SUB_STRIP, i);
            if (x) zwp_tablet_pad_group_v2_send_strip(g, x);
        }
        if (wl_resource_get_version(r) >= ZWP_TABLET_PAD_GROUP_V2_DIAL_SINCE_VERSION)
            for (int i = 0; i < d->c->ndials; i++) {
                struct wl_resource *x = sub_new(r, &zwp_tablet_pad_dial_v2_interface, &dial_impl, SUB_DIAL, i);
                if (x) zwp_tablet_pad_group_v2_send_dial(g, x);
            }
        zwp_tablet_pad_group_v2_send_done(g);     /* one mode: no modes event */
    }
    if (d->c->path[0]) zwp_tablet_pad_v2_send_path(r, d->c->path);
    if (d->c->nbuttons) zwp_tablet_pad_v2_send_buttons(r, (uint32_t)d->c->nbuttons);
    zwp_tablet_pad_v2_send_done(r);
}

static void tool_set_cursor(struct wl_client *c, struct wl_resource *r, uint32_t serial, struct wl_resource *surface, int32_t x, int32_t y) {
    WtTool *t = ref_obj(r);
    /* only the app the tool is over, with the serial of that proximity_in */
    if (!t || !t->surface || t->client != c || serial != t->prox_serial) return;
    wl__tool_cursor(surface, x, y);
}
static const struct zwp_tablet_tool_v2_interface tool_impl = { .set_cursor = tool_set_cursor, .destroy = res_destroy };

static void announce_tool(struct wl_resource *seat, WtTool *t) {
    struct wl_client *c = wl_resource_get_client(seat);
    Ref *f = ref_new(t, seat);
    struct wl_resource *r = f ? wl_resource_create(c, &zwp_tablet_tool_v2_interface, wl_resource_get_version(seat), 0) : NULL;
    if (!r) { free(f); wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &tool_impl, f, listed_gone);
    wl_list_insert(&t->res, wl_resource_get_link(r));
    zwp_tablet_seat_v2_send_tool_added(seat, r);
    zwp_tablet_tool_v2_send_type(r, t->type);
    if (t->serial) zwp_tablet_tool_v2_send_hardware_serial(r, (uint32_t)(t->serial >> 32), (uint32_t)t->serial);
    if (t->hwid) zwp_tablet_tool_v2_send_hardware_id_wacom(r, 0, t->hwid);
    if (t->caps & TAB_CAP_TILT) zwp_tablet_tool_v2_send_capability(r, ZWP_TABLET_TOOL_V2_CAPABILITY_TILT);
    if (t->caps & TAB_CAP_PRESSURE) zwp_tablet_tool_v2_send_capability(r, ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE);
    if (t->caps & TAB_CAP_DISTANCE) zwp_tablet_tool_v2_send_capability(r, ZWP_TABLET_TOOL_V2_CAPABILITY_DISTANCE);
    if (t->caps & TAB_CAP_ROTATION) zwp_tablet_tool_v2_send_capability(r, ZWP_TABLET_TOOL_V2_CAPABILITY_ROTATION);
    if (t->caps & TAB_CAP_SLIDER) zwp_tablet_tool_v2_send_capability(r, ZWP_TABLET_TOOL_V2_CAPABILITY_SLIDER);
    zwp_tablet_tool_v2_send_done(r);
}

/* ---- the manager and the seats ---- */

static void seat_gone(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }
static const struct zwp_tablet_seat_v2_interface seat_impl = { .destroy = res_destroy };
static void mgr_get_seat(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *wl_seat) {
    (void)wl_seat;
    struct wl_resource *s = wl_resource_create(c, &zwp_tablet_seat_v2_interface, wl_resource_get_version(r), id);
    if (!s) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(s, &seat_impl, NULL, seat_gone);
    wl_list_insert(&seats, wl_resource_get_link(s));
    WtDev *d;
    wl_list_for_each_reverse(d, &devs, link) {
        if (d->c->kind == TAB_KIND_PEN) announce_tablet(s, d);
        else announce_pad(s, d);
    }
    WtTool *t;
    wl_list_for_each_reverse(t, &tools, link) announce_tool(s, t);
    /* a pad already focused on this client's window: it hears of it at its next event (wl_tablet_pad) */
}
static const struct zwp_tablet_manager_v2_interface mgr_impl = { .get_tablet_seat = mgr_get_seat, .destroy = res_destroy };
static void bind_mgr(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zwp_tablet_manager_v2_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &mgr_impl, NULL, NULL);
}

/* ---- devices (tablet.c's host) ---- */

static void tool_unfocus(WtTool *t, int release);
static void pad_leave(WtDev *d);

static void dev_added(TabDev *dev) {
    WtDev *d = calloc(1, sizeof *d);
    if (!d) return;
    d->dev = dev;
    d->c = tablet_caps(dev);
    wl_list_init(&d->res);
    wl_list_insert(&devs, &d->link);
    tablet_set_user(dev, d);
    struct wl_resource *s;
    wl_resource_for_each(s, &seats) {
        if (d->c->kind == TAB_KIND_PEN) announce_tablet(s, d);
        else announce_pad(s, d);
    }
}

static void tool_free(WtTool *t) {
    struct wl_resource *r, *tmp;
    wl_resource_for_each_safe(r, tmp, &t->res) { zwp_tablet_tool_v2_send_removed(r); make_inert(r); }
    wl_list_remove(&t->link);
    free(t);
}

static void dev_removed(TabDev *dev) {
    WtDev *d = tablet_user(dev);
    if (!d) return;
    WtTool *t, *tt;
    wl_list_for_each_safe(t, tt, &tools, link) {
        if (t->tablet == d) { tool_unfocus(t, 1); t->tablet = NULL; }
        if (t->owner == d) tool_free(t);                /* a tool with no serial goes with its tablet */
    }
    if (d->c->kind == TAB_KIND_PAD) pad_leave(d);
    struct wl_resource *r, *tmp;
    wl_resource_for_each_safe(r, tmp, &d->res) {
        if (d->c->kind == TAB_KIND_PEN) zwp_tablet_v2_send_removed(r);
        else zwp_tablet_pad_v2_send_removed(r);
        make_inert(r);
    }
    wl_list_remove(&d->link);
    tablet_set_user(dev, NULL);
    free(d);
}

static const TabHost host = { dev_added, dev_removed };
void wl_tablet_init(void) {
    if (inited) return;
    inited = 1;
    wl_list_init(&devs);
    wl_list_init(&tools);
    wl_list_init(&seats);
    wl_list_init(&subs);
    tablet_set_host(&host);
}

static struct wl_listener dpy_gone;
static void on_dpy_gone(struct wl_listener *l, void *data) { (void)l; (void)data; dpy = NULL; }
void wl_tablet_globals(struct wl_display *d) {
    wl_tablet_init();
    dpy = d;
    dpy_gone.notify = on_dpy_gone;
    wl_display_add_destroy_listener(d, &dpy_gone);
    wl_global_create(d, &zwp_tablet_manager_v2_interface, 2, NULL, bind_mgr);
}

/* ---- tools ---- */

static WtTool *tool_for(WtDev *d, const TabFrame *f) {
    WtTool *t;
    wl_list_for_each(t, &tools, link)
        if (t->type == f->tool && t->serial == f->serial && (f->serial || t->owner == d)) return t;
    t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->type = f->tool;
    t->serial = f->serial;
    t->hwid = f->tool_id;
    t->owner = f->serial ? NULL : d;
    t->caps = tablet_tool_caps(d->dev, f->tool);
    wl_list_init(&t->res);
    wl_list_init(&t->surface_gone.link);
    wl_list_insert(&tools, &t->link);
    struct wl_resource *s;
    wl_resource_for_each(s, &seats) announce_tool(s, t);
    return t;
}

/* the tablet resource of client c to name with tool resource r: the one of the same seat if there is one */
static struct wl_resource *tablet_res(WtDev *d, struct wl_resource *tool) {
    struct wl_client *c = wl_resource_get_client(tool);
    struct wl_resource *r, *any = NULL;
    Ref *tf = wl_resource_get_user_data(tool);
    wl_resource_for_each(r, &d->res) {
        if (wl_resource_get_client(r) != c) continue;
        Ref *f = wl_resource_get_user_data(r);
        if (f && tf && f->seat == tf->seat) return r;
        if (!any) any = r;
    }
    return any;
}
/* does client c listen to this tool on this tablet? */
static int client_takes(WtDev *d, WtTool *t, struct wl_client *c) {
    struct wl_resource *r;
    wl_resource_for_each(r, &t->res) if (wl_resource_get_client(r) == c && tablet_res(d, r)) return 1;
    return 0;
}

#define FOR_FOCUS(t, r) wl_resource_for_each(r, &(t)->res) if (wl_resource_get_client(r) == (t)->client)

static void on_surface_gone(struct wl_listener *l, void *data) {
    (void)data;
    WtTool *t = wl_container_of(l, t, surface_gone);
    wl_list_remove(&l->link);
    wl_list_init(&l->link);
    if (t->pressed) wl__tool_press(NULL, 0, 0, 0, 0);
    t->surface = NULL;
    t->client = NULL;
    t->grab = t->pressed = 0;
}

static void send_axes(WtTool *t, struct wl_resource *r, unsigned flags, double lx, double ly) {
    const TabFrame *s = &t->st;
    if (flags & TF_MOTION) zwp_tablet_tool_v2_send_motion(r, wl_fixed_from_double(lx), wl_fixed_from_double(ly));
    if (flags & TF_PRESSURE) zwp_tablet_tool_v2_send_pressure(r, (uint32_t)(s->pressure * 65535 + 0.5));
    if (flags & TF_DISTANCE) zwp_tablet_tool_v2_send_distance(r, (uint32_t)(s->distance * 65535 + 0.5));
    if (flags & TF_TILT) zwp_tablet_tool_v2_send_tilt(r, wl_fixed_from_double(s->tilt_x), wl_fixed_from_double(s->tilt_y));
    if (flags & TF_ROTATION) zwp_tablet_tool_v2_send_rotation(r, wl_fixed_from_double(s->rotation));
    if (flags & TF_SLIDER) zwp_tablet_tool_v2_send_slider(r, (int32_t)(s->slider * 65535));
}

/* the surface's coordinates the app uses: X pixels for a scaled Xwayland */
static void to_client(struct wl_client *c, float *lx, float *ly) {
    float k = wl__client_scale(c);
    *lx *= k;
    *ly *= k;
}

/* The tool leaves the surface it was over: what it holds is let go, then proximity_out. */
static void tool_unfocus(WtTool *t, int release) {
    if (!t->surface) return;
    struct wl_resource *r;
    uint32_t time = now_ms();
    FOR_FOCUS(t, r) {
        if (release)
            for (int i = 0; i < 3; i++)
                if (t->held & (1u << i)) zwp_tablet_tool_v2_send_button(r, wl_display_next_serial(dpy), stylus_code[i], ZWP_TABLET_TOOL_V2_BUTTON_STATE_RELEASED);
        if (t->st.down) zwp_tablet_tool_v2_send_up(r);
        zwp_tablet_tool_v2_send_proximity_out(r);
        zwp_tablet_tool_v2_send_frame(r, time);
    }
    if (t->pressed) wl__tool_press(NULL, 0, 0, 0, 0);
    wl_list_remove(&t->surface_gone.link);
    wl_list_init(&t->surface_gone.link);
    t->surface = NULL;
    t->client = NULL;
    t->grab = t->pressed = 0;
}

static unsigned stylus_bit(unsigned code) {
    return code == BTN_STYLUS ? 1u : code == BTN_STYLUS2 ? 2u : code == BTN_STYLUS3 ? 4u : 0;
}

/* The tool comes over a surface: proximity_in and everything it has now, in one frame. */
static void tool_focus(WtTool *t, WtDev *d, struct wl_resource *surface, float lx, float ly) {
    t->surface = surface;
    t->client = wl_resource_get_client(surface);
    t->surface_gone.notify = on_surface_gone;
    wl_resource_add_destroy_listener(surface, &t->surface_gone);
    wl__pointer_leave();                    /* the app's toolkit follows the tool, not the mouse */
    to_client(t->client, &lx, &ly);
    t->prox_serial = wl_display_next_serial(dpy);
    uint32_t time = now_ms();
    unsigned axes = TF_MOTION;
    if (t->caps & TAB_CAP_PRESSURE) axes |= TF_PRESSURE;
    if (t->caps & TAB_CAP_DISTANCE) axes |= TF_DISTANCE;
    if (t->caps & TAB_CAP_TILT) axes |= TF_TILT;
    if (t->caps & TAB_CAP_ROTATION) axes |= TF_ROTATION;
    if (t->caps & TAB_CAP_SLIDER) axes |= TF_SLIDER;
    struct wl_resource *r;
    FOR_FOCUS(t, r) {
        struct wl_resource *tab = tablet_res(d, r);
        if (!tab) continue;
        zwp_tablet_tool_v2_send_proximity_in(r, t->prox_serial, tab, surface);
        send_axes(t, r, axes, lx, ly);
        if (t->st.down) zwp_tablet_tool_v2_send_down(r, t->prox_serial);
        for (int i = 0; i < 3; i++)
            if (t->held & (1u << i)) zwp_tablet_tool_v2_send_button(r, t->prox_serial, stylus_code[i], ZWP_TABLET_TOOL_V2_BUTTON_STATE_PRESSED);
        zwp_tablet_tool_v2_send_frame(r, time);
    }
}

static void keep_state(WtTool *t, const TabFrame *f) {
    TabFrame *s = &t->st;
    if (f->flags & TF_PRESSURE) s->pressure = f->pressure;
    if (f->flags & TF_DISTANCE) s->distance = f->distance;
    if (f->flags & TF_TILT) { s->tilt_x = f->tilt_x; s->tilt_y = f->tilt_y; }
    if (f->flags & TF_ROTATION) s->rotation = f->rotation;
    if (f->flags & TF_SLIDER) s->slider = f->slider;
    s->down = f->down;
    for (int i = 0; i < f->nbtn; i++) {
        unsigned b = stylus_bit(f->btn[i].code);
        if (f->btn[i].down) t->held |= b; else t->held &= ~b;
    }
}

int wl_tablet_tool(TabDev *dev, const TabFrame *f, float x, float y) {
    WtDev *d = tablet_user(dev);
    if (!dpy || !d) return 0;
    WtTool *t = tool_for(d, f);
    if (!t) return 0;
    if (f->flags & TF_PROX_OUT) {
        int had = t->surface != NULL;
        tool_unfocus(t, 1);
        keep_state(t, f);
        t->tablet = NULL;
        return had;
    }
    int was_down = t->st.down;
    t->tablet = d;
    t->px = x;
    t->py = y;
    struct wl_resource *target = NULL;
    float lx = 0, ly = 0;
    if (t->surface && t->grab) {                /* the tip is down: the surface it pressed keeps it */
        float ox, oy, w, h;
        if (wl__surface_box(t->surface, &ox, &oy, &w, &h)) { target = t->surface; lx = x - ox; ly = y - oy; }
    } else {
        target = wl__surface_at(x, y, &lx, &ly);
        if (target && (!client_takes(d, t, wl_resource_get_client(target)) || wl__surface_held(target))) target = NULL;
        /* the tip came down elsewhere (LordOS's UI took it): no app gets a stroke that began outside it */
        if (target && was_down && f->down && !(f->flags & TF_DOWN) && target != t->surface) target = NULL;
    }
    if (target != t->surface) {
        tool_unfocus(t, 1);                     /* with what the old surface was told */
        keep_state(t, f);
        if (!target) return 0;
        tool_focus(t, d, target, lx, ly);
        if (t->st.down) {                       /* it came in pressing (the press is in this frame): the app's */
            t->grab = t->pressed = 1;
            wl__tool_press(target, x, y, 1, t->prox_serial);
        }
        return 1;
    }
    keep_state(t, f);
    if (!target) return 0;
    to_client(t->client, &lx, &ly);
    uint32_t time = now_ms(), serial = 0;
    if (f->flags & (TF_DOWN | TF_BUTTONS)) serial = wl_display_next_serial(dpy);
    struct wl_resource *r;
    FOR_FOCUS(t, r) {
        send_axes(t, r, f->flags, lx, ly);
        if (f->flags & TF_DOWN) zwp_tablet_tool_v2_send_down(r, serial);
        if (f->flags & TF_UP) zwp_tablet_tool_v2_send_up(r);
        for (int i = 0; i < f->nbtn; i++)
            zwp_tablet_tool_v2_send_button(r, serial, f->btn[i].code, f->btn[i].down ? ZWP_TABLET_TOOL_V2_BUTTON_STATE_PRESSED : ZWP_TABLET_TOOL_V2_BUTTON_STATE_RELEASED);
        zwp_tablet_tool_v2_send_frame(r, time);
    }
    if (f->flags & TF_DOWN) {
        t->grab = 1;
        if (!t->pressed) { t->pressed = 1; wl__tool_press(target, x, y, 1, serial); }
    }
    if (f->flags & TF_UP) {
        t->grab = 0;
        if (t->pressed) { t->pressed = 0; wl__tool_press(NULL, 0, 0, 0, 0); }
    }
    return 1;
}

void wl_tablet_tool_away(TabDev *dev, const TabFrame *f) {
    WtDev *d = tablet_user(dev);
    if (!dpy || !d) return;
    WtTool *t = tool_for(d, f);
    if (!t) return;
    tool_unfocus(t, 1);
    keep_state(t, f);                           /* what it has now, for the surface it may enter next */
    t->tablet = f->flags & TF_PROX_OUT ? NULL : d;
}

int wl_tablet_tool_over(struct wl_resource *tool, struct wl_client *c) {
    if (!inited) return 0;
    WtTool *t;
    wl_list_for_each(t, &tools, link) {
        if (!t->surface || t->client != c) continue;
        struct wl_resource *r;
        wl_resource_for_each(r, &t->res) if (r == tool) return 1;
    }
    return 0;
}

/* ---- pads ---- */

static struct wl_resource *key_surface;   /* the window with the keyboard */
static struct wl_listener key_gone;

/* the pen a pad belongs to: the same USB device (EVIOCGPHYS), else the same vendor and product, else any pen */
static WtDev *pad_tablet(WtDev *pad) {
    WtDev *d, *same = NULL, *any = NULL;
    wl_list_for_each(d, &devs, link) {
        if (d->c->kind != TAB_KIND_PEN) continue;
        if (pad->c->phys[0] && !strcmp(d->c->phys, pad->c->phys)) return d;
        if (!same && d->c->vendor == pad->c->vendor && d->c->product == pad->c->product) same = d;
        if (!any) any = d;
    }
    return same ? same : any;
}
static void on_entered_gone(struct wl_listener *l, void *data) {
    (void)data;
    WtDev *d = wl_container_of(l, d, entered_gone);
    wl_list_remove(&l->link);
    wl_list_init(&l->link);
    d->entered = NULL;
}
static void pad_leave(WtDev *d) {
    if (!d->entered) return;
    struct wl_client *c = wl_resource_get_client(d->entered);
    struct wl_resource *r;
    wl_resource_for_each(r, &d->res)
        if (wl_resource_get_client(r) == c) zwp_tablet_pad_v2_send_leave(r, wl_display_next_serial(dpy), d->entered);
    wl_list_remove(&d->entered_gone.link);
    wl_list_init(&d->entered_gone.link);
    d->entered = NULL;
}
/* the pad on the window with the keyboard: enter (and each group's mode) for the app's pads, once */
static int pad_enter(WtDev *d) {
    if (d->entered == key_surface) return d->entered != NULL;
    pad_leave(d);
    WtDev *tab = pad_tablet(d);
    if (!key_surface || !tab) return 0;
    struct wl_client *c = wl_resource_get_client(key_surface);
    uint32_t serial = wl_display_next_serial(dpy), time = now_ms();
    int any = 0;
    struct wl_resource *r;
    wl_resource_for_each(r, &d->res) {
        if (wl_resource_get_client(r) != c) continue;
        struct wl_resource *tr, *tabr = NULL;
        wl_resource_for_each(tr, &tab->res) if (wl_resource_get_client(tr) == c) { tabr = tr; break; }
        if (!tabr) continue;
        zwp_tablet_pad_v2_send_enter(r, serial, tabr, key_surface);
        PadSub *s;
        wl_list_for_each(s, &subs, link)
            if (s->pad == r && s->kind == SUB_GROUP) zwp_tablet_pad_group_v2_send_mode_switch(s->res, time, serial, 0);
        any = 1;
    }
    if (!any) return 0;
    d->entered = key_surface;
    d->entered_gone.notify = on_entered_gone;
    wl_resource_add_destroy_listener(key_surface, &d->entered_gone);
    return 1;
}

static void on_key_gone(struct wl_listener *l, void *data) {
    (void)data;
    wl_list_remove(&l->link);
    wl_list_init(&l->link);
    key_surface = NULL;
}

void wl_tablet_key_focus(struct wl_resource *surface) {
    if (!inited || !dpy) return;
    if (key_surface == surface) return;
    if (key_surface) { wl_list_remove(&key_gone.link); wl_list_init(&key_gone.link); }
    key_surface = surface;
    if (surface) { key_gone.notify = on_key_gone; wl_resource_add_destroy_listener(surface, &key_gone); }
    WtDev *d;
    wl_list_for_each(d, &devs, link) if (d->c->kind == TAB_KIND_PAD) { pad_leave(d); if (key_surface) pad_enter(d); }
}

void wl_tablet_pad(TabDev *dev, const TabPadFrame *p) {
    WtDev *d = tablet_user(dev);
    if (!dpy || !d || !pad_enter(d)) return;
    struct wl_client *c = wl_resource_get_client(d->entered);
    uint32_t time = now_ms();
    struct wl_resource *r;
    wl_resource_for_each(r, &d->res) {
        if (wl_resource_get_client(r) != c) continue;
        for (int i = 0; i < p->nbtn; i++)
            zwp_tablet_pad_v2_send_button(r, time, p->btn[i].index, p->btn[i].down ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
        PadSub *s;
        wl_list_for_each(s, &subs, link) {
            if (s->pad != r) continue;
            unsigned bit = 1u << s->index;
            if (s->kind == SUB_RING && ((p->ring_set | p->ring_stop) & bit)) {
                zwp_tablet_pad_ring_v2_send_source(s->res, ZWP_TABLET_PAD_RING_V2_SOURCE_FINGER);
                if (p->ring_set & bit) zwp_tablet_pad_ring_v2_send_angle(s->res, wl_fixed_from_double(p->ring[s->index]));
                else zwp_tablet_pad_ring_v2_send_stop(s->res);
                zwp_tablet_pad_ring_v2_send_frame(s->res, time);
            } else if (s->kind == SUB_STRIP && ((p->strip_set | p->strip_stop) & bit)) {
                zwp_tablet_pad_strip_v2_send_source(s->res, ZWP_TABLET_PAD_STRIP_V2_SOURCE_FINGER);
                if (p->strip_set & bit) zwp_tablet_pad_strip_v2_send_position(s->res, (uint32_t)(p->strip[s->index] * 65535 + 0.5));
                else zwp_tablet_pad_strip_v2_send_stop(s->res);
                zwp_tablet_pad_strip_v2_send_frame(s->res, time);
            } else if (s->kind == SUB_DIAL && p->dial[s->index]) {
                zwp_tablet_pad_dial_v2_send_delta(s->res, p->dial[s->index]);
                zwp_tablet_pad_dial_v2_send_frame(s->res, time);
            }
        }
    }
}
