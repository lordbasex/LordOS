# LordOS Studio's models (MediaPipe, Apache-2.0)

LordOS Studio (docs/CAMERA-STUDIO-PLAN.md) runs two of Google's MediaPipe models with LordOS's own inference code
(`server/src/media/studio/nn.c`). The original files are kept here unmodified, with their model cards; LordOS ships the
converted copies (`assets/studio/models/*.lnn`), whose SHA-256 is pinned in `server/src/media/studio/models.h`.

| File | What | Source (downloaded 2026-11-22) | Bytes | SHA-256 |
|---|---|---|---|---|
| `selfie_segmenter_landscape.tflite` | MediaPipe Selfie Segmenter, landscape, float16; input 144×256 RGB 0..1, output the person's probability 144×256 | https://storage.googleapis.com/mediapipe-models/image_segmenter/selfie_segmenter_landscape/float16/1/selfie_segmenter_landscape.tflite (the same bytes as `latest/`) | 250177 | `490e9ea734313e0de10fa0cd9e3c6133e36ea4db2b7a49bde9ef019f72796b8e` |
| `blaze_face_short_range.tflite` | MediaPipe BlazeFace short range, float16; input 128×128 RGB −1..1, output 896 anchors × 16 regressors and 896 scores | https://storage.googleapis.com/mediapipe-models/face_detector/blaze_face_short_range/float16/1/blaze_face_short_range.tflite (the same bytes as `latest/`) | 229746 | `b4578f35940bf5a1a655214a1cce5cab13eba73c1297cd78e1a04c2380b0152f` |
| `model-card-selfie-segmentation.pdf` | Google's model card "MediaPipe Selfie Segmentation" (general and landscape models; "LICENSED UNDER Apache License, Version 2.0") | https://storage.googleapis.com/mediapipe-assets/Model%20Card%20MediaPipe%20Selfie%20Segmentation.pdf | 2286837 | `ef2e8d350891ba71699ac1e079658e697fc8d48c0f9253922990e907ecdade60` |
| `model-card-blazeface-short-range.pdf` | Google's model card "MediaPipe BlazeFace Model Card (Short Range)" ("LICENSED UNDER Apache License, Version 2.0") | https://storage.googleapis.com/mediapipe-assets/MediaPipe%20BlazeFace%20Model%20Card%20(Short%20Range).pdf | 434425 | `cd335c06fc0de7807cd815a0777a697932598bcdb28fa98adaaabf847485f758` |
| `LICENSE` | the Apache License, Version 2.0 (Debian's `/usr/share/common-licenses/Apache-2.0`) | | 11358 | `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30` |

**License.** Both models are © Google LLC, released under the Apache License, Version 2.0 (`LICENSE`), as their model
cards state. The model cards also say what each model is for and is not for; the Selfie Segmenter's lists as out of
scope several people at very different scales, people farther than about 4 m from the camera, and any form of
surveillance or identity recognition. LordOS uses them only on the person's own camera, on the person's computer.

**Modification notice (Apache-2.0 §4(b)).** `assets/studio/models/selfie_segmenter_landscape.lnn` and
`blaze_face_short_range.lnn` are derived from the files above by `tools/studio/tflite2lnn.mjs` (LordOS, MIT): the same
float16 weights, repacked into LordOS's own flat format; the float16-to-float32 DEQUANTIZE steps are folded into the
weights' type, and each ReLU, hard-swish or sigmoid that follows a convolution or an addition is folded into it. No
weight value is changed.

| Converted file | SHA-256 (pinned in `server/src/media/studio/models.h`) |
|---|---|
| `assets/studio/models/selfie_segmenter_landscape.lnn` | `78a166a32fe502e542f6c763716b3fca3bef97ae4e9e94b0ab8f5815e568fd96` |
| `assets/studio/models/blaze_face_short_range.lnn` | `5ac671d02d2f6e6d1c669192b0db70138295cb3a1230755baea710f77a75bb72` |

To convert again: `node tools/studio/tflite2lnn.mjs third_party/studio-models/<model>.tflite assets/studio/models/<model>.lnn`.
A new model or a new conversion means new hashes in `models.h`, new reference outputs (`tools/studio/ort-ref.c`, on the
dev machine) and `build/studio-nn-test` passing.
