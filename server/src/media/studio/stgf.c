/* stgf.c — the matte's pieces (see st.h): downscaling, temporal smoothing, the fast guided filter and box blurs.
 * The guided filter is K. He, J. Sun and X. Tang, "Guided Image Filtering" (ECCV 2010, TPAMI 2013) and its fast
 * subsampled form (He and Sun, 2015); the algorithm is free to implement and this is our own code. */
#include "st.h"

#include <math.h>
#include <string.h>

static inline float lum(const uint8_t *p) { return (0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2]) * (1.0f / 255); }

/* the source columns [x0, x1) that output column ox averages */
static inline void span(int o, int on, int n, int *a, int *b) {
    *a = (int)((long long)o * n / on);
    *b = (int)((long long)(o + 1) * n / on);
    if (*b <= *a) *b = *a + 1;
    if (*b > n) *b = n;
}

void st_downscale_rgb(const uint8_t *bgra, int stride, int w, int h, float *rgb, int ow, int oh) {
    for (int oy = 0; oy < oh; oy++) {
        int y0, y1;
        span(oy, oh, h, &y0, &y1);
        for (int ox = 0; ox < ow; ox++) {
            int x0, x1;
            span(ox, ow, w, &x0, &x1);
            unsigned r = 0, g = 0, b = 0;
            for (int y = y0; y < y1; y++) {
                const uint8_t *p = bgra + (size_t)y * stride + (size_t)x0 * 4;
                for (int x = x0; x < x1; x++, p += 4) { b += p[0]; g += p[1]; r += p[2]; }
            }
            float k = 1.0f / (255.0f * (float)((y1 - y0) * (x1 - x0)));
            float *o = rgb + ((size_t)oy * ow + ox) * 3;
            o[0] = (float)r * k;
            o[1] = (float)g * k;
            o[2] = (float)b * k;
        }
    }
}

void st_downscale_luma(const uint8_t *bgra, int stride, int w, int h, float *yp, int ow, int oh) {
    for (int oy = 0; oy < oh; oy++) {
        int y0, y1;
        span(oy, oh, h, &y0, &y1);
        for (int ox = 0; ox < ow; ox++) {
            int x0, x1;
            span(ox, ow, w, &x0, &x1);
            float s = 0;
            for (int y = y0; y < y1; y++) {
                const uint8_t *p = bgra + (size_t)y * stride + (size_t)x0 * 4;
                for (int x = x0; x < x1; x++, p += 4) s += lum(p);
            }
            yp[(size_t)oy * ow + ox] = s / (float)((y1 - y0) * (x1 - x0));
        }
    }
}

void st_resize_plane(const float *src, int sw, int sh, float *dst, int dw, int dh) {
    float sx = (float)sw / dw, sy = (float)sh / dh;
    for (int y = 0; y < dh; y++) {
        float fy = (y + 0.5f) * sy - 0.5f;
        if (fy < 0) fy = 0;
        int y0 = (int)fy;
        if (y0 > sh - 1) y0 = sh - 1;
        int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        float ly = fy - (float)y0;
        if (ly > 1) ly = 1;
        const float *r0 = src + (size_t)y0 * sw, *r1 = src + (size_t)y1 * sw;
        for (int x = 0; x < dw; x++) {
            float fx = (x + 0.5f) * sx - 0.5f;
            if (fx < 0) fx = 0;
            int x0 = (int)fx;
            if (x0 > sw - 1) x0 = sw - 1;
            int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            float lx = fx - (float)x0;
            if (lx > 1) lx = 1;
            float top = r0[x0] + (r0[x1] - r0[x0]) * lx, bot = r1[x0] + (r1[x1] - r1[x0]) * lx;
            dst[(size_t)y * dw + x] = top + (bot - top) * ly;
        }
    }
}

static inline float smooth(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

/* Moving parts follow the network quickly (weight 0.7), still parts slowly (0.2), so a still edge doesn't shimmer
 * and a moving hand doesn't trail. The output then goes through a soft threshold whose centre depends on the
 * previous output (0.45 for what was the person, 0.55 for what wasn't): that hysteresis keeps edges from flickering. */
void st_temporal(float *state, const float *raw, const float *luma, float *prev_luma, int w, int h, int first) {
    size_t n = (size_t)w * h;
    if (first) {
        for (size_t i = 0; i < n; i++) state[i] = smooth(0.4f, 0.6f, raw[i]);
        memcpy(prev_luma, luma, n * sizeof(float));
        return;
    }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        size_t i = (size_t)y * w + x;
        float d = fabsf(luma[i] - prev_luma[i]);
        float a = 0.2f + 0.5f * (d * 12 > 1 ? 1 : d * 12);
        float prev = state[i];
        float m = prev + (raw[i] - prev) * a;
        float c = prev >= 0.5f ? 0.45f : 0.55f;
        state[i] = smooth(c - 0.15f, c + 0.15f, m);
    }
    memcpy(prev_luma, luma, n * sizeof(float));
}

/* a box mean of radius r over a w×h plane (clamped window: divided by the window's real size), O(n). The vertical
 * pass keeps one running row so its loops run along memory and vectorise. tmp: w·h + w floats. */
static void box_mean(const float *src, float *dst, int w, int h, int r, float *tmp) {
    for (int y = 0; y < h; y++) {                 /* rows into tmp */
        const float *s = src + (size_t)y * w;
        float *t = tmp + (size_t)y * w;
        float acc = 0;
        int lo = 0, hi = -1;
        for (int x = 0; x < w; x++) {
            int nlo = x - r < 0 ? 0 : x - r, nhi = x + r > w - 1 ? w - 1 : x + r;
            while (hi < nhi) acc += s[++hi];
            while (lo < nlo) acc -= s[lo++];
            t[x] = acc / (float)(hi - lo + 1);
        }
    }
    float *acc = tmp + (size_t)w * h;
    for (int x = 0; x < w; x++) acc[x] = 0;
    int lo = 0, hi = -1;
    for (int y = 0; y < h; y++) {                 /* columns, a row at a time */
        int nlo = y - r < 0 ? 0 : y - r, nhi = y + r > h - 1 ? h - 1 : y + r;
        while (hi < nhi) { const float *t = tmp + (size_t)++hi * w; for (int x = 0; x < w; x++) acc[x] += t[x]; }
        while (lo < nlo) { const float *t = tmp + (size_t)lo++ * w; for (int x = 0; x < w; x++) acc[x] -= t[x]; }
        float inv = 1.0f / (float)(hi - lo + 1);
        float *d = dst + (size_t)y * w;
        for (int x = 0; x < w; x++) d[x] = acc[x] * inv;
    }
}

/* tmp: 6·w·h + w floats */
void st_guided_coeffs(const float *I, const float *p, int w, int h, int r, float eps, float *a, float *b, float *tmp) {
    size_t n = (size_t)w * h;
    float *mI = tmp, *mp = tmp + n, *cIp = tmp + 2 * n, *vI = tmp + 3 * n, *prod = tmp + 4 * n, *scratch = tmp + 5 * n;
    box_mean(I, mI, w, h, r, scratch);
    box_mean(p, mp, w, h, r, scratch);
    for (size_t i = 0; i < n; i++) prod[i] = I[i] * p[i];
    box_mean(prod, cIp, w, h, r, scratch);
    for (size_t i = 0; i < n; i++) prod[i] = I[i] * I[i];
    box_mean(prod, vI, w, h, r, scratch);
    for (size_t i = 0; i < n; i++) {
        float cov = cIp[i] - mI[i] * mp[i], var = vI[i] - mI[i] * mI[i];
        float ai = cov / (var + eps);
        prod[i] = ai;                             /* a before its mean */
        cIp[i] = mp[i] - ai * mI[i];              /* b before its mean */
    }
    box_mean(prod, a, w, h, r, scratch);
    box_mean(cIp, b, w, h, r, scratch);
}

/* one horizontal then one vertical box pass of radius r over 4 interleaved channels, three times (about a Gaussian).
 * tmp: 4·w·h + 4·w floats. */
void st_box_blur4(float *p, int w, int h, int r, float *tmp) {
    if (r < 1) return;
    float *acc = tmp + (size_t)w * h * 4;
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < h; y++) {
            const float *s = p + (size_t)y * w * 4;
            float *t = tmp + (size_t)y * w * 4;
            float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
            int lo = 0, hi = -1;
            for (int x = 0; x < w; x++) {
                int nlo = x - r < 0 ? 0 : x - r, nhi = x + r > w - 1 ? w - 1 : x + r;
                while (hi < nhi) { hi++; a0 += s[hi * 4]; a1 += s[hi * 4 + 1]; a2 += s[hi * 4 + 2]; a3 += s[hi * 4 + 3]; }
                while (lo < nlo) { a0 -= s[lo * 4]; a1 -= s[lo * 4 + 1]; a2 -= s[lo * 4 + 2]; a3 -= s[lo * 4 + 3]; lo++; }
                float inv = 1.0f / (float)(hi - lo + 1);
                t[x * 4] = a0 * inv; t[x * 4 + 1] = a1 * inv; t[x * 4 + 2] = a2 * inv; t[x * 4 + 3] = a3 * inv;
            }
        }
        for (int x = 0; x < w * 4; x++) acc[x] = 0;
        int lo = 0, hi = -1;
        for (int y = 0; y < h; y++) {
            int nlo = y - r < 0 ? 0 : y - r, nhi = y + r > h - 1 ? h - 1 : y + r;
            while (hi < nhi) { const float *t = tmp + (size_t)++hi * w * 4; for (int x = 0; x < w * 4; x++) acc[x] += t[x]; }
            while (lo < nlo) { const float *t = tmp + (size_t)lo++ * w * 4; for (int x = 0; x < w * 4; x++) acc[x] -= t[x]; }
            float inv = 1.0f / (float)(hi - lo + 1);
            float *d = p + (size_t)y * w * 4;
            for (int x = 0; x < w * 4; x++) d[x] = acc[x] * inv;
        }
    }
}

/* One pass over the frame: the box average of each guide cell (gw×gh) as RGB 0..255 (rgbg, 3 per cell) and as luma
 * 0..1 (yg). The segmenter's 256×144 input then comes from rgbg (st_resize_rgb), not from a second full pass. */
void st_downscale_guide(const uint8_t *bgra, int stride, int w, int h, float *rgbg, float *yg, int gw, int gh, uint16_t *rowsum) {
    for (int gy = 0; gy < gh; gy++) {
        int y0, y1;
        span(gy, gh, h, &y0, &y1);
        for (int i = 0; i < w * 4; i++) rowsum[i] = 0;
        for (int y = y0; y < y1; y++) {          /* at most 257 rows per cell (h ≤ ST_MAX_H, gh ≥ 16 → 135): fits */
            const uint8_t *restrict p = bgra + (size_t)y * stride;
            uint16_t *restrict rs = rowsum;
            for (int i = 0; i < w * 4; i++) rs[i] = (uint16_t)(rs[i] + p[i]);
        }
        for (int gx = 0; gx < gw; gx++) {
            int x0, x1;
            span(gx, gw, w, &x0, &x1);
            uint32_t b = 0, g = 0, r = 0;
            for (int x = x0; x < x1; x++) { b += rowsum[x * 4]; g += rowsum[x * 4 + 1]; r += rowsum[x * 4 + 2]; }
            float k = 1.0f / (float)((y1 - y0) * (x1 - x0));
            float *o = rgbg + ((size_t)gy * gw + gx) * 3;
            o[0] = (float)r * k; o[1] = (float)g * k; o[2] = (float)b * k;
            yg[(size_t)gy * gw + gx] = (0.299f * o[0] + 0.587f * o[1] + 0.114f * o[2]) * (1.0f / 255);
        }
    }
}

/* the source cells [first, first + n) that output cell o covers along one axis, with their area weights (n ≤ 4) */
typedef struct { int first, n; float w[4]; } Taps;
static void taps(int o, int on, int sn, Taps *t) {
    float f0 = (float)o * sn / on, f1 = (float)(o + 1) * sn / on, sum = 0;
    t->first = (int)f0;
    t->n = 0;
    for (int i = t->first; i < sn && (float)i < f1 && t->n < 4; i++) {
        float w = fminf(f1, (float)i + 1) - fmaxf(f0, (float)i);
        if (w <= 0) { if (!t->n) t->first++; continue; }
        t->w[t->n++] = w;
        sum += w;
    }
    for (int i = 0; i < t->n; i++) t->w[i] /= sum;
}

/* RGB 0..255 (sw×sh×3) to RGB 0..1 (dw×dh×3), area-weighted (each output cell averages the source cells it covers);
 * separable, for downscales of at most 4:1. tmp: sh·dw·3 floats. */
void st_resize_rgb(const float *src, int sw, int sh, float *dst, int dw, int dh, float *tmp) {
    for (int x = 0; x < dw; x++) {
        Taps t;
        taps(x, dw, sw, &t);
        for (int y = 0; y < sh; y++) {
            const float *q = src + ((size_t)y * sw + t.first) * 3;
            float r = 0, g = 0, b = 0;
            for (int k = 0; k < t.n; k++) { r += q[3 * k] * t.w[k]; g += q[3 * k + 1] * t.w[k]; b += q[3 * k + 2] * t.w[k]; }
            float *o = tmp + ((size_t)y * dw + x) * 3;
            o[0] = r; o[1] = g; o[2] = b;
        }
    }
    for (int y = 0; y < dh; y++) {
        Taps t;
        taps(y, dh, sh, &t);
        float *o = dst + (size_t)y * dw * 3;
        for (int i = 0; i < dw * 3; i++) o[i] = 0;
        for (int k = 0; k < t.n; k++) {
            const float *q = tmp + (size_t)(t.first + k) * dw * 3;
            float wk = t.w[k] * (1.0f / 255);
            for (int i = 0; i < dw * 3; i++) o[i] += q[i] * wk;
        }
    }
}
