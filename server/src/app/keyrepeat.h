/* keyrepeat.h — the repeat of a held key in an isolated app (lord-server --app, appclient.c).
 *
 * The compositor sends Wayland clients only the press and the release of a key, never the kernel's repeats: each
 * client repeats on its own with the rate and the delay of wl_keyboard.repeat_info (wl.c). An isolated app runs as
 * such a client, so it makes the repeats itself and hands them to the JS keymap as value 2, as the kernel's reach an
 * in-process app. A press of a key that repeats (not a modifier: keyboard.c kb_key_repeats) starts it after `delay`
 * ms, then one every 1000/rate ms; its release, another press or the keyboard leaving the window ends it. A rate of 0
 * turns repeating off (the protocol's meaning). An app that stalls catches up with at most one repeat per call, so
 * a long frame doesn't release a burst of them. Times are CLOCK_MONOTONIC milliseconds.
 * Pure: tests/keyrepeat_test.c. */
#ifndef LORD_APP_KEYREPEAT_H
#define LORD_APP_KEYREPEAT_H

#define KEYREPEAT_RATE  25      /* until the compositor's repeat_info arrives */
#define KEYREPEAT_DELAY 400

typedef struct { int rate, delay, code; double next; } KeyRepeat;

static inline void keyrepeat_init(KeyRepeat *r) { r->rate = KEYREPEAT_RATE; r->delay = KEYREPEAT_DELAY; r->code = -1; r->next = 0; }

static inline void keyrepeat_stop(KeyRepeat *r) { r->code = -1; }

/* wl_keyboard.repeat_info: repeats per second (0 none) and the ms before the first */
static inline void keyrepeat_info(KeyRepeat *r, int rate, int delay) {
    r->rate = rate <= 0 ? 0 : rate > 100 ? 100 : rate;
    r->delay = delay > 0 && delay < 5000 ? delay : KEYREPEAT_DELAY;
    if (!r->rate) keyrepeat_stop(r);
}

/* a key went down (repeats: the keymap says it repeats) or up */
static inline void keyrepeat_key(KeyRepeat *r, int code, int pressed, int repeats, double now) {
    if (!pressed) { if (code == r->code) keyrepeat_stop(r); return; }
    if (repeats && r->rate > 0) { r->code = code; r->next = now + r->delay; }
    else keyrepeat_stop(r);
}

/* ms until the next repeat (0: due now), -1 when no key repeats */
static inline int keyrepeat_wait(const KeyRepeat *r, double now) {
    if (r->code < 0) return -1;
    double d = r->next - now;
    return d <= 0 ? 0 : (int)(d + 0.999);
}

/* the key to repeat now (-1 none); schedules the next one */
static inline int keyrepeat_due(KeyRepeat *r, double now) {
    if (r->code < 0 || now < r->next) return -1;
    double step = 1000.0 / r->rate;
    r->next += step;
    if (r->next <= now) r->next = now + step;      /* stalled: one now, the rest dropped */
    return r->code;
}

#endif
