/* handsmotion.h — how the agent's cursor moves in Hands' human mode (docs/HANDS.md "The agent's cursor"): the pure math,
 * with no compositor, so tests/hands_test.c checks it alone.
 *
 *   - a glide from where the cursor is to the target, along a slightly curved path (a quadratic Bézier whose control
 *     point is pushed sideways by a share of the distance), eased in and out;
 *   - its duration by Fitts's law: longer for a far or small target, between HANDS_GLIDE_MIN and HANDS_GLIDE_MAX ms; with
 *     reduce motion (Settings → Accessibility) a short glide of at most HANDS_GLIDE_REDUCED ms;
 *   - typing at a person's pace: 25 to 60 ms a character with a small jitter, a little longer after a space or a
 *     punctuation mark, and never more than HANDS_TYPE_BUDGET ms for the whole text (a long text goes faster);
 *   - a scroll in a few steps.
 * The randomness is a seeded xorshift: the same seed gives the same motion (the tests). */
#ifndef LORD_HANDS_HANDSMOTION_H
#define LORD_HANDS_HANDSMOTION_H

#include <stddef.h>
#include <stdint.h>

#define HANDS_GLIDE_MIN     250
#define HANDS_GLIDE_MAX     900
#define HANDS_GLIDE_REDUCED 120     /* reduce motion: a short glide */
#define HANDS_HOVER_MIN     90      /* the pause over the target before a click */
#define HANDS_HOVER_MAX     170
#define HANDS_TYPE_MIN      25      /* ms a character */
#define HANDS_TYPE_MAX      60
#define HANDS_TYPE_BUDGET   10000   /* the most a Type call may take: a long text is typed faster */
#define HANDS_SCROLL_STEP   70      /* ms between the steps of a scroll */
#define HANDS_SCROLL_STEPS  4       /* at most */

typedef struct {
    double x0, y0, x1, y1;          /* from, to */
    double cx, cy;                  /* the control point: the curve's bend */
    double ms;                      /* its duration */
} HandsGlide;

uint32_t hands_rand(uint32_t *seed);                    /* xorshift32; a 0 seed is taken as 1 */
double hands_ease(double t);                            /* ease-in-out (cubic) on [0, 1] */
/* Fitts's law: the time to reach a target of `width` points at `dist` points, clamped to [MIN, MAX] (or REDUCED with
 * reduce motion); 0 when the cursor is already there */
double hands_fitts_ms(double dist, double width, int reduce);
/* a glide from (x0, y0) to (x1, y1) for a target `width` wide; the bend's side comes from the seed */
void hands_glide_make(HandsGlide *g, double x0, double y0, double x1, double y1, double width, int reduce, uint32_t *seed);
/* where the cursor is at `ms` since the glide began (the end once ms >= g->ms); 1 when it arrived */
int  hands_glide_at(const HandsGlide *g, double ms, double *x, double *y);
/* the pause over the target before a click (shorter with reduce motion) */
double hands_hover_ms(int reduce, uint32_t *seed);
/* the wait before the character after `prev` (its first byte), for a text of `chars` characters */
double hands_type_ms(unsigned char prev, size_t chars, uint32_t *seed);
/* a scroll of `amount` notches in steps: their sizes into steps[HANDS_SCROLL_STEPS], their count returned */
int  hands_scroll_steps(int amount, int *steps);

#endif
