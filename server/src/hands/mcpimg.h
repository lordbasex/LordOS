/* mcpimg.h — a PNG back to RGB for lordos-mcp's recordings (mcpimg.c) */
#ifndef LORD_HANDS_MCPIMG_H
#define LORD_HANDS_MCPIMG_H

#include <stddef.h>
#include <stdint.h>

uint8_t *mcp_png_rgb(const uint8_t *png, size_t n, int *w, int *h);   /* RGB, w × h; mcp_png_free it */
void mcp_png_free(uint8_t *p);

#endif
