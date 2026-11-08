/* imgops.c — the image editor's pixel operations (see imgops.h). Premultiplied ARGB in, the same out:
 * averaging premultiplied channels is what keeps transparent edges free of dark fringes. */
#include "imgops.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

int img_clip(int w, int h, float ax, float ay, float bx, float by, int *x0, int *y0, int *x1, int *y1) {
    if (w < 1 || h < 1 || !isfinite(ax) || !isfinite(ay) || !isfinite(bx) || !isfinite(by)) return 0;
    float lx = clampf(fminf(ax, bx), 0, (float)w), hx = clampf(fmaxf(ax, bx), 0, (float)w);
    float ly = clampf(fminf(ay, by), 0, (float)h), hy = clampf(fmaxf(ay, by), 0, (float)h);
    *x0 = (int)floorf(lx + 1e-4f);
    *y0 = (int)floorf(ly + 1e-4f);
    *x1 = (int)ceilf(hx - 1e-4f);
    *y1 = (int)ceilf(hy - 1e-4f);
    return *x1 > *x0 && *y1 > *y0;
}

void img_fill(uint32_t *px, int w, int h, float ax, float ay, float bx, float by, uint32_t argb) {
    int x0, y0, x1, y1;
    if (!px || !img_clip(w, h, ax, ay, bx, by, &x0, &y0, &x1, &y1)) return;
    uint32_t c = argb | 0xff000000u;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) px[(size_t)y * w + x] = c;
}

void img_pixelate(uint32_t *px, int w, int h, float ax, float ay, float bx, float by, int block) {
    int x0, y0, x1, y1;
    if (!px || !img_clip(w, h, ax, ay, bx, by, &x0, &y0, &x1, &y1)) return;
    if (block < 2) block = 2;
    if (block > 512) block = 512;
    for (int cy = y0; cy < y1; cy += block)
        for (int cx = x0; cx < x1; cx += block) {
            int ex = cx + block < x1 ? cx + block : x1, ey = cy + block < y1 ? cy + block : y1;
            uint64_t s[4] = { 0, 0, 0, 0 };
            for (int y = cy; y < ey; y++)
                for (int x = cx; x < ex; x++) {
                    uint32_t p = px[(size_t)y * w + x];
                    s[0] += p >> 24; s[1] += (p >> 16) & 255; s[2] += (p >> 8) & 255; s[3] += p & 255;
                }
            uint64_t n = (uint64_t)(ex - cx) * (uint64_t)(ey - cy), half = n / 2;
            uint32_t a = (uint32_t)((s[0] + half) / n), r = (uint32_t)((s[1] + half) / n);
            uint32_t g = (uint32_t)((s[2] + half) / n), b = (uint32_t)((s[3] + half) / n);
            if (r > a) r = a;           /* premultiplied: no channel above alpha */
            if (g > a) g = a;
            if (b > a) b = a;
            uint32_t c = a << 24 | r << 16 | g << 8 | b;
            for (int y = cy; y < ey; y++)
                for (int x = cx; x < ex; x++) px[(size_t)y * w + x] = c;
        }
}

/* ---- resize: one set of taps per axis ---- */

typedef struct { int start, n; } Tap;

/* Taps for s → d: per output pixel, `n` source pixels from `start`, with weights in wt (summing to 1).
 * per = the most taps an output pixel uses. */
static int make_taps(int s, int d, Tap **taps, float **wt, int *per) {
    double scale = (double)s / d;
    int p = scale > 1 ? (int)ceil(scale) + 1 : 2;
    Tap *t = calloc((size_t)d, sizeof *t);
    float *w = calloc((size_t)d * p, sizeof *w);
    if (!t || !w) { free(t); free(w); return -1; }
    for (int i = 0; i < d; i++) {
        float *wi = w + (size_t)i * p;
        if (scale > 1) {                        /* area: the source interval [a, b) */
            double a = i * scale, b = (i + 1) * scale;
            int j0 = (int)floor(a), j1 = (int)ceil(b);
            if (j1 > s) j1 = s;
            if (j1 - j0 > p) j1 = j0 + p;
            double sum = 0;
            for (int j = j0; j < j1; j++) {
                double cov = fmin(b, j + 1) - fmax(a, j);
                wi[j - j0] = (float)(cov > 0 ? cov : 0);
                sum += wi[j - j0];
            }
            for (int j = j0; j < j1; j++) wi[j - j0] = (float)(sum > 0 ? wi[j - j0] / sum : 0);
            t[i] = (Tap){ j0, j1 - j0 };
        } else {                                /* bilinear between the two nearest centers */
            double c = (i + 0.5) * scale - 0.5;
            int j0 = (int)floor(c);
            double f = c - j0;
            if (j0 < 0) { j0 = 0; f = 0; }
            if (j0 >= s - 1) { j0 = s - 1; f = 0; }
            int n = f > 0 && j0 + 1 < s ? 2 : 1;
            wi[0] = (float)(n == 2 ? 1 - f : 1);
            if (n == 2) wi[1] = (float)f;
            t[i] = (Tap){ j0, n };
        }
    }
    *taps = t;
    *wt = w;
    *per = p;
    return 0;
}

static uint32_t pack(const float *a) {
    uint32_t v[4];
    for (int k = 0; k < 4; k++) { float f = a[k] + 0.5f; v[k] = f <= 0 ? 0 : f >= 255 ? 255 : (uint32_t)f; }
    for (int k = 1; k < 4; k++) if (v[k] > v[0]) v[k] = v[0];
    return v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3];
}

int img_resize(const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh) {
    if (!src || !dst || sw < 1 || sh < 1 || dw < 1 || dh < 1 || sw > IMG_MAX_DIM || sh > IMG_MAX_DIM || dw > IMG_MAX_DIM || dh > IMG_MAX_DIM) return -1;
    if (sw == dw && sh == dh) { memcpy(dst, src, (size_t)sw * sh * 4); return 0; }
    Tap *tx = NULL, *ty = NULL;
    float *wx = NULL, *wy = NULL, *acc = NULL;
    uint32_t *tmp = NULL;
    int px_, py_, r = -1;
    if (make_taps(sw, dw, &tx, &wx, &px_) || make_taps(sh, dh, &ty, &wy, &py_)) goto out;
    tmp = malloc((size_t)dw * sh * 4);          /* horizontal pass: dw × sh */
    acc = malloc((size_t)dw * 4 * sizeof *acc);
    if (!tmp || !acc) goto out;
    for (int y = 0; y < sh; y++) {
        const uint32_t *row = src + (size_t)y * sw;
        for (int x = 0; x < dw; x++) {
            float a[4] = { 0, 0, 0, 0 };
            const float *w = wx + (size_t)x * px_;
            for (int k = 0; k < tx[x].n; k++) {
                uint32_t p = row[tx[x].start + k];
                a[0] += w[k] * (float)(p >> 24); a[1] += w[k] * (float)((p >> 16) & 255);
                a[2] += w[k] * (float)((p >> 8) & 255); a[3] += w[k] * (float)(p & 255);
            }
            tmp[(size_t)y * dw + x] = pack(a);
        }
    }
    for (int y = 0; y < dh; y++) {                /* vertical pass, a row of accumulators at a time */
        memset(acc, 0, (size_t)dw * 4 * sizeof *acc);
        const float *w = wy + (size_t)y * py_;
        for (int k = 0; k < ty[y].n; k++) {
            const uint32_t *row = tmp + (size_t)(ty[y].start + k) * dw;
            float f = w[k];
            for (int x = 0; x < dw; x++) {
                uint32_t p = row[x];
                float *a = acc + (size_t)x * 4;
                a[0] += f * (float)(p >> 24); a[1] += f * (float)((p >> 16) & 255);
                a[2] += f * (float)((p >> 8) & 255); a[3] += f * (float)(p & 255);
            }
        }
        for (int x = 0; x < dw; x++) dst[(size_t)y * dw + x] = pack(acc + (size_t)x * 4);
    }
    r = 0;
out:
    free(tx); free(ty); free(wx); free(wy); free(tmp); free(acc);
    return r;
}

void img_rotated_size(int w, int h, int turns, int *ow, int *oh) {
    int t = ((turns % 4) + 4) % 4;
    *ow = t & 1 ? h : w;
    *oh = t & 1 ? w : h;
}

void img_rotate(const uint32_t *src, int w, int h, int turns, uint32_t *dst) {
    int t = ((turns % 4) + 4) % 4, ow, oh;
    img_rotated_size(w, h, t, &ow, &oh);
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            int sx = x, sy = y;                   /* the same mapping as lord-media's edit job */
            if (t == 1) { sx = y; sy = h - 1 - x; }
            else if (t == 2) { sx = w - 1 - x; sy = h - 1 - y; }
            else if (t == 3) { sx = w - 1 - y; sy = x; }
            dst[(size_t)y * ow + x] = src[(size_t)sy * w + sx];
        }
}

/* How much of the pixel (x, y) lies inside the quarter circle of radius r centered at (cx, cy): 4 × 4
 * samples, like the cursor and the rasterizer's edges. */
static float corner_cover(int x, int y, float cx, float cy, float r) {
    int in = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            float dx = x + (sx + 0.5f) / 4 - cx, dy = y + (sy + 0.5f) / 4 - cy;
            in += dx * dx + dy * dy <= r * r;
        }
    return in / 16.0f;
}

void img_round_corners(uint32_t *px, int w, int h, int r) {
    if (!px || w < 1 || h < 1 || r <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int y = 0; y < r; y++)
        for (int x = 0; x < r; x++) {
            /* the top-left corner's pixel; the other three are its mirror images */
            float k = corner_cover(x, y, (float)r, (float)r, (float)r);
            if (k >= 1) continue;
            int at[4][2] = { { x, y }, { w - 1 - x, y }, { x, h - 1 - y }, { w - 1 - x, h - 1 - y } };
            for (int i = 0; i < 4; i++) {
                uint32_t *p = &px[(size_t)at[i][1] * w + at[i][0]];
                uint32_t v = *p;               /* premultiplied: every channel scales with the coverage */
                uint32_t a = (uint32_t)((v >> 24) * k + 0.5f), cr = (uint32_t)(((v >> 16) & 255) * k + 0.5f);
                uint32_t cg = (uint32_t)(((v >> 8) & 255) * k + 0.5f), cb = (uint32_t)((v & 255) * k + 0.5f);
                *p = a << 24 | cr << 16 | cg << 8 | cb;
            }
        }
}
