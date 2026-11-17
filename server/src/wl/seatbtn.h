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

/* A start_drag's serial against the press it must name (issue #177): one of the serials that press was sent with
 * (one per wl_pointer of the client), or a newer serial that is not newer than the newest the display gave out.
 * GTK 3 sends the newest serial it has seen, not its press's: after a click that focused the window that is the
 * keyboard's enter or modifiers, which come after the button (the theme focuses the window when it sees the
 * press), so Firefox's first drag in a window it had just been clicked into was refused. wl.c asks this only
 * while that client's press is still held on the window it names. Serials wrap: "newer" is by signed difference. */
static inline int sb_serial_ok(uint32_t serial, const uint32_t *press, int npress, uint32_t newest) {
    if (npress <= 0) return 0;
    uint32_t first = press[0];
    for (int i = 0; i < npress; i++) {
        if (press[i] == serial) return 1;
        if ((int32_t)(press[i] - first) < 0) first = press[i];
    }
    return (int32_t)(serial - first) > 0 && (int32_t)(newest - serial) >= 0;
}

#endif
