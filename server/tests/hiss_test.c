/* hiss_test.c — the hiss measure lord-tts uses to make a noisy sentence again (src/tts/hiss.c). */
#include "tts/hiss.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int n, bad;
static void check(const char *what, int ok, double v) { n++; if (!ok) { bad++; printf("✗ %s (%.1f dB)\n", what, v); } }

int main(void) {
    enum { R = 24000, L = R * 2 };
    float *s = malloc(L * sizeof *s);
    unsigned seed = 1;
    /* a voiced sound: harmonics of 150 Hz falling off */
    for (int i = 0; i < L; i++) { double v = 0; for (int h = 1; h < 30; h++) v += sin(2 * M_PI * 150 * h * i / R) / (h * h); s[i] = (float)(0.3 * v); }
    double clean = tts_hiss(s, L, R, 1);
    check("harmonics are clean", clean < TTS_HISS_MAX, clean);
    /* the same with a bed of white noise */
    for (int i = 0; i < L; i++) { seed = seed * 1103515245u + 12345u; s[i] += (float)(((seed >> 8) / 16777216.0 - 0.5) * 0.2); }
    double noisy = tts_hiss(s, L, R, 1);
    check("a bed of noise is hiss", noisy > TTS_HISS_MAX, noisy);
    /* silence and nonsense */
    for (int i = 0; i < L; i++) s[i] = 0;
    check("silence: nothing to measure", tts_hiss(s, L, R, 1) == -100, 0);
    s[100] = NAN;
    check("a NaN is ignored", tts_hiss(s, L, R, 1) == -100, 0);
    check("too short", tts_hiss(s, 100, R, 1) == -100, 0);
    check("no samples", tts_hiss(NULL, L, R, 1) == -100, 0);
    /* a try the model ended early */
    check("syllables: Spanish", tts_syllables("Hola, LordOS ya puede leer en voz alta.") == 12, (double)tts_syllables("Hola, LordOS ya puede leer en voz alta."));
    check("syllables: accents and a lone y", tts_syllables("Acción y canción, ÁÉ") == 6, (double)tts_syllables("Acción y canción, ÁÉ"));
    check("syllables: Portuguese", tts_syllables("Não sei, você está aí?") == 7, (double)tts_syllables("Não sei, você está aí?"));
    check("a whole sentence (2.56 s) is fine", !tts_cut_short(61440, R, 12, 76800), 0);
    check("\"Hola\" alone (0.88 s) is cut", tts_cut_short(21120, R, 12, 21120), 0);
    check("1.6 s for 13 syllables is cut", tts_cut_short(38400, R, 13, 38400), 0);
    check("under 60 % of the longest try is cut", tts_cut_short(30000, R, 3, 76800), 0);
    check("a short word is never judged by its syllables", !tts_cut_short(11520, R, 1, 11520), 0);
    check("no rate: cut", tts_cut_short(1000, 0, 2, 1000), 0);
    free(s);
    printf("hiss_test: %d/%d\n", n - bad, n);
    return bad != 0;
}
