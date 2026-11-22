/* stgl.h — LordOS Studio's GPU composite (see st.h): OpenGL ES 3 offscreen on the render node lord-server opened, with
 * GBM and a surfaceless EGL context, as lord-bench does (bench/lbgpu.c). It does step 4 of the pipeline (alpha from the
 * guided filter's coefficients, the background, the mix, the mirror) at full size and reads the result back. */
#ifndef LORD_STUDIO_STGL_H
#define LORD_STUDIO_STGL_H

#include <stdint.h>

#include "st.h"

typedef struct StGl StGl;

typedef struct {
    const uint8_t *frame; int stride, w, h;     /* BGRA in */
    const float *a, *b; int gw, gh;              /* guided filter coefficients */
    int mode;                                    /* ST_MODE_* */
    const float *blur; int bw, bh;               /* premultiplied RGBW, blurred (ST_MODE_BLUR) */
    const uint8_t *pic; int pw, ph, pstride;     /* picture (ST_MODE_PICTURE), cover-fitted by the shader */
    int pic_changed;                             /* 1: upload the picture again */
    int mirror;
    uint8_t *out; int ostride;                   /* BGRA out */
    /* phase 3's looks (st.h); all off when zero */
    int matte;                                   /* the matte is wanted (a background, or Studio Light) */
    int key;                                     /* ST_KEY_*: the green screen, with dil (gw × gh) */
    const float *dil;
    float keycbcr[2]; int similarity, smoothness; float spill;
    int match; float m[9], o[3];                 /* colour match: rgb' = m·rgb + o (0..255) */
    int light; int has_face; float face[4];      /* Studio Light 0..100; the face in the frame's pixels */
    const uint8_t *lut; int lut_changed; float lut_amt;   /* the filter's 3D LUT (ST_LUT_N³ RGB) or NULL */
} StGlJob;

#ifdef LORD_GPU
StGl *stgl_new(int render_fd);
void  stgl_free(StGl *g);
int   stgl_compose(StGl *g, const StGlJob *j);  /* 0 on success; -1: use the CPU */
#else
static inline StGl *stgl_new(int render_fd) { (void)render_fd; return 0; }
static inline void  stgl_free(StGl *g) { (void)g; }
static inline int   stgl_compose(StGl *g, const StGlJob *j) { (void)g; (void)j; return -1; }
#endif

#endif
