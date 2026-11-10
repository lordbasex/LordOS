/* perms.h — per-app permissions with system-drawn prompts (see perms.c and docs/PRIVACY.md).
 *
 * Each permission of each app has a state: ask (the first time), allowed or denied.
 * Decisions are stored per user in ~/.config/lordos/permisos.json, a file that
 * apps can NOT write (it's on lord.fs's deny list, like settings.json). The
 * broker (and, while apps run in the same context, the system SDK) looks it up here:
 * the lookup is O(1) over an in-memory table. */
#ifndef LORD_SEC_PERMS_H
#define LORD_SEC_PERMS_H

#include <stddef.h>

/* The permissions an app can declare and request. The order is perms.c's. */
typedef enum {
    PERM_FILES = 0,     /* open/save through the system dialog (powerbox) */
    PERM_CLIPBOARD,     /* read the clipboard */
    PERM_NOTIFICATIONS,
    PERM_NETWORK,
    PERM_CAMERA,
    PERM_MICROPHONE,
    PERM_SCREEN,        /* screen capture */
    PERM_LISTEN,        /* open ports to listen on */
    PERM_USB,
    PERM_BLUETOOTH,
    PERM_APPS,          /* launch other apps */
    PERM_STATUS,        /* an item by the clock (sec/statusitem.c) */
    PERM_SHORTCUTS,     /* take ❖ and ⌥Tab while in front: a Linux app or a LordOS window, never a manifest's (wl/kbinhibit.h) */
    PERM_COUNT,
} Perm;

/* An app's id with its NUL: an .lpk id, or for "shortcuts" a Linux app's checked identity, "flatpak:<id>" or
 * "program:<path>" (wl/kbinhibit.h's KBI_ID_MAX) */
#define PERM_APP_MAX 192

typedef enum { PERM_ASK = 0, PERM_ALLOW = 1, PERM_DENY = 2 } PermState;

const char *perm_name(Perm p);              /* "microphone", "camera"... */
int         perm_from_name(const char *s);  /* -1 if it doesn't exist */
int         perm_declarable(const char *s); /* can it be declared in the manifest? */

/* Saved state (O(1)); PERM_ASK if there's no decision. */
PermState perm_get(const char *app_id, Perm p);
/* Saves a decision (and persists it in permisos.json). Revoking = PERM_ASK or PERM_DENY. */
int perm_set(const char *app_id, Perm p, PermState state);
/* Forgets all of an app's permissions (on uninstall). */
void perm_forget(const char *app_id);

/* For the Privacy page: walks the saved decisions. Returns the count written. */
typedef struct { char app[PERM_APP_MAX]; Perm perm; PermState state; long long when; } PermEntry;
int perm_list(PermEntry *out, int max);

/* Loads permisos.json (once). The path is added to lord.fs's deny list. */
void perm_init(const char *home);
const char *perm_file_suffix(void);         /* "/.config/lordos/permisos.json" for the deny list */

#endif
