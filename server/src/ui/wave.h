/* wave.h — <SoundWave>: the sound wave widget's lines, drawn in C from the spectrum (sound/spectrum.h).
 *
 * The node draws itself: JS only sets its look (setProp "wave") and which source the server listens to
 * (lord.sound.spectrum). When the bands change, wave_tick damages each wave node's box and nothing else;
 * while there is silence the spectrum is quiet, nothing is damaged and the line stays still. */
#ifndef LORD_WAVE_H
#define LORD_WAVE_H

#include <stdint.h>
#include "../canvas.h"

struct Node;
struct Spectrum;

typedef struct WaveConf {
    int lines;                      /* 4 … 24 */
    uint32_t col[3];                /* RGB: the first line, the middle one, the last one */
    float gain;                     /* 0.25 … 3 */
    /* the last raster, reused while neither the bands nor the size changed */
    uint32_t *px;
    float *acc, *half, *g1, *g2, *tmp;
    int pw, ph;
    uint32_t seq;
    int built;
} WaveConf;

WaveConf *wave_conf_new(void);
void wave_conf_free(WaveConf *w);
void wave_conf_changed(WaveConf *w);    /* its look changed: draw it again */

/* Draws the node's wave into (x, y, w, h) in device pixels, scale k, opacity op. */
void wave_paint(Canvas *c, struct Node *n, float x, float y, float w, float h, float k, float op);
/* The spectrum the nodes draw (the sound client's, snd_spectrum_now); silence until set. */
void wave_source(const struct Spectrum *s);
/* Once per loop: damages the wave nodes when the bands changed. */
void wave_tick(void);

/* Pure, for tests: renders the lines of spectrum sp into w->px (pw × ph, premultiplied ARGB). */
void wave_render(WaveConf *w, const struct Spectrum *sp, int pw, int ph, float k);

#endif
