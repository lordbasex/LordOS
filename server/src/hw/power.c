/* power.c — /sys/class/power_supply reader (see power.h). */
#include "power.h"
#include "../sec/json.h"

#include <dirent.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* One attribute file, trimmed. 0 if read. */
static int attr(const char *root, const char *dev, const char *name, char *out, size_t n) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s/%s", root, dev, name);
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r < 0) return -1;
    out[r] = 0;
    while (r > 0 && (out[r - 1] == '\n' || out[r - 1] == ' ')) out[--r] = 0;
    return 0;
}

static long long num(const char *root, const char *dev, const char *name) {
    char b[32];
    if (attr(root, dev, name, b, sizeof b) || !b[0]) return -1;
    char *e;
    long long v = strtoll(b, &e, 10);
    return *e ? -1 : v;
}

static void read_one(const char *root, const char *dev, PowerSupply *s) {
    char type[32] = "", scope[32] = "";
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", dev);
    attr(root, dev, "type", type, sizeof type);
    attr(root, dev, "scope", scope, sizeof scope);
    s->battery = !strcmp(type, "Battery");
    s->device_scope = !strcmp(scope, "Device");
    long long online = num(root, dev, "online"), present = num(root, dev, "present");
    s->online = online > 0;
    s->present = present != 0;               /* missing file = present */
    s->percent = -1;
    s->minutes_left = s->health = s->cycles = -1;
    s->watts = -1;
    if (!s->battery) return;
    if (attr(root, dev, "status", s->status, sizeof s->status)) snprintf(s->status, sizeof s->status, "Unknown");
    attr(root, dev, "capacity_level", s->level, sizeof s->level);
    if (!strcmp(s->level, "Unknown")) s->level[0] = 0;
    attr(root, dev, "model_name", s->model, sizeof s->model);
    attr(root, dev, "manufacturer", s->vendor, sizeof s->vendor);
    attr(root, dev, "technology", s->technology, sizeof s->technology);
    long long cap = num(root, dev, "capacity");
    /* energy_* in µWh with power_now in µW, or charge_* in µAh with current_now in µA */
    long long now = num(root, dev, "energy_now"), full = num(root, dev, "energy_full"), design = num(root, dev, "energy_full_design");
    long long rate = num(root, dev, "power_now");
    if (now < 0) {
        now = num(root, dev, "charge_now");
        full = num(root, dev, "charge_full");
        design = num(root, dev, "charge_full_design");
        rate = num(root, dev, "current_now");
        long long uv = num(root, dev, "voltage_now");
        if (rate >= 0 && uv > 0) s->watts = (double)rate * (double)uv / 1e12;
    } else if (rate >= 0) s->watts = (double)rate / 1e6;
    if (cap >= 0 && cap <= 100) s->percent = (int)cap;
    else if (now >= 0 && full > 0) s->percent = (int)(now * 100 / full);
    if (s->percent > 100) s->percent = 100;
    if (full > 0 && design > 0) s->health = (int)(full * 100 / design);
    if (s->health > 100) s->health = 100;
    s->cycles = (int)num(root, dev, "cycle_count");
    if (s->cycles == 0) s->cycles = -1;      /* many firmwares report 0 for "unknown" */
    long long tte = num(root, dev, "time_to_empty_now"), ttf = num(root, dev, "time_to_full_now");
    if (!strcmp(s->status, "Discharging")) {
        if (tte > 0) s->minutes_left = (int)(tte / 60);
        else if (rate > 0 && now >= 0) s->minutes_left = (int)(now * 60 / rate);
    } else if (!strcmp(s->status, "Charging")) {
        if (ttf > 0) s->minutes_left = (int)(ttf / 60);
        else if (rate > 0 && full > now && now >= 0) s->minutes_left = (int)((full - now) * 60 / rate);
    }
}

static int by_name(const void *a, const void *b) { return strcmp(((const PowerSupply *)a)->name, ((const PowerSupply *)b)->name); }

int power_read(const char *root, PowerState *ps) {
    memset(ps, 0, sizeof *ps);
    ps->percent = ps->minutes_left = -1;
    DIR *d = opendir(root);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && ps->n < POWER_MAX) {
            if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof ps->s[0].name) continue;
            read_one(root, e->d_name, &ps->s[ps->n]);
            ps->n++;
        }
        closedir(d);
    }
    qsort(ps->s, (size_t)ps->n, sizeof ps->s[0], by_name);
    long long sum = 0, cnt = 0, mins = 0, have_mins = 0;
    int any_charger = 0;
    for (int i = 0; i < ps->n; i++) {
        const PowerSupply *s = &ps->s[i];
        if (!s->battery) { any_charger = 1; if (s->online) ps->on_ac = 1; continue; }
        if (s->device_scope || !s->present) continue;
        ps->has_battery = 1;
        if (s->percent >= 0) { sum += s->percent; cnt++; }
        if (s->minutes_left >= 0) { mins += s->minutes_left; have_mins = 1; }
        if (!strcmp(s->status, "Charging")) ps->charging = 1;
    }
    if (!ps->has_battery) ps->on_ac = 1;
    else if (!any_charger) ps->on_ac = ps->charging;
    if (cnt) ps->percent = (int)(sum / cnt);
    if (have_mins) ps->minutes_left = (int)mins;
    return ps->n;
}

typedef struct { char *p; size_t n, cap; } Out;
static void w(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void w(Out *o, const char *fmt, ...) {
    if (o->n + 1 >= o->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(o->p + o->n, o->cap - o->n, fmt, ap);
    va_end(ap);
    if (r > 0) o->n += (size_t)r < o->cap - o->n ? (size_t)r : o->cap - o->n - 1;
}
static void wq(Out *o, const char *s) {
    if (o->n + 1 >= o->cap) return;
    o->n += json_quote(o->p + o->n, o->cap - o->n, s);
    if (o->n >= o->cap) o->n = o->cap - 1;
}

size_t power_json(const PowerState *ps, char *out, size_t n) {
    Out o = { out, 0, n };
    if (!n) return 0;
    out[0] = 0;
    w(&o, "{\"onAC\":%s,\"hasBattery\":%s,\"percent\":%d,\"minutesLeft\":%d,\"charging\":%s,\"supplies\":[",
      ps->on_ac ? "true" : "false", ps->has_battery ? "true" : "false", ps->percent, ps->minutes_left, ps->charging ? "true" : "false");
    for (int i = 0; i < ps->n; i++) {
        const PowerSupply *s = &ps->s[i];
        w(&o, "%s{\"name\":", i ? "," : "");
        wq(&o, s->name);
        w(&o, ",\"battery\":%s,\"device\":%s,\"online\":%s,\"present\":%s,\"percent\":%d,\"status\":",
          s->battery ? "true" : "false", s->device_scope ? "true" : "false", s->online ? "true" : "false", s->present ? "true" : "false", s->percent);
        wq(&o, s->status);
        w(&o, ",\"level\":");
        wq(&o, s->level);
        w(&o, ",\"minutesLeft\":%d,\"health\":%d,\"cycles\":%d,\"watts\":%.2f,\"model\":", s->minutes_left, s->health, s->cycles, s->watts);
        wq(&o, s->model);
        w(&o, ",\"vendor\":");
        wq(&o, s->vendor);
        w(&o, ",\"technology\":");
        wq(&o, s->technology);
        w(&o, "}");
    }
    w(&o, "]}");
    return o.n;
}
