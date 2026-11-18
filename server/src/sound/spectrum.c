/* spectrum.c — levels per frequency band for the sound wave widget (see spectrum.h).
 *
 * Each block: a Hann-windowed FFT of the last SPEC_N samples, the largest bin of each log-spaced band in
 * decibels mapped to 0..1 (-72 dB … -12 dB of full scale), then smoothed. A block that is digital silence
 * skips the FFT and only lets the bands fall; once they are all at zero the spectrum is "quiet" and seq
 * stops moving, so nothing on screen repaints until there is sound again. */
#include "spectrum.h"

#include <math.h>
#include <string.h>

static float hann[SPEC_N];
static int band_lo[SPEC_BANDS + 1];
static int ready_rate;

static void tables(int rate) {
    if (ready_rate == rate) return;
    for (int i = 0; i < SPEC_N; i++) hann[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (SPEC_N - 1));
    const double f0 = 40, f1 = 16000, hz = (double)rate / SPEC_N;
    for (int b = 0; b <= SPEC_BANDS; b++) {
        double f = f0 * pow(f1 / f0, (double)b / SPEC_BANDS);
        int k = (int)(f / hz + 0.5);
        if (k < 1) k = 1;
        if (k > SPEC_N / 2 - 1) k = SPEC_N / 2 - 1;
        if (b && k <= band_lo[b - 1]) k = band_lo[b - 1] + 1;   /* at least one bin per band */
        band_lo[b] = k;
    }
    ready_rate = rate;
}

/* In-place radix-2 complex FFT. */
static void fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float a = -2 * (float)M_PI / len, wr = cosf(a), wi = sinf(a);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int p = i + k, q = p + len / 2;
                float tr = re[q] * cr - im[q] * ci, ti = re[q] * ci + im[q] * cr;
                re[q] = re[p] - tr; im[q] = im[p] - ti;
                re[p] += tr; im[p] += ti;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

float spectrum_band_hz(int rate, int b) {
    if (rate <= 0 || b < 0 || b >= SPEC_BANDS) return 0;
    tables(rate);
    return (band_lo[b] + band_lo[b + 1] - 1) * 0.5f * rate / SPEC_N;
}

void spectrum_init(Spectrum *s, int rate) {
    memset(s, 0, sizeof *s);
    s->rate = rate > 0 ? rate : 48000;
    s->quiet = 1;
    tables(s->rate);
}

int spectrum_feed(Spectrum *s, const float *x, int n) {
    if (n <= 0) return 0;
    tables(s->rate);
    float peak = 0;
    for (int i = 0; i < n; i++) {
        float v = x[i];
        if (!(v == v)) v = 0;                            /* NaN from a broken stream */
        if (v > 1) v = 1; else if (v < -1) v = -1;
        s->ring[s->pos] = v;
        s->pos = (s->pos + 1) % SPEC_N;
        float a = fabsf(v);
        if (a > peak) peak = a;
    }
    float target[SPEC_BANDS] = { 0 };
    if (peak > 1e-4f) {                                  /* about -80 dB: anything quieter draws as silence */
        static float re[SPEC_N], im[SPEC_N];
        for (int i = 0; i < SPEC_N; i++) { re[i] = s->ring[(s->pos + i) % SPEC_N] * hann[i]; im[i] = 0; }
        fft(re, im, SPEC_N);
        const float norm = 4.0f / SPEC_N;                /* a full-scale sine ≈ 1 (Hann's gain is 1/2) */
        for (int b = 0; b < SPEC_BANDS; b++) {
            float m = 0;
            for (int k = band_lo[b]; k < band_lo[b + 1]; k++) {
                float p = re[k] * re[k] + im[k] * im[k];
                if (p > m) m = p;
            }
            float db = 10 * log10f(m * norm * norm + 1e-12f);
            float v = (db + 72) / 60;
            target[b] = v < 0 ? 0 : v > 1 ? 1 : v;
        }
    } else if (s->quiet) return 0;                       /* still silent: nothing changed */
    float level = 0;
    int any = 0;
    for (int b = 0; b < SPEC_BANDS; b++) {
        float cur = s->bands[b], t = target[b];
        cur += (t - cur) * (t > cur ? 0.6f : 0.18f);
        if (cur < 0.004f) cur = 0;
        s->bands[b] = cur;
        if (cur > level) level = cur;
        if (cur > 0) any = 1;
    }
    s->level = level;
    s->quiet = !any;
    s->seq++;
    return 1;
}
