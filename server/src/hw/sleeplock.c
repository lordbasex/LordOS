/* sleeplock.c — the screen locks before the computer sleeps (see sleeplock.h). */
#include "sleeplock.h"
#include "display.h"
#include "hw.h"
#include "sysbus.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOGIN1      "org.freedesktop.login1"
#define LOGIN1_PATH "/org/freedesktop/login1"
#define LOGIN1_MGR  "org.freedesktop.login1.Manager"
/* logind waits for a delay inhibitor at most InhibitDelayMaxSec (5 s by default): ours is let go before */
#define HOLD_MS 4000.0

static int inhibit_fd = -1, asking, lock_sent, holding;
static char owner[128];                 /* logind's unique name; "" while it has none */
static double hold_until;               /* CLOCK_MONOTONIC ms */
static double asleep_at, dark_before;   /* CLOCK_BOOTTIME ms at PrepareForSleep(true); ms dark before it */

static double clock_ms(clockid_t id) {
    struct timespec t;
    clock_gettime(id, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int sleeplock_inhibitor(void) { return inhibit_fd; }

static void release(const char *why) {
    if (inhibit_fd >= 0) {
        close(inhibit_fd);
        if (why) fprintf(stderr, "sleep: %s\n", why);
    }
    inhibit_fd = -1;
    holding = 0;
}

static void got_inhibit(const DMsg *m, void *ctx) {
    (void)ctx;
    asking = 0;
    if (!m || m->type != DBUS_RETURN || !m->sig || strcmp(m->sig, "h") || m->n_fds != 1) {
        fprintf(stderr, "sleep: logind did not give a sleep delay (%s): the lock before sleep is not waited for\n",
                m && m->error ? m->error : "no answer");
        return;
    }
    DRd r;
    dr_init(&r, m);
    uint32_t idx = dr_u32(&r);
    if (r.err || idx != 0) return;
    if (inhibit_fd >= 0) close(inhibit_fd);
    inhibit_fd = fcntl(m->fds[0], F_DUPFD_CLOEXEC, 3);     /* the message's own is closed after this */
}

static void take(void) {
    if (asking || inhibit_fd >= 0 || !owner[0]) return;
    DBuf b;
    dw_init(&b);
    dw_str(&b, "sleep");
    dw_str(&b, "LordOS");
    dw_str(&b, "Locking the screen before sleep");
    dw_str(&b, "delay");
    if (sysbus_call(LOGIN1, LOGIN1_PATH, LOGIN1_MGR, "Inhibit", "ssss", &b, got_inhibit, NULL)) asking = 1;
    dw_free(&b);
}

static void on_owner(const char *name, const char *o) {
    (void)name;
    snprintf(owner, sizeof owner, "%s", o ? o : "");
    if (owner[0]) take();
}

static void going_to_sleep(void) {
    double mono = clock_ms(CLOCK_MONOTONIC);
    asleep_at = clock_ms(CLOCK_BOOTTIME);
    dark_before = display_dark_ms(mono);
    lock_sent = 0;
    if (!display_on_monitor()) { release(NULL); return; }  /* not on the screen: nothing to hide */
    hw_emit("idle", "{\"stage\":\"suspend\"}");             /* the theme locks if the settings say so */
    if (lock_sent && display_on_monitor() && inhibit_fd >= 0) {
        holding = 1;
        hold_until = mono + HOLD_MS;
        fprintf(stderr, "sleep: locking the screen before the computer sleeps\n");
    } else release(lock_sent ? NULL : "the computer sleeps (no lock asked for)");
}

static void woke(void) {
    double slept = asleep_at > 0 ? clock_ms(CLOCK_BOOTTIME) - asleep_at + dark_before : 0;
    asleep_at = dark_before = 0;
    release(NULL);
    if (display_on_monitor()) {
        char j[64];
        snprintf(j, sizeof j, "{\"stage\":\"resume\",\"slept\":%.0f}", slept > 0 ? slept / 1000.0 : 0.0);
        hw_emit("idle", j);
    }
    take();                             /* for the next sleep */
}

static void on_signal(const DMsg *m) {
    if (m->type != DBUS_SIGNAL || !m->member || strcmp(m->member, "PrepareForSleep") || !m->iface ||
        strcmp(m->iface, LOGIN1_MGR) || !m->path || strcmp(m->path, LOGIN1_PATH) || !m->sig || strcmp(m->sig, "b") ||
        !m->sender || !owner[0] || strcmp(m->sender, owner))
        return;                         /* only logind itself (the bus sets the sender) */
    DRd r;
    dr_init(&r, m);
    int sleeping = dr_bool(&r);
    if (r.err) return;
    if (sleeping) going_to_sleep();
    else woke();
}

void sleeplock_start(void) {
    if (!sysbus_up()) return;
    sysbus_on_signal(on_signal);
    sysbus_add_match("type='signal',sender='" LOGIN1 "',path='" LOGIN1_PATH "',interface='" LOGIN1_MGR "',member='PrepareForSleep'");
    sysbus_watch_name(LOGIN1, on_owner);
}

void sleeplock_lock_sent(void) { lock_sent = 1; }

void sleeplock_monitor_taken(void) { if (holding) release("the screen is locked; the computer may sleep"); }

double sleeplock_timeout(double now) {
    if (!holding) return -1;
    return hold_until > now ? hold_until - now : 0;
}

void sleeplock_tick(double now) {
    if (holding && now >= hold_until) release("the monitor was not taken in time; letting the computer sleep");
}
