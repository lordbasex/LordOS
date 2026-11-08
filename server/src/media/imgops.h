/* imgops.h — pixel operations of the image editor (the markup editor of Screenshots, also opened on
 * any picture from Photos and Files): resize, quarter turns, solid fill and pixelate, over
 * premultiplied ARGB in lord-server's memory. Pure functions with no other dependency, tested by
 * build/imgops-test (tests/imgops_test.c). */
#ifndef LORD_IMGOPS_H
#define LORD_IMGOPS_H

#include <stdint.h>

#define IMG_MAX_DIM 8192            /* same bound as MP_MAX_DIM (a decoded image's largest side) */

/* The rectangle between two corners, rounded outwards and clamped to w × h:
 * [*x0, *x1) × [*y0, *y1). Returns 0 when nothing is left (or a coordinate is not finite). */
int img_clip(int w, int h, float ax, float ay, float bx, float by, int *x0, int *y0, int *x1, int *y1);

/* An opaque color (ARGB, alpha forced to 255) over the rectangle: redaction that hides everything. */
void img_fill(uint32_t *px, int w, int h, float ax, float ay, float bx, float by, uint32_t argb);

/* Every block × block cell of the rectangle (cells start at its corner) becomes its average color. */
void img_pixelate(uint32_t *px, int w, int h, float ax, float ay, float bx, float by, int block);

/* src (sw × sh) → dst (dw × dh): area average when shrinking, bilinear when enlarging, per axis.
 * Sides 1..IMG_MAX_DIM. Returns 0, or -1 on bad sizes or no memory (dst is then untouched). */
int img_resize(const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh);

/* Quarter turns clockwise (0..3; other values are taken mod 4). dst is h × w for odd turns. */
void img_rotate(const uint32_t *src, int w, int h, int turns, uint32_t *dst);

/* The size after `turns` (odd ones swap the sides). */
void img_rotated_size(int w, int h, int turns, int *ow, int *oh);

/* The picture's own shape as a rectangle with rounded corners of radius r (px, at most half the shorter
 * side): what falls outside becomes transparent, the edge antialiased (a window capture, issue #70).
 * r <= 0 does nothing. */
void img_round_corners(uint32_t *px, int w, int h, int r);

#endif
