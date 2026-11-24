/* gnotes.c — the guest desktop's own notifications, passed on to the host (see gnotes.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gnotes.h"
#include "dbusmin.h"
#include "../../server/src/vm/vmagent.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

static Db mon = { .fd = -1 };
static int want;
static long long retry_at;
static char bus_path[256];
static size_t skip;                     /* bytes of a message too large to keep, still to drop */
static AgRate rate;

static long long mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static void path_of(const char *bus) {
    bus_path[0] = 0;
    if (bus) { snprintf(bus_path, sizeof bus_path, "%s", bus); return; }
    const char *a = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (a && !strncmp(a, "unix:path=/", 11)) {
        const char *p = a + 10;
        size_t l = strcspn(p, ",;");
        if (l < sizeof bus_path && !memchr(p, '%', l)) { memcpy(bus_path, p, l); bus_path[l] = 0; return; }
    }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0] == '/' && (size_t)snprintf(bus_path, sizeof bus_path, "%s/bus", rt) >= sizeof bus_path) bus_path[0] = 0;
}

static void stop(void) { db_close(&mon); skip = 0; }

static void start(void) {
    if (!bus_path[0] || db_open(&mon, bus_path, 2000)) { mon.fd = -1; return; }
    static const char *const rules[] = { "type='method_call',interface='org.freedesktop.Notifications',member='Notify'" };
    uint8_t body[512];
    DbW w;
    dbw_init(&w, body, sizeof body);
    dbw_strv(&w, rules, 1);
    dbw_u32(&w, 0);
    DbMsg r;
    if (w.bad || db_call(&mon, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus.Monitoring", "BecomeMonitor", "asu", body, w.n, &r, 2000)) {
        fprintf(stderr, "lordos-guest: the session bus doesn't let this helper see notifications\n");
        stop();
        return;
    }
    /* what came with the reply stays in mon.in after it */
    if (mon.used) { memmove(mon.in, mon.in + mon.used, mon.in_n - mon.used); mon.in_n -= mon.used; mon.used = 0; }
}

void gnotes_want(int on, const char *bus) {
    if (!on) { want = 0; if (mon.fd >= 0) stop(); return; }
    if (!want) { want = 1; retry_at = 0; path_of(bus); }
    if (mon.fd < 0 && mono() >= retry_at) { retry_at = mono() + 30000; start(); }
}

int gnotes_fd(void) { return want ? mon.fd : -1; }

static void one(int port, const DbMsg *m) {
    char app[128], summary[512], body[2048], t[AG_NOTE_TITLE_MAX + 1], b[AG_NOTE_BODY_MAX + 1];
    if (db_notify_body(m, app, sizeof app, summary, sizeof summary, body, sizeof body)) return;
    size_t tn = db_note_plain(summary, t, AG_NOTE_TITLE_MAX);
    if (!tn) tn = db_note_plain(app, t, AG_NOTE_TITLE_MAX);
    if (!tn) return;
    size_t bn = db_note_plain(body, b, AG_NOTE_BODY_MAX);
    uint8_t out[AG_NOTE_BYTES];
    size_t n = ag_note_write((const uint8_t *)t, tn, (const uint8_t *)b, bn, out, sizeof out);
    if (!n || port < 0 || !ag_rate_take(&rate, mono(), 3, 1000)) return;
    uint8_t h[AG_HEAD];
    ag_frame_head(h, AGK_NOTIFY, 0, (uint32_t)n);
    struct iovec io[2] = { { h, AG_HEAD }, { out, n } };
    if (writev(port, io, 2) < 0) {}
}

void gnotes_read(int port) {
    if (mon.fd < 0) return;
    ssize_t r = read(mon.fd, mon.in + mon.in_n, sizeof mon.in - mon.in_n);
    if (r <= 0) { if (r < 0 && (errno == EAGAIN || errno == EINTR)) return; stop(); retry_at = mono() + 30000; return; }
    mon.in_n += (size_t)r;
    for (;;) {
        if (skip) {
            size_t k = skip < mon.in_n ? skip : mon.in_n;
            memmove(mon.in, mon.in + k, mon.in_n - k);
            mon.in_n -= k; skip -= k;
            if (skip) return;
        }
        long total = db_msg_total(mon.in, mon.in_n);
        if (total < 0) { stop(); retry_at = mono() + 30000; return; }
        if (!total) return;
        if ((size_t)total > sizeof mon.in) { skip = (size_t)total; continue; }    /* too large to keep: dropped whole */
        if ((size_t)total > mon.in_n) return;
        DbMsg m;
        int used = db_msg_parse(mon.in, mon.in_n, &m);
        if (used > 0) one(port, &m);
        memmove(mon.in, mon.in + total, mon.in_n - (size_t)total);
        mon.in_n -= (size_t)total;
    }
}
