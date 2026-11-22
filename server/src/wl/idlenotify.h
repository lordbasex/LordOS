/* idlenotify.h — ext_idle_notifier_v1 (ext-idle-notify-v1): an app that asks to be told when the person is away from the
 * computer, only with the person's permission (docs/WAYLAND.md, "Idle notifications"; approved by the maintainer
 * 2026-11-22). Pure, header only: tests/idlenotify_test.c checks it; wl.c speaks the protocol.
 *
 * Whose decision it is: an isolated LordOS app's id, or the identity lord-server checked for any other client
 * (wl/kbinhibit.h kbi_identity: "program:<path>" or "flatpak:<id>"); the answer is the "idle" permission in
 * permisos.json (sec/perms.c), asked once per app with the system-drawn prompt and changed in Settings → Privacy. Never
 * for Xwayland (it speaks for every X11 app), on the login screen, or for a client with no identity. A denied or
 * undecided app keeps its notification objects but is never told "idled" (hiding the global would break the clients
 * that bind it unconditionally). Idle is the time since the last activity lord-server counts for its own idle timers
 * (input, remote input, a surface's idle inhibitor or a portal inhibit: while one holds, nobody is idle). */
#ifndef LORD_WL_IDLENOTIFY_H
#define LORD_WL_IDLENOTIFY_H

/* What a new notification's app gets: never (no identity, Xwayland, the login screen, or the person said no), the
 * prompt (no decision yet), or notifications (allowed). state: the saved PermState (0 ask, 1 allow, 2 deny). */
enum { IDN_NEVER, IDN_PROMPT, IDN_GRANT };
static inline int idn_decide(int state, int xwayland, int greeter, int has_key) {
    if (xwayland || greeter || !has_key || state == 2) return IDN_NEVER;
    return state == 1 ? IDN_GRANT : IDN_PROMPT;
}

/* One notification: its timeout and whether "idled" was sent (and when). */
typedef struct { double timeout_ms, idled_at; int idled; } IdnState;
enum { IDN_NONE, IDN_IDLED, IDN_RESUMED };

/* At `now` (ms), with the last activity at `last`: what to send. Allowed only: an app that lost the permission while
 * idled is told "resumed" and nothing more. *wait (if not NULL) is lowered to the ms until this one is due. */
static inline int idn_step(IdnState *n, int allowed, double last, double now, double *wait) {
    if (n->idled) {
        if (!allowed || last > n->idled_at) { n->idled = 0; return IDN_RESUMED; }
        return IDN_NONE;
    }
    if (!allowed) return IDN_NONE;
    double due = last + n->timeout_ms;
    if (now >= due) { n->idled = 1; n->idled_at = now; return IDN_IDLED; }
    if (wait && (*wait < 0 || due - now < *wait)) *wait = due - now;
    return IDN_NONE;
}

#endif
