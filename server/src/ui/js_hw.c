/* js_hw.c — lord.display, lord.power, lord.bt, lord.mime and lord.timedate for Settings'
 * hardware pages (src/hw, docs/SETTINGS.md). Every input is validated here; nothing runs a
 * program (root work goes through the admin helper's closed actions, hw/hw_admin.c). Notices
 * arrive in JS as __lordHw(kind, data).
 *
 *   lord.display.list()                         outputs, modes, Night Light, backlight
 *   lord.display.apply(conn, w, h, mHz, rot, scale)   live (queued between frames); scale in % (100–300,
 *                                               25 % steps); true if accepted
 *   lord.display.arrange(conn, x, y, apply)     drop a display at global logical (x, y): { x, y } snapped
 *   lord.display.setMirror(on) / setMain(conn) / setCaptureAll(on)   (docs/DISPLAYS.md)
 *   lord.display.save(conn)                     keep the current configuration of every display
 *   lord.display.setTemperature(K) / temperature()
 *   lord.display.sun(zone, y, m, d)             { rise, set } minutes UTC, or null
 *   lord.display.setIdle(offS, lockS, sleepS) / wake()
 *   lord.power.state() / profiles() / setProfile(p) / buttons()
 *   lord.bt.state() / power(on) / discover(on) / pair(path) / connect(path) / disconnect(path)
 *          / forget(path) / answer(id, accept, value)
 *   lord.mime.get(mime) / set(mime, id) / guess(path) / terminal() / setTerminal(id)
 *   lord.mime.resolve(mime)                     { id, explicit, source }: the app launchWith('') uses
 *   lord.mime.unset(mime)                       back to the automatic choice
 *   lord.timedate.refresh()                     answer as __lordHw('timedate', …)
 *   lord.notifyd.closed(id, reason) / action(id, key)   back to the Linux app that notified
 *   lord.mpris.list()                           the Linux players on the session bus (hw/mpris.c), read-only;
 *                                               changes also arrive as __lordHw('mpris', list)
 *   lord.mpris.control(name, cmd, ms)           cmd 'toggle' | 'play' | 'pause' | 'next' | 'prev' | 'seek' (ms:
 *                                               the position, clamped to the track); true if sent. Never waits
 *   lord.mpris.refresh()                        read every player again (Position isn't signalled)
 *   lord.hwinfo.scan()                          read-only hardware facts for the Store's suggestions
 *                                               (hw/hwscan.c: DMI, device tree, PCI/USB IDs, virtio
 *                                               ports, missing firmware when the kernel log is readable)
 *   lord.sysinfo.report()                       System Information (sysreport/: the same report as
 *                                               lordos-sysreport, no serials, addresses or names) plus
 *                                               session: how this session composes the screen and its
 *                                               GPU context (gpu/gpu.h gpu_info, lord_composition)
 *   lord.sysinfo.facts()                        the Checks tab's facts (sysreport/facts.c): lord-server's
 *                                               uid/euid, its cgroup and who owns it, app scopes, and
 *                                               whether anything listens on the remote desktop's port
 *   lord.sysinfo.probe()                        the last sandbox probe's result (sec/brokersrv.c, BOP_PROBE:
 *                                               the system package `sysprobe`, app/probe.c), or null
 *   lord.sysinfo.markdown()                     the same report as Markdown, as lordos-sysreport prints it,
 *                                               with a "This session" section (System Information's Report tab)
 *   lord.sysinfo.peripherals()                  (0.8.7) keyboards, mice, receivers and other devices on this machine's USB
 *                                               and input buses (sysreport/periph.c), for the Hardware tab only: their
 *                                               names can carry a person's, so it is not part of report() or markdown()
 *   lord.sysinfo.community()                    (0.8.7) the tested-hardware catalogue's snapshot for the Compatibility
 *                                               panel: { state, snapshot } where snapshot is given only when
 *                                               <assets>/hwreport/snapshot.json is signed with the key this build
 *                                               trusts and well formed (sysreport/hwsnap.c); no arguments, no path
 *                                               from JS; state 'ok' | 'missing' | 'unsigned' | 'bad-signature' | 'invalid'
 *   lord.sysinfo.bench.start(mode, sustainedS) / poll() / cancel() / saved(which) / boots()
 *                                               LordBench (bench/lbrun.c, the lord-bench helper): mode
 *                                               'quick' | 'full' (then video in lord-media) | 'desktop'
 *                                               (the compositor's frame times, alone and under load);
 *                                               poll() → {state, step, progress, result}; saved(0|1) the
 *                                               last or previous peak result, saved(2|3) the desktop ones;
 *                                               boots() the boot times per LordOS version (lord-sessiond's history);
 *                                               bootTest() the boot time test's state or null (bench/boottest.h;
 *                                               started and stopped with the boot-test admin actions)
 *   lord.fan.status() / setProfile(p) / setCurve(pts) / force(v) / caseConfig()   (js_fan.c, lord-fand)
 *   lord.browserFrame.apply(on, which)          Settings → Appearance → "Browsers: tabs in the title bar":
 *                                               one preference in the user's Chromium/Firefox profiles
 *                                               (hw/brframe.c); which = 'all' | 'chromium' | 'firefox';
 *                                               { chromium: {…}, firefox: {…} } counts
 *   lord.browserFrame.tidy()                    removes LordOS's Firefox user.js line once Firefox read it
 *   lord.browserFrame.chromiumRunning()         Chromium's profile folder is in use
 *   lord.browserFrame.state()                   { chromiumOn, chromiumOff }: profiles with each frame
 */
#include "js_hw.h"
#include "js_fan.h"
#include "../hw/brframe.h"
#include "../hw/bt.h"
#include "../apps.h"
#include "../hw/defaults.h"
#include "../hw/display.h"
#include "../hw/hw.h"
#include "../hw/hwscan.h"
#include "../gpu/gpu.h"
#include "../sysreport/sysreport.h"
#include "../sysreport/periph.h"
#include "../sysreport/facts.h"
#include "../sysreport/hwsnap.h"
#include "../sec/codesig.h"
#include "js_sys.h"
#include "../bench/lbrun.h"
#include "../bench/boottest.h"
#include "../sec/broker.h"
#include "lord_version.h"           /* build/gen: the version for the report */
#include "../hw/mimeapps.h"
#include "../hw/mpris.h"
#include "../hw/notifyd.h"
#include "../hw/pkagent.h"
#include "portal.h"
#include "../cast/castproto.h"
#include "ui.h"
#include "sni.h"
#include "../hw/ppd.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static JSValue json_or_null(JSContext *c, char *j) {
    if (!j) return JS_NULL;
    JSValue v = JS_ParseJSON(c, j, strlen(j), "<hw>");
    free(j);
    return v;
}

static int arg_int(JSContext *c, JSValueConst v, int def) {
    int32_t x;
    return JS_ToInt32(c, &x, v) ? def : x;
}

/* A short string argument (≤ max bytes) copied into out. 0 if valid. */
static int arg_str(JSContext *c, JSValueConst v, char *out, size_t max) {
    size_t n;
    const char *s = JS_ToCStringLen(c, &n, v);
    if (!s) return -1;
    int ok = n < max && !memchr(s, 0, n);
    if (ok) memcpy(out, s, n + 1);
    JS_FreeCString(c, s);
    return ok ? 0 : -1;
}

/* ---- lord.display ---- */

static JSValue f_display_list(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return json_or_null(c, display_list_json());
}

static JSValue f_display_apply(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 5) return JS_FALSE;
    int conn = arg_int(c, argv[0], 0), w = arg_int(c, argv[1], 0), h = arg_int(c, argv[2], 0);
    int hz = arg_int(c, argv[3], 0), rot = arg_int(c, argv[4], 0), scale = argc > 5 ? arg_int(c, argv[5], 100) : 100;
    if (scale == 1 || scale == 2) scale *= 100;
    if (conn <= 0 || w < 0 || h < 0 || w > 16384 || h > 16384 || hz < 0 || hz > 1000000 || rot < 0 || rot > 3 || !out_scale_valid(scale)) return JS_FALSE;
    return JS_NewBool(c, display_request((uint32_t)conn, w, h, hz, rot, scale) == 0);
}

static JSValue f_display_arrange(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 3) return JS_NULL;
    int conn = arg_int(c, argv[0], 0), x = arg_int(c, argv[1], 0), y = arg_int(c, argv[2], 0);
    int apply = argc > 3 && JS_ToBool(c, argv[3]) > 0, sx = 0, sy = 0;
    if (conn <= 0 || x < -65536 || x > 65536 || y < -65536 || y > 65536) return JS_NULL;
    if (display_arrange((uint32_t)conn, x, y, apply, &sx, &sy)) return JS_NULL;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "x", JS_NewInt32(c, sx));
    JS_SetPropertyStr(c, o, "y", JS_NewInt32(c, sy));
    return o;
}

static JSValue f_display_mirror(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    return JS_NewBool(c, argc > 0 && display_set_mirror(JS_ToBool(c, argv[0]) > 0) == 0);
}

static JSValue f_display_main(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int conn = argc ? arg_int(c, argv[0], 0) : 0;
    return JS_NewBool(c, conn > 0 && display_set_main((uint32_t)conn) == 0);
}

static JSValue f_display_capture(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc > 0) display_set_capture_all(JS_ToBool(c, argv[0]) > 0);
    return JS_NewBool(c, display_capture_all());
}

/* Settings → Displays, "Rotate automatically" (shown only when there is an accelerometer) */
static JSValue f_display_auto_rotate(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc > 0) display_set_auto_rotate(JS_ToBool(c, argv[0]) > 0);
    return JS_NewBool(c, display_auto_rotate());
}

static JSValue f_display_save(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int conn = argc ? arg_int(c, argv[0], 0) : 0;
    return JS_NewBool(c, conn >= 0 && display_save((uint32_t)conn) == 0);
}

static JSValue f_display_set_temp(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int k = argc ? arg_int(c, argv[0], 6500) : 6500;
    display_set_temperature(k);
    return JS_UNDEFINED;
}

static JSValue f_display_temp(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewInt32(c, display_temperature());
}

static JSValue f_display_sun(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char zone[96];
    if (argc < 4 || arg_str(c, argv[0], zone, sizeof zone)) return JS_NULL;
    int y = arg_int(c, argv[1], 0), m = arg_int(c, argv[2], 0), d = arg_int(c, argv[3], 0);
    double rise, set;
    if (y < 1970 || y > 2200 || m < 1 || m > 12 || d < 1 || d > 31 || display_sun(zone, y, m, d, &rise, &set)) return JS_NULL;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "rise", JS_NewFloat64(c, rise));
    JS_SetPropertyStr(c, o, "set", JS_NewFloat64(c, set));
    return o;
}

static JSValue f_display_idle(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int v[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4 && i < argc; i++) { v[i] = arg_int(c, argv[i], 0); if (v[i] < 0 || v[i] > 86400) v[i] = 0; }
    display_set_idle(v[0], v[1], v[2], v[3]);
    return JS_UNDEFINED;
}

static JSValue f_display_wake(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    display_wake();
    return JS_UNDEFINED;
}

static JSValue f_display_blank(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    display_blank();
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry display_funcs[] = {
    JS_CFUNC_DEF("list", 0, f_display_list),
    JS_CFUNC_DEF("apply", 6, f_display_apply),
    JS_CFUNC_DEF("save", 1, f_display_save),
    JS_CFUNC_DEF("arrange", 4, f_display_arrange),
    JS_CFUNC_DEF("setMirror", 1, f_display_mirror),
    JS_CFUNC_DEF("setMain", 1, f_display_main),
    JS_CFUNC_DEF("setCaptureAll", 1, f_display_capture),
    JS_CFUNC_DEF("setAutoRotate", 1, f_display_auto_rotate),
    JS_CFUNC_DEF("setTemperature", 1, f_display_set_temp),
    JS_CFUNC_DEF("temperature", 0, f_display_temp),
    JS_CFUNC_DEF("sun", 4, f_display_sun),
    JS_CFUNC_DEF("setIdle", 4, f_display_idle),
    JS_CFUNC_DEF("wake", 0, f_display_wake),
    JS_CFUNC_DEF("blank", 0, f_display_blank),
};

/* ---- lord.power ---- */

static JSValue f_power_state(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char j[4096];
    power_json(hw_power(), j, sizeof j);
    return JS_ParseJSON(c, j, strlen(j), "<power>");
}

static JSValue f_power_profiles(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return json_or_null(c, ppd_state_json());
}

static JSValue f_power_set_profile(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char p[24];
    if (!argc || arg_str(c, argv[0], p, sizeof p) || !ppd_profile_valid(p)) return JS_FALSE;
    return JS_NewBool(c, ppd_set(p) == 0);
}

/* What logind does with the power button and the lid: our drop-in if it exists, else logind's
 * defaults (poweroff, suspend, suspend). Read-only; changed by the power-buttons action. */
static JSValue f_power_buttons(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char power[16] = "poweroff", lid[16] = "suspend", lidac[16] = "", buf[1024];
    const char *files[] = { "/etc/systemd/logind.conf", "/etc/systemd/logind.conf.d/lordos.conf" };
    for (int f = 0; f < 2; f++) {
        int fd = open(files[f], O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t r = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (r <= 0) continue;
        buf[r] = 0;
        for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
            char v[16];
            if (sscanf(line, "HandlePowerKey=%15s", v) == 1) snprintf(power, sizeof power, "%s", v);
            else if (sscanf(line, "HandleLidSwitchExternalPower=%15s", v) == 1) snprintf(lidac, sizeof lidac, "%s", v);
            else if (sscanf(line, "HandleLidSwitch=%15s", v) == 1) snprintf(lid, sizeof lid, "%s", v);
        }
    }
    if (!lidac[0]) snprintf(lidac, sizeof lidac, "%s", lid);     /* logind: unset = same as HandleLidSwitch */
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "power", JS_NewString(c, power));
    JS_SetPropertyStr(c, o, "lid", JS_NewString(c, lid));
    JS_SetPropertyStr(c, o, "lidAC", JS_NewString(c, lidac));
    return o;
}

static const JSCFunctionListEntry power_funcs[] = {
    JS_CFUNC_DEF("state", 0, f_power_state),
    JS_CFUNC_DEF("profiles", 0, f_power_profiles),
    JS_CFUNC_DEF("setProfile", 1, f_power_set_profile),
    JS_CFUNC_DEF("buttons", 0, f_power_buttons),
};

/* ---- lord.bt ---- */

static JSValue f_bt_state(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return json_or_null(c, bt_state_json());
}

static JSValue f_bt_flag(JSContext *c, JSValueConst t, int argc, JSValueConst *argv, int magic) {
    (void)t;
    int on = argc && JS_ToBool(c, argv[0]) == 1;
    return JS_NewBool(c, (magic ? bt_discover(on) : bt_power(on)) == 0);
}

static JSValue f_bt_dev(JSContext *c, JSValueConst t, int argc, JSValueConst *argv, int magic) {
    (void)t;
    char path[128];
    if (!argc || arg_str(c, argv[0], path, sizeof path) || !bt_path_valid(path)) return JS_FALSE;
    int r = magic == 0 ? bt_pair(path) : magic == 1 ? bt_connect(path) : magic == 2 ? bt_disconnect(path) : bt_forget(path);
    return JS_NewBool(c, r == 0);
}

static JSValue f_bt_answer(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_FALSE;
    int id = arg_int(c, argv[0], -1), accept = JS_ToBool(c, argv[1]) == 1;
    char value[24] = "";
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2]) && arg_str(c, argv[2], value, sizeof value)) return JS_FALSE;
    return JS_NewBool(c, bt_answer(id, accept, value) == 0);
}

/* The authentication prompt can only be cancelled from here: no way to read or submit the field. */
static JSValue f_polkit_cancel(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc >= 1) pkagent_cancel(arg_int(c, argv[0], -1));
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry polkit_funcs[] = {
    JS_CFUNC_DEF("cancel", 1, f_polkit_cancel),
};

/* lord.portal.chosen(id, paths | null): the answer to an app's file dialog (ui/portal.c). */
#define PORTAL_MAX_PATHS 256
static JSValue f_portal_chosen(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_UNDEFINED;
    int id = arg_int(c, argv[0], -1);
    const char *paths[PORTAL_MAX_PATHS];
    int n = 0;
    if (argc > 1 && JS_IsArray(c, argv[1])) {
        uint32_t len = 0;
        JSValue lv = JS_GetPropertyStr(c, argv[1], "length");
        JS_ToUint32(c, &len, lv);
        JS_FreeValue(c, lv);
        for (uint32_t i = 0; i < len && n < PORTAL_MAX_PATHS; i++) {
            JSValue v = JS_GetPropertyUint32(c, argv[1], i);
            const char *s = JS_ToCString(c, v);
            JS_FreeValue(c, v);
            if (!s) continue;
            if (s[0] == '/') paths[n++] = s; else JS_FreeCString(c, s);
        }
    }
    portal_file_chosen(id, paths, n);
    for (int i = 0; i < n; i++) JS_FreeCString(c, paths[i]);
    return JS_UNDEFINED;
}

/* lord.portal.opened(id, code): the shell opened what an app asked for (0), the person said no (1) or it was refused (2) */
static JSValue f_portal_opened(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_UNDEFINED;
    portal_open_done(arg_int(c, argv[0], -1), arg_int(c, argv[1], 2));
    return JS_UNDEFINED;
}

/* lord.portal.still(id): the file an app asked to open is still the one lord-server checked (portal_open_still) */
static JSValue f_portal_still(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    return JS_NewBool(c, argc >= 1 && portal_open_still(arg_int(c, argv[0], -1)));
}

/* lord.portal.shortcutsChosen(id, [{ id, trigger }] | null): the person's answer to an app's global
 * shortcuts sheet (ui/portal.c); lord-server checks every trigger again. */
#define PORTAL_MAX_SHORTCUTS 32
static JSValue f_portal_shortcuts_chosen(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_UNDEFINED;
    int id = arg_int(c, argv[0], -1);
    if (argc < 2 || !JS_IsArray(c, argv[1])) { portal_shortcuts_chosen(id, NULL, NULL, -1); return JS_UNDEFINED; }
    static char ids[PORTAL_MAX_SHORTCUTS][64], trig[PORTAL_MAX_SHORTCUTS][64];
    const char *idp[PORTAL_MAX_SHORTCUTS], *tp[PORTAL_MAX_SHORTCUTS];
    uint32_t len = 0;
    JSValue lv = JS_GetPropertyStr(c, argv[1], "length");
    JS_ToUint32(c, &len, lv);
    JS_FreeValue(c, lv);
    int n = 0;
    for (uint32_t i = 0; i < len && n < PORTAL_MAX_SHORTCUTS; i++) {
        JSValue e = JS_GetPropertyUint32(c, argv[1], i);
        JSValue a = JS_GetPropertyStr(c, e, "id"), b = JS_GetPropertyStr(c, e, "trigger");
        if (JS_IsString(a) && JS_IsString(b) && !arg_str(c, a, ids[n], sizeof ids[n]) && !arg_str(c, b, trig[n], sizeof trig[n])) {
            idp[n] = ids[n];
            tp[n] = trig[n];
            n++;
        }
        JS_FreeValue(c, a);
        JS_FreeValue(c, b);
        JS_FreeValue(c, e);
    }
    portal_shortcuts_chosen(id, idp, tp, n);
    return JS_UNDEFINED;
}

/* lord.portal.shortcuts(): every kept decision, [{ app, id, description, trigger }] (Settings → Keyboard) */
static JSValue f_portal_shortcuts(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char *j = portal_shortcuts_list();
    JSValue v = j ? JS_ParseJSON(c, j, strlen(j), "<shortcuts>") : JS_NewArray(c);
    free(j);
    return v;
}

/* lord.portal.shortcutCheck(trigger, { session, app, id }): { trigger, kept, owner } as lord-server would
 * decide it, or null when it is not a trigger */
static JSValue f_portal_shortcut_check(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char trigger[64], app[128] = "", id[64] = "", out[64], owner[256];
    if (argc < 1 || arg_str(c, argv[0], trigger, sizeof trigger)) return JS_NULL;
    int session = 0;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(c, argv[1], "session");
        session = arg_int(c, v, 0);
        JS_FreeValue(c, v);
        v = JS_GetPropertyStr(c, argv[1], "app");
        if (JS_IsString(v) && arg_str(c, v, app, sizeof app)) app[0] = 0;
        JS_FreeValue(c, v);
        v = JS_GetPropertyStr(c, argv[1], "id");
        if (JS_IsString(v) && arg_str(c, v, id, sizeof id)) id[0] = 0;
        JS_FreeValue(c, v);
    }
    int kept = 0;
    if (portal_shortcut_check(trigger, session, app, id, out, sizeof out, &kept, owner, sizeof owner)) return JS_NULL;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "trigger", JS_NewString(c, out));
    JS_SetPropertyStr(c, o, "kept", JS_NewInt32(c, kept));
    JS_SetPropertyStr(c, o, "owner", JS_NewString(c, owner));
    return o;
}

/* lord.portal.setShortcut(app, id, trigger | ''): Settings changes a kept decision; false when refused */
static JSValue f_portal_set_shortcut(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char app[128], id[64], trigger[64];
    if (argc < 3 || arg_str(c, argv[0], app, sizeof app) || arg_str(c, argv[1], id, sizeof id) || arg_str(c, argv[2], trigger, sizeof trigger))
        return JS_FALSE;
    return JS_NewBool(c, portal_shortcut_set(app, id, trigger) == 0);
}

/* lord.portal.forgetShortcuts(app): Settings forgets every decision for an app */
static JSValue f_portal_forget_shortcuts(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char app[128];
    if (argc < 1 || arg_str(c, argv[0], app, sizeof app)) return JS_NewInt32(c, 0);
    return JS_NewInt32(c, portal_shortcuts_forget(app));
}

/* lord.portal.pauseShortcuts(on): while the shell records a combination, no app's shortcut fires */
static JSValue f_portal_pause_shortcuts(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    portal_shortcuts_pause(argc >= 1 && JS_ToBool(c, argv[0]) > 0);
    return JS_UNDEFINED;
}

/* A source of the ScreenCast chooser: { display: id } (lord.screen.displays) or { node, also } (wm.frameNode,
 * wm.sheetNodes), each node id with the serial of the node it names now (the share follows that node, never a later
 * one with its id). 0, or -1 for anything else or a window that isn't there. */
static int cast_source(JSContext *c, JSValueConst v, CastSource *out) {
    memset(out, 0, sizeof *out);
    if (!JS_IsObject(v)) return -1;
    JSValue d = JS_GetPropertyStr(c, v, "display"), n = JS_GetPropertyStr(c, v, "node");
    int r = -1;
    if (JS_IsNumber(d)) {
        out->kind = CAST_SRC_MONITOR;
        out->display = (uint32_t)arg_int(c, d, 0);
        r = 0;
    } else if (JS_IsNumber(n) && arg_int(c, n, 0) > 0) {
        out->kind = CAST_SRC_WINDOW;
        out->node = arg_int(c, n, 0);
        Node *w = node_get(out->node);
        if (w) out->serial = w->serial;
        JSValue a = JS_GetPropertyStr(c, v, "also");
        if (JS_IsArray(c, a)) {
            uint32_t len = 0;
            JSValue lv = JS_GetPropertyStr(c, a, "length");
            JS_ToUint32(c, &len, lv);
            JS_FreeValue(c, lv);
            for (uint32_t i = 0; i < len && out->nalso < CAST_ALSO_MAX; i++) {
                JSValue e = JS_GetPropertyUint32(c, a, i);
                int id = JS_IsNumber(e) ? arg_int(c, e, 0) : 0;
                Node *sh = id > 0 ? node_get(id) : NULL;
                if (sh) { out->also[out->nalso] = id; out->also_serial[out->nalso++] = sh->serial; }
                JS_FreeValue(c, e);
            }
        }
        JS_FreeValue(c, a);
        r = out->serial ? 0 : -1;
    }
    JS_FreeValue(c, d);
    JS_FreeValue(c, n);
    return r;
}

/* lord.portal.cast(id, sources | null): the person's picks in the ScreenCast chooser (null or [] is Cancel) */
static JSValue f_portal_cast(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_UNDEFINED;
    int id = arg_int(c, argv[0], -1), n = 0, bad = 0;
    CastSource src[CAST_MAX_STREAMS];
    if (argc > 1 && JS_IsArray(c, argv[1])) {
        uint32_t len = 0;
        JSValue lv = JS_GetPropertyStr(c, argv[1], "length");
        JS_ToUint32(c, &len, lv);
        JS_FreeValue(c, lv);
        if (len > CAST_MAX_STREAMS) bad = 1;
        for (uint32_t i = 0; i < len && !bad; i++) {
            JSValue v = JS_GetPropertyUint32(c, argv[1], i);
            if (cast_source(c, v, &src[n])) bad = 1; else n++;
            JS_FreeValue(c, v);
        }
    }
    portal_cast_chosen(id, src, bad ? -1 : n);
    return JS_UNDEFINED;
}

/* lord.portal.castThumb(id, source): a picture of it for the chooser of session id (a lord.media id), or 0 */
static JSValue f_portal_cast_thumb(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    CastSource src;
    if (argc < 2 || cast_source(c, argv[1], &src)) return JS_NewInt32(c, 0);
    return JS_NewInt32(c, portal_cast_thumb(arg_int(c, argv[0], -1), &src));
}

/* lord.portal.castShown(id) / castStop(id): the sharing indicator of session id is on screen / its Stop */
static JSValue f_portal_cast_shown(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc >= 1) portal_cast_shown(arg_int(c, argv[0], -1));
    return JS_UNDEFINED;
}
static JSValue f_portal_cast_stop(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc >= 1) portal_cast_stop(arg_int(c, argv[0], -1));
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry portal_funcs[] = {
    JS_CFUNC_DEF("chosen", 2, f_portal_chosen),
    JS_CFUNC_DEF("opened", 2, f_portal_opened),
    JS_CFUNC_DEF("still", 1, f_portal_still),
    JS_CFUNC_DEF("shortcutsChosen", 2, f_portal_shortcuts_chosen),
    JS_CFUNC_DEF("shortcuts", 0, f_portal_shortcuts),
    JS_CFUNC_DEF("shortcutCheck", 2, f_portal_shortcut_check),
    JS_CFUNC_DEF("setShortcut", 3, f_portal_set_shortcut),
    JS_CFUNC_DEF("forgetShortcuts", 1, f_portal_forget_shortcuts),
    JS_CFUNC_DEF("pauseShortcuts", 1, f_portal_pause_shortcuts),
    JS_CFUNC_DEF("cast", 2, f_portal_cast),
    JS_CFUNC_DEF("castThumb", 2, f_portal_cast_thumb),
    JS_CFUNC_DEF("castShown", 1, f_portal_cast_shown),
    JS_CFUNC_DEF("castStop", 1, f_portal_cast_stop),
};

/* lord.sni: the tray icons of Linux apps (ui/sni.c). Only the desktop's own context has it, never an isolated app. */
static JSValue f_sni_list(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    HBuf b = { 0 };
    sni_list(&b);
    char *j = hb_take(&b);
    JSValue v = j ? JS_ParseJSON(c, j, strlen(j), "<sni>") : JS_NewArray(c);
    free(j);
    return v;
}

/* lord.sni.pixels(id): { width, height, data } (straight RGBA) for <Image pixels>, or null */
static JSValue f_sni_pixels(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int w = 0, h = 0;
    const uint8_t *px = argc >= 1 ? sni_pixels(arg_int(c, argv[0], 0), &w, &h) : NULL;
    if (!px) return JS_NULL;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "width", JS_NewInt32(c, w));
    JS_SetPropertyStr(c, o, "height", JS_NewInt32(c, h));
    JS_SetPropertyStr(c, o, "data", JS_NewArrayBufferCopy(c, px, (size_t)w * h * 4));
    return o;
}

/* lord.sni.activate(id, x, y, secondary) */
static JSValue f_sni_activate(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 3) return JS_FALSE;
    int secondary = argc > 3 && JS_ToBool(c, argv[3]) > 0;
    return JS_NewBool(c, !sni_activate(arg_int(c, argv[0], 0), arg_int(c, argv[1], 0), arg_int(c, argv[2], 0), secondary));
}

/* lord.sni.menu(id, x, y): 1 its menu comes as the "sni-menu" event, 0 the app shows its own, -1 none */
static JSValue f_sni_menu(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 3) return JS_NewInt32(c, -1);
    return JS_NewInt32(c, sni_menu(arg_int(c, argv[0], 0), arg_int(c, argv[1], 0), arg_int(c, argv[2], 0)));
}

/* lord.sni.clicked(id, entry): an entry of that menu was chosen */
static JSValue f_sni_clicked(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_FALSE;
    return JS_NewBool(c, !sni_menu_clicked(arg_int(c, argv[0], 0), arg_int(c, argv[1], -1)));
}

/* lord.sni.closed(id): the menu shown for that item closed */
static JSValue f_sni_closed(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc >= 1) sni_menu_closed(arg_int(c, argv[0], 0));
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry sni_funcs[] = {
    JS_CFUNC_DEF("list", 0, f_sni_list),
    JS_CFUNC_DEF("pixels", 1, f_sni_pixels),
    JS_CFUNC_DEF("activate", 4, f_sni_activate),
    JS_CFUNC_DEF("menu", 3, f_sni_menu),
    JS_CFUNC_DEF("clicked", 2, f_sni_clicked),
    JS_CFUNC_DEF("closed", 1, f_sni_closed),
};

static const JSCFunctionListEntry bt_funcs[] = {
    JS_CFUNC_DEF("state", 0, f_bt_state),
    JS_CFUNC_MAGIC_DEF("power", 1, f_bt_flag, 0),
    JS_CFUNC_MAGIC_DEF("discover", 1, f_bt_flag, 1),
    JS_CFUNC_MAGIC_DEF("pair", 1, f_bt_dev, 0),
    JS_CFUNC_MAGIC_DEF("connect", 1, f_bt_dev, 1),
    JS_CFUNC_MAGIC_DEF("disconnect", 1, f_bt_dev, 2),
    JS_CFUNC_MAGIC_DEF("forget", 1, f_bt_dev, 3),
    JS_CFUNC_DEF("answer", 3, f_bt_answer),
};

/* ---- lord.mime ---- */

static JSValue f_mime_get(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char mime[MIME_MAX], id[DESKTOP_ID_MAX];
    if (!argc || arg_str(c, argv[0], mime, sizeof mime) || !mime_valid(mime) || defaults_lookup(mime, id, sizeof id)) return JS_NULL;
    return JS_NewString(c, id);
}

static JSValue f_mime_set(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char mime[MIME_MAX], id[DESKTOP_ID_MAX];
    if (argc < 2 || arg_str(c, argv[0], mime, sizeof mime) || arg_str(c, argv[1], id, sizeof id)) return JS_FALSE;
    return JS_NewBool(c, mime_valid(mime) && desktop_id_valid(id) && defaults_set(mime, id) == 0);
}

/* lord.mime.resolve(mime) → { id: 'x.desktop', explicit, source: 'user'|'system'|'declared' } or
 * null: the app launchWith('', …) uses (apps_default_for); explicit only for the user's own choice. */
static JSValue f_mime_resolve(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char mime[MIME_MAX], id[DESKTOP_ID_MAX];
    if (!argc || arg_str(c, argv[0], mime, sizeof mime) || !mime_valid(mime)) return JS_NULL;
    int r = apps_default_for(mime, id, sizeof id);
    if (r <= 0) return JS_NULL;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "id", JS_NewString(c, id));
    JS_SetPropertyStr(c, o, "explicit", JS_NewBool(c, r == APPS_DEFAULT_USER));
    JS_SetPropertyStr(c, o, "source", JS_NewString(c, r == APPS_DEFAULT_USER ? "user" : r == APPS_DEFAULT_LISTED ? "system" : "declared"));
    return o;
}

/* lord.mime.unset(mime): the user's choice for that type is removed (back to automatic). */
static JSValue f_mime_unset(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char mime[MIME_MAX];
    if (!argc || arg_str(c, argv[0], mime, sizeof mime)) return JS_FALSE;
    return JS_NewBool(c, mime_valid(mime) && defaults_unset(mime) == 0);
}

static JSValue f_mime_guess(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char path[1024], mime[MIME_MAX];
    if (!argc || arg_str(c, argv[0], path, sizeof path) || defaults_guess(path, mime, sizeof mime)) return JS_NULL;
    return JS_NewString(c, mime);
}

static JSValue f_mime_terminal(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char id[DESKTOP_ID_MAX];
    return defaults_terminal(id, sizeof id) ? JS_NULL : JS_NewString(c, id);
}

static JSValue f_mime_set_terminal(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char id[DESKTOP_ID_MAX];
    if (!argc || arg_str(c, argv[0], id, sizeof id) || !desktop_id_valid(id)) return JS_FALSE;
    return JS_NewBool(c, defaults_set_terminal(id) == 0);
}

static const JSCFunctionListEntry mime_funcs[] = {
    JS_CFUNC_DEF("get", 1, f_mime_get),
    JS_CFUNC_DEF("set", 2, f_mime_set),
    JS_CFUNC_DEF("resolve", 1, f_mime_resolve),
    JS_CFUNC_DEF("unset", 1, f_mime_unset),
    JS_CFUNC_DEF("guess", 1, f_mime_guess),
    JS_CFUNC_DEF("terminal", 0, f_mime_terminal),
    JS_CFUNC_DEF("setTerminal", 1, f_mime_set_terminal),
};

/* ---- lord.timedate ---- */

static JSValue f_td_refresh(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    hw_timedate_refresh();
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry td_funcs[] = { JS_CFUNC_DEF("refresh", 0, f_td_refresh) };

/* ---- lord.notifyd (Linux apps' notifications, hw/notifyd.c) ---- */

static JSValue f_nd_closed(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int id = argc ? arg_int(c, argv[0], 0) : 0, reason = argc > 1 ? arg_int(c, argv[1], 2) : 2;
    if (id > 0) notifyd_closed((uint32_t)id, (uint32_t)reason);
    return JS_UNDEFINED;
}

static JSValue f_nd_action(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char key[129];
    int id = argc ? arg_int(c, argv[0], 0) : 0;
    if (id > 0 && argc > 1 && !arg_str(c, argv[1], key, sizeof key)) notifyd_action((uint32_t)id, key);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry nd_funcs[] = { JS_CFUNC_DEF("closed", 2, f_nd_closed), JS_CFUNC_DEF("action", 2, f_nd_action) };

/* ---- lord.mpris (Linux players in Now Playing and the media keys, hw/mpris.c) ---- */

static JSValue f_mpris_list(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return json_or_null(c, mpris_json());
}

static JSValue f_mpris_control(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    static const char *const cmds[] = { "toggle", "play", "pause", "next", "prev", "seek" };
    char name[256], cmd[8];
    double ms = 0;
    if (argc < 2 || arg_str(c, argv[0], name, sizeof name) || arg_str(c, argv[1], cmd, sizeof cmd)) return JS_FALSE;
    if (argc > 2 && JS_ToFloat64(c, &ms, argv[2])) return JS_FALSE;
    for (int i = 0; i < (int)(sizeof cmds / sizeof cmds[0]); i++)
        if (!strcmp(cmd, cmds[i])) return JS_NewBool(c, mpris_control(name, MPRIS_TOGGLE + i, ms) == 0);
    return JS_FALSE;
}

static JSValue f_mpris_refresh(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    mpris_refresh();
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry mpris_funcs[] = {
    JS_CFUNC_DEF("list", 0, f_mpris_list),
    JS_CFUNC_DEF("control", 3, f_mpris_control),
    JS_CFUNC_DEF("refresh", 0, f_mpris_refresh),
};

/* ---- lord.hwinfo (the Store's "Suggested for your computer", docs/PACKAGES.md) ---- */

/* LORD_HWSCAN_ROOT (development and screenshots only): read a fake tree instead of the real
 * system; the facts then say fixture: true and the Store marks them as test data. Only an absolute
 * path without ".." is taken. */
static const char *hwscan_root(void) {
    const char *r = getenv("LORD_HWSCAN_ROOT");
    if (!r || r[0] != '/' || strstr(r, "..") || strlen(r) > 200) return "";
    return r;
}

static JSValue f_hwinfo_scan(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return json_or_null(c, hwscan_json(hwscan_root()));
}

static const JSCFunctionListEntry hwinfo_funcs[] = { JS_CFUNC_DEF("scan", 0, f_hwinfo_scan) };

/* ---- lord.sysinfo (System Information, docs/BENCHMARK.md) ---- */

/* The report of this machine (or of the development fixture), with LordOS's version; NULL on OOM. */
static SysReport *sysinfo_collect(void) {
    SysReport *r = malloc(sizeof *r);
    if (!r) return NULL;
    sysreport_collect(r, hwscan_root(), NULL, NULL);      /* the same development fixtures as the Store's */
    snprintf(r->sw.lordos, sizeof r->sw.lordos, "%s%s%s", LORD_VERSION, LORD_CHANNEL[0] ? "-" : "", LORD_CHANNEL);
    return r;
}

static JSValue f_sysinfo_report(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    const char *root = hwscan_root();
    SysReport *r = sysinfo_collect();
    if (!r) return JS_NULL;
    JSValue v = json_or_null(c, sysreport_json(r));
    free(r);
    if (!JS_IsObject(v)) return v;
    GpuInfo g;
    gpu_info(&g);
    JSValue s = JS_NewObject(c);
    JS_SetPropertyStr(c, s, "composition", JS_NewString(c, lord_composition()));
    JS_SetPropertyStr(c, s, "gpuInit", JS_NewString(c, g.why));
    JS_SetPropertyStr(c, s, "renderer", JS_NewString(c, g.renderer));
    JS_SetPropertyStr(c, s, "renderNode", JS_NewString(c, g.render_node));
    JS_SetPropertyStr(c, s, "dmabuf", JS_NewBool(c, g.dmabuf));
    JS_SetPropertyStr(c, s, "scanouts", JS_NewFloat64(c, (double)g.scanouts));
    JS_SetPropertyStr(c, s, "fixture", JS_NewBool(c, root[0] != 0));
    JS_SetPropertyStr(c, v, "session", s);
    return v;
}

static JSValue f_sysinfo_markdown(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    SysReport *r = sysinfo_collect();
    char *md = r ? sysreport_markdown(r) : NULL;
    free(r);
    if (!md) return JS_NULL;
    GpuInfo g;
    gpu_info(&g);
    const char *comp = lord_composition();
    char sess[512];
    if (!strcmp(comp, "gpu"))
        snprintf(sess, sizeof sess, "\n## This session\n\nThe screen is composed on the GPU (%s); %lu frames shown by direct scanout since the session started.\n",
                 g.renderer, g.scanouts);
    else
        snprintf(sess, sizeof sess, "\n## This session\n\nThe screen is composed on the CPU (`%s`%s%s).\n",
                 comp, !strcmp(comp, "gpu-off") && g.why[0] ? ", GPU: " : "", !strcmp(comp, "gpu-off") ? g.why : "");
    size_t a = strlen(md), b = strlen(sess);
    char *all = malloc(a + b + 1);
    if (!all) { free(md); return JS_NULL; }
    memcpy(all, md, a);
    memcpy(all + a, sess, b + 1);
    free(md);
    JSValue v = JS_NewString(c, all);
    free(all);
    return v;
}

static JSValue f_sysinfo_facts(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    const char *root = hwscan_root();
    SrFacts f;
    sr_facts(&f, root);
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "uid", JS_NewInt32(c, f.uid));
    JS_SetPropertyStr(c, o, "euid", JS_NewInt32(c, f.euid));
    JS_SetPropertyStr(c, o, "cgroup", JS_NewString(c, f.cgroup));
    JS_SetPropertyStr(c, o, "leafRoot", f.leaf_root < 0 ? JS_NULL : JS_NewBool(c, f.leaf_root));
    JS_SetPropertyStr(c, o, "parentRoot", f.parent_root < 0 ? JS_NULL : JS_NewBool(c, f.parent_root));
    JS_SetPropertyStr(c, o, "appScopes", JS_NewInt32(c, f.app_scopes));
    int l = sr_tcp_listening(root, 5673);   /* the remote desktop's port (docs/REMOTE.md) */
    JS_SetPropertyStr(c, o, "remoteListening", l < 0 ? JS_NULL : JS_NewBool(c, l));
    JS_SetPropertyStr(c, o, "fixture", JS_NewBool(c, root[0] != 0));
    return o;
}

static JSValue f_sysinfo_probe(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    const char *j = brokersrv_probe_result();
    return j ? JS_ParseJSON(c, j, strlen(j), "<probe>") : JS_NULL;
}

/* (0.8.7) the catalogue's snapshot, verified in C before JS sees any of it; read again on every call (the
 * Compatibility tab asks once when it opens), so a package update is seen without restarting the session */
static JSValue f_sysinfo_community(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char dir[4096], err[160] = "";
    const char *assets = js_sys_assets();
    JSValue o = JS_NewObject(c);
    char *json = NULL;
    size_t len = 0;
    HwSnapState st = HWSNAP_MISSING;
    if (assets && assets[0] == '/' && (size_t)snprintf(dir, sizeof dir, "%s/hwreport", assets) < sizeof dir)
        st = hwsnap_load(dir, codesig_key(), &json, &len, err, sizeof err);
    JS_SetPropertyStr(c, o, "state", JS_NewString(c, hwsnap_state_name(st)));
    if (st == HWSNAP_OK) {
        JSValue v = JS_ParseJSON(c, json, len, "<hwsnapshot>");
        if (JS_IsException(v)) { JS_FreeValue(c, JS_GetException(c)); JS_SetPropertyStr(c, o, "state", JS_NewString(c, "invalid")); }
        else JS_SetPropertyStr(c, o, "snapshot", v);
    } else if (st != HWSNAP_MISSING) {
        fprintf(stderr, "sysinfo: hardware snapshot not used: %s (%s)\n", hwsnap_state_name(st), err);
    }
    free(json);
    return o;
}

/* ---- lord.sysinfo.bench (LordBench) ---- */

/* what a result records about this session: how the screen is composed and each active display's
 * connector, mode, refresh, scale and rotation (no names, vendors or identities) */
static char *bench_session_json(JSContext *c) {
    GpuInfo g;
    gpu_info(&g);
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "composition", JS_NewString(c, lord_composition()));
    JS_SetPropertyStr(c, o, "gpuInit", JS_NewString(c, g.why ? g.why : ""));
    JS_SetPropertyStr(c, o, "renderer", JS_NewString(c, g.renderer ? g.renderer : ""));
    JSValue outs = JS_NewArray(c);
    JSValue d = json_or_null(c, display_list_json());
    JSValue list = JS_IsObject(d) ? JS_GetPropertyStr(c, d, "outputs") : JS_UNDEFINED;
    uint32_t k = 0;
    for (uint32_t i = 0; JS_IsArray(c, list) && i < 16; i++) {
        JSValue e = JS_GetPropertyUint32(c, list, i);
        if (!JS_IsObject(e)) { JS_FreeValue(c, e); break; }
        JSValue act = JS_GetPropertyStr(c, e, "active");
        if (JS_ToBool(c, act) > 0) {
            JSValue x = JS_NewObject(c);
            static const char *const keep[] = { "connector", "builtin", "mode", "scale", "rotation" };
            for (int f = 0; f < 5; f++) JS_SetPropertyStr(c, x, keep[f], JS_GetPropertyStr(c, e, keep[f]));
            JS_SetPropertyUint32(c, outs, k++, x);
        }
        JS_FreeValue(c, act);
        JS_FreeValue(c, e);
    }
    JS_FreeValue(c, list);
    JS_FreeValue(c, d);
    JS_SetPropertyStr(c, o, "displays", outs);
    JSValue str = JS_JSONStringify(c, o, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(c, o);
    const char *s = JS_ToCString(c, str);
    char *r = s ? strdup(s) : NULL;
    if (s) JS_FreeCString(c, s);
    JS_FreeValue(c, str);
    return r;
}

static JSValue f_bench_start(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *mode = argc > 0 ? JS_ToCString(c, argv[0]) : NULL;
    if (!mode) return JS_FALSE;
    int sus = argc > 1 ? arg_int(c, argv[1], 0) : 0;
    char *extra = bench_session_json(c);
    int r = lbrun_start(mode, sus, extra);
    free(extra);
    JS_FreeCString(c, mode);
    return JS_NewBool(c, r == 0);
}

static JSValue f_bench_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return json_or_null(c, lbrun_poll());
}

static JSValue f_bench_cancel(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    lbrun_cancel();
    return JS_UNDEFINED;
}

static JSValue f_bench_saved(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int which = argc > 0 ? arg_int(c, argv[0], 0) : 0;
    if (which < 0 || which > 3) return JS_NULL;
    JSValue v = json_or_null(c, lbrun_saved(which));
    if (JS_IsException(v)) { JS_FreeValue(c, JS_GetException(c)); return JS_NULL; }
    return v;
}

static JSValue f_bench_boots(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return json_or_null(c, lbrun_boot_history("/var/lib/lordos/boot-times.jsonl"));
}

/* the boot time test's state (bench/boottest.h): {total, left, at} while one is set, else null */
static JSValue f_bench_boot_test(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    BootTest b;
    if (bt_read(BT_PATH, &b)) return JS_NULL;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "total", JS_NewInt32(c, b.total));
    JS_SetPropertyStr(c, o, "left", JS_NewInt32(c, b.left));
    JS_SetPropertyStr(c, o, "at", JS_NewFloat64(c, (double)b.at));
    return o;
}

static const JSCFunctionListEntry bench_funcs[] = {
    JS_CFUNC_DEF("start", 2, f_bench_start),
    JS_CFUNC_DEF("poll", 0, f_bench_poll),
    JS_CFUNC_DEF("cancel", 0, f_bench_cancel),
    JS_CFUNC_DEF("saved", 1, f_bench_saved),
    JS_CFUNC_DEF("boots", 0, f_bench_boots),
    JS_CFUNC_DEF("bootTest", 0, f_bench_boot_test),
};

static JSValue f_sysinfo_peripherals(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return json_or_null(c, periph_json(hwscan_root(), NULL));
}

static const JSCFunctionListEntry sysinfo_funcs[] = {
    JS_CFUNC_DEF("report", 0, f_sysinfo_report),
    JS_CFUNC_DEF("probe", 0, f_sysinfo_probe),
    JS_CFUNC_DEF("facts", 0, f_sysinfo_facts),
    JS_CFUNC_DEF("markdown", 0, f_sysinfo_markdown),
    JS_CFUNC_DEF("peripherals", 0, f_sysinfo_peripherals),
    JS_CFUNC_DEF("community", 0, f_sysinfo_community),
};

/* ---- lord.browserFrame (Settings → Appearance; hw/brframe.c, docs/WAYLAND.md) ---- */

/* The folders are fixed: $XDG_CONFIG_HOME (absolute) or ~/.config, and ~ for the legacy ~/.mozilla. */
static int brf_dirs(char *cfg, size_t n, const char **home) {
    const char *h = getenv("HOME"), *x = getenv("XDG_CONFIG_HOME");
    if (!h || h[0] != '/') return -1;
    *home = h;
    if (x && x[0] == '/' && !strstr(x, "..")) return (size_t)snprintf(cfg, n, "%s", x) < n ? 0 : -1;
    return (size_t)snprintf(cfg, n, "%s/.config", h) < n ? 0 : -1;
}

static JSValue f_brf_apply(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char which[16] = "all", cfg[4096];
    const char *home;
    if (argc < 1 || !JS_IsBool(argv[0])) return JS_NULL;
    if (argc > 1 && arg_str(c, argv[1], which, sizeof which)) return JS_NULL;
    int ch = !strcmp(which, "all") || !strcmp(which, "chromium"), ff = !strcmp(which, "all") || !strcmp(which, "firefox");
    if ((!ch && !ff) || brf_dirs(cfg, sizeof cfg, &home)) return JS_NULL;
    int on = JS_ToBool(c, argv[0]);
    BrfResult rc = {0}, rf = {0};
    if (ch) brf_chromium_apply(cfg, on, &rc);
    if (ff) brf_firefox_apply(cfg, home, on, &rf);
    return json_or_null(c, brf_result_json(&rc, &rf));
}

static JSValue f_brf_tidy(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char cfg[4096];
    const char *home;
    return JS_NewInt32(c, brf_dirs(cfg, sizeof cfg, &home) ? 0 : brf_firefox_tidy(cfg, home));
}

static JSValue f_brf_running(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char cfg[4096];
    const char *home;
    return JS_NewBool(c, !brf_dirs(cfg, sizeof cfg, &home) && brf_chromium_running(cfg));
}

static JSValue f_brf_state(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char cfg[4096];
    const char *home;
    int on = 0, off = 0;
    if (!brf_dirs(cfg, sizeof cfg, &home)) brf_chromium_state(cfg, &on, &off);
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "chromiumOn", JS_NewInt32(c, on));
    JS_SetPropertyStr(c, o, "chromiumOff", JS_NewInt32(c, off));
    return o;
}

static const JSCFunctionListEntry brf_funcs[] = {
    JS_CFUNC_DEF("state", 0, f_brf_state),
    JS_CFUNC_DEF("apply", 2, f_brf_apply),
    JS_CFUNC_DEF("tidy", 0, f_brf_tidy),
    JS_CFUNC_DEF("chromiumRunning", 0, f_brf_running),
};

static void add(JSContext *c, JSValue lord, const char *name, const JSCFunctionListEntry *f, int n) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, f, n);
    JS_SetPropertyStr(c, lord, name, o);
}

void js_hw_register(JSContext *c, JSValue lord) {
    add(c, lord, "display", display_funcs, sizeof display_funcs / sizeof display_funcs[0]);
    add(c, lord, "power", power_funcs, sizeof power_funcs / sizeof power_funcs[0]);
    add(c, lord, "bt", bt_funcs, sizeof bt_funcs / sizeof bt_funcs[0]);
    add(c, lord, "polkit", polkit_funcs, sizeof polkit_funcs / sizeof polkit_funcs[0]);
    /* lord.portal already holds set (js_wm.c): these go on the same object */
    JSValue po = JS_GetPropertyStr(c, lord, "portal");
    if (JS_IsObject(po)) JS_SetPropertyFunctionList(c, po, portal_funcs, sizeof portal_funcs / sizeof portal_funcs[0]);
    else add(c, lord, "portal", portal_funcs, sizeof portal_funcs / sizeof portal_funcs[0]);
    JS_FreeValue(c, po);
    add(c, lord, "sni", sni_funcs, sizeof sni_funcs / sizeof sni_funcs[0]);
    add(c, lord, "mime", mime_funcs, sizeof mime_funcs / sizeof mime_funcs[0]);
    add(c, lord, "timedate", td_funcs, sizeof td_funcs / sizeof td_funcs[0]);
    add(c, lord, "notifyd", nd_funcs, sizeof nd_funcs / sizeof nd_funcs[0]);
    add(c, lord, "mpris", mpris_funcs, sizeof mpris_funcs / sizeof mpris_funcs[0]);
    add(c, lord, "hwinfo", hwinfo_funcs, sizeof hwinfo_funcs / sizeof hwinfo_funcs[0]);
    add(c, lord, "sysinfo", sysinfo_funcs, sizeof sysinfo_funcs / sizeof sysinfo_funcs[0]);
    JSValue si = JS_GetPropertyStr(c, lord, "sysinfo");
    JSValue bo = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, bo, bench_funcs, sizeof bench_funcs / sizeof bench_funcs[0]);
    JS_SetPropertyStr(c, si, "bench", bo);
    JS_FreeValue(c, si);
    add(c, lord, "browserFrame", brf_funcs, sizeof brf_funcs / sizeof brf_funcs[0]);
    js_fan_register(c, lord);           /* lord.fan: Settings → Cooling and Monitor (js_fan.c) */
}
