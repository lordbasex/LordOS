/* mcursor.h — the pointer image of a media node's source (a Machines guest's own cursor), see mcursor.c. */
#ifndef LORD_MCURSOR_H
#define LORD_MCURSOR_H

#include <stdint.h>

#define MCUR_MAX_IN  256        /* VD_MAX_CURSOR */
#define MCUR_MAX_OUT 64         /* the cursor plane (GPU path) takes 64 × 64 at most */

typedef struct {
    uint32_t *px;               /* premultiplied ARGB */
    int w, h, hx, hy;           /* size and hotspot */
} McurImage;

/* B, G, R, A bytes with straight alpha (vmproto.h, VD_CURSOR), w × h, hotspot inside → *out: premultiplied, cut to
 * what isn't fully transparent (the hotspot kept inside). 0 ok, 1 nothing visible (a hidden pointer), -1 refused. */
int mcur_prepare(const uint8_t *bgra, int w, int h, int hx, int hy, McurImage *out);
/* The factor to draw a w × h cursor at the picture's scale f, at most max pixels on its long side (0: no limit),
 * 1 when f is within 5 % of it. */
float mcur_fit_scale(int w, int h, float f, int max);
/* in scaled by f (bilinear), hotspot moved with it. 0 ok. */
int mcur_scale(const McurImage *in, float f, McurImage *out);
void mcur_free(McurImage *m);

#endif
