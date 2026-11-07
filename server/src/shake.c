/* shake.c — shake the pointer to find it (see shake.h). A stroke is a run of horizontal motion in one
 * direction; when the direction changes after a stroke of SHAKE_STROKE_PX or more, that is a turn. With
 * SHAKE_STROKES turns inside SHAKE_WINDOW_MS the pointer is being shaken: the level rises to 1 in
 * SHAKE_GROW_MS, stays while the shaking goes on and SHAKE_HOLD_MS after, and falls in SHAKE_SHRINK_MS. */
#include "shake.h"

#include <math.h>
#include <string.h>

void shake_reset(Shake *s) { memset(s, 0, sizeof *s); }

void shake_motion(Shake *s, double now, double dx, double dy) {
    /* mostly vertical motion, or a pixel of jitter, doesn't make or break a stroke */
    if (fabs(dx) < 1 || fabs(dx) < fabs(dy)) return;
    int dir = dx > 0 ? 1 : -1;
    if (dir == s->dir) { s->stroke += fabs(dx); return; }
    if (s->dir && s->stroke >= SHAKE_STROKE_PX) {
        if (s->nturns == SHAKE_STROKES) memmove(s->turns, s->turns + 1, sizeof s->turns[0] * (SHAKE_STROKES - 1));
        else s->nturns++;
        s->turns[s->nturns - 1] = now;
        if (s->nturns == SHAKE_STROKES && now - s->turns[0] <= SHAKE_WINDOW_MS) {
            shake_level(s, now);                    /* the level so far, before the target changes */
            s->shaken_at = now;
        }
    } else if (s->dir) s->nturns = 0;               /* a short stroke: start counting again */
    s->dir = dir;
    s->stroke = fabs(dx);
}

static int growing(const Shake *s, double now) { return s->shaken_at > 0 && now - s->shaken_at < SHAKE_HOLD_MS; }

double shake_level(Shake *s, double now) {
    double dt = s->level_t > 0 ? now - s->level_t : 0;
    if (dt < 0) dt = 0;
    s->level_t = now;
    if (growing(s, now)) s->level += dt / SHAKE_GROW_MS;
    else s->level -= dt / SHAKE_SHRINK_MS;
    if (s->level > 1) s->level = 1;
    if (s->level < 0) s->level = 0;
    double l = s->level;
    return l * l * (3 - 2 * l);                     /* smoothstep */
}

double shake_timeout(const Shake *s, double now) {
    if (growing(s, now)) return s->level < 1 ? 16 : SHAKE_HOLD_MS - (now - s->shaken_at);
    return s->level > 0 ? 16 : -1;
}
