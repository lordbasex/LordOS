/* webask.h — org.lordos.WebApps1 on the session bus (docs/WEBAPPS.md, "Permissions"): a web app's page asked
 * for the camera, the microphone, the screen, the location or notifications, its registry entry says "ask",
 * and lord-web asks the person through lord-server:
 *
 *   org.lordos.WebApps1.Ask(s app, s profile, s kind) → s   on /org/lordos/WebApps1, bus name org.lordos.WebApps1
 *
 * The answer is "allow", "deny" or "once". Only the lord-web process lord-server started for that app and
 * profile (or one of its own processes) gets an answer: the bus says the caller's pid
 * (GetConnectionUnixProcessID), never the caller. The question goes to the shell as the "webapps-ask" event;
 * lord.webapps.answer() brings the person's answer back, which is stored in the registry unless it is "once".
 * Served on portal.c's connection; portal.c hands the calls, replies and departures over. */
#ifndef LORD_UI_WEBASK_H
#define LORD_UI_WEBASK_H

#include "../vault/dbus.h"

#define WEBASK_NAME  "org.lordos.WebApps1"
#define WEBASK_PATH  "/org/lordos/WebApps1"
#define WEBASK_IFACE "org.lordos.WebApps1"
#define WEBASK_MAX   16

void webask_call(DConn *c, const DMsg *m);     /* a call on WEBASK_PATH */
int  webask_reply(DConn *c, const DMsg *m);    /* 1 if m answered one of our pid questions */
void webask_gone(const char *name);            /* that connection left the bus: its questions go */
void webask_lost(void);                        /* the bus went away */
/* The person's answer to question id: "allow", "deny" or "once". 0 answered, -1 unknown id or value. */
int  webask_answer(int id, const char *value);
/* The questions waiting for the person, as a JSON array (malloc'd), for a new JS context. */
char *webask_pending(void);

#endif
