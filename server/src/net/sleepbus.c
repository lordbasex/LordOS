/* sleepbus.c — lord-netd hears the computer go to sleep and wake (issue #14).
 *
 * logind announces both with the PrepareForSleep(b) signal on the system bus: true just before the
 * sleep, false right after waking. lord-netd listens with LordOS's own D-Bus client (vault/dbus.c:
 * libc only, strict parser), subscribed to that one signal, and only believes it from the
 * connection that owns org.freedesktop.login1 (the bus sets the sender; nobody can forge it).
 * It owns no name, exports nothing and calls nothing else. Without a system bus (a container, a
 * development system) it tries again later, and the jump of CLOCK_BOOTTIME over CLOCK_MONOTONIC
 * (actions.c) still catches a wake.
 *
 * Built only into lord-netd (Makefile: NETD_OBJ). */
#include "net.h"
#include "../vault/dbus.h"

#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define LOGIN1      "org.freedesktop.login1"
#define LOGIN1_PATH "/org/freedesktop/login1"
#define LOGIN1_MGR  "org.freedesktop.login1.Manager"

static DConn conn = { .fd = -1 };
static int up, tries;
static char owner[256];                 /* logind's unique name (":1.4"); "" while it has none */
static long retry_at;

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static uint32_t bus_call(const char *member, const char *sig, const char *arg, int reply) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, arg);
    DHdr h = { .type = DBUS_CALL, .flags = reply ? 0 : DBUS_NO_REPLY, .path = "/org/freedesktop/DBus",
               .iface = "org.freedesktop.DBus", .member = member, .dest = "org.freedesktop.DBus", .sig = sig };
    uint32_t s = dbus_send(&conn, &h, &b);
    dw_free(&b);
    return s;
}

static void connect_bus(void) {
    int fd = dbus_connect_system();
    if (fd < 0 || dbus_open(&conn, fd, 2000)) {
        if (fd >= 0) { conn.fd = fd; dbus_close(&conn); }
        conn.fd = -1;
        long wait = 5000L << (tries < 4 ? tries : 4);      /* 5, 10, 20, 40, 80 s */
        if (!tries++) fprintf(stderr, "lord-netd: no system bus yet: waking from sleep is noticed by the clock only\n");
        retry_at = now_ms() + wait;
        return;
    }
    up = 1;
    tries = 0;
    owner[0] = 0;
    /* who logind is now; then only its signal, and when that changes */
    uint32_t s = bus_call("GetNameOwner", "s", LOGIN1, 1);
    DMsg m = {0};
    int got = s ? dbus_wait_reply(&conn, s, &m, 2000) : -1;
    if (got == 1) {
        DRd r;
        dr_init(&r, &m);
        const char *o = m.sig && !strcmp(m.sig, "s") ? dr_str(&r, NULL) : NULL;
        if (o && !r.err) snprintf(owner, sizeof owner, "%s", o);
    }
    if (got >= 0) dbus_msg_free(&m);           /* 0: logind isn't running (yet); NameOwnerChanged tells */
    bus_call("AddMatch", "s", "type='signal',sender='" LOGIN1 "',path='" LOGIN1_PATH "',interface='" LOGIN1_MGR "',member='PrepareForSleep'", 0);
    bus_call("AddMatch", "s", "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" LOGIN1 "'", 0);
    dbus_flush(&conn);
    fprintf(stderr, "lord-netd: listening for sleep and wake on the system bus%s\n", owner[0] ? "" : " (logind not running yet)");
}

static void lost(void) {
    dbus_close(&conn);
    conn.fd = -1;
    up = 0;
    owner[0] = 0;
    retry_at = now_ms() + 5000;
    fprintf(stderr, "lord-netd: the system bus connection was lost; trying again in 5 s\n");
}

int net_sleep_poll(short *events) {
    if (!up && !tries && !retry_at) connect_bus();             /* the first time */
    if (!up) return -1;
    *events = (short)(POLLIN | (dbus_pending_out(&conn) ? POLLOUT : 0));
    return conn.fd;
}

int net_sleep_timeout(void) {
    if (up || !retry_at) return -1;
    long left = retry_at - now_ms();
    return left < 0 ? 0 : (int)left;
}

static void handle(const DMsg *m) {
    if (m->type != DBUS_SIGNAL || !m->member || !m->sender || !m->sig) return;
    DRd r;
    dr_init(&r, m);
    if (!strcmp(m->member, "NameOwnerChanged") && !strcmp(m->sender, "org.freedesktop.DBus") && !strcmp(m->sig, "sss")) {
        const char *name = dr_str(&r, NULL), *old = dr_str(&r, NULL), *now = dr_str(&r, NULL);
        (void)old;
        if (!r.err && name && now && !strcmp(name, LOGIN1)) snprintf(owner, sizeof owner, "%s", now);
        return;
    }
    if (strcmp(m->member, "PrepareForSleep") || !m->iface || strcmp(m->iface, LOGIN1_MGR) || !m->path ||
        strcmp(m->path, LOGIN1_PATH) || strcmp(m->sig, "b") || !owner[0] || strcmp(m->sender, owner))
        return;
    int sleeping = dr_bool(&r);
    if (!r.err) net_wake_event(sleeping, "logind");
}

void net_sleep_run(int revents) {
    if (!up) {
        if (retry_at && now_ms() >= retry_at) { retry_at = 0; connect_bus(); }
        return;
    }
    if (!revents) return;
    DMsg m;
    int r;
    while ((r = dbus_next(&conn, &m)) == 1) { handle(&m); dbus_msg_free(&m); }
    if (r < 0 || dbus_flush(&conn) < 0 || (revents & (POLLERR | POLLHUP))) lost();
}
