/* st.h — LordOS Studio's frame pipeline (docs/CAMERA-STUDIO-PLAN.md 3.2, 4.3, 4.4, 5; phases 1 and 3).
 *
 * One frame in (BGRA, the camera's size), one frame out (BGRA, the same size):
 *   0. Center Stage (stframe.c): BlazeFace every few frames on a 128×128 letterboxed copy, a 1€ filter on the box, and
 *      the frame cropped and scaled back to its size around the person; then Retouch (stlook.c) on the face's skin;
 *   1. a box-averaged 256×144 RGB copy feeds the Selfie Segmenter (nn.c) → a person probability;
 *   2. temporal smoothing at that size (an average per pixel weighted by motion, with hysteresis);
 *   3. the fast guided filter (He and Sun, 2015): its coefficients at a quarter of the frame, guided by the luma;
 *   4. the composite at full size: alpha = a·luma + b (or, with a green screen, the chroma key inside the dilated
 *      person), the person with colour match and Studio Light, over a blurred copy of the frame (the person's own
 *      pixels kept out of the blur so no halo), a picture or a video frame; then the filter (a 3D LUT) and the mirror.
 * Steps 0–3 run on the CPU; step 4 on the GPU when lord-studio got a render node and GLES 3 works there (stgl.c), on
 * the CPU otherwise. Both paths give the same picture within a few levels (build/studio-test).
 *
 * Nothing here opens a file or a device: the models arrive as bytes, the render node as an fd. */
#ifndef LORD_STUDIO_ST_H
#define LORD_STUDIO_ST_H

#include <stddef.h>
#include <stdint.h>

/* the background: none, blurred, a picture, a video (a picture that changes every frame: stcam.c decodes it) */
enum { ST_MODE_NONE = 0, ST_MODE_BLUR = 1, ST_MODE_PICTURE = 2, ST_MODE_VIDEO = 3, ST_MODE_LAST = ST_MODE_VIDEO };
/* how the person is cut out: the segmenter alone, or a real green or blue screen (chroma key, plan 4.4) */
enum { ST_KEY_NONE = 0, ST_KEY_GREEN = 1, ST_KEY_BLUE = 2, ST_KEY_LAST = ST_KEY_BLUE };
/* the filters (3D LUTs built from our own parameters in stlook.c; plan 5.4) */
enum { ST_FILTER_NONE = 0, ST_FILTER_WARM, ST_FILTER_COOL, ST_FILTER_VIVID, ST_FILTER_MONO, ST_FILTER_NOIR, ST_FILTER_FILM,
       ST_FILTER_FADE, ST_FILTER_SEPIA, ST_FILTER_LAST = ST_FILTER_SEPIA };

#define ST_MAX_W 3840
#define ST_MAX_H 2160
#define ST_BLUR_MAX 100
#define ST_MATCH_MAX 60          /* colour match: at most 60 % of the way (plan 5.3) */

typedef struct {
    int mode;                    /* ST_MODE_* */
    int blur;                    /* 0..100: how strong the background blur is */
    int mirror;                  /* 1: the output is mirrored left to right */
    int key;                     /* ST_KEY_*: a green or blue screen (only with a picture or a video behind) */
    int similarity, smoothness, spill;   /* the chroma key's sliders, 0..100 each */
    int framing;                 /* Center Stage: 0 off, 1..100 from wide to tight */
    int light;                   /* Studio Light: 0 off, 1..100 its intensity */
    int match;                   /* colour match: 0..ST_MATCH_MAX % (a picture or a video behind) */
    int filter, filter_amt;      /* ST_FILTER_* and its strength 0..100 */
    int retouch;                 /* 0 off, 1..100 its strength */
} StCfg;

/* the crop Center Stage uses now, in the camera's pixels */
typedef struct { float x, y, w, h; } StRect;

typedef struct {
    double seg_ms, refine_ms, compose_ms;   /* the last frame's steps */
    double face_ms, frame_ms;               /* BlazeFace (the last time it ran) and Center Stage's crop with Retouch */
    int gpu;                     /* 1: the composite ran on the GPU */
    int frames;
} StStats;

typedef struct StFx StFx;

/* model: the segmenter's .lnn bytes (already hash-checked by lord-server); render_fd: a render node or -1 (not taken
 * over: the caller keeps it). NULL on failure, with a reason in err. */
StFx *stfx_new(const void *model, size_t len, int render_fd, char *err, size_t errlen);
/* BlazeFace's .lnn bytes (hash-checked by lord-server): Center Stage, Studio Light's face and Retouch need it; without
 * it they do nothing (Studio Light still dims the background). 0 on success. */
int   stfx_face_model(StFx *s, const void *model, size_t len, char *err, size_t errlen);
void  stfx_free(StFx *s);
/* Out-of-range values are clamped; returns 0. */
int   stfx_config(StFx *s, const StCfg *c);
/* The picture background (BGRA, any size up to ST_MAX_W×ST_MAX_H): copied; NULL clears it. 0 on success. A video
 * background is this once per frame (cheapest at exactly the frame's size: no fitting then). */
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
/* Center Stage's crop now (the whole frame when it is off) and the face it follows (cx, cy, w, h normalised to the
 * camera's frame); 1 when a face was seen within the last 2 s */
int   stfx_framing(const StFx *s, StRect *crop, float face[4]);
/* the tests: run BlazeFace every n frames instead of every 6 */
void  stfx_face_every(StFx *s, int n);
/* 1 when BlazeFace was loaded (stfx_face_model) */
int   stfx_has_faces(const StFx *s);

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
/* the largest value within radius r (a square), in place: the person's mask dilated; tmp: w·h floats */
void st_dilate(float *p, int w, int h, int r, float *tmp);

/* ---- stlook.c: the looks, exposed for the tests ---- */
#define ST_LUT_N 33
#define ST_CURVE_N 1024
/* A filter is a 3×3 matrix then a curve per channel (our own numbers, no file): the CPU applies that directly, the GPU
 * samples it as a 3D LUT. rgb 0..255 in and out. */
typedef struct { float m[9]; float curve[3][ST_CURVE_N]; } StLook;
int  st_look_build(int filter, StLook *l);           /* 0, or -1 for an unknown filter */
void st_look_apply(const StLook *l, const float in[3], float out[3]);
/* the filter's 3D LUT (ST_LUT_N³ × RGB bytes, red fastest), sampled from st_look; identity for ST_FILTER_NONE. 0 on
 * success, -1 for an unknown filter. */
int  st_lut_build(int filter, uint8_t *lut);
/* one colour through a LUT (trilinear), rgb 0..255 in and out */
void st_lut_apply(const uint8_t *lut, const float in[3], float out[3]);
/* The colour match (Reinhard, Ashikhmin, Gooch and Shirley 2001, in a linear opponent space so it is one affine map of
 * RGB): the person's and the background's statistics → m (3×3, row major) and o (3) with rgb' = m·rgb + o, at
 * strength 0..1, the exposure clamped to ±1 EV. Returns 0, or -1 when the statistics are too thin to use. */
typedef struct { double w, s[3], ss[3]; } StStat;   /* weight, Σ w·c and Σ w·c² per opponent channel */
void st_stat_add(StStat *st, const float rgb[3], float w);
int  st_match_transform(const StStat *person, const StStat *bg, float strength, float m[9], float o[3]);
/* the chroma key (plan 4.4): rgb (0..255) → 1 for the person, 0 for the screen. key: the screen's (Cb, Cr), 0..255;
 * similarity and smoothness 0..100 */
float st_chroma_alpha(const float rgb[3], const float key[2], int similarity, int smoothness);
/* the screen's (Cb, Cr): the median of the frame's border when it is that colour, else the key's preset */
void st_chroma_key(const uint8_t *bgra, int stride, int w, int h, int key, float out[2]);
/* spill suppression of the key's colour on the person, amount 0..1 */
void st_despill(float rgb[3], int key, float amount);
/* Studio Light: the person's gain at (x, y) (lifted inside the face's feathered ellipse; face cx, cy, w, h in the same
 * units, or NULL) and the background's, as factors; intensity 0..100 */
float st_light_person(float x, float y, const float *face, int intensity);
float st_light_background(int intensity);
/* Retouch: edge-preserving smoothing (a self-guided filter) of the skin inside the face's ellipse, in place on a BGRA
 * frame; face (cx, cy, w, h) in pixels; strength 0..100; *tmp (*cap floats) grows as the face needs. 0, or -1 when
 * memory ran out (the frame is then unchanged). */
int  st_retouch(uint8_t *bgra, int stride, int w, int h, const float face[4], int strength, float **tmp, size_t *cap);

/* ---- stframe.c: Center Stage, exposed for the tests ---- */
/* the 1€ filter (Casiez, Roussel and Vogel, CHI 2012) */
typedef struct { float x, dx; int primed; float mincut, beta, dcut; } StEuro;
float st_euro(StEuro *f, float x, float dt);
/* the crop that frames a face (cx, cy, w, h in pixels; eyes_y: the eyes' height in pixels) in a W×H frame: the
 * frame's shape, the eyes at a third of its height, the shoulders in; tight 1..100 (wide → tight); never smaller than
 * half the frame (2×) and always inside it */
void st_frame_target(const float face[4], float eyes_y, int W, int H, int tight, StRect *out);
/* the crop r of src (W×H) scaled to dst (W×H), bilinear */
void st_crop_scale(const uint8_t *src, int sstride, int W, int H, const StRect *r, uint8_t *dst, int dstride);

#endif
