/* kbinhibit.h — a Linux app that takes the desktop's shortcuts while it has the keyboard
 * (zwp_keyboard_shortcuts_inhibit_manager_v1, wl.c; see kbinhibit.c and docs/WAYLAND.md).
 *
 * VMs, remote desktops and games ask for every key, ❖ and ⌥Tab included. LordOS asks the person once per
 * app, known by an identity it checks itself (kbi_identity); the answer is the "shortcuts" permission in permisos.json (sec/perms.c), which Settings → Privacy
 * lists and revokes. While an allowed inhibitor holds and its window has the keyboard, keys go to the app
 * instead of LordOS's shortcuts, except the few below that always stay LordOS's. ❖Esc turns it off for that
 * window and, pressed again, back on. Never for an isolated LordOS app or on the login screen. */
#ifndef LORD_WL_KBINHIBIT_H
#define LORD_WL_KBINHIBIT_H

#include <stddef.h>

#define KBI_ID_MAX 192                  /* "flatpak:<id>" or "program:<path>", with its NUL (perms.c's PERM_APP_MAX) */

/* Whose decision it is: who the client is, as the compositor checks it itself, never what the client says (its
 * app_id names nothing here: any app can set any app_id). Read once, when the client connects (wl.c's
 * ClientInfo), from procfd, its /proc/<pid> directory opened while the pid was surely its own, so a pid reused
 * later can't answer for it:
 *   - a Flatpak app (its root holds .flatpak-info, a regular file opened without following a link):
 *     "flatpak:<the [Application] name in it>"; a damaged or unreadable file names no one;
 *   - any other process in our root and in our mount, pid and user namespaces (dndg_proc_sandboxed):
 *     "program:<the program it runs>", its /proc/<pid>/exe resolved (realpath) and still that same file there;
 *   - anything else (another sandbox or container, a deleted or replaced program, other characters, too long):
 *     no one.
 * 1 if out holds one; 0: no identity, and such a client never takes the shortcuts (fail closed). */
int kbi_identity(int procfd, char *out, size_t n);
/* The /proc/<pid> directory (O_PATH) of the process at the other end of a Unix socket, surely that process: by the
 * socket's pidfd (SO_PEERPIDFD), checked still alive after the open so its pid wasn't reused in between; on a
 * kernel without it, by pid (SO_PEERCRED's, which the caller passes), as soon as the client connects. -1 if gone,
 * or if the pidfd and the pid disagree. */
int kbi_peer_proc(int sock, int pid);
/* .flatpak-info's text (a key file) → the name= of its [Application] group, if it is a valid app id. 1, or 0. */
/* A client that connected through a security context (wp_security_context_v1, secctx.c): its identity is the app id
 * the sandbox engine gave the context, a checked one, since only a client outside any sandbox may create a context.
 * Only Flatpak's ("org.flatpak") names one: "flatpak:<app_id>". checked is what kbi_identity made of the client's
 * process ("" if nothing): when it names another app, no one. 1 if out holds one; 0: no identity (fail closed). */
int kbi_context_identity(const char *engine, const char *app_id, const char *checked, char *out, size_t n);
int kbi_flatpak_name(const char *text, size_t len, char *out, size_t n);
/* [Instance] instance-id= of a .flatpak-info text (digits only, at most 20): 1, or 0. */
int kbi_flatpak_instance(const char *text, size_t len, char *out, size_t n);
/* Is this a name kbi_identity could have made? (lord.perms.set takes one only for "shortcuts".) Names of the
 * first, never released format ("desktop:<app_id>", "exe:<path>") don't pass and are never looked up. */
int kbi_key_valid(const char *key);
/* A name's parts: 'f' (flatpak, *rest = the app id) or 'p' (program, *rest = the path); 0 if not valid. */
int kbi_key_kind(const char *key, const char **rest);

/* What happens to a new inhibitor: never (an isolated LordOS app or Xwayland, the login screen, a client with
 * no identity, or the person said no), ask the person, or hold right away. state: the saved PermState (0 ask, 1 allow,
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
