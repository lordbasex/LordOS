/* fancurve.c — profiles, curve evaluation, fan.json and the socket requests (see fancurve.h). */
#include "fan/fancurve.h"
#include "sec/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const NAMES[FP__COUNT] = { "auto", "quiet", "balanced", "performance", "always", "custom" };

/* Presets. Balanced is the curve most cases ship with: 55 °C → 30 %, 60 °C → 55 %, 65 °C → 100 %. */
static const FanCurve PRESET[FP__COUNT] = {
    [FP_QUIET]       = { 3, { { 60, 30 }, { 67, 60 }, { 72, 100 } }, 0 },
    [FP_BALANCED]    = { 3, { { 55, 30 }, { 60, 55 }, { 65, 100 } }, 0 },
    [FP_PERFORMANCE] = { 3, { { 45, 40 }, { 55, 70 }, { 60, 100 } }, 0 },
    [FP_ALWAYS]      = { 2, { { 55, 60 }, { 65, 100 } }, 40 },
};

const char *fan_profile_name(FanProfile p) { return (unsigned)p < FP__COUNT ? NAMES[p] : "auto"; }

int fan_profile_parse(const char *s, FanProfile *out) {
    for (int i = 0; i < FP__COUNT; i++)
        if (!strcmp(s, NAMES[i])) { *out = (FanProfile)i; return 0; }
    return -1;
}

void fan_profile_curve(const FanConfig *cfg, FanCurve *out) {
    memset(out, 0, sizeof *out);
    if (cfg->profile == FP_CUSTOM) *out = cfg->custom;
    else if (cfg->profile != FP_AUTO && (unsigned)cfg->profile < FP__COUNT) *out = PRESET[cfg->profile];
}

int fan_curve_valid(const FanCurve *c) {
    if (c->n < 1 || c->n > FAN_MAX_POINTS) return -1;
    if (c->floor_pct < 0 || c->floor_pct > 100) return -1;
    for (int i = 0; i < c->n; i++) {
        const FanPoint *p = &c->p[i];
        if (p->temp < FAN_TEMP_MIN || p->temp > FAN_TEMP_MAX || p->pct < 0 || p->pct > 100) return -1;
        if (i && (p->temp <= c->p[i - 1].temp || p->pct < c->p[i - 1].pct)) return -1;
    }
    return 0;
}

int fan_curve_step(const FanCurve *c, int hyst, int prev, int temp_mc) {
    if (c->n <= 0) return -1;
    int s = prev < -1 ? -1 : prev >= c->n ? c->n - 1 : prev;
    /* leave a step only hyst °C below it ... */
    while (s >= 0 && temp_mc < (c->p[s].temp - hyst) * 1000) s--;
    /* ... and enter every step the temperature has reached */
    while (s + 1 < c->n && temp_mc >= c->p[s + 1].temp * 1000) s++;
    return s;
}

int fan_curve_pct(const FanCurve *c, int step) {
    int f = c->floor_pct > 0 ? c->floor_pct : 0;
    if (step < 0 || step >= c->n) return f;
    return c->p[step].pct > f ? c->p[step].pct : f;
}

int fan_output_pct(int pct) {
    if (pct <= 0) return 0;
    if (pct >= 100) return 100;
    return pct < FAN_MIN_PCT ? FAN_MIN_PCT : pct;
}

int fan_gpu_pct(int pct) { return pct < FAN_GPU_FLOOR ? FAN_GPU_FLOOR : pct > 100 ? 100 : pct; }

void fan_config_default(FanConfig *cfg, int has_board_fan) {
    memset(cfg, 0, sizeof *cfg);
    /* a board fan (Raspberry Pi fan header) gets our curve from the first boot; a PC's
     * motherboard fans stay with the firmware until the user picks a profile */
    cfg->profile = has_board_fan ? FP_BALANCED : FP_AUTO;
    cfg->custom = PRESET[FP_BALANCED];
    cfg->hyst = FAN_HYST_DEFAULT;
    cfg->force = FF_OFF;
}

static void seterr(char *err, size_t n, const char *m) { if (err && n) snprintf(err, n, "%s", m); }

static int is_int(double v, int lo, int hi) { return v >= lo && v <= hi && v == (double)(int)v; }

int fan_config_parse(const char *text, size_t len, FanConfig *cfg, char *err, size_t errlen) {
    /* the caller fills cfg with fan_config_default(); missing keys keep those values */
    FanConfig c = *cfg, fallback = *cfg;
    JDoc d;
    if (json_parse(text, len, &d, err, errlen)) { *cfg = fallback; return -1; }
    const JVal *o = d.root;
    int bad = 0;
    if (!o || o->type != J_OBJ) { seterr(err, errlen, "not an object"); bad = 1; }
    for (const JVal *k = o && !bad ? o->kid : NULL; k && !bad; k = k->next) {
        if (!strcmp(k->key, "version")) {
            if (k->type != J_NUM || k->num != 1) { seterr(err, errlen, "version"); bad = 1; }
        } else if (!strcmp(k->key, "profile")) {
            if (k->type != J_STR || fan_profile_parse(k->str, &c.profile)) { seterr(err, errlen, "profile"); bad = 1; }
        } else if (!strcmp(k->key, "hyst")) {
            if (k->type != J_NUM || !is_int(k->num, FAN_HYST_MIN, FAN_HYST_MAX)) { seterr(err, errlen, "hyst"); bad = 1; }
            else c.hyst = (int)k->num;
        } else if (!strcmp(k->key, "custom")) {
            if (k->type != J_ARR || k->len < 1 || k->len > FAN_MAX_POINTS) { seterr(err, errlen, "custom"); bad = 1; break; }
            FanCurve cu = { 0 };
            for (const JVal *pt = k->kid; pt; pt = pt->next) {
                const JVal *a = pt->kid, *b = a ? a->next : NULL;
                if (pt->type != J_ARR || pt->len != 2 || a->type != J_NUM || b->type != J_NUM ||
                    !is_int(a->num, FAN_TEMP_MIN, FAN_TEMP_MAX) || !is_int(b->num, 0, 100)) { bad = 1; break; }
                cu.p[cu.n].temp = (int)a->num;
                cu.p[cu.n].pct = (int)b->num;
                cu.n++;
            }
            if (bad || fan_curve_valid(&cu)) { seterr(err, errlen, "custom"); bad = 1; }
            else c.custom = cu;
        } else if (!strcmp(k->key, "force")) {
            if (k->type != J_STR) { seterr(err, errlen, "force"); bad = 1; }
            else if (!strcmp(k->str, "off")) c.force = FF_OFF;
            else if (!strcmp(k->str, "on")) c.force = FF_ON;
            else if (!strcmp(k->str, "timed")) c.force = FF_TIMED;
            else { seterr(err, errlen, "force"); bad = 1; }
        } else if (!strcmp(k->key, "forceUntil")) {
            if (k->type != J_NUM || k->num < 0 || k->num > 4102444800.0 || k->num != (double)(long long)k->num) { seterr(err, errlen, "forceUntil"); bad = 1; }
            else c.force_until = (long long)k->num;
        } else { seterr(err, errlen, "unknown key"); bad = 1; }
    }
    json_free(&d);
    if (!bad && c.force == FF_TIMED && c.force_until <= 0) { seterr(err, errlen, "forceUntil"); bad = 1; }
    if (bad) { *cfg = fallback; return -1; }
    if (c.force != FF_TIMED) c.force_until = 0;
    *cfg = c;
    return 0;
}

size_t fan_config_write(const FanConfig *cfg, char *out, size_t n) {
    char pts[FAN_MAX_POINTS * 12 + 4];
    size_t k = 0;
    pts[0] = 0;
    for (int i = 0; i < cfg->custom.n && k < sizeof pts; i++)
        k += (size_t)snprintf(pts + k, sizeof pts - k, "%s[%d,%d]", i ? "," : "", cfg->custom.p[i].temp, cfg->custom.p[i].pct);
    int r = snprintf(out, n, "{\"version\":1,\"profile\":\"%s\",\"hyst\":%d,\"custom\":[%s],\"force\":\"%s\",\"forceUntil\":%lld}\n",
                     fan_profile_name(cfg->profile), cfg->hyst, pts,
                     cfg->force == FF_ON ? "on" : cfg->force == FF_TIMED ? "timed" : "off",
                     cfg->force == FF_TIMED ? cfg->force_until : 0LL);
    return r < 0 ? 0 : (size_t)r;
}

/* A decimal integer of 1..3 digits at *s; advances *s. */
static int take_int(const char **s, int *out) {
    const char *p = *s;
    int v = 0, d = 0;
    while (*p >= '0' && *p <= '9') { if (++d > 4) return -1; v = v * 10 + (*p++ - '0'); }
    if (!d) return -1;
    *s = p;
    *out = v;
    return 0;
}

int fan_curve_parse_text(const char *s, FanCurve *c) {
    FanCurve cu = { 0 };
    for (;;) {
        if (cu.n >= FAN_MAX_POINTS) return -1;
        int t, p;
        if (take_int(&s, &t) || *s++ != ':' || take_int(&s, &p)) return -1;
        cu.p[cu.n].temp = t;
        cu.p[cu.n].pct = p;
        cu.n++;
        if (!*s) break;
        if (*s++ != ',') return -1;
    }
    if (fan_curve_valid(&cu)) return -1;
    *c = cu;
    return 0;
}

int fan_req_parse(const char *line, size_t len, FanReq *r) {
    if (len == 0 || len >= FAN_REQ_MAX || strlen(line) != len) return -1;
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)line[i] < 0x20 || (unsigned char)line[i] > 0x7e) return -1;
    memset(r, 0, sizeof *r);
    if (!strcmp(line, "STATUS")) { r->type = FR_STATUS; return 0; }
    if (!strncmp(line, "PROFILE ", 8)) {
        r->type = FR_PROFILE;
        return fan_profile_parse(line + 8, &r->profile);
    }
    if (!strncmp(line, "CURVE ", 6)) {
        r->type = FR_CURVE;
        return fan_curve_parse_text(line + 6, &r->curve);
    }
    if (!strncmp(line, "GPUFAN ", 7)) {
        const char *a = line + 7;
        int g, p;
        r->type = FR_GPUFAN;
        if (take_int(&a, &g) || g < 0 || g > 3 || *a != ' ') return -1;
        a++;
        if (!strcmp(a, "auto")) { r->gpu = g; r->pct = -1; return 0; }
        if (take_int(&a, &p) || *a || p < 0 || p > 100) return -1;
        r->gpu = g;
        r->pct = p;
        return 0;
    }
    if (!strncmp(line, "FORCE ", 6)) {
        const char *a = line + 6;
        r->type = FR_FORCE;
        if (!strcmp(a, "off")) { r->force = FF_OFF; return 0; }
        if (!strcmp(a, "on")) { r->force = FF_ON; return 0; }
        int m;
        if (take_int(&a, &m) || *a || m < 1 || m > FAN_FORCE_MAX_MIN) return -1;
        r->force = FF_TIMED;
        r->minutes = m;
        return 0;
    }
    return -1;
}
