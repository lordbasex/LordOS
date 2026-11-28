/* nv12.h — NV12 (4:2:0) video frames in lord-server (docs/MEDIA.md, "Video frames"): the colour matrices, shared by the
 * GPU's shader (gpu.c gpu_yuv_convert) and the CPU's conversion below, which is used only where the CPU needs the
 * pixels of a frame the GPU shows (a screenshot, the strips under a blurred Dock or menu bar, a lost GPU context).
 * Pure code: no GPU, no media source (engine-test and nv12-test link it alone). */
#ifndef LORD_MEDIA_NV12_H
#define LORD_MEDIA_NV12_H

#include <stdint.h>

/* RGB (0..1) = m · (Y, Cb, Cr, 1) from the bytes (0..1, as a texture samples them): rows r, g, b of 4 numbers.
 * matrix: MC_601, MC_709 or MC_2020 (proto.h), anything else BT.601; full: full range. */
void nv12_matrix(int matrix, int full, float m[12]);

/* Rows [y0, y1) of a w × h NV12 frame into the same rows of out (ARGB words, alpha 255, row 0 at out, ostride words a
 * row). Planes as
 * mp_ring_layout gives them. Chroma nearest. Out-of-range rows are clamped; nothing is read outside the planes. */
void nv12_to_argb(const uint8_t *yp, int ystride, const uint8_t *uvp, int uvstride, int w, int h, int matrix, int full,
                  uint32_t *out, int ostride, int y0, int y1);

#endif
