/* spectrum.h — the sound wave widget's spectrum: levels per frequency band of a mono float stream.
 *
 * Pure (no I/O): snd.c feeds it the samples of the chosen source (the desktop, one app or a microphone)
 * and ui/wave.c draws from the bands. Only these numbers ever leave the sound client: never the sound. */
#ifndef LORD_SPECTRUM_H
#define LORD_SPECTRUM_H

#include <stdint.h>

#define SPEC_N 2048                     /* FFT size: ~23 Hz per bin at 48 kHz */
#define SPEC_BANDS 48                   /* log-spaced, 40 Hz … 16 kHz */

typedef struct Spectrum {
    float ring[SPEC_N];                 /* the last SPEC_N samples */
    int pos;
    float bands[SPEC_BANDS];            /* 0..1, smoothed: quick to rise, slow to fall */
    float level;                        /* the largest band */
    uint32_t seq;                       /* bumped each time the bands change */
    int quiet;                          /* 1 once every band has fallen to zero (nothing to draw moving) */
    int rate;
} Spectrum;

void spectrum_init(Spectrum *s, int rate);
/* The middle of band b in Hz (below ~500 Hz each band is one FFT bin, so they are evenly spaced there). */
float spectrum_band_hz(int rate, int b);
/* Adds a block of samples and updates the bands; returns 1 if they changed (seq bumped). */
int  spectrum_feed(Spectrum *s, const float *x, int n);

#endif
