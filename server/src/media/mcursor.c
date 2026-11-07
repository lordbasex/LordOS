/* mcursor.c — the pointer image a media node's source brings with it (a Machines guest's CursorDefine): made
 * ready for lord-server's cursor (premultiplied ARGB, see cursor.h) and drawn at the size the picture is shown.
 * Pure functions, no state: tests/mcursor_test.c. */
#include "mcursor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int mcur_prepare(const uint8_t *bgra, int w, int h, int hx, int hy, McurImage *out) {
    memset(out, 0, sizeof *out);
    if (!bgra || w <= 0 || h <= 0 || w > MCUR_MAX_IN || h > MCUR_MAX_IN || hx < 0 || hy < 0 || hx >= w || hy >= h) return -1;
    /* what isn't fully transparent; a guest cursor is usually 64 × 64 with a small arrow in a corner */
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (bgra[((size_t)y * w + x) * 4 + 3]) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (x1 < 0) return 1;                       /* nothing visible: the guest hides its pointer this way */
    /* the hotspot stays inside the image */
    if (hx < x0) x0 = hx;
    if (hx > x1) x1 = hx;
    if (hy < y0) y0 = hy;
    if (hy > y1) y1 = hy;
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    uint32_t *px = malloc((size_t)cw * ch * 4);
    if (!px) return -1;
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) {
            const uint8_t *p = bgra + ((size_t)(y + y0) * w + (x + x0)) * 4;
            uint32_t a = p[3];
            uint32_t r = (p[2] * a + 127) / 255, g = (p[1] * a + 127) / 255, b = (p[0] * a + 127) / 255;
            px[(size_t)y * cw + x] = a << 24 | r << 16 | g << 8 | b;
        }
    *out = (McurImage){ px, cw, ch, hx - x0, hy - y0 };
    return 0;
}

float mcur_fit_scale(int w, int h, float f, int max) {
    if (!(f > 0) || w <= 0 || h <= 0) return 1;
    if (f > 8) f = 8;
    int big = w > h ? w : h;
    if (max > 0 && big * f > max) f = (float)max / big;
    /* within 5 % of 1: drawn as it is, crisp */
    if (fabsf(f - 1) < 0.05f) f = 1;
    return f;
}

int mcur_scale(const McurImage *in, float f, McurImage *out) {
    memset(out, 0, sizeof *out);
    if (!in || !in->px || in->w <= 0 || in->h <= 0 || !(f > 0)) return -1;
    int w = (int)lroundf(in->w * f), h = (int)lroundf(in->h * f);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) return -1;
    if (w == in->w && h == in->h) memcpy(px, in->px, (size_t)w * h * 4);
    else
        for (int y = 0; y < h; y++) {
            /* bilinear on premultiplied pixels (no dark fringe) */
            float v = (y + 0.5f) * in->h / h - 0.5f;
            if (v < 0) v = 0;
            int y0 = (int)v, y1 = y0 + 1 < in->h ? y0 + 1 : y0;
            uint32_t fy = (uint32_t)((v - y0) * 256);
            for (int x = 0; x < w; x++) {
                float u = (x + 0.5f) * in->w / w - 0.5f;
                if (u < 0) u = 0;
                int x0 = (int)u, x1 = x0 + 1 < in->w ? x0 + 1 : x0;
                uint32_t fx = (uint32_t)((u - x0) * 256);
                uint32_t a = in->px[(size_t)y0 * in->w + x0], b = in->px[(size_t)y0 * in->w + x1];
                uint32_t c = in->px[(size_t)y1 * in->w + x0], d = in->px[(size_t)y1 * in->w + x1], o = 0;
                for (int k = 0; k < 32; k += 8) {
                    uint32_t top = ((a >> k) & 255) * (256 - fx) + ((b >> k) & 255) * fx;
                    uint32_t bot = ((c >> k) & 255) * (256 - fx) + ((d >> k) & 255) * fx;
                    o |= ((top * (256 - fy) + bot * fy + 32768) >> 16) << k;
                }
                px[(size_t)y * w + x] = o;
            }
        }
    int hx = (int)floorf(in->hx * f + f / 2), hy = (int)floorf(in->hy * f + f / 2);
    if (f == 1) { hx = in->hx; hy = in->hy; }
    if (hx >= w) hx = w - 1;
    if (hy >= h) hy = h - 1;
    *out = (McurImage){ px, w, h, hx, hy };
    return 0;
}

void mcur_free(McurImage *m) {
    free(m->px);
    memset(m, 0, sizeof *m);
}
