/* lm_aqlevel.h — how much decoded audio waits in the player's queue (lm_play.c), in bytes.
 * The decoding thread adds whole chunks and the main thread takes them out in whatever pieces fit in lord-server's
 * ring. Counting milliseconds instead, each piece rounded down on its own, took out less than was put in: the level
 * crept up until it read a full second with the queue empty, the decoder stopped making audio, the audio clock
 * stopped and with it the picture: anything with 44.1 kHz audio froze after about half a minute (resampled to 48 kHz,
 * its chunks never fit the ring's free space whole, so every one was split).
 * Bytes add up exactly; only the comparison is in milliseconds. No FFmpeg here: build/aqlevel-test checks it. */
#ifndef LM_AQLEVEL_H
#define LM_AQLEVEL_H

#include <stdint.h>

typedef struct { int64_t bytes; int bytes_per_s; } LmAqLevel;

static inline void lm_aq_init(LmAqLevel *q, int bytes_per_s) { q->bytes = 0; q->bytes_per_s = bytes_per_s; }
static inline void lm_aq_add(LmAqLevel *q, int64_t bytes) { q->bytes += bytes; }
static inline void lm_aq_take(LmAqLevel *q, int64_t bytes) { q->bytes -= bytes; if (q->bytes < 0) q->bytes = 0; }
static inline void lm_aq_clear(LmAqLevel *q) { q->bytes = 0; }
static inline int64_t lm_aq_ms(const LmAqLevel *q) { return q->bytes_per_s > 0 ? q->bytes * 1000 / q->bytes_per_s : 0; }
#endif
