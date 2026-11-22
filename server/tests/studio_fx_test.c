/* studio_fx_test.c — LordOS Studio's phase 3 looks on fixed frames (docs/CAMERA-STUDIO-PLAN.md 4.4, 5; 11).
 *
 *   build/studio-fx-test          each effect on tests/data/studio's CC0 photographs (scaled to 1280×720) and on frames
 *                                 made from them (a green screen behind the person): what it must do where the
 *                                 reference matte says "sure person" and "sure background"; CPU always, and the GPU
 *                                 path when a render node opens (the two must agree)
 *   build/studio-fx-test time     also the cost of each effect at 1280×720 (medians of 40 frames), CPU and GPU
 *   STUDIO_TEST_OUT=<dir>         writes the frames as PPM files to look at
 *
 * Run from server/. */
#include "media/studio/models.h"
#include "media/studio/st.h"

#include "stb_image.h"

#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define W 1280
#define H 720
#define SEGW STUDIO_SEG_W
#define SEGH STUDIO_SEG_H

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = b ? (size_t)n : 0;
    return b;
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* a 256×144 PNG scaled up (bilinear) to a 1280×720 BGRA frame */
static uint8_t *frame_of(const char *name) {
    char path[256];
    snprintf(path, sizeof path, "tests/data/studio/%s-seg.png", name);
    int w, h, c;
    unsigned char *px = stbi_load(path, &w, &h, &c, 3);
    if (!px) return NULL;
    uint8_t *f = malloc((size_t)W * H * 4);
    for (int y = 0; y < H; y++) {
        float fy = (y + 0.5f) * h / H - 0.5f;
        fy = fy < 0 ? 0 : fy;
        int y0 = (int)fy, y1 = y0 + 1 < h ? y0 + 1 : h - 1;
        float ly = fy - y0;
        for (int x = 0; x < W; x++) {
            float fx = (x + 0.5f) * w / W - 0.5f;
            fx = fx < 0 ? 0 : fx;
            int x0 = (int)fx, x1 = x0 + 1 < w ? x0 + 1 : w - 1;
            float lx = fx - x0;
            for (int k = 0; k < 3; k++) {
                float a = px[(y0 * w + x0) * 3 + k], b = px[(y0 * w + x1) * 3 + k], cc = px[(y1 * w + x0) * 3 + k], d = px[(y1 * w + x1) * 3 + k];
                float t = a + (b - a) * lx, u = cc + (d - cc) * lx;
                f[((size_t)y * W + x) * 4 + 2 - k] = (uint8_t)(t + (u - t) * ly + 0.5f);
            }
            f[((size_t)y * W + x) * 4 + 3] = 255;
        }
    }
    stbi_image_free(px);
    return f;
}

static float *ref_matte(const char *name) {
    char path[256];
    snprintf(path, sizeof path, "tests/data/studio/%s-seg.ref", name);
    size_t n;
    unsigned char *r = slurp(path, &n);
    if (!r || n != SEGW * SEGH * 2) { free(r); return NULL; }
    float *m = malloc(sizeof(float) * SEGW * SEGH);
    for (int i = 0; i < SEGW * SEGH; i++) m[i] = (r[2 * i] | r[2 * i + 1] << 8) / 65535.0f;
    free(r);
    return m;
}
static inline float refat(const float *ref, int x, int y) { return ref[(y * SEGH / H) * SEGW + x * SEGW / W]; }

static void save(const char *name, const char *what, const uint8_t *bgra) {
    const char *dir = getenv("STUDIO_TEST_OUT");
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s-%s.ppm", dir, name, what);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6 %d %d 255\n", W, H);
    for (size_t i = 0; i < (size_t)W * H; i++) { uint8_t p[3] = { bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4] }; fwrite(p, 1, 3, f); }
    fclose(f);
}

static double luma(const uint8_t *p) { return 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2]; }

/* mean luma and mean RGB over sure person (person=1) or sure background */
static void region_mean(const uint8_t *f, const float *ref, int person, double *y, double rgb[3]) {
    double s = 0, c[3] = { 0, 0, 0 };
    long n = 0;
    for (int yy = 0; yy < H; yy += 2) for (int x = 0; x < W; x += 2) {
        float m = refat(ref, x, yy);
        if (person ? m < 0.98f : m > 0.02f) continue;
        const uint8_t *p = f + ((size_t)yy * W + x) * 4;
        s += luma(p);
        c[0] += p[2]; c[1] += p[1]; c[2] += p[0];
        n++;
    }
    *y = n ? s / n : 0;
    if (rgb) for (int k = 0; k < 3; k++) rgb[k] = n ? c[k] / n : 0;
}

static void compare_cpu_gpu(const char *what, const uint8_t *cpu, const uint8_t *gpu, double mean_max) {
    double s = 0;
    long over = 0;
    int mx = 0;
    for (size_t i = 0; i < (size_t)W * H * 4; i++) {
        if ((i & 3) == 3) continue;
        int d = abs(cpu[i] - gpu[i]);
        s += d;
        if (d > 12) over++;
        if (d > mx) mx = d;
    }
    double mean = s / ((double)W * H * 3);
    printf("  %s: CPU and GPU differ by %.2f levels on average, %ld values over 12, at most %d\n", what, mean, over, mx);
    CHECK(mean < mean_max, "%s: GPU differs from the CPU by %.2f levels", what, mean);
    CHECK(over < (long)W * H * 3 / 500, "%s: %ld values differ by more than 12", what, over);
}

static unsigned char *seg_model, *face_model;
static size_t seg_len, face_len;
static int rfd = -1;

static StFx *make(int gpu) {
    char err[96] = "";
    StFx *s = stfx_new(seg_model, seg_len, gpu ? rfd : -1, err, sizeof err);
    if (s && face_model) stfx_face_model(s, face_model, face_len, err, sizeof err);
    return s;
}

/* frames through a fresh pipeline (its history: those frames only) */
static void run(StFx *s, const StCfg *c, const uint8_t *in, uint8_t *out, int frames) {
    stfx_config(s, c);
    for (int i = 0; i < frames; i++) stfx_frame(s, in, W * 4, W, H, out, W * 4);
}

static void cpu_vs_gpu(const char *what, const StCfg *c, const uint8_t *in, const uint8_t *pic, int pw, int ph, const uint8_t *cpu_out, double mean_max) {
    if (rfd < 0) return;
    StFx *g = make(1);
    if (!g) return;
    uint8_t *gout = malloc((size_t)W * H * 4);
    if (pic) stfx_background(g, pic, pw, ph, pw * 4);
    run(g, c, in, gout, 3);
    StStats st;
    stfx_stats(g, &st);
    CHECK(st.gpu, "%s: the GPU path fell back to the CPU", what);
    compare_cpu_gpu(what, cpu_out, gout, mean_max);
    free(gout);
    stfx_free(g);
}

/* ---- the pure pieces ---------------------------------------------------------------------------------------------- */

static void test_pieces(void) {
    /* the LUTs: none is the identity grid; every filter builds; mono is grey */
    static uint8_t lut[ST_LUT_N * ST_LUT_N * ST_LUT_N * 3];
    CHECK(!st_lut_build(ST_FILTER_NONE, lut), "lut none");
    int ident = 1;
    for (int b = 0; b < ST_LUT_N; b++) for (int g = 0; g < ST_LUT_N; g++) for (int r = 0; r < ST_LUT_N; r++) {
        const uint8_t *o = lut + (((size_t)b * ST_LUT_N + g) * ST_LUT_N + r) * 3;
        int e[3] = { (r * 255 + 16) / 32, (g * 255 + 16) / 32, (b * 255 + 16) / 32 };
        for (int k = 0; k < 3; k++) if (abs(o[k] - e[k]) > 1) ident = 0;
    }
    CHECK(ident, "the identity LUT isn't the identity");
    for (int f = 1; f <= ST_FILTER_LAST; f++) CHECK(!st_lut_build(f, lut), "filter %d", f);
    CHECK(st_lut_build(ST_FILTER_LAST + 1, lut) == -1 && st_lut_build(-1, lut) == -1, "an unknown filter was built");
    st_lut_build(ST_FILTER_MONO, lut);
    float in[3] = { 200, 40, 90 }, out[3];
    st_lut_apply(lut, in, out);
    CHECK(fabsf(out[0] - out[1]) < 1 && fabsf(out[1] - out[2]) < 1, "mono isn't grey (%.1f %.1f %.1f)", out[0], out[1], out[2]);
    st_lut_build(ST_FILTER_NONE, lut);
    st_lut_apply(lut, in, out);
    CHECK(fabsf(out[0] - 200) < 1.5f && fabsf(out[1] - 40) < 1.5f && fabsf(out[2] - 90) < 1.5f, "identity LUT changed a colour");
    st_lut_build(ST_FILTER_WARM, lut);
    float grey[3] = { 128, 128, 128 };
    st_lut_apply(lut, grey, out);
    CHECK(out[0] > out[2] + 10, "warm doesn't warm grey (%.1f vs %.1f)", out[0], out[2]);
    st_lut_build(ST_FILTER_COOL, lut);
    st_lut_apply(lut, grey, out);
    CHECK(out[2] > out[0] + 10, "cool doesn't cool grey");

    /* the colour match: a grey person toward a blue background, at 50 %: halfway in the mean, exposure clamped */
    StStat p = { 0 }, b = { 0 };
    for (int i = 0; i < 200; i++) {
        float c1[3] = { 120.0f + i % 20, 120.0f + i % 20, 120.0f + i % 20 }, c2[3] = { 40.0f + i % 10, 60.0f + i % 10, 180.0f + i % 10 };
        st_stat_add(&p, c1, 1);
        st_stat_add(&b, c2, 1);
    }
    float m[9], o[3];
    CHECK(!st_match_transform(&p, &b, 0.5f, m, o), "match transform");
    float c[3] = { 130, 130, 130 }, r[3];
    for (int i = 0; i < 3; i++) r[i] = m[3 * i] * c[0] + m[3 * i + 1] * c[1] + m[3 * i + 2] * c[2] + o[i];
    CHECK(r[2] > r[0] + 20, "the matched grey isn't bluer (%.1f %.1f %.1f)", r[0], r[1], r[2]);
    CHECK(fabsf((r[0] + r[1] + r[2]) / 3 - 105) < 15, "the matched luma isn't halfway (%.1f)", (r[0] + r[1] + r[2]) / 3);
    CHECK(!st_match_transform(&p, &b, 0.0f, m, o) && fabsf(m[0] - 1) < 1e-4f && fabsf(m[1]) < 1e-4f && fabsf(o[0]) < 1e-3f, "strength 0 isn't the identity");
    StStat few = { 0 };
    CHECK(st_match_transform(&few, &b, 0.5f, m, o) == -1, "thin statistics were used");
    /* exposure: a very bright background moves a dark person at most one stop */
    StStat dark = { 0 }, bright = { 0 };
    for (int i = 0; i < 100; i++) { float d[3] = { 40, 40, 40 }, w[3] = { 250, 250, 250 }; st_stat_add(&dark, d, 1); st_stat_add(&bright, w, 1); }
    st_match_transform(&dark, &bright, 1.0f, m, o);
    float dk[3] = { 40, 40, 40 };
    float L = 0;
    for (int i = 0; i < 3; i++) L += (m[3 * i] * dk[0] + m[3 * i + 1] * dk[1] + m[3 * i + 2] * dk[2] + o[i]) / 3;
    CHECK(L <= 81 && L >= 79, "the exposure moved %.1f, not one stop", L);

    /* the chroma key: green is the screen, skin and grey are not; blue for blue */
    float key[2];
    st_chroma_key(NULL, 0, 0, 0, ST_KEY_GREEN, key);
    float green[3] = { 40, 170, 70 }, skin[3] = { 210, 160, 130 }, gray[3] = { 128, 128, 128 }, blue[3] = { 30, 60, 200 };
    CHECK(st_chroma_alpha(green, key, 40, 20) < 0.05f, "green isn't keyed");
    CHECK(st_chroma_alpha(skin, key, 40, 20) > 0.95f && st_chroma_alpha(gray, key, 40, 20) > 0.95f, "skin or grey keyed");
    st_chroma_key(NULL, 0, 0, 0, ST_KEY_BLUE, key);
    CHECK(st_chroma_alpha(blue, key, 40, 20) < 0.05f && st_chroma_alpha(skin, key, 40, 20) > 0.95f, "blue key");
    float sp[3] = { 100, 200, 100 };
    st_despill(sp, ST_KEY_GREEN, 1.0f);
    CHECK(fabsf(sp[1] - 100) < 0.5f, "despill left green %.1f", sp[1]);

    /* Studio Light: the face lifted more than the rest of the person; the background dimmed half a stop at full */
    float face[4] = { 100, 100, 40, 50 };
    CHECK(st_light_person(100, 96, face, 100) > st_light_person(300, 300, face, 100), "the face isn't lifted more");
    CHECK(fabsf(st_light_background(100) - 0.7071f) < 0.01f && st_light_background(0) == 1.0f && st_light_person(1, 1, face, 0) == 1.0f, "light gains");

    /* the 1€ filter: a noisy constant is smoothed, a step is followed */
    StEuro e = { .mincut = 0.4f, .beta = 0.004f, .dcut = 1.0f };
    double var = 0;
    unsigned seed = 1;
    for (int i = 0; i < 200; i++) {
        seed = seed * 1103515245u + 12345u;
        float noisy = 100 + (float)((seed >> 16) % 21) - 10;
        float v = st_euro(&e, noisy, 1.0f / 30);
        if (i > 100) var += (v - 100) * (v - 100);
    }
    CHECK(sqrt(var / 99) < 2.5, "the 1€ filter left %.2f of jitter (the input had 6)", sqrt(var / 99));
    float v = 0;
    for (int i = 0; i < 90; i++) v = st_euro(&e, 300, 1.0f / 30);
    CHECK(fabsf(v - 300) < 3, "the 1€ filter didn't follow a step (%.1f)", v);

    /* the framing target: eyes at a third, inside the frame, the frame's shape, never past 2× */
    float f2[4] = { 640, 360, 120, 150 };
    StRect t;
    st_frame_target(f2, 330, W, H, 50, &t);
    CHECK(t.x >= 0 && t.y >= 0 && t.x + t.w <= W + 0.01f && t.y + t.h <= H + 0.01f, "target outside the frame");
    CHECK(fabsf(t.w / t.h - (float)W / H) < 0.01f, "target isn't 16:9");
    CHECK(fabsf(t.y + t.h / 3 - 330) < 2, "the eyes aren't at a third (%.1f vs %.1f)", t.y + t.h / 3, 330.0f);
    float tiny[4] = { 640, 360, 10, 12 };
    st_frame_target(tiny, 355, W, H, 100, &t);
    CHECK(t.h >= H * 0.5f - 0.01f, "zoomed past 2× (%.1f)", t.h);
    float edge[4] = { 30, 40, 100, 120 };
    st_frame_target(edge, 30, W, H, 80, &t);
    CHECK(t.x == 0 && t.y == 0, "a face in the corner pushed the crop out (%.1f, %.1f)", t.x, t.y);
    printf("pieces: LUTs, colour match, chroma key, light, 1€ filter, framing target: checked\n");
}

/* ---- the effects on frames ---------------------------------------------------------------------------------------- */

static uint8_t *gradient(int w, int h, int blue) {
    uint8_t *p = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint8_t *q = p + ((size_t)y * w + x) * 4;
        if (blue) { q[0] = (uint8_t)(170 + x * 60 / w); q[1] = (uint8_t)(70 + y * 40 / h); q[2] = 40; }
        else { q[0] = (uint8_t)(x * 255 / (w - 1)); q[1] = 90; q[2] = (uint8_t)(255 - x * 255 / (w - 1)); }
        q[3] = 255;
    }
    return p;
}

static void test_light(const char *name, const uint8_t *in, const float *ref) {
    StFx *s = make(0);
    uint8_t *out = malloc((size_t)W * H * 4);
    StCfg c = { .mode = ST_MODE_NONE, .light = 100 };
    run(s, &c, in, out, 3);
    double pi, po, bi, bo;
    region_mean(in, ref, 1, &pi, NULL);
    region_mean(out, ref, 1, &po, NULL);
    region_mean(in, ref, 0, &bi, NULL);
    region_mean(out, ref, 0, &bo, NULL);
    printf("  light: the person %.1f → %.1f, the background %.1f → %.1f\n", pi, po, bi, bo);
    CHECK(po > pi * 1.04, "%s: Studio Light didn't lift the person (%.1f → %.1f)", name, pi, po);
    CHECK(bi < 8 || fabs(bo / bi - 0.7071) < 0.06, "%s: the background wasn't dimmed half a stop (%.3f)", name, bo / bi);
    save(name, "light", out);
    cpu_vs_gpu("light", &c, in, NULL, 0, 0, out, 1.5);
    free(out);
    stfx_free(s);
}

static void test_match(const char *name, const uint8_t *in, const float *ref) {
    uint8_t *pic = gradient(640, 360, 1), *out = malloc((size_t)W * H * 4), *base = malloc((size_t)W * H * 4);
    StFx *s = make(0);
    stfx_background(s, pic, 640, 360, 640 * 4);
    StCfg c = { .mode = ST_MODE_PICTURE };
    run(s, &c, in, base, 2);
    c.match = ST_MATCH_MAX;
    run(s, &c, in, out, 2);
    double y0, y1, a[3], b[3];
    region_mean(base, ref, 1, &y0, a);
    region_mean(out, ref, 1, &y1, b);
    double before = a[2] - a[0], after = b[2] - b[0];
    printf("  match: the person's blue − red %.1f → %.1f\n", before, after);
    CHECK(after > before + 8, "%s: the colour match didn't move the person toward the blue picture", name);
    save(name, "match", out);
    cpu_vs_gpu("match", &c, in, pic, 640, 360, out, 1.5);
    stfx_free(s);
    free(pic); free(out); free(base);
}

static void test_filters(const char *name, const uint8_t *in) {
    StFx *s = make(0);
    uint8_t *out = malloc((size_t)W * H * 4), *half = malloc((size_t)W * H * 4);
    StCfg c = { .mode = ST_MODE_NONE, .filter = ST_FILTER_MONO, .filter_amt = 100 };
    run(s, &c, in, out, 1);
    long grey = 0;
    for (size_t i = 0; i < (size_t)W * H; i += 7) { const uint8_t *p = out + i * 4; grey += abs(p[0] - p[1]) <= 2 && abs(p[1] - p[2]) <= 2; }
    CHECK(grey > (long)W * H / 7 * 99 / 100, "%s: mono isn't grey", name);
    c.filter_amt = 0;
    run(s, &c, in, half, 1);
    CHECK(!memcmp(half, in, (size_t)W * H * 4), "%s: a filter at strength 0 changed the frame", name);
    c.filter = ST_FILTER_SEPIA;
    c.filter_amt = 100;
    run(s, &c, in, out, 1);
    c.filter_amt = 50;
    run(s, &c, in, half, 1);
    double err = 0;
    for (size_t i = 0; i < (size_t)W * H * 4; i += 4) for (int k = 0; k < 3; k++) err += fabs(half[i + k] - (in[i + k] + out[i + k]) / 2.0);
    err /= (double)W * H * 3;
    printf("  filters: mono grey, strength 0 exact, sepia at 50 %% is halfway within %.2f levels\n", err);
    CHECK(err < 1.0, "%s: half strength isn't halfway (%.2f)", name, err);
    save(name, "sepia", out);
    StCfg g = { .mode = ST_MODE_BLUR, .blur = 50, .filter = ST_FILTER_FILM, .filter_amt = 80 };
    StFx *s2 = make(0);
    run(s2, &g, in, out, 3);
    cpu_vs_gpu("film over blur", &g, in, NULL, 0, 0, out, 1.5);
    stfx_free(s2);
    free(out); free(half);
    stfx_free(s);
}

/* the person in front of a green screen: f04's sure background painted green (with some noise), and a little green
 * spill on the person's edge */
static void test_green(const char *name, const uint8_t *src, const float *ref) {
    uint8_t *in = malloc((size_t)W * H * 4), *out = malloc((size_t)W * H * 4), *pic = gradient(640, 360, 0);
    unsigned seed = 7;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *p = in + ((size_t)y * W + x) * 4;
        const uint8_t *q = src + ((size_t)y * W + x) * 4;
        float m = refat(ref, x, y);
        seed = seed * 1103515245u + 12345u;
        int n = (int)((seed >> 16) % 13) - 6;
        uint8_t g[4] = { (uint8_t)(80 + n), (uint8_t)(170 + n), (uint8_t)(60 + n), 255 };
        for (int k = 0; k < 4; k++) p[k] = (uint8_t)(q[k] * m + g[k] * (1 - m) + 0.5f);
    }
    float key[2];
    st_chroma_key(in, W * 4, W, H, ST_KEY_GREEN, key);
    printf("  green screen: the key from the border Cb %.0f Cr %.0f\n", key[0], key[1]);
    CHECK(fabsf(key[0] - 106) < 12 && fabsf(key[1] - 75) < 12, "%s: the screen's colour from the border is off", name);
    StFx *s = make(0);
    stfx_background(s, pic, 640, 360, 640 * 4);
    StCfg c = { .mode = ST_MODE_PICTURE, .key = ST_KEY_GREEN, .similarity = 40, .smoothness = 20, .spill = 60 };
    run(s, &c, in, out, 3);
    /* sure background: the picture; sure person: unchanged (but for despill) */
    double sb = 0, sp = 0;
    long nb = 0, np = 0, greenish = 0;
    for (int y = 0; y < H; y += 2) for (int x = 0; x < W; x += 2) {
        float m = refat(ref, x, y);
        const uint8_t *o = out + ((size_t)y * W + x) * 4, *i = in + ((size_t)y * W + x) * 4;
        if (m < 0.02f) {
            const uint8_t *p = pic + ((size_t)(y * 360 / H) * 640 + x * 640 / W) * 4;
            sb += abs(o[0] - p[0]) + abs(o[1] - p[1]) + abs(o[2] - p[2]);
            nb += 3;
            greenish += o[1] > o[0] + 60 && o[1] > o[2] + 60;
        } else if (m > 0.98f) {
            sp += abs(o[0] - i[0]) + abs(o[2] - i[2]);
            np += 2;
        }
    }
    printf("  green screen: the background is the picture within %.2f levels, %ld green pixels left; the person %.2f\n",
           nb ? sb / nb : 0.0, greenish, np ? sp / np : 0.0);
    CHECK(nb && sb / nb < 6, "%s: the screen wasn't replaced (%.2f)", name, sb / nb);
    CHECK(greenish < 50, "%s: %ld green pixels left behind", name, greenish);
    CHECK(np && sp / np < 3, "%s: the person changed (%.2f)", name, sp / np);
    save(name, "green-in", in);
    save(name, "green", out);
    cpu_vs_gpu("green screen", &c, in, pic, 640, 360, out, 1.5);
    stfx_free(s);
    free(in); free(out); free(pic);
}

/* a video background: each frame's picture is what shows (no stale frame), on both paths */
static void test_video(const char *name, const uint8_t *in, const float *ref) {
    uint8_t *a = gradient(W, H, 0), *b = gradient(W, H, 1), *out = malloc((size_t)W * H * 4);
    StFx *paths[2] = { make(0), rfd >= 0 ? make(1) : NULL };
    for (int p = 0; p < 2; p++) {
        StFx *s = paths[p];
        if (!s) continue;
        StCfg c = { .mode = ST_MODE_VIDEO };
        stfx_config(s, &c);
        for (int f = 0; f < 4; f++) {
            const uint8_t *pic = f & 1 ? b : a;
            stfx_background(s, pic, W, H, W * 4);
            stfx_frame(s, in, W * 4, W, H, out, W * 4);
            double d = 0;
            long n = 0;
            for (int y = 0; y < H; y += 3) for (int x = 0; x < W; x += 3) {
                if (refat(ref, x, y) > 0.02f) continue;
                const uint8_t *o = out + ((size_t)y * W + x) * 4, *q = pic + ((size_t)y * W + x) * 4;
                d += abs(o[0] - q[0]) + abs(o[1] - q[1]) + abs(o[2] - q[2]);
                n += 3;
            }
            CHECK(n && d / n < 3, "%s: video frame %d (%s) isn't the background (%.2f)", name, f, p ? "GPU" : "CPU", d / n);
        }
        stfx_free(s);
    }
    printf("  video: every frame's picture shows behind the person\n");
    free(a); free(b); free(out);
}

/* Center Stage on a frame whose person is off to one side: the crop closes in on the face, the eyes at a third,
 * steady once there; with no face for 2 s it goes back to the whole frame */
static int test_framing(const char *name, const uint8_t *src) {
    if (!face_model) return 0;
    /* the person farther away and off to one side: the frame at 60 % in the bottom right, its edges repeated */
    uint8_t *in = malloc((size_t)W * H * 4), *out = malloc((size_t)W * H * 4);
    const int ox = W - W * 6 / 10 - 40, oy = H - H * 6 / 10;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int sx = (x - ox) * 10 / 6, sy = (y - oy) * 10 / 6;
        sx = sx < 0 ? 0 : sx >= W ? W - 1 : sx;
        sy = sy < 0 ? 0 : sy >= H ? H - 1 : sy;
        memcpy(in + ((size_t)y * W + x) * 4, src + ((size_t)sy * W + sx) * 4, 4);
    }
    StFx *s = make(0);
    stfx_face_every(s, 1);
    StCfg c = { .mode = ST_MODE_NONE, .framing = 60 };
    StRect r, prev = { 0 };
    float face[4];
    double move = 0;
    int found = 0;
    for (int i = 0; i < 150; i++) {
        run(s, &c, in, out, 1);
        found = stfx_framing(s, &r, face);
        if (i >= 140) move += fabsf(r.x - prev.x) + fabsf(r.y - prev.y) + fabsf(r.h - prev.h);
        prev = r;
    }
    if (!found) { printf("  framing: no face found in %s (skipped)\n", name); free(in); free(out); stfx_free(s); return 0; }
    float fx = face[0] * W, fy = face[1] * H;
    printf("  framing: face at %.0f,%.0f (%.0f×%.0f); crop %.0f,%.0f %.0f×%.0f; moved %.2f px in the last 10 frames\n",
           fx, fy, face[2] * W, face[3] * H, r.x, r.y, r.w, r.h, move);
    /* the crop's height follows the face's (st_frame_target, Framing 60), within the 2× limit */
    float want = face[3] * H * (4.2f - 2.2f * 0.6f);
    want = want < H * 0.5f ? H * 0.5f : want > H ? (float)H : want;
    CHECK(fabsf(r.h - want) < H * 0.05f, "%s: the crop is %.0f high, not %.0f", name, r.h, want);
    CHECK(fabsf(r.x + r.w / 2 - fx) < r.w * 0.12f || r.x < 1 || r.x + r.w > W - 1, "%s: the face isn't centred in the crop", name);
    CHECK(fabsf(r.w / r.h - (float)W / H) < 0.01f, "%s: the crop isn't 16:9", name);
    CHECK(move < 3, "%s: the crop jitters (%.2f px)", name, move);
    save(name, "framing", out);
    /* no face: back to the whole frame after 2 s */
    memset(in, 128, (size_t)W * H * 4);
    double t0 = now_ms();
    while (now_ms() - t0 < 4200) run(s, &c, in, out, 1);
    stfx_framing(s, &r, NULL);
    CHECK(r.h > H * 0.98f && r.x < 2 && r.y < 2, "%s: Center Stage didn't go back to the whole frame without a face (%.0f)", name, r.h);
    free(in); free(out);
    stfx_free(s);
    return 1;
}

static int test_retouch(const char *name, const uint8_t *in) {
    if (!face_model) return 0;
    StFx *s = make(0);
    stfx_face_every(s, 1);
    uint8_t *out = malloc((size_t)W * H * 4);
    StCfg c = { .mode = ST_MODE_NONE, .retouch = 100 };
    /* a little grain on the frame, so there is fine detail to smooth */
    uint8_t *g = malloc((size_t)W * H * 4);
    unsigned seed = 3;
    for (size_t i = 0; i < (size_t)W * H * 4; i++) { seed = seed * 1103515245u + 12345u; int n = (int)((seed >> 16) % 11) - 5; int v = in[i] + ((i & 3) == 3 ? 0 : n); g[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
    run(s, &c, g, out, 2);
    float face[4];
    if (!stfx_framing(s, NULL, face)) { printf("  retouch: no face in %s (skipped)\n", name); free(out); free(g); stfx_free(s); return 0; }
    int fx = (int)(face[0] * W), fy = (int)(face[1] * H), fw = (int)(face[2] * W), fh = (int)(face[3] * H);
    /* fine detail (|Laplacian| of the luma) in the face's middle, before and after; nothing changes far from it */
    double e0 = 0, e1 = 0;
    for (int y = fy - fh / 4; y < fy + fh / 4; y++) for (int x = fx - fw / 4; x < fx + fw / 4; x++) {
        if (x < 1 || y < 1 || x >= W - 1 || y >= H - 1) continue;
        const uint8_t *a = g + ((size_t)y * W + x) * 4, *b = out + ((size_t)y * W + x) * 4;
        e0 += fabs(4 * luma(a) - luma(a - 4) - luma(a + 4) - luma(a - W * 4) - luma(a + W * 4));
        e1 += fabs(4 * luma(b) - luma(b - 4) - luma(b + 4) - luma(b - W * 4) - luma(b + W * 4));
    }
    long far = 0;
    for (int y = 0; y < H; y += 2) for (int x = 0; x < W; x += 2) {
        if (abs(x - fx) < fw * 1.3 && abs(y - fy) < fh * 1.6) continue;
        far += memcmp(out + ((size_t)y * W + x) * 4, g + ((size_t)y * W + x) * 4, 3) != 0;
    }
    printf("  retouch: the face's fine detail %.0f → %.0f (%.0f %%); %ld pixels changed away from it\n", e0, e1, e0 > 0 ? 100 * (1 - e1 / e0) : 0.0, far);
    CHECK(e1 < e0 * 0.85, "%s: Retouch didn't smooth the face (%.0f → %.0f)", name, e0, e1);
    CHECK(far == 0, "%s: Retouch changed %ld pixels away from the face", name, far);
    save(name, "retouch", out);
    free(out); free(g);
    stfx_free(s);
    return 1;
}

static void timing(const uint8_t *in, const uint8_t *frame2) {
    static const struct { const char *name; StCfg c; int pic; } cases[] = {
        { "blur (phase 1)", { .mode = ST_MODE_BLUR, .blur = 50 }, 0 },
        { "picture (phase 1)", { .mode = ST_MODE_PICTURE }, 1 },
        { "video (a new 720p picture every frame)", { .mode = ST_MODE_VIDEO }, 2 },
        { "green screen + picture", { .mode = ST_MODE_PICTURE, .key = ST_KEY_GREEN, .similarity = 40, .smoothness = 20, .spill = 50 }, 1 },
        { "colour match + picture", { .mode = ST_MODE_PICTURE, .match = 30 }, 1 },
        { "Studio Light (no background)", { .mode = ST_MODE_NONE, .light = 50 }, 0 },
        { "Studio Light + blur", { .mode = ST_MODE_BLUR, .blur = 50, .light = 50 }, 0 },
        { "filter (Film) only", { .mode = ST_MODE_NONE, .filter = ST_FILTER_FILM, .filter_amt = 100 }, 0 },
        { "filter (Film) + blur", { .mode = ST_MODE_BLUR, .blur = 50, .filter = ST_FILTER_FILM, .filter_amt = 100 }, 0 },
        { "Center Stage only", { .mode = ST_MODE_NONE, .framing = 60 }, 0 },
        { "Retouch only", { .mode = ST_MODE_NONE, .retouch = 60 }, 0 },
        { "everything (blur, light, film, Center Stage, retouch)", { .mode = ST_MODE_BLUR, .blur = 50, .light = 50, .filter = ST_FILTER_FILM, .filter_amt = 80, .framing = 60, .retouch = 50 }, 0 },
    };
    uint8_t *pic = gradient(1920, 1080, 1), *vid = gradient(W, H, 0), *vid2 = gradient(W, H, 1), *out = malloc((size_t)W * H * 4);
    for (int p = 0; p < 2; p++) {
        if (p && rfd < 0) break;
        for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
            StFx *s = make(p);
            if (cases[k].pic == 1) stfx_background(s, pic, 1920, 1080, 1920 * 4);
            stfx_config(s, &cases[k].c);
            double t[40], fr[40], fa[40];
            for (int i = -5; i < 40; i++) {
                if (cases[k].pic == 2) stfx_background(s, i & 1 ? vid : vid2, W, H, W * 4);
                const uint8_t *f = i & 1 ? frame2 : in;
                double t0 = now_ms();
                stfx_frame(s, f, W * 4, W, H, out, W * 4);
                StStats st;
                stfx_stats(s, &st);
                if (i >= 0) { t[i] = now_ms() - t0; fr[i] = st.frame_ms; fa[i] = st.face_ms; }
            }
            double *arr[3] = { t, fr, fa };
            for (int a = 0; a < 3; a++) for (int i = 1; i < 40; i++) { double v = arr[a][i]; int j = i - 1; while (j >= 0 && arr[a][j] > v) { arr[a][j + 1] = arr[a][j]; j--; } arr[a][j + 1] = v; }
            printf("time %s 1280x720 %-55s total %6.2f ms (Center Stage and Retouch %.2f ms, BlazeFace %.2f ms when it runs)\n", p ? "GPU" : "CPU",
                   cases[k].name, t[20], fr[20], fa[39]);
            stfx_free(s);
        }
    }
    free(pic); free(vid); free(vid2); free(out);
}

int main(int argc, char **argv) {
    int time_it = argc > 1 && !strcmp(argv[1], "time");
    seg_model = slurp("../assets/studio/models/" STUDIO_SEG_FILE, &seg_len);
    face_model = slurp("../assets/studio/models/" STUDIO_FACE_FILE, &face_len);
    if (!seg_model || !face_model) { printf("FAIL: a model is missing\n"); return 1; }
#ifdef LORD_GPU
    for (int i = 128; i < 136 && rfd < 0; i++) {
        char dev[32];
        snprintf(dev, sizeof dev, "/dev/dri/renderD%d", i);
        rfd = open(dev, O_RDWR | O_CLOEXEC);
    }
    if (rfd >= 0) {
        StFx *probe = make(1);
        StStats st = { 0 };
        uint8_t *a = calloc((size_t)W * H, 4), *b = malloc((size_t)W * H * 4);
        StCfg c = { .mode = ST_MODE_NONE, .filter = ST_FILTER_MONO, .filter_amt = 100 };
        if (probe) { run(probe, &c, a, b, 1); stfx_stats(probe, &st); }
        if (!st.gpu) { close(rfd); rfd = -1; }
        free(a); free(b);
        stfx_free(probe);
    }
#endif
    test_pieces();
    static const char *const names[] = { "f00", "f04", "f08", "f02", "f06" };
    int framed = 0, retouched = 0;
    for (int k = 0; k < 5; k++) {
        uint8_t *in = frame_of(names[k]);
        float *ref = ref_matte(names[k]);
        CHECK(in && ref, "%s: frame or reference missing", names[k]);
        if (!in || !ref) { free(in); free(ref); continue; }
        printf("%s:\n", names[k]);
        if (k < 3) {
            test_light(names[k], in, ref);
            test_match(names[k], in, ref);
            test_filters(names[k], in);
            test_green(names[k], in, ref);
            test_video(names[k], in, ref);
        }
        framed += test_framing(names[k], in);
        retouched += test_retouch(names[k], in);
        free(in);
        free(ref);
    }
    CHECK(framed >= 2, "Center Stage found a face in only %d frames", framed);
    CHECK(retouched >= 2, "Retouch found a face in only %d frames", retouched);
    if (time_it) {
        uint8_t *a = frame_of("f04"), *b = frame_of("f08");
        if (a && b) timing(a, b);
        free(a); free(b);
    }
    if (rfd < 0) printf("GPU path: skipped (no render node or no OpenGL ES 3)\n");
    else close(rfd);
    free(seg_model);
    free(face_model);
    if (fails) { printf("studio-fx-test: %d failures\n", fails); return 1; }
    printf("studio-fx-test: ok\n");
    return 0;
}
