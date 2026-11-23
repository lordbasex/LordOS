/* xsstore.h — what xdg-session-management keeps (xsession.c), as plain data: sessions, their windows' places, and
 * the file they live in. Pure (no Wayland), for tests/xsstore_test.c.
 *
 * A session belongs to an owner: who lord-server checked the app is ("program:<exe>", "flatpak:<id>",
 * "app:<isolated LordOS app>", wl__client_owner), never anything the app said. Its id is 32 random hex characters
 * lord-server made (xs_new_id); a session is found only by that id together with the same owner, so an app that
 * learns or guesses another app's id gets a new, empty session instead of the other app's windows. An app with no
 * owner LordOS can tell (Xwayland, the login screen) gets sessions that live only in memory and are never found again.
 *
 * The file (~/.config/lordos/wl-sessions/sessions, protected from lord.fs: fsops.c) is text, one record per line:
 *   lordos-wl-sessions 1
 *   S <id> <last used, unix seconds> <owner>
 *   T <x> <y> <w> <h> <maximized 0|1> <name>          (a window of the S above: its geometry on screen, logical)
 * Any program of the user can edit it, so all of it is checked again when read: a bad line is skipped, a bad
 * session with all of its windows. Limits: XS_SESSIONS sessions, XS_PER_OWNER per owner (the least recently used
 * goes first; the apps with no owner share one owner's limit, so they can't push other apps' sessions out), XS_TOPS windows each, names up to XS_NAME_MAX bytes without control characters (a longer name works
 * while the app runs but isn't kept), the file at most XS_FILE_MAX bytes. */
#ifndef LORD_XSSTORE_H
#define LORD_XSSTORE_H

#include <stddef.h>

#define XS_ID_LEN 32
#define XS_OWNER_MAX 200
#define XS_NAME_MAX 255
#define XS_TOPS 32
#define XS_SESSIONS 128
#define XS_PER_OWNER 16
#define XS_FILE_MAX (256 * 1024)
#define XS_COORD 100000                 /* coordinates are clamped to ±this, sizes to (0, this] */

typedef struct { char name[XS_NAME_MAX + 1]; int r[4]; int max; } XsTop;
typedef struct {
    char id[XS_ID_LEN + 1];
    char owner[XS_OWNER_MAX];
    long long used;
    XsTop top[XS_TOPS];
    int ntop;
    int persist;                        /* has an owner: saved to the file */
    int inuse;                          /* an app holds it now (xsession.c): never evicted */
} XsSess;
typedef struct { XsSess *s[XS_SESSIONS]; int n; } XsStore;

int  xs_id_ok(const char *id);          /* 32 lowercase hex characters */
int  xs_owner_ok(const char *owner);    /* a known kind, printable, short enough */
int  xs_name_ok(const char *name);      /* storable: at most XS_NAME_MAX bytes, no control characters */
void xs_new_id(const unsigned char rnd[16], char out[XS_ID_LEN + 1]);

XsSess *xs_find(XsStore *st, const char *id, const char *owner);   /* NULL for no owner */
/* A new session (id from xs_new_id), evicting the least recently used ones not in use past the limits. NULL if
 * every slot is in use or no memory. owner "" makes one that is never saved nor found. */
XsSess *xs_new(XsStore *st, const char *id, const char *owner, long long now);
void xs_drop(XsStore *st, XsSess *s);
void xs_clear(XsStore *st);

XsTop *xs_top(XsSess *s, const char *name);
XsTop *xs_top_put(XsSess *s, const char *name);   /* found or added (empty: r all 0); NULL if not storable or full */
void   xs_top_del(XsSess *s, const char *name);
void   xs_top_set(XsTop *t, const int r[4], int max);   /* clamped */
int    xs_top_has(const XsTop *t);                  /* a place was ever stored */
/* The window's place now, into its entry: 1 if the entry changed. A normal window: its rect, not maximized. A maximized
 * one keeps its last normal place and is marked maximized; a fullscreen one keeps the entry as it was. Either, when it
 * has no place yet (it was maximized or fullscreen since it opened), takes its rect as that place, so it can still
 * be restored: without one the entry would be neither restored (xs_top_has) nor saved (xs_dump). */
int    xs_top_note(XsTop *t, const int r[4], int maximized, int fullscreen);

char *xs_dump(const XsStore *st, size_t *len);      /* malloc'd file text (sessions with an owner only) */
int   xs_load(XsStore *st, const char *buf, size_t len);   /* sessions read; st is cleared first */

#endif
