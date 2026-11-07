/* shake.h — shake the pointer to find it (Settings → Mouse & Trackpad, as in macOS): a few quick
 * left-right strokes make the cursor grow for a moment. Pure logic, no time source of its own: the
 * caller passes the time and the pointer's motion, and reads back how much to grow (0 = normal size,
 * 1 = the largest the cursor plane takes). Checked by build/shake-test. */
#ifndef LORD_SHAKE_H
#define LORD_SHAKE_H

#define SHAKE_STROKES 4         /* direction changes that make a shake ... */
#define SHAKE_WINDOW_MS 900     /* ... within this time */
#define SHAKE_STROKE_PX 60      /* each stroke at least this long (logical pixels) */
#define SHAKE_HOLD_MS 500       /* it stays grown this long after the last turn */
#define SHAKE_GROW_MS 120
#define SHAKE_SHRINK_MS 300

typedef struct {
    double turns[SHAKE_STROKES];    /* when the last direction changes were */
    int nturns;
    int dir;                        /* -1 left, 1 right, 0 none yet */
    double stroke;                  /* length of the current stroke */
    double shaken_at;               /* last time it counted as a shake (0 = never) */
    double level, level_t;          /* current growth (0..1) and when it was computed */
} Shake;

void   shake_reset(Shake *s);
/* The pointer moved by dx, dy (logical pixels) at now (ms). */
void   shake_motion(Shake *s, double now, double dx, double dy);
/* How much the cursor grows now: 0..1, eased. */
double shake_level(Shake *s, double now);
/* Milliseconds until the level changes again (a frame while it grows or shrinks), or -1. */
double shake_timeout(const Shake *s, double now);

#endif
