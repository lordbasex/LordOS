/* gpu.h — GPU composition (Mesa's GBM + EGL + GLES2) and direct scanout (see gpu.c and
 * docs/GPU.md). If there is no usable GPU, gpu_init fails and lord-server keeps going by CPU as always. */
#ifndef LORD_GPU_H
#define LORD_GPU_H

#include <stdint.h>
#include <sys/types.h>
#include "../canvas.h"
#include "../kms.h"

typedef struct GpuTex GpuTex;

/* An app's dmabuf buffer (up to 4 planes). */
typedef struct {
    int width, height, nplanes;
    uint32_t fourcc;
    uint64_t modifier;                  /* DRM_FORMAT_MOD_INVALID = implicit */
    int fd[4];
    uint32_t offset[4], stride[4];
} GpuDmabuf;

typedef struct { uint32_t fourcc; uint64_t modifier; } GpuFormat;

int  gpu_init(Kms *k);                  /* 0 if it is active; LORD_GPU=0 turns it off */
int  gpu_active(void);
void gpu_shutdown(void);
const char *gpu_renderer(void);         /* "llvmpipe (LLVM …)", "AMD Radeon …" */

/* What System Information shows of the GPU path (docs/BENCHMARK.md, "The GPU as separate layers").
 * why: the last gpu_init's outcome, a fixed code: "on", "not-built" (built without Mesa), "env-off"
 * (LORD_GPU=0), "no-3d", "no-gbm", "no-egl", "no-config", "no-context", "software" (llvmpipe and
 * the like), "shaders", or "" (never tried). The rest is the live context's, "" or 0 without one
 * (the renderer also names the software one that was turned down);
 * scanouts counts the frames shown directly since lord-server started. */
typedef struct {
    const char *why, *renderer, *render_node;
    int dmabuf;
    unsigned long scanouts;
} GpuInfo;
void gpu_info(GpuInfo *i);

/* Formats/modifiers that can be imported (linux-dmabuf) and the DRM device. */
int  gpu_formats(GpuFormat *out, int max);
int  gpu_device(dev_t *dev);

/* Textures: from premultiplied ARGB pixels (shm) or from a dmabuf (EGLImage). */
GpuTex *gpu_tex_pixels(GpuTex *t, const uint32_t *px, int w, int h, int opaque);
GpuTex *gpu_tex_dmabuf(const GpuDmabuf *d);
void    gpu_tex_free(GpuTex *t);

/* GPU context lifetime. Each gpu_init that succeeds starts a new context ("generation"); a
 * texture made in an earlier one is stale: its GL name and EGLImage died with that context, so it
 * must never be drawn and gpu_tex_free only frees its memory. The session leaving and coming back
 * to the monitor, a remote viewer, a display change or a failed frame all re-create the context. */
int      gpu_tex_current(const GpuTex *t);  /* 1 if t belongs to the live context */
unsigned gpu_generation(void);              /* 0 while there is no GPU */
/* Hooks for the compositor: lost() runs inside gpu_shutdown with the old context still current
 * (last chance to read textures back or free them); changed() runs once the switch is done, after
 * a successful gpu_init and after gpu_shutdown (to re-import buffers and re-send dmabuf feedback). */
void gpu_set_hooks(void (*lost)(void), void (*changed)(void));
/* Reads a texture back as premultiplied ARGB pixels (t->w × t->h, rows top to bottom), for the
 * last good frame of a dmabuf window when the GPU goes away. 0 on success. */
int  gpu_tex_read(const GpuTex *t, uint32_t *px, int w, int h);

/* One frame: what changed in the UI canvas is uploaded, the layers are drawn and the flip is done.
 * draw_layer(0) = app windows (below the UI), draw_layer(1) = app menus and
 * fullscreen (on top). */
int  gpu_frame(int out, const Canvas *scene, Rect damage, void (*draw_layer)(int layer));

/* G0: the dragged window composited on the GPU with the CPU's own arithmetic (docs/GPU.md, "The
 * dragged window on the GPU"). paint_root_g0 (ui/paint.c) leaves the window's rectangle w out of the
 * scene and fills this; one shader then writes, inside w, the layer (lpx) over the copy of what is
 * below (bpx) exactly as canvas_blit's blit_row does. The two are textures of their own, uploaded
 * only where they changed (ldirty, bdirty; *full: all of it). */
#define G0_DIRTY 8
typedef struct GpuG0 {
    Rect w;                             /* where the shader writes (the output's physical px, top-down) */
    const uint32_t *lpx;                /* the layer: lw × lh, its pixel (0, 0) at (lx, ly) */
    int lw, lh, lx, ly;
    const uint32_t *bpx;                /* the copy of what is below: bw × bh, the scene's size */
    int bw, bh;
    Rect ldirty[G0_DIRTY], bdirty[G0_DIRTY];    /* in the layer's and the copy's pixels */
    int nl, nb, lfull, bfull;
    Rect strip[4];                      /* what the CPU painted around w this frame (uploaded to the UI texture) */
    int nstrip;
    const uint32_t *ref;                /* LORD_GPU_VERIFY: the CPU's pixels for w (w's width a row), or NULL */
    long frame;                         /* its number, for the log */
} GpuG0;
/* 1 if G0 may be used on that output: the shader has 24-bit floats, LORD_G0 is not 0, the self-test
 * (every valid premultiplied combination, and a placement pass at least as large as the output, drawn
 * into a framebuffer and read back) gave the CPU's pixels. gpu_init runs that test, once per GL renderer
 * and version. */
int  gpu_g0_ready(int out);
/* gpu_frame with several UI rectangles to upload and, with g0, the window composited over w (g0
 * without dirty rectangles redraws the last one: a frame with no UI damage while the scene's w is
 * stale). Returns -1 like gpu_frame; *g0_drawn says whether the G0 quad was drawn. -2: g0 was given but
 * its quad could not be drawn (G0 turned off, or its textures are not that output's): nothing was
 * shown, and the caller paints w on the CPU in its next frame. */
int  gpu_frame_ex(int out, const Canvas *scene, const Rect *dmg, int n, const GpuG0 *g0, int *g0_drawn,
                  void (*draw_layer)(int layer));
int  gpu_g0_holds(int out);             /* the G0 textures hold that output's last upload, in this context */
/* uploaded pixels of the copy of what is below and of the layer, and G0 quads drawn, since the last call */
void gpu_g0_stats(long *below_px, long *layer_px, long *quads);
/* Another display (docs/DISPLAYS.md): its own GBM/EGL surface on the same context; gpu_init made
 * output 0. Returns the output index for gpu_frame, or -1 (then that display goes by the CPU). */
int  gpu_add_output(Kms *k);
void gpu_draw(GpuTex *t, float dx, float dy, float dw, float dh,
              float sx, float sy, float sw, float sh, Rect clip, float opacity);

/* The remote desktop's copy of the next composed frame (docs/REMOTE.md, "Video (WebRTC)"): the
 * next gpu_frame of a w×h display reads the whole frame back into px (0xAARRGGBB, rows top to
 * bottom) before showing it; gpu_read_done says whether it did. */
void gpu_read_next(uint32_t *px, int w, int h);
int  gpu_read_done(void);

/* Direct scanout of a fullscreen dmabuf (without composing). done(data) is called when
 * the buffer leaves the screen. 0 if the flip was sent. */
int  gpu_scanout(const GpuDmabuf *d, void (*done)(void *), void *data);
/* Can it be shown directly? (output size, format and primary plane modifier) */
int  gpu_can_scanout(const GpuDmabuf *d, int w, int h);

#endif
