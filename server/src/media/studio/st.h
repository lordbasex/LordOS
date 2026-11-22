/* st.h — LordOS Studio's frame pipeline (docs/CAMERA-STUDIO-PLAN.md 3.2, 4.3; phase 1).
 *
 * One frame in (BGRA, the camera's size), one frame out (BGRA, the same size):
 *   1. a box-averaged 256×144 RGB copy feeds the Selfie Segmenter (nn.c) → a person probability;
 *   2. temporal smoothing at that size (an average per pixel weighted by motion, with hysteresis);
 *   3. the fast guided filter (He and Sun, 2015): its coefficients at a quarter of the frame, guided by the luma;
 *   4. the composite at full size: alpha = a·luma + b, over a blurred copy of the frame (the person's own pixels kept
 *      out of the blur so no halo) or over a picture, optionally mirrored.
 * Steps 1–3 run on the CPU; step 4 on the GPU when lord-studio got a render node and GLES 3 works there (stgl.c), on
 * the CPU otherwise. Both paths give the same picture within a few levels (build/studio-test).
 *
 * Nothing here opens a file or a device: the model arrives as bytes, the render node as an fd. */
#ifndef LORD_STUDIO_ST_H
#define LORD_STUDIO_ST_H

#include <stddef.h>
#include <stdint.h>

enum { ST_MODE_NONE = 0, ST_MODE_BLUR = 1, ST_MODE_PICTURE = 2, ST_MODE_LAST = ST_MODE_PICTURE };

#define ST_MAX_W 3840
#define ST_MAX_H 2160
#define ST_BLUR_MAX 100

typedef struct {
    int mode;                    /* ST_MODE_* */
    int blur;                    /* 0..100: how strong the background blur is */
    int mirror;                  /* 1: the output is mirrored left to right */
} StCfg;

typedef struct {
    double seg_ms, refine_ms, compose_ms;   /* the last frame's steps */
    int gpu;                     /* 1: the composite ran on the GPU */
    int frames;
} StStats;

typedef struct StFx StFx;

/* model: the segmenter's .lnn bytes (already hash-checked by lord-server); render_fd: a render node or -1 (not taken
 * over: the caller keeps it). NULL on failure, with a reason in err. */
StFx *stfx_new(const void *model, size_t len, int render_fd, char *err, size_t errlen);
void  stfx_free(StFx *s);
/* Out-of-range values are clamped; returns 0. */
int   stfx_config(StFx *s, const StCfg *c);
/* The picture background (BGRA, any size up to ST_MAX_W×ST_MAX_H): copied; NULL clears it. 0 on success. */
int   stfx_background(StFx *s, const uint8_t *bgra, int w, int h, int stride);
/* One frame: in and out are BGRA of w×h (even, 64..ST_MAX_W×ST_MAX_H); they may not overlap. 0 on success. */
int   stfx_frame(StFx *s, const uint8_t *in, int istride, int w, int h, uint8_t *out, int ostride);
void  stfx_stats(const StFx *s, StStats *out);
/* the last matte at full size would be costly; this is the refined 256×144 one (tests) */
const float *stfx_matte(const StFx *s, int *w, int *h);
/* forget the matte's history (a new camera, or the tests) */
void  stfx_reset(StFx *s);
/* force the CPU composite even with a render node (tests compare both) */
void  stfx_force_cpu(StFx *s, int cpu);

/* ---- stgf.c: the pieces, exposed for the tests ---- */
/* box-averages BGRA (w×h) into RGB floats 0..1 (ow×oh×3) */
void st_downscale_rgb(const uint8_t *bgra, int stride, int w, int h, float *rgb, int ow, int oh);
/* box-averages BGRA into luma 0..1 (ow×oh) */
void st_downscale_luma(const uint8_t *bgra, int stride, int w, int h, float *y, int ow, int oh);
/* bilinear resize of one float plane (half-pixel centres) */
void st_resize_plane(const float *src, int sw, int sh, float *dst, int dw, int dh);
/* temporal smoothing with motion and hysteresis: mask (n) is updated in place from raw; luma and prev_luma drive the
 * motion; state holds the previous output (n floats, 0 at the start) */
void st_temporal(float *state, const float *raw, const float *luma, float *prev_luma, int w, int h, int first);
/* one pass over the frame: box averages per guide cell as RGB 0..255 and luma 0..1 (rowsum: w·4 scratch) */
void st_downscale_guide(const uint8_t *bgra, int stride, int w, int h, float *rgbg, float *yg, int gw, int gh, uint16_t *rowsum);
/* area-weighted resize of RGB 0..255 into RGB 0..1 (the segmenter's input from the guide cells), at most 4:1;
 * tmp: sh·dw·3 floats */
void st_resize_rgb(const float *src, int sw, int sh, float *dst, int dw, int dh, float *tmp);
/* fast guided filter coefficients: I guide and p input at the same low size (w×h); radius r; eps; a and b out;
 * tmp: 6·w·h + w floats */
void st_guided_coeffs(const float *I, const float *p, int w, int h, int r, float eps, float *a, float *b, float *tmp);
/* box blur of a premultiplied RGBW plane (w×h×4 floats) in place, three passes of radius r (about a Gaussian);
 * tmp: 4·w·h + 4·w floats */
void st_box_blur4(float *rgbw, int w, int h, int r, float *tmp);

#endif
