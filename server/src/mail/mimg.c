/* mimg.c — images for Mail and Contacts, decoded inside the sandboxed axia-maild (never in
 * axia-server): PNG, JPEG, GIF (first frame) and BMP with stb_image, scaled down to fit and
 * returned as premultiplied ARGB32 like every image the LordOS renderer takes. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

uint32_t *img_decode(const unsigned char *data, size_t len, int maxw, int maxh, int *ow, int *oh);
unsigned char *img_thumb_jpeg(const unsigned char *data, size_t len, int max, size_t *olen);

uint32_t *img_decode(const unsigned char *data, size_t len, int maxw, int maxh, int *ow, int *oh) {
    if (!data || !len || len > (64u << 20)) return NULL;
    int w, h, comp;
    if (!stbi_info_from_memory(data, (int)len, &w, &h, &comp) || w <= 0 || h <= 0 || (long long)w * h > 40000000LL) return NULL;
    unsigned char *px = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
    if (!px) return NULL;
    /* scale (box filter) so it fits maxw x maxh */
    double s = 1.0;
    if (w > maxw) s = (double)maxw / w;
    if (h * s > maxh) s = (double)maxh / h;
    int nw = (int)(w * s + 0.5), nh = (int)(h * s + 0.5);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    uint32_t *out = malloc((size_t)nw * (size_t)nh * 4);
    if (!out) { stbi_image_free(px); return NULL; }
    for (int y = 0; y < nh; y++) {
        int y0 = (int)((double)y / s), y1 = (int)((double)(y + 1) / s);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > h) y1 = h;
        for (int x = 0; x < nw; x++) {
            int x0 = (int)((double)x / s), x1 = (int)((double)(x + 1) / s);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > w) x1 = w;
            unsigned long r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = y0; yy < y1; yy += (y1 - y0 > 4 ? (y1 - y0) / 4 : 1))
                for (int xx = x0; xx < x1; xx += (x1 - x0 > 4 ? (x1 - x0) / 4 : 1)) {
                    const unsigned char *p = px + ((size_t)yy * (size_t)w + (size_t)xx) * 4;
                    unsigned long pa = p[3];
                    r += p[0] * pa; g += p[1] * pa; b += p[2] * pa; a += pa; n++;
                }
            if (!n) n = 1;
            unsigned long A = a / n;
            unsigned long R = r / n / 255, G = g / n / 255, B = b / n / 255;   /* premultiplied */
            out[(size_t)y * (size_t)nw + (size_t)x] = (uint32_t)(A << 24 | R << 16 | G << 8 | B);
        }
    }
    stbi_image_free(px);
    *ow = nw;
    *oh = nh;
    return out;
}

/* A contact photo: decoded, scaled to fit max x max and re-encoded as JPEG (so a photo from
 * anywhere becomes a small, plain JPEG before it is stored or synced). */
typedef struct { unsigned char *p; size_t n, cap; } WBuf;
static void wcb(void *ctx, void *data, int size) {
    WBuf *w = ctx;
    if (size <= 0) return;
    if (w->n + (size_t)size > w->cap) {
        size_t nc = w->cap ? w->cap * 2 : 65536;
        while (nc < w->n + (size_t)size) nc *= 2;
        unsigned char *np = realloc(w->p, nc);
        if (!np) return;
        w->p = np;
        w->cap = nc;
    }
    memcpy(w->p + w->n, data, (size_t)size);
    w->n += (size_t)size;
}

unsigned char *img_thumb_jpeg(const unsigned char *data, size_t len, int max, size_t *olen) {
    int w, h;
    uint32_t *px = img_decode(data, len, max, max, &w, &h);
    if (!px) return NULL;
    unsigned char *rgb = malloc((size_t)w * (size_t)h * 3);
    if (!rgb) { free(px); return NULL; }
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        uint32_t p = px[i];
        unsigned a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        /* over white (contact photos have no transparency in vCard) */
        rgb[i * 3] = (unsigned char)(r + 255 - a);
        rgb[i * 3 + 1] = (unsigned char)(g + 255 - a);
        rgb[i * 3 + 2] = (unsigned char)(b + 255 - a);
    }
    free(px);
    WBuf wb = { 0 };
    int ok = stbi_write_jpg_to_func(wcb, &wb, w, h, 3, rgb, 88);
    free(rgb);
    if (!ok || !wb.p) { free(wb.p); return NULL; }
    *olen = wb.n;
    return wb.p;
}
