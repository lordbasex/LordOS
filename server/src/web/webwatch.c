/* webwatch.c — see webwatch.h. */
#include "webwatch.h"

#include <string.h>

void webwatch_init(WebWatch *w, WebWatchLimits lim, double now) {
    memset(w, 0, sizeof *w);
    w->lim = lim;
    w->started = now;
    w->last_step = now;
    w->last_ping = now;                 /* the first ping a period after the start */
}

static double limit_of(const WebWatch *w) { return w->in_flight_urgent ? w->lim.urgent : w->lim.wait; }

int webwatch_step(WebWatch *w, double now) {
    double gap = now - w->last_step;
    if (gap > WEBWATCH_STALL_MS) {
        /* the watcher didn't run: what it waited for waits again, the grace included */
        if (w->sent > 0) w->sent += gap;
        w->last_ping += gap;
        if (now - gap - w->started < w->lim.grace) w->started += gap;
    }
    w->last_step = now;
    if (w->sent > 0) {
        /* serial 0: webwatch_absent said nobody is there, which counts as not answered now */
        if (w->serial && now - w->sent < limit_of(w)) return WEBWATCH_WAIT;
        int urgent = w->in_flight_urgent;
        w->sent = 0;
        w->serial = 0;
        w->in_flight_urgent = 0;
        if (now - w->started < w->lim.grace) { w->misses = 0; return WEBWATCH_WAIT; }
        w->misses++;
        if (urgent || w->misses >= w->lim.misses) {
            webwatch_init(w, w->lim, now);
            return WEBWATCH_HUNG;
        }
    }
    if (w->want_urgent || now - w->last_ping >= w->lim.period) return WEBWATCH_PING;
    return WEBWATCH_WAIT;
}

void webwatch_sent(WebWatch *w, uint32_t serial, double now) {
    w->sent = now > 0 ? now : 1e-3;
    w->serial = serial ? serial : 1;
    w->last_ping = now;
    w->in_flight_urgent = w->want_urgent;
    w->want_urgent = 0;
}

int webwatch_answer(WebWatch *w, uint32_t serial, double now) {
    (void)now;
    if (!(w->sent > 0) || !serial || serial != w->serial) return 0;
    w->sent = 0;
    w->serial = 0;
    w->in_flight_urgent = 0;
    w->misses = 0;
    return 1;
}

int webwatch_absent(WebWatch *w, uint32_t serial, double now) {
    if (!(w->sent > 0) || !serial || serial != w->serial) return 0;
    if (now - w->started < w->lim.grace) {
        /* not on the bus yet: it is still starting */
        w->sent = 0;
        w->serial = 0;
        w->in_flight_urgent = 0;
        return 1;
    }
    w->serial = 0;                      /* the next step counts it as not answered, at once */
    return 1;
}

void webwatch_urgent(WebWatch *w) {
    if (w->sent > 0) w->in_flight_urgent = 1;   /* the ping in flight now has the short wait, from when it went */
    else w->want_urgent = 1;
}

double webwatch_next(const WebWatch *w, double now) {
    double d;
    if (w->sent > 0) d = w->serial ? w->sent + limit_of(w) - now : 0;
    else if (w->want_urgent) d = 0;
    else d = w->last_ping + w->lim.period - now;
    return d > 0 ? d : 0;
}
