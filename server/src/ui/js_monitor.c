/* js_monitor.c — lord.monitor: system metrics for the Monitor app (see docs/MONITOR.md).
 *
 * Everything is read-only. Reads of /proc, /sys and sock_diag are done by a dedicated thread
 * (monitor/mon.c) only while someone has called start() and is querying; these functions never
 * touch files (except info() and /proc/<pid>/cgroup of each new process, cached: scope_of), so
 * they don't stall the compositor's frame.
 *
 *   start() / stop()            interested-party count (the thread sleeps after 3 s without queries)
 *   want(processes, connections, countries)  which lists it builds besides the sample (countries: local database)
 *   sample()                    latest sample, or null if there is no new one since the previous
 *   processes(language)         processes, each with its app (if recognized) — or null
 *   connections(language)       the user's connections, with country and app — or null
 *   info()                      CPU model, cores, location by time zone, country database
 *   map(view)                   requests a frame of the vector map (another thread draws it; map.c)
 *   mapFrame()                  the latest ready frame { seq, width, height, lon, lat, ppd, ms, data } or null
 *   detail(pid, start)          Get Info: which process the thread describes once per second (0: none)
 *   detailInfo()                its latest details (procctl.c), or null if nothing new
 *   signal(pid, start, kind)    stops one of the user's own processes: kind 'TERM' or 'KILL'; null or
 *                               an error code (proc.gone, proc.protected…, proc.notOwn, proc.denied).
 *                               Other users' processes go through the root helper (proc-signal,
 *                               administrators only; sdk/monitor.js).
 *   watchDisks(on)              disk temperatures with the Monitor closed (every 30 s; sdk/diskwatch.js)
 *   diskTemps()                 the latest { t, disks: [{ name, model, sensor, temp, tempMax, … }] } or
 *                               null if nothing new; it does not keep the 4 Hz sampling awake
 *   wantVoltage(on)             Monitor → Voltage on screen: the thread reads the voltages about every 2 s
 *   voltage()                   the latest { board, alarm, sinceBoot, chips, supplies } (monitor/voltage.c), or null
 *                               if nothing new; the sample's "volt" says whether this machine has any
 *   historyEnable(on)           Settings → Privacy → Monitor → "Keep a traffic history" (0.8.2)
 *   history({ from, step, n, app })  bytes per bucket from the history file (monitor/nethist.c):
 *                               { res, oldest, now, rx: [n], tx: [n], top: [[app, rx, tx]] } or null;
 *                               from/step in Unix seconds, n ≤ 1500, app an app id or null (the machine)
 *   historyClear()              deletes the history; true if it is gone
 */
#include "js_monitor.h"
#include "../monitor/mon.h"
#include "../monitor/map.h"
#include "../monitor/procctl.h"
#include "../monitor/netrec.h"
#include "../apps.h"
#include "js_sys.h"
#include "js_err.h"

#include <ctype.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t seq_sample, seq_procs, seq_conns, seq_detail, seq_temps, seq_volt;

static JSValue parse(JSContext *c, char *j, const char *what) {
    if (!j) return JS_NULL;
    JSValue v = JS_ParseJSON(c, j, strlen(j), what);
    free(j);
    return v;
}

static JSValue f_start(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    mon_start();
    return JS_TRUE;
}

static JSValue f_stop(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    mon_stop();
    return JS_TRUE;
}

static JSValue f_want(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int p = argc > 0 && JS_ToBool(c, argv[0]) > 0, k = argc > 1 && JS_ToBool(c, argv[1]) > 0, g = argc > 2 && JS_ToBool(c, argv[2]) > 0;
    mon_want(p, k, g);
    return JS_TRUE;
}

static JSValue f_sample(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return parse(c, mon_sample_json(&seq_sample), "<monitor.sample>");
}

static JSValue f_watch_disks(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    mon_watch_disks(argc > 0 && JS_ToBool(c, argv[0]) > 0);
    return JS_TRUE;
}

static JSValue f_disk_temps(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return parse(c, mon_disk_temps_json(&seq_temps), "<monitor.diskTemps>");
}

static JSValue f_want_voltage(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    mon_want_voltage(argc > 0 && JS_ToBool(c, argv[0]) > 0);
    return JS_TRUE;
}

static JSValue f_voltage(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return parse(c, mon_voltage_json(&seq_volt), "<monitor.voltage>");
}

static JSValue f_info(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue o = parse(c, mon_info_json(), "<monitor.info>");
    /* the CPU model comes from the same place as About (on ARM there is no "model name") */
    char cpu[160];
    js_sys_cpu_name(cpu, sizeof cpu);
    if (JS_IsObject(o) && cpu[0]) JS_SetPropertyStr(c, o, "cpuModel", JS_NewString(c, cpu));
    return o;
}

/* Language for app names: "es", "pt-BR"… (letters, digits, '-' and '_', max. 15). */
static int arg_lang(JSContext *c, int argc, JSValueConst *argv, char *out, size_t n) {
    snprintf(out, n, "en");
    if (argc < 1 || !JS_IsString(argv[0])) return 0;
    const char *s = JS_ToCString(c, argv[0]);
    if (!s) return -1;
    size_t len = strlen(s);
    int ok = len > 0 && len < 16;
    for (size_t i = 0; ok && i < len; i++) ok = isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_';
    if (ok) snprintf(out, n, "%s", s);
    JS_FreeCString(c, s);
    return 0;
}

static const char *base(const char *p) { const char *b = strrchr(p, '/'); return b ? b + 1 : p; }

/* App of a process: lord-server itself is LordOS; otherwise, the one from the .desktop whose program
 * matches the executable, argv[0] or the process name (comm, truncated to 15). */
static const DesktopApp *match(const DesktopApp *apps, int n, const char *exe, const char *cmd, const char *name) {
    const char *eb = exe[0] ? base(exe) : "";
    for (int i = 0; i < n; i++) {
        const char *b = apps[i].bin;
        if (!b[0]) continue;
        if ((eb[0] && !strcmp(b, eb)) || (cmd[0] && !strcmp(b, cmd)) || (name[0] && (strlen(name) >= 15 ? !strncmp(b, name, 15) : !strcmp(b, name))))
            return &apps[i];
    }
    for (int i = 0; i < n; i++)                         /* StartupWMClass == name (case-insensitive) */
        if (apps[i].wmclass[0] && name[0] && !strcasecmp(apps[i].wmclass, name)) return &apps[i];
    return NULL;
}

/* The cgroup scope of a process (app-lordos-<id>.scope / app-lordos-lpk-<id>.scope), the same
 * identity the firewall uses in the kernel (docs/FIREWALL.md): this way a curl running inside
 * foot counts as foot. A small /proc read per pid, with a cache of 64 pids. */
static int scope_of(int pid, char *kind, char *id, size_t n) {
    static struct { int pid; char kind; char id[160]; time_t at; } cache[64];
    int slot = pid & 63;
    time_t now = time(NULL);
    /* "no scope" is only true for now: a companion joins its app's scope a moment after it starts */
    if (cache[slot].pid == pid && (cache[slot].kind || now - cache[slot].at < 5)) { *kind = cache[slot].kind; snprintf(id, n, "%s", cache[slot].id); return *kind ? 0 : -1; }
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%d/cgroup", pid);
    FILE *f = fopen(path, "re");
    char k = 0, buf[160] = "";
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "0::", 3)) continue;
            line[strcspn(line, "\n")] = 0;
            const char *leaf = strrchr(line, '/');
            leaf = leaf ? leaf + 1 : line + 3;
            size_t l = strlen(leaf);
            if (l > 17 && !strncmp(leaf, "app-lordos-", 11) && !strcmp(leaf + l - 6, ".scope") && l - 17 < sizeof buf) {
                const char *b = leaf + 11;
                size_t bl = l - 17;
                if (bl > 4 && !strncmp(b, "lpk-", 4)) { k = 'l'; b += 4; bl -= 4; } else k = 'd';
                memcpy(buf, b, bl);
                buf[bl] = 0;
            }
        }
        fclose(f);
    }
    cache[slot].pid = pid;
    cache[slot].kind = k;
    cache[slot].at = now;
    snprintf(cache[slot].id, sizeof cache[slot].id, "%s", buf);
    *kind = k;
    snprintf(id, n, "%s", buf);
    return k ? 0 : -1;
}

static void str_prop(JSContext *c, JSValueConst o, const char *key, char *out, size_t n) {
    JSValue v = JS_GetPropertyStr(c, o, key);
    const char *s = JS_IsString(v) ? JS_ToCString(c, v) : NULL;
    snprintf(out, n, "%s", s ? s : "");
    if (s) JS_FreeCString(c, s);
    JS_FreeValue(c, v);
}

/* Adds app, appName and appIcon to each element of obj.list. */
static JSValue annotate(JSContext *c, JSValue obj, const char *lang) {
    if (!JS_IsObject(obj)) return obj;
    const DesktopApp *apps = NULL;
    int napps = apps_list(lang, &apps);
    JSValue list = JS_GetPropertyStr(c, obj, "list");
    uint32_t len = 0;
    JSValue lv = JS_GetPropertyStr(c, list, "length");
    JS_ToUint32(c, &len, lv);
    JS_FreeValue(c, lv);
    for (uint32_t i = 0; i < len && i < 20000; i++) {
        JSValue e = JS_GetPropertyUint32(c, list, i);
        JSValue self = JS_GetPropertyStr(c, e, "self");
        int is_self = JS_ToBool(c, self) > 0;
        JS_FreeValue(c, self);
        if (is_self) JS_SetPropertyStr(c, e, "app", JS_NewString(c, "lordos"));
        else {
            char exe[4096], cmd[256], name[64];
            str_prop(c, e, "exe", exe, sizeof exe);
            str_prop(c, e, "cmd", cmd, sizeof cmd);
            str_prop(c, e, "name", name, sizeof name);
            const DesktopApp *a = NULL;
            int32_t pid = 0;
            JSValue pv = JS_GetPropertyStr(c, e, "pid");
            JS_ToInt32(c, &pid, pv);
            JS_FreeValue(c, pv);
            char sk, sid[160];
            if (pid > 0 && !scope_of(pid, &sk, sid, sizeof sid)) {
                if (sk == 'l') {
                    char id[200];
                    snprintf(id, sizeof id, "lpk:%s", sid);
                    JS_SetPropertyStr(c, e, "app", JS_NewString(c, id));
                    JS_FreeValue(c, e);
                    continue;
                }
                for (int k = 0; k < napps && !a; k++) if (!strcmp(apps[k].id, sid)) a = &apps[k];
                if (!a) {                               /* the scope wins even if the .desktop is not found */
                    char id[200];
                    snprintf(id, sizeof id, "desktop:%s", sid);
                    JS_SetPropertyStr(c, e, "app", JS_NewString(c, id));
                    JS_FreeValue(c, e);
                    continue;
                }
            }
            if (!a) a = match(apps, napps, exe, cmd, name);
            if (a) {
                char id[200];
                snprintf(id, sizeof id, "desktop:%s", a->id);
                JS_SetPropertyStr(c, e, "app", JS_NewString(c, id));
                JS_SetPropertyStr(c, e, "appName", JS_NewString(c, a->name));
                JS_SetPropertyStr(c, e, "appIcon", JS_NewString(c, a->icon));
            }
        }
        JS_FreeValue(c, e);
    }
    JS_FreeValue(c, list);
    return obj;
}

static JSValue f_processes(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char lang[16];
    if (arg_lang(c, argc, argv, lang, sizeof lang)) return JS_EXCEPTION;
    return annotate(c, parse(c, mon_procs_json(&seq_procs), "<monitor.processes>"), lang);
}

static JSValue f_connections(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char lang[16];
    if (arg_lang(c, argc, argv, lang, sizeof lang)) return JS_EXCEPTION;
    return annotate(c, parse(c, mon_conns_json(&seq_conns), "<monitor.connections>"), lang);
}

/* ---- map ---- */

static int num_prop(JSContext *c, JSValueConst o, const char *k, double lo, double hi, double *out) {
    JSValue v = JS_GetPropertyStr(c, o, k);
    double d = 0;
    int ok = JS_IsNumber(v) && !JS_ToFloat64(c, &d, v) && isfinite(d) && d >= lo && d <= hi;
    JS_FreeValue(c, v);
    if (ok) *out = d;
    return ok ? 0 : -1;
}

static int color_prop(JSContext *c, JSValueConst o, const char *k, uint32_t *out) {
    double d;
    if (num_prop(c, o, k, 0, 4294967295.0, &d)) return -1;
    *out = (uint32_t)d;
    return 0;
}

/* map({ width, height, lon, lat, ppd, land, border, accent, homeColor, selColor,
 *       home: { lon, lat } | null, dots: [{ lon, lat, r, sel, idle }],
 *       arcs?: [{ lon, lat, lift, color, sel, idle }] }) → request number */
static JSValue f_map(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsObject(argv[0])) return js_throw_code(c, "sys.badArgument", NULL);
    JSValueConst o = argv[0];
    static MapReq q;
    memset(&q, 0, sizeof q);
    double w, h;
    if (num_prop(c, o, "width", 1, MAP_MAX_W, &w) || num_prop(c, o, "height", 1, MAP_MAX_H, &h) ||
        num_prop(c, o, "lon", -720, 720, &q.lon0) || num_prop(c, o, "lat", -360, 360, &q.lat0) || num_prop(c, o, "ppd", 0.1, 2000, &q.ppd) ||
        color_prop(c, o, "land", &q.land) || color_prop(c, o, "border", &q.border) || color_prop(c, o, "accent", &q.accent) ||
        color_prop(c, o, "homeColor", &q.home_color) || color_prop(c, o, "selColor", &q.sel_color))
        return js_throw_code(c, "sys.badArgument", NULL);
    q.w = (int)w;
    q.h = (int)h;
    JSValue home = JS_GetPropertyStr(c, o, "home");
    if (JS_IsObject(home)) q.has_home = !num_prop(c, home, "lon", -180, 180, &q.hlon) && !num_prop(c, home, "lat", -90, 90, &q.hlat);
    JS_FreeValue(c, home);
    JSValue dots = JS_GetPropertyStr(c, o, "dots");
    uint32_t n = 0;
    if (JS_IsArray(c, dots)) {
        JSValue lv = JS_GetPropertyStr(c, dots, "length");
        JS_ToUint32(c, &n, lv);
        JS_FreeValue(c, lv);
    }
    for (uint32_t i = 0; i < n && q.ndots < MAP_MAX_DOTS; i++) {
        JSValue d = JS_GetPropertyUint32(c, dots, i);
        double lon, lat, r;
        if (JS_IsObject(d) && !num_prop(c, d, "lon", -180, 180, &lon) && !num_prop(c, d, "lat", -90, 90, &lat) && !num_prop(c, d, "r", 0, 40, &r)) {
            JSValue sv = JS_GetPropertyStr(c, d, "sel");
            MapDot *m = &q.dots[q.ndots++];
            m->lon = (float)lon;
            m->lat = (float)lat;
            m->r = (float)r;
            m->sel = JS_ToBool(c, sv) > 0;
            JS_FreeValue(c, sv);
            JSValue iv = JS_GetPropertyStr(c, d, "idle");
            m->idle = JS_IsBool(iv) && JS_ToBool(c, iv) > 0;
            JS_FreeValue(c, iv);
        }
        JS_FreeValue(c, d);
    }
    JS_FreeValue(c, dots);
    /* arcs: [{ lon, lat, lift, color, sel, idle }] (optional, 0.8.5) */
    JSValue arcs = JS_GetPropertyStr(c, o, "arcs");
    n = 0;
    if (JS_IsArray(c, arcs)) {
        JSValue lv = JS_GetPropertyStr(c, arcs, "length");
        JS_ToUint32(c, &n, lv);
        JS_FreeValue(c, lv);
    }
    for (uint32_t i = 0; i < n && q.narcs < MAP_MAX_ARCS; i++) {
        JSValue d = JS_GetPropertyUint32(c, arcs, i);
        double lon, lat, lift;
        uint32_t col;
        if (JS_IsObject(d) && !num_prop(c, d, "lon", -180, 180, &lon) && !num_prop(c, d, "lat", -90, 90, &lat) &&
            !num_prop(c, d, "lift", 0, 1, &lift) && !color_prop(c, d, "color", &col)) {
            MapArc *m = &q.arcs[q.narcs++];
            m->lon = (float)lon;
            m->lat = (float)lat;
            m->lift = (float)lift;
            m->color = col;
            JSValue sv = JS_GetPropertyStr(c, d, "sel"), iv = JS_GetPropertyStr(c, d, "idle");
            m->sel = JS_ToBool(c, sv) > 0;
            m->idle = JS_IsBool(iv) && JS_ToBool(c, iv) > 0;
            JS_FreeValue(c, sv);
            JS_FreeValue(c, iv);
        }
        JS_FreeValue(c, d);
    }
    JS_FreeValue(c, arcs);
    return JS_NewFloat64(c, (double)map_request(&q));
}

static uint64_t seq_map;

static JSValue f_map_frame(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    uint8_t *rgba = NULL;
    static MapReq v;
    double ms = 0;
    if (!map_take(&seq_map, &rgba, &v, &ms)) return JS_NULL;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "seq", JS_NewFloat64(c, (double)seq_map));
    JS_SetPropertyStr(c, o, "width", JS_NewInt32(c, v.w));
    JS_SetPropertyStr(c, o, "height", JS_NewInt32(c, v.h));
    JS_SetPropertyStr(c, o, "lon", JS_NewFloat64(c, v.lon0));
    JS_SetPropertyStr(c, o, "lat", JS_NewFloat64(c, v.lat0));
    JS_SetPropertyStr(c, o, "ppd", JS_NewFloat64(c, v.ppd));
    JS_SetPropertyStr(c, o, "ms", JS_NewFloat64(c, ms));
    JS_SetPropertyStr(c, o, "data", JS_NewArrayBufferCopy(c, rgba, (size_t)v.w * v.h * 4));
    free(rgba);
    return o;
}

/* ---- one process: Get Info and stop ---- */

/* pid (1..PID_MAX) and start time (an integer number of ticks, 0..2^53) from argv[0..1]. */
static int arg_proc(JSContext *c, int argc, JSValueConst *argv, int *pid, unsigned long long *start) {
    double p, st;
    if (argc < 2 || !JS_IsNumber(argv[0]) || !JS_IsNumber(argv[1]) || JS_ToFloat64(c, &p, argv[0]) || JS_ToFloat64(c, &st, argv[1])) return -1;
    if (!isfinite(p) || p != floor(p) || p < 0 || p > PROC_PID_MAX) return -1;
    if (!isfinite(st) || st != floor(st) || st < 0 || st > 9007199254740992.0) return -1;
    *pid = (int)p;
    *start = (unsigned long long)st;
    return 0;
}

static JSValue f_detail(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int pid;
    unsigned long long start;
    if (arg_proc(c, argc, argv, &pid, &start)) return js_throw_code(c, "sys.badArgument", NULL);
    mon_detail(pid, start);
    seq_detail = 0;
    return JS_TRUE;
}

static JSValue f_detail_info(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return parse(c, mon_detail_json(&seq_detail), "<monitor.detail>");
}

static JSValue f_signal(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int pid;
    unsigned long long start;
    if (arg_proc(c, argc, argv, &pid, &start) || pid < 1 || argc < 3 || !JS_IsString(argv[2])) return js_throw_code(c, "sys.badArgument", NULL);
    const char *k = JS_ToCString(c, argv[2]);
    if (!k) return JS_EXCEPTION;
    int sig = !strcmp(k, "TERM") ? SIGTERM : !strcmp(k, "KILL") ? SIGKILL : 0;
    JS_FreeCString(c, k);
    if (!sig) return js_throw_code(c, "sys.badArgument", NULL);
    /* only the user's own processes here; the rest goes through the root helper (administrators) */
    char p[64];
    struct stat sb;
    snprintf(p, sizeof p, "/proc/%d", pid);
    if (stat(p, &sb)) return JS_NewString(c, "proc.gone");
    if (sb.st_uid != getuid()) return JS_NewString(c, "proc.notOwn");
    const char *e = proc_signal(pid, start, sig, (int)getpid());
    return e ? JS_NewString(c, e) : JS_NULL;
}

/* ---- traffic history (0.8.2) ---- */

static JSValue f_history_enable(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    netrec_enable(argc > 0 && JS_ToBool(c, argv[0]) > 0);
    return JS_NewBool(c, netrec_enabled());
}

static int int_prop(JSContext *c, JSValueConst o, const char *k, double lo, double hi, long long *out) {
    JSValue v = JS_GetPropertyStr(c, o, k);
    double d;
    int bad = !JS_IsNumber(v) || JS_ToFloat64(c, &d, v) || !isfinite(d) || d != floor(d) || d < lo || d > hi;
    JS_FreeValue(c, v);
    if (!bad) *out = (long long)d;
    return bad ? -1 : 0;
}

static JSValue f_history(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    NetHist *h = netrec_hist();
    if (!h || argc < 1 || !JS_IsObject(argv[0])) return JS_NULL;
    long long from, step, n;
    if (int_prop(c, argv[0], "from", 0, 4102444800.0, &from) || int_prop(c, argv[0], "step", 60, 86400.0 * 31, &step) ||
        int_prop(c, argv[0], "n", 1, NH_BUCKETS_MAX, &n)) return JS_NULL;
    char app[NH_ID_MAX] = "";
    JSValue a = JS_GetPropertyStr(c, argv[0], "app");
    if (JS_IsString(a)) {
        const char *s = JS_ToCString(c, a);
        int ok = s && strlen(s) > 0 && strlen(s) < NH_ID_MAX;
        for (const char *q = s; ok && *q; q++) ok = isalnum((unsigned char)*q) || strchr("._:@+~/-", *q);
        if (ok) snprintf(app, sizeof app, "%s", s);
        if (s) JS_FreeCString(c, s);
        if (!ok) { JS_FreeValue(c, a); return JS_NULL; }
    } else if (!JS_IsUndefined(a) && !JS_IsNull(a)) { JS_FreeValue(c, a); return JS_NULL; }
    JS_FreeValue(c, a);
    double *rx = malloc(sizeof(double) * (size_t)n), *tx = malloc(sizeof(double) * (size_t)n);
    NhTop top[12];
    int ntop = 0;
    long long oldest = 0, now = netrec_now();
    int res = rx && tx ? nh_query(h, now, from, step, (int)n, app[0] ? app : NULL, rx, tx, top, 12, &ntop, &oldest) : -1;
    if (res < 0) { free(rx); free(tx); return JS_NULL; }
    JSValue o = JS_NewObject(c), arx = JS_NewArray(c), atx = JS_NewArray(c), atop = JS_NewArray(c);
    for (int i = 0; i < n; i++) {
        JS_SetPropertyUint32(c, arx, (uint32_t)i, JS_NewFloat64(c, round(rx[i])));
        JS_SetPropertyUint32(c, atx, (uint32_t)i, JS_NewFloat64(c, round(tx[i])));
    }
    for (int i = 0; i < ntop; i++) {
        JSValue e = JS_NewArray(c);
        JS_SetPropertyUint32(c, e, 0, JS_NewString(c, top[i].id));
        JS_SetPropertyUint32(c, e, 1, JS_NewFloat64(c, (double)top[i].rx));
        JS_SetPropertyUint32(c, e, 2, JS_NewFloat64(c, (double)top[i].tx));
        JS_SetPropertyUint32(c, atop, (uint32_t)i, e);
    }
    JS_SetPropertyStr(c, o, "res", JS_NewInt32(c, res));
    JS_SetPropertyStr(c, o, "oldest", JS_NewFloat64(c, (double)oldest));
    JS_SetPropertyStr(c, o, "now", JS_NewFloat64(c, (double)now));
    JS_SetPropertyStr(c, o, "enabled", JS_NewBool(c, netrec_enabled()));
    JS_SetPropertyStr(c, o, "rx", arx);
    JS_SetPropertyStr(c, o, "tx", atx);
    JS_SetPropertyStr(c, o, "top", atop);
    free(rx);
    free(tx);
    return o;
}

static JSValue f_history_clear(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    NetHist *h = netrec_hist();
    return JS_NewBool(c, h && nh_clear(h) == 0);
}

/* (0.8.5) netTotals() → { t, rx, tx }: the machine's byte counters now (the menu bar's network item),
 * without starting the sampling thread. */
static JSValue f_net_totals(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    uint64_t rx, tx;
    if (netrec_totals(&rx, &tx)) return JS_NULL;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "t", JS_NewFloat64(c, ts.tv_sec * 1e3 + ts.tv_nsec / 1e6));
    JS_SetPropertyStr(c, o, "rx", JS_NewFloat64(c, (double)rx));
    JS_SetPropertyStr(c, o, "tx", JS_NewFloat64(c, (double)tx));
    return o;
}

static const JSCFunctionListEntry monitor_funcs[] = {
    JS_CFUNC_DEF("netTotals", 0, f_net_totals),
    JS_CFUNC_DEF("start", 0, f_start),
    JS_CFUNC_DEF("stop", 0, f_stop),
    JS_CFUNC_DEF("want", 3, f_want),
    JS_CFUNC_DEF("sample", 0, f_sample),
    JS_CFUNC_DEF("processes", 1, f_processes),
    JS_CFUNC_DEF("connections", 1, f_connections),
    JS_CFUNC_DEF("info", 0, f_info),
    JS_CFUNC_DEF("map", 1, f_map),
    JS_CFUNC_DEF("mapFrame", 0, f_map_frame),
    JS_CFUNC_DEF("detail", 2, f_detail),
    JS_CFUNC_DEF("detailInfo", 0, f_detail_info),
    JS_CFUNC_DEF("signal", 3, f_signal),
    JS_CFUNC_DEF("watchDisks", 1, f_watch_disks),
    JS_CFUNC_DEF("diskTemps", 0, f_disk_temps),
    JS_CFUNC_DEF("wantVoltage", 1, f_want_voltage),
    JS_CFUNC_DEF("voltage", 0, f_voltage),
    JS_CFUNC_DEF("historyEnable", 1, f_history_enable),
    JS_CFUNC_DEF("history", 1, f_history),
    JS_CFUNC_DEF("historyClear", 0, f_history_clear),
};

void js_monitor_register(JSContext *c, JSValue lord, const char *assets_dir) {
    mon_init(assets_dir);
    map_init(assets_dir);
    mon_reset();                       /* new context: the previous one's start() calls don't count */
    seq_sample = seq_procs = seq_conns = seq_detail = seq_temps = seq_volt = 0;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, monitor_funcs, sizeof monitor_funcs / sizeof monitor_funcs[0]);
    JS_SetPropertyStr(c, lord, "monitor", o);
}
