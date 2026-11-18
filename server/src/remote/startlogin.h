/* startlogin.h — who may ask lord-sessiond for the login screen in text mode (startlordos; docs/SESSIONS.md, "Text
 * mode"). Pure, tested by tests/startlogin_test.c. The request comes over /run/lordos/start.sock; lord-sessiond reads
 * the caller from the socket itself (SO_PEERCRED), never from what it sends, and gathers the rest as root:
 *   tty_nr       the caller's controlling terminal (/proc/<pid>/stat, field 7)
 *   active       the console in front (VT_GETSTATE)
 *   owner        the uid that owns /dev/tty<N> (login gives the console to whoever logged in on it)
 * Accepted only from someone logged in on the console that is in front, typing it there: never a process of a
 * graphical session, a remote shell, a cron job or another console. Returns that console's number, or a negative
 * reason. Showing the login screen asks for the password anyway; this only keeps anyone else from turning it on. */
#ifndef LORD_STARTLOGIN_H
#define LORD_STARTLOGIN_H
#include <sys/types.h>

#define SL_NOT_A_CONSOLE (-1)   /* no controlling terminal, or a pseudo-terminal (a terminal window, ssh) */
#define SL_NOT_IN_FRONT  (-2)   /* a console, but not the one being shown */
#define SL_NOT_ITS_OWNER (-3)   /* that console isn't the caller's own (another user logged in there, or nobody) */
#define SL_NOT_A_PERSON  (-4)   /* root or a system account */

static inline int startlogin_check(uid_t peer, unsigned long tty_nr, int active, uid_t owner) {
    if (peer < 1000 || peer == (uid_t)65534) return SL_NOT_A_PERSON;
    unsigned major = (unsigned)((tty_nr >> 8) & 0xfff), minor = (unsigned)((tty_nr & 0xff) | ((tty_nr >> 12) & 0xfff00));
    if (major != 4 || minor < 1 || minor > 63) return SL_NOT_A_CONSOLE;   /* /dev/tty1…63: TTY_MAJOR 4 */
    if ((int)minor != active) return SL_NOT_IN_FRONT;
    if (owner != peer) return SL_NOT_ITS_OWNER;
    return (int)minor;
}
#endif
