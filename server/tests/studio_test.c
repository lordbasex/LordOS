/* studio_test.c — LordOS Studio's frame pipeline on fixed frames (docs/CAMERA-STUDIO-PLAN.md 3.2, 4.3; phase 1).
 *
 *   build/studio-test          the CPU path always; the GPU path too when a render node opens (both must agree)
 *   build/studio-test time     also the time of each step at 1280×720 (median of 60 frames), for the plan's 3.3;
 *                              -DSTUDIO_TEST_W=1920 -DSTUDIO_TEST_H=1080 measures another size
 *   STUDIO_TEST_OUT=<dir>      writes the composites as PPM files to look at
 *
 * Run from server/. The frames are tests/data/studio's CC0 photographs, scaled up to 1280×720. */
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

#ifndef STUDIO_TEST_W
#define STUDIO_TEST_W 1280
#define STUDIO_TEST_H 720
#endif
#define W STUDIO_TEST_W
#define H STUDIO_TEST_H

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

/* the reference matte from ONNX Runtime for this frame (u16) */
static float *ref_matte(const char *name) {
    char path[256];
    snprintf(path, sizeof path, "tests/data/studio/%s-seg.ref", name);
    size_t n;
    unsigned char *r = slurp(path, &n);
    if (!r || n != STUDIO_SEG_W * STUDIO_SEG_H * 2) { free(r); return NULL; }
    float *m = malloc(sizeof(float) * STUDIO_SEG_W * STUDIO_SEG_H);
    for (int i = 0; i < STUDIO_SEG_W * STUDIO_SEG_H; i++) m[i] = (r[2 * i] | r[2 * i + 1] << 8) / 65535.0f;
    free(r);
    return m;
}

/* mean absolute difference over the pixels where the reference matte (scaled) says sure person / sure background */
static void region_diff(const uint8_t *a, const uint8_t *b, const float *ref, int person, double *mean, long *count) {
    double s = 0;
    long n = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float m = ref[(y * STUDIO_SEG_H / H) * STUDIO_SEG_W + x * STUDIO_SEG_W / W];
        if (person ? m < 0.98f : m > 0.02f) continue;
        const uint8_t *p = a + ((size_t)y * W + x) * 4, *q = b + ((size_t)y * W + x) * 4;
        s += abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2]);
        n += 3;
    }
    *mean = n ? s / (double)n : 0;
    *count = n / 3;
}

/* the background's fine detail: mean |horizontal step| over sure background */
static double bg_detail(const uint8_t *f, const float *ref) {
    double s = 0;
    long n = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x + 1 < W; x++) {
        if (ref[(y * STUDIO_SEG_H / H) * STUDIO_SEG_W + x * STUDIO_SEG_W / W] > 0.02f) continue;
        const uint8_t *p = f + ((size_t)y * W + x) * 4;
        s += abs(p[0] - p[4]) + abs(p[1] - p[5]) + abs(p[2] - p[6]);
        n += 3;
    }
    return n ? s / (double)n : 0;
}

static void compare_cpu_gpu(const char *what, const uint8_t *cpu, const uint8_t *gpu) {
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
    CHECK(mean < 1.5, "%s: GPU differs from the CPU by %.2f levels", what, mean);
    CHECK(over < (long)W * H * 3 / 1000, "%s: %ld values differ by more than 12", what, over);
}

int main(int argc, char **argv) {
    int timing = argc > 1 && !strcmp(argv[1], "time");
    size_t ml;
    unsigned char *model = slurp("../assets/studio/models/" STUDIO_SEG_FILE, &ml);
    if (!model) { printf("FAIL: the segmenter is missing\n"); return 1; }
    char err[96] = "";
    StFx *cpu = stfx_new(model, ml, -1, err, sizeof err);
    CHECK(cpu, "pipeline: %s", err);
    if (!cpu) return 1;
    /* a render node for the GPU path, when there is one */
    StFx *gpu = NULL;
    int rfd = -1;
#ifdef LORD_GPU
    for (int i = 128; i < 136 && rfd < 0; i++) {
        char dev[32];
        snprintf(dev, sizeof dev, "/dev/dri/renderD%d", i);
        rfd = open(dev, O_RDWR | O_CLOEXEC);
    }
    if (rfd >= 0) gpu = stfx_new(model, ml, rfd, err, sizeof err);
#endif
    uint8_t *out = malloc((size_t)W * H * 4), *out2 = malloc((size_t)W * H * 4), *gout = malloc((size_t)W * H * 4);

    /* bad sizes are refused */
    uint8_t small[64 * 64 * 4] = { 0 };
    CHECK(stfx_frame(cpu, small, 63 * 4, 63, 63, out, W * 4) == -1, "an odd size was accepted");
    CHECK(stfx_frame(cpu, small, 32 * 4, 32, 32, out, W * 4) == -1, "a 32×32 frame was accepted");
    CHECK(stfx_background(cpu, small, 1, 1, 4) == -1, "a 1×1 picture was accepted");

    static const char *const names[] = { "f00", "f04", "f08" };
    /* a picture background: a horizontal gradient */
    uint8_t *pic = malloc(640 * 360 * 4);
    for (int y = 0; y < 360; y++) for (int x = 0; x < 640; x++) {
        uint8_t *p = pic + ((size_t)y * 640 + x) * 4;
        p[0] = (uint8_t)(x * 255 / 639); p[1] = 90; p[2] = (uint8_t)(255 - x * 255 / 639); p[3] = 255;
    }
    for (int k = 0; k < 3; k++) {
        const char *name = names[k];
        uint8_t *in = frame_of(name);
        float *ref = ref_matte(name);
        CHECK(in && ref, "%s: frame or reference missing", name);
        if (!in || !ref) { free(in); free(ref); continue; }
        printf("%s:\n", name);
        StCfg c = { .mode = ST_MODE_NONE };
        /* no effect: the same frame, or its mirror */
        stfx_config(cpu, &c);
        CHECK(!stfx_frame(cpu, in, W * 4, W, H, out, W * 4) && !memcmp(in, out, (size_t)W * H * 4), "%s: no effect changed the frame", name);
        c.mirror = 1;
        stfx_config(cpu, &c);
        stfx_frame(cpu, in, W * 4, W, H, out, W * 4);
        int mirrored = 1;
        for (int y = 0; y < H && mirrored; y += 7) for (int x = 0; x < W; x++)
            if (memcmp(out + ((size_t)y * W + x) * 4, in + ((size_t)y * W + W - 1 - x) * 4, 4)) { mirrored = 0; break; }
        CHECK(mirrored, "%s: the mirror is not exact", name);
        /* blur: the matte agrees with ONNX Runtime's (after the camera-size round trip), the person stays, the
         * background changes */
        c = (StCfg){ .mode = ST_MODE_BLUR, .blur = 70 };
        stfx_config(cpu, &c);
        CHECK(!stfx_frame(cpu, in, W * 4, W, H, out, W * 4), "%s: blur frame", name);
        int mw, mh;
        const float *mt = stfx_matte(cpu, &mw, &mh);
        long inter = 0, uni = 0;
        for (int i = 0; i < mw * mh; i++) { int a = mt[i] > 0.5f, b = ref[i] > 0.5f; inter += a && b; uni += a || b; }
        double iou = uni ? (double)inter / uni : 1;
        printf("  matte IoU against ONNX Runtime's %.3f\n", iou);
        CHECK(iou > 0.95, "%s: the matte's IoU is %.3f", name, iou);
        double dp, db;
        long np, nb;
        region_diff(in, out, ref, 1, &dp, &np);
        region_diff(in, out, ref, 0, &db, &nb);
        double din = bg_detail(in, ref), dout = bg_detail(out, ref);
        printf("  blur: the person moved %.2f levels (%ld px), the background %.2f (%ld px); its detail %.2f → %.2f\n", dp, np, db, nb, din, dout);
        CHECK(np < 1000 || dp < 4, "%s: the person changed by %.2f levels", name, dp);
        CHECK(nb < 1000 || din < 0.05 || dout < din * 0.6, "%s: the background kept its detail (%.2f → %.2f)", name, din, dout);
        save(name, "blur", out);
        /* the same with the mirror: exactly the mirror image */
        c.mirror = 1;
        StFx *cpu2 = stfx_new(model, ml, -1, err, sizeof err);
        stfx_config(cpu2, &c);
        stfx_frame(cpu2, in, W * 4, W, H, out2, W * 4);
        mirrored = 1;
        for (int y = 0; y < H && mirrored; y += 5) for (int x = 0; x < W; x++)
            if (memcmp(out2 + ((size_t)y * W + x) * 4, out + ((size_t)y * W + W - 1 - x) * 4, 4)) { mirrored = 0; break; }
        CHECK(mirrored, "%s: the mirrored blur is not the blur's mirror", name);
        stfx_free(cpu2);
        if (gpu) {
            c.mirror = 0;
            stfx_reset(gpu);
            stfx_config(gpu, &c);
            CHECK(!stfx_frame(gpu, in, W * 4, W, H, gout, W * 4), "%s: GPU frame", name);
            StStats st;
            stfx_stats(gpu, &st);
            CHECK(st.gpu, "%s: the GPU path fell back to the CPU", name);
            StFx *fresh = stfx_new(model, ml, -1, err, sizeof err);      /* the same history: this one frame */
            stfx_config(fresh, &c);
            stfx_frame(fresh, in, W * 4, W, H, out2, W * 4);
            stfx_free(fresh);
            compare_cpu_gpu("blur", out2, gout);
            save(name, "blur-gpu", gout);
        }
        /* picture */
        c = (StCfg){ .mode = ST_MODE_PICTURE };
        stfx_config(cpu, &c);
        CHECK(!stfx_background(cpu, pic, 640, 360, 640 * 4), "picture");
        stfx_frame(cpu, in, W * 4, W, H, out, W * 4);
        /* in sure background, the output is the gradient (scaled to the frame) */
        double s = 0;
        long n = 0;
        for (int y = 0; y < H; y += 3) for (int x = 0; x < W; x += 3) {
            if (ref[(y * STUDIO_SEG_H / H) * STUDIO_SEG_W + x * STUDIO_SEG_W / W] > 0.02f) continue;
            const uint8_t *o = out + ((size_t)y * W + x) * 4, *p = pic + ((size_t)(y * 360 / H) * 640 + x * 640 / W) * 4;
            s += abs(o[0] - p[0]) + abs(o[1] - p[1]) + abs(o[2] - p[2]);
            n += 3;
        }
        printf("  picture: the background is the picture within %.2f levels (%ld px)\n", n ? s / n : 0.0, n / 3);
        CHECK(n < 300 || s / n < 6, "%s: the background isn't the picture (%.2f)", name, s / n);
        region_diff(in, out, ref, 1, &dp, &np);
        CHECK(np < 1000 || dp < 4, "%s: the person changed by %.2f levels over the picture", name, dp);
        save(name, "picture", out);
        if (gpu) {
            /* the picture chosen while the blur is on, then the picture mode: the GPU must upload it then */
            StCfg blur = { .mode = ST_MODE_BLUR, .blur = 40 };
            stfx_config(gpu, &blur);
            stfx_background(gpu, pic, 640, 360, 640 * 4);
            stfx_frame(gpu, in, W * 4, W, H, gout, W * 4);
            stfx_reset(gpu);
            stfx_config(gpu, &c);
            stfx_frame(gpu, in, W * 4, W, H, gout, W * 4);
            StFx *fresh = stfx_new(model, ml, -1, err, sizeof err);
            stfx_config(fresh, &c);
            stfx_background(fresh, pic, 640, 360, 640 * 4);
            stfx_frame(fresh, in, W * 4, W, H, out2, W * 4);
            stfx_free(fresh);
            compare_cpu_gpu("picture", out2, gout);
            save(name, "picture-gpu", gout);
        }
        free(in);
        free(ref);
    }
    /* still frames: the matte settles and stays finite */
    {
        uint8_t *in = frame_of("f08");
        StCfg c = { .mode = ST_MODE_BLUR, .blur = 30 };
        StFx *s = stfx_new(model, ml, -1, err, sizeof err);
        stfx_config(s, &c);
        float prev[STUDIO_SEG_W * STUDIO_SEG_H];
        double change = 0;
        for (int i = 0; i < 8; i++) {
            stfx_frame(s, in, W * 4, W, H, out, W * 4);
            const float *m = stfx_matte(s, NULL, NULL);
            change = 0;
            int finite = 1;
            for (int k = 0; k < STUDIO_SEG_W * STUDIO_SEG_H; k++) { if (!isfinite(m[k])) finite = 0; change += fabs(m[k] - prev[k]); prev[k] = m[k]; }
            CHECK(finite, "the matte is not finite");
        }
        printf("still frames: the matte changes by %.4f after 8 frames\n", change / (STUDIO_SEG_W * STUDIO_SEG_H));
        CHECK(change / (STUDIO_SEG_W * STUDIO_SEG_H) < 1e-3, "the matte does not settle on a still frame");
        stfx_free(s);
        free(in);
    }
    if (timing) {
        uint8_t *in = frame_of("f04");
        StFx *paths[2] = { cpu, gpu };
        for (int p = 0; p < 2; p++) {
            if (!paths[p]) continue;
            StCfg c = { .mode = ST_MODE_BLUR, .blur = 50 };
            stfx_config(paths[p], &c);
            double seg[60], ref[60], comp[60], tot[60];
            for (int i = -5; i < 60; i++) {
                double t0 = now_ms();
                stfx_frame(paths[p], in, W * 4, W, H, out, W * 4);
                double t = now_ms() - t0;
                StStats st;
                stfx_stats(paths[p], &st);
                if (i >= 0) { seg[i] = st.seg_ms; ref[i] = st.refine_ms; comp[i] = st.compose_ms; tot[i] = t; }
            }
            double *arr[4] = { seg, ref, comp, tot };
            for (int a = 0; a < 4; a++) for (int i = 1; i < 60; i++) { double v = arr[a][i]; int j = i - 1; while (j >= 0 && arr[a][j] > v) { arr[a][j + 1] = arr[a][j]; j--; } arr[a][j + 1] = v; }
            printf("time %s, %dx%d blur: segmenter (with the downscale) %.2f ms, matte + guided filter + blur %.2f ms, composite %.2f ms%s, total %.2f ms (medians of 60)\n",
                   p ? "GPU" : "CPU", W, H, seg[30], ref[30], comp[30], p ? " (upload, draw, read back)" : "", tot[30]);
        }
        free(in);
    }
    if (!gpu) printf("GPU path: skipped (no render node or no OpenGL ES 3)\n");
    stfx_free(cpu);
    stfx_free(gpu);
    if (rfd >= 0) close(rfd);
    free(out); free(out2); free(gout); free(pic); free(model);
    if (fails) { printf("studio-test: %d failures\n", fails); return 1; }
    printf("studio-test: ok\n");
    return 0;
}
