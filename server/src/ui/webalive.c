/* webalive.c — see webalive.h. */
#include "webalive.h"
#include "../hw/hw.h"
#include "../web/webmgr.h"
#include "../web/webreg.h"
#include "../web/webwatch.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    int used;
    int asked;                                   /* the person asked for it since its last verdict (webalive_urgent) */
    pid_t pid;                                   /* the lord-web this watch is about: a new process gets a new watch */
    char app[WEBREG_ID_MAX + 1], profile[WEBREG_ID_MAX + 1];
    WebWatch w;
} Slot;

/* the automatic restarts of an app and profile, for WEBALIVE_RESTARTS */
typedef struct { char app[WEBREG_ID_MAX + 1], profile[WEBREG_ID_MAX + 1]; double at[WEBALIVE_RESTARTS]; int left_alone; } Hist;

/* the same clock as lord-server's main loop (CLOCK_MONOTONIC, ms) */
static double mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static WebWatchLimits limits;
static int limits_set;
static WebWatchLimits lim_now(void) { return limits_set ? limits : WEBWATCH_PROCESS; }
void webalive_set_limits(WebWatchLimits lim) { limits = lim; limits_set = 1; }

static Slot slots[WEB_MAX_RUNNING];
static Hist hist[WEB_MAX_RUNNING];
static DConn *bus;

void webalive_attach(DConn *c) { bus = c; }

void webalive_lost(void) {
    bus = NULL;
    memset(slots, 0, sizeof slots);              /* the pings in flight will never be answered */
}

static Slot *slot_of(const char *app, const char *profile) {
    for (int i = 0; i < WEB_MAX_RUNNING; i++)
        if (slots[i].used && !strcmp(slots[i].app, app) && !strcmp(slots[i].profile, profile)) return &slots[i];
    return NULL;
}

static Hist *hist_of(const char *app, const char *profile, int make) {
    Hist *free_one = NULL;
    for (int i = 0; i < WEB_MAX_RUNNING; i++) {
        if (hist[i].app[0] && !strcmp(hist[i].app, app) && !strcmp(hist[i].profile, profile)) return &hist[i];
        if (!hist[i].app[0] && !free_one) free_one = &hist[i];
    }
    if (!make) return NULL;
    if (!free_one) free_one = &hist[0];
    memset(free_one, 0, sizeof *free_one);
    snprintf(free_one->app, sizeof free_one->app, "%s", app);
    snprintf(free_one->profile, sizeof free_one->profile, "%s", profile);
    return free_one;
}

/* the running processes now: a watch for each new one, none for the ones that ended */
static void sync_slots(double now) {
    WebRunning r[WEB_MAX_RUNNING];
    int n = webmgr_running(r, WEB_MAX_RUNNING);
    for (int i = 0; i < WEB_MAX_RUNNING; i++) {
        if (!slots[i].used) continue;
        int live = 0;
        for (int k = 0; k < n && !live; k++) live = r[k].pid == slots[i].pid;
        if (!live) memset(&slots[i], 0, sizeof slots[i]);
    }
    for (int k = 0; k < n; k++) {
        int have = 0;
        for (int i = 0; i < WEB_MAX_RUNNING && !have; i++) have = slots[i].used && slots[i].pid == r[k].pid;
        if (have) continue;
        for (int i = 0; i < WEB_MAX_RUNNING; i++) {
            if (slots[i].used) continue;
            slots[i].used = 1;
            slots[i].pid = r[k].pid;
            _Static_assert(sizeof slots[i].app == sizeof r[k].app && sizeof slots[i].profile == sizeof r[k].profile, "the same ids");
            memcpy(slots[i].app, r[k].app, sizeof slots[i].app);
            memcpy(slots[i].profile, r[k].profile, sizeof slots[i].profile);
            slots[i].app[sizeof slots[i].app - 1] = slots[i].profile[sizeof slots[i].profile - 1] = 0;
            webwatch_init(&slots[i].w, lim_now(), now);
            break;
        }
    }
}

static uint32_t send_ping(const Slot *s) {
    char name[160];
    if (!bus || webreg_bus_name(s->app, s->profile, name, sizeof name)) return 0;
    DHdr h = { .type = DBUS_CALL, .flags = DBUS_NO_AUTOSTART, .path = WEBREG_SHOW_PATH, .iface = WEBREG_SHOW_IFACE,
               .member = "Ping", .dest = name };
    uint32_t serial = dbus_send(bus, &h, NULL);
    if (serial) dbus_flush(bus);
    return serial;
}

/* it didn't answer: ended and started again, unless it was restarted too often lately */
static void hung(Slot *s, double now) {
    char app[WEBREG_ID_MAX + 1], profile[WEBREG_ID_MAX + 1];
    snprintf(app, sizeof app, "%s", s->app);
    snprintf(profile, sizeof profile, "%s", s->profile);
    Hist *h = hist_of(app, profile, 1);
    int recent = 0, oldest = 0;
    for (int i = 0; i < WEBALIVE_RESTARTS; i++) {
        if (h->at[i] > 0 && now - h->at[i] < WEBALIVE_WINDOW_MS) recent++;
        if (h->at[i] < h->at[oldest]) oldest = i;
    }
    /* restarted too often lately: left alone, unless the person is asking for it right now */
    if (recent >= WEBALIVE_RESTARTS && !s->asked) {
        if (!h->left_alone) fprintf(stderr, "webapps: %s/%s did not answer: restarted %d times in 10 minutes, left alone\n", app, profile, recent);
        h->left_alone = 1;
        webwatch_init(&s->w, lim_now(), now);
        return;
    }
    h->left_alone = 0;
    h->at[oldest] = now;
    int was = 0;
    const char *e = webmgr_restart(app, profile, &was);
    memset(s, 0, sizeof *s);                     /* the new process gets its own watch (sync_slots) */
    if (e) { fprintf(stderr, "webapps: %s/%s did not answer: ended, not started again: %s\n", app, profile, e); return; }
    fprintf(stderr, "webapps: %s/%s did not answer: restarted\n", app, profile);
    char j[160];
    snprintf(j, sizeof j, "{\"app\":\"%s\",\"profile\":\"%s\"}", app, profile);
    hw_emit("webapps-restarted", j);
}

void webalive_tick(double now) {
    if (!bus) return;
    sync_slots(now);
    for (int i = 0; i < WEB_MAX_RUNNING; i++) {
        Slot *s = &slots[i];
        if (!s->used) continue;
        switch (webwatch_step(&s->w, now)) {
        case WEBWATCH_PING: {
            uint32_t serial = send_ping(s);
            if (serial) webwatch_sent(&s->w, serial, now);
            else s->w.last_ping = now;           /* couldn't be sent: the bus is going away; try at the next period */
            break;
        }
        case WEBWATCH_HUNG: hung(s, now); break;
        default: break;
        }
    }
}

double webalive_timeout(double now) {
    if (!bus) return -1;
    double best = -1;
    for (int i = 0; i < WEB_MAX_RUNNING; i++) {
        if (!slots[i].used) continue;
        double d = webwatch_next(&slots[i].w, now);
        if (best < 0 || d < best) best = d;
    }
    /* a process started since the last pass gets its watch at the next one */
    if (best < 0 || best > 1000) {
        WebRunning r[WEB_MAX_RUNNING];
        int n = webmgr_running(r, WEB_MAX_RUNNING), watched = 0;
        for (int i = 0; i < WEB_MAX_RUNNING; i++) watched += slots[i].used;
        if (n != watched) best = 1000;
    }
    return best;
}

void webalive_urgent(const char *app, const char *profile) {
    if (!bus || !app || !profile) return;
    Slot *s = slot_of(app, profile);
    if (!s) {
        sync_slots(mono_ms());
        s = slot_of(app, profile);
    }
    if (s) { webwatch_urgent(&s->w); s->asked = 1; }
}

int webalive_reply(const DMsg *m) {
    if (!m->reply_serial) return 0;
    for (int i = 0; i < WEB_MAX_RUNNING; i++) {
        Slot *s = &slots[i];
        if (!s->used || !s->w.serial || s->w.serial != m->reply_serial) continue;
        int absent = m->type == DBUS_ERROR && m->error &&
                     (!strcmp(m->error, "org.freedesktop.DBus.Error.ServiceUnknown") || !strcmp(m->error, "org.freedesktop.DBus.Error.NameHasNoOwner"));
        int noreply = m->type == DBUS_ERROR && m->error && !strcmp(m->error, "org.freedesktop.DBus.Error.NoReply");
        if (noreply) return 1;                   /* the bus gave up waiting: our own wait decides */
        if (absent) webwatch_absent(&s->w, m->reply_serial, mono_ms());
        else if (webwatch_answer(&s->w, m->reply_serial, mono_ms())) s->asked = 0;
        return 1;
    }
    return 0;
}
