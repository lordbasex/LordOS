/* canvas.c — primitives with alpha, signed-distance antialiasing and clipping. */
#include "canvas.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Output alpha of "over" (exact: an opaque destination stays at 255). The canvas stores
 * premultiplied alpha: on the CPU screen it does not matter (KMS is XRGB), and on the GPU path it marks
 * where the app windows below the UI show through (src/gpu). */
static inline uint32_t over_alpha(uint32_t sa, uint32_t da) {
    return sa + (((da * (255 - sa) + 128) * 257) >> 16);
}

static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t a) {
    uint32_t ia = 255 - a;
    uint32_t rb = (((src & 0xff00ff) * a + (dst & 0xff00ff) * ia) >> 8) & 0xff00ff;
    uint32_t g  = (((src & 0x00ff00) * a + (dst & 0x00ff00) * ia) >> 8) & 0x00ff00;
    return over_alpha(a, dst >> 24) << 24 | rb | g;
}

/* The corners' restore: a weight a of the saved pixel src and 255 - a of dst, both premultiplied, in
 * every channel. blend() takes src as opaque (alpha a + dst's under it), which made what was saved
 * over transparency opaque: on the drag layer, a window's shadow outside its rounded corners came back
 * as black squares. Where both are opaque (the screen) this gives blend()'s pixel exactly. */
static inline uint32_t mix_px(uint32_t dst, uint32_t src, uint32_t a) {
    uint32_t ia = 255 - a;
    uint32_t rb = (((src & 0xff00ff) * a + (dst & 0xff00ff) * ia) >> 8) & 0xff00ff;
    uint32_t g  = (((src & 0x00ff00) * a + (dst & 0x00ff00) * ia) >> 8) & 0x00ff00;
    return ((src >> 24) * a + (dst >> 24) * ia + 127) / 255 << 24 | rb | g;
}

static inline void put(uint32_t *p, uint32_t argb, uint32_t a) {
    if (a >= 255) *p = argb | 0xff000000u;
    else if (a) *p = blend(*p, argb, a);
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- rows: portable C and, on ARM64 with NEON=1 (Makefile), NEON ----
 * Both versions give the same bit-exact result for valid premultiplied sources;
 * tests/canvas_test.c checks it (make build/canvas-test). See docs/RASPBERRY.md. */

/* One canvas_blit row without corners: premultiplied source scaled by k (0..256), "over". */
static void blit_row_c(uint32_t *row, const uint32_t *s, int n, uint32_t k) {
    if (!k) return;
    for (int i = 0; i < n; i++) {
        uint32_t p = s[i];
        if (!(p >> 24)) continue;
        if (k >= 256 && p >= 0xff000000u) { row[i] = p; continue; }     /* opaque: copied (same result) */
        uint32_t sa = ((p >> 24) * k) >> 8;
        uint32_t srb = (((p & 0xff00ff) * k) >> 8) & 0xff00ff, sg = (((p & 0xff00) * k) >> 8) & 0xff00;
        uint32_t ia = 255 - sa, d = row[i];
        uint32_t drb = (((d & 0xff00ff) * ia) >> 8) & 0xff00ff, dg = (((d & 0xff00) * ia) >> 8) & 0xff00;
        row[i] = over_alpha(sa, d >> 24) << 24 | (srb + drb) | (sg + dg);
    }
}

/* One canvas_alpha_mask row: argb color with coverage m (0..255) times its alpha ca. */
static void mask_row_c(uint32_t *row, const uint8_t *m, int n, uint32_t argb, uint32_t ca) {
    for (int i = 0; i < n; i++)
        if (m[i]) put(&row[i], argb, (m[i] * ca + 127) / 255);
}

#if defined(LORD_NEON) && defined(__ARM_NEON)
#include <arm_neon.h>

static void blit_row_neon(uint32_t *row, const uint32_t *s, int n, uint32_t k) {
    if (!k) return;
    const uint16x8_t kk = vdupq_n_u16((uint16_t)k);
    const uint32x4_t c255 = vdupq_n_u32(255), c128 = vdupq_n_u32(128), zero = vdupq_n_u32(0), rgb = vdupq_n_u32(0x00ffffff);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32x4_t p = vld1q_u32(s + i), d = vld1q_u32(row + i);
        uint8x16_t p8 = vreinterpretq_u8_u32(p), d8 = vreinterpretq_u8_u32(d);
        /* (c·k) >> 8 on all four channels (c·k ≤ 65280: fits in 16 bits) */
        uint16x8_t lo = vmulq_u16(vmovl_u8(vget_low_u8(p8)), kk), hi = vmulq_u16(vmovl_u8(vget_high_u8(p8)), kk);
        uint8x16_t s8 = vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8));
        uint32x4_t sa = vshrq_n_u32(vreinterpretq_u32_u8(s8), 24), ia = vsubq_u32(c255, sa);
        uint8x16_t ia8 = vreinterpretq_u8_u32(vmulq_n_u32(ia, 0x01010101u));
        uint16x8_t dl = vmull_u8(vget_low_u8(d8), vget_low_u8(ia8)), dh = vmull_u8(vget_high_u8(d8), vget_high_u8(ia8));
        uint8x16_t dd = vcombine_u8(vshrn_n_u16(dl, 8), vshrn_n_u16(dh, 8));
        uint32x4_t col = vandq_u32(vreinterpretq_u32_u8(vaddq_u8(s8, dd)), rgb);
        /* alpha: over_alpha(sa, da) in 32 bits */
        uint32x4_t x = vmlaq_u32(c128, vshrq_n_u32(d, 24), ia);
        uint32x4_t a = vaddq_u32(sa, vshrq_n_u32(vmulq_n_u32(x, 257), 16));
        uint32x4_t out = vorrq_u32(col, vshlq_n_u32(a, 24));
        out = vbslq_u32(vceqq_u32(vshrq_n_u32(p, 24), zero), d, out);     /* transparent source: unchanged */
        vst1q_u32(row + i, out);
    }
    blit_row_c(row + i, s + i, n - i, k);
}

static void mask_row_neon(uint32_t *row, const uint8_t *m, int n, uint32_t argb, uint32_t ca) {
    const uint32x4_t vca = vdupq_n_u32(ca), c127 = vdupq_n_u32(127), c128 = vdupq_n_u32(128), c255 = vdupq_n_u32(255);
    const uint32x4_t zero = vdupq_n_u32(0), rgb = vdupq_n_u32(0x00ffffff), full = vdupq_n_u32(argb | 0xff000000u);
    const uint8x16_t src8 = vreinterpretq_u8_u32(vdupq_n_u32(argb));
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t mm;
        memcpy(&mm, m + i, 4);
        if (!mm) continue;
        uint32x4_t mv = vmovl_u16(vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(mm)))));
        /* exact (m·ca + 127) / 255: x·0x8081 >> 23 for x < 65536 */
        uint32x4_t a = vshrq_n_u32(vmulq_n_u32(vmlaq_u32(c127, mv, vca), 0x8081u), 23), ia = vsubq_u32(c255, a);
        uint8x16_t a8 = vreinterpretq_u8_u32(vmulq_n_u32(a, 0x01010101u)), ia8 = vreinterpretq_u8_u32(vmulq_n_u32(ia, 0x01010101u));
        uint32x4_t d = vld1q_u32(row + i);
        uint8x16_t d8 = vreinterpretq_u8_u32(d);
        uint16x8_t lo = vmlal_u8(vmull_u8(vget_low_u8(src8), vget_low_u8(a8)), vget_low_u8(d8), vget_low_u8(ia8));
        uint16x8_t hi = vmlal_u8(vmull_u8(vget_high_u8(src8), vget_high_u8(a8)), vget_high_u8(d8), vget_high_u8(ia8));
        uint32x4_t col = vandq_u32(vreinterpretq_u32_u8(vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8))), rgb);
        uint32x4_t al = vaddq_u32(a, vshrq_n_u32(vmulq_n_u32(vmlaq_u32(c128, vshrq_n_u32(d, 24), ia), 257), 16));
        uint32x4_t out = vorrq_u32(col, vshlq_n_u32(al, 24));
        out = vbslq_u32(vcgeq_u32(a, c255), full, out);                   /* full coverage */
        out = vbslq_u32(vceqq_u32(a, zero), d, out);                      /* nothing */
        vst1q_u32(row + i, out);
    }
    mask_row_c(row + i, m + i, n - i, argb, ca);
}
#  define blit_row blit_row_neon
#  define mask_row mask_row_neon
#  define CANVAS_SIMD "NEON"
#else
#  define blit_row blit_row_c
#  define mask_row mask_row_c
#  define CANVAS_SIMD "C"
#endif

/* GPU drag's shader (gpu/gpu.c, fs_gdrag) in C, with the same float operations in the same order: a texel read
 * back as c/255 and rounded, then the layer over the copy of what is below with integers held in
 * floats. tests/canvas_test.c checks it against blit_row_c (and blit_row_neon) over every valid
 * premultiplied combination; engine-test composes with it where the GPU would. */
static float gdrag_fetch(uint32_t v) { return floorf((float)v / 255.0f * 255.0f + 0.5f); }
uint32_t canvas_gdrag_model(uint32_t s, uint32_t d) {
    float sc[4], dc[4], o[4];
    for (int i = 0; i < 4; i++) { sc[i] = gdrag_fetch(s >> (8 * i) & 255); dc[i] = gdrag_fetch(d >> (8 * i) & 255); o[i] = dc[i]; }
    if (sc[3] > 0.0f) {                         /* [3]: alpha, [0..2]: blue, green, red */
        float ia = 255.0f - sc[3];
        for (int i = 0; i < 3; i++) o[i] = sc[i] + floorf(dc[i] * ia * 0.00390625f);
        o[3] = sc[3] + floorf((dc[3] * ia + 128.0f) * 257.0f * 0.0000152587890625f);
    }
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) r |= (uint32_t)floorf(o[i] / 255.0f * 255.0f + 0.5f) << (8 * i);
    return r;
}

#ifdef LORD_CANVAS_TEST
const char *canvas_test_simd(void) { return CANVAS_SIMD; }
void canvas_test_blit_row(int simd, uint32_t *row, const uint32_t *s, int n, uint32_t k) {
    if (simd) blit_row(row, s, n, k); else blit_row_c(row, s, n, k);
}
void canvas_test_mask_row(int simd, uint32_t *row, const uint8_t *m, int n, uint32_t argb) {
    if (simd) mask_row(row, m, n, argb, argb >> 24); else mask_row_c(row, m, n, argb, argb >> 24);
}
#endif

/* Signed distance to the rounded rectangle (negative inside). */
static inline float rr_sdf(float px, float py, float cx, float cy, float hw, float hh, float r) {
    float qx = fabsf(px - cx) - (hw - r), qy = fabsf(py - cy) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return (ox || oy ? sqrtf(ox * ox + oy * oy) : 0) + (in < 0 ? in : 0) - r;
}

static inline float cov_of(float d) { float c = 0.5f - d; return c <= 0 ? 0 : c >= 1 ? 1 : c; }

/* The columns [s0, s1) of a rounded rectangle's row whose pixel centers lie between the corners
 * (|px - cx| - lim <= 0, lim = hw - r: the qx of rr_sdf): there rr_sdf doesn't depend on px when the
 * row is above or below the straight band. Empty (s0 = s1 = x1) if there are none. */
static void mid_span(float cx, float lim, int x0, int x1, int *s0, int *s1) {
    int a = (int)ceilf(cx - lim - 0.5f), b = (int)floorf(cx + lim - 0.5f) + 1;
    if (a < x0) a = x0;
    if (b > x1) b = x1;
    /* the same float test as rr_sdf, so the span's edges agree exactly (pixels left out are only
     * worked out per pixel, as before) */
    while (a < b && fabsf(a + 0.5f - cx) - lim > 0) a++;
    while (b > a && fabsf(b - 0.5f - cx) - lim > 0) b--;
    if (a < b) { *s0 = a; *s1 = b; } else { *s0 = *s1 = x1; }
}

static float fix_radius(float r, float w, float h) {
    float m = (w < h ? w : h) * 0.5f;
    return r < 0 ? 0 : r > m ? m : r;
}

/* Clips the bounding box [x, x+w) x [y, y+h) against the clip. */
static int bounds(const Canvas *c, float x, float y, float w, float h, int *x0, int *y0, int *x1, int *y1) {
    *x0 = clampi((int)floorf(x), c->clip.x0, c->clip.x1);
    *y0 = clampi((int)floorf(y), c->clip.y0, c->clip.y1);
    *x1 = clampi((int)ceilf(x + w), c->clip.x0, c->clip.x1);
    *y1 = clampi((int)ceilf(y + h), c->clip.y0, c->clip.y1);
    return *x0 < *x1 && *y0 < *y1;
}

void canvas_init(Canvas *c, uint32_t *px, int w, int h, int stride) {
    c->px = px; c->w = w; c->h = h; c->stride = stride;
    c->clip = (Rect){0, 0, w, h};
}

Rect canvas_push_clip(Canvas *c, float x, float y, float w, float h) {
    Rect old = c->clip;
    Rect n = { (int)floorf(x), (int)floorf(y), (int)ceilf(x + w), (int)ceilf(y + h) };
    if (n.x0 < old.x0) n.x0 = old.x0;
    if (n.y0 < old.y0) n.y0 = old.y0;
    if (n.x1 > old.x1) n.x1 = old.x1;
    if (n.y1 > old.y1) n.y1 = old.y1;
    if (n.x1 < n.x0) n.x1 = n.x0;
    if (n.y1 < n.y0) n.y1 = n.y0;
    c->clip = n;
    return old;
}

void canvas_set_clip(Canvas *c, Rect r) { c->clip = r; }

void canvas_copy(Canvas *dst, const Canvas *src) {
    int w = dst->w < src->w ? dst->w : src->w, h = dst->h < src->h ? dst->h : src->h;
    for (int y = 0; y < h; y++)
        memcpy(dst->px + (size_t)y * dst->stride, src->px + (size_t)y * src->stride, (size_t)w * 4);
}

void canvas_clear(Canvas *c, uint32_t rgb) {
    for (int y = c->clip.y0; y < c->clip.y1; y++) {
        uint32_t *row = c->px + (size_t)y * c->stride;
        for (int x = c->clip.x0; x < c->clip.x1; x++) row[x] = rgb;
    }
}

/* ---- parallel rows (lord-server sets canvas_parallel; tests and helpers stay serial) ----
 * The row-local primitives (every pixel depends only on its own row: fills, strokes, gradients,
 * blits, shadow masks) run as the same call on each thread with the clip cut to a band of rows,
 * so the pixels are the same as on one thread. */
static CanvasPar par_fn;
static _Thread_local int in_band;
void canvas_parallel(CanvasPar fn) { par_fn = fn; }
int canvas_parallel_on(void) { return par_fn != NULL; }

typedef struct { const Canvas *c; void (*draw)(Canvas *, const void *); const void *a; } Band;
static void band_run(void *ctx, int j0, int j1) {
    const Band *b = ctx;
    Canvas c = *b->c;
    if (j0 > c.clip.y0) c.clip.y0 = j0;
    if (j1 < c.clip.y1) c.clip.y1 = j1;
    in_band = 1;
    if (c.clip.y0 < c.clip.y1) b->draw(&c, b->a);
    in_band = 0;
}
/* Worth splitting: a pool, not already in a band, and enough pixels inside the clip. */
static int banded(const Canvas *c, float x, float y, float w, float h) {
    if (!par_fn || in_band) return 0;
    int x0, y0, x1, y1;
    if (!bounds(c, x, y, w, h, &x0, &y0, &x1, &y1)) return 0;
    /* waking the pool costs tens of µs: only big jobs (about 512×512 and up) are worth it */
    return y1 - y0 >= 64 && (long)(x1 - x0) * (y1 - y0) >= 262144;
}
static void in_bands(const Canvas *c, float y, float h, void (*draw)(Canvas *, const void *), const void *a) {
    Band b = { c, draw, a };
    int y0 = clampi((int)floorf(y), c->clip.y0, c->clip.y1), y1 = clampi((int)ceilf(y + h), c->clip.y0, c->clip.y1);
    par_fn(y0, y1, band_run, &b);
}

typedef struct { float x, y, w, h, r, bw, angle, opacity; uint32_t argb, to; } ShapeArgs;
static void rrect_band(Canvas *c, const void *p) { const ShapeArgs *a = p; canvas_rrect(c, a->x, a->y, a->w, a->h, a->r, a->argb); }
static void stroke_band(Canvas *c, const void *p) { const ShapeArgs *a = p; canvas_rrect_stroke(c, a->x, a->y, a->w, a->h, a->r, a->bw, a->argb); }
static void gradient_band(Canvas *c, const void *p) {
    const ShapeArgs *a = p;
    canvas_rrect_gradient(c, a->x, a->y, a->w, a->h, a->r, a->argb, a->to, a->angle, a->opacity);
}

void canvas_rrect(Canvas *c, float x, float y, float w, float h, float r, uint32_t argb) {
    if (banded(c, x, y, w, h)) { ShapeArgs a = { x, y, w, h, r, 0, 0, 0, argb, 0 }; in_bands(c, y, h, rrect_band, &a); return; }
    uint32_t a = argb >> 24;
    int x0, y0, x1, y1;
    if (!a || w <= 0 || h <= 0 || !bounds(c, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    r = fix_radius(r, w, h);
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    int fx0 = (int)ceilf(x), fx1 = (int)floorf(x + w);   /* fully covered columns */
    for (int j = y0; j < y1; j++) {
        uint32_t *row = c->px + (size_t)j * c->stride;
        if (j >= y + r && j + 1 <= y + h - r) {
            /* Straight band: only the left/right edges are partial; the middle, in one run. */
            int m0 = fx0 > x0 ? fx0 : x0, m1 = fx1 < x1 ? fx1 : x1;
            if (m1 < m0) m1 = m0;
            for (int i = x0; i < m0; i++) {
                float cov = fminf(i + 1.0f, x + w) - fmaxf((float)i, x);
                if (cov > 0) put(&row[i], argb, (uint32_t)(a * cov + 0.5f));
            }
            if (a >= 255) { uint32_t v = argb | 0xff000000u; for (int i = m0; i < m1; i++) row[i] = v; }
            else for (int i = m0; i < m1; i++) row[i] = blend(row[i], argb, a);
            for (int i = m1; i < x1; i++) {
                float cov = fminf(i + 1.0f, x + w) - fmaxf((float)i, x);
                if (cov > 0) put(&row[i], argb, (uint32_t)(a * cov + 0.5f));
            }
        } else {
            float py = j + 0.5f;
            /* between the corners the distance only depends on the row (once the row is outside the
             * straight band): worked out once there, per pixel at the corners */
            int s0 = x1, s1 = x1;
            if (fabsf(py - cy) - (hh - r) > 0) mid_span(cx, hw - r, x0, x1, &s0, &s1);
            for (int i = x0; i < x1; i++) {
                if (i == s0) {
                    float cov = cov_of(rr_sdf(i + 0.5f, py, cx, cy, hw, hh, r));
                    if (cov > 0) { uint32_t k = (uint32_t)(a * cov + 0.5f); for (; i < s1; i++) put(&row[i], argb, k); }
                    i = s1 - 1;
                    continue;
                }
                float cov = cov_of(rr_sdf(i + 0.5f, py, cx, cy, hw, hh, r));
                if (cov > 0) put(&row[i], argb, (uint32_t)(a * cov + 0.5f));
            }
        }
    }
}

static inline uint32_t lerp_color(uint32_t a, uint32_t b, float t) {
    int ti = (int)(t * 256);
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8) {
        int ca = (a >> s) & 255, cb = (b >> s) & 255;
        out |= (uint32_t)(ca + (((cb - ca) * ti) >> 8)) << s;
    }
    return out;
}

void canvas_rrect_gradient(Canvas *c, float x, float y, float w, float h, float r,
                           uint32_t from, uint32_t to, float angle_deg, float opacity) {
    if (banded(c, x, y, w, h)) {
        ShapeArgs a = { x, y, w, h, r, 0, angle_deg, opacity, from, to };
        in_bands(c, y, h, gradient_band, &a);
        return;
    }
    int x0, y0, x1, y1;
    if (w <= 0 || h <= 0 || !bounds(c, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    r = fix_radius(r, w, h);
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    /* Same as CSS: 180deg = top to bottom. */
    float rad = angle_deg * 3.14159265f / 180.0f, dx = sinf(rad), dy = -cosf(rad);
    float len = fabsf(w * dx) + fabsf(h * dy);
    if (len <= 0) len = 1;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = c->px + (size_t)j * c->stride;
        float py = j + 0.5f;
        for (int i = x0; i < x1; i++) {
            float px = i + 0.5f;
            float cov = cov_of(rr_sdf(px, py, cx, cy, hw, hh, r));
            if (cov <= 0) continue;
            float t = 0.5f + ((px - cx) * dx + (py - cy) * dy) / len;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            uint32_t col = lerp_color(from, to, t);
            put(&row[i], col, (uint32_t)((col >> 24) * cov * opacity + 0.5f));
        }
    }
}

void canvas_rrect_stroke(Canvas *c, float x, float y, float w, float h, float r, float bw, uint32_t argb) {
    if (banded(c, x, y, w, h)) { ShapeArgs a = { x, y, w, h, r, bw, 0, 0, argb, 0 }; in_bands(c, y, h, stroke_band, &a); return; }
    uint32_t a = argb >> 24;
    int x0, y0, x1, y1;
    if (!a || bw <= 0 || w <= 0 || h <= 0 || !bounds(c, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    r = fix_radius(r, w, h);
    float ir = r - bw > 0 ? r - bw : 0;
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    float edge = (r > bw ? r : bw) + 1;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = c->px + (size_t)j * c->stride;
        float py = j + 0.5f;
        int middle = py > y + edge && py < y + h - edge;
        /* the span where both distances only depend on the row (rows outside both straight bands) */
        int s0 = x1, s1 = x1;
        if (!middle && fabsf(py - cy) - (hh - r) > 0 && fabsf(py - cy) - ((hh - bw) - ir) > 0) {
            int a0, a1, b0, b1;
            mid_span(cx, hw - r, x0, x1, &a0, &a1);
            mid_span(cx, (hw - bw) - ir, x0, x1, &b0, &b1);
            if (a0 < x1 && b0 < x1) {
                s0 = a0 > b0 ? a0 : b0;
                s1 = a1 < b1 ? a1 : b1;
                if (s0 >= s1) s0 = s1 = x1;
            }
        }
        for (int i = x0; i < x1; i++) {
            float px = i + 0.5f;
            if (middle && px > x + bw + 1 && px < x + w - bw - 1) {
                /* skips the middle: up to the last pixel whose center is before the right
                 * edge (never backwards: with fractional coordinates that was a loop) */
                int last = (int)ceilf(x + w - bw - 1 - 0.5f) - 1;
                if (last > i) i = last;
                continue;
            }
            if (i == s0) {                  /* between the corners: one value for the whole span */
                float cov = cov_of(rr_sdf(px, py, cx, cy, hw, hh, r)) - cov_of(rr_sdf(px, py, cx, cy, hw - bw, hh - bw, ir));
                if (cov > 0) { uint32_t k = (uint32_t)(a * cov + 0.5f); for (; i < s1; i++) put(&row[i], argb, k); }
                i = s1 - 1;
                continue;
            }
            float outer = cov_of(rr_sdf(px, py, cx, cy, hw, hh, r));
            float inner = cov_of(rr_sdf(px, py, cx, cy, hw - bw, hh - bw, ir));
            float cov = outer - inner;
            if (cov > 0) put(&row[i], argb, (uint32_t)(a * cov + 0.5f));
        }
    }
}

/* Shadow masks cached by size: moving a window does not recompute the shadow. */
typedef struct {
    int w, h, r, blur, dx, dy;
    int x0, y0, mw, mh;
    uint8_t *a;
    unsigned long used;
} ShadowMask;

#define SHADOW_CACHE 32
static ShadowMask shadow_cache[SHADOW_CACHE];
static unsigned long shadow_stamp;

static ShadowMask *shadow_mask(int w, int h, int r, int blur, int dx, int dy) {
    ShadowMask *m = NULL, *lru = &shadow_cache[0];
    for (int i = 0; i < SHADOW_CACHE; i++) {
        ShadowMask *k = &shadow_cache[i];
        if (k->a && k->w == w && k->h == h && k->r == r && k->blur == blur && k->dx == dx && k->dy == dy) { m = k; break; }
        if (k->used < lru->used) lru = k;
    }
    if (!m) {
        m = lru;
        free(m->a);
        *m = (ShadowMask){ w, h, r, blur, dx, dy, 0, 0, 0, 0, NULL, 0 };
        m->x0 = (dx < 0 ? dx : 0) - blur;
        m->y0 = (dy < 0 ? dy : 0) - blur;
        m->mw = w + abs(dx) + 2 * blur;
        m->mh = h + abs(dy) + 2 * blur;
        m->a = calloc((size_t)m->mw * m->mh, 1);
        if (!m->a) return NULL;
        float fr = fix_radius((float)r, (float)w, (float)h), fb = (float)blur;
        float bcx = w * 0.5f, bcy = h * 0.5f, scx = bcx + dx, scy = bcy + dy, hw = w * 0.5f, hh = h * 0.5f;
        for (int j = 0; j < m->mh; j++)
            for (int i = 0; i < m->mw; i++) {
                float px = m->x0 + i + 0.5f, py = m->y0 + j + 0.5f;
                float inside = cov_of(rr_sdf(px, py, bcx, bcy, hw, hh, fr));
                if (inside >= 1) continue;
                float t = (rr_sdf(px, py, scx, scy, hw, hh, fr) + fb) / (2 * fb);
                if (t >= 1) continue;
                if (t < 0) t = 0;
                float f = (1 - t * t * (3 - 2 * t)) * (1 - inside);
                m->a[(size_t)j * m->mw + i] = (uint8_t)(f * 255 + 0.5f);
            }
    }
    m->used = ++shadow_stamp;
    return m;
}

void canvas_shadow(Canvas *c, float x, float y, float w, float h, float r,
                   float dx, float dy, float blur, uint32_t argb) {
    if (!(argb >> 24) || w <= 0 || h <= 0) return;
    int b = blur < 1 ? 1 : (int)roundf(blur);
    ShadowMask *m = shadow_mask((int)roundf(w), (int)roundf(h), (int)roundf(r), b, (int)roundf(dx), (int)roundf(dy));
    if (m) canvas_alpha_mask(c, (int)roundf(x) + m->x0, (int)roundf(y) + m->y0, m->a, m->mw, m->mh, m->mw, argb);
}

/* Separable box blur, 3 passes ≈ Gaussian, edges repeated. Both directions slide a window sum
 * (the vertical one row by row, for the cache) and divide through a table: dt[sum] = sum / win. */
/* The backdrop blur's state, shared by its phases; each phase runs over rows (or columns) that are its
 * own, on the pool when there is enough to do, with its own scratch buffers. */
typedef struct {
    Canvas *c;
    int x0, y0, x1, y1, bx0, by0, bx1, by1, f, sw, sh, rad, cw, in0, in1;
    float x, y, w, h, r, cx, cy, hw, hh, inv;
    uint32_t *buf, *src;
    const uint8_t *dt;          /* dt[sum] = sum / win */
    int *sum;                   /* per column of the vertical pass */
    int *cx0, *cx1, *cfx;       /* per output column: the two small columns and the fraction */
} Blur;

static void blur_phase(Blur *b, int n, long work, void (*fn)(void *, int, int)) {
    if (par_fn && !in_band && n >= 8 && work >= 131072) par_fn(0, n, fn, b);
    else fn(b, 0, n);
}

/* the f×f average, row by row (each source pixel read once). Two channels share a 32-bit sum
 * (red|blue, alpha|green: at most 16 × 255 each, f <= 4); f = 1 is a copy. */
static void blur_down(void *ctx, int j0, int j1) {
    Blur *b = ctx;
    const Canvas *c = b->c;
    int f = b->f, sw = b->sw, bx0 = b->bx0, bx1 = b->bx1, by0 = b->by0, by1 = b->by1;
    if (f == 1) {
        for (int j = j0; j < j1; j++) memcpy(b->buf + (size_t)j * sw, c->px + (size_t)(by0 + j) * c->stride + bx0, (size_t)sw * 4);
        return;
    }
    uint32_t *acc = malloc((size_t)sw * 2 * 4);
    if (!acc) return;
    int span = bx1 - bx0, full = span / f;
    for (int j = j0; j < j1; j++) {
        memset(acc, 0, (size_t)sw * 8);
        int rows = by1 - (by0 + j * f) < f ? by1 - (by0 + j * f) : f;
        for (int v = 0; v < rows; v++) {
            const uint32_t *row = c->px + (size_t)(by0 + j * f + v) * c->stride + bx0;
            for (int i = 0; i < full; i++) {
                uint32_t rb = 0, ag = 0;
                const uint32_t *q = row + (size_t)i * f;
                for (int u = 0; u < f; u++) { rb += q[u] & 0x00ff00ffu; ag += (q[u] >> 8) & 0x00ff00ffu; }
                acc[i * 2] += rb;
                acc[i * 2 + 1] += ag;
            }
            for (int px = full * f; px < span; px++) {
                acc[full * 2] += row[px] & 0x00ff00ffu;
                acc[full * 2 + 1] += (row[px] >> 8) & 0x00ff00ffu;
            }
        }
        for (int i = 0; i < sw; i++) {
            int cols = bx1 - (bx0 + i * f) < f ? bx1 - (bx0 + i * f) : f;
            uint32_t n = (uint32_t)(cols * rows), rb = acc[i * 2], ag = acc[i * 2 + 1];
            b->buf[(size_t)j * sw + i] = n ? ((ag >> 16) / n) << 24 | ((rb >> 16) / n) << 16 | ((ag & 0xffff) / n) << 8 | ((rb & 0xffff) / n) : 0;
        }
    }
    free(acc);
}

/* Separable box blur, 3 passes ≈ Gaussian, edges repeated. Both directions slide a window sum (the
 * vertical one row by row over a range of columns, for the cache) and divide through dt. */
static void blur_rows(void *ctx, int j0, int j1) {
    Blur *b = ctx;
    int w = b->sw, rad = b->rad;
    const uint8_t *dt = b->dt;
    uint32_t *tmp = malloc((size_t)w * 4);
    if (!tmp) return;
    for (int j = j0; j < j1; j++) {
        uint32_t *row = b->buf + (size_t)j * w;
        memcpy(tmp, row, (size_t)w * 4);
        int sa = 0, sr = 0, sg = 0, sb = 0;
        for (int k = -rad; k <= rad; k++) {
            uint32_t p = tmp[clampi(k, 0, w - 1)];
            sa += p >> 24; sr += (p >> 16) & 255; sg += (p >> 8) & 255; sb += p & 255;
        }
        for (int i = 0; i < w; i++) {
            row[i] = (uint32_t)dt[sa] << 24 | (uint32_t)dt[sr] << 16 | (uint32_t)dt[sg] << 8 | dt[sb];
            int ai = i + rad + 1, si = i - rad;
            uint32_t add = tmp[ai < w ? ai : w - 1], sub = tmp[si > 0 ? si : 0];
            sa += (int)(add >> 24) - (int)(sub >> 24);
            sr += (int)((add >> 16) & 255) - (int)((sub >> 16) & 255);
            sg += (int)((add >> 8) & 255) - (int)((sub >> 8) & 255);
            sb += (int)(add & 255) - (int)(sub & 255);
        }
    }
    free(tmp);
}

static void blur_copy(void *ctx, int j0, int j1) {
    Blur *b = ctx;
    memcpy(b->src + (size_t)j0 * b->sw, b->buf + (size_t)j0 * b->sw, (size_t)(j1 - j0) * b->sw * 4);
}

static void blur_cols(void *ctx, int i0, int i1) {
    Blur *b = ctx;
    int w = b->sw, h = b->sh, rad = b->rad;
    const uint8_t *dt = b->dt;
    const uint32_t *src = b->src;
    int *sum = b->sum;
    memset(sum + (size_t)i0 * 4, 0, (size_t)(i1 - i0) * 4 * sizeof(int));
    for (int k = -rad; k <= rad; k++) {
        const uint32_t *r = src + (size_t)clampi(k, 0, h - 1) * w;
        for (int i = i0; i < i1; i++) {
            uint32_t p = r[i];
            int *s = sum + i * 4;
            s[0] += p >> 24; s[1] += (p >> 16) & 255; s[2] += (p >> 8) & 255; s[3] += p & 255;
        }
    }
    for (int j = 0; j < h; j++) {
        uint32_t *out = b->buf + (size_t)j * w;
        int aj = j + rad + 1, sj = j - rad;
        const uint32_t *add = src + (size_t)(aj < h ? aj : h - 1) * w, *sub = src + (size_t)(sj > 0 ? sj : 0) * w;
        for (int i = i0; i < i1; i++) {
            int *s = sum + i * 4;
            out[i] = (uint32_t)dt[s[0]] << 24 | (uint32_t)dt[s[1]] << 16 | (uint32_t)dt[s[2]] << 8 | dt[s[3]];
            uint32_t a = add[i], d = sub[i];
            s[0] += (int)(a >> 24) - (int)(d >> 24);
            s[1] += (int)((a >> 16) & 255) - (int)((d >> 16) & 255);
            s[2] += (int)((a >> 8) & 255) - (int)((d >> 8) & 255);
            s[3] += (int)(a & 255) - (int)(d & 255);
        }
    }
}

/* Back to full size, over output rows y0 + [j0, j1): bilinear, in fixed point (8 bits of fraction),
 * with each column's position worked out once and each small row interpolated across once (it serves
 * f output rows), not per pixel. */
static void blur_up(void *ctx, int j0, int j1) {
    Blur *b = ctx;
    Canvas *c = b->c;
    int cw = b->cw, sw = b->sw, sh = b->sh, x0 = b->x0, x1 = b->x1, by0 = b->by0, in0 = b->in0, in1 = b->in1;
    float x = b->x, y = b->y, w = b->w, h = b->h, r = b->r, inv = b->inv;
    int *hr = malloc((size_t)cw * 8 * sizeof(int));
    uint32_t *vrow = malloc((size_t)cw * 4);
    if (!hr || !vrow) { free(hr); free(vrow); return; }
    int *hrow[2] = { hr, hr + (size_t)cw * 4 }, hidx[2] = { -1, -1 };
    uint8_t *vrow8 = (uint8_t *)vrow;
    for (int j = b->y0 + j0; j < b->y0 + j1; j++) {
        uint32_t *row = c->px + (size_t)j * c->stride;
        float v = (j - by0 + 0.5f) * inv - 0.5f;
        if (v < 0) v = 0;
        if (v > sh - 1) v = (float)(sh - 1);
        int ry0 = (int)v, ry1 = ry0 + 1 < sh ? ry0 + 1 : ry0, fy = (int)((v - ry0) * 256);
        const int *top = NULL, *bot = NULL;
        for (int want = 0; want < 2; want++) {
            int ry = want ? ry1 : ry0, k = hidx[0] == ry ? 0 : hidx[1] == ry ? 1 : -1;
            if (k < 0) {
                /* reuse the slot that holds neither the other row of this pair nor the top one */
                int keep = want ? (top == hrow[0] ? 0 : 1) : (hidx[0] == ry1 ? 0 : hidx[1] == ry1 ? 1 : -1);
                k = keep == 0 ? 1 : 0;
                const uint32_t *br = b->buf + (size_t)ry * sw;
                int *hh2 = hrow[k];
                for (int i = 0; i < cw; i++) {
                    uint32_t p0 = br[b->cx0[i]], p1 = br[b->cx1[i]];
                    for (int s = 0; s < 4; s++) {
                        int a = (p0 >> (s * 8)) & 255, d = (p1 >> (s * 8)) & 255;
                        hh2[i * 4 + s] = a * 256 + (d - a) * b->cfx[i];
                    }
                }
                hidx[k] = ry;
            }
            if (want) bot = hrow[k]; else top = hrow[k];
        }
        /* the row of blurred pixels: the vertical step for every channel in one plain loop (bytes in
         * memory order are the channels of a little-endian 0xAARRGGBB) */
        for (int k = 0; k < cw * 4; k++) vrow8[k] = (uint8_t)(((top[k] << 8) + (bot[k] - top[k]) * fy) >> 16);
        int corner_row = j < y + r || j + 1 > y + h - r;
        int m0 = corner_row ? x1 : in0, m1 = corner_row ? x1 : in1;
        for (int i = x0; i < x1; i++) {
            if (i == m0) {                  /* inside the edges: no coverage to work out */
                for (; i < m1; i++) {
                    uint32_t p = vrow[i - x0], d = row[i];
                    if ((p >> 24) >= 255) { row[i] = p; continue; }
                    uint32_t pa = p >> 24;
                    uint32_t prb = p & 0xff00ff, pg = p & 0xff00, ia = 255 - pa;
                    uint32_t drb = (((d & 0xff00ff) * ia) >> 8) & 0xff00ff, dg = (((d & 0xff00) * ia) >> 8) & 0xff00;
                    row[i] = over_alpha(pa, d >> 24) << 24 | (prb + drb) | (pg + dg);
                }
                if (i >= x1) break;
            }
            uint32_t a = 255;
            if (corner_row || i < x + 1 || i + 1 > x + w - 1) {
                float cov = cov_of(rr_sdf(i + 0.5f, j + 0.5f, b->cx, b->cy, b->hw, b->hh, r));
                if (cov <= 0) continue;
                a = (uint32_t)(cov * 255 + 0.5f);
            }
            uint32_t p = vrow[i - x0];
            uint32_t d = row[i];
            /* premultiplied with its alpha: over the opaque UI it is the same as before; over the hole of
             * an app window (GPU path) it stays transparent and the app shows through */
            if (a >= 255 && (p >> 24) >= 255) { row[i] = p; continue; }
            uint32_t pa = ((p >> 24) * a + 127) / 255;
            uint32_t prb = (((p & 0xff00ff) * a) / 255) & 0xff00ff, pg = (((p & 0xff00) * a) / 255) & 0xff00;
            uint32_t ia = 255 - pa;
            uint32_t drb = (((d & 0xff00ff) * ia) >> 8) & 0xff00ff, dg = (((d & 0xff00) * ia) >> 8) & 0xff00;
            row[i] = over_alpha(pa, d >> 24) << 24 | (prb + drb) | (pg + dg);
        }
    }
    free(hr);
    free(vrow);
}

void canvas_backdrop_blur(Canvas *c, float x, float y, float w, float h, float r, int radius) {
    Blur b = { .c = c, .x = x, .y = y, .w = w, .h = h };
    if (radius < 1 || w <= 0 || h <= 0 || !bounds(c, x, y, w, h, &b.x0, &b.y0, &b.x1, &b.y1)) return;
    /* Margin so the edge takes color from outside, as in CSS. */
    int m = radius * 2;
    b.bx0 = clampi(b.x0 - m, 0, c->w); b.by0 = clampi(b.y0 - m, 0, c->h);
    b.bx1 = clampi(b.x1 + m, 0, c->w); b.by1 = clampi(b.y1 + m, 0, c->h);
    /* Blurred at reduced resolution: same visual result, much cheaper. */
    b.f = radius >= 12 ? 4 : radius >= 6 ? 2 : 1;
    b.sw = (b.bx1 - b.bx0 + b.f - 1) / b.f;
    b.sh = (b.by1 - b.by0 + b.f - 1) / b.f;
    b.rad = radius / (2 * b.f) > 0 ? radius / (2 * b.f) : 1;
    int win = 2 * b.rad + 1;
    b.cw = b.x1 - b.x0;
    b.buf = malloc((size_t)b.sw * b.sh * 4);
    b.src = malloc((size_t)b.sw * b.sh * 4);
    uint8_t *dt = malloc((size_t)255 * win + 1);
    b.sum = malloc((size_t)b.sw * 4 * sizeof(int));
    b.cx0 = malloc((size_t)b.cw * 3 * sizeof(int));
    if (!b.buf || !b.src || !dt || !b.sum || !b.cx0) goto out;
    for (int k = 0; k <= 255 * win; k++) dt[k] = (uint8_t)(k / win);
    b.dt = dt;
    long small = (long)b.sw * b.sh;
    blur_phase(&b, b.sh, (long)(b.bx1 - b.bx0) * (b.by1 - b.by0), blur_down);
    for (int pass = 0; pass < 3; pass++) {
        blur_phase(&b, b.sh, small, blur_rows);
        blur_phase(&b, b.sh, small, blur_copy);
        blur_phase(&b, b.sw, small, blur_cols);
    }
    b.r = fix_radius(r, w, h);
    b.cx = x + w * 0.5f; b.cy = y + h * 0.5f; b.hw = w * 0.5f; b.hh = h * 0.5f;
    b.inv = 1.0f / b.f;
    b.cx1 = b.cx0 + b.cw;
    b.cfx = b.cx1 + b.cw;
    for (int i = b.x0; i < b.x1; i++) {
        float u = (i - b.bx0 + 0.5f) * b.inv - 0.5f;
        if (u < 0) u = 0;
        if (u > b.sw - 1) u = (float)(b.sw - 1);
        int a = (int)u;
        b.cx0[i - b.x0] = a;
        b.cx1[i - b.x0] = a + 1 < b.sw ? a + 1 : a;
        b.cfx[i - b.x0] = (int)((u - a) * 256);
    }
    /* the columns whose coverage is whole on rows away from the corners: the same float tests as
     * the per-pixel path (i < x + 1, i + 1 > x + w - 1) */
    b.in0 = b.x0; b.in1 = b.x1;
    while (b.in0 < b.x1 && b.in0 < x + 1) b.in0++;
    while (b.in1 > b.in0 && b.in1 - 1 + 1 > x + w - 1) b.in1--;
    if (b.in0 >= b.in1) b.in0 = b.in1 = b.x1;
    blur_phase(&b, b.y1 - b.y0, (long)b.cw * (b.y1 - b.y0), blur_up);
out:
    free(b.buf);
    free(b.src);
    free(dt);
    free(b.sum);
    free(b.cx0);
}

void canvas_corners_save(Canvas *c, Corners *k, float x, float y, float w, float h, float r) {
    r = fix_radius(r, w, h);
    k->n = (int)ceilf(r) + 1;
    k->buf = NULL;
    if (r <= 0) return;
    k->buf = malloc((size_t)k->n * k->n * 4 * 4);
    if (!k->buf) return;
    int xs[4] = { (int)floorf(x), (int)ceilf(x + w) - k->n, (int)floorf(x), (int)ceilf(x + w) - k->n };
    int ys[4] = { (int)floorf(y), (int)floorf(y), (int)ceilf(y + h) - k->n, (int)ceilf(y + h) - k->n };
    for (int q = 0; q < 4; q++) {
        k->x[q] = xs[q]; k->y[q] = ys[q];
        for (int j = 0; j < k->n; j++)
            for (int i = 0; i < k->n; i++) {
                int px = xs[q] + i, py = ys[q] + j;
                uint32_t v = 0;
                if (px >= 0 && py >= 0 && px < c->w && py < c->h) v = c->px[(size_t)py * c->stride + px];
                k->buf[((size_t)q * k->n + j) * k->n + i] = v;
            }
    }
}

void canvas_corners_restore(Canvas *c, Corners *k, float x, float y, float w, float h, float r) {
    if (!k->buf) return;
    r = fix_radius(r, w, h);
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    for (int q = 0; q < 4; q++)
        for (int j = 0; j < k->n; j++)
            for (int i = 0; i < k->n; i++) {
                int px = k->x[q] + i, py = k->y[q] + j;
                if (px < c->clip.x0 || py < c->clip.y0 || px >= c->clip.x1 || py >= c->clip.y1) continue;
                float cov = cov_of(rr_sdf(px + 0.5f, py + 0.5f, cx, cy, hw, hh, r));
                if (cov >= 1) continue;
                uint32_t *d = &c->px[(size_t)py * c->stride + px];
                *d = mix_px(*d, k->buf[((size_t)q * k->n + j) * k->n + i], (uint32_t)((1 - cov) * 255 + 0.5f));
            }
    free(k->buf);
    k->buf = NULL;
}

typedef struct { const void *src; int sw, sh, x, y, stride, opaque, sw_px, sh_px; float r, opacity, sx, sy, fw, fh, dx, dy, dw, dh; uint32_t argb; } BlitArgs;
static void mask_band(Canvas *c, const void *p) { const BlitArgs *b = p; canvas_alpha_mask(c, b->x, b->y, b->src, b->sw, b->sh, b->stride, b->argb); }
static void blit_band(Canvas *c, const void *p) { const BlitArgs *b = p; canvas_blit(c, b->src, b->sw, b->sh, b->x, b->y, b->r, b->opacity, b->opaque); }
static void scaled_band(Canvas *c, const void *p) {
    const BlitArgs *b = p;
    canvas_blit_scaled(c, b->src, b->stride, b->sw_px, b->sh_px, b->sx, b->sy, b->fw, b->fh, b->dx, b->dy, b->dw, b->dh, b->opacity, b->opaque);
}

void canvas_alpha_mask(Canvas *c, int x, int y, const uint8_t *a, int w, int h, int stride, uint32_t argb) {
    if (banded(c, (float)x, (float)y, (float)w, (float)h)) {
        BlitArgs b = { .src = a, .x = x, .y = y, .sw = w, .sh = h, .stride = stride, .argb = argb };
        in_bands(c, (float)y, (float)h, mask_band, &b);
        return;
    }
    uint32_t ca = argb >> 24;
    int x0 = clampi(x, c->clip.x0, c->clip.x1), x1 = clampi(x + w, c->clip.x0, c->clip.x1);
    int y0 = clampi(y, c->clip.y0, c->clip.y1), y1 = clampi(y + h, c->clip.y0, c->clip.y1);
    for (int j = y0; j < y1; j++) {
        uint32_t *row = c->px + (size_t)j * c->stride;
        const uint8_t *m = a + (size_t)(j - y) * stride - x;
        if (x1 > x0) mask_row(row + x0, m + x0, x1 - x0, argb, ca);
    }
}

void canvas_blit(Canvas *c, const uint32_t *src, int sw, int sh, int x, int y, float r, float opacity, int opaque) {
    if (banded(c, (float)x, (float)y, (float)sw, (float)sh)) {
        BlitArgs b = { .src = src, .sw = sw, .sh = sh, .x = x, .y = y, .r = r, .opacity = opacity, .opaque = opaque };
        in_bands(c, (float)y, (float)sh, blit_band, &b);
        return;
    }
    int x0 = clampi(x, c->clip.x0, c->clip.x1), x1 = clampi(x + sw, c->clip.x0, c->clip.x1);
    int y0 = clampi(y, c->clip.y0, c->clip.y1), y1 = clampi(y + sh, c->clip.y0, c->clip.y1);
    if (x0 >= x1 || y0 >= y1) return;
    r = fix_radius(r, (float)sw, (float)sh);
    uint32_t k_op = (uint32_t)(opacity * 256 + 0.5f);
    if (opaque && r <= 0 && k_op >= 256) {
        for (int j = y0; j < y1; j++)
            memcpy(c->px + (size_t)j * c->stride + x0, src + (size_t)(j - y) * sw + (x0 - x), (size_t)(x1 - x0) * 4);
        return;
    }
    float cx = x + sw * 0.5f, cy = y + sh * 0.5f, hw = sw * 0.5f, hh = sh * 0.5f;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = c->px + (size_t)j * c->stride;
        const uint32_t *s = src + (size_t)(j - y) * sw - x;
        int corner_row = j < y + r || j >= y + sh - r;
        if (!corner_row || r <= 0) { blit_row(row + x0, s + x0, x1 - x0, k_op); continue; }   /* no corners */
        for (int i = x0; i < x1; i++) {
            uint32_t p = s[i];
            if (!(p >> 24)) continue;
            uint32_t k = k_op;
            if (corner_row && r > 0 && (i < x + r || i >= x + sw - r))
                k = (uint32_t)(k * cov_of(rr_sdf(i + 0.5f, j + 0.5f, cx, cy, hw, hh, r)));
            if (!k) continue;
            /* src premultiplied scaled by k, then "over". */
            uint32_t sa = ((p >> 24) * k) >> 8;
            uint32_t srb = (((p & 0xff00ff) * k) >> 8) & 0xff00ff, sg = (((p & 0xff00) * k) >> 8) & 0xff00;
            uint32_t ia = 255 - sa, d = row[i];
            uint32_t drb = (((d & 0xff00ff) * ia) >> 8) & 0xff00ff, dg = (((d & 0xff00) * ia) >> 8) & 0xff00;
            row[i] = over_alpha(sa, d >> 24) << 24 | (srb + drb) | (sg + dg);
        }
    }
}

void canvas_bitmap(Canvas *c, int x, int y, const char *const *rows, uint32_t fg, uint32_t bg) {
    for (int j = 0; rows[j]; j++) {
        int py = y + j;
        if (py < c->clip.y0 || py >= c->clip.y1) continue;
        for (int i = 0; rows[j][i]; i++) {
            int px = x + i;
            if (px < c->clip.x0 || px >= c->clip.x1) continue;
            char ch = rows[j][i];
            if (ch == 'X') c->px[(size_t)py * c->stride + px] = fg;
            else if (ch == 'o') c->px[(size_t)py * c->stride + px] = bg;
        }
    }
}

/* Scaled copy of the source rect (sx, sy, sw, sh) of a premultiplied image (stride in pixels) into
 * the destination rect (dx, dy, dw, dh): bilinear when shrinking or growing by a fraction (a
 * Wayland buffer at another scale, wp_viewporter), the same "over" as canvas_blit. Equal sizes at
 * integer positions fall back to canvas_blit. */
void canvas_blit_scaled(Canvas *c, const uint32_t *src, int stride, int sw_px, int sh_px, float sx, float sy, float sw, float sh,
                        float dx, float dy, float dw, float dh, float opacity, int opaque) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || sw_px <= 0 || sh_px <= 0) return;
    if (banded(c, dx, dy, dw, dh)) {
        BlitArgs b = { .src = src, .stride = stride, .sw_px = sw_px, .sh_px = sh_px, .sx = sx, .sy = sy, .fw = sw, .fh = sh,
                       .dx = dx, .dy = dy, .dw = dw, .dh = dh, .opacity = opacity, .opaque = opaque };
        in_bands(c, dy, dh, scaled_band, &b);
        return;
    }
    if (sx == 0 && sy == 0 && sw == sw_px && sh == sh_px && stride == sw_px && dw == sw && dh == sh && dx == floorf(dx) && dy == floorf(dy)) {
        canvas_blit(c, src, sw_px, sh_px, (int)dx, (int)dy, 0, opacity, opaque);
        return;
    }
    int x0 = clampi((int)floorf(dx), c->clip.x0, c->clip.x1), x1 = clampi((int)ceilf(dx + dw), c->clip.x0, c->clip.x1);
    int y0 = clampi((int)floorf(dy), c->clip.y0, c->clip.y1), y1 = clampi((int)ceilf(dy + dh), c->clip.y0, c->clip.y1);
    if (x0 >= x1 || y0 >= y1) return;
    uint32_t k_op = (uint32_t)(opacity * 256 + 0.5f);
    float ux = sw / dw, uy = sh / dh;
    for (int j = y0; j < y1; j++) {
        float fy = sy + (j + 0.5f - dy) * uy - 0.5f;
        if (fy < 0) fy = 0;
        if (fy > sh_px - 1) fy = (float)(sh_px - 1);
        int iy = (int)fy, iy1 = iy + 1 < sh_px ? iy + 1 : iy;
        uint32_t wy = (uint32_t)((fy - iy) * 256);
        const uint32_t *r0 = src + (size_t)iy * stride, *r1 = src + (size_t)iy1 * stride;
        uint32_t *row = c->px + (size_t)j * c->stride;
        for (int i = x0; i < x1; i++) {
            float fx = sx + (i + 0.5f - dx) * ux - 0.5f;
            if (fx < 0) fx = 0;
            if (fx > sw_px - 1) fx = (float)(sw_px - 1);
            int ix = (int)fx, ix1 = ix + 1 < sw_px ? ix + 1 : ix;
            uint32_t wx = (uint32_t)((fx - ix) * 256);
            uint32_t a = r0[ix], b = r0[ix1], cc = r1[ix], d = r1[ix1], p = 0;
            for (int sh8 = 0; sh8 < 32; sh8 += 8) {              /* each channel (premultiplied) */
                uint32_t top = ((a >> sh8 & 255) * (256 - wx) + (b >> sh8 & 255) * wx) >> 8;
                uint32_t bot = ((cc >> sh8 & 255) * (256 - wx) + (d >> sh8 & 255) * wx) >> 8;
                p |= ((top * (256 - wy) + bot * wy) >> 8 & 255) << sh8;
            }
            if (opaque) p |= 0xff000000u;
            if (k_op >= 256 && (p >> 24) == 255) { row[i] = p; continue; }
            uint32_t sa = ((p >> 24) * k_op) >> 8;
            if (!sa) continue;
            uint32_t srb = (((p & 0xff00ff) * k_op) >> 8) & 0xff00ff, sg = (((p & 0xff00) * k_op) >> 8) & 0xff00;
            uint32_t ia = 255 - sa, dd = row[i];
            uint32_t drb = (((dd & 0xff00ff) * ia) >> 8) & 0xff00ff, dg = (((dd & 0xff00) * ia) >> 8) & 0xff00;
            row[i] = over_alpha(sa, dd >> 24) << 24 | (srb + drb) | (sg + dg);
        }
    }
}

/* ---- per-corner radii, per-side borders, affine copies ---- */

/* CSS: radii that don't fit are all scaled down by the same factor. */
static void fix_radii(const float in[4], float w, float h, float r[4]) {
    float f = 1;
    float top = in[0] + in[1], bot = in[3] + in[2], left = in[0] + in[3], right = in[1] + in[2];
    if (top > w && top > 0) f = fminf(f, w / top);
    if (bot > w && bot > 0) f = fminf(f, w / bot);
    if (left > h && left > 0) f = fminf(f, h / left);
    if (right > h && right > 0) f = fminf(f, h / right);
    for (int i = 0; i < 4; i++) r[i] = in[i] > 0 ? in[i] * f : 0;
}

static inline float rr4_sdf(float px, float py, float cx, float cy, float hw, float hh, const float r[4]) {
    float k = px < cx ? (py < cy ? r[0] : r[3]) : (py < cy ? r[1] : r[2]);
    return rr_sdf(px, py, cx, cy, hw, hh, k);
}

void canvas_rrect4(Canvas *c, float x, float y, float w, float h, const float rin[4], uint32_t argb) {
    uint32_t a = argb >> 24;
    int x0, y0, x1, y1;
    if (!a || w <= 0 || h <= 0 || !bounds(c, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    float r[4];
    fix_radii(rin, w, h, r);
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    float top = fmaxf(r[0], r[1]), bot = fmaxf(r[2], r[3]);
    int fx0 = (int)ceilf(x), fx1 = (int)floorf(x + w);
    for (int j = y0; j < y1; j++) {
        uint32_t *row = c->px + (size_t)j * c->stride;
        if (j >= y + top && j + 1 <= y + h - bot) {
            int m0 = fx0 > x0 ? fx0 : x0, m1 = fx1 < x1 ? fx1 : x1;
            if (m1 < m0) m1 = m0;
            for (int i = x0; i < m0; i++) {
                float cov = fminf(i + 1.0f, x + w) - fmaxf((float)i, x);
                if (cov > 0) put(&row[i], argb, (uint32_t)(a * cov + 0.5f));
            }
            if (a >= 255) { uint32_t v = argb | 0xff000000u; for (int i = m0; i < m1; i++) row[i] = v; }
            else for (int i = m0; i < m1; i++) row[i] = blend(row[i], argb, a);
            for (int i = m1; i < x1; i++) {
                float cov = fminf(i + 1.0f, x + w) - fmaxf((float)i, x);
                if (cov > 0) put(&row[i], argb, (uint32_t)(a * cov + 0.5f));
            }
        } else {
            float py = j + 0.5f;
            for (int i = x0; i < x1; i++) {
                float cov = cov_of(rr4_sdf(i + 0.5f, py, cx, cy, hw, hh, r));
                if (cov > 0) put(&row[i], argb, (uint32_t)(a * cov + 0.5f));
            }
        }
    }
}

void canvas_rrect4_gradient(Canvas *c, float x, float y, float w, float h, const float rin[4],
                            uint32_t from, uint32_t to, float angle_deg, float opacity) {
    int x0, y0, x1, y1;
    if (w <= 0 || h <= 0 || !bounds(c, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    float r[4];
    fix_radii(rin, w, h, r);
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    float rad = angle_deg * 3.14159265f / 180.0f, dx = sinf(rad), dy = -cosf(rad);
    float len = fabsf(w * dx) + fabsf(h * dy);
    if (len <= 0) len = 1;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = c->px + (size_t)j * c->stride;
        float py = j + 0.5f;
        for (int i = x0; i < x1; i++) {
            float px = i + 0.5f;
            float cov = cov_of(rr4_sdf(px, py, cx, cy, hw, hh, r));
            if (cov <= 0) continue;
            float t = 0.5f + ((px - cx) * dx + (py - cy) * dy) / len;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            uint32_t col = lerp_color(from, to, t);
            put(&row[i], col, (uint32_t)((col >> 24) * cov * opacity + 0.5f));
        }
    }
}

void canvas_border4(Canvas *c, float x, float y, float w, float h, const float rin[4], const float bw[4], const uint32_t col[4]) {
    int x0, y0, x1, y1;
    if (w <= 0 || h <= 0 || !bounds(c, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    enum { L, T, R, B };
    float r[4];
    fix_radii(rin, w, h, r);
    if (r[0] <= 0 && r[1] <= 0 && r[2] <= 0 && r[3] <= 0) {
        /* square corners: four rectangles (the top and bottom ones span the corners) */
        float bt = fminf(bw[T], h), bb = fminf(bw[B], h - bt);
        if (bt > 0) canvas_rrect(c, x, y, w, bt, 0, col[T]);
        if (bb > 0) canvas_rrect(c, x, y + h - bb, w, bb, 0, col[B]);
        float mh = h - bt - bb;
        if (mh > 0 && bw[L] > 0) canvas_rrect(c, x, y + bt, fminf(bw[L], w), mh, 0, col[L]);
        if (mh > 0 && bw[R] > 0) canvas_rrect(c, x + w - fminf(bw[R], w), y + bt, fminf(bw[R], w), mh, 0, col[R]);
        return;
    }
    float ix = x + bw[L], iy = y + bw[T], iw = w - bw[L] - bw[R], ih = h - bw[T] - bw[B];
    float ir[4] = { fmaxf(0, r[0] - fmaxf(bw[L], bw[T])), fmaxf(0, r[1] - fmaxf(bw[R], bw[T])),
                    fmaxf(0, r[2] - fmaxf(bw[R], bw[B])), fmaxf(0, r[3] - fmaxf(bw[L], bw[B])) };
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    float icx = ix + iw * 0.5f, icy = iy + ih * 0.5f, ihw = iw * 0.5f, ihh = ih * 0.5f;
    int hollow = iw > 0 && ih > 0;
    float etop = fmaxf(bw[T], fmaxf(r[0], r[1])) + 1, ebot = fmaxf(bw[B], fmaxf(r[2], r[3])) + 1;
    float eleft = fmaxf(bw[L], fmaxf(r[0], r[3])) + 1, eright = fmaxf(bw[R], fmaxf(r[1], r[2])) + 1;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = c->px + (size_t)j * c->stride;
        float py = j + 0.5f;
        int middle = hollow && py > y + etop && py < y + h - ebot;
        for (int i = x0; i < x1; i++) {
            float px = i + 0.5f;
            if (middle && px > x + eleft && px < x + w - eright) {
                int last = (int)ceilf(x + w - eright - 0.5f) - 1;
                if (last > i) i = last;
                continue;
            }
            float cov = cov_of(rr4_sdf(px, py, cx, cy, hw, hh, r));
            if (hollow) cov -= cov_of(rr4_sdf(px, py, icx, icy, ihw, ihh, ir));
            if (cov <= 0) continue;
            /* the side whose border band the pixel is deepest in, relative to its width (corner diagonals) */
            float best = 1e30f;
            int side = T;
            float d[4] = { px - x, py - y, x + w - px, y + h - py };
            for (int s = 0; s < 4; s++) {
                if (bw[s] <= 0) continue;
                float v = d[s] / bw[s];
                if (v < best) { best = v; side = s; }
            }
            uint32_t a = col[side] >> 24;
            if (a && bw[side] > 0) put(&row[i], col[side], (uint32_t)(a * cov + 0.5f));
        }
    }
}

void canvas_corners_save4(Canvas *c, Corners *k, float x, float y, float w, float h, const float rin[4]) {
    float r[4];
    fix_radii(rin, w, h, r);
    float m = fmaxf(fmaxf(r[0], r[1]), fmaxf(r[2], r[3]));
    k->n = (int)ceilf(m) + 1;
    k->buf = NULL;
    if (m <= 0) return;
    k->buf = malloc((size_t)k->n * k->n * 4 * 4);
    if (!k->buf) return;
    int xs[4] = { (int)floorf(x), (int)ceilf(x + w) - k->n, (int)floorf(x), (int)ceilf(x + w) - k->n };
    int ys[4] = { (int)floorf(y), (int)floorf(y), (int)ceilf(y + h) - k->n, (int)ceilf(y + h) - k->n };
    for (int q = 0; q < 4; q++) {
        k->x[q] = xs[q]; k->y[q] = ys[q];
        for (int j = 0; j < k->n; j++)
            for (int i = 0; i < k->n; i++) {
                int px = xs[q] + i, py = ys[q] + j;
                uint32_t v = 0;
                if (px >= 0 && py >= 0 && px < c->w && py < c->h) v = c->px[(size_t)py * c->stride + px];
                k->buf[((size_t)q * k->n + j) * k->n + i] = v;
            }
    }
}

void canvas_corners_restore4(Canvas *c, Corners *k, float x, float y, float w, float h, const float rin[4]) {
    if (!k->buf) return;
    float r[4];
    fix_radii(rin, w, h, r);
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    for (int q = 0; q < 4; q++)
        for (int j = 0; j < k->n; j++)
            for (int i = 0; i < k->n; i++) {
                int px = k->x[q] + i, py = k->y[q] + j;
                if (px < c->clip.x0 || py < c->clip.y0 || px >= c->clip.x1 || py >= c->clip.y1) continue;
                float cov = cov_of(rr4_sdf(px + 0.5f, py + 0.5f, cx, cy, hw, hh, r));
                if (cov >= 1) continue;
                uint32_t *d = &c->px[(size_t)py * c->stride + px];
                *d = mix_px(*d, k->buf[((size_t)q * k->n + j) * k->n + i], (uint32_t)((1 - cov) * 255 + 0.5f));
            }
    free(k->buf);
    k->buf = NULL;
}

static inline uint32_t texel(const uint32_t *src, int sw, int sh, int x, int y) {
    return x < 0 || y < 0 || x >= sw || y >= sh ? 0 : src[(size_t)y * sw + x];
}

/* A row of canvas_blit_affine steps its source coordinates from the first pixel it paints, so where
 * the clip cuts the image on the left changes the bilinear weights by a rounding. A partial repaint
 * (paint.c's drag layer) watches its canvas: if an image there starts left of the repaint's own left
 * edge x, the row began at x, where a wider clip would not have begun it, and the caller paints it
 * all instead. */
static const Canvas *affine_watch;
static int affine_watch_x, affine_cut;
void canvas_affine_watch(const Canvas *c, int x) { affine_watch = c; affine_watch_x = x; affine_cut = 0; }
int canvas_affine_cut(void) { affine_watch = NULL; return affine_cut; }

void canvas_blit_affine(Canvas *c, const uint32_t *src, int sw, int sh, const float inv[6], const float fwd[6], float opacity) {
    if (sw <= 0 || sh <= 0) return;
    /* destination bounds: the four transformed corners */
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    const float cs[4][2] = { {0, 0}, {(float)sw, 0}, {(float)sw, (float)sh}, {0, (float)sh} };
    for (int q = 0; q < 4; q++) {
        float px = fwd[0] * cs[q][0] + fwd[2] * cs[q][1] + fwd[4], py = fwd[1] * cs[q][0] + fwd[3] * cs[q][1] + fwd[5];
        bx0 = fminf(bx0, px); by0 = fminf(by0, py); bx1 = fmaxf(bx1, px); by1 = fmaxf(by1, py);
    }
    int x0, y0, x1, y1;
    if (!bounds(c, bx0 - 1, by0 - 1, bx1 - bx0 + 2, by1 - by0 + 2, &x0, &y0, &x1, &y1)) return;
    uint32_t k_op = (uint32_t)(opacity * 256 + 0.5f);
    if (!k_op) return;
    if (c == affine_watch && x0 == affine_watch_x && (int)floorf(bx0 - 1) < x0) affine_cut = 1;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = c->px + (size_t)j * c->stride;
        float dy = j + 0.5f;
        /* source coordinates of the pixel center, stepped along the row */
        float u = inv[0] * (x0 + 0.5f) + inv[2] * dy + inv[4] - 0.5f;
        float v = inv[1] * (x0 + 0.5f) + inv[3] * dy + inv[5] - 0.5f;
        for (int i = x0; i < x1; i++, u += inv[0], v += inv[1]) {
            if (u <= -1 || v <= -1 || u >= sw || v >= sh) continue;
            int ix = (int)floorf(u), iy = (int)floorf(v);
            uint32_t wx = (uint32_t)((u - ix) * 256), wy = (uint32_t)((v - iy) * 256);
            uint32_t a = texel(src, sw, sh, ix, iy), b = texel(src, sw, sh, ix + 1, iy);
            uint32_t cc = texel(src, sw, sh, ix, iy + 1), d = texel(src, sw, sh, ix + 1, iy + 1), p = 0;
            if (!(a | b | cc | d)) continue;
            for (int s8 = 0; s8 < 32; s8 += 8) {
                uint32_t top = ((a >> s8 & 255) * (256 - wx) + (b >> s8 & 255) * wx) >> 8;
                uint32_t bot = ((cc >> s8 & 255) * (256 - wx) + (d >> s8 & 255) * wx) >> 8;
                p |= ((top * (256 - wy) + bot * wy) >> 8 & 255) << s8;
            }
            uint32_t sa = ((p >> 24) * k_op) >> 8;
            if (!sa) continue;
            uint32_t srb = (((p & 0xff00ff) * k_op) >> 8) & 0xff00ff, sg = (((p & 0xff00) * k_op) >> 8) & 0xff00;
            uint32_t ia = 255 - sa, dd = row[i];
            uint32_t drb = (((dd & 0xff00ff) * ia) >> 8) & 0xff00ff, dg = (((dd & 0xff00) * ia) >> 8) & 0xff00;
            row[i] = over_alpha(sa, dd >> 24) << 24 | (srb + drb) | (sg + dg);
        }
    }
}
