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
 * Served on portal.c's connection; portal.c hands the calls, replies and departures over.
 *
 *   org.lordos.WebApps1.SetBadge(s app, s profile, u count)                  (no answer needed)
 *
 * The page's unread count for the Dock (docs/WEBAPPS.md, "Unread count on the Dock"). The same pid check as Ask
 * (webbadge.h keeps who was checked); the count is clamped to 0..9999 and goes to the shell as the "webapps-badge"
 * event { app, profile, count }; a lord-web that leaves the bus gets a 0.
 *
 *   org.lordos.WebApps1.Menu(s app, s profile, as items) → s item
 *
 * A right click in a web app's page (docs/WEBAPPS.md, "Editing"): lord-web asks for a menu at the pointer. The items
 * are names from webedit.h's closed list (never text), checked, with the same pid check as Ask; the shell gets the
 * "webapps-menu" event { id, app, profile, items } and lord.webapps.menuAnswer(id, item) brings back the chosen
 * item, or "" when the menu closed. A new menu from the same lord-web closes the one before ("webapps-menu-gone").
 *
 * The other way, lord-server to lord-web: org.lordos.WebApp1.Edit(s command) on its bus name, from the shell's Edit
 * menu (webask_edit); lord-web runs it only when the caller is the lord-server that started it. And
 * org.lordos.WebApp1.SetColorScheme(s scheme), "light" or "dark" only (web/webscheme.h), when the desktop's mode
 * changes (webask_scheme), with the same check. */
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
/* The session bus connection (portal.c, once it is up): also for the calls lord-server makes itself. */
void webask_attach(DConn *c);
/* Brings a running web app forward: org.lordos.WebApp1.Show(token) to its lord-web (webreg_bus_name), no reply
 * wanted. lord-web activates its window with the token, or maps it again when it was hidden behind its tray icon.
 * 0 sent, -1 no bus or a bad id. */
int  webask_show(const char *app, const char *profile, const char *token);
/* The person's choice in the menu id: one of its items, or "" (closed). 0 answered, -1 unknown id or item. */
int  webask_menu_answer(int id, const char *item);
/* The Edit menu: org.lordos.WebApp1.Edit(command) to that app and profile's lord-web, no reply wanted; command is
 * one of webedit.h's names. 0 sent, -1 no bus, a bad id or an unknown command. */
int  webask_edit(const char *app, const char *profile, const char *command);
/* The desktop's mode, from the theme (lord.portal.set): kept for the next lord-web's environment and, when it
 * changed, told to every running one (SetColorScheme, no reply wanted). */
void webask_scheme(int dark);
/* The counts shown now, as a JSON array [{ app, profile, count }] (malloc'd), for a new JS context. */
char *webask_badges(void);
/* The questions waiting for the person, as a JSON array (malloc'd), for a new JS context. */
char *webask_pending(void);

#endif
