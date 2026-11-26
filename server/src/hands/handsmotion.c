/* handsmotion.c — the agent cursor's motion in human mode (see handsmotion.h) */
#include "handsmotion.h"

#include <math.h>

uint32_t hands_rand(uint32_t *seed) {
    uint32_t x = *seed ? *seed : 1;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *seed = x;
    return x;
}

double hands_ease(double t) {
    if (!(t > 0)) return 0;
    if (t >= 1) return 1;
    return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2;
}

double hands_fitts_ms(double dist, double width, int reduce) {
    if (!(dist >= 1)) return 0;
    if (!(width >= 4)) width = 4;
    if (width > 400) width = 400;
    /* MT = a + b log2(D / W + 1): a 120 ms start, 130 ms a bit of difficulty */
    double ms = 120 + 130 * log2(dist / width + 1);
    if (reduce) return ms < 60 ? 60 : ms > HANDS_GLIDE_REDUCED ? HANDS_GLIDE_REDUCED : ms;
    return ms < HANDS_GLIDE_MIN ? HANDS_GLIDE_MIN : ms > HANDS_GLIDE_MAX ? HANDS_GLIDE_MAX : ms;
}

void hands_glide_make(HandsGlide *g, double x0, double y0, double x1, double y1, double width, int reduce, uint32_t *seed) {
    g->x0 = x0; g->y0 = y0; g->x1 = x1; g->y1 = y1;
    double dx = x1 - x0, dy = y1 - y0, d = sqrt(dx * dx + dy * dy);
    g->ms = hands_fitts_ms(d, width, reduce);
    /* a slight curve: the middle pushed sideways by 6 to 14 % of the distance (at most 90 points), one side or the other */
    double bend = 0;
    if (d >= 1 && !reduce) {
        uint32_t r = hands_rand(seed);
        bend = d * (0.06 + 0.08 * (double)(r % 1000) / 1000.0);
        if (bend > 90) bend = 90;
        if (r & 0x10000) bend = -bend;
    }
    double nx = d >= 1 ? -dy / d : 0, ny = d >= 1 ? dx / d : 0;
    g->cx = (x0 + x1) / 2 + nx * bend;
    g->cy = (y0 + y1) / 2 + ny * bend;
}

int hands_glide_at(const HandsGlide *g, double ms, double *x, double *y) {
    if (!(g->ms > 0) || ms >= g->ms) { *x = g->x1; *y = g->y1; return 1; }
    double t = hands_ease(ms / g->ms), u = 1 - t;
    *x = u * u * g->x0 + 2 * u * t * g->cx + t * t * g->x1;
    *y = u * u * g->y0 + 2 * u * t * g->cy + t * t * g->y1;
    return 0;
}

double hands_hover_ms(int reduce, uint32_t *seed) {
    if (reduce) return 50;
    return HANDS_HOVER_MIN + (double)(hands_rand(seed) % (HANDS_HOVER_MAX - HANDS_HOVER_MIN + 1));
}

double hands_type_ms(unsigned char prev, size_t chars, uint32_t *seed) {
    double ms = HANDS_TYPE_MIN + (double)(hands_rand(seed) % (HANDS_TYPE_MAX - HANDS_TYPE_MIN + 1));
    if (prev == ' ' || prev == '.' || prev == ',' || prev == '\n') ms += 15;      /* a word ends: a breath */
    /* the whole text within HANDS_TYPE_BUDGET: a long one goes faster (never under 2 ms a character) */
    if (chars > 0) {
        double most = (double)HANDS_TYPE_BUDGET / (double)chars;
        if (ms > most) ms = most < 2 ? 2 : most;
    }
    return ms;
}

int hands_scroll_steps(int amount, int *steps) {
    if (amount < 1) amount = 1;
    int n = amount < HANDS_SCROLL_STEPS ? amount : HANDS_SCROLL_STEPS;
    for (int i = 0; i < n; i++) steps[i] = amount / n + (i < amount % n ? 1 : 0);
    return n;
}
