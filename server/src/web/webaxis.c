/* webaxis.c — wl_pointer's axis events to libwpe's (see webaxis.h). */
#include "webaxis.h"

#include <string.h>

static void clear_frame(WebAxis *a) {
    int r0 = a->rem120[0], r1 = a->rem120[1];
    memset(a, 0, sizeof *a);
    a->source = WEBAXIS_SRC_NONE;
    a->rem120[0] = r0;
    a->rem120[1] = r1;
}

void webaxis_reset(WebAxis *a) {
    memset(a, 0, sizeof *a);
    a->source = WEBAXIS_SRC_NONE;
}

void webaxis_source(WebAxis *a, uint32_t s) { a->source = s <= WEBAXIS_SRC_TILT ? (int)s : WEBAXIS_SRC_NONE; }
void webaxis_value(WebAxis *a, uint32_t axis, double v) { if (axis <= 1 && v == v) a->px[axis] += v; }
void webaxis_discrete(WebAxis *a, uint32_t axis, int32_t steps) {
    if (axis > 1 || steps < -1000 || steps > 1000) return;
    a->gotdisc = 1;
    a->disc[axis] += steps;
}
void webaxis_value120(WebAxis *a, uint32_t axis, int32_t v) {
    if (axis > 1 || v < -120000 || v > 120000) return;
    a->got120 = 1;
    a->v120[axis] += v;
}
void webaxis_stop(WebAxis *a, uint32_t axis) { if (axis <= 1) a->stop[axis] = 1; }

static int clamp_lines(int n) { return n > WEBAXIS_LINES_MAX ? WEBAXIS_LINES_MAX : n < -WEBAXIS_LINES_MAX ? -WEBAXIS_LINES_MAX : n; }
static double flip(double v, double scale) { return v != 0 ? -v * scale : 0; }     /* never a -0 */

int webaxis_frame(WebAxis *a, double scale, WebAxisOut out[2]) {
    int n = 0;
    if (!(scale > 0 && scale <= 8)) scale = 1;
    int smooth = a->source == WEBAXIS_SRC_FINGER || a->source == WEBAXIS_SRC_CONTINUOUS;
    if (!smooth && (a->got120 || a->gotdisc)) {
        int lines[2];
        for (int i = 0; i < 2; i++) {
            int v = a->got120 ? a->v120[i] : a->disc[i] * 120;     /* axis_value120 replaces axis_discrete */
            /* a partial notch the other way doesn't add to the one before */
            if ((v > 0 && a->rem120[i] < 0) || (v < 0 && a->rem120[i] > 0)) a->rem120[i] = 0;
            a->rem120[i] += v;
            lines[i] = a->rem120[i] / 120;
            a->rem120[i] -= lines[i] * 120;
            lines[i] = clamp_lines(lines[i]);
        }
        if (lines[0] || lines[1]) out[n++] = (WebAxisOut){ WEBAXIS_OUT_LINES, lines[WEBAXIS_H] ? -lines[WEBAXIS_H] : 0, lines[WEBAXIS_V] ? -lines[WEBAXIS_V] : 0 };
    } else if ((a->source == WEBAXIS_SRC_WHEEL || a->source == WEBAXIS_SRC_TILT) && (a->px[0] != 0 || a->px[1] != 0)) {
        /* a wheel that doesn't count its notches: a line per axis that moved */
        out[n++] = (WebAxisOut){ WEBAXIS_OUT_LINES, a->px[WEBAXIS_H] > 0 ? -1 : a->px[WEBAXIS_H] < 0 ? 1 : 0, a->px[WEBAXIS_V] > 0 ? -1 : a->px[WEBAXIS_V] < 0 ? 1 : 0 };
    } else if (a->px[0] != 0 || a->px[1] != 0) {
        /* a touchpad, or a compositor that says nothing about the source: pixels */
        out[n++] = (WebAxisOut){ WEBAXIS_OUT_PIXELS, flip(a->px[WEBAXIS_H], scale), flip(a->px[WEBAXIS_V], scale) };
    }
    if (smooth && (a->stop[0] || a->stop[1])) out[n++] = (WebAxisOut){ WEBAXIS_OUT_END, 0, 0 };
    clear_frame(a);
    return n;
}
