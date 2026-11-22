/* dmaread.h — a second EGL reader for dmabufs the desktop's GPU can't import (docs/MACHINES-GPU.md, Mode A).
 *
 * A Machines guest with an Intel GVT-g vGPU hands QEMU's display Y-tiled dmabufs made by the Intel iGPU; when the
 * desktop runs on another GPU (an AMD card), or on the CPU path (LORD_GPU=0, or no usable GPU), the desktop's EGL can't
 * import them. This opens the buffer's own render node with its own GBM device, EGL display and GLES2 context (no
 * surface: EGL_KHR_surfaceless_context), imports the dmabuf as an EGLImage and reads its rows back into the caller's
 * memory, as gpu_tex_read_rows does. Its context is never left current: whatever context was current before a call
 * (gpu.c's) is current again after it.
 *
 * Built without Mesa (no -DLORD_GPU) dmaread_import and dmaread_out_new return NULL, the rest -1. */
#ifndef LORD_GPU_DMAREAD_H
#define LORD_GPU_DMAREAD_H

#include <stdint.h>

typedef struct DmaImg DmaImg;           /* one imported dmabuf: its EGLImage and texture, kept until dmaread_free */

/* "/dev/dri/renderD<N>" of an Intel GPU (sysfs vendor 0x8086; one whose PCI device lists mdev_supported_types, GVT-g
 * on, is preferred), looked up once and cached; NULL if there is none. */
const char *dmaread_intel_node(void);
/* 1 if mod is an Intel vendor modifier (I915_FORMAT_MOD_*), 0 for linear, invalid and other vendors' */
int dmaread_mod_intel(uint64_t mod);
/* 1 if node is a "/dev/dri/renderD<N>" path (N 128..1023) */
int dmaread_node_valid(const char *node);

/* Imports a one-plane dmabuf (fd is only read: the caller keeps it, and keeps it open while the import lives) on the
 * render node node (dmaread_node_valid). The node's device and context are made on first use and kept; a node that
 * failed once isn't tried again. NULL if it can't be imported. */
DmaImg *dmaread_import(const char *node, int fd, int w, int h, int stride, uint32_t fourcc, uint64_t mod);
/* Rows [y, y + h) of the picture into px (w × h of the import, w * 4 bytes a row, rows top to bottom), as the B, G, R, X
 * bytes of x8r8g8b8 (X 255); flip: the buffer's first row is the picture's bottom. 0 on success, -1 on failure. */
int  dmaread_rows(DmaImg *im, uint8_t *px, int y, int h, int flip);
/* dmaread_rows with a change test: key names whose picture px is (a Machines session); while the reader's framebuffer
 * still holds that picture, the rows are first compared with it on the GPU (GL_EXT_occlusion_query_boolean) and not read
 * when nothing in them changed. 1 read, 0 the same (px untouched), -1 failure. key NULL: always read */
int  dmaread_rows_new(DmaImg *im, uint8_t *px, int y, int h, int flip, const void *key);
void dmaread_free(DmaImg *im);

/* Linear copies for the desktop's GPU (docs/MACHINES-GPU.md, "A vGPU's picture without a readback"): a buffer of the
 * import's size on its device (GBM, linear, rows a multiple of 256 bytes), exported as a dmabuf another GPU imports.
 * dmaread_copy draws rows [y, y + h) of src into it on the reader's GPU and only queues the work (glFlush, no wait): its
 * end is the write fence on the copy's dmabuf (poll() it for POLLIN). With prev (the copy shown now) the same pass also
 * tests whether those rows differ from prev's; dmaread_changed gives the answer once the copy is done: 1 changed, 0 the
 * same, -1 unknown (no test, or not ready: count it as changed). NULL / -1 where not possible. */
typedef struct DmaOut DmaOut;
DmaOut *dmaread_out_new(const DmaImg *like);
int  dmaread_out_fd(const DmaOut *o, int *stride);   /* the copy's dmabuf (the DmaOut keeps it), its row stride */
int  dmaread_copy(DmaImg *src, DmaOut *dst, int y, int h, int flip, const DmaOut *prev);
int  dmaread_changed(DmaOut *o);
void dmaread_out_free(DmaOut *o);
/* Frees every device and context (tests; lord-server keeps them for its lifetime). Imports must be freed first. */
void dmaread_close_all(void);

#endif
