/* nn.h — LordOS Studio's own neural network inference (docs/CAMERA-STUDIO-PLAN.md 4.2, decision D3).
 *
 * It runs exactly two models, MediaPipe's Selfie Segmenter (landscape, 144×256) and BlazeFace short range (128×128),
 * converted once by tools/studio/tflite2lnn.mjs into our flat ".lnn" file. Only the operators those two models use
 * exist here; anything else fails closed when the file is loaded. The file comes from our own package with its
 * SHA-256 checked by lord-server (models.h), but the loader is still written as if it were hostile: every count,
 * shape, offset and value is bounded before it is used, and the run never reads or writes outside its own arena.
 * Float only (f16 weights widened to f32 at load), NHWC, batch 1, one thread. No library. */
#ifndef LORD_STUDIO_NN_H
#define LORD_STUDIO_NN_H

#include <stddef.h>

/* The .lnn layout (tools/studio/tflite2lnn.mjs writes it) */
#define LNN_MAGIC        "LNN1"
#define LNN_VERSION      1
#define LNN_HEAD_BYTES   64
#define LNN_TENSOR_BYTES 32
#define LNN_OP_BYTES     64
#define LNN_MAX_TENSORS  1024
#define LNN_MAX_OPS      1024
#define LNN_MAX_HALFS    (4u << 20)          /* 4 Mi weights (8 MB): both models have about 100 k */
#define LNN_MAX_DIM      4096
#define LNN_MAX_ELEMS    (16u << 20)         /* per tensor */
#define LNN_MAX_ARENA    (64u << 20)         /* bytes of activations at once */
#define LNN_MAX_OUTPUTS  4

enum { LNN_ACTIVATION = 1, LNN_CONSTANT = 2 };
enum {
    LNN_CONV = 1, LNN_DWCONV, LNN_ADD, LNN_MUL, LNN_MEAN, LNN_RESIZE, LNN_TCONV, LNN_PAD, LNN_MAXPOOL, LNN_RESHAPE,
    LNN_CONCAT, LNN_RELU, LNN_HSWISH, LNN_SIGMOID, LNN_OP_LAST = LNN_SIGMOID
};
enum { LNN_ACT_NONE = 0, LNN_ACT_RELU, LNN_ACT_HSWISH, LNN_ACT_SIGMOID };
enum { LNN_PAD_SAME = 0, LNN_PAD_VALID = 1 };

typedef struct Lnn Lnn;

/* Loads a model from memory (the bytes are copied where needed; data may be freed afterwards). NULL on any problem,
 * with a short English reason in err (for the log; never shown to the person). */
Lnn *lnn_load(const void *data, size_t len, char *err, size_t errlen);
void lnn_free(Lnn *m);
/* The input tensor (NHWC, batch 1) to fill before lnn_run: its size in h, w, c. */
float *lnn_input(Lnn *m, int *h, int *w, int *c);
/* Runs the network. 0 on success. */
int lnn_run(Lnn *m);
/* Output k (0..n-1): its values (NHWC) and count; NULL past the last. Valid until the next run. */
const float *lnn_output(const Lnn *m, int k, size_t *count);
int lnn_outputs(const Lnn *m);
/* Total multiply-adds of one run (for the log and the tests). */
double lnn_macs(const Lnn *m);

/* float16 → float32 (IEEE 754 half), exposed for the tests */
float lnn_half(unsigned short h);

#endif
