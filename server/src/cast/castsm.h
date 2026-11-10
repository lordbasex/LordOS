/* castsm.h — the sessions of org.freedesktop.portal.ScreenCast (ui/portal.c), as a state machine with no I/O, so
 * tests/castsm_test.c covers every step: what an app may call in which state, and what ends a session.
 *
 *   CreateSession  → CREATED
 *   SelectSources  CREATED → SELECTED (once: what may be shared, one or several, the pointer)
 *   Start          SELECTED → CHOOSING: LordOS's own chooser is up; nothing is shared until the person picks
 *     picked       CHOOSING → STARTING (the helper makes the streams) → STARTED (the app gets them)
 *     Cancel       CHOOSING → SELECTED (the app hears response 1 and may Start again)
 *     no streams   STARTING → SELECTED (response 2)
 *   OpenPipeWireRemote  STARTED only
 *   Session.Close, the app leaving the bus, the indicator's Stop, a lock: the session ends from any state.
 * A session belongs to the bus name that made it: every call names it and another sender's is "not found". */
#ifndef LORD_CAST_CASTSM_H
#define LORD_CAST_CASTSM_H

#include <stdint.h>

#define CSM_MAX 16                      /* sessions at once */
#define CSM_PER_APP 4                   /* per bus name */
#define CSM_SENDER_MAX 256
#define CSM_PATH_MAX 400

enum { CSM_TYPE_MONITOR = 1, CSM_TYPE_WINDOW = 2, CSM_TYPES = 3 };
/* hidden and embedded; metadata (a cursor sent apart from the picture) isn't offered */
enum { CSM_CURSOR_HIDDEN = 1, CSM_CURSOR_EMBEDDED = 2, CSM_CURSOR_METADATA = 4, CSM_CURSORS = 3 };
enum { CSM_MAX_SOURCES = 4 };           /* CAST_MAX_STREAMS */

typedef enum { CS_FREE, CS_CREATED, CS_SELECTED, CS_CHOOSING, CS_STARTING, CS_STARTED } CsState;
/* errors, each a D-Bus error name in portal.c */
enum { CSM_OK = 0, CSM_E_ARGS = -1, CSM_E_STATE = -2, CSM_E_FULL = -3, CSM_E_NOTFOUND = -4, CSM_E_BUSY = -5 };

typedef struct {
    int id;
    CsState st;
    char sender[CSM_SENDER_MAX];
    char path[CSM_PATH_MAX];            /* the Session object */
    char req[CSM_PATH_MAX];             /* the Request of the Start being answered, "" else */
    uint32_t types, cursor;
    int multiple;
    uint32_t persist;                   /* asked for; never kept (no restore token is given) */
} CsSess;

typedef struct { CsSess s[CSM_MAX]; int seq; } CastSm;

void csm_init(CastSm *m);
/* OBJ/<what>/<sender without ':' and with '.' as '_'>/<token> — the form the spec gives, so the app can listen before
 * it calls. A token with other characters than [A-Za-z0-9_] is cut to those; with none left it is "lordos<id>". */
void csm_path(const char *obj, const char *what, const char *sender, const char *token, int id, char *out, unsigned n);
/* CreateSession: a new session for sender with the session's token. *out stays valid until csm_end. */
int  csm_create(CastSm *m, const char *sender, const char *token, CsSess **out);
/* The session at path, if it is sender's. */
CsSess *csm_find(CastSm *m, const char *sender, const char *path);
CsSess *csm_by_id(CastSm *m, int id);
/* SelectSources: types 0 means monitor; cursor 0 means hidden. persist is accepted and not kept. */
int  csm_select(CastSm *m, CsSess *s, uint32_t types, int multiple, uint32_t cursor, uint32_t persist);
/* Start: the chooser for request handle req. Only one chooser at a time (CSM_E_BUSY). */
int  csm_start(CastSm *m, CsSess *s, const char *req);
/* The person's answer: n sources of these kinds (CSM_TYPE_*), or n = 0 for Cancel. 0, or an error when the picks
 * don't fit what was selected (the shell is trusted, the check still holds). */
int  csm_chosen(CastSm *m, CsSess *s, const uint32_t *kinds, int n);
/* The streams are there: STARTING → STARTED. */
int  csm_started(CsSess *s);
/* They couldn't be made (no PipeWire, the helper failed): STARTING → SELECTED, the app hears response 2. */
int  csm_failed(CsSess *s);
/* The app's Request.Close of its Start: back to SELECTED. 0 if req was that Start's. */
int  csm_request_close(CastSm *m, const char *sender, const char *req, CsSess **out);
/* OpenPipeWireRemote may be called now. */
int  csm_may_remote(const CsSess *s);
/* Ends a session: its slot is free again. */
void csm_end(CsSess *s);
/* The app's bus name went away: the ids of its sessions (up to max), each to end by the caller. */
int  csm_gone(CastSm *m, const char *sender, int *ids, int max);
/* The session whose chooser is up, or NULL. */
CsSess *csm_choosing(CastSm *m);

/* Before the chooser: who asked decides what happens (portal.c cast_ask), failing closed like sec/pwguard.c.
 *   identified          the sender is an app LordOS verified (its launcher's scope, appid_of_pid), never what the
 *                       process calls itself
 *   restricted          that app has the firewall's "no screen capture" (FW_R_NO_SCREEN)
 *   restricted_running  some app with "no screen capture" runs in this session
 * An app with the restriction is refused. A sender LordOS can't tell is refused while a restricted app runs (it may
 * be that app, out of its scope); otherwise it gets the chooser, which says it is an unknown program. */
enum { CSM_GATE_REFUSE = 0, CSM_GATE_APP = 1, CSM_GATE_UNKNOWN = 2 };
int csm_gate(int identified, int restricted, int restricted_running);

#endif
