/* webbadge.h — the unread counts web apps put on their Dock icon (docs/WEBAPPS.md, "Unread count on the Dock"):
 *
 *   org.lordos.WebApps1.SetBadge(s app, s profile, u count)   on /org/lordos/WebApps1, no answer needed
 *
 * Only the lord-web process lord-server started for that app and profile counts, the same check as Ask: the bus says
 * the caller's pid (GetConnectionUnixProcessID), never the caller, and webmgr_owner says whose lord-web it is. A bus
 * name is never given to another connection, so a sender checked once stays that app and profile until it leaves the
 * bus; one that turned out to be anything else is refused from then on.
 *
 * Pure: the table of senders and what to do with each call (webask.c does the bus and the events). Tested by
 * tests/webbadge_test.c. */
#ifndef LORD_UI_WEBBADGE_H
#define LORD_UI_WEBBADGE_H

#include <stdint.h>

#define WEBBADGE_MAX     32           /* senders: as many as lord-web processes can run (WEB_MAX_RUNNING) */
#define WEBBADGE_COUNT_MAX 9999       /* a count is clamped to 0..9999 */
#define WEBBADGE_ID_MAX  40           /* WEBREG_ID_MAX */
#define WEBBADGE_NAME_MAX 255         /* a bus name */

enum { WB_NEW, WB_ASKING, WB_OK, WB_REFUSED };

typedef struct {
    int state;                        /* WB_*; WB_NEW: the slot is free */
    char sender[WEBBADGE_NAME_MAX + 1];
    char app[WEBBADGE_ID_MAX + 1], profile[WEBBADGE_ID_MAX + 1];
    uint32_t serial;                  /* our GetConnectionUnixProcessID while WB_ASKING */
    uint32_t count;                   /* the last count (shown once WB_OK) */
} WebBadge;

typedef struct { WebBadge b[WEBBADGE_MAX]; } WebBadges;

/* What a SetBadge call needs. */
enum {
    WB_EMIT,        /* a checked sender: show *count for its app and profile now */
    WB_ASK_PID,     /* a new sender: ask the bus for its pid and pass the serial to webbadge_asked */
    WB_WAIT,        /* its pid is being asked already: the count is kept and shown when it is checked */
    WB_REFUSE,      /* bad ids, a sender of another app or profile, one refused before, or no room */
};

uint32_t webbadge_clamp(uint32_t count);
/* A SetBadge(app, profile, count) from sender. *slot (out) is the entry it concerns (for WB_ASK_PID, WB_EMIT). */
int  webbadge_call(WebBadges *t, const char *sender, const char *app, const char *profile, uint32_t count, int *slot);
/* The pid question for slot went out with this serial (0: it could not, and the slot is freed). */
void webbadge_asked(WebBadges *t, int slot, uint32_t serial);
/* The bus answered serial. owner_ok: the pid is the lord-web lord-server started for exactly that slot's app and
 * profile. The slot of serial (or -1 when none waits for it); 1 into *show when its count is to be shown now. */
int  webbadge_answer(WebBadges *t, uint32_t serial, int owner_ok, int *show);
/* That connection left the bus: its slot is freed. 1 when it had a count shown (so its app's badge must drop it), with
 * its app and profile copied out. */
int  webbadge_gone(WebBadges *t, const char *sender, char *app, char *profile);

#endif
