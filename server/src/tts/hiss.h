/* hiss.h — how much hiss a piece of synthesized speech has. Pocket TTS now and then makes a sentence with a bed of
 * broadband noise under the voice (it depends on the sentence and the seed, not on the backend); lord-tts measures
 * each sentence and makes it again with another seed when it is noisy (h_tts.c). Pure, tested by tests/hiss_test.c. */
#ifndef LORD_TTS_HISS_H
#define LORD_TTS_HISS_H

#include <stddef.h>

/* The median, over the frames of 512 samples that aren't a pause, of the share of the energy between 6 and 11 kHz,
 * in dB. Clean speech measures about -27 to -30; a noisy sentence -25 or more. -100 when there is nothing to measure. */
double tts_hiss(const float *s, size_t frames, int rate, int ch);

#define TTS_HISS_MAX (-26.0)

#endif
