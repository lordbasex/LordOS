/* gscissor.h — what a composed frame must redraw, from its buffer's age (EGL_EXT_buffer_age; docs/GPU.md,
 * "The buffer-age scissor"). No GL here: gpu.c queries the age and draws, build/engine-test runs this same code
 * against a simulated swap chain. */
#ifndef LORD_GSCISSOR_H
#define LORD_GSCISSOR_H

#include "../canvas.h"

#define GSC_HIST 5                      /* frames remembered: an older buffer is redrawn whole */

typedef struct {
    Rect r[GSC_HIST];                   /* what each swapped frame changed, newest first */
    int n;                              /* how many of them are known */
} GpuScissor;

/* Forget the history (a frame that wrote the buffer and was not swapped, or the screen shown from
 * something else): the next frames redraw whole until the history is long enough again. */
void gsc_reset(GpuScissor *g);
/* The rectangle to redraw in a buffer of this age (0: its content is undefined) for a frame that changes
 * cur, on a W × H output: cur plus what the age - 1 frames since that buffer was shown changed. The whole
 * output if the history doesn't reach that far; empty if nothing changed. */
Rect gsc_region(const GpuScissor *g, int age, Rect cur, int W, int H);
/* A swapped frame changed cur. Every frame swapped must be noted, whole ones included. */
void gsc_push(GpuScissor *g, Rect cur);
/* Clipped to W × H; a rectangle with no area stays empty ({0,0,0,0}). */
Rect gsc_clip(Rect r, int W, int H);
Rect gsc_join(Rect a, Rect b);
long gsc_area(Rect r);

#endif
