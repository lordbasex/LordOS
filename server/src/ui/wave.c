/* wave.c — the sound wave widget's lines (see wave.h).
 *
 * Each line is a sum of three sines whose height follows the spectrum along the width: the bass in the
 * middle, the treble towards the ends, which fade out. The lines differ in phase and colour (the three
 * colours of the look, first to last), are added up as light, and a glow (the same lines blurred at half
 * size, twice) goes around them. The result is premultiplied with alpha = its brightest channel, so over
 * a dark wallpaper it reads as added light and over a card as lines with a halo.
 *
 * Cost: the raster is built only when the bands change (30 a second at most, never in silence) and at the
 * node's size; any other repaint of that area blits the kept raster. */
#include "wave.h"
#include "ui.h"
#include "../sound/spectrum.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WAVE_MAX_W 4096
#define WAVE_MAX_H 1600

WaveConf *wave_conf_new(void) {
    WaveConf *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->lines = 16;
    w->col[0] = 0x408cff;           /* blue */
    w->col[1] = 0xa05aff;           /* violet */
    w->col[2] = 0xff3cf0;           /* magenta */
    w->gain = 1;
    return w;
}

static void drop_raster(WaveConf *w) {
    free(w->px); free(w->acc); free(w->half); free(w->g1); free(w->g2); free(w->tmp);
    w->px = NULL; w->acc = w->half = w->g1 = w->g2 = w->tmp = NULL;
    w->pw = w->ph = 0;
    w->built = 0;
}

void wave_conf_free(WaveConf *w) {
    if (!w) return;
    drop_raster(w);
    free(w);
}

void wave_conf_changed(WaveConf *w) { if (w) w->built = 0; }

/* Box blur of a 3-channel float image, radius r: horizontal running sums along each row, then vertical ones kept per
 * column while walking down the rows (both in memory order). tmp holds the horizontal pass; col is 3·w floats. */
static void box3(float *img, float *tmp, float *col, int w, int h, int r) {
    if (r < 1) return;
    const float inv = 1.0f / (2 * r + 1);
    const int w3 = w * 3;
    for (int y = 0; y < h; y++) {
        const float *row = img + (size_t)y * w3;
        float *out = tmp + (size_t)y * w3;
        float s0 = 0, s1 = 0, s2 = 0;
        for (int i = -r; i <= r; i++) { int xi = i < 0 ? 0 : i >= w ? w - 1 : i; s0 += row[xi * 3]; s1 += row[xi * 3 + 1]; s2 += row[xi * 3 + 2]; }
        for (int x = 0; x < w; x++) {
            out[x * 3] = s0 * inv; out[x * 3 + 1] = s1 * inv; out[x * 3 + 2] = s2 * inv;
            int xa = (x - r < 0 ? 0 : x - r) * 3, xb = (x + r + 1 >= w ? w - 1 : x + r + 1) * 3;
            s0 += row[xb] - row[xa]; s1 += row[xb + 1] - row[xa + 1]; s2 += row[xb + 2] - row[xa + 2];
        }
    }
    for (int i = 0; i < w3; i++) col[i] = 0;
    for (int k = -r; k <= r; k++) {
        const float *src = tmp + (size_t)(k < 0 ? 0 : k >= h ? h - 1 : k) * w3;
        for (int i = 0; i < w3; i++) col[i] += src[i];
    }
    for (int y = 0; y < h; y++) {
        float *out = img + (size_t)y * w3;
        for (int i = 0; i < w3; i++) out[i] = col[i] * inv;
        const float *add = tmp + (size_t)(y + r + 1 >= h ? h - 1 : y + r + 1) * w3, *sub = tmp + (size_t)(y - r < 0 ? 0 : y - r) * w3;
        for (int i = 0; i < w3; i++) col[i] += add[i] - sub[i];
    }
}

static float smooth01(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t); }

/* The height of the bands at u (0 = the middle, 1 = an end): bass in the middle. */
static float env_at(const Spectrum *sp, float u) {
    const int top = SPEC_BANDS * 5 / 6;             /* above ~8 kHz there is little to see */
    float p = u * (top - 1);
    int i = (int)p;
    if (i >= top - 1) return sp->bands[top - 1];
    float f = p - i, a = sp->bands[i], b = sp->bands[i + 1];
    f = (1 - cosf(f * (float)M_PI)) * 0.5f;
    return a + (b - a) * f;
}

void wave_render(WaveConf *w, const Spectrum *sp, int pw, int ph, float k) {
    if (pw < 2 || ph < 2) return;
    if (pw > WAVE_MAX_W) pw = WAVE_MAX_W;
    if (ph > WAVE_MAX_H) ph = WAVE_MAX_H;
    if (pw != w->pw || ph != w->ph || !w->px) {
        drop_raster(w);
        size_t n = (size_t)pw * ph, hn = (size_t)(pw / 2 + 1) * (ph / 2 + 1);
        w->px = malloc(n * 4);
        w->acc = malloc(n * 3 * sizeof(float));
        w->half = malloc(hn * 3 * sizeof(float));
        w->g1 = malloc(hn * 3 * sizeof(float));
        w->g2 = malloc(hn * 3 * sizeof(float));
        w->tmp = malloc(hn * 3 * sizeof(float));
        if (!w->px || !w->acc || !w->half || !w->g1 || !w->g2 || !w->tmp) { drop_raster(w); return; }
        w->pw = pw;
        w->ph = ph;
    }
    if (k < 0.5f) k = 0.5f;
    const int lines = w->lines < 2 ? 2 : w->lines > 24 ? 24 : w->lines;

    /* the envelope along the width; ys holds every line's heights (lines × (pw + 1)) */
    float *env = malloc((size_t)(pw + 1) * sizeof(float));
    float *ys = malloc((size_t)(pw + 1) * lines * sizeof(float));
    if (!env || !ys) { free(env); free(ys); return; }
    float gain = w->gain > 0 ? w->gain : 1;
    float edge = pw * 0.10f;
    for (int x = 0; x <= pw; x++) {
        float u = fabsf((float)x / pw - 0.5f) * 2;
        float e = env_at(sp, u) * gain;
        env[x] = e > 1.2f ? 1.2f : e;
    }
    /* smoothed along the width (two running means of ~5 % of it): swells, not spikes */
    int er = pw / 40 > 1 ? pw / 40 : 1;
    for (int pass = 0; pass < 2; pass++) {
        float sum = 0;
        for (int i = -er; i <= er; i++) sum += env[i < 0 ? 0 : i > pw ? pw : i];
        for (int x = 0; x <= pw; x++) {
            ys[x] = sum / (2 * er + 1);
            int xa = x - er < 0 ? 0 : x - er, xb = x + er + 1 > pw ? pw : x + er + 1;
            sum += env[xb] - env[xa];
        }
        memcpy(env, ys, (size_t)(pw + 1) * sizeof(float));
    }
    for (int x = 0; x <= pw; x++) {
        float d = x < pw - x ? (float)x : (float)(pw - x);
        env[x] *= smooth01(d / edge);
    }

    const float cy = ph * 0.5f, amp = ph * 0.42f, th = 1.1f * k;
    const float a1 = 7.96f / ph, a2 = 14.2f / ph, a3 = 3.1f / ph;
    const float t = (float)(sp->seq % 100000u) * 0.06f;
    float ymin = cy, ymax = cy;
    for (int i = 0; i < lines; i++) {
        float f = (float)i / (lines - 1);
        float ph0 = f * 2.3f + t, shape = 0.55f + 0.45f * sinf(f * 3.1f);
        float *yl = ys + (size_t)i * (pw + 1);
        for (int x = 0; x <= pw; x++) {
            float s = sinf(x * a1 * (1 + 0.13f * f) + ph0 * 0.4f) * 0.55f + sinf(x * a2 * (1 - 0.12f * f) - ph0 * 1.7f) * 0.30f
                    + sinf(x * a3 + ph0 * 0.6f) * 0.35f;
            yl[x] = cy + s * env[x] * amp * shape;
            if (yl[x] < ymin) ymin = yl[x];
            if (yl[x] > ymax) ymax = yl[x];
        }
    }
    /* only the rows the lines and their glow can reach are drawn (in quiet passages a thin band): the glow reaches
     * 2 × (2 r1 + 2 r2) full-size pixels past a line */
    const int r1 = (int)(1.5f * k + 0.5f), r2 = (int)(4.0f * k + 0.5f), reach = 4 * (r1 + r2) + 4;
    int b0 = (int)floorf(ymin - th) - reach, b1 = (int)ceilf(ymax + th) + reach;
    if (b0 < 0) b0 = 0;
    if (b1 > ph) b1 = ph;
    memset(w->acc + (size_t)b0 * pw * 3, 0, (size_t)(b1 - b0) * pw * 3 * sizeof(float));
    for (int i = 0; i < lines; i++) {
        float f = (float)i / (lines - 1);
        const float *yl = ys + (size_t)i * (pw + 1);
        /* colour: first → middle → last */
        uint32_t ca = f < 0.5f ? w->col[0] : w->col[1], cb = f < 0.5f ? w->col[1] : w->col[2];
        float g = f < 0.5f ? f * 2 : (f - 0.5f) * 2;
        float cr = (((ca >> 16) & 255) + ((((cb >> 16) & 255) - (float)((ca >> 16) & 255)) * g)) / 255.0f;
        float cg = (((ca >> 8) & 255) + ((((cb >> 8) & 255) - (float)((ca >> 8) & 255)) * g)) / 255.0f;
        float cbl = ((ca & 255) + (((cb & 255) - (float)(ca & 255)) * g)) / 255.0f;
        const float lw = 6.0f / lines;
        cr *= lw; cg *= lw; cbl *= lw;
        for (int x = 0; x < pw; x++) {
            float y0 = yl[x], y1 = yl[x + 1];
            float lo = (y0 < y1 ? y0 : y1) - th * 0.5f, hi = (y0 < y1 ? y1 : y0) + th * 0.5f;
            float wgt = th * 1.2f / (hi - lo);
            if (wgt > 1) wgt = 1;
            if (wgt < 0.35f) wgt = 0.35f;
            int r0 = (int)floorf(lo), r1 = (int)ceilf(hi);
            if (r0 < 0) r0 = 0;
            if (r1 > ph) r1 = ph;
            for (int r = r0; r < r1; r++) {
                float a = (r + 1 < hi ? r + 1 : hi) - (r > lo ? r : lo);
                if (a <= 0) continue;
                a *= wgt;
                float *p = w->acc + ((size_t)r * pw + x) * 3;
                p[0] += cr * a; p[1] += cg * a; p[2] += cbl * a;
            }
        }
    }
    free(env);
    free(ys);

    /* the glow: half size, two blurs, over the band's rows */
    int hw = pw / 2 > 0 ? pw / 2 : 1, hh = ph / 2 > 0 ? ph / 2 : 1;
    int hy0 = b0 / 2, hy1 = (b1 + 1) / 2;
    if (hy1 > hh) hy1 = hh;
    if (hy0 >= hy1) hy0 = hy1 > 0 ? hy1 - 1 : 0;
    const int hb = hy1 - hy0 > 0 ? hy1 - hy0 : 1;
    for (int y = hy0; y < hy0 + hb; y++)
        for (int x = 0; x < hw; x++)
            for (int ch = 0; ch < 3; ch++) {
                int x0 = x * 2, y0 = y * 2, x1 = x0 + 1 < pw ? x0 + 1 : x0, y1 = y0 + 1 < ph ? y0 + 1 : y0;
                w->half[((size_t)y * hw + x) * 3 + ch] = 0.25f * (w->acc[((size_t)y0 * pw + x0) * 3 + ch] + w->acc[((size_t)y0 * pw + x1) * 3 + ch]
                                                              + w->acc[((size_t)y1 * pw + x0) * 3 + ch] + w->acc[((size_t)y1 * pw + x1) * 3 + ch]);
            }
    const size_t off = (size_t)hy0 * hw * 3, hn3 = (size_t)hw * hb * 3;
    float *g1 = w->g1 + off, *g2 = w->g2 + off;
    memcpy(g1, w->half + off, hn3 * sizeof(float));
    float *col = malloc((size_t)hw * 3 * sizeof(float));
    if (!col) return;
    box3(g1, w->tmp, col, hw, hb, r1);
    box3(g1, w->tmp, col, hw, hb, r1);
    memcpy(g2, w->half + off, hn3 * sizeof(float));
    box3(g2, w->tmp, col, hw, hb, r2);
    box3(g2, w->tmp, col, hw, hb, r2);
    free(col);
    /* the glow together, then the sum of the three at full size: up-sampled per row and column from tables */
    for (size_t i = 0; i < hn3; i++) g1[i] = g1[i] * 1.4f + g2[i] * 2.0f;

    /* the ends fade out, so without a card it melts into the wallpaper; 1 - e^(-1.6 v) softens the highlights so
     * overlapping lines don't flatten to white (a table: the exponent per pixel was most of the cost) */
    static float tone[1025];
    if (tone[1024] == 0) for (int i = 0; i <= 1024; i++) tone[i] = 1 - expf(-1.6f * (i / 1024.0f * 6));
    int *xa = malloc((size_t)pw * 2 * sizeof(int));
    float *tx = malloc((size_t)pw * 2 * sizeof(float));
    if (!xa || !tx) { free(xa); free(tx); return; }
    float fade_w = pw * 0.07f;
    for (int x = 0; x < pw; x++) {
        float fx = (x + 0.5f) * 0.5f - 0.5f;
        int x0 = (int)floorf(fx);
        tx[x] = fx - x0;
        xa[x] = (x0 < 0 ? 0 : x0 >= hw ? hw - 1 : x0) * 3;
        xa[pw + x] = (x0 + 1 < 0 ? 0 : x0 + 1 >= hw ? hw - 1 : x0 + 1) * 3;
        float dx = x < pw - 1 - x ? (float)x : (float)(pw - 1 - x);
        tx[pw + x] = smooth01(dx / fade_w) * (1024 / 6.0f);
    }
    const int hl = hy0 + hb - 1;
    for (int y = 0; y < ph; y++) {
        if (y < b0 || y >= b1) { memset(w->px + (size_t)y * pw, 0, (size_t)pw * 4); continue; }
        float fy = (y + 0.5f) * 0.5f - 0.5f;
        int y0 = (int)floorf(fy);
        float ty = fy - y0;
        const float *ra = w->g1 + (size_t)(y0 < hy0 ? hy0 : y0 > hl ? hl : y0) * hw * 3;
        const float *rb = w->g1 + (size_t)(y0 + 1 < hy0 ? hy0 : y0 + 1 > hl ? hl : y0 + 1) * hw * 3;
        float *gr = w->tmp;                         /* this row of the glow, between the two half-size rows */
        for (int i = 0; i < hw * 3; i++) gr[i] = ra[i] + (rb[i] - ra[i]) * ty;
        const float *acc = w->acc + (size_t)y * pw * 3;
        uint32_t *out = w->px + (size_t)y * pw;
        for (int x = 0; x < pw; x++) {
            const int a0 = xa[x], a1 = xa[pw + x];
            const float u = tx[x], k = tx[pw + x];
            uint32_t c[3];
            for (int ch = 0; ch < 3; ch++) {
                float v = (acc[x * 3 + ch] + gr[a0 + ch] + (gr[a1 + ch] - gr[a0 + ch]) * u) * k;
                int i = v >= 1024 ? 1024 : v <= 0 ? 0 : (int)v;
                c[ch] = (uint32_t)(tone[i] * 255 + 0.5f);
            }
            uint32_t A = c[0] > c[1] ? c[0] : c[1];
            if (c[2] > A) A = c[2];
            out[x] = A << 24 | c[0] << 16 | c[1] << 8 | c[2];
        }
    }
    free(xa);
    free(tx);
}

/* ---- the nodes on screen ---- */

static Spectrum silent;                     /* until main hands over the sound client's */
static const Spectrum *src = &silent;
void wave_source(const Spectrum *s) { src = s ? s : &silent; }

#define WAVE_NODES 8
static struct { int id; uint64_t serial; } shown[WAVE_NODES];
static uint32_t ticked_seq;

static void note_shown(const Node *n) {
    int free_slot = -1;
    for (int i = 0; i < WAVE_NODES; i++) {
        if (shown[i].id == n->id && shown[i].serial == n->serial) return;
        if (free_slot < 0 && (!shown[i].id || !node_get(shown[i].id) || node_get(shown[i].id)->serial != shown[i].serial)) free_slot = i;
    }
    if (free_slot < 0) free_slot = 0;
    shown[free_slot].id = n->id;
    shown[free_slot].serial = n->serial;
}

void wave_paint(Canvas *c, Node *n, float x, float y, float w, float h, float k, float op) {
    if (!n->wave || w < 2 || h < 2 || op <= 0) return;
    note_shown(n);
    const Spectrum *sp = src;
    int pw = (int)(w + 0.5f), ph = (int)(h + 0.5f);
    WaveConf *wc = n->wave;
    if (!wc->built || wc->pw != pw || wc->ph != ph || wc->seq != sp->seq) {
        wave_render(wc, sp, pw, ph, k);
        if (!wc->px) return;
        wc->seq = sp->seq;
        wc->built = 1;
    }
    canvas_blit(c, wc->px, wc->pw, wc->ph, (int)(x + 0.5f), (int)(y + 0.5f), 0, op, 0);
}

void wave_tick(void) {
    const Spectrum *sp = src;
    if (sp->seq == ticked_seq) return;
    ticked_seq = sp->seq;
    for (int i = 0; i < WAVE_NODES; i++) {
        if (!shown[i].id) continue;
        Node *n = node_get(shown[i].id);
        if (!n || n->serial != shown[i].serial || n->type != NODE_WAVE || !n->wave || n->prev_box.x1 <= n->prev_box.x0) {
            shown[i].id = 0;
            continue;
        }
        damage_add((Box){ floorf(n->ax), floorf(n->ay), ceilf(n->ax + n->w), ceilf(n->ay + n->h) });
        ui.dirty = 1;
    }
}
