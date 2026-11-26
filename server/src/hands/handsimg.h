/* handsimg.h — a Hands screenshot's pixels as PNG (handsimg.c). Pure: tests/hands_test.c checks the fit and the scale. */
#ifndef LORD_HANDS_HANDSIMG_H
#define LORD_HANDS_HANDSIMG_H

#include <stddef.h>
#include <stdint.h>

/* the size w × h becomes with its longest side at most maxdim (or, grow, at least maxdim, at most 4 times larger) */
void hands_fit(int w, int h, int maxdim, int grow, int *ow, int *oh);
/* 0xAARRGGBB pixels (stride in pixels) as RGB bytes of ow × oh, malloc'd */
uint8_t *hands_scale_rgb(const uint32_t *px, int stride, int w, int h, int ow, int oh);
/* RGB as a PNG file in memory (malloc'd), its size in *len; NULL on failure */
uint8_t *hands_png(const uint8_t *rgb, int w, int h, size_t *len);

#endif
