/* levels_test.c — the bands of the wave the desktop draws while it reads aloud (src/tts/levels.c). */
#include "tts/levels.h"

#include <math.h>
#include <stdio.h>

static int n, bad;
static void check(const char *what, int ok) { n++; if (!ok) { bad++; printf("✗ %s\n", what); } }

static int loudest(const uint8_t *b) { int m = 0; for (int i = 1; i < SAY_BANDS; i++) if (b[i] > b[m]) m = i; return m; }

int main(void) {
    enum { R = 24000, L = 960 };
    int16_t s[L];
    uint8_t b[SAY_BANDS];
    for (int i = 0; i < L; i++) s[i] = 0;
    say_levels(s, L, R, b);
    int quiet = 1;
    for (int i = 0; i < SAY_BANDS; i++) quiet &= b[i] == 0;
    check("silence: every band at 0", quiet);

    for (int i = 0; i < L; i++) s[i] = (int16_t)(16000 * sin(2 * M_PI * 200 * i / R));
    say_levels(s, L, R, b);
    int low = loudest(b);
    check("200 Hz: a low band is the loudest", low <= 3);
    check("200 Hz: loud", b[low] > 200);
    check("200 Hz: the top band is quiet", b[SAY_BANDS - 1] < 60);

    for (int i = 0; i < L; i++) s[i] = (int16_t)(16000 * sin(2 * M_PI * 5000 * i / R));
    say_levels(s, L, R, b);
    check("5 kHz: a high band is the loudest", loudest(b) >= SAY_BANDS - 4);

    for (int i = 0; i < L; i++) s[i] = (int16_t)(100 * sin(2 * M_PI * 200 * i / R));
    say_levels(s, L, R, b);
    check("very soft: low", b[loudest(b)] < 100);

    say_levels(s, 10, R, b);
    check("few samples: padded, no crash", 1);
    say_levels(NULL, L, R, b);
    check("no samples: all 0", b[0] == 0 && b[SAY_BANDS - 1] == 0);
    say_levels(s, L, 100, b);
    check("a rate too low: all 0", b[0] == 0);
    printf("levels_test: %d/%d\n", n - bad, n);
    return bad != 0;
}
