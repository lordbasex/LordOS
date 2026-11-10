/* xwscale.c — the size of an X pixel (see xwscale.h). */
#include "xwscale.h"

#include <stdio.h>

static int clamp_pct(int pct) { return pct < XWS_MIN ? XWS_MIN : pct > XWS_MAX ? XWS_MAX : pct; }

int xws_pick(const int *pct, int n, int self) {
    if (!self || !pct) return 100;
    int best = 0;
    for (int i = 0; i < n; i++) if (pct[i] > best) best = pct[i];
    return best ? clamp_pct(best) : 100;
}

/* a * b / c rounded half away from zero, in 64 bits (no overflow for any int and pct) */
static int mul_div(int a, int b, int c) {
    long long p = (long long)a * b;
    long long q = p >= 0 ? (p + c / 2) / c : -((-p + c / 2) / c);
    return q > 2147483647LL ? 2147483647 : q < -2147483647LL ? -2147483647 : (int)q;
}

int xws_to_x(int logical, int pct) { pct = clamp_pct(pct); return pct == 100 ? logical : mul_div(logical, pct, 100); }
int xws_from_x(int x, int pct) { pct = clamp_pct(pct); return pct == 100 ? x : mul_div(x, 100, pct); }
double xws_to_xf(double logical, int pct) { return logical * clamp_pct(pct) / 100.0; }
double xws_from_xf(double x, int pct) { return x * 100.0 / clamp_pct(pct); }

int xws_dpi(int pct) { return mul_div(96, clamp_pct(pct), 100); }

size_t xws_resources(int pct, char *out, size_t size) {
    if (!out || !size) return 0;
    out[0] = 0;
    if (clamp_pct(pct) == 100) return 0;
    int n = snprintf(out, size, "Xft.dpi:\t%d\n", xws_dpi(pct));
    if (n < 0 || (size_t)n >= size) { out[0] = 0; return 0; }
    return (size_t)n;
}
