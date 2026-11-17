/* seatbtn_test.c — the seat's buttons as drag and drop sees them (src/wl/seatbtn.h; issue #176). */
#include "wl/seatbtn.h"

#include <stdio.h>

static int fails, total;

static void is(long got, long want, const char *what) {
    total++;
    if (got != want) { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}

/* one event; 1 if it ends a drag started with drag_button */
static int ev(SeatBtn *s, int drag_button, int button, int down) {
    sb_event(s, button, down);
    return sb_drag_released(s, drag_button, button, down);
}

int main(void) {
    /* the plain case: press left, drag, release left */
    SeatBtn s = { 0, -1 };
    is(sb_any(&s), 0, "nothing held at first");
    is(ev(&s, -1, 0, 1), 0, "a press never drops");
    is(s.last, 0, "the press is the left button's");
    is(ev(&s, s.last, 0, 0), 1, "its release drops");
    is(sb_any(&s), 0, "nothing held after it");

    /* a release that never arrived (the issue): with a count the seat stayed "held" for good and no later drag
     * ended at its release; as a set, the next press and release of that button put it right */
    s = (SeatBtn){ 0, -1 };
    sb_event(&s, 0, 1);                 /* its release is lost */
    is(ev(&s, -1, 0, 1), 0, "the next press");
    is(ev(&s, 0, 0, 0), 1, "the drag ends at the release");
    is(sb_any(&s), 0, "and the seat is clear again");

    /* the same press reported twice (two devices with one button): one release clears it */
    s = (SeatBtn){ 0, -1 };
    sb_event(&s, 0, 1);
    sb_event(&s, 0, 1);
    is(ev(&s, 0, 0, 0), 1, "a doubled press: its one release drops");

    /* another button stuck down (its release lost): the drag's own button still drops */
    s = (SeatBtn){ 0, -1 };
    sb_event(&s, 2, 1);                 /* right, never released */
    sb_event(&s, 0, 1);
    int b = s.last;
    is(b, 0, "the drag's button is the newest press");
    is(ev(&s, b, 0, 0), 1, "left's release drops although right looks held");

    /* a second button pressed and released mid-drag does not drop; the drag's own release does */
    s = (SeatBtn){ 0, -1 };
    sb_event(&s, 0, 1);
    b = s.last;
    is(ev(&s, b, 2, 1), 0, "right pressed mid-drag");
    is(ev(&s, b, 2, 0), 0, "right released mid-drag, left still held");
    is(ev(&s, b, 0, 0), 1, "left released");

    /* the drag's button released first while another stays down: it drops */
    s = (SeatBtn){ 0, -1 };
    sb_event(&s, 0, 1);
    b = s.last;
    sb_event(&s, 1, 1);
    is(ev(&s, b, 0, 0), 1, "left released with middle held");

    /* buttons outside 0..31 are never held */
    s = (SeatBtn){ 0, -1 };
    sb_event(&s, 40, 1);
    sb_event(&s, -1, 1);
    is(sb_any(&s), 0, "out-of-range buttons hold nothing");

    printf("seatbtn: %d/%d ok\n", total - fails, total);
    return fails != 0;
}
