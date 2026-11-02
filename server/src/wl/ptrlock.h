/* ptrlock.h — when a pointer lock or confinement holds (zwp_pointer_constraints_v1, wl.c).
 *
 * A locked pointer is hidden and stops moving, so a constraint must only hold while the user can get out
 * of it: with the pointer on the surface AND the keyboard on its app. Without the second condition a
 * window in the background hides the pointer as it passes over, and nothing the user types reaches
 * the app that could let it go. ❖Esc always lets go (wl_key).
 *
 * `spent`: the constraint must not hold again (a oneshot that was released, or ❖Esc). */
#ifndef LORD_WL_PTRLOCK_H
#define LORD_WL_PTRLOCK_H

enum { PTRLOCK_SAME, PTRLOCK_ON, PTRLOCK_OFF };

/* What to tell the app now. Updates *active, and *spent when a oneshot ends. */
static inline int ptrlock_step(int *active, int *spent, int oneshot, int pointer_on, int keyboard_on) {
    int want = pointer_on && keyboard_on && !*spent;
    if (want == *active) return PTRLOCK_SAME;
    *active = want;
    if (!want && oneshot) *spent = 1;
    return want ? PTRLOCK_ON : PTRLOCK_OFF;
}

/* ❖Esc: released, and not again until the app asks for a new constraint. */
static inline int ptrlock_escape(int *active, int *spent) {
    int was = *active;
    *active = 0;
    *spent = 1;
    return was ? PTRLOCK_OFF : PTRLOCK_SAME;
}

#endif
