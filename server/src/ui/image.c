/* image.c — PNG/JPEG loading (stb_image), cached scaling and drawing. */
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image.h"

struct Image {
    char *path;
    int w, h, opaque;
    uint32_t *px;                   /* premultiplied ARGB */
    uint32_t *scaled;               /* last scaled version */
    int sw, sh, sfit;
    uint32_t *scaled2;              /* the previous one: an icon at two sizes (Dock and bar) is not rescaled every frame */
    int sw2, sh2, sfit2;
    long ver;                       /* the "#<version>" it was loaded for (image_get) */
    struct Image *next;
};

static struct Image *images;

/* the file's pixels, premultiplied; NULL if it doesn't load */
static uint32_t *load_file(const char *path, int *w, int *h, int *opaque) {
    int ch;
    unsigned char *rgba = stbi_load(path, w, h, &ch, 4);
    if (!rgba) { fprintf(stderr, "image: could not load %s\n", path); return NULL; }
    uint32_t *px = malloc((size_t)*w * *h * 4);
    *opaque = 1;
    for (size_t i = 0; px && i < (size_t)*w * *h; i++) {
        uint32_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
        if (a < 255) *opaque = 0;
        px[i] = a << 24 | ((r * a + 127) / 255) << 16 | ((g * a + 127) / 255) << 8 | ((b * a + 127) / 255);
    }
    stbi_image_free(rgba);
    return px;
}

/* A path may end in "#<digits>": a version of a file that is rewritten in place (Remote Desk's card pictures). The
 * cache keeps one entry for the file and loads it again, into the same Image, when the version changes. */
Image *image_get(const char *spec) {
    char path[4096];
    const char *hash = strrchr(spec, '#');
    long ver = 0;
    size_t dl = hash ? strspn(hash + 1, "0123456789") : 0;
    if (hash && dl && dl <= 18 && !hash[1 + dl] && (size_t)(hash - spec) < sizeof path) {
        memcpy(path, spec, (size_t)(hash - spec));
        path[hash - spec] = 0;
        ver = strtol(hash + 1, NULL, 10);
    } else {
        snprintf(path, sizeof path, "%s", spec);
        hash = NULL;
    }
    for (Image *i = images; i; i = i->next) {
        if (strcmp(i->path, path)) continue;
        if (!hash || i->ver == ver) return i;
        int w, h, opaque;
        uint32_t *px = load_file(path, &w, &h, &opaque);
        if (!px) return i;                     /* the last one it had */
        free(i->px); free(i->scaled); free(i->scaled2);
        i->scaled = i->scaled2 = NULL;
        i->sw = i->sh = i->sw2 = i->sh2 = 0;
        i->px = px; i->w = w; i->h = h; i->opaque = opaque; i->ver = ver;
        return i;
    }
    int w, h, opaque;
    uint32_t *px = load_file(path, &w, &h, &opaque);
    if (!px) return NULL;
    Image *img = calloc(1, sizeof *img);
    if (!img) { free(px); return NULL; }
    img->path = strdup(path);
    img->w = w; img->h = h; img->opaque = opaque; img->px = px; img->ver = ver;
    img->next = images;
    images = img;
    return img;
}

void image_size(const Image *img, int *w, int *h) { *w = img->w; *h = img->h; }

/* A node's own image (`pixels` prop: an app's canvas, e.g. drawn from WASM).
 * It does not go into the path cache; it is reused if the size doesn't change. rgba = w*h*4 bytes. */
Image *image_pixels(Image *img, int w, int h, const uint8_t *rgba) {
    if (img && (img->path || img->w != w || img->h != h)) { image_release(img); img = NULL; }
    if (!img) {
        img = calloc(1, sizeof *img);
        if (!img) return NULL;
        img->w = w; img->h = h;
        img->px = malloc((size_t)w * h * 4);
        if (!img->px) { free(img); return NULL; }
    }
    img->opaque = 1;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
        if (a < 255) img->opaque = 0;
        img->px[i] = a << 24 | ((r * a + 127) / 255) << 16 | ((g * a + 127) / 255) << 8 | ((b * a + 127) / 255);
    }
    free(img->scaled);
    free(img->scaled2);
    img->scaled = img->scaled2 = NULL;
    img->sw = img->sh = img->sw2 = img->sh2 = 0;
    return img;
}

/* Same as image_pixels but with ARGB32 pixels already premultiplied (the ones returned by the
 * document helper, doc/doc.c: PDF pages, font samples). */
Image *image_argb(Image *img, int w, int h, const uint32_t *argb) {
    if (img && (img->path || img->w != w || img->h != h)) { image_release(img); img = NULL; }
    if (!img) {
        img = calloc(1, sizeof *img);
        if (!img) return NULL;
        img->w = w; img->h = h;
        img->px = malloc((size_t)w * h * 4);
        if (!img->px) { free(img); return NULL; }
    }
    memcpy(img->px, argb, (size_t)w * h * 4);
    img->opaque = 1;
    for (size_t i = 0; i < (size_t)w * h; i++) if ((argb[i] >> 24) != 255) { img->opaque = 0; break; }
    free(img->scaled);
    free(img->scaled2);
    img->scaled = img->scaled2 = NULL;
    img->sw = img->sh = img->sw2 = img->sh2 = 0;
    return img;
}

/* Frees an image from image_pixels (file images stay in the cache). */
void image_release(Image *img) {
    if (!img || img->path) return;
    free(img->px);
    free(img->scaled);
    free(img->scaled2);
    free(img);
}

static uint32_t sample_bilinear(const Image *img, float u, float v) {
    if (u < 0) u = 0;
    if (v < 0) v = 0;
    if (u > img->w - 1) u = (float)(img->w - 1);
    if (v > img->h - 1) v = (float)(img->h - 1);
    int x0 = (int)u, y0 = (int)v;
    int x1 = x0 + 1 < img->w ? x0 + 1 : x0, y1 = y0 + 1 < img->h ? y0 + 1 : y0;
    float fx = u - x0, fy = v - y0;
    uint32_t p[4] = { img->px[(size_t)y0 * img->w + x0], img->px[(size_t)y0 * img->w + x1],
                      img->px[(size_t)y1 * img->w + x0], img->px[(size_t)y1 * img->w + x1] };
    float wt[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy };
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8) {
        float acc = 0;
        for (int k = 0; k < 4; k++) acc += ((p[k] >> s) & 255) * wt[k];
        out |= (uint32_t)(acc + 0.5f) << s;
    }
    return out;
}

/* Area averaging for strong downscaling (large icons → dock). */
static uint32_t sample_box(const Image *img, float u0, float v0, float u1, float v1) {
    int x0 = (int)floorf(u0), y0 = (int)floorf(v0), x1 = (int)ceilf(u1), y1 = (int)ceilf(v1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > img->w) x1 = img->w;
    if (y1 > img->h) y1 = img->h;
    uint32_t acc[4] = {0}, n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t p = img->px[(size_t)y * img->w + x];
            acc[0] += p & 255; acc[1] += (p >> 8) & 255; acc[2] += (p >> 16) & 255; acc[3] += p >> 24;
            n++;
        }
    if (!n) return 0;
    return (acc[3] / n) << 24 | (acc[2] / n) << 16 | (acc[1] / n) << 8 | (acc[0] / n);
}

static void rescale(Image *img, int bw, int bh, int fit) {
    if (img->scaled && img->sw == bw && img->sh == bh && img->sfit == fit) return;
    /* the other saved version: they are swapped */
    uint32_t *t = img->scaled; img->scaled = img->scaled2; img->scaled2 = t;
    int tw = img->sw, th = img->sh, tf = img->sfit;
    img->sw = img->sw2; img->sh = img->sh2; img->sfit = img->sfit2;
    img->sw2 = tw; img->sh2 = th; img->sfit2 = tf;
    if (img->scaled && img->sw == bw && img->sh == bh && img->sfit == fit) return;
    free(img->scaled);
    img->scaled = calloc((size_t)bw * bh, 4);
    if (!img->scaled) { img->sw = img->sh = 0; return; }
    img->sw = bw; img->sh = bh; img->sfit = fit;
    float sx = (float)bw / img->w, sy = (float)bh / img->h, ox = 0, oy = 0;
    if (fit != FIT_FILL) {
        float s = fit == FIT_COVER ? fmaxf(sx, sy) : fminf(sx, sy);
        sx = sy = s;
        ox = (bw - img->w * s) / 2;
        oy = (bh - img->h * s) / 2;
    }
    int box = sx < 0.75f || sy < 0.75f;
    for (int j = 0; j < bh; j++)
        for (int i = 0; i < bw; i++) {
            float u0 = (i - ox) / sx, v0 = (j - oy) / sy, u1 = (i + 1 - ox) / sx, v1 = (j + 1 - oy) / sy;
            if (u1 <= 0 || v1 <= 0 || u0 >= img->w || v0 >= img->h) continue;   /* contain margin */
            img->scaled[(size_t)j * bw + i] = box ? sample_box(img, u0, v0, u1, v1)
                                                  : sample_bilinear(img, (u0 + u1) / 2 - 0.5f, (v0 + v1) / 2 - 0.5f);
        }
}

void image_draw(Canvas *c, Image *img, float x, float y, float w, float h, float r, int fit, float opacity) {
    int bw = (int)roundf(w), bh = (int)roundf(h);
    if (bw <= 0 || bh <= 0) return;
    /* outside what is being repainted: nothing (not even rescaling) */
    int ix = (int)roundf(x), iy = (int)roundf(y);
    if (ix >= c->clip.x1 || iy >= c->clip.y1 || ix + bw <= c->clip.x0 || iy + bh <= c->clip.y0) return;
    if (bw == img->w && bh == img->h && img->px) {      /* 1:1 (a scrolled map, a new frame): no rescaling */
        canvas_blit(c, img->px, bw, bh, (int)roundf(x), (int)roundf(y), r, opacity, img->opaque);
        return;
    }
    rescale(img, bw, bh, fit);
    int opaque = img->opaque && (fit != FIT_CONTAIN);
    if (!img->scaled) return;
    canvas_blit(c, img->scaled, bw, bh, (int)roundf(x), (int)roundf(y), r, opacity, opaque);
}
