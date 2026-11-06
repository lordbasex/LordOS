/* sleeplock.h — the screen locks before the computer sleeps (issue #36, docs/SETTINGS.md).
 *
 * Closing the lid, the power or sleep button and the sleep timers all end in logind's sleep, which
 * announces it with PrepareForSleep(true) and, after waking, PrepareForSleep(false). The session on
 * the screen holds a "delay" sleep inhibitor (allowed to anyone by logind's policy), so logind waits for
 * it: on PrepareForSleep(true) the theme hears idle {stage:"suspend"} and, with "Require password after
 * the display turns off" set to Immediately, locks (lord.session.lock). The inhibitor is let go once
 * lord-sessiond has taken the monitor from the session (RP_UNDISPLAY), so the computer never sleeps,
 * and never wakes, with the desktop on the screen. After waking: idle {stage:"resume", slept: s} (the
 * seconds asleep plus the time the screen had already been off), for a longer lock delay.
 *
 * Only in a session managed by lord-sessiond and on the system bus (hw/sysbus.c, which passes fds for
 * the inhibitor). Only the session on the screen is told. */
#ifndef LORD_HW_SLEEPLOCK_H
#define LORD_HW_SLEEPLOCK_H

void   sleeplock_start(void);           /* after sysbus_start */
void   sleeplock_lock_sent(void);       /* lord.session.lock() sent a lock (js_sys.c) */
void   sleeplock_monitor_taken(void);   /* RP_UNDISPLAY: lord-sessiond took the monitor (main.c) */
double sleeplock_timeout(double now_ms);/* ms until the inhibitor must be let go anyway, or -1 */
void   sleeplock_tick(double now_ms);

/* For tests: the inhibitor fd held (-1 none). */
int    sleeplock_inhibitor(void);

#endif
