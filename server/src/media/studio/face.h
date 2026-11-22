/* face.h — BlazeFace short range's output decoded into faces (docs/CAMERA-STUDIO-PLAN.md 5.1).
 *
 * The network gives 896 anchors (a 16×16 grid with 2 anchors per cell, then an 8×8 grid with 6), each with 16
 * regressors (box centre and size, 6 keypoints) in input pixels and one score logit. The anchors are MediaPipe's
 * short-range SSD layout (strides 8, 16, 16, 16; fixed size), written here from the published configuration. Used by
 * Auto Framing, Face Light and Retouch (later phases); phase 1 only checks it in build/studio-nn-test. */
#ifndef LORD_STUDIO_FACE_H
#define LORD_STUDIO_FACE_H

#include <stddef.h>

#define FACE_ANCHORS 896
#define FACE_MAX     4

typedef struct {
    float score;                 /* 0..1 */
    float x, y, w, h;            /* box, normalised to the 128×128 input (centre and size) */
    float kp[6][2];              /* right eye, left eye, nose tip, mouth, right ear tragion, left ear tragion */
} Face;

/* regressors: 896×16 floats; scores: 896 logits. Writes up to max faces with score ≥ min_score, best first, after
 * non-maximum suppression (IoU 0.3). Returns how many. */
int face_decode(const float *regressors, const float *scores, float min_score, Face *out, int max);

#endif
