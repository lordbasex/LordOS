/* ort-ref.c — reference outputs for build/studio-nn-test, computed by ONNX Runtime on the dev machine only
 * (docs/CAMERA-STUDIO-PLAN.md 4.2: the way tests/hb_compare.py checks shaping against HarfBuzz). LordOS never links
 * ONNX Runtime (decision D3); this tool is not built by server/Makefile and is not installed.
 *
 * Build (Debian 13: libonnxruntime1.21 installed; the headers from libonnxruntime-dev, `apt-get download` + dpkg -x
 * is enough, nothing needs installing):
 *   cc -O2 -I<headers>/onnxruntime -isystem third_party/stb -o ort-ref tools/studio/ort-ref.c \
 *      /usr/lib/x86_64-linux-gnu/libonnxruntime.so.1.21 -lm
 * Run (the .onnx files come from tools/studio/tflite2lnn.mjs --onnx):
 *   ./ort-ref seg  seg.onnx  out_dir frame.png...      → out_dir/<frame>.seg.ref  (144×256 u16, value × 65535)
 *   ./ort-ref face face.onnx out_dir frame.png...      → out_dir/<frame>.face.ref (896×16 + 896 float16)
 *   ./ort-ref time seg.onnx|face.onnx runs             → median ms of one run, one thread
 * Inputs: 8-bit RGB PNGs of the model's own size; seg takes RGB/255, face RGB/127.5 − 1 (MediaPipe's own ranges). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <onnxruntime_c_api.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const OrtApi *ort;

#define CHECK(x) do { OrtStatus *st_ = (x); if (st_) { fprintf(stderr, "ort: %s\n", ort->GetErrorMessage(st_)); exit(1); } } while (0)

static unsigned short to_half(float f) {
    unsigned int u;
    memcpy(&u, &f, 4);
    unsigned int s = (u >> 16) & 0x8000, e = (u >> 23) & 255, m = u & 0x7fffff;
    if (e == 255) return (unsigned short)(s | 0x7c00 | (m ? 0x200 : 0));
    int ee = (int)e - 127 + 15;
    if (ee >= 31) return (unsigned short)(s | 0x7c00);
    if (ee <= 0) {
        if (ee < -10) return (unsigned short)s;
        m |= 0x800000;
        int shift = 14 - ee;
        unsigned int h = m >> shift, rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1))) h++;
        return (unsigned short)(s | h);
    }
    unsigned int h = (unsigned int)ee << 10 | m >> 13, rem = m & 0x1fff;
    if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) h++;
    return (unsigned short)(s | h);
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: ort-ref seg|face model.onnx out_dir frame.png... | ort-ref time model.onnx runs\n"); return 2; }
    ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    OrtEnv *env;
    CHECK(ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "ref", &env));
    OrtSessionOptions *so;
    CHECK(ort->CreateSessionOptions(&so));
    CHECK(ort->SetIntraOpNumThreads(so, 1));
    CHECK(ort->SetInterOpNumThreads(so, 1));
    OrtSession *sess;
    CHECK(ort->CreateSession(env, argv[2], so, &sess));
    OrtMemoryInfo *mi;
    CHECK(ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mi));
    int face = strstr(argv[2], "face") != NULL;
    int64_t shape[4] = { 1, face ? 128 : 144, face ? 128 : 256, 3 };
    size_t n_in = (size_t)shape[1] * shape[2] * 3;
    float *in = calloc(n_in, sizeof(float));
    const char *in_names[] = { "input" };
    const char *out_names[] = { "output0", "output1" };
    int nout = face ? 2 : 1;
    if (!strcmp(argv[1], "time")) {
        int runs = atoi(argv[3]);
        if (runs < 1) runs = 50;
        for (size_t k = 0; k < n_in; k++) in[k] = (float)((k * 2654435761u) % 255) / 255.0f;
        double *t = calloc((size_t)runs, sizeof(double));
        for (int r = -5; r < runs; r++) {
            OrtValue *iv = NULL, *ov[2] = { NULL, NULL };
            CHECK(ort->CreateTensorWithDataAsOrtValue(mi, in, n_in * sizeof(float), shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &iv));
            double t0 = now_ms();
            CHECK(ort->Run(sess, NULL, in_names, (const OrtValue *const *)&iv, 1, out_names, (size_t)nout, ov));
            if (r >= 0) t[r] = now_ms() - t0;
            ort->ReleaseValue(iv);
            for (int k = 0; k < nout; k++) ort->ReleaseValue(ov[k]);
        }
        for (int a = 1; a < runs; a++) { double v = t[a]; int b = a - 1; while (b >= 0 && t[b] > v) { t[b + 1] = t[b]; b--; } t[b + 1] = v; }
        printf("%s: median %.3f ms, min %.3f ms (%d runs, 1 thread)\n", argv[2], t[runs / 2], t[0], runs);
        return 0;
    }
    for (int a = 4; a < argc; a++) {
        int w, h, ch;
        unsigned char *px = stbi_load(argv[a], &w, &h, &ch, 3);
        if (!px || w != shape[2] || h != shape[1]) { fprintf(stderr, "%s: needs a %lldx%lld PNG\n", argv[a], (long long)shape[2], (long long)shape[1]); return 1; }
        for (size_t k = 0; k < n_in; k++) in[k] = face ? px[k] / 127.5f - 1.0f : px[k] / 255.0f;
        stbi_image_free(px);
        OrtValue *iv = NULL, *ov[2] = { NULL, NULL };
        CHECK(ort->CreateTensorWithDataAsOrtValue(mi, in, n_in * sizeof(float), shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &iv));
        CHECK(ort->Run(sess, NULL, in_names, (const OrtValue *const *)&iv, 1, out_names, (size_t)nout, ov));
        const char *base = strrchr(argv[a], '/');
        base = base ? base + 1 : argv[a];
        char path[1024];
        snprintf(path, sizeof path, "%s/%.*s.%s.ref", argv[3], (int)(strlen(base) - 4), base, face ? "face" : "seg");
        FILE *f = fopen(path, "wb");
        if (!f) { perror(path); return 1; }
        for (int k = 0; k < nout; k++) {
            float *o;
            CHECK(ort->GetTensorMutableData(ov[k], (void **)&o));
            size_t n = face ? (k == 0 ? 896 * 16 : 896) : 144 * 256;
            for (size_t i = 0; i < n; i++) {
                unsigned short v;
                if (face) v = to_half(o[i]);
                else { float c = o[i] < 0 ? 0 : o[i] > 1 ? 1 : o[i]; v = (unsigned short)lrintf(c * 65535.0f); }
                unsigned char b2[2] = { (unsigned char)(v & 255), (unsigned char)(v >> 8) };
                fwrite(b2, 1, 2, f);
            }
            ort->ReleaseValue(ov[k]);
        }
        fclose(f);
        ort->ReleaseValue(iv);
        printf("%s\n", path);
    }
    return 0;
}
