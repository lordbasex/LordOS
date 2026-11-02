/* draw.h — lordos-testbench's pixel drawing (see draw.c). */
#ifndef LORD_TESTBENCH_DRAW_H
#define LORD_TESTBENCH_DRAW_H

#include <stdint.h>

typedef struct { uint32_t *px; int w, h, stride; } Surf;   /* XRGB; stride in pixels */

#define TB_RGB(r, g, b) (0xff000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

void d_fill(Surf *s, int x, int y, int w, int h, uint32_t c);
void d_frame(Surf *s, int x, int y, int w, int h, int thick, uint32_t c);
void d_circle(Surf *s, int cx, int cy, int r, uint32_t c);
int  d_text(Surf *s, int x, int y, int scale, uint32_t c, const char *t);   /* returns the width drawn */
int  d_text_w(const char *t, int scale);
void d_text_c(Surf *s, int cx, int cy, int scale, uint32_t c, const char *t);

#endif
