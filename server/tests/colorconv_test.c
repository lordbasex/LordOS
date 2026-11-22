/* colorconv_test.c — the colour conversion behind color-management-v1 (src/wl/colorconv.c, docs/WAYLAND.md
 * "Colour management"): which descriptions need nothing, the primaries matrices against published values, and every
 * converted pixel against a double-precision reference (decode, matrix, clip, gamma 2.2, premultiply). */
#include "wl/colorconv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const CcPrim bt2020 = { 0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290 };
static const CcPrim p3d65 = { 0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290 };
static const CcPrim dcip3 = { 0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.314, 0.351 };
static const CcPrim adobe = { 0.640, 0.330, 0.210, 0.710, 0.150, 0.060, 0.3127, 0.3290 };
static const CcPrim film = { 0.681, 0.319, 0.243, 0.692, 0.145, 0.049, 0.310, 0.316 };
static const CcPrim xyz = { 1, 0, 0, 1, 0, 0, 1 / 3.0, 1 / 3.0 };

/* The reference: one pixel, as colorconv.h describes it, in doubles. */
static uint32_t ref_px(const CcDesc *d, int alpha, uint32_t p, int opaque) {
    double m[9];
    if (cc_matrix(&d->prim, m)) return 0xdeadbeef;
    int a = opaque ? 255 : (int)(p >> 24);
    if (!a) return 0;
    double fa = a / 255.0, l[3];
    for (int k = 0; k < 3; k++) {
        double c = ((p >> (16 - 8 * k)) & 255) / 255.0;
        if (alpha == CC_ALPHA_STRAIGHT || a == 255) l[k] = cc_decode(d, c);
        else if (alpha == CC_ALPHA_OPTICAL) l[k] = cc_decode(d, c) / fa;
        else l[k] = cc_decode(d, fmin(1.0, c / fa));
    }
    uint32_t o = (uint32_t)a << 24;
    for (int k = 0; k < 3; k++) {
        double v = m[k * 3] * l[0] + m[k * 3 + 1] * l[1] + m[k * 3 + 2] * l[2];
        double e = cc_encode_out(fmin(1.0, fmax(0.0, v)));
        o |= (uint32_t)lrint(e * 255.0 * fa) << (16 - 8 * k);
    }
    return o;
}

/* Every channel within tol of the reference; tol grows near black, where 8-bit linear light has few steps */
static int near(uint32_t a, uint32_t b, int tol) {
    if ((a >> 24) != (b >> 24)) return 0;
    for (int k = 0; k < 3; k++) {
        int x = (int)((a >> (8 * k)) & 255), y = (int)((b >> (8 * k)) & 255);
        int t = tol + (y < 24 ? 2 : 0);
        if (abs(x - y) > t) return 0;
    }
    return 1;
}

static void sweep(const char *name, const CcDesc *d, int alpha, int tol) {
    CcConv *c = cc_conv_new(d, alpha);
    CHECK(c != NULL);
    if (!c) return;
    int bad = 0, n = 0;
    uint32_t worst_in = 0, worst_out = 0, worst_ref = 0;
    static const int as[] = { 255, 254, 200, 128, 64, 17, 1, 0 };
    for (size_t ai = 0; ai < sizeof as / sizeof *as; ai++)
        for (int r = 0; r < 256; r += 15)
            for (int g = 0; g < 256; g += 17)
                for (int b = 0; b < 256; b += 51) {
                    uint32_t a = (uint32_t)as[ai];
                    /* premultiplied input can't have a channel above alpha */
                    uint32_t rr = (uint32_t)r, gg = (uint32_t)g, bb = (uint32_t)b;
                    if (alpha != CC_ALPHA_STRAIGHT) { rr = rr * a / 255; gg = gg * a / 255; bb = bb * a / 255; }
                    uint32_t p = a << 24 | rr << 16 | gg << 8 | bb, q = p;
                    cc_convert(c, &q, 1, 0, 0, 1, 1, 0);
                    uint32_t want = ref_px(d, alpha, p, 0);
                    /* low alpha: 8-bit premultiplied values hold few steps of colour */
                    int t = tol + (a < 128 ? (int)(255 / (a ? a : 1)) / 2 + 1 : 0);
                    n++;
                    if (!near(q, want, t)) { if (!bad) { worst_in = p; worst_out = q; worst_ref = want; } bad++; }
                }
    if (bad) fprintf(stderr, "%s: %d of %d pixels off (first: %08x → %08x, reference %08x)\n", name, bad, n, worst_in, worst_out, worst_ref);
    CHECK(bad == 0);
    printf("  %s: %d pixels match the reference\n", name, n);
    cc_conv_free(c);
}

static void test_identity(void) {
    CcDesc g22 = { CC_TF_GAMMA22, 0, cc_srgb }, srgb = { CC_TF_SRGB, 0, cc_srgb }, pw = { CC_TF_POWER, 2.2, cc_srgb };
    CHECK(!cc_needed(&g22, CC_ALPHA_ELECTRICAL));
    CHECK(!cc_needed(&srgb, CC_ALPHA_ELECTRICAL));
    CHECK(!cc_needed(&pw, CC_ALPHA_ELECTRICAL));
    CHECK(cc_conv_new(&g22, CC_ALPHA_ELECTRICAL) == NULL);
    /* the output's own encoding, but straight alpha: it must still be premultiplied */
    CHECK(cc_needed(&g22, CC_ALPHA_STRAIGHT));
    CcConv *c = cc_conv_new(&g22, CC_ALPHA_STRAIGHT);
    CHECK(c != NULL);
    uint32_t p[3] = { 0x80ff0000u, 0xffffffffu, 0x00ffffffu };
    cc_convert(c, p, 3, 0, 0, 3, 1, 0);
    CHECK(p[0] == 0x80800000u);             /* red at alpha 128/255: 255 × 128 / 255 */
    CHECK(p[1] == 0xffffffffu);
    CHECK(p[2] == 0);                       /* transparent is zero, whatever the colour */
    /* 8-bit round trip through the tables is exact at alpha 1 */
    uint32_t row[256];
    for (int i = 0; i < 256; i++) row[i] = 0xff000000u | (uint32_t)i << 16 | (uint32_t)i << 8 | (uint32_t)i;
    cc_convert(c, row, 256, 0, 0, 256, 1, 0);
    int same = 1;
    for (int i = 0; i < 256; i++) if (row[i] != (0xff000000u | (uint32_t)i * 0x010101u)) same = 0;
    CHECK(same);
    cc_conv_free(c);
    CcDesc other = { CC_TF_GAMMA22, 0, bt2020 }, lin = { CC_TF_LINEAR, 0, cc_srgb };
    CHECK(cc_needed(&other, CC_ALPHA_ELECTRICAL));
    CHECK(cc_needed(&lin, CC_ALPHA_ELECTRICAL));
}

static void mat_near(const CcPrim *p, const double want[9], double tol, const char *name) {
    double m[9];
    CHECK(cc_matrix(p, m) == 0);
    int ok = 1;
    for (int i = 0; i < 9; i++) if (fabs(m[i] - want[i]) > tol) ok = 0;
    if (!ok) fprintf(stderr, "%s: %.4f %.4f %.4f / %.4f %.4f %.4f / %.4f %.4f %.4f\n", name, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
    CHECK(ok);
}

static void test_matrices(void) {
    /* published linear-light matrices to sRGB (BT.709) primaries */
    static const double from2020[9] = { 1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083, -0.0182, -0.1006, 1.1187 };
    static const double fromp3[9] = { 1.2249, -0.2247, 0.0000, -0.0420, 1.0419, 0.0000, -0.0197, -0.0786, 1.0979 };
    static const double ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    mat_near(&bt2020, from2020, 2e-3, "bt2020");
    mat_near(&p3d65, fromp3, 2e-3, "display_p3");
    mat_near(&cc_srgb, ident, 1e-9, "srgb");
    /* white stays white (the source's white adapted to D65), for every set, other whites included */
    const CcPrim *all[] = { &bt2020, &p3d65, &dcip3, &adobe, &film, &xyz };
    for (size_t i = 0; i < sizeof all / sizeof *all; i++) {
        double m[9];
        CHECK(cc_matrix(all[i], m) == 0);
        for (int r = 0; r < 3; r++) CHECK(fabs(m[r * 3] + m[r * 3 + 1] + m[r * 3 + 2] - 1.0) < 1e-6);
    }
    /* primaries lord-server can't use are refused */
    double m[9];
    CcPrim line = { 0.2, 0.2, 0.4, 0.4, 0.6, 0.3, 0.3127, 0.3290 };   /* on one line */
    CcPrim neg = cc_srgb;
    neg.rx = -0.1;
    CcPrim out = cc_srgb;
    out.wx = 0.9; out.wy = 0.09;                                        /* the white outside the triangle */
    CcPrim nowhite = cc_srgb;
    nowhite.wy = 0;
    CHECK(cc_matrix(&line, m) != 0);
    CHECK(cc_matrix(&neg, m) != 0);
    CHECK(cc_matrix(&out, m) != 0);
    CHECK(cc_matrix(&nowhite, m) != 0);
    CcDesc bad = { CC_TF_GAMMA22, 0, line };
    CHECK(cc_conv_new(&bad, CC_ALPHA_ELECTRICAL) == NULL);
}

static void test_values(void) {
    /* linear light: 50 % gray is 0.5^(1/2.2) = 0.7297 of the output, 186 */
    CcDesc lin = { CC_TF_LINEAR, 0, cc_srgb };
    CcConv *c = cc_conv_new(&lin, CC_ALPHA_ELECTRICAL);
    uint32_t p = 0xff808080u;
    cc_convert(c, &p, 1, 0, 0, 1, 1, 0);
    CHECK(p == 0xffbababau);                /* 128/255 = 0.502 → 0.7304 → 186 (0xba) */
    /* XRGB: whatever the alpha byte says, it is opaque */
    p = 0x00808080u;
    cc_convert(c, &p, 1, 0, 0, 1, 1, 1);
    CHECK(p == 0xffbababau);
    /* only the box changes */
    uint32_t img[4 * 3];
    for (int i = 0; i < 12; i++) img[i] = 0xff808080u;
    cc_convert(c, img, 4, 1, 1, 3, 2, 0);
    int ok = 1;
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 4; x++) ok &= img[y * 4 + x] == (x >= 1 && x < 3 && y == 1 ? 0xffbababau : 0xff808080u);
    CHECK(ok);
    cc_conv_free(c);
    /* BT.2020 pure green is outside sRGB: clipped to sRGB's green */
    CcDesc g2020 = { CC_TF_GAMMA22, 0, bt2020 };
    c = cc_conv_new(&g2020, CC_ALPHA_ELECTRICAL);
    p = 0xff00ff00u;
    cc_convert(c, &p, 1, 0, 0, 1, 1, 0);
    CHECK(p == 0xff00ff00u);
    /* BT.2020 white is white */
    p = 0xffffffffu;
    cc_convert(c, &p, 1, 0, 0, 1, 1, 0);
    CHECK(p == 0xffffffffu);
    cc_conv_free(c);
    /* BT.1886: a 2.4 power shown on a 2.2 display: 128 → (0.502^2.4)^(1/2.2) = 0.4723 → 120 */
    CcDesc bt = { CC_TF_BT1886, 0, cc_srgb };
    c = cc_conv_new(&bt, CC_ALPHA_ELECTRICAL);
    p = 0xff808080u;
    cc_convert(c, &p, 1, 0, 0, 1, 1, 0);
    CHECK(p == 0xff787878u);
    cc_conv_free(c);
    /* premultiplied optical in linear light is the same as electrical: 50 % alpha, linear 0.25 → 0.5 straight */
    CcConv *opt = cc_conv_new(&lin, CC_ALPHA_OPTICAL), *ele = cc_conv_new(&lin, CC_ALPHA_ELECTRICAL);
    uint32_t a = 0x80404040u, b = a;
    cc_convert(opt, &a, 1, 0, 0, 1, 1, 0);
    cc_convert(ele, &b, 1, 0, 0, 1, 1, 0);
    CHECK(a == b);
    CHECK(near(a, ref_px(&lin, CC_ALPHA_OPTICAL, 0x80404040u, 0), 1));
    cc_conv_free(opt);
    cc_conv_free(ele);
}

int main(void) {
    test_identity();
    test_matrices();
    test_values();
    CcDesc lin = { CC_TF_LINEAR, 0, cc_srgb }, g2020 = { CC_TF_GAMMA22, 0, bt2020 }, p3 = { CC_TF_SRGB, 0, p3d65 };
    CcDesc dci = { CC_TF_POWER, 2.6, dcip3 }, bt = { CC_TF_BT1886, 0, cc_srgb }, g28 = { CC_TF_GAMMA28, 0, adobe };
    CcDesc lin2020 = { CC_TF_LINEAR, 0, bt2020 }, xyzl = { CC_TF_LINEAR, 0, xyz };
    sweep("ext_linear, sRGB primaries, premultiplied electrical", &lin, CC_ALPHA_ELECTRICAL, 1);
    sweep("ext_linear, sRGB primaries, premultiplied optical", &lin, CC_ALPHA_OPTICAL, 1);
    sweep("ext_linear, sRGB primaries, straight", &lin, CC_ALPHA_STRAIGHT, 1);
    sweep("gamma 2.2, BT.2020", &g2020, CC_ALPHA_ELECTRICAL, 1);
    sweep("sRGB, Display P3, straight", &p3, CC_ALPHA_STRAIGHT, 1);
    sweep("power 2.6, DCI-P3 (DCI white)", &dci, CC_ALPHA_ELECTRICAL, 1);
    sweep("BT.1886, sRGB primaries, optical", &bt, CC_ALPHA_OPTICAL, 1);
    sweep("gamma 2.8, Adobe RGB", &g28, CC_ALPHA_ELECTRICAL, 1);
    sweep("ext_linear, BT.2020, optical", &lin2020, CC_ALPHA_OPTICAL, 1);
    sweep("ext_linear, CIE 1931 XYZ", &xyzl, CC_ALPHA_STRAIGHT, 1);
    printf("colorconv: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
