/* seatbtn.h — the seat's mouse buttons as wl.c's drag and drop sees them (wl.c: wl_pointer, dnd_pointer, dev_drag).
 *
 * Which buttons are held is a set, not a count: a press reported twice (two devices with the same button) or a
 * release that never arrived (a kernel buffer overrun, a VT or session switch while a button was down) leaves a
 * count one too high for good, and a drag that ends "when no button is held" then never ends at its release
 * (issue #176). As a set, the button's next release clears it. A drag also ends at the release of the button
 * that started it, whatever another button's state says (Weston does the same).
 */
#ifndef LORD_SEATBTN_H
#define LORD_SEATBTN_H

#include <stdint.h>

typedef struct {
    uint32_t held;                      /* DOM buttons held (bit n: button n; 0 left, 1 middle, 2 right, 3 back, 4 forward) */
    int last;                           /* the button of the newest press, -1 before any */
} SeatBtn;

static inline uint32_t sb_bit(int button) { return button >= 0 && button < 32 ? 1u << button : 0; }

/* One button event, in the order it came. */
static inline void sb_event(SeatBtn *s, int button, int down) {
    if (down) { s->held |= sb_bit(button); s->last = button; }
    else s->held &= ~sb_bit(button);
}
static inline int sb_any(const SeatBtn *s) { return s->held != 0; }

/* After sb_event for a release of `button`: does it end a drag started with `drag_button`? Its own button's
 * release does, and so does the release that left no button held. */
static inline int sb_drag_released(const SeatBtn *after, int drag_button, int button, int down) {
    if (down) return 0;
    return button == drag_button || !sb_any(after);
}

#endif
