/* stfx.c — LordOS Studio's frame pipeline (see st.h). */
#include "st.h"
#include "stgl.h"
#include "nn.h"
#include "models.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__) && defined(__has_attribute)
#if __has_attribute(target_clones)
#define ST_HOT __attribute__((target_clones("arch=x86-64-v3", "default")))   /* AVX2 gathers when the CPU has them */
#endif
#endif
#ifndef ST_HOT
#define ST_HOT
#endif

#define SW STUDIO_SEG_W
#define SH STUDIO_SEG_H
#define GF_R   2                     /* guided filter radius at a quarter of the frame (8 pixels at full size) */
#define GF_EPS 1e-3f

struct StFx {
    Lnn *nn;
    StCfg cfg;
    int force_cpu;
    StGl *gl;
    int gl_failed;
    StStats st;
    /* the segmenter's size */
    float seg_luma[SW * SH], prev_luma[SW * SH], state[SW * SH];
    int first;
    /* the frame's size and everything that depends on it */
    int W, H, gw, gh;
    float *gI, *gp, *ga, *gb, *gtmp, *galpha;    /* gw × gh (gtmp: 6× + gw) */
    float *grgb;                                 /* gw × gh × 3: each guide cell's average colour, 0..255 */
    uint16_t *rowsum;                            /* W × 4 */
    float *rtmp;                                 /* gh × 256 × 3 (the segmenter input's resize) */
    float *blur, *blurtmp;                       /* gw × gh × 4 (blurtmp: + 4·gw) */
    int *xi; float *xl;                          /* per output column: guide column and weight */
    float *rowa, *rowb, *rowbl;                  /* one guide row interpolated (gw, gw, gw × 4) */
    float *al, *bg;                              /* one output row: alpha (W) and background (W × 3) */
    /* the picture */
    uint8_t *pic; int pw, ph;
    uint8_t *pic_fit;                            /* W × H, for the CPU */
    int pic_dirty, pic_gpu_dirty;
};

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

StFx *stfx_new(const void *model, size_t len, int render_fd, char *err, size_t errlen) {
    StFx *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->nn = lnn_load(model, len, err, errlen);
    int h, w, c;
    if (s->nn && (!lnn_input(s->nn, &h, &w, &c) || h != SH || w != SW || c != 3 || lnn_outputs(s->nn) != 1)) {
        if (err && errlen) snprintf(err, errlen, "not the segmenter");
        lnn_free(s->nn);
        s->nn = NULL;
    }
    if (!s->nn) { free(s); return NULL; }
    s->cfg.mode = ST_MODE_BLUR;
    s->cfg.blur = 50;
    s->first = 1;
    if (render_fd >= 0) s->gl = stgl_new(render_fd);
    return s;
}

static void free_sized(StFx *s) {
    free(s->gI); free(s->gp); free(s->ga); free(s->gb); free(s->gtmp); free(s->galpha);
    free(s->blur); free(s->blurtmp); free(s->xi); free(s->xl); free(s->rowa); free(s->rowb); free(s->rowbl);
    free(s->pic_fit); free(s->grgb); free(s->rowsum); free(s->rtmp); free(s->al); free(s->bg);
    s->gI = s->gp = s->ga = s->gb = s->gtmp = s->galpha = s->blur = s->blurtmp = s->xl = s->rowa = s->rowb = s->rowbl = NULL;
    s->grgb = s->al = s->bg = s->rtmp = NULL;
    s->rowsum = NULL;
    s->xi = NULL;
    s->pic_fit = NULL;
    s->W = s->H = 0;
}

void stfx_free(StFx *s) {
    if (!s) return;
    stgl_free(s->gl);
    lnn_free(s->nn);
    free_sized(s);
    free(s->pic);
    free(s);
}

int stfx_config(StFx *s, const StCfg *c) {
    if (!s || !c) return -1;
    s->cfg.mode = c->mode >= 0 && c->mode <= ST_MODE_LAST ? c->mode : ST_MODE_NONE;
    s->cfg.blur = c->blur < 0 ? 0 : c->blur > ST_BLUR_MAX ? ST_BLUR_MAX : c->blur;
    s->cfg.mirror = c->mirror != 0;
    return 0;
}

void stfx_force_cpu(StFx *s, int cpu) { if (s) s->force_cpu = cpu; }
void stfx_reset(StFx *s) { if (s) s->first = 1; }
void stfx_stats(const StFx *s, StStats *o) { if (s && o) *o = s->st; }
const float *stfx_matte(const StFx *s, int *w, int *h) { if (w) *w = SW; if (h) *h = SH; return s ? s->state : NULL; }

int stfx_background(StFx *s, const uint8_t *bgra, int w, int h, int stride) {
    if (!s) return -1;
    free(s->pic);
    s->pic = NULL;
    s->pw = s->ph = 0;
    s->pic_dirty = s->pic_gpu_dirty = 1;
    if (!bgra) return 0;
    if (w < 2 || h < 2 || w > ST_MAX_W || h > ST_MAX_H || stride < w * 4) return -1;
    s->pic = malloc((size_t)w * h * 4);
    if (!s->pic) return -1;
    for (int y = 0; y < h; y++) memcpy(s->pic + (size_t)y * w * 4, bgra + (size_t)y * stride, (size_t)w * 4);
    s->pw = w;
    s->ph = h;
    return 0;
}

static int resize_to(StFx *s, int W, int H) {
    if (s->W == W && s->H == H) return 0;
    free_sized(s);
    int gw = W / 4, gh = H / 4;
    if (gw < 16) gw = 16;
    if (gh < 16) gh = 16;
    size_t n = (size_t)gw * gh;
    s->gI = malloc(n * sizeof(float));
    s->gp = malloc(n * sizeof(float));
    s->ga = malloc(n * sizeof(float));
    s->gb = malloc(n * sizeof(float));
    s->galpha = malloc(n * sizeof(float));
    s->gtmp = malloc((6 * n + (size_t)gw) * sizeof(float));
    s->grgb = malloc(3 * n * sizeof(float));
    s->rowsum = malloc((size_t)W * 4 * sizeof(uint16_t));
    s->rtmp = malloc((size_t)gh * SW * 3 * sizeof(float));
    s->al = malloc((size_t)W * sizeof(float));
    s->bg = malloc((size_t)W * 3 * sizeof(float));
    s->blur = malloc(4 * n * sizeof(float));
    s->blurtmp = malloc((4 * n + 4 * (size_t)gw) * sizeof(float));
    s->xi = malloc((size_t)W * sizeof(int));
    s->xl = malloc((size_t)W * sizeof(float));
    s->rowa = malloc((size_t)gw * sizeof(float));
    s->rowb = malloc((size_t)gw * sizeof(float));
    s->rowbl = malloc((size_t)gw * 4 * sizeof(float));
    if (!s->gI || !s->gp || !s->ga || !s->gb || !s->galpha || !s->gtmp || !s->blur || !s->blurtmp || !s->xi || !s->xl || !s->rowa ||
        !s->rowb || !s->rowbl || !s->grgb || !s->rowsum || !s->rtmp || !s->al || !s->bg) { free_sized(s); return -1; }
    s->W = W;
    s->H = H;
    s->gw = gw;
    s->gh = gh;
    for (int x = 0; x < W; x++) {                /* half-pixel centres, clamped at the edges */
        float fx = (x + 0.5f) * gw / W - 0.5f;
        if (fx < 0) fx = 0;
        int x0 = (int)fx;
        if (x0 > gw - 2) x0 = gw - 2;
        float l = fx - (float)x0;
        s->xi[x] = x0;
        s->xl[x] = l > 1 ? 1 : l;
    }
    s->first = 1;
    s->pic_dirty = 1;
    return 0;
}

/* the picture fitted to cover W×H (centred), bilinear: the same mapping the GPU's shader uses */
static void fit_picture(StFx *s) {
    s->pic_dirty = 0;
    free(s->pic_fit);
    s->pic_fit = NULL;
    if (!s->pic) return;
    int W = s->W, H = s->H, pw = s->pw, ph = s->ph;
    s->pic_fit = malloc((size_t)W * H * 4);
    if (!s->pic_fit) return;
    float sc = fmaxf((float)W / pw, (float)H / ph);
    for (int y = 0; y < H; y++) {
        float fy = ((float)y + 0.5f - H * 0.5f) / sc + ph * 0.5f - 0.5f;
        fy = fy < 0 ? 0 : fy > ph - 1 ? (float)(ph - 1) : fy;
        int y0 = (int)fy, y1 = y0 + 1 < ph ? y0 + 1 : ph - 1;
        float ly = fy - (float)y0;
        for (int x = 0; x < W; x++) {
            float fx = ((float)x + 0.5f - W * 0.5f) / sc + pw * 0.5f - 0.5f;
            fx = fx < 0 ? 0 : fx > pw - 1 ? (float)(pw - 1) : fx;
            int x0 = (int)fx, x1 = x0 + 1 < pw ? x0 + 1 : pw - 1;
            float lx = fx - (float)x0;
            const uint8_t *a = s->pic + ((size_t)y0 * pw + x0) * 4, *b = s->pic + ((size_t)y0 * pw + x1) * 4;
            const uint8_t *c = s->pic + ((size_t)y1 * pw + x0) * 4, *d = s->pic + ((size_t)y1 * pw + x1) * 4;
            uint8_t *o = s->pic_fit + ((size_t)y * W + x) * 4;
            for (int k = 0; k < 4; k++) {
                float t = a[k] + (b[k] - a[k]) * lx, u = c[k] + (d[k] - c[k]) * lx;
                o[k] = (uint8_t)(t + (u - t) * ly + 0.5f);
            }
            o[3] = 255;
        }
    }
}

/* one guide row (gy as a float position) of a plane, interpolated vertically */
static void vrow(const float *p, int gw, int gh, float fy, float *out, int ch) {
    if (fy < 0) fy = 0;
    int y0 = (int)fy;
    if (y0 > gh - 2) y0 = gh - 2;
    float l = fy - (float)y0;
    if (l > 1) l = 1;
    const float *r0 = p + (size_t)y0 * gw * ch, *r1 = r0 + (size_t)gw * ch;
    for (int i = 0; i < gw * ch; i++) out[i] = r0[i] + (r1[i] - r0[i]) * l;
}

/* The CPU composite, a row at a time in loops simple enough to vectorise: the row's alpha, then its background, then
 * the mix (mirrored by writing the row backwards). */
ST_HOT static void compose_cpu(StFx *s, const uint8_t *in, int istride, uint8_t *out, int ostride) {
    const int W = s->W, H = s->H, gw = s->gw, gh = s->gh, mode = s->cfg.mode, mirror = s->cfg.mirror;
    float *restrict al = s->al, *restrict bg = s->bg;
    const int *restrict xi = s->xi;
    const float *restrict xl = s->xl;
    for (int y = 0; y < H; y++) {
        const uint8_t *restrict src = in + (size_t)y * istride;
        uint8_t *restrict dst = out + (size_t)y * ostride;
        if (mode == ST_MODE_NONE) {
            if (!mirror) { memcpy(dst, src, (size_t)W * 4); continue; }
            for (int x = 0; x < W; x++) memcpy(dst + (size_t)(W - 1 - x) * 4, src + (size_t)x * 4, 4);
            continue;
        }
        float fy = (y + 0.5f) * gh / H - 0.5f;
        vrow(s->ga, gw, gh, fy, s->rowa, 1);
        vrow(s->gb, gw, gh, fy, s->rowb, 1);
        const float *restrict ra = s->rowa, *restrict rb = s->rowb;
        for (int x = 0; x < W; x++) {
            int x0 = xi[x];
            float l = xl[x];
            float a = ra[x0] + (ra[x0 + 1] - ra[x0]) * l, b = rb[x0] + (rb[x0 + 1] - rb[x0]) * l;
            const uint8_t *p = src + (size_t)x * 4;
            float I = (0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2]) * (1.0f / 255);
            float v = a * I + b;
            al[x] = v < 0 ? 0 : v > 1 ? 1 : v;
        }
        if (mode == ST_MODE_BLUR) {
            vrow(s->blur, gw, gh, fy, s->rowbl, 4);
            const float *restrict q = s->rowbl;
            for (int x = 0; x < W; x++) {
                int x0 = xi[x] * 4;
                float l = xl[x];
                float wsum = q[x0 + 3] + (q[x0 + 7] - q[x0 + 3]) * l;
                float inv = wsum > 1e-3f ? 1.0f / wsum : 0;
                const uint8_t *p = src + (size_t)x * 4;
                for (int k = 0; k < 3; k++) {
                    float c = (q[x0 + k] + (q[x0 + 4 + k] - q[x0 + k]) * l) * inv;
                    bg[x * 3 + k] = wsum > 1e-3f ? c : p[k];
                }
            }
        } else {
            const uint8_t *pic = s->pic_fit ? s->pic_fit + (size_t)y * W * 4 : NULL;
            for (int x = 0; x < W; x++) for (int k = 0; k < 3; k++) bg[x * 3 + k] = pic ? pic[(size_t)x * 4 + k] : 0;
        }
        for (int x = 0; x < W; x++) {
            const uint8_t *p = src + (size_t)x * 4;
            uint8_t *o = dst + (size_t)(mirror ? W - 1 - x : x) * 4;
            float a = al[x];
            for (int k = 0; k < 3; k++) {
                float v = bg[x * 3 + k] + (p[k] - bg[x * 3 + k]) * a;
                o[k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
            }
            o[3] = 255;
        }
    }
}

int stfx_frame(StFx *s, const uint8_t *in, int istride, int W, int H, uint8_t *out, int ostride) {
    if (!s || !in || !out || W < 64 || H < 64 || W > ST_MAX_W || H > ST_MAX_H || (W & 1) || (H & 1) || istride < W * 4 || ostride < W * 4)
        return -1;
    if (resize_to(s, W, H)) return -1;
    double t0 = now_ms();
    s->st.frames++;
    if (s->cfg.mode != ST_MODE_NONE) {
        /* 1. the network on a 256×144 copy */
        int h, w, c;
        float *x = lnn_input(s->nn, &h, &w, &c);
        st_downscale_guide(in, istride, W, H, s->grgb, s->gI, s->gw, s->gh, s->rowsum);
        st_resize_rgb(s->grgb, s->gw, s->gh, x, SW, SH, s->rtmp);
        for (int i = 0; i < SW * SH; i++) s->seg_luma[i] = 0.299f * x[3 * i] + 0.587f * x[3 * i + 1] + 0.114f * x[3 * i + 2];
        if (lnn_run(s->nn)) return -1;
        size_t n;
        const float *m = lnn_output(s->nn, 0, &n);
        if (!m || n != SW * SH) return -1;
        double t1 = now_ms();
        s->st.seg_ms = t1 - t0;
        /* 2. over time; 3. the guided filter at a quarter of the frame */
        st_temporal(s->state, m, s->seg_luma, s->prev_luma, SW, SH, s->first);
        s->first = 0;
        st_resize_plane(s->state, SW, SH, s->gp, s->gw, s->gh);
        st_guided_coeffs(s->gI, s->gp, s->gw, s->gh, GF_R, GF_EPS, s->ga, s->gb, s->gtmp);
        size_t gn = (size_t)s->gw * s->gh;
        for (size_t i = 0; i < gn; i++) { float al = s->ga[i] * s->gI[i] + s->gb[i]; s->galpha[i] = al < 0 ? 0 : al > 1 ? 1 : al; }
        if (s->cfg.mode == ST_MODE_BLUR) {
            /* the background without the person: each colour weighted by (1 − alpha), blurred, divided back */
            float *bl = s->blur;
            for (size_t i = 0; i < gn; i++) {
                float wgt = 1.0f - s->galpha[i];
                const float *c = s->grgb + i * 3;
                bl[i * 4] = c[2] * wgt; bl[i * 4 + 1] = c[1] * wgt; bl[i * 4 + 2] = c[0] * wgt; bl[i * 4 + 3] = wgt;
            }
            int radius = 1 + s->cfg.blur * 10 / ST_BLUR_MAX;
            st_box_blur4(bl, s->gw, s->gh, radius, s->blurtmp);
        }
        if (s->cfg.mode == ST_MODE_PICTURE && s->pic_dirty) fit_picture(s);
        s->st.refine_ms = now_ms() - t1;
    } else {
        s->st.seg_ms = s->st.refine_ms = 0;
        s->first = 1;
    }
    /* 4. the composite */
    double t2 = now_ms();
    int done = 0;
    if (s->gl && !s->force_cpu && !s->gl_failed) {
        StGlJob j = { .frame = in, .stride = istride, .w = W, .h = H, .a = s->ga, .b = s->gb, .gw = s->gw, .gh = s->gh,
                      .mode = s->cfg.mode, .blur = s->blur, .bw = s->gw, .bh = s->gh, .pic = s->pic, .pw = s->pw, .ph = s->ph,
                      .pstride = s->pw * 4, .pic_changed = s->pic_gpu_dirty, .mirror = s->cfg.mirror, .out = out, .ostride = ostride };
        if (stgl_compose(s->gl, &j) == 0) {
            done = 1;
            if (s->cfg.mode == ST_MODE_PICTURE && s->pic) s->pic_gpu_dirty = 0;     /* uploaded only in that mode */
        }
        else { s->gl_failed = 1; fprintf(stderr, "lord-studio: the GPU composite failed; the CPU does it from now on\n"); }
    }
    if (!done) compose_cpu(s, in, istride, out, ostride);
    s->st.gpu = done;
    s->st.compose_ms = now_ms() - t2;
    return 0;
}
