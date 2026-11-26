/* mloop.c — the loop cache's rules (mloop.h). Pure: no GPU, no I/O. */
#include "mloop.h"

#include <stdlib.h>
#include <string.h>

#define MB (1024LL * 1024)

int64_t mloop_budget(int64_t setting_mb, int64_t vram_total, int64_t vram_used, int64_t mem_total, int integrated) {
    if (setting_mb <= 0) return 0;
    int64_t b = setting_mb * MB;
    if (integrated || vram_total <= 0) {
        int64_t share = mem_total > 0 ? mem_total / 16 : 256 * MB;
        if (share > 1024 * MB) share = 1024 * MB;
        return b < share ? b : share;
    }
    int64_t half = vram_total / 2;
    if (b > half) b = half;
    if (vram_used >= 0) {
        int64_t head = vram_total / 10 > 256 * MB ? vram_total / 10 : 256 * MB;
        int64_t room = vram_total - vram_used - head;
        if (room < 0) room = 0;
        if (b > room) b = room;
    }
    return b;
}

int mloop_pressure(int64_t vram_total, int64_t vram_used, int64_t mem_avail, int64_t mem_total, int integrated) {
    if (integrated || vram_total <= 0) return mem_avail >= 0 && mem_total > 0 && mem_avail < mem_total / 10;
    if (vram_used < 0) return 0;
    int64_t head = vram_total / 10 > 256 * MB ? vram_total / 10 : 256 * MB;
    return vram_total - vram_used < head / 2;
}

int64_t mloop_frame_bytes(int w, int h) { return w > 0 && h > 0 ? (int64_t)w * h + (int64_t)w * (h / 2) : 0; }

int64_t mloop_estimate(int w, int h, int64_t dur_ms, int64_t interval_ms) {
    if (dur_ms <= 0 || interval_ms <= 0) return -1;
    return (dur_ms / interval_ms + 2) * mloop_frame_bytes(w, h);
}

void mloop_rec_reset(MLoopRec *r) {
    int64_t last = r->last, iv = r->interval;
    int seen = r->seen;
    memset(r, 0, sizeof *r);
    r->last = last;
    r->interval = iv;
    r->seen = seen;
}

static int cmp64(const void *a, const void *b) { int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return x < y ? -1 : x > y; }

static int64_t median_gap(const int64_t *p, int n) {
    if (n < 2) return 0;
    int k = n - 1 < 64 ? n - 1 : 64;
    int64_t g[64];
    for (int i = 0; i < k; i++) g[i] = p[n - k + i] - p[n - k + i - 1];
    qsort(g, (size_t)k, sizeof g[0], cmp64);
    return g[k / 2];
}

int mloop_rec_frame(MLoopRec *r, int64_t pts, int w, int h, int start) {
    int64_t prev = r->seen ? r->last : -1;
    int back = r->seen && pts < prev;               /* the loop started over (or a seek) */
    if (r->seen && !back && pts > prev && (r->interval <= 0 || pts - prev < r->interval)) r->interval = pts - prev;
    r->seen++;
    r->last = pts;
    if (!r->on) {
        int at_start = back || (r->interval > 0 && pts >= 0 && pts <= r->interval);
        if (!start || !at_start || pts < 0 || w <= 0 || h <= 0) return MLR_SKIP;
        r->on = 1;
        r->w = w;
        r->h = h;
        r->n = 0;
    } else {
        if (w != r->w || h != r->h) { mloop_rec_reset(r); return MLR_ABORT; }
        if (back) return r->n >= 2 ? MLR_DONE : (mloop_rec_reset(r), MLR_ABORT);
        if (pts <= r->pts[r->n - 1]) return MLR_SKIP;           /* the same frame again (a redraw) */
        if (r->n >= MLOOP_MAX_FRAMES || pts > MLOOP_MAX_MS) { mloop_rec_reset(r); return MLR_ABORT; }
    }
    r->pts[r->n++] = pts;
    int64_t m = median_gap(r->pts, r->n);
    if (m > 0) r->interval = m;
    return MLR_KEEP;
}

int mloop_rec_whole(const MLoopRec *r, int64_t dur_ms, int64_t *period) {
    if (!r->on || r->n < 2) return 0;
    int64_t iv = median_gap(r->pts, r->n);
    if (iv <= 0) return 0;
    if (r->pts[0] > iv) return 0;                               /* it didn't start at the start */
    for (int i = 1; i < r->n; i++) if (r->pts[i] - r->pts[i - 1] > 3 * iv) return 0;
    int64_t end = r->pts[r->n - 1] + iv;
    if (dur_ms > 0 && (end < dur_ms - 3 * iv || end > dur_ms + 3 * iv)) return 0;
    if (period) *period = end;
    return 1;
}

int mloop_pick(const int64_t *pts, int n, int64_t period, int64_t t, int cap, int shown, int64_t *next) {
    if (n <= 0 || period <= 0) { if (next) *next = -1; return -1; }
    if (t < 0) t = 0;
    t %= period;
    int lo = 0, hi = n - 1, k = 0;                              /* the last frame whose pts ≤ t (frame 0 before it) */
    while (lo <= hi) { int mid = (lo + hi) / 2; if (pts[mid] <= t) { k = mid; lo = mid + 1; } else hi = mid - 1; }
    int64_t nx = k + 1 < n ? pts[k + 1] : period;
    if (cap > 0 && shown >= 0 && shown < n && shown != k) {
        int64_t gap = 1000 / cap - 2;
        int64_t since = t - pts[shown];
        if (since < 0) since += period;                          /* the shown one is from the last turn */
        if (since < gap) {
            int64_t due = pts[shown] + gap;
            if (due >= period) due -= period;
            if (next) *next = due > t ? due : nx;
            return shown;
        }
    }
    if (next) *next = nx;
    return k;
}

int mloop_cap(int setting, int battery_lower) {
    int c = setting == 30 || setting == 15 ? setting : 0;
    if (battery_lower && (c == 0 || c > 15)) c = 15;
    return c;
}

int mloop_hold(int any_visible, int js_paused, double hidden_ms) {
    if (!any_visible && hidden_ms >= MLOOP_HIDDEN_STOP_MS) return MLH_STOP;
    if (!any_visible || js_paused) return MLH_PAUSE;
    return MLH_RUN;
}
