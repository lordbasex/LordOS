/* webalive.h — lord-server keeps watch over the lord-web processes it started (docs/WEBAPPS.md, "When a web app stops
 * answering"). Each one is asked org.lordos.WebApp1.Ping() on its bus name (webreg_bus_name) every 15 s, and at once when
 * the person asks for the app (the launcher, the Dock: webalive_urgent, from lord.webapps.launch when it already runs).
 * lord-web answers from its main loop; the rules of when silence means a hang are web/webwatch.h's (WEBWATCH_PROCESS).
 *
 * A lord-web that doesn't answer is ended and started again (webmgr_restart: only a process of webmgr's table, its group
 * and the WebKit processes under it), one line goes to the log ("webapps: <app>/<profile> did not answer: restarted")
 * and the shell gets the "webapps-restarted" event { app, profile }, which it shows as a notification. At most
 * WEBALIVE_RESTARTS restarts of one app and profile in WEBALIVE_WINDOW_MS: after that it is left alone (and logged),
 * unless the person is asking for it (webalive_urgent) when it hangs.
 *
 * An answer of any kind counts (an older lord-web without Ping answers UnknownMethod: it is there). An error that says
 * nobody owns the name counts as no answer once its start's grace is over: a launch couldn't reach it either. Served on
 * portal.c's connection (its replies come here first). */
#ifndef LORD_UI_WEBALIVE_H
#define LORD_UI_WEBALIVE_H

#include "../vault/dbus.h"
#include "../web/webwatch.h"

#define WEBALIVE_RESTARTS  3
#define WEBALIVE_WINDOW_MS (10 * 60 * 1000.0)

void   webalive_attach(DConn *c);       /* the session bus is up */
void   webalive_lost(void);             /* the bus went away: no pings, nothing decided */
int    webalive_reply(const DMsg *m);   /* 1 when m answered one of our pings */
void   webalive_tick(double now);       /* the main loop, every pass */
double webalive_timeout(double now);    /* ms until webalive_tick has something to do; -1 nothing to watch */
/* The person asked for that app and profile while it runs: a ping now, and one not answered within 5 s is a hang. */
void   webalive_urgent(const char *app, const char *profile);
/* For tests: shorter limits than WEBWATCH_PROCESS (for watches started after the call). */
void   webalive_set_limits(WebWatchLimits lim);

#endif
