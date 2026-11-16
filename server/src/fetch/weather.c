/* weather.c — the forecast's request path and strict parsing (see weather.h). */
#include "weather.h"
#include "../sec/json.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int num_in(const JVal *v, double lo, double hi, double *out) {
    if (!v || v->type != J_NUM || !isfinite(v->num) || v->num < lo || v->num > hi) return -1;
    *out = v->num;
    return 0;
}

static int code_ok(double c) { return c >= 0 && c <= 99 && c == floor(c); }

int wx_path(double lat, double lon, char *out, size_t n) {
    if (!isfinite(lat) || !isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180) return -1;
    int w = snprintf(out, n,
                     "/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,apparent_temperature,weather_code,is_day"
                     "&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=%d",
                     lat, lon, WX_DAYS);
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

/* the i-th element of an array, or NULL */
static const JVal *at(const JVal *arr, size_t i) {
    if (!arr || arr->type != J_ARR || i >= arr->len) return NULL;
    const JVal *v = arr->kid;
    while (v && i--) v = v->next;
    return v;
}

int wx_parse(const char *json, size_t len, WxData *d) {
    memset(d, 0, sizeof *d);
    if (!json || !len || len > WX_ANSWER_MAX) return -1;
    JDoc doc;
    if (json_parse(json, len, &doc, NULL, 0)) return -1;
    int rc = -1;
    const JVal *cur = json_get(doc.root, "current"), *daily = json_get(doc.root, "daily");
    double c, day;
    if (!cur || !daily || cur->type != J_OBJ || daily->type != J_OBJ) goto out;
    if (num_in(json_get(cur, "temperature_2m"), -95, 65, &d->temp) || num_in(json_get(cur, "apparent_temperature"), -110, 80, &d->feels) ||
        num_in(json_get(cur, "weather_code"), 0, 99, &c) || !code_ok(c) || num_in(json_get(cur, "is_day"), 0, 1, &day)) goto out;
    d->code = (int)c;
    d->day = day >= 0.5;
    const JVal *codes = json_get(daily, "weather_code"), *hi = json_get(daily, "temperature_2m_max"), *lo = json_get(daily, "temperature_2m_min");
    if (!codes || !hi || !lo || codes->type != J_ARR || hi->type != J_ARR || lo->type != J_ARR) goto out;
    size_t n = codes->len < hi->len ? codes->len : hi->len;
    if (lo->len < n) n = lo->len;
    if (n > WX_DAYS) n = WX_DAYS;
    if (!n) goto out;
    for (size_t i = 0; i < n; i++) {
        double k;
        if (num_in(at(codes, i), 0, 99, &k) || !code_ok(k) || num_in(at(hi, i), -95, 65, &d->hi[i]) || num_in(at(lo, i), -95, 65, &d->lo[i])) goto out;
        d->codes[i] = (int)k;
    }
    d->ndays = (int)n;
    rc = 0;
out:
    json_free(&doc);
    if (rc) memset(d, 0, sizeof *d);
    return rc;
}

size_t wx_compact(const WxData *d, char *out, size_t n) {
    size_t w = 0;
    int k = snprintf(out, n, "{\"t\":%.1f,\"f\":%.1f,\"c\":%d,\"d\":%d,\"days\":[", d->temp, d->feels, d->code, d->day ? 1 : 0);
    if (k < 0 || (size_t)k >= n) return 0;
    w = (size_t)k;
    for (int i = 0; i < d->ndays && i < WX_DAYS; i++) {
        k = snprintf(out + w, n - w, "%s[%d,%.1f,%.1f]", i ? "," : "", d->codes[i], d->hi[i], d->lo[i]);
        if (k < 0 || (size_t)k >= n - w) return 0;
        w += (size_t)k;
    }
    k = snprintf(out + w, n - w, "]}");
    if (k < 0 || (size_t)k >= n - w) return 0;
    return w + (size_t)k;
}

int wx_from_compact(const char *json, size_t len, WxData *d) {
    memset(d, 0, sizeof *d);
    if (!json || !len || len > 4096) return -1;
    JDoc doc;
    if (json_parse(json, len, &doc, NULL, 0)) return -1;
    int rc = -1;
    double c, day;
    const JVal *days = json_get(doc.root, "days");
    if (num_in(json_get(doc.root, "t"), -95, 65, &d->temp) || num_in(json_get(doc.root, "f"), -110, 80, &d->feels) ||
        num_in(json_get(doc.root, "c"), 0, 99, &c) || !code_ok(c) || num_in(json_get(doc.root, "d"), 0, 1, &day) ||
        !days || days->type != J_ARR || !days->len || days->len > WX_DAYS) goto out;
    d->code = (int)c;
    d->day = day >= 0.5;
    for (size_t i = 0; i < days->len; i++) {
        const JVal *row = at(days, i);
        double k;
        if (!row || row->type != J_ARR || row->len != 3 || num_in(at(row, 0), 0, 99, &k) || !code_ok(k) ||
            num_in(at(row, 1), -95, 65, &d->hi[i]) || num_in(at(row, 2), -95, 65, &d->lo[i])) goto out;
        d->codes[i] = (int)k;
    }
    d->ndays = (int)days->len;
    rc = 0;
out:
    json_free(&doc);
    if (rc) memset(d, 0, sizeof *d);
    return rc;
}
