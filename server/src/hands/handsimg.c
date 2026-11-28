/* handsimg.c — a Hands screenshot as PNG (hands/handsimg.h): fitted to the size the agent asked for (an area average
 * when it shrinks, the nearest pixel when it grows) and written with vendored stb_image_write. */
#include "handsimg.h"

#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "stb_image_write.h"
#pragma GCC diagnostic pop
#pragma GCC diagnostic ignored "-Wunused-function"      /* the writer's other formats, reported at the end of the file */

void hands_fit(int w, int h, int maxdim, int grow, int *ow, int *oh) {
    double k = 1;
    int big = w > h ? w : h;
    if (maxdim > 0 && big > maxdim) k = (double)maxdim / big;
    else if (grow && maxdim > 0 && big < maxdim) {
        k = (double)maxdim / big;
        if (k > 4) k = 4;
    }
    *ow = (int)(w * k + 0.5);
    *oh = (int)(h * k + 0.5);
    if (*ow < 1) *ow = 1;
    if (*oh < 1) *oh = 1;
}

uint8_t *hands_scale_rgb(const uint32_t *px, int stride, int w, int h, int ow, int oh) {
    if (!px || w < 1 || h < 1 || ow < 1 || oh < 1 || (size_t)ow * oh > (size_t)8192 * 8192) return NULL;
    uint8_t *rgb = malloc((size_t)ow * oh * 3);
    if (!rgb) return NULL;
    for (int y = 0; y < oh; y++) {
        int y0 = (int)((long long)y * h / oh), y1 = (int)((long long)(y + 1) * h / oh);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > h) y1 = h;
        for (int x = 0; x < ow; x++) {
            int x0 = (int)((long long)x * w / ow), x1 = (int)((long long)(x + 1) * w / ow);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > w) x1 = w;
            unsigned long r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint32_t v = px[(size_t)yy * stride + xx];
                    r += (v >> 16) & 255;
                    g += (v >> 8) & 255;
                    b += v & 255;
                    n++;
                }
            uint8_t *o = rgb + ((size_t)y * ow + x) * 3;
            o[0] = (uint8_t)(r / n);
            o[1] = (uint8_t)(g / n);
            o[2] = (uint8_t)(b / n);
        }
    }
    return rgb;
}

uint8_t *hands_png(const uint8_t *rgb, int w, int h, size_t *len) {
    *len = 0;
    if (!rgb || w < 1 || h < 1) return NULL;
    stbi_write_png_compression_level = 3;
    int n = 0;
    unsigned char *png = stbi_write_png_to_mem(rgb, w * 3, w, h, 3, &n);
    if (!png || n <= 0) { free(png); return NULL; }
    *len = (size_t)n;
    return png;
}
