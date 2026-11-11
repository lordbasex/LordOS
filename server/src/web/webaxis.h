/* webaxis.h — the wheel and the touchpad, from wl_pointer's axis events to libwpe's (docs/WEBAPPS.md, "Input").
 * Pure: no Wayland or WebKit here (tests/webapp_test.c); webwin.c feeds it the events of one wl_pointer.frame and
 * turns what webaxis_frame gives into libwpe axis events.
 *
 *   - a mouse wheel (axis_value120, or axis_discrete before wl_seat 8): whole notches, one line event per notch and
 *     axis; a high-resolution wheel's partial notches add up across frames until they make one. Only the axis that
 *     moved gets an event: WebKit 2.48 reads a line event's 2D values by their sign (copysign), so a 0 on the other
 *     axis would scroll it a line too, and the page then scrolls diagonally or not at all.
 *   - a touchpad (axis_source finger or continuous): pixels, times the scale (libwpe's coordinates are the buffer's),
 *     with an exact 0 on the axis that didn't move; axis_stop ends the gesture (a 2D event of 0, 0: WebKit's phase
 *     "ended", where kinetic scrolling starts).
 *   - a wheel with no notch count (a compositor before wl_pointer 5): one line per axis that moved.
 * Wayland's positive is down and right; libwpe's is up and left: every value changes sign. */
#ifndef LORD_WEB_WEBAXIS_H
#define LORD_WEB_WEBAXIS_H

#include <stdint.h>

enum { WEBAXIS_V = 0, WEBAXIS_H = 1 };      /* wl_pointer's axis numbers, libwpe's too */
enum { WEBAXIS_SRC_NONE = -1, WEBAXIS_SRC_WHEEL = 0, WEBAXIS_SRC_FINGER = 1, WEBAXIS_SRC_CONTINUOUS = 2, WEBAXIS_SRC_TILT = 3 };
enum { WEBAXIS_OUT_LINES = 1, WEBAXIS_OUT_PIXELS, WEBAXIS_OUT_END };
#define WEBAXIS_LINES_MAX 10                /* notches per axis and frame: more is a broken device */

typedef struct {
    int source;                             /* this frame's axis_source, or WEBAXIS_SRC_NONE */
    int got120, gotdisc;
    int v120[2], disc[2];                   /* this frame's axis_value120 and axis_discrete */
    double px[2];                           /* this frame's axis values, in surface pixels */
    int stop[2];
    int rem120[2];                          /* a high-resolution wheel's partial notch, kept between frames */
} WebAxis;

/* x and y in libwpe's sign: notches for LINES (whole numbers, at most WEBAXIS_LINES_MAX each), buffer pixels for
 * PIXELS, nothing for END */
typedef struct { int kind; double x, y; } WebAxisOut;

void webaxis_reset(WebAxis *a);
void webaxis_source(WebAxis *a, uint32_t source);
void webaxis_value(WebAxis *a, uint32_t axis, double v);
void webaxis_discrete(WebAxis *a, uint32_t axis, int32_t steps);
void webaxis_value120(WebAxis *a, uint32_t axis, int32_t v);
void webaxis_stop(WebAxis *a, uint32_t axis);
/* wl_pointer.frame: what to send (0..2 events into out), and the frame's fields cleared */
int  webaxis_frame(WebAxis *a, double scale, WebAxisOut out[2]);

#endif
