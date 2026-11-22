/* pointerlock.h — pointer lock for LordOS's own windows (Machines' "Capture mouse", docs/VIRTUALIZATION.md 6.25; any app or
 * game that wants the mouse's motion rather than a position, lord.pointer.lock).
 *
 * While a lock is held the pointer stays at the centre of its owner's node, and nothing the owner says moves it: hit
 * tests, clicks and the display it's on all use that point. The host's pointer is hidden unless the owner shows one
 * (ptrlock_show: a machine's guest has its own pointer somewhere in the picture); then only its image is drawn there,
 * kept inside the node (ptrlock_cursor), and the real pointer stays at the centre. The mouse's relative motion (after
 * Settings → Mouse's speed, before any clamping to the screen) goes to the owner's sink instead of moving the pointer,
 * so it never stops at a screen's edge. With a button sink, buttons and the wheel go straight to the owner too, with no
 * hit test at all (a machine's guest can't steer a click anywhere); without one they go to the centre as usual.
 *
 * It ends when its owner ends it, when ❖Esc or Ctrl+Alt is pressed (main.c's on_input asks ptrlock_key before any
 * JavaScript, so no app can keep it), when the owner's node is gone or has no size for half a second (a window rebuilt
 * names its new node with ptrlock_retarget meanwhile), and when the session loses its
 * input (another console, the display detached). One lock at a time: a new one ends the old one first. Wayland clients
 * have their own (zwp_pointer_constraints_v1, wl.c); both are never held at once (main.c gives this one precedence). */
#ifndef LORD_POINTERLOCK_H
#define LORD_POINTERLOCK_H

/* why a lock ended (the sink's ended callback, lord.pointer's "lockend" event) */
enum { PTRLOCK_END_OWNER = 0, PTRLOCK_END_KEYS, PTRLOCK_END_GONE, PTRLOCK_END_INPUT, PTRLOCK_END_REPLACED };

typedef struct {
    /* the motion, in whole pixels (fractions are carried to the next call); NULL: kept for ptrlock_take (JavaScript) */
    void (*motion)(void *ud, int dx, int dy);
    void (*ended)(void *ud, int why);   /* may be NULL; called once, after the lock is already gone */
    /* may be NULL (then buttons and wheel go to the node's centre): button in the DOM's numbering, down 1/0; wheel steps,
     * dy + up, dx + right */
    void (*button)(void *ud, int button, int down);
    void (*wheel)(void *ud, int dy, int dx);
    void *ud;
} PtrLockSink;

/* the node's box in logical screen coordinates (x, y, w, h); 0 when it is there and has a size. Set once by main.c */
typedef int (*PtrLockRect)(int node, float r[4]);
void ptrlock_init(PtrLockRect rect);

/* 0 the lock is held; -1 no such node, or it has no size */
int  ptrlock_begin(int node, const PtrLockSink *sink);
void ptrlock_end(int why);
/* the owner's node is now another one (its window was rebuilt, as on entering full screen); 0 done */
int  ptrlock_retarget(void *ud, int node);
/* ends the lock only when it's this sink's (ud) */
void ptrlock_end_owner(void *ud);
int  ptrlock_active(void);
int  ptrlock_node(void);
int  ptrlock_owned_by(void *ud);

/* the owner shows a pointer's image at (x, y) (logical screen coordinates, kept inside the node), or hides it again. It
 * never moves the real pointer (ptrlock_where) */
void ptrlock_show(int visible, float x, float y);
/* where the shown pointer's image is drawn, rounded and inside the node; 0 when none is shown (draw at the pointer) */
int  ptrlock_cursor(int *x, int *y);
int  ptrlock_hidden(void);              /* held, and the host's pointer isn't shown */

/* ---- main.c's input path ---- */
/* relative motion while held: to the sink */
void ptrlock_motion(double dx, double dy);
/* a key from a physical keyboard (EV_KEY code, value 1 down / 0 up / 2 repeat), with the modifiers as they are after it.
 * Returns 1 when the key ended the lock and must not go on (❖Esc's Esc); Ctrl+Alt ends it and goes on (0) */
int  ptrlock_key(int code, int value, int meta, int ctrl, int alt);
/* a button or wheel steps while held: 1 when the owner's sink took it (it goes nowhere else) */
int  ptrlock_button(int button, int down);
int  ptrlock_wheel(int dy, int dx);
int  ptrlock_wants_wheel(void);
/* where the pointer stays now (the node's centre), rounded; 0 when no lock is held or its node is gone (the lock ends
 * once it has been gone for PTRLOCK_GONE_MS) */
#define PTRLOCK_GONE_MS 500
int  ptrlock_where(int *x, int *y);
int  ptrlock_waiting(void);             /* its node is gone and the lock waits for a new one: ask again soon */
/* changes whenever the lock starts, ends, moves or shows/hides its pointer: main.c redraws the cursor then */
unsigned ptrlock_serial(void);
/* the motion gathered for a lock without a motion sink (JavaScript's, sent once a frame); 0 when there's none */
int  ptrlock_take(int *dx, int *dy);
/* the last lock that ended, whoever held it, once: its node and why (JavaScript hears it as "lockend": the shell learns
 * that ❖ of ❖Esc wasn't pressed alone); 0 when none ended since */
int  ptrlock_take_end(int *node, int *why);

#endif
