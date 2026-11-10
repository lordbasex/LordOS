/* kbinhibit.h — a Linux app that takes the desktop's shortcuts while it has the keyboard
 * (zwp_keyboard_shortcuts_inhibit_manager_v1, wl.c; see kbinhibit.c and docs/WAYLAND.md).
 *
 * VMs, remote desktops and games ask for every key, ❖ and ⌥Tab included. LordOS asks the person once per
 * app; the answer is the "shortcuts" permission in permisos.json (sec/perms.c), which Settings → Privacy
 * lists and revokes. While an allowed inhibitor holds and its window has the keyboard, keys go to the app
 * instead of LordOS's shortcuts, except the few below that always stay LordOS's. ❖Esc turns it off for that
 * window and, pressed again, back on. Never for an isolated LordOS app or on the login screen. */
#ifndef LORD_WL_KBINHIBIT_H
#define LORD_WL_KBINHIBIT_H

#include <stddef.h>

#define KBI_ID_MAX 96                   /* "desktop:<app_id>" or "exe:<path>", with its NUL (perms.c's PERM_APP_MAX) */

/* The app a decision belongs to: "desktop:<app_id>" when the window has a usable app_id, otherwise
 * "exe:<program>" (absolute, no "..", plain characters). 1 if out holds one; 0: nothing to ask for. */
int kbi_app_key(const char *app_id, const char *exe, char *out, size_t n);
/* Is this a key kbi_app_key could have made? (lord.perms.set takes one only for "shortcuts".) */
int kbi_key_valid(const char *key);

/* What happens to a new inhibitor: never (an isolated LordOS app, the login screen, no app to name, or
 * the person said no), ask the person, or hold right away. state: the saved PermState (0 ask, 1 allow,
 * 2 deny). */
enum { KBI_NEVER, KBI_PROMPT, KBI_GRANT };
int kbi_decide(int state, int isolated, int greeter, int has_key);

/* One inhibitor's state: allowed (the saved decision) and off (❖Esc turned it off for that window).
 * active is what the app was last told: it holds while allowed and not off. */
typedef struct { int allowed, off, active; } KbiState;
enum { KBI_SAME, KBI_ACTIVE, KBI_INACTIVE };
/* The decision changed (asked, allowed, revoked): what to tell the app. */
int kbi_set_allowed(KbiState *k, int allowed);
/* ❖Esc in front of an allowed inhibitor's window: off, or back on. */
int kbi_toggle(KbiState *k);

/* Where a key press goes in front of an allowed inhibitor's window. mods: keygrab.h's KG_* bits; letter:
 * keygrab_letter's. KBI_KEY_TOGGLE: ❖Esc; KBI_KEY_LORD: as if there were no inhibitor (lock, media and
 * light keys, Ctrl+Alt+F1…F12; and every key while it is off); KBI_KEY_APP: the app gets it, ❖ shortcuts
 * and ⌥Tab included. A release goes where its press went (wl.c). */
enum { KBI_KEY_LORD, KBI_KEY_APP, KBI_KEY_TOGGLE };
int kbi_route(int code, char letter, unsigned mods, int active);

#endif
