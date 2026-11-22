/* nn.c — LordOS Studio's inference (see nn.h and docs/CAMERA-STUDIO-PLAN.md 4.2).
 *
 * Loading: the file is checked field by field (nn.h's limits), every operator's shapes are recomputed from its inputs
 * and must equal the shapes the file declares, weights are widened to float32 and repacked for the kernels, and the
 * activations get one arena laid out by lifetime (a tensor never shares bytes with one alive at the same time).
 * Running: plain C loops written so the compiler vectorises the inner one over channels; on x86-64 the hot kernels are
 * also built for x86-64-v3 (AVX2 + FMA) and picked at load time by the C library (target_clones), and on aarch64
 * NEON is part of the baseline. Results match ONNX Runtime within 1e-3 (build/studio-nn-test). */
#include "nn.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__) && defined(__has_attribute)
#if __has_attribute(target_clones)
#define NN_HOT __attribute__((target_clones("arch=x86-64-v3", "default")))
#endif
#endif
#ifndef NN_HOT
#define NN_HOT
#endif

typedef struct {
    int kind;
    int d[4];                    /* 1, h, w, c (activations) or the weight's own dims */
    size_t n;                    /* elements */
    float *p;                    /* activations: into the arena (set at load); constants: own copy */
    int def, last;               /* lifetime in op indices (-1 = the input) */
    size_t off;                  /* arena offset in floats */
} Tensor;

typedef struct {
    int op, in[3], out;
    int p[11];
    int pad_t, pad_l;            /* SAME padding before (computed) */
    float *w;                    /* repacked weights (conv: [kh][kw][I][O]; dw: [kh][kw][C]; tconv: [ky][kx][I][O]) */
    int kh, kw;
} Op;

struct Lnn {
    int nt, no, input, nout, out[LNN_MAX_OUTPUTS];
    Tensor *t;
    Op *o;
    float *arena;
    double macs;
};

float lnn_half(unsigned short h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, f = h & 1023, bits;
    if (e == 0) {
        if (!f) bits = s;
        else {                                   /* subnormal: normalise */
            int k = 0;
            while (!(f & 1024)) { f <<= 1; k++; }
            bits = s | (uint32_t)(113 - k) << 23 | (f & 1023) << 13;
        }
    } else if (e == 31) bits = s | 0x7f800000u | f << 13;
    else bits = s | (e + 112) << 23 | f << 13;
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

#define FAIL(...) do { if (err && errlen) snprintf(err, errlen, __VA_ARGS__); goto bad; } while (0)

void lnn_free(Lnn *m) {
    if (!m) return;
    if (m->t) for (int i = 0; i < m->nt; i++) if (m->t[i].kind == LNN_CONSTANT) free(m->t[i].p);
    if (m->o) for (int i = 0; i < m->no; i++) free(m->o[i].w);
    free(m->t);
    free(m->o);
    free(m->arena);
    free(m);
}

static int is_act(const Lnn *m, int i) { return i >= 0 && i < m->nt && m->t[i].kind == LNN_ACTIVATION; }
static int is_const(const Lnn *m, int i) { return i >= 0 && i < m->nt && m->t[i].kind == LNN_CONSTANT; }

/* SAME / VALID: the output size and the padding before, as TensorFlow defines them */
static int conv_out(int in, int k, int s, int mode, int *pad_before) {
    if (mode == LNN_PAD_VALID) { *pad_before = 0; return in >= k ? (in - k) / s + 1 : 0; }
    int out = (in + s - 1) / s;
    int total = (out - 1) * s + k - in;
    if (total < 0) total = 0;
    *pad_before = total / 2;
    return out;
}

static int same4(const int a[4], int b0, int b1, int b2, int b3) { return a[0] == b0 && a[1] == b1 && a[2] == b2 && a[3] == b3; }

/* Checks one operator against its tensors and prepares its weights. 0 ok. */
static int op_prepare(Lnn *m, Op *o, char *err, size_t errlen) {
    if (!is_act(m, o->in[0]) || !is_act(m, o->out) || o->in[0] == o->out) FAIL("op tensors");
    Tensor *x = &m->t[o->in[0]], *y = &m->t[o->out];
    const int H = x->d[1], W = x->d[2], C = x->d[3];
    switch (o->op) {
    case LNN_CONV: case LNN_DWCONV: case LNN_TCONV: {
        if (!is_const(m, o->in[1]) || !is_const(m, o->in[2]) || o->in[1] == o->in[2]) FAIL("conv weights");
        Tensor *w = &m->t[o->in[1]], *b = &m->t[o->in[2]];
        int sh = o->p[0], sw = o->p[1], mode = o->p[2], act = o->p[3];
        if (sh < 1 || sh > 2 || sw < 1 || sw > 2 || (mode != LNN_PAD_SAME && mode != LNN_PAD_VALID) || act < 0 || act > LNN_ACT_SIGMOID) FAIL("conv params");
        int kh = w->d[1], kw = w->d[2];
        if (kh < 1 || kh > 7 || kw < 1 || kw > 7) FAIL("kernel size");
        int O = o->op == LNN_DWCONV ? C : w->d[0];
        if (o->op == LNN_DWCONV ? (w->d[0] != 1 || w->d[3] != C) : (w->d[3] != C)) FAIL("conv channels");
        if (!same4(b->d, 1, 1, 1, O)) FAIL("bias");
        int oh, ow;
        if (o->op == LNN_TCONV) {
            if (kh != 2 || kw != 2 || sh != 2 || sw != 2 || mode != LNN_PAD_SAME) FAIL("tconv");
            oh = H * 2; ow = W * 2;
            o->pad_t = o->pad_l = 0;
        } else {
            oh = conv_out(H, kh, sh, mode, &o->pad_t);
            ow = conv_out(W, kw, sw, mode, &o->pad_l);
        }
        if (!same4(y->d, 1, oh, ow, O)) FAIL("conv output shape");
        o->kh = kh;
        o->kw = kw;
        size_t nw = (size_t)kh * kw * (o->op == LNN_DWCONV ? 1 : C) * O;
        o->w = malloc(nw * sizeof(float));
        if (!o->w) FAIL("memory");
        const float *src = w->p;
        if (o->op == LNN_DWCONV) memcpy(o->w, src, nw * sizeof(float));            /* already [1][kh][kw][C] */
        else for (int oc = 0; oc < O; oc++) for (int ky = 0; ky < kh; ky++) for (int kx = 0; kx < kw; kx++) for (int ic = 0; ic < C; ic++)
            o->w[(((size_t)ky * kw + kx) * C + ic) * O + oc] = src[(((size_t)oc * kh + ky) * kw + kx) * C + ic];
        m->macs += (double)oh * ow * kh * kw * (o->op == LNN_DWCONV ? 1 : C) * O / (o->op == LNN_TCONV ? 4 : 1);
        return 0;
    }
    case LNN_ADD: case LNN_MUL: {
        if (!is_act(m, o->in[1]) || o->in[2] != -1 || o->p[3] < 0 || o->p[3] > LNN_ACT_SIGMOID) FAIL("add/mul");
        Tensor *b = &m->t[o->in[1]];
        int bcast = o->op == LNN_MUL && same4(b->d, 1, 1, 1, C);
        if (!bcast && memcmp(b->d, x->d, sizeof b->d)) FAIL("add/mul shapes");
        if (memcmp(y->d, x->d, sizeof y->d)) FAIL("add/mul output");
        return 0;
    }
    case LNN_MEAN: if (!same4(y->d, 1, 1, 1, C)) FAIL("mean"); return 0;
    case LNN_RESIZE:
        if (y->d[3] != C || y->d[1] < 1 || y->d[2] < 1 || (o->p[0] && o->p[1]) || (unsigned)o->p[0] > 1 || (unsigned)o->p[1] > 1) FAIL("resize");
        return 0;
    case LNN_PAD:
        if (o->p[0] < 0 || o->p[1] < 0 || o->p[0] > LNN_MAX_DIM || o->p[1] > LNN_MAX_DIM || !same4(y->d, 1, H, W, C + o->p[0] + o->p[1])) FAIL("pad");
        return 0;
    case LNN_MAXPOOL: {
        int kh = o->p[4], kw = o->p[5], sh = o->p[0], sw = o->p[1], mode = o->p[2];
        if (kh < 1 || kh > 3 || kw < 1 || kw > 3 || sh < 1 || sh > 2 || sw < 1 || sw > 2 || (mode != LNN_PAD_SAME && mode != LNN_PAD_VALID)) FAIL("maxpool");
        int oh = conv_out(H, kh, sh, mode, &o->pad_t), ow = conv_out(W, kw, sw, mode, &o->pad_l);
        if (!same4(y->d, 1, oh, ow, C)) FAIL("maxpool output");
        o->kh = kh;
        o->kw = kw;
        return 0;
    }
    case LNN_RESHAPE: if (y->n != x->n) FAIL("reshape"); return 0;
    case LNN_CONCAT: {
        if (!is_act(m, o->in[1])) FAIL("concat");
        Tensor *b = &m->t[o->in[1]];
        if (y->n != x->n + b->n || y->d[3] != x->d[3] || b->d[3] != x->d[3] || x->d[1] != 1 || b->d[1] != 1 || y->d[1] != 1) FAIL("concat shapes");
        return 0;
    }
    case LNN_RELU: case LNN_HSWISH: case LNN_SIGMOID: if (memcmp(y->d, x->d, sizeof y->d)) FAIL("activation shape"); return 0;
    default: FAIL("unknown op %d", o->op);
    }
bad:
    return -1;
}

/* the arena: each activation gets the lowest offset that no tensor alive at the same time uses */
static int plan_arena(Lnn *m, char *err, size_t errlen) {
    int *order = malloc(sizeof(int) * (size_t)m->nt);
    if (!order) FAIL("memory");
    int n = 0;
    for (int i = 0; i < m->nt; i++) if (m->t[i].kind == LNN_ACTIVATION) order[n++] = i;
    /* by definition order (stable) */
    for (int a = 1; a < n; a++) { int v = order[a], b = a - 1; while (b >= 0 && m->t[order[b]].def > m->t[v].def) { order[b + 1] = order[b]; b--; } order[b + 1] = v; }
    size_t total = 0;
    for (int a = 0; a < n; a++) {
        Tensor *t = &m->t[order[a]];
        size_t need = (t->n + 15) & ~(size_t)15, off = 0;
        for (int moved = 1; moved;) {
            moved = 0;
            for (int b = 0; b < a; b++) {
                Tensor *u = &m->t[order[b]];
                if (u->last < t->def || t->last < u->def) continue;        /* never alive together */
                size_t un = (u->n + 15) & ~(size_t)15;
                if (off < u->off + un && u->off < off + need) { off = u->off + un; moved = 1; }
            }
        }
        t->off = off;
        if (off + need > total) total = off + need;
    }
    free(order);
    if (total * sizeof(float) > LNN_MAX_ARENA) FAIL("arena too large");
    m->arena = aligned_alloc(64, (total * sizeof(float) + 63) & ~(size_t)63);
    if (!m->arena) FAIL("memory");
    memset(m->arena, 0, total * sizeof(float));
    for (int i = 0; i < m->nt; i++) if (m->t[i].kind == LNN_ACTIVATION) m->t[i].p = m->arena + m->t[i].off;
    return 0;
bad:
    return -1;
}

Lnn *lnn_load(const void *data, size_t len, char *err, size_t errlen) {
    const uint8_t *b = data;
    Lnn *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    if (!b || len < LNN_HEAD_BYTES || memcmp(b, LNN_MAGIC, 4) || rd32(b + 4) != LNN_VERSION) FAIL("not an LNN1 file");
    uint32_t nt = rd32(b + 8), no = rd32(b + 12), halfs = rd32(b + 16), input = rd32(b + 20), nout = rd32(b + 24);
    if (nt < 2 || nt > LNN_MAX_TENSORS || no < 1 || no > LNN_MAX_OPS || halfs > LNN_MAX_HALFS || nout < 1 || nout > LNN_MAX_OUTPUTS) FAIL("counts");
    size_t data_off = LNN_HEAD_BYTES + (size_t)nt * LNN_TENSOR_BYTES + (size_t)no * LNN_OP_BYTES;
    if (len != data_off + (((size_t)halfs * 2 + 3) & ~(size_t)3)) FAIL("size");
    const uint8_t *hp = b + data_off;
    m->nt = (int)nt;
    m->no = (int)no;
    m->t = calloc(nt, sizeof *m->t);
    m->o = calloc(no, sizeof *m->o);
    if (!m->t || !m->o) FAIL("memory");
    /* tensors */
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *r = b + LNN_HEAD_BYTES + (size_t)i * LNN_TENSOR_BYTES;
        Tensor *t = &m->t[i];
        t->kind = (int)rd32(r);
        if (t->kind != LNN_ACTIVATION && t->kind != LNN_CONSTANT) FAIL("tensor kind");
        size_t n = 1;
        for (int k = 0; k < 4; k++) {
            uint32_t d = rd32(r + 4 + 4 * k);
            if (d < 1 || d > LNN_MAX_DIM) FAIL("dimension");
            t->d[k] = (int)d;
            n *= d;
            if (n > LNN_MAX_ELEMS) FAIL("tensor too large");
        }
        t->n = n;
        t->def = -2;
        t->last = -2;
        if (t->kind == LNN_ACTIVATION) { if (t->d[0] != 1) FAIL("batch"); continue; }
        uint32_t off = rd32(r + 20), count = rd32(r + 24);
        if (count != n || off > halfs || count > halfs - off) FAIL("constant range");
        t->p = malloc(n * sizeof(float));
        if (!t->p) FAIL("memory");
        for (size_t k = 0; k < n; k++) {
            const uint8_t *q = hp + 2 * ((size_t)off + k);
            float v = lnn_half((unsigned short)(q[0] | q[1] << 8));
            if (!isfinite(v)) FAIL("weight not finite");
            t->p[k] = v;
        }
    }
    if (!is_act(m, (int)input)) FAIL("input");
    m->input = (int)input;
    m->t[input].def = -1;
    m->t[input].last = -1;
    /* operators, in order: each reads tensors already written and writes one new tensor */
    for (uint32_t i = 0; i < no; i++) {
        const uint8_t *r = b + LNN_HEAD_BYTES + (size_t)nt * LNN_TENSOR_BYTES + (size_t)i * LNN_OP_BYTES;
        Op *o = &m->o[i];
        o->op = (int)rd32(r);
        if (o->op < LNN_CONV || o->op > LNN_OP_LAST) FAIL("op %d", o->op);
        for (int k = 0; k < 3; k++) {
            o->in[k] = (int32_t)rd32(r + 4 + 4 * k);
            if (o->in[k] < -1 || o->in[k] >= (int)nt) FAIL("op input");
        }
        o->out = (int32_t)rd32(r + 16);
        for (int k = 0; k < 11; k++) o->p[k] = (int32_t)rd32(r + 20 + 4 * k);
        for (int k = 0; k < 3; k++) {
            int ti = o->in[k];
            if (ti < 0 || m->t[ti].kind != LNN_ACTIVATION) continue;
            if (m->t[ti].def == -2) FAIL("op %u reads a tensor never written", i);
            m->t[ti].last = (int)i;
        }
        if (!is_act(m, o->out) || m->t[o->out].def != -2) FAIL("op %u output", i);
        m->t[o->out].def = (int)i;
        m->t[o->out].last = (int)i;
        if (op_prepare(m, o, err, errlen)) goto bad;
    }
    m->nout = (int)nout;
    for (uint32_t k = 0; k < nout; k++) {
        uint32_t oi = rd32(b + 28 + 4 * k);
        if (!is_act(m, (int)oi) || m->t[oi].def < 0) FAIL("output");
        m->out[k] = (int)oi;
        m->t[oi].last = (int)no;                     /* alive after the run */
    }
    for (int i = 0; i < m->nt; i++) if (m->t[i].kind == LNN_ACTIVATION && m->t[i].def == -2) m->t[i].def = m->t[i].last = -3;   /* unused: no room needed but placed apart */
    if (plan_arena(m, err, errlen)) goto bad;
    return m;
bad:
    lnn_free(m);
    return NULL;
}

float *lnn_input(Lnn *m, int *h, int *w, int *c) {
    Tensor *t = &m->t[m->input];
    if (h) *h = t->d[1];
    if (w) *w = t->d[2];
    if (c) *c = t->d[3];
    return t->p;
}

int lnn_outputs(const Lnn *m) { return m ? m->nout : 0; }
double lnn_macs(const Lnn *m) { return m ? m->macs : 0; }

const float *lnn_output(const Lnn *m, int k, size_t *count) {
    if (!m || k < 0 || k >= m->nout) return NULL;
    const Tensor *t = &m->t[m->out[k]];
    if (count) *count = t->n;
    return t->p;
}

/* ---- kernels ---- */

/* exp(x) without branches, so the loops around it vectorise: 2^n · e^r with |r| ≤ ln2/2 and a degree-6 polynomial
 * (relative error about 2e-7, the same as expf for these uses); x is clamped to ±87 */
static inline float nn_exp(float x) {
    x = x > 87.0f ? 87.0f : x < -87.0f ? -87.0f : x;
    float n = (float)(int32_t)(x * 1.44269504f + (x >= 0 ? 0.5f : -0.5f));
    float r = x - n * 0.693145752f - n * 1.42860677e-6f;
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.166666672f + r * (0.0416666679f + r * (0.00833333377f + r * 0.00138888892f)))));
    int32_t e = (int32_t)((uint32_t)((int32_t)n + 127) << 23);
    float s;
    memcpy(&s, &e, 4);
    return p * s;
}

NN_HOT static void act_n(float *restrict p, size_t n, int act) {
    switch (act) {
    case LNN_ACT_RELU: for (size_t i = 0; i < n; i++) p[i] = p[i] > 0 ? p[i] : 0; break;
    case LNN_ACT_HSWISH:
        for (size_t i = 0; i < n; i++) { float r = p[i] + 3; r = r < 0 ? 0 : r; r = r > 6 ? 6 : r; p[i] = p[i] * r * (1.0f / 6); }
        break;
    case LNN_ACT_SIGMOID: for (size_t i = 0; i < n; i++) p[i] = 1.0f / (1.0f + nn_exp(-p[i])); break;
    default: break;
    }
}

#define MAXC 4096

/* 1×1 stride-1 convolution: a matrix product, four pixels at a time so each weight row is loaded once per four */
NN_HOT static void conv1x1(const float *restrict x, float *restrict y, const float *restrict w, const float *restrict bias, size_t npx, int C, int O) {
    size_t p = 0;
    for (; p + 4 <= npx; p += 4) {
        float *y0 = y + p * O, *y1 = y0 + O, *y2 = y1 + O, *y3 = y2 + O;
        const float *x0 = x + p * C, *x1 = x0 + C, *x2 = x1 + C, *x3 = x2 + C;
        for (int o = 0; o < O; o++) y0[o] = y1[o] = y2[o] = y3[o] = bias[o];
        for (int i = 0; i < C; i++) {
            const float *wr = w + (size_t)i * O;
            float a0 = x0[i], a1 = x1[i], a2 = x2[i], a3 = x3[i];
            for (int o = 0; o < O; o++) { float wv = wr[o]; y0[o] += a0 * wv; y1[o] += a1 * wv; y2[o] += a2 * wv; y3[o] += a3 * wv; }
        }
    }
    for (; p < npx; p++) {
        float *yr = y + p * O;
        const float *xr = x + p * C;
        for (int o = 0; o < O; o++) yr[o] = bias[o];
        for (int i = 0; i < C; i++) { const float *wr = w + (size_t)i * O; float a = xr[i]; for (int o = 0; o < O; o++) yr[o] += a * wr[o]; }
    }
}

/* A k×k convolution. CONV_BODY is instantiated with O as a constant for the two first layers (16 and 24 outputs over
 * 3 input channels) so the accumulators stay in vector registers; any other O takes the generic copy. */
#define CONV_BODY(O_, ACC_N)                                                                                          \
    const int H = X->d[1], W = X->d[2], C = X->d[3], OH = Y->d[1], OW = Y->d[2], O = (O_);                            \
    const int sh = op->p[0], sw = op->p[1], kh = op->kh, kw = op->kw;                                                  \
    const float *x = X->p, *w = op->w;                                                                                 \
    float acc[ACC_N];                                                                                                  \
    for (int oy = 0; oy < OH; oy++) for (int ox = 0; ox < OW; ox++) {                                                  \
        for (int o = 0; o < O; o++) acc[o] = bias[o];                                                                  \
        for (int ky = 0; ky < kh; ky++) {                                                                              \
            int iy = oy * sh - op->pad_t + ky;                                                                         \
            if (iy < 0 || iy >= H) continue;                                                                           \
            for (int kx = 0; kx < kw; kx++) {                                                                          \
                int ix = ox * sw - op->pad_l + kx;                                                                     \
                if (ix < 0 || ix >= W) continue;                                                                       \
                const float *xr = x + ((size_t)iy * W + ix) * C;                                                       \
                const float *wk = w + ((size_t)ky * kw + kx) * C * O;                                                  \
                for (int i = 0; i < C; i++) { float a = xr[i]; const float *wr = wk + (size_t)i * O;                   \
                                              for (int o = 0; o < O; o++) acc[o] += a * wr[o]; }                       \
            }                                                                                                          \
        }                                                                                                              \
        float *yr = Y->p + ((size_t)oy * OW + ox) * O;                                                                 \
        for (int o = 0; o < O; o++) yr[o] = acc[o];                                                                    \
    }
NN_HOT static void conv_gen16(const Op *op, const Tensor *X, Tensor *Y, const float *bias) { CONV_BODY(16, 16) }
NN_HOT static void conv_gen24(const Op *op, const Tensor *X, Tensor *Y, const float *bias) { CONV_BODY(24, 24) }
NN_HOT static void conv_genN(const Op *op, const Tensor *X, Tensor *Y, const float *bias) { CONV_BODY(Y->d[3] < MAXC ? Y->d[3] : MAXC, MAXC) }
static void conv_gen(const Op *op, const Tensor *X, Tensor *Y, const float *bias) {
    if (Y->d[3] == 16) conv_gen16(op, X, Y, bias);
    else if (Y->d[3] == 24) conv_gen24(op, X, Y, bias);
    else conv_genN(op, X, Y, bias);
}

/* Depthwise: the inner loop runs over channels; pixels whose window lies inside the input skip the bound checks. */
NN_HOT static void dwconv(const Op *op, const Tensor *X, Tensor *Y, const float *bias) {
    const int H = X->d[1], W = X->d[2], C = X->d[3], OH = Y->d[1], OW = Y->d[2];
    const int sh = op->p[0], sw = op->p[1], kh = op->kh, kw = op->kw;
    const float *x = X->p, *w = op->w;
    float *y = Y->p;
    for (int oy = 0; oy < OH; oy++) {
        int iy0 = oy * sh - op->pad_t;
        int rows_in = iy0 >= 0 && iy0 + kh <= H;
        for (int ox = 0; ox < OW; ox++) {
            float *restrict yr = y + ((size_t)oy * OW + ox) * C;
            int ix0 = ox * sw - op->pad_l;
            for (int c = 0; c < C; c++) yr[c] = bias[c];
            if (rows_in && ix0 >= 0 && ix0 + kw <= W) {
                for (int ky = 0; ky < kh; ky++) {
                    const float *restrict xr = x + ((size_t)(iy0 + ky) * W + ix0) * C;
                    const float *restrict wr = w + (size_t)ky * kw * C;
                    for (int k = 0; k < kw * C; k += C) for (int c = 0; c < C; c++) yr[c] += xr[k + c] * wr[k + c];
                }
                continue;
            }
            for (int ky = 0; ky < kh; ky++) {
                int iy = iy0 + ky;
                if (iy < 0 || iy >= H) continue;
                for (int kx = 0; kx < kw; kx++) {
                    int ix = ix0 + kx;
                    if (ix < 0 || ix >= W) continue;
                    const float *xr = x + ((size_t)iy * W + ix) * C, *wr = w + ((size_t)ky * kw + kx) * C;
                    for (int c = 0; c < C; c++) yr[c] += xr[c] * wr[c];
                }
            }
        }
    }
}

/* 2×2 stride-2 transposed convolution; with one output channel (the segmenter's last layer) it is a dot product per
 * output pixel over the input channels */
NN_HOT static void tconv(const Op *op, const Tensor *X, Tensor *Y, const float *bias) {
    const int H = X->d[1], W = X->d[2], C = X->d[3], OW = Y->d[2], O = Y->d[3];
    if (O == 1 && C <= 64) {
        float w4[64][4];                         /* [input channel][ky * 2 + kx] */
        for (int i = 0; i < C; i++) for (int k = 0; k < 4; k++) w4[i][k] = op->w[(size_t)k * C + i];
        for (int iy = 0; iy < H; iy++) for (int ix = 0; ix < W; ix++) {
            const float *restrict xr = X->p + ((size_t)iy * W + ix) * C;
            float a[4] = { bias[0], bias[0], bias[0], bias[0] };
            for (int i = 0; i < C; i++) for (int k = 0; k < 4; k++) a[k] += xr[i] * w4[i][k];
            float *y0 = Y->p + (size_t)(iy * 2) * OW + ix * 2, *y1 = y0 + OW;
            y0[0] = a[0]; y0[1] = a[1]; y1[0] = a[2]; y1[1] = a[3];
        }
        return;
    }
    for (int iy = 0; iy < H; iy++) for (int ix = 0; ix < W; ix++) {
        const float *restrict xr = X->p + ((size_t)iy * W + ix) * C;
        for (int ky = 0; ky < 2; ky++) for (int kx = 0; kx < 2; kx++) {
            float *restrict yr = Y->p + ((size_t)(iy * 2 + ky) * OW + ix * 2 + kx) * O;
            const float *restrict wk = op->w + ((size_t)ky * 2 + kx) * C * O;
            for (int o = 0; o < O; o++) yr[o] = bias[o];
            for (int i = 0; i < C; i++) { float a = xr[i]; const float *wr = wk + (size_t)i * O; for (int o = 0; o < O; o++) yr[o] += a * wr[o]; }
        }
    }
}

static void resize_bilinear(const Op *op, const Tensor *X, Tensor *Y) {
    const int H = X->d[1], W = X->d[2], C = X->d[3], OH = Y->d[1], OW = Y->d[2];
    const int align = op->p[0], half = op->p[1];
    float sy = align && OH > 1 ? (float)(H - 1) / (OH - 1) : (float)H / OH;
    float sx = align && OW > 1 ? (float)(W - 1) / (OW - 1) : (float)W / OW;
    for (int oy = 0; oy < OH; oy++) {
        float fy = half ? (oy + 0.5f) * sy - 0.5f : oy * sy;
        int y0 = (int)floorf(fy), y1;
        float ly = fy - (float)y0;
        if (y0 < 0) { y0 = 0; ly = 0; }
        y1 = y0 + 1 < H ? y0 + 1 : H - 1;
        if (y0 >= H - 1) { y0 = y1 = H - 1; ly = 0; }
        for (int ox = 0; ox < OW; ox++) {
            float fx = half ? (ox + 0.5f) * sx - 0.5f : ox * sx;
            int x0 = (int)floorf(fx), x1;
            float lx = fx - (float)x0;
            if (x0 < 0) { x0 = 0; lx = 0; }
            x1 = x0 + 1 < W ? x0 + 1 : W - 1;
            if (x0 >= W - 1) { x0 = x1 = W - 1; lx = 0; }
            const float *a = X->p + ((size_t)y0 * W + x0) * C, *b = X->p + ((size_t)y0 * W + x1) * C;
            const float *c = X->p + ((size_t)y1 * W + x0) * C, *d = X->p + ((size_t)y1 * W + x1) * C;
            float *o = Y->p + ((size_t)oy * OW + ox) * C;
            float w00 = (1 - ly) * (1 - lx), w01 = (1 - ly) * lx, w10 = ly * (1 - lx), w11 = ly * lx;
            for (int k = 0; k < C; k++) o[k] = a[k] * w00 + b[k] * w01 + c[k] * w10 + d[k] * w11;
        }
    }
}

static void maxpool(const Op *op, const Tensor *X, Tensor *Y) {
    const int H = X->d[1], W = X->d[2], C = X->d[3], OH = Y->d[1], OW = Y->d[2];
    for (int oy = 0; oy < OH; oy++) for (int ox = 0; ox < OW; ox++) {
        float *o = Y->p + ((size_t)oy * OW + ox) * C;
        for (int c = 0; c < C; c++) o[c] = -INFINITY;
        for (int ky = 0; ky < op->kh; ky++) {
            int iy = oy * op->p[0] - op->pad_t + ky;
            if (iy < 0 || iy >= H) continue;
            for (int kx = 0; kx < op->kw; kx++) {
                int ix = ox * op->p[1] - op->pad_l + kx;
                if (ix < 0 || ix >= W) continue;
                const float *xr = X->p + ((size_t)iy * W + ix) * C;
                for (int c = 0; c < C; c++) if (xr[c] > o[c]) o[c] = xr[c];
            }
        }
    }
}

#ifdef NN_PROFILE
#include <time.h>
double nn_prof_ms[LNN_MAX_OPS];
static double prof_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
#endif

static int nn_run_ops(Lnn *m);
int lnn_run(Lnn *m) {
    if (!m) return -1;
#if defined(__x86_64__)
    /* denormals flushed to zero for the run (tiny activations deep in the decoder would otherwise take the slow
     * microcode path); the caller's floating-point state comes back afterwards. aarch64 runs them at full speed. */
    unsigned int csr = __builtin_ia32_stmxcsr();
    __builtin_ia32_ldmxcsr(csr | 0x8040);
#endif
    int rc = nn_run_ops(m);
#if defined(__x86_64__)
    __builtin_ia32_ldmxcsr(csr);
#endif
    return rc;
}

static int nn_run_ops(Lnn *m) {
    for (int i = 0; i < m->no; i++) {
#ifdef NN_PROFILE
        double prof_t0 = prof_now();
#endif
        const Op *op = &m->o[i];
        const Tensor *X = &m->t[op->in[0]];
        Tensor *Y = &m->t[op->out];
        const int C = X->d[3];
        const size_t npx = (size_t)X->d[1] * X->d[2];
        switch (op->op) {
        case LNN_CONV: {
            const float *bias = m->t[op->in[2]].p;
            if (op->kh == 1 && op->kw == 1 && op->p[0] == 1 && op->p[1] == 1) conv1x1(X->p, Y->p, op->w, bias, npx, C, Y->d[3]);
            else conv_gen(op, X, Y, bias);
            act_n(Y->p, Y->n, op->p[3]);
            break;
        }
        case LNN_DWCONV: dwconv(op, X, Y, m->t[op->in[2]].p); act_n(Y->p, Y->n, op->p[3]); break;
        case LNN_TCONV: tconv(op, X, Y, m->t[op->in[2]].p); act_n(Y->p, Y->n, op->p[3]); break;
        case LNN_ADD: {
            const float *b = m->t[op->in[1]].p;
            for (size_t k = 0; k < Y->n; k++) Y->p[k] = X->p[k] + b[k];
            act_n(Y->p, Y->n, op->p[3]);
            break;
        }
        case LNN_MUL: {
            const Tensor *B = &m->t[op->in[1]];
            if (B->n == X->n) for (size_t k = 0; k < Y->n; k++) Y->p[k] = X->p[k] * B->p[k];
            else for (size_t q = 0; q < npx; q++) for (int c = 0; c < C; c++) Y->p[q * C + c] = X->p[q * C + c] * B->p[c];
            act_n(Y->p, Y->n, op->p[3]);
            break;
        }
        case LNN_MEAN: {
            double acc[MAXC];
            int cc = C < MAXC ? C : MAXC;
            for (int c = 0; c < cc; c++) acc[c] = 0;
            for (size_t q = 0; q < npx; q++) for (int c = 0; c < cc; c++) acc[c] += X->p[q * C + c];
            for (int c = 0; c < cc; c++) Y->p[c] = (float)(acc[c] / (double)npx);
            break;
        }
        case LNN_RESIZE: resize_bilinear(op, X, Y); break;
        case LNN_PAD: {
            int OC = Y->d[3];
            for (size_t q = 0; q < npx; q++) {
                float *o = Y->p + q * OC;
                memset(o, 0, sizeof(float) * (size_t)op->p[0]);
                memcpy(o + op->p[0], X->p + q * C, sizeof(float) * (size_t)C);
                memset(o + op->p[0] + C, 0, sizeof(float) * (size_t)op->p[1]);
            }
            break;
        }
        case LNN_MAXPOOL: maxpool(op, X, Y); break;
        case LNN_RESHAPE: memcpy(Y->p, X->p, X->n * sizeof(float)); break;
        case LNN_CONCAT: {
            const Tensor *B = &m->t[op->in[1]];
            memcpy(Y->p, X->p, X->n * sizeof(float));
            memcpy(Y->p + X->n, B->p, B->n * sizeof(float));
            break;
        }
        case LNN_RELU: case LNN_HSWISH: case LNN_SIGMOID:
            memcpy(Y->p, X->p, X->n * sizeof(float));
            act_n(Y->p, Y->n, op->op == LNN_RELU ? LNN_ACT_RELU : op->op == LNN_HSWISH ? LNN_ACT_HSWISH : LNN_ACT_SIGMOID);
            break;
        default: return -1;
        }
#ifdef NN_PROFILE
        nn_prof_ms[i] += prof_now() - prof_t0;
#endif
    }
    return 0;
}

#ifdef NN_PROFILE
void nn_prof_dump(const Lnn *m, int runs) {
    for (int i = 0; i < m->no; i++) {
        const Op *o = &m->o[i];
        const Tensor *x = &m->t[o->in[0]], *y = &m->t[o->out];
        fprintf(stderr, "%.3f ms: %3d op%-2d k%dx%d s%d %3dx%3dx%3d -> %3dx%3dx%3d\n", nn_prof_ms[i] / runs, i, o->op, o->kh, o->kw, o->p[0],
                x->d[1], x->d[2], x->d[3], y->d[1], y->d[2], y->d[3]);
    }
}
#endif
