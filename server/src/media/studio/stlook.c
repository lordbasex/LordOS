/* stlook.c — LordOS Studio's looks (see st.h; docs/CAMERA-STUDIO-PLAN.md 4.4, 5.3, 5.4): the filters' 3D LUTs, the
 * colour match, the chroma key, Face Light's gains and Retouch. All our own code. The filters are built here from a
 * few numbers each (no file is read or parsed); the colour transfer is Reinhard, Ashikhmin, Gooch and Shirley's
 * ("Color Transfer between Images", IEEE CG&A 2001) moved into a linear opponent space so that it is one affine map
 * of RGB; the chroma key is the usual distance in the CbCr plane (none of OBS's GPL code is used). */
#include "st.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float smoothf(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
}
static inline float luma709(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

/* ---- filters ----------------------------------------------------------------------------------------------------- */


/* an S-curve of strength k (0: none, 1: smoothstep), x in 0..1 */
static float scurve(float x, float k) {
    x = clampf(x, 0, 1);
    return x + (x * x * (3 - 2 * x) - x) * k;
}

/* the matrix that changes saturation by k around Rec. 709's luma */
static void sat_matrix(float k, float m[9]) {
    static const float L[3] = { 0.2126f, 0.7152f, 0.0722f };
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[3 * r + c] = (1 - k) * L[c] + (r == c ? k : 0);
}

static void mat_mul(const float a[9], const float b[9], float o[9]) {
    float t[9];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) t[3 * r + c] = a[3 * r] * b[c] + a[3 * r + 1] * b[3 + c] + a[3 * r + 2] * b[6 + c];
    memcpy(o, t, sizeof t);
}

/* channel k's curve of filter f, x in 0..1 (after the matrix) */
static float curve(int f, int k, float x) {
    x = clampf(x, 0, 1);
    switch (f) {
    case ST_FILTER_VIVID: return scurve(x, 0.18f);
    case ST_FILTER_MONO: return scurve(x, 0.08f);
    case ST_FILTER_NOIR: return scurve((x - 0.06f) / 0.88f, 0.45f);
    case ST_FILTER_FILM: {
        /* split toning on each channel's own level: teal in the shadows, warm in the highlights; lifted blacks and
         * rolled-off whites */
        static const float SH[3] = { -0.025f, 0.01f, 0.03f }, HI[3] = { 0.03f, 0.01f, -0.03f };
        float sh = 1 - smoothf(0.0f, 0.5f, x), hi = smoothf(0.5f, 1.0f, x);
        return 0.035f + scurve(x + SH[k] * sh + HI[k] * hi, 0.12f) * 0.93f;
    }
    case ST_FILTER_FADE: return 0.09f + x * 0.83f;
    default: return x;
    }
}

int st_look_build(int f, StLook *l) {
    if (!l || f < ST_FILTER_NONE || f > ST_FILTER_LAST) return -1;
    static const float ID[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    memcpy(l->m, ID, sizeof ID);
    float t[9];
    switch (f) {
    case ST_FILTER_WARM: {
        const float g[9] = { 1.07f, 0, 0, 0, 1.015f, 0, 0, 0, 0.88f };
        sat_matrix(1.05f, t);
        mat_mul(t, g, l->m);
        break;
    }
    case ST_FILTER_COOL: {
        const float g[9] = { 0.90f, 0, 0, 0, 0.99f, 0, 0, 0, 1.08f };
        memcpy(l->m, g, sizeof g);
        break;
    }
    case ST_FILTER_VIVID: sat_matrix(1.35f, l->m); break;
    case ST_FILTER_MONO: case ST_FILTER_NOIR: sat_matrix(0.0f, l->m); break;
    case ST_FILTER_FILM: sat_matrix(0.9f, l->m); break;
    case ST_FILTER_FADE: sat_matrix(0.72f, l->m); break;
    case ST_FILTER_SEPIA: {
        const float k = 1.08f / 1.351f;          /* white stays white-ish: the matrix's largest row sum */
        const float s[9] = { 0.393f * k, 0.769f * k, 0.189f * k, 0.349f * k, 0.686f * k, 0.168f * k, 0.272f * k, 0.534f * k, 0.131f * k };
        memcpy(l->m, s, sizeof s);
        break;
    }
    default: break;
    }
    for (int k = 0; k < 3; k++) for (int i = 0; i < ST_CURVE_N; i++) l->curve[k][i] = 255 * curve(f, k, (float)i / (ST_CURVE_N - 1));
    return 0;
}

void st_look_apply(const StLook *l, const float in[3], float out[3]) {
    for (int k = 0; k < 3; k++) {
        float v = (l->m[3 * k] * in[0] + l->m[3 * k + 1] * in[1] + l->m[3 * k + 2] * in[2]) * ((ST_CURVE_N - 1) / 255.0f);
        int i = (int)(clampf(v, 0, ST_CURVE_N - 1) + 0.5f);
        out[k] = l->curve[k][i];
    }
}

int st_lut_build(int filter, uint8_t *lut) {
    StLook l;
    if (!lut || st_look_build(filter, &l)) return -1;
    const int N = ST_LUT_N;
    for (int b = 0; b < N; b++) for (int g = 0; g < N; g++) for (int r = 0; r < N; r++) {
        float c[3] = { r * 255.0f / (N - 1), g * 255.0f / (N - 1), b * 255.0f / (N - 1) }, o3[3];
        st_look_apply(&l, c, o3);
        uint8_t *o = lut + (((size_t)b * N + g) * N + r) * 3;
        for (int i = 0; i < 3; i++) o[i] = (uint8_t)(clampf(o3[i], 0, 255) + 0.5f);
    }
    return 0;
}

void st_lut_apply(const uint8_t *lut, const float in[3], float out[3]) {
    const int N = ST_LUT_N;
    float f[3];
    int i0[3];
    for (int k = 0; k < 3; k++) {
        float v = clampf(in[k], 0, 255) * ((float)(N - 1) / 255);
        int i = (int)v;
        if (i > N - 2) i = N - 2;
        i0[k] = i;
        f[k] = v - (float)i;
    }
    const size_t sr = 3, sg = (size_t)N * 3, sb = (size_t)N * N * 3;
    const uint8_t *p = lut + (size_t)i0[2] * sb + (size_t)i0[1] * sg + (size_t)i0[0] * sr;
    for (int k = 0; k < 3; k++) {
        float c00 = p[k] + (p[sr + k] - p[k]) * f[0];
        float c10 = p[sg + k] + (p[sg + sr + k] - p[sg + k]) * f[0];
        float c01 = p[sb + k] + (p[sb + sr + k] - p[sb + k]) * f[0];
        float c11 = p[sb + sg + k] + (p[sb + sg + sr + k] - p[sb + sg + k]) * f[0];
        float c0 = c00 + (c10 - c00) * f[1], c1 = c01 + (c11 - c01) * f[1];
        out[k] = c0 + (c1 - c0) * f[2];
    }
}

/* ---- colour match ------------------------------------------------------------------------------------------------ */

/* the opponent space: L = (r+g+b)/3, A = r−g, B = (r+g)/2 − b; linear, so a per-channel affine map there is an affine
 * map of RGB */
static const float T[9] = { 1.0f / 3, 1.0f / 3, 1.0f / 3, 1, -1, 0, 0.5f, 0.5f, -1 };
static const float Ti[9] = { 1, 0.5f, 1.0f / 3, 1, -0.5f, 1.0f / 3, 1, 0, -2.0f / 3 };

void st_stat_add(StStat *st, const float rgb[3], float w) {
    if (!st || !(w > 0)) return;
    for (int k = 0; k < 3; k++) {
        double v = T[3 * k] * rgb[0] + T[3 * k + 1] * rgb[1] + T[3 * k + 2] * rgb[2];
        st->s[k] += w * v;
        st->ss[k] += w * v * v;
    }
    st->w += w;
}

int st_match_transform(const StStat *p, const StStat *b, float strength, float m[9], float o[3]) {
    for (int i = 0; i < 9; i++) m[i] = i % 4 == 0 ? 1.0f : 0.0f;
    o[0] = o[1] = o[2] = 0;
    if (!p || !b || p->w < 16 || b->w < 16) return -1;
    float s = clampf(strength, 0, 1);
    float d[3], e[3];
    for (int k = 0; k < 3; k++) {
        double mp = p->s[k] / p->w, mb = b->s[k] / b->w;
        double vp = p->ss[k] / p->w - mp * mp, vb = b->ss[k] / b->w - mb * mb;
        double sp = sqrt(vp > 0 ? vp : 0), sb = sqrt(vb > 0 ? vb : 0);
        double g = sp > 1.0 ? sb / sp : 1.0;
        g = g < 0.5 ? 0.5 : g > 2.0 ? 2.0 : g;
        if (k == 0) {
            /* the exposure: at most one stop either way (plan 5.3), and the contrast follows only halfway */
            if (mp > 1.0) { if (mb > mp * 2) mb = mp * 2; if (mb < mp * 0.5) mb = mp * 0.5; }
            g = 1 + (g - 1) * 0.5;
        }
        /* the colour channels move only part of the way, so skin keeps reading as skin over a strongly coloured scene */
        double sk = k == 0 ? s : s * 0.6;
        d[k] = (float)(1 - sk + sk * g);
        e[k] = (float)(sk * (mb - g * mp));
    }
    /* M = Ti · diag(d) · T, o = Ti · e */
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            float v = 0;
            for (int k = 0; k < 3; k++) v += Ti[3 * r + k] * d[k] * T[3 * k + c];
            m[3 * r + c] = v;
        }
        o[r] = Ti[3 * r] * e[0] + Ti[3 * r + 1] * e[1] + Ti[3 * r + 2] * e[2];
    }
    return 0;
}

/* ---- chroma key -------------------------------------------------------------------------------------------------- */

static inline void cbcr(const float c[3], float *cb, float *cr) {
    *cb = 128 - 0.168736f * c[0] - 0.331264f * c[1] + 0.5f * c[2];
    *cr = 128 + 0.5f * c[0] - 0.418688f * c[1] - 0.081312f * c[2];
}

float st_chroma_alpha(const float rgb[3], const float key[2], int similarity, int smoothness) {
    float cb, cr;
    cbcr(rgb, &cb, &cr);
    float d = sqrtf((cb - key[0]) * (cb - key[0]) + (cr - key[1]) * (cr - key[1])) * (1.0f / 255);
    float t0 = 0.04f + clampf((float)similarity, 0, 100) * 0.003f, t1 = t0 + 0.01f + clampf((float)smoothness, 0, 100) * 0.0025f;
    return smoothf(t0, t1, d);
}

/* the presets: a typical lit green screen (40, 170, 70) and blue screen (30, 60, 200) */
static const float PRESET[3][2] = { { 128, 128 }, { 100.0f, 71.1f }, { 203.0f, 101.6f } };

void st_chroma_key(const uint8_t *bgra, int stride, int w, int h, int key, float out[2]) {
    if (key < ST_KEY_GREEN || key > ST_KEY_LAST) key = ST_KEY_GREEN;
    out[0] = PRESET[key][0];
    out[1] = PRESET[key][1];
    if (!bgra || w < 16 || h < 16) return;
    /* the top band and both sides (a person usually fills the bottom middle), every few pixels */
    static unsigned hb[256], hr[256];
    memset(hb, 0, sizeof hb);
    memset(hr, 0, sizeof hr);
    unsigned n = 0;
    int band = h / 20 > 2 ? h / 20 : 2, side = w / 20 > 2 ? w / 20 : 2;
    for (int y = 0; y < h; y += 3) for (int x = 0; x < w; x += 3) {
        if (y >= band && x >= side && x < w - side) continue;
        const uint8_t *p = bgra + (size_t)y * stride + (size_t)x * 4;
        float c[3] = { p[2], p[1], p[0] }, cb, cr;
        cbcr(c, &cb, &cr);
        hb[(int)clampf(cb, 0, 255)]++;
        hr[(int)clampf(cr, 0, 255)]++;
        n++;
    }
    if (n < 16) return;
    float m[2];
    unsigned *hs[2] = { hb, hr };
    for (int k = 0; k < 2; k++) {
        unsigned acc = 0;
        int i = 0;
        while (i < 255 && (acc += hs[k][i]) < n / 2) i++;
        m[k] = (float)i;
    }
    /* that colour only when it is clearly the key's: saturated enough and on the key's side of the plane */
    float dist = sqrtf((m[0] - 128) * (m[0] - 128) + (m[1] - 128) * (m[1] - 128));
    int ok = dist > 24 && (key == ST_KEY_GREEN ? m[0] < 128 && m[1] < 120 : m[0] > 140 && m[1] < 140);
    if (ok) { out[0] = m[0]; out[1] = m[1]; }
}

void st_despill(float c[3], int key, float amount) {
    if (amount <= 0) return;
    if (key == ST_KEY_GREEN) {
        float lim = (c[0] + c[2]) * 0.5f;
        if (c[1] > lim) c[1] -= (c[1] - lim) * amount;
    } else if (key == ST_KEY_BLUE) {
        float lim = (c[0] + c[1]) * 0.5f;
        if (c[2] > lim) c[2] -= (c[2] - lim) * amount;
    }
}

/* ---- Face Light ------------------------------------------------------------------------------------------------ */

/* how much of the face's lift a point gets: 1 inside the face's ellipse (a little above the box's centre, where the
 * forehead and eyes are), fading to 0 at 1.7 times its radii */
static float face_weight(float x, float y, const float *f) {
    if (!f || !(f[2] > 0) || !(f[3] > 0)) return 0;
    float dx = (x - f[0]) / (0.75f * f[2]), dy = (y - (f[1] - 0.08f * f[3])) / (0.95f * f[3]);
    float r = sqrtf(dx * dx + dy * dy);
    return 1 - smoothf(1.0f, 1.7f, r);
}

float st_light_person(float x, float y, const float *face, int intensity) {
    float k = clampf((float)intensity, 0, 100) / 100;
    if (k <= 0) return 1;
    /* +0.15 EV over the whole person, up to +0.6 EV on the face (plan 5.3: +0.3 to +0.6) */
    return exp2f(k * (0.15f + 0.45f * face_weight(x, y, face)));
}

float st_light_background(int intensity) {
    float k = clampf((float)intensity, 0, 100) / 100;
    return exp2f(-0.5f * k);
}

/* ---- Retouch ----------------------------------------------------------------------------------------------------- */

/* skin, loosely: the classic YCbCr range (Cb 77..127, Cr 133..173), feathered */
static float skin(const uint8_t *p) {
    float c[3] = { p[2], p[1], p[0] }, cb, cr;
    cbcr(c, &cb, &cr);
    return smoothf(71, 81, cb) * (1 - smoothf(123, 133, cb)) * smoothf(127, 137, cr) * (1 - smoothf(169, 179, cr));
}

/* The fast guided filter (He and Sun 2015) on the face's region: each channel guides itself; its coefficients at half
 * size, interpolated back; mixed in by the skin weight. */
int st_retouch(uint8_t *bgra, int stride, int w, int h, const float f[4], int strength, float **tmp, size_t *cap) {
    if (!bgra || !f || strength <= 0 || !(f[2] > 4) || !(f[3] > 4)) return 0;
    int x0 = (int)(f[0] - 0.9f * f[2]), x1 = (int)(f[0] + 0.9f * f[2]), y0 = (int)(f[1] - 1.1f * f[3]), y1 = (int)(f[1] + 1.0f * f[3]);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    int rw = (x1 - x0) & ~1, rh = (y1 - y0) & ~1;
    if (rw < 16 || rh < 16) return 0;
    int hw = rw / 2, hh = rh / 2;
    size_t n = (size_t)rw * rh, m = (size_t)hw * hh, need = 9 * m + (size_t)hw + n;
    if (*cap < need) {
        float *t = realloc(*tmp, need * sizeof(float));
        if (!t) return -1;
        *tmp = t;
        *cap = need;
    }
    float *I = *tmp, *a = I + m, *b = a + m, *gt = b + m, *wt = gt + 6 * m + hw;     /* gt: 6m + hw; wt: n */
    /* the filter's size follows the face: a radius of about 1/40 of its width (at full size), at least 1 half pixel */
    int r = (int)(f[2] / 80);
    if (r < 1) r = 1;
    if (r > 8) r = 8;
    float k = clampf((float)strength, 0, 100) / 100;
    float eps = 0.0012f + 0.0028f * k;                        /* larger: smoother; edges (eyes, lips) stay */
    /* the weight of each pixel: skin inside the face's ellipse */
    for (int y = 0; y < rh; y++) {
        const uint8_t *p = bgra + (size_t)(y0 + y) * stride + (size_t)x0 * 4;
        float *rwt = wt + (size_t)y * rw;
        float dy = (y0 + y + 0.5f - f[1]) / (0.78f * f[3]);
        for (int x = 0; x < rw; x++) {
            float dx = (x0 + x + 0.5f - f[0]) / (0.62f * f[2]);
            float e = 1 - smoothf(0.75f, 1.0f, sqrtf(dx * dx + dy * dy));
            rwt[x] = e > 0 ? e * skin(p + (size_t)x * 4) * k * 0.85f : 0;
        }
    }
    for (int c = 0; c < 3; c++) {
        for (int y = 0; y < hh; y++) {
            const uint8_t *p = bgra + (size_t)(y0 + 2 * y) * stride + (size_t)x0 * 4 + c, *q = p + stride;
            float *o = I + (size_t)y * hw;
            for (int x = 0; x < hw; x++) o[x] = (p[8 * x] + p[8 * x + 4] + q[8 * x] + q[8 * x + 4]) * (1.0f / 1020);
        }
        st_guided_coeffs(I, I, hw, hh, r, eps, a, b, gt);
        for (int y = 0; y < rh; y++) {
            uint8_t *p = bgra + (size_t)(y0 + y) * stride + (size_t)x0 * 4 + c;
            const float *rwt = wt + (size_t)y * rw;
            /* half-pixel centres: full row y is at (y − 0.5) / 2 in the half rows */
            float fy = clampf((y - 0.5f) * 0.5f, 0, (float)(hh - 1));
            int j = (int)fy;
            if (j > hh - 2) j = hh - 2;
            float ly = fy - (float)j;
            const float *a0 = a + (size_t)j * hw, *a1 = a0 + hw, *b0 = b + (size_t)j * hw, *b1 = b0 + hw;
            for (int x = 0; x < rw; x++) {
                if (rwt[x] <= 0) continue;
                float fx = clampf((x - 0.5f) * 0.5f, 0, (float)(hw - 1));
                int i = (int)fx;
                if (i > hw - 2) i = hw - 2;
                float lx = fx - (float)i;
                float av = (a0[i] + (a0[i + 1] - a0[i]) * lx) * (1 - ly) + (a1[i] + (a1[i + 1] - a1[i]) * lx) * ly;
                float bv = (b0[i] + (b0[i + 1] - b0[i]) * lx) * (1 - ly) + (b1[i] + (b1[i + 1] - b1[i]) * lx) * ly;
                float v = p[(size_t)x * 4] * (1.0f / 255), q = av * v + bv;
                v += (q - v) * rwt[x];
                p[(size_t)x * 4] = (uint8_t)(clampf(v, 0, 1) * 255 + 0.5f);
            }
        }
    }
    return 0;
}
