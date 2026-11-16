/* spectrum-test — the sound wave widget's spectrum (src/sound/spectrum.c) and its lines (src/ui/wave.c wave_render):
 * a tone lights its own band, silence goes quiet and stops moving, and the lines stay inside their box with the
 * bass in the middle. */
#include "../src/sound/spectrum.h"
#include "../src/ui/wave.h"
#include "../src/ui/ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* wave.c's ties to the engine, unused by wave_render */
Ui ui;
Node *node_get(int id) { (void)id; return NULL; }
void damage_add(Box b) { (void)b; }
void canvas_blit(Canvas *c, const uint32_t *src, int sw, int sh, int x, int y, float r, float opacity, int opaque) {
    (void)c; (void)src; (void)sw; (void)sh; (void)x; (void)y; (void)r; (void)opacity; (void)opaque;
}

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void tone(Spectrum *s, double hz, double amp, int blocks) {
    static double ph;
    float buf[1600];
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < 1600; i++) { buf[i] = (float)(amp * sin(ph)); ph += 2 * M_PI * hz / 48000; }
        spectrum_feed(s, buf, 1600);
    }
}

static int loudest(const Spectrum *s) {
    int k = 0;
    for (int b = 1; b < SPEC_BANDS; b++) if (s->bands[b] > s->bands[k]) k = b;
    return k;
}

static double band_hz(int b) { return spectrum_band_hz(48000, b); }

int main(void) {
    Spectrum s;
    spectrum_init(&s, 48000);
    CHECK(s.quiet && s.seq == 0, "starts quiet");
    float zero[1600] = { 0 };
    CHECK(!spectrum_feed(&s, zero, 1600) && s.seq == 0, "silence while quiet changes nothing");

    /* tones land in their own band, and louder is higher */
    const double hz[] = { 60, 250, 1000, 4000, 12000 };
    for (int i = 0; i < 5; i++) {
        spectrum_init(&s, 48000);
        tone(&s, hz[i], 0.5, 8);
        int b = loudest(&s);
        CHECK(fabs(log2(band_hz(b) / hz[i])) < 0.5, "%.0f Hz lands in band %d (%.0f Hz)", hz[i], b, band_hz(b));
        CHECK(s.bands[b] > 0.8f && !s.quiet, "%.0f Hz at -6 dB is near the top: %.2f", hz[i], s.bands[b]);
    }
    spectrum_init(&s, 48000);
    tone(&s, 1000, 0.01, 8);
    float soft = s.bands[loudest(&s)];
    CHECK(soft > 0.2f && soft < 0.75f, "-40 dB is in the middle: %.2f", soft);

    /* it falls back to quiet after the sound stops, then stops moving */
    tone(&s, 1000, 0.5, 4);
    int blocks = 0;
    while (!s.quiet && blocks < 200) { spectrum_feed(&s, zero, 1600); blocks++; }
    CHECK(s.quiet && blocks > 5 && blocks < 60, "quiet after %d blocks of silence", blocks);
    uint32_t seq = s.seq;
    for (int i = 0; i < 30; i++) spectrum_feed(&s, zero, 1600);
    CHECK(s.seq == seq, "a second of silence repaints nothing (seq %u → %u)", seq, s.seq);

    /* broken input doesn't poison it */
    float nan[4] = { NAN, INFINITY, -INFINITY, 0.5f };
    spectrum_feed(&s, nan, 4);
    for (int b = 0; b < SPEC_BANDS; b++) CHECK(s.bands[b] >= 0 && s.bands[b] <= 1, "band %d in range after NaN: %f", b, s.bands[b]);

    /* the lines: silence is one flat line across the middle */
    WaveConf *w = wave_conf_new();
    spectrum_init(&s, 48000);
    const int W = 728, H = 170;
    wave_render(w, &s, W, H, 1);
    CHECK(w->px && w->pw == W && w->ph == H, "renders at its size");
    int lit_mid = 0, lit_far = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint32_t p = w->px[y * W + x], a = p >> 24;
        CHECK(((p >> 16) & 255) <= a && ((p >> 8) & 255) <= a && (p & 255) <= a, "premultiplied at %d,%d", x, y);
        if (a > 64) { if (abs(y - H / 2) <= 2) lit_mid++; else if (abs(y - H / 2) > 24) lit_far++; }
    }
    CHECK(lit_mid > W / 2 && lit_far == 0, "silence: a glowing line along the middle (%d lit there, %d away)", lit_mid, lit_far);

    /* bass moves the middle, not the ends */
    tone(&s, 80, 0.5, 6);
    wave_render(w, &s, W, H, 1);
    int spread_mid = 0, spread_end = 0;
    for (int y = 0; y < H; y++) {
        if (abs(y - H / 2) < 20) continue;
        for (int x = W / 2 - 60; x < W / 2 + 60; x++) if ((w->px[y * W + x] >> 24) > 64) spread_mid++;
        for (int x = 0; x < 40; x++) if ((w->px[y * W + x] >> 24) > 64) spread_end++;
    }
    CHECK(spread_mid > 50 && spread_end == 0, "bass: lines open in the middle (%d), the ends stay flat (%d)", spread_mid, spread_end);

    /* a size change, and sizes past the limit, stay safe */
    wave_render(w, &s, 200, 60, 2);
    CHECK(w->pw == 200 && w->ph == 60, "re-sized");
    wave_render(w, &s, 9000, 3000, 1);
    CHECK(w->pw <= 4096 && w->ph <= 1600, "clamped: %dx%d", w->pw, w->ph);
    wave_render(w, &s, 1, 1, 1);
    wave_conf_free(w);

    printf("spectrum-test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
