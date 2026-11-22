/* studio_nn_test.c — LordOS Studio's own inference against ONNX Runtime (docs/CAMERA-STUDIO-PLAN.md 4.2).
 *
 *   build/studio-nn-test            check: model hashes, both models on 10 + 10 fixed frames against the stored ONNX
 *                                   Runtime outputs (tests/data/studio/, made by tools/studio/ort-ref.c), face
 *                                   decoding, and a hostile-file fuzz of the loader
 *   build/studio-nn-test time [N]   also the median time of one run of each model (one thread), for the plan's 3.3
 *
 * Run from server/. Frames: CC0 photographs from Wikimedia Commons (tests/data/studio/README.md). */
#include "media/studio/face.h"
#include "media/studio/models.h"
#include "media/studio/nn.h"
#include "pkg/crypto.h"

#include "stb_image.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

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

static int hash_ok(const unsigned char *b, size_t n, const char *hex) {
    uint8_t h[32];
    char s[65];
    sha256(b, n, h);
    for (int i = 0; i < 32; i++) snprintf(s + 2 * i, 3, "%02x", h[i]);
    return !strcmp(s, hex);
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static const char *const FRAMES[] = { "f00", "f01", "f02", "f03", "f04", "f05", "f06", "f08", "f09", "f10" };
#define NFRAMES (int)(sizeof FRAMES / sizeof FRAMES[0])

static int fill(Lnn *m, const char *png, int face) {
    int h, w, c, W, H;
    float *in = lnn_input(m, &H, &W, &c);
    unsigned char *px = stbi_load(png, &w, &h, &c, 3);
    if (!px || w != W || h != H) { stbi_image_free(px); return -1; }
    for (size_t k = 0; k < (size_t)W * H * 3; k++) in[k] = face ? px[k] / 127.5f - 1.0f : px[k] / 255.0f;
    stbi_image_free(px);
    return 0;
}

static float half_of(const unsigned char *p) { return lnn_half((unsigned short)(p[0] | p[1] << 8)); }

static void test_seg(Lnn *m) {
    double worst = 0, sum_all = 0;
    for (int f = 0; f < NFRAMES; f++) {
        char png[256], ref[256];
        snprintf(png, sizeof png, "tests/data/studio/%s-seg.png", FRAMES[f]);
        snprintf(ref, sizeof ref, "tests/data/studio/%s-seg.ref", FRAMES[f]);
        size_t rl;
        unsigned char *r = slurp(ref, &rl);
        CHECK(r && rl == 144 * 256 * 2, "%s: reference missing", ref);
        CHECK(!fill(m, png, 0), "%s: frame", png);
        if (!r || rl != 144 * 256 * 2) { free(r); continue; }
        CHECK(!lnn_run(m), "seg run");
        size_t n;
        const float *o = lnn_output(m, 0, &n);
        CHECK(o && n == 144 * 256, "seg output size");
        double sum = 0, mx = 0;
        for (size_t i = 0; o && i < n; i++) {
            double e = fabs(o[i] - (r[2 * i] | r[2 * i + 1] << 8) / 65535.0);
            sum += e;
            if (e > mx) mx = e;
        }
        double mae = sum / (double)n;
        CHECK(mae < 1e-3, "%s: segmenter MAE %.2e (ONNX Runtime)", FRAMES[f], mae);
        CHECK(mx < 2e-2, "%s: segmenter max error %.2e", FRAMES[f], mx);
        if (mx > worst) worst = mx;
        sum_all += mae;
        free(r);
    }
    printf("segmenter: %d frames, mean abs error %.2e, max %.2e against ONNX Runtime\n", NFRAMES, sum_all / NFRAMES, worst);
}

static void test_face(Lnn *m) {
    int with_face = 0;
    double sum_r = 0, sum_s = 0;
    for (int f = 0; f < NFRAMES; f++) {
        char png[256], ref[256];
        snprintf(png, sizeof png, "tests/data/studio/%s-face.png", FRAMES[f]);
        snprintf(ref, sizeof ref, "tests/data/studio/%s-face.ref", FRAMES[f]);
        size_t rl;
        unsigned char *r = slurp(ref, &rl);
        CHECK(r && rl == 896 * 17 * 2, "%s: reference missing", ref);
        CHECK(!fill(m, png, 1), "%s: frame", png);
        if (!r || rl != 896 * 17 * 2) { free(r); continue; }
        CHECK(!lnn_run(m), "face run");
        size_t n0, n1;
        const float *reg = lnn_output(m, 0, &n0), *sc = lnn_output(m, 1, &n1);
        CHECK(reg && sc && n0 == 896 * 16 && n1 == 896, "face output sizes");
        if (!reg || !sc) { free(r); continue; }
        /* the references are float16: compare relative to the value (half precision is about 5e-4) */
        double er = 0, es = 0;
        for (size_t i = 0; i < n0; i++) { double v = half_of(r + 2 * i); er += fabs(reg[i] - v) / fmax(1.0, fabs(v)); }
        for (size_t i = 0; i < n1; i++) { double v = half_of(r + 2 * (n0 + i)); es += fabs(sc[i] - v) / fmax(1.0, fabs(v)); }
        er /= (double)n0;
        es /= (double)n1;
        CHECK(er < 1e-3, "%s: BlazeFace regressors error %.2e", FRAMES[f], er);
        CHECK(es < 1e-3, "%s: BlazeFace scores error %.2e", FRAMES[f], es);
        sum_r += er;
        sum_s += es;
        Face faces[FACE_MAX];
        int nf = face_decode(reg, sc, 0.5f, faces, FACE_MAX);
        if (nf > 0) {
            with_face++;
            CHECK(faces[0].x > 0 && faces[0].x < 1 && faces[0].y > 0 && faces[0].y < 1 && faces[0].w > 0.05f && faces[0].w < 1.5f,
                  "%s: face box %.2f,%.2f %.2fx%.2f", FRAMES[f], faces[0].x, faces[0].y, faces[0].w, faces[0].h);
            /* the eyes are above the mouth */
            CHECK(faces[0].kp[0][1] < faces[0].kp[3][1] && faces[0].kp[1][1] < faces[0].kp[3][1], "%s: keypoints", FRAMES[f]);
        }
        free(r);
    }
    printf("BlazeFace: %d frames, mean relative error %.2e (boxes) %.2e (scores); a face found in %d\n", NFRAMES, sum_r / NFRAMES, sum_s / NFRAMES, with_face);
    CHECK(with_face >= 4, "faces found in only %d of %d frames", with_face, NFRAMES);     /* the crops cut several faces: 5 today */
}

/* the loader against hostile files: flipped bytes, truncations, huge counts. Each must be refused or, if it is still
 * a valid model, run without touching memory outside its arena (run under ASan/valgrind for the strong form). */
static void fuzz(const unsigned char *good, size_t len) {
    unsigned char *b = malloc(len + 64);
    uint32_t seed = 12345;
    int loaded = 0;
    for (int it = 0; it < 3000; it++) {
        memcpy(b, good, len);
        size_t n = len;
        int flips = 1 + it % 4;
        for (int k = 0; k < flips; k++) {
            seed = seed * 1664525u + 1013904223u;
            /* mostly the tables (header, tensors, ops), sometimes the weights */
            size_t span = (seed >> 8) % 4 ? 64 + 32 * 260 + 64 * 100 : len;
            size_t at = (seed >> 3) % (span < len ? span : len);
            seed = seed * 1664525u + 1013904223u;
            b[at] = (seed >> 16) % 3 == 0 ? 0xff : (unsigned char)(seed >> 24);
        }
        if (it % 50 == 0) n = (seed >> 4) % len;
        char err[96];
        Lnn *m = lnn_load(b, n, err, sizeof err);
        if (m) { loaded++; lnn_run(m); lnn_free(m); }
    }
    CHECK(!lnn_load(good, 10, NULL, 0), "a 10-byte file loads");
    CHECK(!lnn_load(NULL, 0, NULL, 0), "NULL loads");
    memcpy(b, good, len);
    b[8] = 0xff; b[9] = 0xff; b[10] = 0xff; b[11] = 0x7f;       /* 2^31 tensors */
    CHECK(!lnn_load(b, len, NULL, 0), "a huge tensor count loads");
    printf("fuzz: 3000 damaged files, %d still valid models (ran without trouble)\n", loaded);
    free(b);
}

static double median_ms(Lnn *m, int runs) {
    double *t = calloc((size_t)runs, sizeof(double));
    for (int r = -5; r < runs; r++) {
        double t0 = now_ms();
        lnn_run(m);
        if (r >= 0) t[r] = now_ms() - t0;
    }
    for (int a = 1; a < runs; a++) { double v = t[a]; int b = a - 1; while (b >= 0 && t[b] > v) { t[b + 1] = t[b]; b--; } t[b + 1] = v; }
    double med = t[runs / 2];
    free(t);
    return med;
}

int main(int argc, char **argv) {
    int timing = argc > 1 && !strcmp(argv[1], "time");
    int runs = argc > 2 ? atoi(argv[2]) : 200;
    if (runs < 10) runs = 10;
    size_t sl, fl;
    unsigned char *seg = slurp("../assets/studio/models/" STUDIO_SEG_FILE, &sl), *fb = slurp("../assets/studio/models/" STUDIO_FACE_FILE, &fl);
    if (!seg || !fb) { printf("FAIL: the models in assets/studio/models are missing\n"); return 1; }
    CHECK(hash_ok(seg, sl, STUDIO_SEG_SHA256), "segmenter hash differs from models.h");
    CHECK(hash_ok(fb, fl, STUDIO_FACE_SHA256), "BlazeFace hash differs from models.h");
    char err[96] = "";
    Lnn *ms = lnn_load(seg, sl, err, sizeof err);
    CHECK(ms, "segmenter: %s", err);
    Lnn *mf = lnn_load(fb, fl, err, sizeof err);
    CHECK(mf, "BlazeFace: %s", err);
    if (ms) test_seg(ms);
    if (mf) test_face(mf);
    fuzz(seg, sl);
    fuzz(fb, fl);
    if (timing && ms && mf) {
        printf("time: segmenter 256x144 %.3f ms (%.1f M multiply-adds), BlazeFace 128x128 %.3f ms (%.1f M), median of %d, one thread\n",
               median_ms(ms, runs), lnn_macs(ms) / 1e6, median_ms(mf, runs), lnn_macs(mf) / 1e6, runs);
    }
    lnn_free(ms);
    lnn_free(mf);
    free(seg);
    free(fb);
    if (fails) { printf("studio-nn-test: %d failures\n", fails); return 1; }
    printf("studio-nn-test: ok\n");
    return 0;
}
