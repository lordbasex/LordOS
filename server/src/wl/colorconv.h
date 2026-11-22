/* colorconv.h — the colour conversion behind color-management-v1 and color-representation-v1 (wlcolor.c; see
 * docs/WAYLAND.md, "Colour management"). No Wayland here: plain pixel arithmetic, tested by tests/colorconv_test.c.
 *
 * Every LordOS output is driven as an SDR sRGB display: sRGB (BT.709) primaries and D65 white, encoded for a
 * gamma 2.2 display, no HDR. A surface that describes its content otherwise is converted into that encoding when its
 * buffer is taken (wl.c's take_buffer), on the CPU, into the surface's own copy of the pixels. Both compositing
 * paths (the CPU's and the GPU's) then draw those same converted pixels, so there is one conversion and one result.
 *
 * The conversion, per pixel (8-bit ARGB in, premultiplied 8-bit ARGB out):
 *   1. alpha: straight colour as is; premultiplied (electrical) divided by alpha; premultiplied optical: decoded
 *      first, then divided by alpha in linear light;
 *   2. decode: the content's transfer function to linear light (sRGB and gamma 2.2 both decode as a 2.2 power, the
 *      display the sRGB standard assumes; BT.1886 as a 2.4 power with a zero black level; any power curve);
 *   3. primaries: a 3×3 matrix from the content's primaries to sRGB's, with a Bradford adaptation of its white point
 *      to D65 (relative colorimetric; the perceptual intent is the same mapping); out-of-gamut colour is clipped per
 *      channel to [0, 1] in linear light;
 *   4. encode: linear light to the output's gamma 2.2, then premultiplied by alpha again.
 * A description equal to the output's (gamma 2.2 or sRGB, sRGB primaries, premultiplied electrical alpha) needs
 * nothing, and cc_conv_new returns NULL for it: such a surface keeps the zero-copy paths (dmabuf textures, direct
 * scanout). */
#ifndef LORD_WL_COLORCONV_H
#define LORD_WL_COLORCONV_H

#include <stdint.h>

/* Transfer functions lord-server decodes (electrical → optical) */
enum { CC_TF_GAMMA22, CC_TF_SRGB, CC_TF_LINEAR, CC_TF_BT1886, CC_TF_GAMMA28, CC_TF_POWER };
/* Alpha modes (color-representation-v1's, same order) */
enum { CC_ALPHA_ELECTRICAL, CC_ALPHA_OPTICAL, CC_ALPHA_STRAIGHT };

/* CIE 1931 xy chromaticities of the red, green and blue primaries and the white point */
typedef struct { double rx, ry, gx, gy, bx, by, wx, wy; } CcPrim;

typedef struct {
    int tf;                     /* CC_TF_* */
    double power;               /* CC_TF_POWER: the exponent, 1.0 to 10.0 */
    CcPrim prim;
} CcDesc;

extern const CcPrim cc_srgb;    /* BT.709 primaries, D65 */

/* The 3×3 matrix (row major) from linear RGB in these primaries to linear sRGB, white adapted to D65. 0, or -1 when
 * the primaries can't make one (a chromaticity out of range, primaries on one line, a white of zero luminance). */
int cc_matrix(const CcPrim *p, double m[9]);
/* 1 if two sets of primaries are the same (within 1e-5) */
int cc_prim_equal(const CcPrim *a, const CcPrim *b);
/* Decode one electrical value (0..1) to linear light with a description's transfer function */
double cc_decode(const CcDesc *d, double e);
/* The output's encoding: linear light (0..1) to gamma 2.2 */
double cc_encode_out(double l);

typedef struct CcConv CcConv;
/* 1 if pixels in this description and alpha mode must be converted to be shown right on an LordOS output */
int  cc_needed(const CcDesc *d, int alpha);
/* A converter, or NULL: nothing to convert (cc_needed is 0), out of memory or primaries cc_matrix refuses. */
CcConv *cc_conv_new(const CcDesc *d, int alpha);
void cc_conv_free(CcConv *c);
/* Converts the box [x0, x1) × [y0, y1) of px (ARGB8888, stride in pixels) in place: the source's colour as
 * described, to premultiplied ARGB8888 for the output. opaque: alpha is 255 whatever the buffer says (XRGB). */
void cc_convert(const CcConv *c, uint32_t *px, int stride, int x0, int y0, int x1, int y1, int opaque);

#endif
