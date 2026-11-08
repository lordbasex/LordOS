/* hiss.h — how much hiss a piece of synthesized speech has, and whether a try was cut short. Pocket TTS now and then makes a sentence with a bed of
 * broadband noise under the voice (it depends on the sentence and the seed, not on the backend); lord-tts measures
 * each sentence and makes it again with another seed when it is noisy (h_tts.c). Pure, tested by tests/hiss_test.c. */
#ifndef LORD_TTS_HISS_H
#define LORD_TTS_HISS_H

#include <stddef.h>

/* The median, over the frames of 512 samples that aren't a pause, of the share of the energy between 6 and 11 kHz,
 * in dB. Clean speech measures about -27 to -30; a noisy sentence -25 or more. -100 when there is nothing to measure. */
double tts_hiss(const float *s, size_t frames, int rate, int ch);

#define TTS_HISS_MAX (-26.0)

/* Pocket TTS also, now and then, ends a sentence early ("Hola, LordOS ya…" came out as just "Hola"), so a cleaner try
 * must never win for being cut. A try is cut short when it is under 60 % of the longest try of the same sentence, or,
 * for a sentence of 6 syllables or more, faster than 7.5 syllables a second (its whole sentences measured 3.8–7.4;
 * cut ones 8.1 and 13.6). Syllables are counted as groups of vowels (Spanish, Portuguese and English). */
size_t tts_syllables(const char *text);
int tts_cut_short(size_t frames, int rate, size_t syllables, size_t longest);

#endif
