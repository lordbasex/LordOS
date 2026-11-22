/* colorconv.c — see colorconv.h. */
#include "colorconv.h"

#include <math.h>
#include <stdlib.h>

const CcPrim cc_srgb = { 0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290 };

#define ENC_N 16384             /* linear light → 8 bits: entries of the encoding table */

struct CcConv {
    int alpha, ident_m;
    float m[9];
    float dec[256];             /* electrical 8-bit → linear light */
    uint8_t lut[256];           /* opaque pixels with no primaries change: electrical in → electrical out */
    uint8_t enc[ENC_N];         /* linear light (index / (ENC_N - 1)) → gamma 2.2 8-bit */
};

static void mul3(const double a[9], const double b[9], double o[9]) {
    double t[9];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) t[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
    for (int i = 0; i < 9; i++) o[i] = t[i];
}

static int inv3(const double m[9], double o[9]) {
    double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    double det = a * A + b * B + c * C;
    if (!isfinite(det) || fabs(det) < 1e-9) return -1;
    double t[9] = { A, -(b * i - c * h), b * f - c * e, B, a * i - c * g, -(a * f - c * d), C, -(a * h - b * g), a * e - b * d };
    for (int k = 0; k < 9; k++) o[k] = t[k] / det;
    return 0;
}

static int xy_ok(double x, double y) { return isfinite(x) && isfinite(y) && x >= 0 && y >= 0 && x <= 1 && y <= 1 && x + y <= 1.0 + 1e-9; }

/* Linear RGB in primaries p → CIE XYZ (white Y = 1). Each primary's xyz (x, y, 1 - x - y) is a column scaled so the
 * three add up to the white point; xyz rather than XYZ, so a primary with y = 0 (CIE 1931 XYZ's) works too. */
static int rgb_to_xyz(const CcPrim *p, double o[9]) {
    if (!xy_ok(p->rx, p->ry) || !xy_ok(p->gx, p->gy) || !xy_ok(p->bx, p->by) || !xy_ok(p->wx, p->wy) || p->wy < 1e-6) return -1;
    double P[9] = { p->rx, p->gx, p->bx, p->ry, p->gy, p->by, 1 - p->rx - p->ry, 1 - p->gx - p->gy, 1 - p->bx - p->by }, Pi[9];
    if (inv3(P, Pi)) return -1;
    double W[3] = { p->wx / p->wy, 1.0, (1 - p->wx - p->wy) / p->wy };
    for (int r = 0; r < 3; r++) {
        double s = Pi[r * 3] * W[0] + Pi[r * 3 + 1] * W[1] + Pi[r * 3 + 2] * W[2];
        if (!isfinite(s) || s <= 0) return -1;              /* the white is outside the primaries' triangle */
        for (int k = 0; k < 3; k++) P[k * 3 + r] *= s;
    }
    for (int k = 0; k < 9; k++) o[k] = P[k];
    return 0;
}

/* Bradford chromatic adaptation from white (x, y) to D65 */
static void bradford_to_d65(double wx, double wy, double o[9]) {
    static const double B[9] = { 0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296 };
    double Bi[9];
    inv3(B, Bi);
    double ws[3] = { wx / wy, 1.0, (1 - wx - wy) / wy };
    double wd[3] = { cc_srgb.wx / cc_srgb.wy, 1.0, (1 - cc_srgb.wx - cc_srgb.wy) / cc_srgb.wy };
    double cs[3], cd[3];
    for (int r = 0; r < 3; r++) {
        cs[r] = B[r * 3] * ws[0] + B[r * 3 + 1] * ws[1] + B[r * 3 + 2] * ws[2];
        cd[r] = B[r * 3] * wd[0] + B[r * 3 + 1] * wd[1] + B[r * 3 + 2] * wd[2];
    }
    double D[9] = { cd[0] / cs[0], 0, 0, 0, cd[1] / cs[1], 0, 0, 0, cd[2] / cs[2] };
    mul3(Bi, D, o);
    mul3(o, B, o);
}

int cc_prim_equal(const CcPrim *a, const CcPrim *b) {
    const double *x = &a->rx, *y = &b->rx;
    for (int i = 0; i < 8; i++) if (fabs(x[i] - y[i]) > 1e-5) return 0;
    return 1;
}

int cc_matrix(const CcPrim *p, double m[9]) {
    double src[9], dst[9], dsti[9], ad[9];
    if (rgb_to_xyz(p, src) || rgb_to_xyz(&cc_srgb, dst) || inv3(dst, dsti)) return -1;
    if (fabs(p->wx - cc_srgb.wx) > 1e-5 || fabs(p->wy - cc_srgb.wy) > 1e-5) {
        bradford_to_d65(p->wx, p->wy, ad);
        mul3(ad, src, src);
    }
    mul3(dsti, src, m);
    for (int i = 0; i < 9; i++) if (!isfinite(m[i])) return -1;
    return 0;
}

double cc_decode(const CcDesc *d, double e) {
    if (e <= 0) return 0;
    if (e >= 1) return 1;
    switch (d->tf) {
    case CC_TF_LINEAR: return e;
    case CC_TF_BT1886: return pow(e, 2.4);
    case CC_TF_GAMMA28: return pow(e, 2.8);
    case CC_TF_POWER: return pow(e, d->power);
    default: return pow(e, 2.2);            /* gamma 2.2, and sRGB as the 2.2 display it is shown on */
    }
}

double cc_encode_out(double l) { return l <= 0 ? 0 : l >= 1 ? 1 : pow(l, 1 / 2.2); }

static int tf_is_out(const CcDesc *d) {
    return d->tf == CC_TF_GAMMA22 || d->tf == CC_TF_SRGB || (d->tf == CC_TF_POWER && fabs(d->power - 2.2) < 1e-4);
}

int cc_needed(const CcDesc *d, int alpha) {
    return alpha != CC_ALPHA_ELECTRICAL || !tf_is_out(d) || !cc_prim_equal(&d->prim, &cc_srgb);
}

CcConv *cc_conv_new(const CcDesc *d, int alpha) {
    if (!cc_needed(d, alpha)) return NULL;
    double m[9];
    if (cc_matrix(&d->prim, m)) return NULL;
    CcConv *c = malloc(sizeof *c);
    if (!c) return NULL;
    c->alpha = alpha;
    c->ident_m = 1;
    for (int i = 0; i < 9; i++) {
        c->m[i] = (float)m[i];
        if (fabs(m[i] - (i % 4 == 0 ? 1.0 : 0.0)) > 1e-6) c->ident_m = 0;
    }
    for (int i = 0; i < ENC_N; i++) c->enc[i] = (uint8_t)lrint(255.0 * cc_encode_out((double)i / (ENC_N - 1)));
    for (int i = 0; i < 256; i++) {
        double l = cc_decode(d, i / 255.0);
        c->dec[i] = (float)l;
        c->lut[i] = (uint8_t)lrint(255.0 * cc_encode_out(l));
    }
    return c;
}

void cc_conv_free(CcConv *c) { free(c); }

static inline uint32_t enc8(const CcConv *c, float l) {
    if (!(l > 0)) return 0;                 /* (NaN too) */
    if (l >= 1) return 255;
    return c->enc[(int)(l * (ENC_N - 1) + 0.5f)];
}

void cc_convert(const CcConv *c, uint32_t *px, int stride, int x0, int y0, int x1, int y1, int opaque) {
    if (!c || !px) return;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = px + (size_t)y * (size_t)stride;
        for (int x = x0; x < x1; x++) {
            uint32_t p = row[x], a = opaque ? 255 : p >> 24;
            uint32_t ch[3] = { (p >> 16) & 255, (p >> 8) & 255, p & 255 };
            if (!a) { row[x] = 0; continue; }
            if (a == 255 && c->ident_m) {   /* every alpha mode is the same at alpha 1 */
                row[x] = 0xff000000u | (uint32_t)c->lut[ch[0]] << 16 | (uint32_t)c->lut[ch[1]] << 8 | c->lut[ch[2]];
                continue;
            }
            if (c->ident_m && c->alpha != CC_ALPHA_OPTICAL) {  /* straight electrical values through the table */
                uint32_t o = a << 24;
                for (int k = 0; k < 3; k++) {
                    uint32_t e = c->alpha == CC_ALPHA_STRAIGHT ? ch[k] : (ch[k] * 255 + a / 2) / a;
                    o |= ((uint32_t)c->lut[e > 255 ? 255 : e] * a + 127) / 255 << (16 - 8 * k);
                }
                row[x] = o;
                continue;
            }
            float l[3], fa = (float)a / 255.0f;
            for (int k = 0; k < 3; k++) {
                if (a == 255 || c->alpha == CC_ALPHA_STRAIGHT) l[k] = c->dec[ch[k]];
                else if (c->alpha == CC_ALPHA_OPTICAL) l[k] = c->dec[ch[k]] / fa;
                else {                      /* premultiplied electrical: back to straight first, between table steps */
                    float e = (float)ch[k] * 255.0f / (float)a;
                    if (e >= 255.0f) l[k] = c->dec[255];
                    else {
                        int i = (int)e;
                        l[k] = c->dec[i] + (c->dec[i + 1] - c->dec[i]) * (e - (float)i);
                    }
                }
            }
            if (!c->ident_m) {
                float r = c->m[0] * l[0] + c->m[1] * l[1] + c->m[2] * l[2];
                float g = c->m[3] * l[0] + c->m[4] * l[1] + c->m[5] * l[2];
                float b = c->m[6] * l[0] + c->m[7] * l[1] + c->m[8] * l[2];
                l[0] = r; l[1] = g; l[2] = b;
            }
            uint32_t o = a << 24;
            for (int k = 0; k < 3; k++) {
                uint32_t e = enc8(c, l[k]);
                if (a != 255) e = (e * a + 127) / 255;
                o |= e << (16 - 8 * k);
            }
            row[x] = o;
        }
    }
}
