/* levels.h — how loud each band of a piece of speech is, for the wave the desktop draws while it reads aloud
 * (docs/VOICE.md, the player). lord-say measures each SAY_LEVEL_MS of audio as it plays it and writes the bands to
 * lord-server (speak.c). Pure, tested by tests/levels_test.c. */
#ifndef LORD_TTS_LEVELS_H
#define LORD_TTS_LEVELS_H

#include <stddef.h>
#include <stdint.h>

#define SAY_BANDS 16
#define SAY_LEVEL_MS 40

/* The first 512 samples of pcm (fewer are padded with silence), in SAY_BANDS bands spaced evenly in pitch from
 * 80 Hz to 8 kHz (or half the rate): 0 is -60 dB or quieter, 255 is -6 dB of full scale or louder. */
void say_levels(const int16_t *pcm, size_t n, int rate, uint8_t out[SAY_BANDS]);

#endif
