/* lid_test.c — the lid closed on an external display (issue #97): src/hw/lid.c reading logind's
 * LidClosed, and src/hw/display.c's display_plan leaving the built-in panel out, with the system bus and
 * the card replaced by this file:
 *   - LidClosed is read (Properties.Get) once logind has an owner, and followed by PropertiesChanged;
 *   - a signal from anyone but logind's own connection, another interface or the wrong signature is
 *     ignored; an invalidated LidClosed is read again; an answer older than a signal is ignored;
 *   - logind going away counts as an open lid;
 *   - closed with an external display connected: only the external one is in the plan, and it is main;
 *     closed with only the panel (or the external one unplugged): the panel stays; open: both;
 *   - eDP, LVDS and DSI are built-in panels; two external displays both stay;
 *   - lord.display.list() says the panel is off because of the lid, not for want of a CRTC. */
#include "hw/lid.h"
#include "hw/display.h"
#include "hw/sysbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, total;
static void check(int ok, const char *what) { total++; if (!ok) { fails++; printf("FAIL: %s\n", what); } }

/* ---- the system bus, replaced ---- */
static SbSignal sig_fn;
static SbOwner owner_fn;
static SbReply reply_fn;
static void *reply_ctx;
static int get_calls, matches;

int sysbus_up(void) { return 1; }
void sysbus_on_signal(SbSignal fn) { sig_fn = fn; }
void sysbus_add_match(const char *rule) {
    if (strstr(rule, "member='PropertiesChanged'") && strstr(rule, "sender='org.freedesktop.login1'") &&
        strstr(rule, "path='/org/freedesktop/login1'"))
        matches++;
}
void sysbus_watch_name(const char *name, SbOwner fn) { if (!strcmp(name, "org.freedesktop.login1")) owner_fn = fn; }
uint32_t sysbus_call(const char *dest, const char *path, const char *iface, const char *member,
                     const char *sig, const DBuf *body, SbReply cb, void *ctx) {
    if (strcmp(dest, "org.freedesktop.login1") || strcmp(path, "/org/freedesktop/login1") ||
        strcmp(iface, "org.freedesktop.DBus.Properties") || strcmp(member, "Get") || strcmp(sig, "ss")) return 0;
    DMsg m = { .sig = "ss", .body = body->p, .body_len = (uint32_t)body->n };
    DRd r;
    dr_init(&r, &m);
    const char *i = dr_str(&r, NULL), *p = dr_str(&r, NULL);
    if (r.err || strcmp(i, "org.freedesktop.login1.Manager") || strcmp(p, "LidClosed")) return 0;
    get_calls++;
    reply_fn = cb;
    reply_ctx = ctx;
    return (uint32_t)get_calls;
}

/* logind answers Get with v (b) */
static void answer(int closed) {
    DBuf b;
    dw_init(&b);
    dw_variant(&b, "b");
    dw_bool(&b, closed);
    DMsg m = { .type = DBUS_RETURN, .sig = "v", .body = b.p, .body_len = (uint32_t)b.n };
    reply_fn(&m, reply_ctx);
    dw_free(&b);
}

/* PropertiesChanged(iface, {LidClosed: closed} or {}, invalidated) */
static void changed(const char *sender, const char *iface, int closed, int invalidate, const char *other) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, iface);
    size_t a = dw_arr_begin(&b, 8);
    if (other) { dw_struct(&b); dw_str(&b, other); dw_variant(&b, "s"); dw_str(&b, "x"); }
    if (closed >= 0) { dw_struct(&b); dw_str(&b, "LidClosed"); dw_variant(&b, "b"); dw_bool(&b, closed); }
    dw_arr_end(&b, a);
    a = dw_arr_begin(&b, 4);
    if (invalidate) dw_str(&b, "LidClosed");
    dw_arr_end(&b, a);
    DHdr h = { .type = DBUS_SIGNAL, .serial = 9, .path = "/org/freedesktop/login1", .iface = "org.freedesktop.DBus.Properties",
               .member = "PropertiesChanged", .sig = "sa{sv}as", .sender = sender };
    DBuf raw;
    DMsg m;
    if (!dbus_build(&h, &b, &raw) && !dbus_msg_parse(raw.p, raw.n, &m)) { sig_fn(&m); dbus_msg_free(&m); }
    else check(0, "building PropertiesChanged");
    dw_free(&raw);
    dw_free(&b);
}

/* ---- the card, replaced ---- */
static struct drm_mode_modeinfo mode_panel = { .hdisplay = 1920, .vdisplay = 1200, .vrefresh = 60, .clock = 154000, .htotal = 2080,
                                               .vtotal = 1235, .type = DRM_MODE_TYPE_PREFERRED };
static struct drm_mode_modeinfo mode_ext = { .hdisplay = 2560, .vdisplay = 1440, .vrefresh = 60, .clock = 241500, .htotal = 2720,
                                             .vtotal = 1481, .type = DRM_MODE_TYPE_PREFERRED };
static KmsOutput outs[3];
static int nouts;
static Kms card = { .fd = -1 };

const char *kms_connector_type_name(uint32_t t) {
    switch (t) {
    case DRM_MODE_CONNECTOR_eDP: return "eDP";
    case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
    case DRM_MODE_CONNECTOR_DSI: return "DSI";
    case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
    case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
    default: return "Unknown";
    }
}
int kms_outputs(int fd, KmsOutput *o, int max) { (void)fd; int n = nouts < max ? nouts : max; memcpy(o, outs, sizeof *o * (size_t)n); return n; }
void kms_outputs_free(KmsOutput *o, int n) { (void)o; (void)n; }
int kms_gamma_size(Kms *k) { (void)k; return 0; }
int kms_set_gamma(Kms *k, int n, const uint16_t *r, const uint16_t *g, const uint16_t *b) { (void)k; (void)n; (void)r; (void)g; (void)b; return 0; }
int kms_dpms(Kms *k, int on) { (void)k; (void)on; return 0; }

static Kms *host_kms(void) { return &card; }
static void host_event(const char *kind, const char *json) { (void)kind; (void)json; }
static const DisplayHost host = { .kms = host_kms, .event = host_event };

static void set_outputs(uint32_t panel_type, int panel, int ext, int ext2) {
    memset(outs, 0, sizeof outs);
    outs[0] = (KmsOutput){ .id = 40, .type = panel_type, .type_id = 1, .connected = panel, .mm_w = 300, .mm_h = 190,
                           .nmodes = panel, .modes = &mode_panel };
    outs[1] = (KmsOutput){ .id = 50, .type = DRM_MODE_CONNECTOR_HDMIA, .type_id = 1, .connected = ext, .mm_w = 600, .mm_h = 340,
                           .nmodes = ext, .modes = &mode_ext };
    outs[2] = (KmsOutput){ .id = 60, .type = DRM_MODE_CONNECTOR_DisplayPort, .type_id = 1, .connected = ext2, .mm_w = 600, .mm_h = 340,
                           .nmodes = ext2, .modes = &mode_ext };
    nouts = 3;
}

static int in_plan(const DisplayPlan *p, uint32_t conn) {
    for (int i = 0; i < p->n; i++) if (p->o[i].conn == conn) return 1;
    return 0;
}

/* the plan, applied as main.c does (display_resync_done records it for lord.display.list) */
static int plan(DisplayPlan *p) {
    display_plan(outs, nouts, p);
    display_resync_done(1, p);
    return p->n;
}

int main(void) {
    char home[] = "/tmp/lid-test-XXXXXX";
    if (!mkdtemp(home)) { perror("mkdtemp"); return 1; }
    setenv("HOME", home, 1);
    unsetenv("LORD_MAIN_DISPLAY");
    display_init(&host);
    DisplayPlan p;

    lid_start();
    check(sig_fn && owner_fn && matches == 1, "subscribed to logind's PropertiesChanged and its owner");
    check(get_calls == 0, "nothing asked before logind has an owner");

    /* lid open: the panel and the external display */
    set_outputs(DRM_MODE_CONNECTOR_eDP, 1, 1, 0);
    check(plan(&p) == 2 && in_plan(&p, 40) && in_plan(&p, 50) && p.o[p.main].conn == 40, "lid open: both, the panel first");

    owner_fn("org.freedesktop.login1", ":1.4");
    check(get_calls == 1, "LidClosed is read once logind has an owner");
    display_take_resync();              /* display_init's first plan */
    answer(1);
    check(display_lid_closed() == 1, "logind says the lid is closed");
    check(display_take_resync() == 1, "a lid change asks main.c for a new plan");

    /* lid closed with an external display: only that one, and it is the main display */
    check(plan(&p) == 1 && p.o[0].conn == 50 && p.main == 0 && p.o[0].v.x == 0 && p.o[0].v.y == 0,
          "lid closed with an external display: the panel leaves the plan");
    char *j = display_list_json();
    check(j && strstr(j, "\"connector\":\"eDP-1\"") && strstr(j, "\"active\":false,\"lidClosed\":true"),
          "the list says the panel is off because of the lid");
    check(j && strstr(j, "\"connector\":\"HDMI-A-1\"") && strstr(j, "\"active\":true,\"lidClosed\":false"), "the external one is on");
    free(j);

    /* the external display unplugged with the lid still closed: the panel is the only one, it stays */
    set_outputs(DRM_MODE_CONNECTOR_eDP, 1, 0, 0);
    check(plan(&p) == 1 && p.o[0].conn == 40, "lid closed with only the panel: the only display is never turned off");
    j = display_list_json();
    check(j && strstr(j, "\"active\":true,\"lidClosed\":false"), "the panel kept is not reported as off");
    free(j);

    /* LVDS and DSI are built-in panels too; two external displays both stay */
    set_outputs(DRM_MODE_CONNECTOR_LVDS, 1, 1, 0);
    check(plan(&p) == 1 && p.o[0].conn == 50, "an LVDS panel leaves too");
    set_outputs(DRM_MODE_CONNECTOR_DSI, 1, 1, 1);
    check(plan(&p) == 2 && in_plan(&p, 50) && in_plan(&p, 60) && !in_plan(&p, 40), "a DSI panel leaves; both external ones stay");
    set_outputs(DRM_MODE_CONNECTOR_eDP, 0, 1, 0);
    check(plan(&p) == 1 && p.o[0].conn == 50, "no panel connected: nothing changes");

    /* not logind, not the manager, not the property: ignored */
    set_outputs(DRM_MODE_CONNECTOR_eDP, 1, 1, 0);
    changed(":1.99", "org.freedesktop.login1.Manager", 0, 0, NULL);
    check(display_lid_closed() == 1, "PropertiesChanged from another connection is ignored");
    changed(":1.4", "org.freedesktop.login1.Session", 0, 0, NULL);
    check(display_lid_closed() == 1, "another interface's LidClosed is ignored");
    int calls = get_calls;
    changed(":1.4", "org.freedesktop.login1.Manager", -1, 0, "IdleHint");
    check(display_lid_closed() == 1 && get_calls == calls, "another property: nothing changes, nothing asked");

    /* the lid opens: the panel comes back, first again */
    display_take_resync();
    changed(":1.4", "org.freedesktop.login1.Manager", 0, 0, "IdleHint");
    check(display_lid_closed() == 0 && display_take_resync() == 1, "PropertiesChanged: the lid is open");
    check(plan(&p) == 2 && p.o[p.main].conn == 40 && in_plan(&p, 50), "lid open: the panel is back");

    /* the same value again: no new plan */
    changed(":1.4", "org.freedesktop.login1.Manager", 0, 0, NULL);
    check(display_take_resync() == 0, "the same state: nothing to apply");

    /* invalidated: read again; an answer older than a later signal is ignored */
    changed(":1.4", "org.freedesktop.login1.Manager", -1, 1, NULL);
    check(get_calls == calls + 1, "an invalidated LidClosed is read again");
    SbReply old_fn = reply_fn;
    void *old_ctx = reply_ctx;
    changed(":1.4", "org.freedesktop.login1.Manager", 1, 0, NULL);
    check(display_lid_closed() == 1, "closed again");
    reply_fn = old_fn;
    reply_ctx = old_ctx;
    answer(0);
    check(display_lid_closed() == 1, "a Get answered after a newer signal is ignored");

    /* a reply with the wrong type: ignored */
    owner_fn("org.freedesktop.login1", ":1.4");
    DBuf b;
    dw_init(&b);
    dw_variant(&b, "s");
    dw_str(&b, "no");
    DMsg bad = { .type = DBUS_RETURN, .sig = "v", .body = b.p, .body_len = (uint32_t)b.n };
    reply_fn(&bad, reply_ctx);
    dw_free(&b);
    check(display_lid_closed() == 1, "a LidClosed that is not a boolean is ignored");
    reply_fn(NULL, reply_ctx);
    check(display_lid_closed() == 1, "no answer (the bus went away): the state is kept");

    /* logind goes away: the lid counts as open, every display is shown */
    owner_fn("org.freedesktop.login1", "");
    check(display_lid_closed() == 0, "no logind: the lid is open");
    changed(":1.4", "org.freedesktop.login1.Manager", 1, 0, NULL);
    check(display_lid_closed() == 0, "the old owner's signal is ignored");
    check(plan(&p) == 2, "both displays again");

    char dir[64];                       /* displays.json is only written on "Keep" */
    snprintf(dir, sizeof dir, "%s/.config/lordos", home);
    rmdir(dir);
    snprintf(dir, sizeof dir, "%s/.config", home);
    rmdir(dir);
    rmdir(home);
    printf("lid-test: %d/%d\n", total - fails, total);
    return fails ? 1 : 0;
}
