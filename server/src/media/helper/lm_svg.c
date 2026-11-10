/* lm_svg.c — SVG icons in lord-media: the limits around nanosvg (see lm_svg.h). */
#include "lm_svg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* nanosvg as it was vendored (third_party/nanosvg, -isystem): only this file compiles its code */
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"

/* the end of a construct that starts at p ("-->", "]]>", "?>"), or NULL */
static const char *find(const char *p, const char *end, const char *what) {
    size_t n = strlen(what);
    for (; p + n <= end; p++) if (!memcmp(p, what, n)) return p + n;
    return NULL;
}

int lm_svg_scan(const char *data, size_t len, int *elements, int *depth) {
    const char *p = data, *end = data + len;
    int n = 0, d = 0, deepest = 0;
    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) break;
        p = lt + 1;
        if (p >= end) return -1;
        if (end - p >= 3 && !memcmp(p, "!--", 3)) { if (!(p = find(p + 3, end, "-->"))) return -1; continue; }
        if (end - p >= 8 && !memcmp(p, "![CDATA[", 8)) { if (!(p = find(p + 8, end, "]]>"))) return -1; continue; }
        if (*p == '?') { if (!(p = find(p + 1, end, "?>"))) return -1; continue; }
        int closing = *p == '/', decl = *p == '!';
        /* the tag's end, past quoted attribute values (a '>' inside one doesn't end it) */
        char quote = 0;
        const char *q = p;
        for (; q < end; q++) {
            if (quote) { if (*q == quote) quote = 0; }
            else if (*q == '"' || *q == '\'') quote = *q;
            else if (*q == '>') break;
        }
        if (q >= end) return -1;
        if (!decl) {
            if (++n > LM_SVG_MAX_ELEMENTS) return -1;
            if (closing) { if (d > 0) d--; }
            else if (q[-1] != '/' && ++d > deepest) deepest = d;
            if (deepest > LM_SVG_MAX_DEPTH) return -1;
        }
        p = q + 1;
    }
    if (elements) *elements = n;
    if (depth) *depth = deepest;
    return 0;
}

static int finite_all(const float *v, int n) {
    for (int i = 0; i < n; i++) if (!isfinite(v[i])) return 0;
    return 1;
}

/* A gradient the rasterizer can index safely: a finite transform of sane size and finite stop offsets (a NaN
 * offset or a transform that overflows turns into an index far outside its color table). */
static int paint_ok(const NSVGpaint *pt) {
    if (pt->type != NSVG_PAINT_LINEAR_GRADIENT && pt->type != NSVG_PAINT_RADIAL_GRADIENT) return 1;
    const NSVGgradient *g = pt->gradient;
    if (!g || g->nstops < 0 || !finite_all(g->xform, 6) || !isfinite(g->fx) || !isfinite(g->fy)) return 0;
    for (int i = 0; i < 6; i++) if (fabsf(g->xform[i]) > 1e6f) return 0;
    for (int i = 0; i < g->nstops; i++) if (!isfinite(g->stops[i].offset)) return 0;
    return 1;
}

/* What the rasterizer would face, checked before it runs: counts, finite numbers, every point within
 * LM_SVG_MAX_REACH sides of the picture, strokes no wider than the picture and dashes no shorter than a quarter
 * pixel (a dash pattern of tiny dashes along a long path is millions of strokes). 0, or -1 too complex. */
static int check_image(NSVGimage *img, float scale, int side) {
    int shapes = 0;
    long points = 0;
    float w = img->width, h = img->height;
    float lo_x = -LM_SVG_MAX_REACH * w, hi_x = (LM_SVG_MAX_REACH + 1) * w;
    float lo_y = -LM_SVG_MAX_REACH * h, hi_y = (LM_SVG_MAX_REACH + 1) * h;
    for (NSVGshape *s = img->shapes; s; s = s->next) {
        if (++shapes > LM_SVG_MAX_SHAPES) return -1;
        if (!isfinite(s->opacity) || !isfinite(s->strokeWidth) || s->strokeWidth < 0 || !isfinite(s->miterLimit) ||
            !finite_all(s->bounds, 4) || !finite_all(s->xform, 6) || !paint_ok(&s->fill) || !paint_ok(&s->stroke))
            return -1;
        if (s->strokeWidth * scale > side) s->strokeWidth = side / scale;
        if (s->strokeDashCount < 0 || s->strokeDashCount > 8) return -1;
        int dashes = s->strokeDashCount > 0;
        for (int i = 0; i < s->strokeDashCount; i++)
            if (!isfinite(s->strokeDashArray[i]) || s->strokeDashArray[i] < 0 || s->strokeDashArray[i] * scale < 0.25f) dashes = 0;
        if (!isfinite(s->strokeDashOffset)) dashes = 0;
        if (!dashes) s->strokeDashCount = 0;
        for (NSVGpath *pa = s->paths; pa; pa = pa->next) {
            if (pa->npts < 0 || (points += pa->npts) > LM_SVG_MAX_POINTS) return -1;
            for (int i = 0; i < pa->npts; i++) {
                float x = pa->pts[i * 2], y = pa->pts[i * 2 + 1];
                if (!isfinite(x) || !isfinite(y) || x < lo_x || x > hi_x || y < lo_y || y > hi_y) return -1;
            }
        }
    }
    return 0;
}

const char *lm_svg_raster(const char *data, size_t len, int side, long recolor, uint8_t **out, int *w, int *h) {
    *out = NULL;
    if (side < LM_SVG_MIN_SIDE || side > LM_SVG_MAX_SIDE || recolor > 0xffffff) return "media.invalidArguments";
    if (len > LM_SVG_MAX_BYTES) return "media.svgTooLarge";
    if (!len || memchr(data, 0, len)) return "media.notAnImage";
    if (lm_svg_scan(data, len, NULL, NULL)) return "media.svgTooComplex";
    char *copy = malloc(len + 1);               /* nsvgParse writes into its input */
    if (!copy) return "sys.outOfMemory";
    memcpy(copy, data, len);
    copy[len] = 0;
    NSVGimage *img = nsvgParse(copy, "px", 96.0f);
    free(copy);
    if (!img) return "media.notAnImage";
    const char *err = NULL;
    float iw = img->width, ih = img->height;
    if (!img->shapes || !isfinite(iw) || !isfinite(ih) || iw <= 0 || ih <= 0 || iw > 1e6f || ih > 1e6f) err = "media.notAnImage";
    float scale = err ? 0 : side / fmaxf(iw, ih);
    if (!err && (!isfinite(scale) || scale <= 0 || scale > 1e4f)) err = "media.notAnImage";
    if (!err && check_image(img, scale, side)) err = "media.svgTooComplex";
    int ow = 0, oh = 0;
    uint8_t *px = NULL;
    NSVGrasterizer *r = NULL;
    if (!err) {
        ow = (int)lroundf(iw * scale);
        oh = (int)lroundf(ih * scale);
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
        if (ow > side) ow = side;
        if (oh > side) oh = side;
        px = malloc((size_t)ow * oh * 4);
        r = nsvgCreateRasterizer();
        if (!px || !r) err = "sys.outOfMemory";
    }
    if (!err) nsvgRasterize(r, img, 0, 0, scale, px, ow, oh, ow * 4);
    if (r) nsvgDeleteRasterizer(r);
    nsvgDelete(img);
    if (err) { free(px); return err; }
    /* straight RGBA → premultiplied BGRA (what lord-server's images hold), with the symbolic color if asked */
    unsigned cr = recolor >= 0 ? (unsigned)(recolor >> 16) & 255 : 0, cg = recolor >= 0 ? (unsigned)(recolor >> 8) & 255 : 0,
             cb = recolor >= 0 ? (unsigned)recolor & 255 : 0;
    for (size_t i = 0; i < (size_t)ow * oh; i++) {
        uint8_t *p = px + i * 4;
        unsigned a = p[3], R = p[0], G = p[1], B = p[2];
        if (recolor >= 0) { R = cr; G = cg; B = cb; }
        p[0] = (uint8_t)((B * a + 127) / 255);
        p[1] = (uint8_t)((G * a + 127) / 255);
        p[2] = (uint8_t)((R * a + 127) / 255);
        p[3] = (uint8_t)a;
    }
    *out = px;
    *w = ow;
    *h = oh;
    return NULL;
}
