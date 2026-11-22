/* pointerlock.c — pointer lock for LordOS's own windows (pointerlock.h) */
#include "pointerlock.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stddef.h>
#include <time.h>

typedef struct {
    int on, node;
    PtrLockSink sink;
    int shown;                  /* the owner shows a pointer at (sx, sy) */
    float sx, sy;
    double fx, fy;              /* fractions of a pixel not sent yet */
    int jx, jy, jpend;          /* motion for a lock without a motion sink */
    long long gone_at;          /* when its node was first found gone (0: it's there) */
    unsigned serial;
} Lock;
static Lock L;
static int end_node, end_why = -1;      /* the last end, for ptrlock_take_end */
static PtrLockRect rect_of;

void ptrlock_init(PtrLockRect rect) { rect_of = rect; }

static int node_rect(int node, float r[4]) {
    return rect_of && !rect_of(node, r) && r[2] >= 1 && r[3] >= 1 ? 0 : -1;
}

int ptrlock_begin(int node, const PtrLockSink *sink) {
    float r[4];
    if (node <= 0 || !sink || node_rect(node, r)) return -1;
    if (L.on) ptrlock_end(PTRLOCK_END_REPLACED);
    unsigned serial = L.serial;
    L = (Lock){ 0 };
    L.serial = serial + 1;
    L.on = 1;
    L.node = node;
    L.sink = *sink;
    return 0;
}

void ptrlock_end(int why) {
    if (!L.on) return;
    PtrLockSink s = L.sink;
    unsigned serial = L.serial;
    end_node = L.node;
    end_why = why;
    L = (Lock){ 0 };
    L.serial = serial + 1;
    if (s.ended) s.ended(s.ud, why);
}

static long long mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int ptrlock_retarget(void *ud, int node) {
    float r[4];
    if (!L.on || L.sink.ud != ud || node <= 0 || node_rect(node, r)) return -1;
    if (node != L.node) { L.node = node; L.serial++; }
    L.gone_at = 0;
    return 0;
}

void ptrlock_end_owner(void *ud) { if (L.on && L.sink.ud == ud) ptrlock_end(PTRLOCK_END_OWNER); }
int ptrlock_active(void) { return L.on; }
int ptrlock_node(void) { return L.on ? L.node : 0; }
int ptrlock_owned_by(void *ud) { return L.on && L.sink.ud == ud; }

void ptrlock_show(int visible, float x, float y) {
    if (!L.on) return;
    visible = visible ? 1 : 0;
    if (visible == L.shown && (!visible || (fabsf(x - L.sx) < 0.5f && fabsf(y - L.sy) < 0.5f))) return;
    L.shown = visible;
    if (visible) { L.sx = x; L.sy = y; }
    L.serial++;
}

int ptrlock_hidden(void) { return L.on && !L.shown; }

void ptrlock_motion(double dx, double dy) {
    if (!L.on || (dx == 0 && dy == 0)) return;
    L.fx += dx;
    L.fy += dy;
    int ix = (int)L.fx, iy = (int)L.fy;
    L.fx -= ix;
    L.fy -= iy;
    if (!ix && !iy) return;
    if (L.sink.motion) { L.sink.motion(L.sink.ud, ix, iy); return; }
    /* JavaScript's: kept until the frame sends it, within what an int holds */
    long nx = (long)L.jx + ix, ny = (long)L.jy + iy;
    L.jx = nx > 1000000 ? 1000000 : nx < -1000000 ? -1000000 : (int)nx;
    L.jy = ny > 1000000 ? 1000000 : ny < -1000000 ? -1000000 : (int)ny;
    L.jpend = 1;
}

int ptrlock_key(int code, int value, int meta, int ctrl, int alt) {
    if (!L.on || value != 1) return 0;
    if (code == KEY_ESC && meta) { ptrlock_end(PTRLOCK_END_KEYS); return 1; }
    int mod = code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL || code == KEY_LEFTALT || code == KEY_RIGHTALT;
    if (mod && ctrl && alt) ptrlock_end(PTRLOCK_END_KEYS);
    return 0;
}

int ptrlock_where(int *x, int *y) {
    if (!L.on) return 0;
    float r[4];
    if (node_rect(L.node, r)) {
        long long now = mono_ms();
        if (!L.gone_at) L.gone_at = now;
        else if (now - L.gone_at >= PTRLOCK_GONE_MS) ptrlock_end(PTRLOCK_END_GONE);
        return 0;
    }
    L.gone_at = 0;
    *x = (int)lroundf(r[0] + r[2] / 2);
    *y = (int)lroundf(r[1] + r[3] / 2);
    return 1;
}

int ptrlock_cursor(int *x, int *y) {
    float r[4];
    if (!L.on || !L.shown || node_rect(L.node, r)) return 0;
    /* inside the node, a pixel from its far edges */
    float px = L.sx < r[0] ? r[0] : L.sx > r[0] + r[2] - 1 ? r[0] + r[2] - 1 : L.sx;
    float py = L.sy < r[1] ? r[1] : L.sy > r[1] + r[3] - 1 ? r[1] + r[3] - 1 : L.sy;
    *x = (int)lroundf(px);
    *y = (int)lroundf(py);
    return 1;
}

int ptrlock_button(int button, int down) {
    if (!L.on || !L.sink.button) return 0;
    L.sink.button(L.sink.ud, button, down);
    return 1;
}
int ptrlock_wants_wheel(void) { return L.on && L.sink.wheel != NULL; }
int ptrlock_wheel(int dy, int dx) {
    if (!ptrlock_wants_wheel()) return 0;
    if (dy || dx) L.sink.wheel(L.sink.ud, dy, dx);
    return 1;
}

unsigned ptrlock_serial(void) { return L.serial; }
int ptrlock_waiting(void) { return L.on && L.gone_at != 0; }

int ptrlock_take_end(int *node, int *why) {
    if (end_why < 0) return 0;
    *node = end_node;
    *why = end_why;
    end_why = -1;
    return 1;
}

int ptrlock_take(int *dx, int *dy) {
    if (!L.on || !L.jpend) return 0;
    *dx = L.jx;
    *dy = L.jy;
    L.jx = L.jy = L.jpend = 0;
    return 1;
}
