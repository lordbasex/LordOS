/* wltablet_test.c — tablet-v2 and pointer-warp-v1 on the wire: real libwayland clients against src/wl/wl_tablet.c
 * and src/wl/wl_warp.c (the compositor side and its scene are in wltablet_srv.c). Pens and pads come from tablet.c
 * fed synthetic evdev events, as a Wacom Intuos sends them.
 *
 * Client A binds tablet-v2 and pointer-warp; client B binds neither (as an app that never asked for tablets).
 *   - A's seat hears of the tablet (name, id, bustype, path), the pad (one group: buttons, ring, strip, dial) and
 *     each tool when it first comes near (type, serial, Wacom id, capabilities);
 *   - over A's surface: proximity_in, motion in surface coordinates, axes, down, buttons, up, frame; the pointer
 *     leaves A; a press is noted for move and resize; the tip keeps the surface while down; away from it,
 *     buttons released and proximity_out, in that order; over B's surface or a window under a modal dialog the
 *     pointer takes over (wl_tablet_tool says 0) and B hears nothing;
 *   - set_cursor only with the proximity_in serial; a scaled Xwayland gets X pixels;
 *   - a pad follows the keyboard: enter and mode_switch, buttons, ring, strip, dial; leave when the keyboard goes;
 *   - a tablet unplugged: its tools with no serial are removed with it;
 *   - pointer-warp: done only with pointer focus, the enter serial, the keyboard, inside the surface, unlocked. */
#include "../src/tablet.h"
#include "../src/wl/wl_tablet.h"
#include "wltablet_srv.h"

#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include "tablet-v2-client-protocol.h"
#include "pointer-warp-v1-client-protocol.h"

/* The objects the server announces (tools, groups, rings…) are proxies libwayland-client makes; the test doesn't destroy
 * each one before disconnecting, so LeakSanitizer would count them. The server side is what this checks. */
const char *__asan_default_options(void);
const char *__asan_default_options(void) { return "detect_leaks=0"; }

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); fails++; } } while (0)

/* ---- what client A heard, one line per event ---- */
static char logbuf[1 << 16];
static size_t loglen;
static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(logbuf + loglen, sizeof logbuf - loglen - 1, fmt, ap);
    va_end(ap);
    if (n > 0 && loglen + (size_t)n + 1 < sizeof logbuf) { loglen += (size_t)n; logbuf[loglen++] = '\n'; logbuf[loglen] = 0; }
}
static void clear(void) {
    if (getenv("WLTABLET_LOG")) printf("----\n%s", logbuf);     /* what A heard, step by step */
    loglen = 0;
    logbuf[0] = 0;
}
static const char *has(const char *line) {
    size_t n = strlen(line);
    for (const char *p = logbuf; (p = strstr(p, line)); p++)
        if ((p == logbuf || p[-1] == '\n') && (p[n] == '\n' || p[n] == 0)) return p;
    return NULL;
}
static int before(const char *a, const char *b) { const char *x = has(a), *y = has(b); return x && y && x < y; }

typedef struct {
    struct wl_display *dpy;
    struct wl_registry *reg;
    struct wl_compositor *comp;
    struct wl_seat *seat;
    struct zwp_tablet_manager_v2 *mgr;
    struct wp_pointer_warp_v1 *warp;
    uint32_t mgr_version;
} Client;
static Client A, B;
static struct wl_surface *s1, *s3;          /* A's (surfaces 1 and 3); B's is surface 2 */
static struct zwp_tablet_tool_v2 *last_tool;
static uint32_t prox_serial;
static struct zwp_tablet_v2 *tablets[4];
static int ntablets;

static void pump(void) {
    Client *cs[2] = { &A, &B };
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 2; i++) wl_display_flush(cs[i]->dpy);
        srv_dispatch();
        for (int i = 0; i < 2; i++) {
            struct wl_display *d = cs[i]->dpy;
            while (wl_display_prepare_read(d) != 0) wl_display_dispatch_pending(d);
            struct pollfd p = { wl_display_get_fd(d), POLLIN, 0 };
            if (poll(&p, 1, 2) > 0) wl_display_read_events(d); else wl_display_cancel_read(d);
            wl_display_dispatch_pending(d);
        }
    }
}

/* ---- listeners ---- */
static const char *surf_name(struct wl_surface *s) { return s == s1 ? "s1" : s == s3 ? "s3" : s ? "other" : "none"; }

static void t_name(void *d, struct zwp_tablet_v2 *t, const char *n) { (void)d; (void)t; note("tablet.name %s", n); }
static void t_id(void *d, struct zwp_tablet_v2 *t, uint32_t v, uint32_t p) { (void)d; (void)t; note("tablet.id %x %x", v, p); }
static void t_path(void *d, struct zwp_tablet_v2 *t, const char *p) { (void)d; (void)t; note("tablet.path %s", p); }
static void t_done(void *d, struct zwp_tablet_v2 *t) { (void)d; (void)t; note("tablet.done"); }
static void t_removed(void *d, struct zwp_tablet_v2 *t) { (void)d; note("tablet.removed"); zwp_tablet_v2_destroy(t); }
static void t_bustype(void *d, struct zwp_tablet_v2 *t, uint32_t b) { (void)d; (void)t; note("tablet.bustype %u", b); }
static const struct zwp_tablet_v2_listener tablet_l = { t_name, t_id, t_path, t_done, t_removed, t_bustype };

static void o_type(void *d, struct zwp_tablet_tool_v2 *t, uint32_t v) { (void)d; (void)t; note("tool.type %x", v); }
static void o_serial(void *d, struct zwp_tablet_tool_v2 *t, uint32_t hi, uint32_t lo) { (void)d; (void)t; note("tool.hardware_serial %x %x", hi, lo); }
static void o_hwid(void *d, struct zwp_tablet_tool_v2 *t, uint32_t hi, uint32_t lo) { (void)d; (void)t; note("tool.hardware_id_wacom %x %x", hi, lo); }
static void o_cap(void *d, struct zwp_tablet_tool_v2 *t, uint32_t c) { (void)d; (void)t; note("tool.capability %u", c); }
static void o_done(void *d, struct zwp_tablet_tool_v2 *t) { (void)d; (void)t; note("tool.done"); }
static void o_removed(void *d, struct zwp_tablet_tool_v2 *t) { (void)d; note("tool.removed"); if (t == last_tool) last_tool = NULL; zwp_tablet_tool_v2_destroy(t); }
static void o_in(void *d, struct zwp_tablet_tool_v2 *t, uint32_t serial, struct zwp_tablet_v2 *tab, struct wl_surface *s) {
    (void)d; (void)t;
    prox_serial = serial;
    note("tool.proximity_in %s %s", tab ? "tablet" : "no-tablet", surf_name(s));
}
static void o_out(void *d, struct zwp_tablet_tool_v2 *t) { (void)d; (void)t; note("tool.proximity_out"); }
static void o_down(void *d, struct zwp_tablet_tool_v2 *t, uint32_t serial) { (void)d; (void)t; (void)serial; note("tool.down"); }
static void o_up(void *d, struct zwp_tablet_tool_v2 *t) { (void)d; (void)t; note("tool.up"); }
static void o_motion(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)t; note("tool.motion %.1f %.1f", wl_fixed_to_double(x), wl_fixed_to_double(y)); }
static void o_pressure(void *d, struct zwp_tablet_tool_v2 *t, uint32_t p) { (void)d; (void)t; note("tool.pressure %u", p); }
static void o_distance(void *d, struct zwp_tablet_tool_v2 *t, uint32_t p) { (void)d; (void)t; note("tool.distance %u", p); }
static void o_tilt(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)t; note("tool.tilt %.0f %.0f", wl_fixed_to_double(x), wl_fixed_to_double(y)); }
static void o_rotation(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t r) { (void)d; (void)t; note("tool.rotation %.0f", wl_fixed_to_double(r)); }
static void o_slider(void *d, struct zwp_tablet_tool_v2 *t, int32_t p) { (void)d; (void)t; note("tool.slider %d", p); }
static void o_wheel(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t deg, int32_t c) { (void)d; (void)t; (void)deg; note("tool.wheel %d", c); }
static void o_button(void *d, struct zwp_tablet_tool_v2 *t, uint32_t serial, uint32_t b, uint32_t st) { (void)d; (void)t; (void)serial; note("tool.button %x %u", b, st); }
static void o_frame(void *d, struct zwp_tablet_tool_v2 *t, uint32_t time) { (void)d; (void)t; (void)time; note("tool.frame"); }
static const struct zwp_tablet_tool_v2_listener tool_l = {
    o_type, o_serial, o_hwid, o_cap, o_done, o_removed, o_in, o_out, o_down, o_up, o_motion, o_pressure, o_distance,
    o_tilt, o_rotation, o_slider, o_wheel, o_button, o_frame,
};

static void r_source(void *d, struct zwp_tablet_pad_ring_v2 *r, uint32_t s) { (void)d; (void)r; note("ring.source %u", s); }
static void r_angle(void *d, struct zwp_tablet_pad_ring_v2 *r, wl_fixed_t a) { (void)d; (void)r; note("ring.angle %.0f", wl_fixed_to_double(a)); }
static void r_stop(void *d, struct zwp_tablet_pad_ring_v2 *r) { (void)d; (void)r; note("ring.stop"); }
static void r_frame(void *d, struct zwp_tablet_pad_ring_v2 *r, uint32_t t) { (void)d; (void)r; (void)t; note("ring.frame"); }
static const struct zwp_tablet_pad_ring_v2_listener ring_l = { r_source, r_angle, r_stop, r_frame };
static void s_source(void *d, struct zwp_tablet_pad_strip_v2 *r, uint32_t s) { (void)d; (void)r; note("strip.source %u", s); }
static void s_pos(void *d, struct zwp_tablet_pad_strip_v2 *r, uint32_t p) { (void)d; (void)r; note("strip.position %u", p); }
static void s_stop(void *d, struct zwp_tablet_pad_strip_v2 *r) { (void)d; (void)r; note("strip.stop"); }
static void s_frame(void *d, struct zwp_tablet_pad_strip_v2 *r, uint32_t t) { (void)d; (void)r; (void)t; note("strip.frame"); }
static const struct zwp_tablet_pad_strip_v2_listener strip_l = { s_source, s_pos, s_stop, s_frame };
static void dl_delta(void *d, struct zwp_tablet_pad_dial_v2 *r, int32_t v) { (void)d; (void)r; note("dial.delta %d", v); }
static void dl_frame(void *d, struct zwp_tablet_pad_dial_v2 *r, uint32_t t) { (void)d; (void)r; (void)t; note("dial.frame"); }
static const struct zwp_tablet_pad_dial_v2_listener dial_l = { dl_delta, dl_frame };

static void g_buttons(void *d, struct zwp_tablet_pad_group_v2 *g, struct wl_array *a) { (void)d; (void)g; note("group.buttons %zu", a->size / sizeof(uint32_t)); }
static void g_ring(void *d, struct zwp_tablet_pad_group_v2 *g, struct zwp_tablet_pad_ring_v2 *r) { (void)d; (void)g; note("group.ring"); zwp_tablet_pad_ring_v2_add_listener(r, &ring_l, NULL); }
static void g_strip(void *d, struct zwp_tablet_pad_group_v2 *g, struct zwp_tablet_pad_strip_v2 *r) { (void)d; (void)g; note("group.strip"); zwp_tablet_pad_strip_v2_add_listener(r, &strip_l, NULL); }
static void g_modes(void *d, struct zwp_tablet_pad_group_v2 *g, uint32_t m) { (void)d; (void)g; note("group.modes %u", m); }
static void g_done(void *d, struct zwp_tablet_pad_group_v2 *g) { (void)d; (void)g; note("group.done"); }
static void g_mode(void *d, struct zwp_tablet_pad_group_v2 *g, uint32_t t, uint32_t s, uint32_t m) { (void)d; (void)g; (void)t; (void)s; note("group.mode_switch %u", m); }
static void g_dial(void *d, struct zwp_tablet_pad_group_v2 *g, struct zwp_tablet_pad_dial_v2 *r) { (void)d; (void)g; note("group.dial"); zwp_tablet_pad_dial_v2_add_listener(r, &dial_l, NULL); }
static const struct zwp_tablet_pad_group_v2_listener group_l = { g_buttons, g_ring, g_strip, g_modes, g_done, g_mode, g_dial };

static void p_group(void *d, struct zwp_tablet_pad_v2 *p, struct zwp_tablet_pad_group_v2 *g) { (void)d; (void)p; note("pad.group"); zwp_tablet_pad_group_v2_add_listener(g, &group_l, NULL); }
static void p_path(void *d, struct zwp_tablet_pad_v2 *p, const char *s) { (void)d; (void)p; note("pad.path %s", s); }
static void p_buttons(void *d, struct zwp_tablet_pad_v2 *p, uint32_t n) { (void)d; (void)p; note("pad.buttons %u", n); }
static void p_done(void *d, struct zwp_tablet_pad_v2 *p) { (void)d; (void)p; note("pad.done"); }
static void p_button(void *d, struct zwp_tablet_pad_v2 *p, uint32_t t, uint32_t b, uint32_t s) { (void)d; (void)p; (void)t; note("pad.button %u %u", b, s); }
static void p_enter(void *d, struct zwp_tablet_pad_v2 *p, uint32_t serial, struct zwp_tablet_v2 *tab, struct wl_surface *s) { (void)d; (void)p; (void)serial; note("pad.enter %s %s", tab ? "tablet" : "no-tablet", surf_name(s)); }
static void p_leave(void *d, struct zwp_tablet_pad_v2 *p, uint32_t serial, struct wl_surface *s) { (void)d; (void)p; (void)serial; note("pad.leave %s", surf_name(s)); }
static void p_removed(void *d, struct zwp_tablet_pad_v2 *p) { (void)d; (void)p; note("pad.removed"); }
static const struct zwp_tablet_pad_v2_listener pad_l = { p_group, p_path, p_buttons, p_done, p_button, p_enter, p_leave, p_removed };

static void seat_tablet(void *d, struct zwp_tablet_seat_v2 *s, struct zwp_tablet_v2 *t) {
    (void)d; (void)s;
    note("seat.tablet_added");
    if (ntablets < 4) tablets[ntablets++] = t;
    zwp_tablet_v2_add_listener(t, &tablet_l, NULL);
}
static void seat_tool(void *d, struct zwp_tablet_seat_v2 *s, struct zwp_tablet_tool_v2 *t) {
    (void)d; (void)s;
    note("seat.tool_added");
    last_tool = t;
    zwp_tablet_tool_v2_add_listener(t, &tool_l, NULL);
}
static void seat_pad(void *d, struct zwp_tablet_seat_v2 *s, struct zwp_tablet_pad_v2 *p) { (void)d; (void)s; note("seat.pad_added"); zwp_tablet_pad_v2_add_listener(p, &pad_l, NULL); }
static const struct zwp_tablet_seat_v2_listener tseat_l = { seat_tablet, seat_tool, seat_pad };

static void reg_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v) {
    Client *c = data;
    if (!strcmp(iface, "wl_compositor")) c->comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_seat")) c->seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
    else if (c == &A && !strcmp(iface, "zwp_tablet_manager_v2")) { c->mgr_version = v; c->mgr = wl_registry_bind(r, name, &zwp_tablet_manager_v2_interface, v); }
    else if (c == &A && !strcmp(iface, "wp_pointer_warp_v1")) c->warp = wl_registry_bind(r, name, &wp_pointer_warp_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

/* ---- devices ---- */
static TabFrame fr[2];
static TabPadFrame pf;
static int ev(TabDev *d, int type, int code, int value) {
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = (unsigned short)type;
    e.code = (unsigned short)code;
    e.value = value;
    return tablet_event(d, &e, fr, &pf);
}
/* a report at screen (x, y): every frame it gives to wl_tablet_tool; the last answer */
static int report(TabDev *d, float x, float y) {
    int n = ev(d, EV_SYN, SYN_REPORT, 0), took = -1;
    for (int i = 0; i < n; i++) took = wl_tablet_tool(d, &fr[i], x, y);
    pump();
    return took;
}
static TabCaps intuos(int serial) {
    TabCaps c;
    memset(&c, 0, sizeof c);
    c.kind = TAB_KIND_PEN;
    snprintf(c.name, sizeof c.name, serial ? "Wacom Intuos Pro M Pen" : "Generic Pen");
    snprintf(c.path, sizeof c.path, serial ? "/dev/input/event7" : "/dev/input/event9");
    snprintf(c.phys, sizeof c.phys, "usb-0000:00:14.0-1");
    c.bus = BUS_USB; c.vendor = 0x56a; c.product = 0x357;
    c.x = (struct input_absinfo){ .minimum = 0, .maximum = 44800 };
    c.y = (struct input_absinfo){ .minimum = 0, .maximum = 29600 };
    c.pressure = (struct input_absinfo){ .minimum = 0, .maximum = 8191 };
    c.distance = (struct input_absinfo){ .minimum = 0, .maximum = 63 };
    c.tilt_x = (struct input_absinfo){ .minimum = -64, .maximum = 63, .resolution = 57 };
    c.tilt_y = (struct input_absinfo){ .minimum = -64, .maximum = 63, .resolution = 57 };
    c.caps = TAB_CAP_PRESSURE | TAB_CAP_DISTANCE | TAB_CAP_TILT;
    c.tools = TAB_TOOL_BIT(BTN_TOOL_PEN) | TAB_TOOL_BIT(BTN_TOOL_RUBBER);
    c.has_touch = 1;
    return c;
}

static int connect_client(Client *c, int fd) {
    c->dpy = wl_display_connect_to_fd(fd);
    if (!c->dpy) return -1;
    c->reg = wl_display_get_registry(c->dpy);
    wl_registry_add_listener(c->reg, &reg_l, c);
    return 0;
}

int main(void) {
    int a[2], b[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, a) || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, b)) { perror("socketpair"); return 1; }
    int srv[2] = { a[0], b[0] };
    if (srv_start(srv, 2) || connect_client(&A, a[1]) || connect_client(&B, b[1])) { puts("wltablet: no display"); return 1; }
    pump();
    CHECK(A.comp && A.seat && A.mgr && A.warp && B.comp);
    CHECK(A.mgr_version == 2);
    s1 = wl_compositor_create_surface(A.comp);
    pump();
    struct wl_surface *s2 = wl_compositor_create_surface(B.comp);
    pump();
    s3 = wl_compositor_create_surface(A.comp);
    pump();
    CHECK(srv_surfaces() == 3);
    struct wl_pointer *ptr = wl_seat_get_pointer(A.seat);
    struct zwp_tablet_seat_v2 *tseat = zwp_tablet_manager_v2_get_tablet_seat(A.mgr, A.seat);
    zwp_tablet_seat_v2_add_listener(tseat, &tseat_l, NULL);
    pump();
    CHECK(loglen == 0);                            /* no tablet yet */

    /* ---- a tablet and its pad are plugged in ---- */
    TabCaps pc = intuos(1);
    TabDev *pen = tablet_new(&pc);
    tablet_announce(pen);
    TabCaps padc;
    memset(&padc, 0, sizeof padc);
    padc.kind = TAB_KIND_PAD;
    snprintf(padc.name, sizeof padc.name, "Wacom Intuos Pro M Pad");
    snprintf(padc.path, sizeof padc.path, "/dev/input/event8");
    snprintf(padc.phys, sizeof padc.phys, "usb-0000:00:14.0-1");
    padc.vendor = 0x56a; padc.product = 0x357; padc.bus = BUS_USB;
    padc.nbuttons = 3; padc.button[0] = BTN_0; padc.button[1] = BTN_1; padc.button[2] = BTN_2;
    padc.nrings = 1; padc.ring_code[0] = ABS_WHEEL; padc.ring[0] = (struct input_absinfo){ .minimum = 0, .maximum = 71 };
    padc.nstrips = 1; padc.strip_code[0] = ABS_RX; padc.strip[0] = (struct input_absinfo){ .minimum = 0, .maximum = 4096 };
    padc.ndials = 1; padc.dial_code[0] = REL_WHEEL;
    TabDev *pad = tablet_new(&padc);
    tablet_announce(pad);
    pump();
    CHECK(has("seat.tablet_added") && has("tablet.name Wacom Intuos Pro M Pen") && has("tablet.id 56a 357"));
    CHECK(has("tablet.bustype 3") && has("tablet.path /dev/input/event7") && before("tablet.path /dev/input/event7", "tablet.done"));
    CHECK(has("seat.pad_added") && has("pad.group") && has("group.buttons 3") && has("group.ring") && has("group.strip"));
    CHECK(has("group.dial") && has("group.done") && !has("group.modes 1") && has("pad.path /dev/input/event8"));
    CHECK(has("pad.buttons 3") && before("group.done", "pad.done"));
    CHECK(!has("seat.tool_added"));                /* a tool is announced when it first comes near */
    clear();

    /* ---- the pen comes near over A's window: the tool is announced, then proximity_in with everything ---- */
    scene.under = 1;
    ev(pen, EV_ABS, ABS_X, 8960); ev(pen, EV_ABS, ABS_Y, 5920); ev(pen, EV_ABS, ABS_DISTANCE, 20);
    ev(pen, EV_ABS, ABS_TILT_X, 0); ev(pen, EV_ABS, ABS_TILT_Y, 0); ev(pen, EV_ABS, ABS_MISC, 0x802);
    ev(pen, EV_MSC, MSC_SERIAL, 0x1234567); ev(pen, EV_KEY, BTN_TOOL_PEN, 1);
    CHECK(report(pen, 300, 200) == 1);
    CHECK(has("seat.tool_added") && has("tool.type 140") && has("tool.hardware_serial 0 1234567") && has("tool.hardware_id_wacom 0 802"));
    CHECK(has("tool.capability 1") && has("tool.capability 2") && has("tool.capability 3") && !has("tool.capability 4"));
    CHECK(before("tool.done", "tool.proximity_in tablet s1"));
    CHECK(has("tool.motion 200.0 150.0") && has("tool.pressure 0") && has("tool.tilt 0 0") && has("tool.frame"));
    CHECK(before("tool.proximity_in tablet s1", "tool.motion 200.0 150.0") && !has("tool.down"));
    CHECK(scene.pointer_leaves == 1);            /* the app's toolkit follows the tool now */
    clear();

    /* ---- the tip down: down, the pressure; a press for move and resize ---- */
    ev(pen, EV_KEY, BTN_TOUCH, 1); ev(pen, EV_ABS, ABS_PRESSURE, 4096); ev(pen, EV_ABS, ABS_DISTANCE, 0);
    CHECK(report(pen, 300, 200) == 1);
    CHECK(has("tool.down") && has("tool.pressure 32772") && has("tool.distance 0") && has("tool.frame"));
    CHECK(scene.presses == 1);
    clear();
    /* drawing past the window's edge: the surface keeps the tool while the tip is down */
    scene.under = 0;
    ev(pen, EV_ABS, ABS_X, 44800);
    CHECK(report(pen, 600, 200) == 1 && has("tool.motion 500.0 150.0") && !has("tool.proximity_out"));
    clear();
    ev(pen, EV_ABS, ABS_X, 8960);
    scene.under = 1;
    CHECK(report(pen, 300, 200) == 1 && has("tool.motion 200.0 150.0"));
    /* the lower button, the tip up */
    ev(pen, EV_KEY, BTN_STYLUS, 1);
    CHECK(report(pen, 300, 200) == 1 && has("tool.button 14b 1"));
    ev(pen, EV_KEY, BTN_TOUCH, 0); ev(pen, EV_ABS, ABS_PRESSURE, 0);
    CHECK(report(pen, 300, 200) == 1 && has("tool.up") && scene.releases == 1);
    clear();

    /* ---- set_cursor: with the proximity_in serial only ---- */
    zwp_tablet_tool_v2_set_cursor(last_tool, prox_serial + 1000, NULL, 1, 1);
    pump();
    CHECK(scene.cursor_set == 0);
    zwp_tablet_tool_v2_set_cursor(last_tool, prox_serial, NULL, 3, 4);
    pump();
    CHECK(scene.cursor_set == 1 && scene.cursor_hx == 3 && scene.cursor_hy == 4);

    /* ---- hovering off the window with the button held: released, then proximity_out; the pointer takes over ---- */
    scene.under = 0;
    ev(pen, EV_ABS, ABS_X, 44800);
    CHECK(report(pen, 600, 200) == 0);
    CHECK(has("tool.button 14b 0") && before("tool.button 14b 0", "tool.proximity_out") && has("tool.frame"));
    clear();
    /* over B's window: B never bound tablet-v2, so the pointer acts; A hears nothing */
    scene.under = 2;
    ev(pen, EV_ABS, ABS_X, 8960);
    CHECK(report(pen, 300, 200) == 0 && loglen == 0);
    /* over A's window under a modal dialog: the pointer too */
    scene.under = 1;
    scene.held = 1;
    ev(pen, EV_ABS, ABS_X, 8961);
    CHECK(report(pen, 300, 200) == 0 && loglen == 0);
    scene.held = 0;
    /* back over A's window, the button still down: proximity_in, then the button pressed, in one frame */
    ev(pen, EV_ABS, ABS_X, 8960);
    CHECK(report(pen, 300, 200) == 1 && before("tool.proximity_in tablet s1", "tool.button 14b 1"));
    CHECK(before("tool.button 14b 1", "tool.frame"));
    clear();
    /* a stroke that began outside (the tip down over LordOS's own UI) doesn't move into the app */
    scene.under = 0;
    ev(pen, EV_KEY, BTN_STYLUS, 0); ev(pen, EV_ABS, ABS_X, 44800);
    CHECK(report(pen, 600, 200) == 0);
    ev(pen, EV_KEY, BTN_TOUCH, 1); ev(pen, EV_ABS, ABS_PRESSURE, 2000);
    CHECK(report(pen, 600, 200) == 0);
    clear();
    scene.under = 1;
    ev(pen, EV_ABS, ABS_X, 8960);
    CHECK(report(pen, 300, 200) == 0 && loglen == 0);
    ev(pen, EV_KEY, BTN_TOUCH, 0); ev(pen, EV_ABS, ABS_PRESSURE, 0);
    CHECK(report(pen, 300, 200) == 1 && has("tool.proximity_in tablet s1") && !has("tool.down"));
    clear();

    /* ---- the tool leaves the tablet ---- */
    ev(pen, EV_KEY, BTN_TOOL_PEN, 0);
    CHECK(report(pen, 300, 200) == 1 && has("tool.proximity_out") && has("tool.frame") && !has("tool.up"));
    clear();

    /* ---- a scaled Xwayland (X pixels are 1.5 logical ones) gets X pixels ---- */
    scene.scale = 1.5f;
    ev(pen, EV_KEY, BTN_TOOL_PEN, 1);
    CHECK(report(pen, 300, 200) == 1 && has("tool.motion 300.0 225.0"));
    ev(pen, EV_KEY, BTN_TOOL_PEN, 0);
    report(pen, 300, 200);
    scene.scale = 0;
    clear();

    /* main.c takes a frame for the pointer (a window move the tip began): the tool leaves the surface, pressing */
    ev(pen, EV_KEY, BTN_TOOL_PEN, 1);
    CHECK(report(pen, 300, 200) == 1);
    ev(pen, EV_KEY, BTN_TOUCH, 1); ev(pen, EV_ABS, ABS_PRESSURE, 3000);
    CHECK(report(pen, 300, 200) == 1 && has("tool.down") && scene.presses == 2);
    clear();
    ev(pen, EV_ABS, ABS_X, 9000);
    CHECK(ev(pen, EV_SYN, SYN_REPORT, 0) == 1);
    wl_tablet_tool_away(pen, &fr[0]);
    pump();
    CHECK(before("tool.up", "tool.proximity_out") && scene.releases == 2);
    clear();
    ev(pen, EV_KEY, BTN_TOUCH, 0); ev(pen, EV_ABS, ABS_PRESSURE, 0);
    CHECK(ev(pen, EV_SYN, SYN_REPORT, 0) == 1);
    wl_tablet_tool_away(pen, &fr[0]);              /* the move ends with the tip */
    ev(pen, EV_ABS, ABS_X, 8960);
    CHECK(report(pen, 300, 200) == 1 && has("tool.proximity_in tablet s1") && !has("tool.down"));
    ev(pen, EV_KEY, BTN_TOOL_PEN, 0);
    report(pen, 300, 200);
    clear();

    /* ---- the pad follows the keyboard ---- */
    ev(pad, EV_KEY, BTN_1, 1);
    CHECK(ev(pad, EV_SYN, SYN_REPORT, 0) == 1);
    wl_tablet_pad(pad, &pf);                       /* nobody has the keyboard: nobody hears it */
    pump();
    CHECK(loglen == 0);
    srv_key_focus(1);
    pump();
    CHECK(has("pad.enter tablet s1") && has("group.mode_switch 0"));
    clear();
    ev(pad, EV_KEY, BTN_1, 0);
    ev(pad, EV_SYN, SYN_REPORT, 0);
    wl_tablet_pad(pad, &pf);
    ev(pad, EV_ABS, ABS_WHEEL, 36);
    ev(pad, EV_SYN, SYN_REPORT, 0);
    wl_tablet_pad(pad, &pf);
    ev(pad, EV_ABS, ABS_WHEEL, 0);
    ev(pad, EV_SYN, SYN_REPORT, 0);
    wl_tablet_pad(pad, &pf);
    ev(pad, EV_ABS, ABS_RX, 64);
    ev(pad, EV_SYN, SYN_REPORT, 0);
    wl_tablet_pad(pad, &pf);
    ev(pad, EV_REL, REL_WHEEL, -1);
    ev(pad, EV_SYN, SYN_REPORT, 0);
    wl_tablet_pad(pad, &pf);
    pump();
    CHECK(has("pad.button 1 0") && has("ring.source 1") && has("ring.angle 90") && before("ring.angle 90", "ring.stop"));
    CHECK(has("ring.frame") && has("strip.position 32768") && has("strip.frame") && has("dial.delta -120") && has("dial.frame"));
    clear();
    srv_key_focus(2);                              /* B's window: A's pad leaves; B never bound, hears nothing */
    pump();
    CHECK(has("pad.leave s1") && !has("pad.enter tablet s1"));
    clear();

    /* ---- a pen with no serial: its tool belongs to its tablet and goes with it ---- */
    TabCaps gc = intuos(0);
    TabDev *gen = tablet_new(&gc);
    tablet_announce(gen);
    pump();
    CHECK(has("tablet.name Generic Pen"));
    clear();
    ev(gen, EV_KEY, BTN_TOOL_PEN, 1);
    CHECK(report(gen, 300, 200) == 1 && has("seat.tool_added") && !has("tool.hardware_serial 0 0"));
    clear();
    tablet_free(gen);                              /* unplugged with the pen near */
    pump();
    CHECK(has("tool.proximity_out") && has("tool.removed") && has("tablet.removed"));
    CHECK(before("tool.proximity_out", "tool.removed"));
    clear();

    /* ---- pointer-warp ---- */
    scene.pointer = 1; scene.key = 1; scene.enter_serial = 77;
    wp_pointer_warp_v1_warp_pointer(A.warp, s1, ptr, wl_fixed_from_double(10.5), wl_fixed_from_int(20), 77);
    pump();
    CHECK(scene.warps == 1 && scene.warp_x == 110.5f && scene.warp_y == 70.0f);
    wp_pointer_warp_v1_warp_pointer(A.warp, s1, ptr, wl_fixed_from_int(10), wl_fixed_from_int(20), 78);
    pump();
    CHECK(scene.warps == 1);                       /* not the enter serial */
    wp_pointer_warp_v1_warp_pointer(A.warp, s1, ptr, wl_fixed_from_int(400), wl_fixed_from_int(20), 77);
    wp_pointer_warp_v1_warp_pointer(A.warp, s1, ptr, wl_fixed_from_int(-1), wl_fixed_from_int(20), 77);
    pump();
    CHECK(scene.warps == 1);                       /* outside the surface */
    wp_pointer_warp_v1_warp_pointer(A.warp, s3, ptr, wl_fixed_from_int(10), wl_fixed_from_int(20), 77);
    pump();
    CHECK(scene.warps == 1);                       /* another window of the app's, not the one with the pointer */
    scene.key = 2;
    wp_pointer_warp_v1_warp_pointer(A.warp, s1, ptr, wl_fixed_from_int(10), wl_fixed_from_int(20), 77);
    pump();
    CHECK(scene.warps == 1);                       /* in the background: no keyboard */
    scene.key = 1; scene.pointer = 2;
    wp_pointer_warp_v1_warp_pointer(A.warp, s1, ptr, wl_fixed_from_int(10), wl_fixed_from_int(20), 77);
    pump();
    CHECK(scene.warps == 1);                       /* the pointer is over another app */
    scene.pointer = 1; scene.locked = 1;
    wp_pointer_warp_v1_warp_pointer(A.warp, s1, ptr, wl_fixed_from_int(10), wl_fixed_from_int(20), 77);
    scene.locked = 0; scene.dragging = 1;
    wp_pointer_warp_v1_warp_pointer(A.warp, s1, ptr, wl_fixed_from_int(10), wl_fixed_from_int(20), 77);
    pump();
    CHECK(scene.warps == 1);                       /* locked, or in a drag */
    scene.dragging = 0; scene.scale = 2;
    wp_pointer_warp_v1_warp_pointer(A.warp, s1, ptr, wl_fixed_from_int(10), wl_fixed_from_int(20), 77);
    pump();
    CHECK(scene.warps == 2 && scene.warp_x == 105.0f && scene.warp_y == 60.0f);   /* X pixels from a scaled Xwayland */
    scene.scale = 0;
    CHECK(wl_display_get_error(A.dpy) == 0 && wl_display_get_error(B.dpy) == 0);

    /* ---- the tablet and pad unplugged ---- */
    tablet_free(pen);
    tablet_free(pad);
    pump();
    CHECK(has("tablet.removed") && has("pad.removed") && !has("tool.removed"));   /* a tool with a serial stays */

    wl_surface_destroy(s1);
    wl_surface_destroy(s2);
    wl_surface_destroy(s3);
    pump();
    wl_display_disconnect(A.dpy);
    wl_display_disconnect(B.dpy);
    srv_stop();
    puts(fails ? "wltablet: FAILED" : "wltablet: ok");
    return fails != 0;
}
