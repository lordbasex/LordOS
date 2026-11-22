/* stfx.c — LordOS Studio's frame pipeline (see st.h). */
#include "st.h"
#include "stgl.h"
#include "nn.h"
#include "face.h"
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
#define FS STUDIO_FACE_SIDE
#define GF_R   2                     /* guided filter radius at a quarter of the frame (8 pixels at full size) */
#define GF_EPS 1e-3f
#define FACE_EVERY 6                 /* BlazeFace on every 6th frame: 5 a second is enough with the 1€ filter (plan 3.2) */
#define FACE_KEEP_MS 2000            /* no face for 2 s: Center Stage eases back to the whole frame (plan 5.2) */
#define KEY_DILATE 3                 /* the person's mask dilated by 3 guide cells (12 pixels) for the green screen */

struct StFx {
    Lnn *nn, *face_nn;
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
    float *rowa, *rowb, *rowbl, *rowd;           /* one guide row interpolated (gw, gw, gw × 4, gw) */
    float *al, *bg, *fg;                         /* one output row: alpha (W), background and person (W × 3) */
    float *dil, *diltmp, *glight;                /* gw × gh: the dilated person (green screen); Studio Light's gain */
    float *bgg, *bgy;                            /* gw × gh × 3 and gw × gh: the picture's guide cells (colour match) */
    uint8_t *work;                               /* W × H × 4: the frame after Center Stage and Retouch */
    /* the picture */
    uint8_t *pic; int pw, ph;
    uint8_t *pic_fit;                            /* W × H, for the CPU */
    int pic_dirty, pic_gpu_dirty, pic_stats_dirty;
    /* faces and Center Stage */
    int face_every, frame_no;
    float face[4], eyes_y;                       /* the last face seen, in the camera frame's pixels */
    double face_at, now;                         /* when it was seen; this frame's time (ms) */
    int have_face;
    StEuro ex, ey, eh;
    StRect crop;
    float goal[3];                               /* the target's centre and height, kept within the dead zone */
    double last_t;
    float *rt_tmp; size_t rt_cap;
    float face_in[FS * FS * 3];
    /* the looks */
    float key[2]; int key_age;
    float M[9], o[3]; int match_on;
    uint8_t lut[ST_LUT_N * ST_LUT_N * ST_LUT_N * 3];
    StLook look;                                 /* the same filter for the CPU: a matrix and three curves */
    int lut_for;                                 /* the filter lut holds, -1 none */
    int lut_gpu_dirty;
};

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }

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
    s->cfg.similarity = 40;
    s->cfg.smoothness = 20;
    s->cfg.spill = 50;
    s->cfg.filter_amt = 100;
    s->first = 1;
    s->face_every = FACE_EVERY;
    s->lut_for = -1;
    for (int i = 0; i < 9; i++) s->M[i] = i % 4 == 0 ? 1.0f : 0.0f;
    if (render_fd >= 0) s->gl = stgl_new(render_fd);
    return s;
}

int stfx_face_model(StFx *s, const void *model, size_t len, char *err, size_t errlen) {
    if (!s) return -1;
    lnn_free(s->face_nn);
    s->face_nn = lnn_load(model, len, err, errlen);
    int h, w, c;
    if (s->face_nn && (!lnn_input(s->face_nn, &h, &w, &c) || h != FS || w != FS || c != 3 || lnn_outputs(s->face_nn) != 2)) {
        if (err && errlen) snprintf(err, errlen, "not BlazeFace");
        lnn_free(s->face_nn);
        s->face_nn = NULL;
    }
    return s->face_nn ? 0 : -1;
}

static void free_sized(StFx *s) {
    free(s->gI); free(s->gp); free(s->ga); free(s->gb); free(s->gtmp); free(s->galpha);
    free(s->blur); free(s->blurtmp); free(s->xi); free(s->xl); free(s->rowa); free(s->rowb); free(s->rowbl); free(s->rowd);
    free(s->pic_fit); free(s->grgb); free(s->rowsum); free(s->rtmp); free(s->al); free(s->bg); free(s->fg);
    free(s->dil); free(s->diltmp); free(s->glight); free(s->bgg); free(s->bgy); free(s->work);
    s->gI = s->gp = s->ga = s->gb = s->gtmp = s->galpha = s->blur = s->blurtmp = s->xl = s->rowa = s->rowb = s->rowbl = s->rowd = NULL;
    s->grgb = s->al = s->bg = s->fg = s->rtmp = s->dil = s->diltmp = s->glight = s->bgg = s->bgy = NULL;
    s->rowsum = NULL;
    s->xi = NULL;
    s->pic_fit = s->work = NULL;
    s->W = s->H = 0;
}

void stfx_free(StFx *s) {
    if (!s) return;
    stgl_free(s->gl);
    lnn_free(s->nn);
    lnn_free(s->face_nn);
    free_sized(s);
    free(s->pic);
    free(s->rt_tmp);
    free(s);
}

static int clamp100(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }

int stfx_config(StFx *s, const StCfg *c) {
    if (!s || !c) return -1;
    s->cfg.mode = c->mode >= 0 && c->mode <= ST_MODE_LAST ? c->mode : ST_MODE_NONE;
    s->cfg.blur = c->blur < 0 ? 0 : c->blur > ST_BLUR_MAX ? ST_BLUR_MAX : c->blur;
    s->cfg.mirror = c->mirror != 0;
    s->cfg.key = c->key >= 0 && c->key <= ST_KEY_LAST ? c->key : ST_KEY_NONE;
    if (s->cfg.key != c->key || !c->key) s->key_age = 0;
    s->cfg.similarity = clamp100(c->similarity);
    s->cfg.smoothness = clamp100(c->smoothness);
    s->cfg.spill = clamp100(c->spill);
    s->cfg.framing = clamp100(c->framing);
    s->cfg.light = clamp100(c->light);
    s->cfg.match = c->match < 0 ? 0 : c->match > ST_MATCH_MAX ? ST_MATCH_MAX : c->match;
    s->cfg.filter = c->filter >= 0 && c->filter <= ST_FILTER_LAST ? c->filter : ST_FILTER_NONE;
    s->cfg.filter_amt = clamp100(c->filter_amt);
    s->cfg.retouch = clamp100(c->retouch);
    if (s->cfg.filter != s->lut_for) {
        if (s->cfg.filter && !st_lut_build(s->cfg.filter, s->lut) && !st_look_build(s->cfg.filter, &s->look)) { s->lut_for = s->cfg.filter; s->lut_gpu_dirty = 1; }
        else s->lut_for = -1;
    }
    if (!s->cfg.match) { s->match_on = 0; for (int i = 0; i < 9; i++) s->M[i] = i % 4 == 0 ? 1.0f : 0.0f; s->o[0] = s->o[1] = s->o[2] = 0; }
    return 0;
}

void stfx_force_cpu(StFx *s, int cpu) { if (s) s->force_cpu = cpu; }
void stfx_reset(StFx *s) {
    if (!s) return;
    s->first = 1;
    s->have_face = 0;
    s->frame_no = 0;
    memset(&s->ex, 0, sizeof s->ex);
    memset(&s->ey, 0, sizeof s->ey);
    memset(&s->eh, 0, sizeof s->eh);
    s->goal[2] = 0;
    s->match_on = 0;
    s->key_age = 0;
}
void stfx_stats(const StFx *s, StStats *o) { if (s && o) *o = s->st; }
const float *stfx_matte(const StFx *s, int *w, int *h) { if (w) *w = SW; if (h) *h = SH; return s ? s->state : NULL; }
void stfx_face_every(StFx *s, int n) { if (s) s->face_every = n < 1 ? 1 : n; }
int stfx_has_faces(const StFx *s) { return s && s->face_nn; }

int stfx_framing(const StFx *s, StRect *crop, float face[4]) {
    if (!s) return 0;
    if (crop) *crop = s->crop;
    if (face) {
        for (int i = 0; i < 4; i++) face[i] = 0;
        if (s->have_face && s->W > 0) { face[0] = s->face[0] / s->W; face[1] = s->face[1] / s->H; face[2] = s->face[2] / s->W; face[3] = s->face[3] / s->H; }
    }
    return s->have_face;
}

int stfx_background(StFx *s, const uint8_t *bgra, int w, int h, int stride) {
    if (!s) return -1;
    s->pic_dirty = s->pic_gpu_dirty = s->pic_stats_dirty = 1;
    if (!bgra) { free(s->pic); s->pic = NULL; s->pw = s->ph = 0; return 0; }
    if (w < 2 || h < 2 || w > ST_MAX_W || h > ST_MAX_H || stride < w * 4) { free(s->pic); s->pic = NULL; s->pw = s->ph = 0; return -1; }
    if (!s->pic || s->pw != w || s->ph != h) {       /* a video's next frame reuses the buffer */
        free(s->pic);
        s->pic = malloc((size_t)w * h * 4);
        s->pw = s->ph = 0;
        if (!s->pic) return -1;
    }
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
    s->fg = malloc((size_t)W * 3 * sizeof(float));
    s->blur = malloc(4 * n * sizeof(float));
    s->blurtmp = malloc((4 * n + 4 * (size_t)gw) * sizeof(float));
    s->xi = malloc((size_t)W * sizeof(int));
    s->xl = malloc((size_t)W * sizeof(float));
    s->rowa = malloc((size_t)gw * sizeof(float));
    s->rowb = malloc((size_t)gw * sizeof(float));
    s->rowbl = malloc((size_t)gw * 4 * sizeof(float));
    s->rowd = malloc((size_t)gw * sizeof(float));
    s->dil = malloc(n * sizeof(float));
    s->diltmp = malloc(n * sizeof(float));
    s->bgg = malloc(3 * n * sizeof(float));
    s->bgy = malloc(n * sizeof(float));
    s->glight = malloc(n * sizeof(float));
    s->work = malloc((size_t)W * H * 4);
    if (!s->gI || !s->gp || !s->ga || !s->gb || !s->galpha || !s->gtmp || !s->blur || !s->blurtmp || !s->xi || !s->xl || !s->rowa ||
        !s->rowb || !s->rowbl || !s->rowd || !s->grgb || !s->rowsum || !s->rtmp || !s->al || !s->bg || !s->fg || !s->dil || !s->diltmp ||
        !s->bgg || !s->bgy || !s->glight || !s->work) { free_sized(s); return -1; }
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
    s->pic_dirty = s->pic_stats_dirty = 1;
    s->crop = (StRect){ 0, 0, (float)W, (float)H };
    s->have_face = 0;
    memset(&s->ex, 0, sizeof s->ex);
    memset(&s->ey, 0, sizeof s->ey);
    memset(&s->eh, 0, sizeof s->eh);
    s->goal[2] = 0;
    return 0;
}

/* the picture fitted to cover W×H (centred), bilinear: the same mapping the GPU's shader uses */
static void fit_picture(StFx *s) {
    s->pic_dirty = 0;
    if (!s->pic) { free(s->pic_fit); s->pic_fit = NULL; return; }
    int W = s->W, H = s->H, pw = s->pw, ph = s->ph;
    if (pw == W && ph == H) { free(s->pic_fit); s->pic_fit = NULL; return; }    /* used as it is (a video's frames) */
    if (!s->pic_fit) s->pic_fit = malloc((size_t)W * H * 4);
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

/* the background picture at the frame's size (the fitted copy, or the picture itself when it already is that size) */
static const uint8_t *picture_at_size(const StFx *s) {
    if (!s->pic) return NULL;
    if (s->pw == s->W && s->ph == s->H) return s->pic;
    return s->pic_fit;
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
 * the mix (mirrored by writing the row backwards). This is phase 1's path (blur, picture and mirror only). */
ST_HOT static void compose_cpu(StFx *s, const uint8_t *in, int istride, uint8_t *out, int ostride) {
    const int W = s->W, H = s->H, gw = s->gw, gh = s->gh, mode = s->cfg.mode, mirror = s->cfg.mirror;
    float *restrict al = s->al, *restrict bg = s->bg;
    const int *restrict xi = s->xi;
    const float *restrict xl = s->xl;
    const uint8_t *picfull = picture_at_size(s);
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
            const uint8_t *pic = picfull ? picfull + (size_t)y * W * 4 : NULL;
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

/* Studio Light's lift of the person: a soft one, so highlights roll off instead of clipping (c·g / (1 + (g−1)·c)) */
static inline float lift(float c, float g) {
    float x = c * (1.0f / 255);
    return 255 * x * g / (1 + (g - 1) * x);
}

/* The CPU composite with phase 3's looks: the green screen, the colour match, Studio Light and the filter. The same
 * arithmetic as stgl.c's shader (Studio Light's gain comes from its plane at the guide's size, s->glight), done as
 * passes over a row of floats (RGB) so the loops stay simple. */
ST_HOT static void compose_cpu_fx(StFx *s, const uint8_t *in, int istride, uint8_t *out, int ostride) {
    const int W = s->W, H = s->H, gw = s->gw, gh = s->gh, mode = s->cfg.mode, mirror = s->cfg.mirror;
    const int matte = mode != ST_MODE_NONE || s->cfg.light > 0;
    const int key = (mode == ST_MODE_PICTURE || mode == ST_MODE_VIDEO) ? s->cfg.key : ST_KEY_NONE;
    const int match = s->match_on && (mode == ST_MODE_PICTURE || mode == ST_MODE_VIDEO);
    const int light = s->cfg.light;
    const int lut = s->cfg.filter && s->lut_for == s->cfg.filter && s->cfg.filter_amt > 0;
    const float amt = s->cfg.filter_amt / 100.0f, spill = s->cfg.spill / 100.0f, gbg = light ? st_light_background(light) : 1;
    const uint8_t *picfull = picture_at_size(s);
    float *restrict al = s->al, *restrict fg = s->fg, *restrict bg = s->bg;
    const int *restrict xi = s->xi;
    const float *restrict xl = s->xl;
    const StLook *lk = &s->look;
    const float cn = (ST_CURVE_N - 1) / 255.0f;
    for (int y = 0; y < H; y++) {
        const uint8_t *restrict src = in + (size_t)y * istride;
        uint8_t *restrict dst = out + (size_t)y * ostride;
        float fy = (y + 0.5f) * gh / H - 0.5f;
        for (int x = 0; x < W; x++) { fg[3 * x] = src[4 * x + 2]; fg[3 * x + 1] = src[4 * x + 1]; fg[3 * x + 2] = src[4 * x]; }
        if (matte) {
            vrow(s->ga, gw, gh, fy, s->rowa, 1);
            vrow(s->gb, gw, gh, fy, s->rowb, 1);
            const float *restrict ra = s->rowa, *restrict rb = s->rowb;
            for (int x = 0; x < W; x++) {
                int x0 = xi[x];
                float l = xl[x];
                float a = ra[x0] + (ra[x0 + 1] - ra[x0]) * l, b = rb[x0] + (rb[x0 + 1] - rb[x0]) * l;
                float I = (0.299f * fg[3 * x] + 0.587f * fg[3 * x + 1] + 0.114f * fg[3 * x + 2]) * (1.0f / 255);
                float v = a * I + b;
                al[x] = v < 0 ? 0 : v > 1 ? 1 : v;
            }
            if (key) {
                vrow(s->dil, gw, gh, fy, s->rowd, 1);
                const float *restrict rd = s->rowd;
                /* st_chroma_alpha and st_despill, inlined so the loop vectorises */
                const float t0 = 0.04f + s->cfg.similarity * 0.003f, t1 = t0 + 0.01f + s->cfg.smoothness * 0.0025f, inv = 1.0f / (t1 - t0);
                const float kb = s->key[0], kr = s->key[1];
                const int gi = key == ST_KEY_GREEN ? 1 : 2, o2 = key == ST_KEY_GREEN ? 2 : 1;
                for (int x = 0; x < W; x++) {
                    float *c3 = fg + 3 * x;
                    float d = rd[xi[x]] + (rd[xi[x] + 1] - rd[xi[x]]) * xl[x];
                    float cb = 128 - 0.168736f * c3[0] - 0.331264f * c3[1] + 0.5f * c3[2] - kb;
                    float cr = 128 + 0.5f * c3[0] - 0.418688f * c3[1] - 0.081312f * c3[2] - kr;
                    float t = clampf((sqrtf(cb * cb + cr * cr) * (1.0f / 255) - t0) * inv, 0, 1);
                    float c = t * t * (3 - 2 * t);
                    float u = 1 - fabsf(2 * c - 1);                       /* where the chroma is unsure, the matte decides */
                    float dd = clampf((d - 0.05f) / 0.35f, 0, 1);
                    float k = dd * (c + (al[x] - c) * u);
                    float core = clampf((al[x] - 0.9f) * 10, 0, 1);        /* a wrinkle in the screen never punches a hole */
                    al[x] = k > core ? k : core;
                    float lim = (c3[0] + c3[o2]) * 0.5f, g = c3[gi];     /* spill: the key's channel down to the others' mean */
                    if (g > lim) c3[gi] = g - (g - lim) * spill;
                }
            }
            if (match) {
                const float *M = s->M, *o = s->o;
                for (int x = 0; x < W; x++) {
                    float r = fg[3 * x], g = fg[3 * x + 1], b = fg[3 * x + 2];
                    fg[3 * x] = clampf(M[0] * r + M[1] * g + M[2] * b + o[0], 0, 255);
                    fg[3 * x + 1] = clampf(M[3] * r + M[4] * g + M[5] * b + o[1], 0, 255);
                    fg[3 * x + 2] = clampf(M[6] * r + M[7] * g + M[8] * b + o[2], 0, 255);
                }
            }
            if (light) {
                vrow(s->glight, gw, gh, fy, s->rowd, 1);
                const float *restrict rl = s->rowd;
                for (int x = 0; x < W; x++) {
                    float g = rl[xi[x]] + (rl[xi[x] + 1] - rl[xi[x]]) * xl[x];
                    for (int k = 0; k < 3; k++) fg[3 * x + k] = lift(fg[3 * x + k], g);
                }
            }
            if (mode == ST_MODE_BLUR) {
                vrow(s->blur, gw, gh, fy, s->rowbl, 4);
                const float *restrict q = s->rowbl;
                for (int x = 0; x < W; x++) {
                    int xb = xi[x] * 4;
                    float l = xl[x];
                    float wsum = q[xb + 3] + (q[xb + 7] - q[xb + 3]) * l;
                    float inv = wsum > 1e-3f ? 1.0f / wsum : 0;
                    for (int k = 0; k < 3; k++) {
                        float c = (q[xb + 2 - k] + (q[xb + 6 - k] - q[xb + 2 - k]) * l) * inv;
                        bg[3 * x + k] = wsum > 1e-3f ? c : src[4 * x + 2 - k];
                    }
                }
            } else if (mode == ST_MODE_NONE) {
                for (int x = 0; x < W; x++) for (int k = 0; k < 3; k++) bg[3 * x + k] = src[4 * x + 2 - k];
            } else {
                const uint8_t *pic = picfull ? picfull + (size_t)y * W * 4 : NULL;
                for (int x = 0; x < W; x++) for (int k = 0; k < 3; k++) bg[3 * x + k] = pic ? pic[4 * x + 2 - k] : 0;
            }
            for (int x = 0; x < W; x++) {
                float a = al[x];
                for (int k = 0; k < 3; k++) { float b = bg[3 * x + k] * gbg; fg[3 * x + k] = b + (fg[3 * x + k] - b) * a; }
            }
        }
        if (lut) {
            for (int x = 0; x < W; x++) {
                float r = fg[3 * x], g = fg[3 * x + 1], b = fg[3 * x + 2];
                for (int k = 0; k < 3; k++) {
                    float v = (lk->m[3 * k] * r + lk->m[3 * k + 1] * g + lk->m[3 * k + 2] * b) * cn;
                    int i = (int)(clampf(v, 0, ST_CURVE_N - 1) + 0.5f);
                    float t = lk->curve[k][i];
                    float c = k == 0 ? r : k == 1 ? g : b;
                    fg[3 * x + k] = c + (t - c) * amt;
                }
            }
        }
        for (int x = 0; x < W; x++) {
            uint8_t *o = dst + (size_t)(mirror ? W - 1 - x : x) * 4;
            for (int k = 0; k < 3; k++) { float v = fg[3 * x + 2 - k]; o[k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f); }
            o[3] = 255;
        }
    }
}
/* BlazeFace on the frame letterboxed into 128×128 (MediaPipe's input: RGB in −1..1, black bars); the faces back in
 * the frame's pixels. The best face (the largest of those found) is kept with its eyes' height. */
static void detect_face(StFx *s, const uint8_t *in, int istride, int W, int H) {
    double t0 = now_ms();
    int h, w, c;
    float *x = lnn_input(s->face_nn, &h, &w, &c);
    float sc = (float)FS / (W > H ? W : H);
    int cw = (int)(W * sc + 0.5f), ch = (int)(H * sc + 0.5f);
    if (cw < 8 || ch < 8 || cw > FS || ch > FS) return;
    int ox = (FS - cw) / 2, oy = (FS - ch) / 2;
    for (int i = 0; i < FS * FS * 3; i++) x[i] = -1.0f;
    st_downscale_rgb(in, istride, W, H, s->face_in, cw, ch);
    for (int yy = 0; yy < ch; yy++) for (int xx = 0; xx < cw; xx++) {
        const float *q = s->face_in + ((size_t)yy * cw + xx) * 3;
        float *o = x + ((size_t)(yy + oy) * FS + xx + ox) * 3;
        o[0] = q[0] * 2 - 1; o[1] = q[1] * 2 - 1; o[2] = q[2] * 2 - 1;
    }
    if (lnn_run(s->face_nn)) return;
    size_t n0, n1;
    const float *reg = lnn_output(s->face_nn, 0, &n0), *sco = lnn_output(s->face_nn, 1, &n1);
    if (!reg || !sco || n0 != FACE_ANCHORS * 16 || n1 != FACE_ANCHORS) return;
    Face f[FACE_MAX];
    int nf = face_decode(reg, sco, 0.6f, f, FACE_MAX);
    s->st.face_ms = now_ms() - t0;
    int best = -1;
    for (int i = 0; i < nf; i++) if (best < 0 || f[i].w * f[i].h > f[best].w * f[best].h) best = i;
    if (best < 0) return;
    const Face *b = &f[best];
    float inv = 1.0f / sc;
    s->face[0] = (b->x * FS - ox) * inv;
    s->face[1] = (b->y * FS - oy) * inv;
    s->face[2] = b->w * FS * inv;
    s->face[3] = b->h * FS * inv;
    s->eyes_y = ((b->kp[0][1] + b->kp[1][1]) * 0.5f * FS - oy) * inv;
    s->have_face = 1;
    s->face_at = s->now;
}

/* Center Stage: the crop moves toward the target smoothly (1€ filter), not at all within the dead zone (4 %), never
 * faster than about half the frame a second; with no face for 2 s it eases back to the whole frame */
static void update_crop(StFx *s) {
    const int W = s->W, H = s->H;
    float dt = s->last_t > 0 ? (float)((s->now - s->last_t) / 1000.0) : 1.0f / 30;
    if (!(dt > 0) || dt > 0.5f) dt = 1.0f / 30;
    s->last_t = s->now;
    StRect t = { 0, 0, (float)W, (float)H };
    if (s->have_face && s->now - s->face_at < FACE_KEEP_MS) st_frame_target(s->face, s->eyes_y, W, H, s->cfg.framing, &t);
    float g[3] = { t.x + t.w / 2, t.y + t.h / 2, t.h };
    if (s->goal[2] <= 0 || fabsf(g[0] - s->goal[0]) > 0.04f * W || fabsf(g[1] - s->goal[1]) > 0.04f * H || fabsf(g[2] - s->goal[2]) > 0.04f * H)
        memcpy(s->goal, g, sizeof g);
    StEuro *e[3] = { &s->ex, &s->ey, &s->eh };
    float cur[3] = { s->crop.x + s->crop.w / 2, s->crop.y + s->crop.h / 2, s->crop.h };
    float v[3];
    for (int i = 0; i < 3; i++) {
        e[i]->mincut = 0.4f;
        e[i]->beta = 0.004f;
        e[i]->dcut = 1.0f;
        if (!e[i]->primed) { e[i]->x = cur[i]; e[i]->dx = 0; e[i]->primed = 1; }
        v[i] = st_euro(e[i], s->goal[i], dt);
        float maxd = 0.5f * (i == 1 ? H : W) * dt;      /* half the frame a second at most */
        if (v[i] - cur[i] > maxd) v[i] = cur[i] + maxd;
        if (cur[i] - v[i] > maxd) v[i] = cur[i] - maxd;
        e[i]->x = v[i];
    }
    float ch = clampf(v[2], H * 0.5f, (float)H), cw = ch * W / H;
    s->crop.w = cw;
    s->crop.h = ch;
    s->crop.x = clampf(v[0] - cw / 2, 0, W - cw);
    s->crop.y = clampf(v[1] - ch / 2, 0, H - ch);
}

/* the colour match's statistics: the person (the matte's weight) and the background picture (its visible part),
 * on the guide cells; the transform is smoothed over about a second so a video background doesn't make it flicker */
static void update_match(StFx *s) {
    const uint8_t *pic = picture_at_size(s);
    if (!pic) return;
    size_t gn = (size_t)s->gw * s->gh;
    if (s->pic_stats_dirty || s->cfg.mode == ST_MODE_VIDEO) {
        st_downscale_guide(pic, s->W * 4, s->W, s->H, s->bgg, s->bgy, s->gw, s->gh, s->rowsum);
        s->pic_stats_dirty = 0;
    }
    StStat p = { 0 }, b = { 0 };
    for (size_t i = 0; i < gn; i += 2) {
        float a = s->galpha[i];
        const float *c = s->grgb + i * 3, *q = s->bgg + i * 3;
        st_stat_add(&p, c, a);                          /* guide cells are RGB 0..255 */
        st_stat_add(&b, q, 1 - a);
    }
    float M[9], o[3];
    if (st_match_transform(&p, &b, s->cfg.match / 100.0f, M, o)) return;
    float k = s->match_on ? 0.06f : 1.0f;              /* about 1 s at 30 fps */
    for (int i = 0; i < 9; i++) s->M[i] += (M[i] - s->M[i]) * k;
    for (int i = 0; i < 3; i++) s->o[i] += (o[i] - s->o[i]) * k;
    s->match_on = 1;
}

int stfx_frame(StFx *s, const uint8_t *in, int istride, int W, int H, uint8_t *out, int ostride) {
    if (!s || !in || !out || W < 64 || H < 64 || W > ST_MAX_W || H > ST_MAX_H || (W & 1) || (H & 1) || istride < W * 4 || ostride < W * 4)
        return -1;
    if (resize_to(s, W, H)) return -1;
    double t0 = now_ms();
    s->now = t0;
    s->st.frames++;
    /* 0. faces, Center Stage and Retouch */
    const int want_face = s->face_nn && (s->cfg.framing || s->cfg.light || s->cfg.retouch);
    if (want_face && (s->frame_no % s->face_every == 0)) detect_face(s, in, istride, W, H);
    s->frame_no++;
    if (s->have_face && s->now - s->face_at >= FACE_KEEP_MS) s->have_face = 0;
    const uint8_t *src = in;
    int sstride = istride;
    if (s->cfg.framing && s->face_nn) {
        update_crop(s);
        if (s->crop.w < W - 0.5f || s->crop.x > 0.25f || s->crop.y > 0.25f) {
            st_crop_scale(in, istride, W, H, &s->crop, s->work, W * 4);
            src = s->work;
            sstride = W * 4;
        }
    } else s->crop = (StRect){ 0, 0, (float)W, (float)H };
    /* the face in this frame's pixels (after the crop) */
    float face[4], *fp = NULL;
    if (s->have_face) {
        float kx = W / s->crop.w, ky = H / s->crop.h;
        face[0] = (s->face[0] - s->crop.x) * kx;
        face[1] = (s->face[1] - s->crop.y) * ky;
        face[2] = s->face[2] * kx;
        face[3] = s->face[3] * ky;
        fp = face;
    }
    if (s->cfg.retouch && fp) {
        if (src == in) {
            for (int y = 0; y < H; y++) memcpy(s->work + (size_t)y * W * 4, in + (size_t)y * istride, (size_t)W * 4);
            src = s->work;
            sstride = W * 4;
        }
        st_retouch(s->work, W * 4, W, H, fp, s->cfg.retouch, &s->rt_tmp, &s->rt_cap);
    }
    s->st.frame_ms = now_ms() - t0;
    double t0s = now_ms();
    const int mode = s->cfg.mode;
    const int matte = mode != ST_MODE_NONE || s->cfg.light > 0;
    const int pictured = mode == ST_MODE_PICTURE || mode == ST_MODE_VIDEO;
    if (matte) {
        /* 1. the network on a 256×144 copy */
        int h, w, c;
        float *x = lnn_input(s->nn, &h, &w, &c);
        st_downscale_guide(src, sstride, W, H, s->grgb, s->gI, s->gw, s->gh, s->rowsum);
        st_resize_rgb(s->grgb, s->gw, s->gh, x, SW, SH, s->rtmp);
        for (int i = 0; i < SW * SH; i++) s->seg_luma[i] = 0.299f * x[3 * i] + 0.587f * x[3 * i + 1] + 0.114f * x[3 * i + 2];
        if (lnn_run(s->nn)) return -1;
        size_t n;
        const float *m = lnn_output(s->nn, 0, &n);
        if (!m || n != SW * SH) return -1;
        double t1 = now_ms();
        s->st.seg_ms = t1 - t0s;
        /* 2. over time; 3. the guided filter at a quarter of the frame */
        st_temporal(s->state, m, s->seg_luma, s->prev_luma, SW, SH, s->first);
        s->first = 0;
        st_resize_plane(s->state, SW, SH, s->gp, s->gw, s->gh);
        st_guided_coeffs(s->gI, s->gp, s->gw, s->gh, GF_R, GF_EPS, s->ga, s->gb, s->gtmp);
        size_t gn = (size_t)s->gw * s->gh;
        for (size_t i = 0; i < gn; i++) { float al = s->ga[i] * s->gI[i] + s->gb[i]; s->galpha[i] = al < 0 ? 0 : al > 1 ? 1 : al; }
        if (mode == ST_MODE_BLUR) {
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
        if (pictured && s->pic_dirty) fit_picture(s);
        if (pictured && s->cfg.key) {
            /* the green screen: its colour from the border every second or so; the person's mask dilated */
            if (s->key_age-- <= 0) {
                float k[2];
                st_chroma_key(src, sstride, W, H, s->cfg.key, k);
                if (s->key[0] == 0 && s->key[1] == 0) { s->key[0] = k[0]; s->key[1] = k[1]; }
                else { s->key[0] += (k[0] - s->key[0]) * 0.5f; s->key[1] += (k[1] - s->key[1]) * 0.5f; }
                s->key_age = 30;
            }
            memcpy(s->dil, s->galpha, gn * sizeof(float));
            st_dilate(s->dil, s->gw, s->gh, KEY_DILATE, s->diltmp);
        }
        if (pictured && s->cfg.match) update_match(s);
        if (s->cfg.light) {
            /* Studio Light's gain on the guide's cells (the CPU interpolates it like the matte; the GPU computes it
             * per pixel: it is smooth, the two agree within a level) */
            float kx = (float)W / s->gw, ky = (float)H / s->gh;
            for (int gy = 0; gy < s->gh; gy++) for (int gx = 0; gx < s->gw; gx++)
                s->glight[(size_t)gy * s->gw + gx] = st_light_person((gx + 0.5f) * kx, (gy + 0.5f) * ky, fp, s->cfg.light);
        }
        s->st.refine_ms = now_ms() - t1;
    } else {
        s->st.seg_ms = s->st.refine_ms = 0;
        s->first = 1;
    }
    /* 4. the composite */
    double t2 = now_ms();
    const int looks = (pictured && (s->cfg.key || s->match_on)) || s->cfg.light || (s->cfg.filter && s->cfg.filter_amt);
    int done = 0;
    if (s->gl && !s->force_cpu && !s->gl_failed) {
        StGlJob j = { .frame = src, .stride = sstride, .w = W, .h = H, .a = s->ga, .b = s->gb, .gw = s->gw, .gh = s->gh,
                      .mode = mode, .blur = s->blur, .bw = s->gw, .bh = s->gh, .pic = s->pic, .pw = s->pw, .ph = s->ph,
                      .pstride = s->pw * 4, .pic_changed = s->pic_gpu_dirty, .mirror = s->cfg.mirror, .out = out, .ostride = ostride,
                      .matte = matte, .key = pictured ? s->cfg.key : 0, .dil = s->dil, .similarity = s->cfg.similarity,
                      .smoothness = s->cfg.smoothness, .spill = s->cfg.spill / 100.0f, .match = pictured && s->match_on,
                      .light = s->cfg.light, .lut = s->cfg.filter && s->lut_for == s->cfg.filter ? s->lut : NULL,
                      .lut_changed = s->lut_gpu_dirty, .lut_amt = s->cfg.filter_amt / 100.0f };
        memcpy(j.keycbcr, s->key, sizeof j.keycbcr);
        memcpy(j.m, s->M, sizeof j.m);
        memcpy(j.o, s->o, sizeof j.o);
        if (fp) { memcpy(j.face, fp, sizeof j.face); j.has_face = 1; }
        if (stgl_compose(s->gl, &j) == 0) {
            done = 1;
            if (pictured && s->pic) s->pic_gpu_dirty = 0;     /* uploaded only in those modes */
            if (j.lut) s->lut_gpu_dirty = 0;
        }
        else { s->gl_failed = 1; fprintf(stderr, "lord-studio: the GPU composite failed; the CPU does it from now on\n"); }
    }
    if (!done) {
        if (looks) compose_cpu_fx(s, src, sstride, out, ostride);
        else compose_cpu(s, src, sstride, out, ostride);
    }
    s->st.gpu = done;
    s->st.compose_ms = now_ms() - t2;
    return 0;
}
