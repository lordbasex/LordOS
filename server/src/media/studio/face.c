/* face.c — BlazeFace decoding (see face.h). */
#include "face.h"

#include <math.h>
#include <string.h>

static void anchor(int i, float *ax, float *ay) {
    if (i < 512) {                               /* stride 8: 16×16 cells, 2 anchors each */
        int cell = i / 2;
        *ax = ((float)(cell % 16) + 0.5f) / 16;
        *ay = ((float)(cell / 16) + 0.5f) / 16;
    } else {                                     /* stride 16 (three layers merged): 8×8 cells, 6 anchors each */
        int cell = (i - 512) / 6;
        *ax = ((float)(cell % 8) + 0.5f) / 8;
        *ay = ((float)(cell / 8) + 0.5f) / 8;
    }
}

static float iou(const Face *a, const Face *b) {
    float x0 = fmaxf(a->x - a->w / 2, b->x - b->w / 2), x1 = fminf(a->x + a->w / 2, b->x + b->w / 2);
    float y0 = fmaxf(a->y - a->h / 2, b->y - b->h / 2), y1 = fminf(a->y + a->h / 2, b->y + b->h / 2);
    if (x1 <= x0 || y1 <= y0) return 0;
    float in = (x1 - x0) * (y1 - y0), un = a->w * a->h + b->w * b->h - in;
    return un > 0 ? in / un : 0;
}

int face_decode(const float *reg, const float *scores, float min_score, Face *out, int max) {
    if (!reg || !scores || !out || max < 1) return 0;
    int n = 0;
    for (int pass = 0; pass < max; pass++) {
        /* the best anchor not yet covered by a face already taken */
        Face best = { 0 };
        int found = 0;
        for (int i = 0; i < FACE_ANCHORS; i++) {
            float logit = scores[i];
            if (!isfinite(logit)) continue;
            if (logit > 100) logit = 100;
            if (logit < -100) logit = -100;
            float s = 1.0f / (1.0f + expf(-logit));
            if (s < min_score || (found && s <= best.score)) continue;
            const float *r = reg + (size_t)i * 16;
            Face f;
            float ax, ay;
            anchor(i, &ax, &ay);
            f.score = s;
            f.x = r[0] / 128 + ax;
            f.y = r[1] / 128 + ay;
            f.w = r[2] / 128;
            f.h = r[3] / 128;
            if (!(f.w > 0 && f.h > 0 && f.w < 4 && f.h < 4 && isfinite(f.x) && isfinite(f.y))) continue;
            int covered = 0;
            for (int k = 0; k < n && !covered; k++) covered = iou(&out[k], &f) > 0.3f;
            if (covered) continue;
            for (int k = 0; k < 6; k++) { f.kp[k][0] = r[4 + 2 * k] / 128 + ax; f.kp[k][1] = r[5 + 2 * k] / 128 + ay; }
            best = f;
            found = 1;
        }
        if (!found) break;
        out[n++] = best;
    }
    return n;
}
