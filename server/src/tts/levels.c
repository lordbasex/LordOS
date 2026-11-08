/* levels.c — the bands of the wave the desktop draws while it reads aloud (levels.h). */
#include "levels.h"

#include <complex.h>
#include <math.h>

#define N 512

static void fft(double complex *x, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double complex t = x[i]; x[i] = x[j]; x[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double complex w = cexp(-2 * M_PI * I / len);
        for (int i = 0; i < n; i += len) {
            double complex u = 1;
            for (int k = 0; k < len / 2; k++) {
                double complex a = x[i + k], b = x[i + k + len / 2] * u;
                x[i + k] = a + b;
                x[i + k + len / 2] = a - b;
                u *= w;
            }
        }
    }
}

void say_levels(const int16_t *pcm, size_t n, int rate, uint8_t out[SAY_BANDS]) {
    for (int b = 0; b < SAY_BANDS; b++) out[b] = 0;
    if (!pcm || !n || rate < 8000) return;
    double complex x[N];
    for (int i = 0; i < N; i++) {
        double hann = 0.5 - 0.5 * cos(2 * M_PI * i / (N - 1));
        x[i] = (size_t)i < n ? hann * pcm[i] / 32768.0 : 0;
    }
    fft(x, N);
    double lo = 80, hi = rate / 2.0 < 8000 ? rate / 2.0 : 8000;
    double ref = (N / 4.0) * (N / 4.0);           /* a full-scale sine's peak bin through the Hann window */
    for (int b = 0; b < SAY_BANDS; b++) {
        double f0 = lo * pow(hi / lo, (double)b / SAY_BANDS), f1 = lo * pow(hi / lo, (double)(b + 1) / SAY_BANDS);
        int k0 = (int)floor(f0 * N / rate), k1 = (int)ceil(f1 * N / rate);
        if (k1 <= k0) k1 = k0 + 1;
        if (k1 > N / 2) k1 = N / 2;
        double e = 0;
        for (int k = k0; k < k1; k++) e += creal(x[k]) * creal(x[k]) + cimag(x[k]) * cimag(x[k]);
        double db = e > 0 ? 10 * log10(e / ref) : -200;
        double v = (db + 60) / 54 * 255;
        out[b] = v <= 0 || v != v ? 0 : v >= 255 ? 255 : (uint8_t)v;
    }
}
