/* hw.c — main-loop glue for Settings' hardware modules (see hw.h). */
#include "hw.h"
#include "bt.h"
#include "jbuf.h"
#include "mpris.h"
#include "notifyd.h"
#include "../sec/pwguard.h"
#include "ppd.h"
#include "sysbus.h"
#include "pkagent.h"
#include "sleeplock.h"
#include "uevent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const DisplayHost *host;
static PowerState power;

void hw_emit(const char *kind, const char *json) { if (host && host->event) host->event(kind, json); }
const PowerState *hw_power(void) { return &power; }

/* Many batteries send a uevent only when they start or stop charging, not as the charge goes down, so
 * the percentage is also read again every POWER_POLL_MS while there is a battery, and sent only when it
 * changed (issue #25: the menu bar stayed at 81 % while the battery said 74 %, and the low-battery
 * notices never came). */
#define POWER_POLL_MS 30000.0
static double power_next;               /* CLOCK_MONOTONIC ms of the next read, 0 = none */
static char power_last[4096];

static double hw_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void power_changed(void) {
    power_read("/sys/class/power_supply", &power);
    char j[4096];
    power_json(&power, j, sizeof j);
    power_next = power.has_battery ? hw_now_ms() + POWER_POLL_MS : 0;
    if (!strcmp(j, power_last)) return;
    snprintf(power_last, sizeof power_last, "%s", j);
    hw_emit("power", j);
}

double hw_timeout(double now) {
    double best = sleeplock_timeout(now);
    if (power_next <= 0) return best;
    double p = power_next > now ? power_next - now : 0;
    return best < 0 || p < best ? p : best;
}

static void td_signal(const DMsg *m) {
    if (m->path && !strcmp(m->path, "/org/freedesktop/timedate1") && m->member && !strcmp(m->member, "PropertiesChanged")) hw_timedate_refresh();
}

void hw_start(const DisplayHost *h, int with_bus) {
    host = h;
    display_init(h);
    power_read("/sys/class/power_supply", &power);
    power_json(&power, power_last, sizeof power_last);
    if (power.has_battery) power_next = hw_now_ms() + POWER_POLL_MS;
    if (uevent_open()) fprintf(stderr, "hw: no kernel uevents (hotplugged displays and batteries are not followed)\n");
    if (with_bus && notifyd_start(hw_emit)) fprintf(stderr, "hw: no notification server for Linux apps\n");
    if (with_bus && mpris_start(hw_emit)) fprintf(stderr, "hw: no session bus for the Linux players (Now Playing)\n");
    if (with_bus && pwguard_start()) fprintf(stderr, "hw: PipeWire not reachable yet (the microphone and camera restrictions of Linux apps apply once it is)\n");
    if (with_bus && sysbus_start() == 0) {
        bt_start(hw_emit);
        ppd_start(hw_emit);
        pkagent_start(hw_emit);
        sysbus_on_signal(td_signal);
        sysbus_add_match("type='signal',sender='org.freedesktop.timedate1',interface='org.freedesktop.DBus.Properties',path='/org/freedesktop/timedate1'");
        /* a session lord-sessiond shows can be locked: it locks before the computer sleeps (#36) */
        if (getenv("LORDOS_SESSIOND")) sleeplock_start();
    }
}

int hw_pollfds(struct pollfd *p, int max) {
    int n = 0;
    if (n < max && uevent_fd() >= 0) p[n++] = (struct pollfd){ uevent_fd(), POLLIN, 0 };
    if (n < max && notifyd_fd() >= 0) p[n++] = (struct pollfd){ notifyd_fd(), (short)(POLLIN | (notifyd_wants_write() ? POLLOUT : 0)), 0 };
    if (n < max && mpris_fd() >= 0) p[n++] = (struct pollfd){ mpris_fd(), (short)(POLLIN | (mpris_wants_write() ? POLLOUT : 0)), 0 };
    if (n < max && sysbus_fd() >= 0) p[n++] = (struct pollfd){ sysbus_fd(), (short)(POLLIN | (sysbus_wants_write() ? POLLOUT : 0)), 0 };
    if (n < max && pwguard_fd() >= 0) p[n++] = (struct pollfd){ pwguard_fd(), (short)(POLLIN | (pwguard_wants_write() ? POLLOUT : 0)), 0 };
    n += pkagent_pollfds(p + n, max - n);
    return n;
}

void hw_dispatch(const struct pollfd *p, int n) {
    for (int i = 0; i < n; i++) {
        if (!p[i].revents) continue;
        if (p[i].fd == uevent_fd()) {
            int m = uevent_dispatch();
            if (m & UEV_DRM) display_hotplug();
            if (m & UEV_POWER) power_changed();
            if (m & UEV_BACKLIGHT) display_hotplug();      /* the list carries the backlight */
        } else if (p[i].fd == sysbus_fd()) sysbus_dispatch();
        else if (p[i].fd == notifyd_fd()) notifyd_dispatch();
        else if (p[i].fd == mpris_fd()) mpris_dispatch();
        else if (p[i].fd == pwguard_fd()) pwguard_dispatch();
    }
    pkagent_dispatch(p, n);
}

void hw_events(void) {
    if (power_next > 0 && hw_now_ms() >= power_next) power_changed();
    sleeplock_tick(hw_now_ms());
    bt_flush_events();
    notifyd_tick();
    mpris_tick();
    pwguard_tick();
    pkagent_tick();
    if (sysbus_up() && sysbus_wants_write()) dbus_flush(sysbus_conn());
}

/* ---- timedated (read-only: NTP, synchronized, can NTP) ---- */

static void got_timedate(const DMsg *m, void *ctx) {
    (void)ctx;
    int ntp = 0, synced = 0, can = 0, rtc = 0;
    char zone[96] = "";
    if (m && m->type == DBUS_RETURN && m->sig && !strcmp(m->sig, "a{sv}")) {
        DRd r;
        dr_init(&r, m);
        size_t end = dr_arr(&r, 8);
        while (!r.err && dr_more(&r, end)) {
            dr_struct(&r);
            const char *k = dr_str(&r, NULL), *sig = dr_variant(&r);
            if (r.err || !k || !sig) break;
            if (!strcmp(sig, "b")) {
                int v = dr_bool(&r);
                if (!strcmp(k, "NTP")) ntp = v;
                else if (!strcmp(k, "NTPSynchronized")) synced = v;
                else if (!strcmp(k, "CanNTP")) can = v;
                else if (!strcmp(k, "LocalRTC")) rtc = v;
            } else if (!strcmp(sig, "s") && !strcmp(k, "Timezone")) {
                const char *z = dr_str(&r, NULL);
                snprintf(zone, sizeof zone, "%s", z ? z : "");
            } else dr_skip(&r, sig);
        }
    }
    HBuf b = { 0 };
    hb_printf(&b, "{\"available\":%s,\"ntp\":%s,\"synced\":%s,\"canNTP\":%s,\"localRTC\":%s,\"zone\":",
              m && m->type == DBUS_RETURN ? "true" : "false", ntp ? "true" : "false", synced ? "true" : "false",
              can ? "true" : "false", rtc ? "true" : "false");
    hb_str(&b, zone);
    hb_printf(&b, "}");
    char *j = hb_take(&b);
    if (j) hw_emit("timedate", j);
    free(j);
}

void hw_timedate_refresh(void) {
    if (!sysbus_up()) { got_timedate(NULL, NULL); return; }
    DBuf b;
    dw_init(&b);
    dw_str(&b, "org.freedesktop.timedate1");
    if (!sysbus_call("org.freedesktop.timedate1", "/org/freedesktop/timedate1", "org.freedesktop.DBus.Properties", "GetAll", "s", &b, got_timedate, NULL))
        got_timedate(NULL, NULL);
    dw_free(&b);
}
