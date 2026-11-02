/* js_fan.c — lord.fan: Settings → Cooling and Monitor talk to axia-fand (docs/FANS.md).
 *
 *   lord.fan.status()            the daemon's status object, or null if it is not running
 *   lord.fan.setProfile(name)    auto|quiet|balanced|performance|always|custom   → '' or an error code
 *   lord.fan.setCurve(points)    [[°C, %], ...] (1..8 points, 30..90 °C rising, 0..100 % never falling)
 *   lord.fan.force(v)            'on' | 'off' | minutes (1..1440)
 *   lord.fan.caseConfig()        'argon-one-v5' | 'none' | null (not a LordOS Raspberry Pi image)
 *
 * Everything is validated here and again in axia-fand (which also checks that the user is an
 * administrator). The requests are one short line over /run/lordos/fand.sock; the daemon answers
 * at once, and a call never waits more than FAN_WAIT_MS. LORD_FAND_SOCK and LORD_FAN_CASE_FILE
 * point elsewhere for tests and screenshots (they only change what this user's session reads). */
#include "js_fan.h"
#include "js_err.h"
#include "../fan/fancurve.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define FAN_SOCK      "/run/lordos/fand.sock"
#define FAN_CASE_FILE "/boot/firmware/lordos-case.txt"
#define FAN_CASE_NOTE "/var/lib/lordos/case"
#define FAN_WAIT_MS   300
#define FAN_REPLY_MAX 8192

static long long ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Sends one request line and reads the whole answer. Returns its length, -1 if the daemon is not
 * there or too slow. */
static int fan_ask(const char *req, char *out, size_t n) {
    const char *path = getenv("LORD_FAND_SOCK");
    if (!path || !*path) path = FAN_SOCK;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof a.sun_path) return -1;
    memcpy(a.sun_path, path, strlen(path) + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    char line[FAN_REQ_MAX + 2];
    int l = snprintf(line, sizeof line, "%s\n", req);
    if (l <= 0 || (size_t)l >= sizeof line || send(fd, line, (size_t)l, MSG_NOSIGNAL) != l) { close(fd); return -1; }
    size_t got = 0;
    long long end = ms_now() + FAN_WAIT_MS;
    for (;;) {
        int left = (int)(end - ms_now());
        if (left <= 0) { close(fd); return -1; }
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, left) <= 0) { if (errno == EINTR) continue; close(fd); return -1; }
        ssize_t r = recv(fd, out + got, n - 1 - got, 0);
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (r <= 0) break;
        got += (size_t)r;
        if (got >= n - 1) break;
    }
    close(fd);
    out[got] = 0;
    return (int)got;
}

/* OK → '', ERR <code> → '<code>', no daemon → 'fan.unavailable' */
static JSValue fan_command(JSContext *c, const char *req) {
    char out[128];
    if (fan_ask(req, out, sizeof out) < 0) return JS_NewString(c, "fan.unavailable");
    out[strcspn(out, "\n")] = 0;
    if (!strcmp(out, "OK")) return JS_NewString(c, "");
    if (!strncmp(out, "ERR fan.", 8) && strlen(out) < 40) return JS_NewString(c, out + 4);
    return JS_NewString(c, "fan.badRequest");
}

static JSValue f_status(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char *out = malloc(FAN_REPLY_MAX);
    if (!out) return JS_NULL;
    int n = fan_ask("STATUS", out, FAN_REPLY_MAX);
    JSValue v = n > 0 && out[0] == '{' ? JS_ParseJSON(c, out, (size_t)n, "<fan>") : JS_NULL;
    free(out);
    if (JS_IsException(v)) { JS_FreeValue(c, JS_GetException(c)); return JS_NULL; }
    return v;
}

static JSValue f_set_profile(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsString(argv[0])) return js_throw_code(c, "fan.badRequest", NULL);
    const char *s = JS_ToCString(c, argv[0]);
    if (!s) return JS_EXCEPTION;
    FanProfile p;
    char req[48];
    int ok = strlen(s) < 20 && fan_profile_parse(s, &p) == 0;
    if (ok) snprintf(req, sizeof req, "PROFILE %s", fan_profile_name(p));
    JS_FreeCString(c, s);
    if (!ok) return js_throw_code(c, "fan.badRequest", NULL);
    return fan_command(c, req);
}

static JSValue f_set_curve(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsArray(c, argv[0])) return js_throw_code(c, "fan.badRequest", NULL);
    JSValue lv = JS_GetPropertyStr(c, argv[0], "length");
    int32_t n = -1;
    if (JS_ToInt32(c, &n, lv)) n = -1;
    JS_FreeValue(c, lv);
    if (n < 1 || n > FAN_MAX_POINTS) return js_throw_code(c, "fan.badRequest", NULL);
    FanCurve cu = { 0 };
    for (int i = 0; i < n; i++) {
        JSValue pt = JS_GetPropertyUint32(c, argv[0], (uint32_t)i);
        JSValue a = JS_GetPropertyUint32(c, pt, 0), b = JS_GetPropertyUint32(c, pt, 1);
        double x = -1, y = -1;
        int bad = !JS_IsNumber(a) || !JS_IsNumber(b) || JS_ToFloat64(c, &x, a) || JS_ToFloat64(c, &y, b);
        JS_FreeValue(c, a); JS_FreeValue(c, b); JS_FreeValue(c, pt);
        if (bad || x != (int)x || y != (int)y) return js_throw_code(c, "fan.badRequest", NULL);
        cu.p[i].temp = (int)x;
        cu.p[i].pct = (int)y;
        cu.n++;
    }
    if (fan_curve_valid(&cu)) return js_throw_code(c, "fan.badCurve", NULL);
    char req[FAN_REQ_MAX];
    size_t k = (size_t)snprintf(req, sizeof req, "CURVE ");
    for (int i = 0; i < cu.n; i++) k += (size_t)snprintf(req + k, sizeof req - k, "%s%d:%d", i ? "," : "", cu.p[i].temp, cu.p[i].pct);
    return fan_command(c, req);
}

static JSValue f_force(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char req[32];
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char *s = JS_ToCString(c, argv[0]);
        if (!s) return JS_EXCEPTION;
        int ok = !strcmp(s, "on") || !strcmp(s, "off");
        if (ok) snprintf(req, sizeof req, "FORCE %s", s);
        JS_FreeCString(c, s);
        if (!ok) return js_throw_code(c, "fan.badRequest", NULL);
    } else if (argc >= 1 && JS_IsNumber(argv[0])) {
        double m;
        if (JS_ToFloat64(c, &m, argv[0]) || m != (int)m || m < 1 || m > FAN_FORCE_MAX_MIN) return js_throw_code(c, "fan.badRequest", NULL);
        snprintf(req, sizeof req, "FORCE %d", (int)m);
    } else return js_throw_code(c, "fan.badRequest", NULL);
    return fan_command(c, req);
}

static JSValue f_case(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    const char *path = getenv("LORD_FAN_CASE_FILE");
    if (!path || !*path) {                      /* the boot partition is root's alone: the helper leaves a copy */
        char id[32];
        int fd = open(FAN_CASE_NOTE, O_RDONLY | O_CLOEXEC);
        ssize_t r = fd < 0 ? -1 : read(fd, id, sizeof id - 1);
        if (fd >= 0) close(fd);
        if (r > 0) { id[r] = 0; return JS_NewString(c, strcmp(id, "argon-one-v5") ? "none" : "argon-one-v5"); }
        path = FAN_CASE_FILE;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == EACCES ? JS_NewString(c, "none") : JS_NULL;
    char b[1024];
    ssize_t r = read(fd, b, sizeof b - 1);
    close(fd);
    if (r < 0) return JS_NULL;
    b[r] = 0;
    return JS_NewString(c, strstr(b, "\n[pi5]\ndtoverlay=dwc2,dr_mode=host\n") && strstr(b, "Argon ONE V5 case") ? "argon-one-v5" : "none");
}

static const JSCFunctionListEntry fan_funcs[] = {
    JS_CFUNC_DEF("status", 0, f_status),
    JS_CFUNC_DEF("setProfile", 1, f_set_profile),
    JS_CFUNC_DEF("setCurve", 1, f_set_curve),
    JS_CFUNC_DEF("force", 1, f_force),
    JS_CFUNC_DEF("caseConfig", 0, f_case),
};

void js_fan_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, fan_funcs, sizeof fan_funcs / sizeof fan_funcs[0]);
    JS_SetPropertyStr(c, lord, "fan", o);
}
