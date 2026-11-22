/* aqlevel_test.c — the player's decoded-audio level (src/media/helper/lm_aqlevel.h).
 * 44.1 kHz AAC resampled to the ring's 48 kHz stereo gives chunks of 1114-1115 frames (4456-4460 bytes), and the main
 * thread writes them in pieces that fit the ring's free space (4096 bytes here). Counted in milliseconds rounded per
 * piece, the level crept up ~1 ms per chunk and read a full second with the queue empty after about half a minute:
 * the decoder stopped making audio and playback froze. Counted in bytes it must come back to exactly zero. */
#include "../src/media/helper/lm_aqlevel.h"

#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define BPS (48000 * 4)                 /* s16 stereo at 48 kHz */
#define LIMIT_MS 1000                   /* lm_play.c: AQ_MS */

int main(void) {
    LmAqLevel q;
    lm_aq_init(&q, BPS);
    CHECK(lm_aq_ms(&q) == 0);
    int64_t old_ms = 0;                 /* the way it was counted before: ms, rounded down per chunk and per piece */
    int queued[64], head = 0, n = 0, max_ms = 0;
    long frac = 0;
    for (int chunk = 0; chunk < 3000; chunk++) {          /* ~70 s of audio */
        /* 1024 frames at 44.1 kHz -> 48 kHz: 1114.56 on average */
        frac += 1024L * 48000;
        int frames = (int)(frac / 44100);
        frac %= 44100;
        int bytes = frames * 4;
        queued[(head + n++) % 64] = bytes;
        lm_aq_add(&q, bytes);
        old_ms += (int64_t)bytes * 1000 / BPS;
        if (lm_aq_ms(&q) > max_ms) max_ms = (int)lm_aq_ms(&q);
        /* the ring takes what it has room for: keep ~1 s queued, as the decoder does */
        while (n && lm_aq_ms(&q) >= LIMIT_MS - 30) {
            int *c = &queued[head];
            int piece = *c > 4096 ? 4096 : *c;
            lm_aq_take(&q, piece);
            old_ms -= (int64_t)piece * 1000 / BPS;
            *c -= piece;
            if (!*c) { head = (head + 1) % 64; n--; }
        }
    }
    while (n) {                                           /* the end of the file: the ring takes everything */
        int *c = &queued[head];
        int piece = *c > 4096 ? 4096 : *c;
        lm_aq_take(&q, piece);
        old_ms -= (int64_t)piece * 1000 / BPS;
        *c -= piece;
        if (!*c) { head = (head + 1) % 64; n--; }
    }
    CHECK(q.bytes == 0 && lm_aq_ms(&q) == 0);             /* an empty queue reads empty */
    CHECK(max_ms <= LIMIT_MS + 30);
    CHECK(old_ms >= LIMIT_MS);                            /* the old count: "full" with nothing queued (the freeze) */
    lm_aq_add(&q, BPS / 2);
    CHECK(lm_aq_ms(&q) == 500);
    lm_aq_take(&q, BPS);                                  /* never below zero */
    CHECK(q.bytes == 0);
    lm_aq_add(&q, 1000);
    lm_aq_clear(&q);                                      /* a seek */
    CHECK(lm_aq_ms(&q) == 0);
    if (fails) { printf("aqlevel-test: %d failures\n", fails); return 1; }
    printf("aqlevel-test: ok (the old count ended at %lld ms with nothing queued)\n", (long long)old_ms);
    return 0;
}
