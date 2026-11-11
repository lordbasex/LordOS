/* logindlock.h — logind's Lock on the system bus locks the desktop, and LockedHint follows the lock
 * (the rules: hw/lockstate.h; docs/SESSIONS.md, "Lock from logind").
 *
 * Subscribed to Lock and Unlock of org.freedesktop.login1.Session, believed only from the connection that
 * owns org.freedesktop.login1 (the bus sets the sender). A Lock the rules accept is idle {stage:"lock"} for
 * the theme, which locks as ❖L does (lord.session.lock). Only in a session managed by lord-sessiond, on
 * the Settings system bus (hw/sysbus.c). Calls only GetSessionByPID, Properties.Get "User" on a session
 * and SetLockedHint on its own session: nothing that needs privileges. */
#ifndef LORD_HW_LOGINDLOCK_H
#define LORD_HW_LOGINDLOCK_H

void logindlock_start(void);            /* after sysbus_start */
void logindlock_lock_sent(int on_monitor); /* lord.session.lock() sent a lock (js_sys.c) */
void logindlock_shown(void);            /* RP_DISPLAY: the monitor shows the session again (main.c) */

#endif
