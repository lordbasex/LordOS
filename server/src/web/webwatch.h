/* webwatch.h — is it still answering? (docs/WEBAPPS.md, "When a web app stops answering"). Pure rules, no I/O, used on
 * both sides of a web app:
 *
 *   - lord-server watches each lord-web it started (ui/webalive.c): org.lordos.WebApp1.Ping() over the session bus,
 *     which lord-web answers from its main loop. A lord-web that doesn't answer is ended (its process group, then each of
 *     its WebKit processes) and started again, and the shell says so ("WhatsApp wasn't responding and was restarted").
 *   - lord-web watches its page (web/webhang.c): a script of one character evaluated in a world of its own, which the
 *     web process answers from its main thread. A page that doesn't answer has its web process ended and the app's page
 *     loaded again; lord-web itself, the window and the profile stay.
 *
 * A ping every `period` ms while it runs; one not answered within `wait` ms is a miss, and `misses` in a row mean it
 * hangs. When the person asked for the app (the launcher, the Dock, a notification: webwatch_urgent) a ping goes at once
 * and a single one not answered within `urgent` ms is enough: that is when a hang shows as "nothing happens". Nothing is
 * decided during `grace` ms after the start (taking its bus name, loading its page), and when the watcher itself didn't
 * run for WEBWATCH_STALL_MS (the computer was busy or asleep) the wait starts again: the silence was the watcher's, not
 * theirs. */
#ifndef LORD_WEB_WEBWATCH_H
#define LORD_WEB_WEBWATCH_H

#include <stdint.h>

#define WEBWATCH_STALL_MS 3000.0

typedef struct { double period, wait, urgent, grace; int misses; } WebWatchLimits;

/* lord-web as a process (lord-server's ping): its main loop answers at once unless it hangs */
#define WEBWATCH_PROCESS ((WebWatchLimits){ .period = 15000, .wait = 10000, .urgent = 5000, .grace = 20000, .misses = 2 })
/* a page (lord-web's ping): its main thread may be busy for a while with a heavy script, so longer waits */
#define WEBWATCH_PAGE    ((WebWatchLimits){ .period = 15000, .wait = 15000, .urgent = 8000, .grace = 45000, .misses = 2 })

typedef struct {
    WebWatchLimits lim;
    double started;          /* the start, or the last restart */
    double last_step;        /* the watcher's last pass (WEBWATCH_STALL_MS) */
    double last_ping;        /* when the last ping went (0: none yet) */
    double sent;             /* the ping in flight went then (0: none in flight) */
    uint32_t serial;         /* the caller's id for the ping in flight */
    int in_flight_urgent;    /* the ping in flight has the urgent wait */
    int want_urgent;         /* the person asked for the app: ping now, with the urgent wait */
    int misses;              /* periodic pings not answered, in a row */
} WebWatch;

enum { WEBWATCH_WAIT = 0, WEBWATCH_PING = 1, WEBWATCH_HUNG = 2 };

void webwatch_init(WebWatch *w, WebWatchLimits lim, double now);
/* What to do now: WAIT, PING (send one, then webwatch_sent) or HUNG (restart it: the watch starts over by itself). */
int  webwatch_step(WebWatch *w, double now);
void webwatch_sent(WebWatch *w, uint32_t serial, double now);
/* The answer to the ping in flight (any answer: lord-web said it is there). 1 if serial was that ping. */
int  webwatch_answer(WebWatch *w, uint32_t serial, double now);
/* The bus said nobody owns its name (it hasn't taken it yet, or lost its connection): during the grace nothing, after
 * it a miss at once (a launch then can't reach it either). 1 if serial was the ping in flight. */
int  webwatch_absent(WebWatch *w, uint32_t serial, double now);
/* The person asked for it: the next step pings at once, and one ping not answered within lim.urgent means it hangs. */
void webwatch_urgent(WebWatch *w);
/* ms until the next step has something to do (for the main loop's poll), >= 0 */
double webwatch_next(const WebWatch *w, double now);

#endif
