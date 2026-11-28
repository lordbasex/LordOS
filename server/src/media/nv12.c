/* nv12.c — the colour matrices of NV12 frames and their CPU conversion (nv12.h). */
#include "nv12.h"
#include "proto.h"

void nv12_matrix(int matrix, int full, float m[12]) {
    double kr = 0.299, kb = 0.114;
    if (matrix == MC_709) { kr = 0.2126; kb = 0.0722; }
    else if (matrix == MC_2020) { kr = 0.2627; kb = 0.0593; }
    double kg = 1 - kr - kb;
    /* bytes as 0..1: Y' = (Y - yo) · ys, C' = (C - 128/255) · cs */
    double ys = full ? 1.0 : 255.0 / 219.0, yo = full ? 0.0 : 16.0 / 255.0, cs = full ? 255.0 / 255.0 : 255.0 / 224.0, co = 128.0 / 255.0;
    double rv = 2 * (1 - kr), gu = -2 * kb * (1 - kb) / kg, gv = -2 * kr * (1 - kr) / kg, bu = 2 * (1 - kb);
    double r[4] = { ys, 0, rv * cs, 0 }, g[4] = { ys, gu * cs, gv * cs, 0 }, b[4] = { ys, bu * cs, 0, 0 };
    double *rows[3] = { r, g, b };
    for (int i = 0; i < 3; i++) {
        double *q = rows[i];
        q[3] = -yo * ys - co * (q[1] + q[2]);
        for (int k = 0; k < 4; k++) m[i * 4 + k] = (float)q[k];
    }
}

static inline uint32_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v; }

void nv12_to_argb(const uint8_t *yp, int ystride, const uint8_t *uvp, int uvstride, int w, int h, int matrix, int full,
                  uint32_t *out, int ostride, int y0, int y1) {
    if (!yp || !uvp || !out || w < 2 || h < 2 || ystride < w || uvstride < w) return;
    if (y0 < 0) y0 = 0;
    if (y1 > h) y1 = h;
    float mf[12];
    nv12_matrix(matrix, full, mf);
    /* 16.16 fixed point on byte values: out = (Σ m·byte + m3·255) */
    int m[12];
    for (int i = 0; i < 12; i++) m[i] = (int)(mf[i] * 65536.0f * (i % 4 == 3 ? 255.0f : 1.0f) + (mf[i] >= 0 ? 0.5f : -0.5f));
    for (int y = y0; y < y1; y++) {
        const uint8_t *yr = yp + (size_t)y * ystride, *cr = uvp + (size_t)(y / 2) * uvstride;
        uint32_t *o = out + (size_t)y * ostride;
        for (int x = 0; x < w; x++) {
            int Y = yr[x], U = cr[x & ~1], V = cr[(x & ~1) + 1];
            int R = (m[0] * Y + m[1] * U + m[2] * V + m[3] + 32768) >> 16;
            int G = (m[4] * Y + m[5] * U + m[6] * V + m[7] + 32768) >> 16;
            int B = (m[8] * Y + m[9] * U + m[10] * V + m[11] + 32768) >> 16;
            o[x] = 0xff000000u | clamp8(R) << 16 | clamp8(G) << 8 | clamp8(B);
        }
    }
}
