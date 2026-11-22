/* models.h — LordOS Studio's model files and their pinned SHA-256 (docs/CAMERA-STUDIO-PLAN.md 3.1, 4.1).
 *
 * They are MediaPipe's Selfie Segmenter (landscape) and BlazeFace short range (Google, Apache-2.0), converted by
 * tools/studio/tflite2lnn.mjs from the .tflite files kept in third_party/studio-models/ (sources, hashes and model
 * cards in its README.md). They are installed in <assets>/studio/models/. lord-server opens a model read-only,
 * checks its size and this hash, and only then passes the fd to lord-studio. A new model, or a new conversion, means
 * new hashes here, new reference outputs in tests/data/studio/ (tools/studio/ort-ref.c) and build/studio-nn-test
 * passing. */
#ifndef LORD_STUDIO_MODELS_H
#define LORD_STUDIO_MODELS_H

#define STUDIO_SEG_FILE   "selfie_segmenter_landscape.lnn"
#define STUDIO_SEG_SHA256 "78a166a32fe502e542f6c763716b3fca3bef97ae4e9e94b0ab8f5815e568fd96"
#define STUDIO_SEG_W      256
#define STUDIO_SEG_H      144

#define STUDIO_FACE_FILE   "blaze_face_short_range.lnn"
#define STUDIO_FACE_SHA256 "5ac671d02d2f6e6d1c669192b0db70138295cb3a1230755baea710f77a75bb72"
#define STUDIO_FACE_SIDE   128

#define STUDIO_MODEL_MAX  (1u << 20)         /* bytes: both are about 220 KB */

#endif
