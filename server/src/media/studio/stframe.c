/* stframe.c — Center Stage's pieces (see st.h; docs/CAMERA-STUDIO-PLAN.md 5.2): the 1€ filter that smooths the crop,
 * the crop that frames a face and the bilinear crop-and-scale. The face itself comes from BlazeFace (stfx.c). */
#include "st.h"

#include <math.h>
#include <string.h>

static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }

/* G. Casiez, N. Roussel and D. Vogel, "1€ Filter: A Simple Speed-based Low-pass Filter for Noisy Input in Interactive
 * Systems", CHI 2012: a low-pass whose cutoff rises with the speed, so slow drift is smoothed and real moves follow. */
static float euro_alpha(float cutoff, float dt) {
    float tau = 1.0f / (2 * 3.14159265f * cutoff);
    return 1.0f / (1.0f + tau / dt);
}

float st_euro(StEuro *f, float x, float dt) {
    if (!f->primed || !(dt > 0)) { f->x = x; f->dx = 0; f->primed = 1; return x; }
    float dx = (x - f->x) / dt;
    float ad = euro_alpha(f->dcut > 0 ? f->dcut : 1.0f, dt);
    f->dx += (dx - f->dx) * ad;
    float cut = (f->mincut > 0 ? f->mincut : 1.0f) + f->beta * fabsf(f->dx);
    f->x += (x - f->x) * euro_alpha(cut, dt);
    return f->x;
}

void st_frame_target(const float face[4], float eyes_y, int W, int H, int tight, StRect *out) {
    float k = clampf((float)tight, 1, 100) / 100;
    /* the crop's height in BlazeFace box heights (its box runs from the brows to the chin): about 4.2 wide (head,
     * shoulders and room), 2 tight (head and neck) */
    float ch = face[3] * (4.2f - 2.2f * k);
    float minh = H * 0.5f;                              /* at most 2× (plan 5.2: 1.5× is lossless from 1080p to 720p) */
    ch = clampf(ch, minh, (float)H);
    float cw = ch * W / H;
    /* the shoulders: about 2.2 face widths, with room on both sides */
    if (cw < face[2] * 3.2f) { cw = clampf(face[2] * 3.2f, cw, (float)W); ch = cw * H / W; }
    float cx = face[0];
    float top = eyes_y - ch / 3;                        /* the eyes at a third of the height */
    out->w = cw;
    out->h = ch;
    out->x = clampf(cx - cw / 2, 0, W - cw);
    out->y = clampf(top, 0, H - ch);
}

void st_crop_scale(const uint8_t *src, int sstride, int W, int H, const StRect *r, uint8_t *dst, int dstride) {
    float sx = r->w / W, sy = r->h / H;
    /* columns: 16.16 fixed point positions, clamped inside the source */
    enum { MAXW = 3840 };
    int xi[MAXW];
    uint16_t xf[MAXW];
    if (W > MAXW) return;
    for (int x = 0; x < W; x++) {
        float fx = clampf(r->x + (x + 0.5f) * sx - 0.5f, 0, (float)(W - 1));
        int i = (int)fx;
        if (i > W - 2) i = W - 2;
        xi[x] = i;
        xf[x] = (uint16_t)clampf((fx - (float)i) * 256, 0, 256);
    }
    for (int y = 0; y < H; y++) {
        float fy = clampf(r->y + (y + 0.5f) * sy - 0.5f, 0, (float)(H - 1));
        int j = (int)fy;
        if (j > H - 2) j = H - 2;
        unsigned wy = (unsigned)clampf((fy - (float)j) * 256, 0, 256);
        const uint8_t *r0 = src + (size_t)j * sstride, *r1 = r0 + sstride;
        uint8_t *o = dst + (size_t)y * dstride;
        for (int x = 0; x < W; x++) {
            const uint8_t *a = r0 + (size_t)xi[x] * 4, *c = r1 + (size_t)xi[x] * 4;
            unsigned wx = xf[x];
            for (int k = 0; k < 3; k++) {
                unsigned t = a[k] * (256 - wx) + a[4 + k] * wx, u = c[k] * (256 - wx) + c[4 + k] * wx;
                o[(size_t)x * 4 + k] = (uint8_t)((t * (256 - wy) + u * wy + 32768) >> 16);
            }
            o[(size_t)x * 4 + 3] = 255;
        }
    }
}
