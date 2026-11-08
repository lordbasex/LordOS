/* hiss.c — how much hiss a piece of synthesized speech has (hiss.h). */
#include "hiss.h"

#include <complex.h>
#include <math.h>
#include <stdlib.h>

#define N 512

/* in place, radix 2 */
static void fft(double complex *x, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double complex t = x[i]; x[i] = x[j]; x[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double complex w = cexp(-2.0 * I * M_PI / len);
        for (int i = 0; i < n; i += len) {
            double complex wk = 1;
            for (int k = 0; k < len / 2; k++) {
                double complex u = x[i + k], v = x[i + k + len / 2] * wk;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                wk *= w;
            }
        }
    }
}

static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

double tts_hiss(const float *s, size_t frames, int rate, int ch) {
    if (!s || rate < 16000 || ch < 1 || frames < N) return -100;
    size_t nf = frames / N, used = 0;
    double *r = malloc(nf * sizeof *r);
    if (!r) return -100;
    int lo = (int)(6000.0 * N / rate), hi = (int)(11000.0 * N / rate);
    if (hi > N / 2) hi = N / 2;
    double complex x[N];
    for (size_t f = 0; f < nf; f++) {
        double e = 0;
        for (int i = 0; i < N; i++) {
            double v = 0;
            for (int c = 0; c < ch; c++) v += s[(f * N + (size_t)i) * (size_t)ch + (size_t)c];
            v /= ch;
            if (!isfinite(v)) v = 0;
            x[i] = v;
            e += v * v;
        }
        if (e / N < 0.002 * 0.002) continue;          /* a pause: only what is spoken counts */
        fft(x, N);
        double tot = 1e-12, band = 0;
        for (int k = 0; k < N / 2; k++) {
            double p = creal(x[k]) * creal(x[k]) + cimag(x[k]) * cimag(x[k]);
            tot += p;
            if (k >= lo && k < hi) band += p;
        }
        r[used++] = 10 * log10(band / tot + 1e-12);
    }
    double m = -100;
    if (used) { qsort(r, used, sizeof *r, cmp); m = r[used / 2]; }
    free(r);
    return m;
}
