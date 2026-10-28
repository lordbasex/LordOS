/* canvas.h — our own 2D software rasterizer (XRGB8888). */
#ifndef LORD_CANVAS_H
#define LORD_CANVAS_H

#include <stdint.h>

typedef struct { int x0, y0, x1, y1; } Rect;

typedef struct {
    uint32_t *px;
    int w, h, stride;   /* stride in pixels */
    Rect clip;
} Canvas;

enum { FIT_FILL, FIT_COVER, FIT_CONTAIN };

/* Optional: run row-local pixel loops on a pool (lord-server passes par_rows, see par.h); same pixels. */
typedef void (*CanvasPar)(int j0, int j1, void (*fn)(void *ctx, int a, int b), void *ctx);
void canvas_parallel(CanvasPar fn);
int  canvas_parallel_on(void);       /* the pool is in use now (heavy frames) */

/* Colors as 0xAARRGGBB; AA = opacity. Float coordinates with antialiasing. */
void canvas_init(Canvas *c, uint32_t *px, int w, int h, int stride);
Rect canvas_push_clip(Canvas *c, float x, float y, float w, float h);   /* returns the previous clip */
void canvas_set_clip(Canvas *c, Rect r);
void canvas_copy(Canvas *dst, const Canvas *src);
void canvas_clear(Canvas *c, uint32_t rgb);        /* only inside the clip */

void canvas_rrect(Canvas *c, float x, float y, float w, float h, float r, uint32_t argb);
void canvas_rrect_gradient(Canvas *c, float x, float y, float w, float h, float r,
                           uint32_t from, uint32_t to, float angle_deg, float opacity);
void canvas_rrect_stroke(Canvas *c, float x, float y, float w, float h, float r, float bw, uint32_t argb);
/* CSS box-shadow style shadow: not painted under the box. */
void canvas_shadow(Canvas *c, float x, float y, float w, float h, float r,
                   float dx, float dy, float blur, uint32_t argb);
/* backdrop-filter: blur() over what is already drawn, clipped to the rounded rectangle. */
void canvas_backdrop_blur(Canvas *c, float x, float y, float w, float h, float r, int radius);

/* Corners: save before painting a node with overflow:hidden and restore afterwards. */
typedef struct { int x[4], y[4], n; uint32_t *buf; } Corners;
void canvas_corners_save(Canvas *c, Corners *k, float x, float y, float w, float h, float r);
void canvas_corners_restore(Canvas *c, Corners *k, float x, float y, float w, float h, float r);

/* Per-corner radii r[4] = top-left, top-right, bottom-right, bottom-left (radii that are all equal go
 * through the functions above, which are faster). */
void canvas_rrect4(Canvas *c, float x, float y, float w, float h, const float r[4], uint32_t argb);
void canvas_rrect4_gradient(Canvas *c, float x, float y, float w, float h, const float r[4],
                            uint32_t from, uint32_t to, float angle_deg, float opacity);
/* Border with a width and a color per side (bw/col: left, top, right, bottom) and per-corner radii.
 * Sides meet on the diagonal of each corner, as in CSS. */
void canvas_border4(Canvas *c, float x, float y, float w, float h, const float r[4], const float bw[4], const uint32_t col[4]);
void canvas_corners_save4(Canvas *c, Corners *k, float x, float y, float w, float h, const float r[4]);
void canvas_corners_restore4(Canvas *c, Corners *k, float x, float y, float w, float h, const float r[4]);
/* Affine copy: destination pixel p takes the source at inv(p) (bilinear, transparent outside), a
 * premultiplied w × h image; inv maps canvas pixels to source pixels. */
void canvas_blit_affine(Canvas *c, const uint32_t *src, int sw, int sh, const float inv[6], const float fwd[6], float opacity);
/* partial repaints: watch canvas c for an affine image cut by its left edge x (canvas.c) */
void canvas_affine_watch(const Canvas *c, int x);
int  canvas_affine_cut(void);           /* 1 if one was cut since canvas_affine_watch; stops watching */

/* 8-bit coverage mask (glyphs). */
void canvas_alpha_mask(Canvas *c, int x, int y, const uint8_t *a, int w, int h, int stride, uint32_t argb);
/* Premultiplied ARGB image, with rounded corners and opacity. */
void canvas_blit(Canvas *c, const uint32_t *src, int sw, int sh, int x, int y, float r, float opacity, int opaque);
uint32_t canvas_g0_model(uint32_t src, uint32_t dst);   /* G0's shader in C: src (valid premultiplied) over dst, as blit_row at k = 256 */
/* Character bitmap: 'X' = fg, 'o' = bg, rest transparent (cursor). */
/* Scaled, bilinear: source rect (sx, sy, sw, sh) of an image sw_px × sh_px (stride in pixels) into the
 * destination rect (dx, dy, dw, dh). Wayland buffers at another scale and wp_viewporter (CPU path). */
void canvas_blit_scaled(Canvas *c, const uint32_t *src, int stride, int sw_px, int sh_px, float sx, float sy, float sw, float sh,
                        float dx, float dy, float dw, float dh, float opacity, int opaque);
void canvas_bitmap(Canvas *c, int x, int y, const char *const *rows, uint32_t fg, uint32_t bg);

#endif
