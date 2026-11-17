/* js_weather.c — lord.weather (0.8.8.10, the weather widget; fetch/wxsvc.h).
 *
 *   lord.weather.request(zone) → true | false   asks for a time zone's forecast (tzdata's coordinates for it);
 *                                               false for an unknown zone; a fresh one isn't asked again
 *   lord.weather.cities(query) → [{ id, name, cc, adm1 }]   the offline list (assets/geo/cities.tsv): names that
 *                                               start with, then contain, the folded query; nothing asked of the network
 *   zone may be "geonames/<id>", a city from that list
 *   lord.weather.get(zone) → { t, f, c, d, days: [[code, hi, lo], …], at } | { error } | null
 *                                               t/f/hi/lo in °C, c the WMO code, d 1 by day; at: seconds
 *
 * Registered only in the desktop's own JS context (js.c), never for an isolated app. The desktop asks only
 * while the person has turned the weather on (settings.weather.on); see docs/WORKSPACES.md, "Weather". */
#include "js_weather.h"
#include "../fetch/wxsvc.h"
#include "../fetch/geocities.h"
#include "js_sys.h"
#include <stdio.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int ready;

static void init(void) {
    if (ready) return;
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return;
    self[n] = 0;
    char *sl = strrchr(self, '/');
    if (!sl) return;
    *sl = 0;
    if (!wxs_init(self)) ready = 1;
    char cp[PATH_MAX];
    snprintf(cp, sizeof cp, "%s/geo/cities.tsv", js_sys_assets());
    wxs_set_cities(cp);
}

static JSValue f_cities(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    init();
    const char *q = argc > 0 ? JS_ToCString(c, argv[0]) : NULL;
    char cp[PATH_MAX], out[8192];
    snprintf(cp, sizeof cp, "%s/geo/cities.tsv", js_sys_assets());
    size_t n = geo_search_json(cp, q ? q : "", 20, out, sizeof out);
    if (q) JS_FreeCString(c, q);
    return JS_ParseJSON(c, out, n, "<cities>");
}

static JSValue f_request(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    init();
    const char *z = argc > 0 ? JS_ToCString(c, argv[0]) : NULL;
    int ok = z && ready && wxs_request(z) == 0;
    if (z) JS_FreeCString(c, z);
    return JS_NewBool(c, ok);
}

static JSValue f_get(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *z = argc > 0 ? JS_ToCString(c, argv[0]) : NULL;
    if (!z) return JS_NULL;
    WxData d;
    long long at = 0;
    char err[24] = "";
    int rc = wxs_get(z, &d, &at, err, sizeof err);
    JS_FreeCString(c, z);
    if (rc < 0) return JS_NULL;
    JSValue o = JS_NewObject(c);
    if (rc == 0) {
        if (err[0]) JS_SetPropertyStr(c, o, "error", JS_NewString(c, err));
        return o;
    }
    JS_SetPropertyStr(c, o, "t", JS_NewFloat64(c, d.temp));
    JS_SetPropertyStr(c, o, "f", JS_NewFloat64(c, d.feels));
    JS_SetPropertyStr(c, o, "c", JS_NewInt32(c, d.code));
    JS_SetPropertyStr(c, o, "d", JS_NewInt32(c, d.day));
    JS_SetPropertyStr(c, o, "at", JS_NewFloat64(c, (double)at));
    JSValue days = JS_NewArray(c);
    for (int i = 0; i < d.ndays; i++) {
        JSValue row = JS_NewArray(c);
        JS_SetPropertyUint32(c, row, 0, JS_NewInt32(c, d.codes[i]));
        JS_SetPropertyUint32(c, row, 1, JS_NewFloat64(c, d.hi[i]));
        JS_SetPropertyUint32(c, row, 2, JS_NewFloat64(c, d.lo[i]));
        JS_SetPropertyUint32(c, days, (uint32_t)i, row);
    }
    JS_SetPropertyStr(c, o, "days", days);
    if (err[0]) JS_SetPropertyStr(c, o, "error", JS_NewString(c, err));
    return o;
}

static const JSCFunctionListEntry weather_funcs[] = {
    JS_CFUNC_DEF("request", 1, f_request),
    JS_CFUNC_DEF("get", 1, f_get),
    JS_CFUNC_DEF("cities", 1, f_cities),
};

void js_weather_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, weather_funcs, sizeof weather_funcs / sizeof weather_funcs[0]);
    JS_SetPropertyStr(c, lord, "weather", o);
}
