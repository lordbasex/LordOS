/* gpu.c — lord-server's GPU composition: Mesa's GBM + EGL + GLES2 (approved by the
 * user on 2026-10-16; see docs/GPU.md).
 *
 * Modesetting is still our own (kms.c, ioctls without libdrm): GBM is created on the same
 * card fd and its buffers become framebuffers with DRM_IOCTL_MODE_ADDFB2.
 *
 * Each frame: the LordOS UI (the canvas the CPU paints) is a fullscreen texture to which
 * only what changed is uploaded. App windows are drawn below it (textures of their shm or
 * dmabuf buffers, without going through the CPU) and app menus on top; where there is an
 * app window, the UI leaves a transparent hole (alpha 0). A fullscreen app with a compatible
 * buffer goes straight to the primary plane: zero copies (gpu_scanout).
 *
 * By default it only turns on with a real GPU: with a software renderer (llvmpipe,
 * softpipe) our own CPU path is faster. LORD_GPU=1 forces it (tests), LORD_GPU=0
 * turns it off, LORD_GPU_DEBUG=1 reports what it does. Built without Mesa (no -DLORD_GPU) these are stubs. */
#include "gpu.h"
#include "gscissor.h"
#include "../outputs.h"
#include "../media/nv12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LORD_GPU

int  gpu_init(Kms *k) { (void)k; return -1; }
int  gpu_active(void) { return 0; }
void gpu_shutdown(void) {}
const char *gpu_renderer(void) { return ""; }
void gpu_info(GpuInfo *i) { *i = (GpuInfo){ .why = "not-built", .renderer = "", .render_node = "" }; }
int  gpu_formats(GpuFormat *out, int max) { (void)out; (void)max; return 0; }
int  gpu_device(dev_t *dev) { (void)dev; return -1; }
GpuTex *gpu_tex_pixels(GpuTex *t, const uint32_t *px, int w, int h, int opaque) { (void)t; (void)px; (void)w; (void)h; (void)opaque; return NULL; }
GpuTex *gpu_tex_pixels_rect(GpuTex *t, const uint32_t *px, int w, int h, int opaque, Rect r) { (void)t; (void)px; (void)w; (void)h; (void)opaque; (void)r; return NULL; }
GpuTex *gpu_tex_dmabuf(const GpuDmabuf *d) { (void)d; return NULL; }
void gpu_tex_free(GpuTex *t) { (void)t; }
int  gpu_tex_width(const GpuTex *t) { (void)t; return 0; }
int  gpu_tex_current(const GpuTex *t) { (void)t; return 0; }
unsigned gpu_generation(void) { return 0; }
void gpu_set_hooks(void (*lost)(void), void (*changed)(void)) { (void)lost; (void)changed; }
int  gpu_tex_read(const GpuTex *t, uint32_t *px, int w, int h) { (void)t; (void)px; (void)w; (void)h; return -1; }
int  gpu_tex_read_rows(const GpuTex *t, uint8_t *px, int y, int h, int flip) { (void)t; (void)px; (void)y; (void)h; (void)flip; return -1; }
GpuYuv *gpu_yuv_upload(GpuYuv *t, const uint8_t *y, int ys, const uint8_t *uv, int uvs, int w, int h) { (void)t; (void)y; (void)ys; (void)uv; (void)uvs; (void)w; (void)h; return NULL; }
int  gpu_yuv_convert(const GpuYuv *src, GpuTex **dst, int matrix, int full) { (void)src; (void)dst; (void)matrix; (void)full; return -1; }
int  gpu_yuv_convert_to(const GpuYuv *src, GpuTex **dst, int w, int h, int matrix, int full) { (void)src; (void)dst; (void)w; (void)h; (void)matrix; (void)full; return -1; }
int  gpu_yuv_current(const GpuYuv *t) { (void)t; return 0; }
long gpu_yuv_bytes(const GpuYuv *t) { (void)t; return 0; }
void gpu_yuv_free(GpuYuv *t) { (void)t; }
GpuYuv *gpu_yuv_import(const GpuDmabuf *y, const GpuDmabuf *uv) { (void)y; (void)uv; return NULL; }
int  gpu_dmabuf_plane_ok(uint32_t fourcc, uint64_t modifier) { (void)fourcc; (void)modifier; return 0; }
GpuPack *gpu_yuv_pack(const GpuYuv *src) { (void)src; return NULL; }
int  gpu_pack_convert(const GpuPack *k, GpuTex **dst, int matrix, int full) { (void)k; (void)dst; (void)matrix; (void)full; return -1; }
int  gpu_pack_current(const GpuPack *k) { (void)k; return 0; }
long gpu_pack_bytes(const GpuPack *k) { (void)k; return 0; }
void gpu_pack_free(GpuPack *k) { (void)k; }
int  gpu_pack_ready(void) { return 0; }
int  gpu_init_offscreen(int render_fd) { (void)render_fd; return -1; }
void gpu_offscreen_finish(void) {}
int  gpu_add_output(Kms *k) { (void)k; return -1; }
int  gpu_frame(int out, const Canvas *s, Rect d, void (*f)(int)) { (void)out; (void)s; (void)d; (void)f; return -1; }
int  gpu_gdrag_ready(int out) { (void)out; return 0; }
void gpu_gdrag_max(int *lw, int *lh) { *lw = *lh = 0; }
void gpu_scissor_quiet(int out, int quiet) { (void)out; (void)quiet; }
int  gpu_frame_ex(int out, const Canvas *s, const Rect *d, int n, const GpuDrag *g, int *drawn, void (*f)(int)) {
    (void)out; (void)s; (void)d; (void)n; (void)g; (void)f;
    if (drawn) *drawn = 0;
    return -1;
}
int  gpu_gdrag_holds(int out) { (void)out; return 0; }
void gpu_gdrag_stats(long *b, long *l, long *q) { *b = *l = *q = 0; }
int  gpu_gdrag_feed(int out, int which, const uint32_t *px, int w, int h, Rect r) {
    (void)out; (void)which; (void)px; (void)w; (void)h; (void)r;
    return -1;
}
void gpu_frame_times(int out, GpuFrameTimes *t) { (void)out; *t = (GpuFrameTimes){ 0 }; }
void gpu_fence_on(int on) { (void)on; }
int  gpu_fence_time(int out, uint64_t *ns) { (void)out; *ns = 0; return 0; }
void gpu_draw(GpuTex *t, float dx, float dy, float dw, float dh, float sx, float sy, float sw, float sh, Rect c, float o) {
    (void)t; (void)dx; (void)dy; (void)dw; (void)dh; (void)sx; (void)sy; (void)sw; (void)sh; (void)c; (void)o;
}
void gpu_draw_uv(GpuTex *t, float dx, float dy, float dw, float dh, const float uv[8], Rect c, float o) {
    (void)t; (void)dx; (void)dy; (void)dw; (void)dh; (void)uv; (void)c; (void)o;
}
int  gpu_scanout(const GpuDmabuf *d, void (*done)(void *), void *data) { (void)d; (void)done; (void)data; return -1; }
int  gpu_scanout_tear(const GpuDmabuf *d, void (*done)(void *), void *data, int tear) { (void)d; (void)done; (void)data; (void)tear; return -1; }
int  gpu_flip_torn(void) { return 0; }
int  gpu_can_scanout(const GpuDmabuf *d, int w, int h) { (void)d; (void)w; (void)h; return 0; }
void gpu_read_next(uint32_t *px, int w, int h) { (void)px; (void)w; (void)h; }
int  gpu_read_done(void) { return 0; }

#else

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm/drm_fourcc.h>
#include <drm/virtgpu_drm.h>
#include <linux/sync_file.h>
#include <sys/ioctl.h>

/* Does the card have 3D acceleration? The kernel is asked before loading Mesa, so a machine
 * without a GPU (or a VM without virgl) does not load llvmpipe for nothing. */
static int drm_accelerated(int fd, char *name, size_t n) {
    struct drm_version v;
    memset(&v, 0, sizeof v);
    v.name = name;
    v.name_len = n - 1;
    if (ioctl(fd, DRM_IOCTL_VERSION, &v)) { snprintf(name, n, "?"); return 1; }
    name[v.name_len < n - 1 ? v.name_len : n - 1] = 0;
    static const char *const plain[] = { "simpledrm", "bochs-drm", "bochs", "cirrus", "qxl", "ast", "mgag200", "vkms",
                                         "udl", "gm12u320", "hyperv_drm", "vboxvideo", "ofdrm", "efidrm", "vesadrm", NULL };
    for (int i = 0; plain[i]; i++) if (!strcmp(name, plain[i])) return 0;
    if (!strcmp(name, "virtio_gpu")) {
        int has3d = 0;
        struct drm_virtgpu_getparam g = { .param = VIRTGPU_PARAM_3D_FEATURES, .value = (uintptr_t)&has3d };
        if (ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &g)) return 0;
        return has3d != 0;
    }
    return 1;
}

struct GpuTex {
    GLuint id;
    int w, h, opaque, external, swap;
    EGLImageKHR image;
    unsigned gen;                       /* the context it was made in (gpu_generation) */
};

/* What is on screen (or on its way): a BO of the gbm_surface or an app's dmabuf. */
typedef struct {
    int kind;                           /* 0 nothing, 1 GBM BO, 2 direct scanout */
    struct gbm_bo *bo;
    uint32_t fb, handles[4];
    void (*done)(void *);
    void *data;
} Shown;

/* One display (docs/DISPLAYS.md): its GBM surface and EGL window surface, the texture of its UI frame
 * and what is on its screen. Every display shares the context, programs and app textures. */
typedef struct {
    int used, first;
    Kms *k;
    int W, H;
    struct gbm_surface *gs;
    EGLSurface surf;
    GpuTex canvas;
    Shown front, pending;
    GpuFrameTimes times;                /* the last frame's (gpu_frame_times) */
    int fence;                          /* its native fence's fd + 1 (a sync file), 0 none (so a wiped GOut has none) */
    GpuScissor sc;                      /* what the last frames changed (the buffer-age scissor) */
    int sc_quiet, sc_busy;              /* this frame has only the UI (gpu_scissor_quiet); the last one didn't */
} GOut;

static struct {
    int on, debug;
    Kms *k;                             /* the first display's head (the card's fd) */
    struct gbm_device *gbm;
    EGLDisplay dpy;
    EGLContext ctx;
    EGLConfig cfg;
    GOut out[OUT_MAX];
    GOut *cur;                          /* the display being drawn (gpu_draw) */
    GLuint prog[2], vbo;                /* 0 = sampler2D, 1 = samplerExternalOES */
    GLint u_tex[2], u_op[2], u_opaque[2], u_swap[2];
    int bgra, have_external;
    int unpack_sub;                     /* GL_EXT_unpack_subimage: GL_UNPACK_ROW_LENGTH_EXT exists */
    char renderer[160];
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
    PFNEGLQUERYDMABUFFORMATSEXTPROC query_formats;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_modifiers;
    long frames, scanouts, torn;        /* torn: direct scanouts flipped asynchronously (tearing-control) */
    char render_node[64];               /* where Mesa renders (on the Pi 5: v3d, not vc4) */
    unsigned gen;                       /* this context's generation (0 = none) */
    int scissor;                        /* the buffer-age scissor: LORD_GPU_SCISSOR=1 and EGL_EXT_buffer_age */
    /* GPU drag, the dragged window composited here (gpu_gdrag_ready): 0 off, 1 on (gdrag_setup, at gpu_init) */
    int gdrag;
    int gdragtout[2];                      /* the output whose pixels each texture holds (-1: none); what it
                                         * still lacks is paint.c's dirty rectangles */
    GLuint gdragprog, gdragtex[2];            /* 0: the copy of what is below, 1: the layer */
    int gdragtw[2], gdragth[2];               /* their sizes (0: nothing uploaded) */
    int gdraglw, gdraglh;                     /* the largest output the self-test's placement covered */
    GLint gu_below, gu_layer, gu_bsize, gu_lrect, gu_swap;
    /* EGL_ANDROID_native_fence_sync: when the GPU finished a frame (gpu_fence_on) */
    PFNEGLCREATESYNCKHRPROC create_sync;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence;
} G;
static int fence_wanted;                /* gpu_fence_on (outlives the context) */

/* These outlive the context (G is wiped by gpu_shutdown). */
static unsigned last_gen;
static const char *last_why = "";       /* gpu_info: the last gpu_init's outcome */
static unsigned long total_scanouts;
static char last_renderer[160];         /* the software renderer that was turned down */
static long sc_frames, sc_cut;          /* frames with the scissor on, and those that redrew less than the whole output */
static double sc_px, sc_all_px;         /* what they composed, and what they would have without it */
static long gdrag_bpx, gdrag_lpx, gdrag_quads;   /* GPU drag's uploads and quads since gpu_gdrag_stats (across contexts) */
static void (*hook_lost)(void), (*hook_changed)(void);

void gpu_set_hooks(void (*lost)(void), void (*changed)(void)) { hook_lost = lost; hook_changed = changed; }
unsigned gpu_generation(void) { return G.on ? G.gen : 0; }
int gpu_tex_current(const GpuTex *t) { return t && G.on && t->gen == G.gen; }

static double mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static void take_fence(GOut *o);

static void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void dbg(const char *fmt, ...) {
    if (!G.debug) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "gpu: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int has_ext(const char *list, const char *ext) {
    size_t n = strlen(ext);
    for (const char *p = list; p && (p = strstr(p, ext)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || !p[n])) return 1;
    return 0;
}

/* The render node EGL uses (EGL_EXT_device_query + EGL_EXT_device_drm_render_node). On
 * boards with separate display and GPU (Raspberry Pi 4/5: vc4 + v3d; many ARM boards with kmsro) it is
 * not the display card's, and it is the one apps have to use for their buffers. */
#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#  define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif
#ifndef EGL_DEVICE_EXT
#  define EGL_DEVICE_EXT 0x322C
#endif
static void query_render_node(void) {
    G.render_node[0] = 0;
    const char *cl = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!has_ext(cl, "EGL_EXT_device_query") && !has_ext(cl, "EGL_EXT_device_base")) return;
    typedef EGLBoolean (*QueryDisplayAttrib)(EGLDisplay, EGLint, EGLAttrib *);
    typedef const char *(*QueryDeviceString)(void *, EGLint);
    QueryDisplayAttrib qda = (QueryDisplayAttrib)eglGetProcAddress("eglQueryDisplayAttribEXT");
    QueryDeviceString qds = (QueryDeviceString)eglGetProcAddress("eglQueryDeviceStringEXT");
    EGLAttrib dev = 0;
    if (!qda || !qds || !qda(G.dpy, EGL_DEVICE_EXT, &dev) || !dev) return;
    const char *dexts = qds((void *)dev, EGL_EXTENSIONS);
    if (!has_ext(dexts, "EGL_EXT_device_drm_render_node")) return;
    const char *node = qds((void *)dev, EGL_DRM_RENDER_NODE_FILE_EXT);
    if (node && !strncmp(node, "/dev/dri/renderD", 16) && strlen(node) < sizeof G.render_node)
        snprintf(G.render_node, sizeof G.render_node, "%s", node);
}

/* ---- shaders ---- */

static int gdrag_setup(void);

static const char *vs_src =
    "attribute vec2 a_pos; attribute vec2 a_uv; varying vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char *fs_2d =
    "precision mediump float; varying vec2 v_uv; uniform sampler2D u_tex;\n"
    "uniform float u_op; uniform float u_opaque; uniform float u_swap;\n"
    "void main() { vec4 c = texture2D(u_tex, v_uv); c = mix(c, c.bgra, u_swap);\n"
    "  if (u_opaque > 0.5) c.a = 1.0; gl_FragColor = c * u_op; }\n";
static const char *fs_ext =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float; varying vec2 v_uv; uniform samplerExternalOES u_tex;\n"
    "uniform float u_op; uniform float u_opaque; uniform float u_swap;\n"
    "void main() { vec4 c = texture2D(u_tex, v_uv);\n"
    "  if (u_opaque > 0.5) c.a = 1.0; gl_FragColor = c * u_op; }\n";

/* GPU drag: the layer over the copy of what is below, with blit_row's integer arithmetic (canvas.c, k = 256)
 * in 24-bit floats: every value is an integer below 2^24 and scaling by 2^-8 or 2^-16 is exact, so
 * floor gives the CPU's result bit for bit (docs/GPU.md, "The dragged window on the GPU"). Texels come
 * back as c/255 even through 16-bit floats close enough that floor(c·255 + 0.5) is c again; the
 * output c/255 is converted back to c by the framebuffer as the UI texture's is. The pixel comes from
 * v_px, a highp varying in pixels (top-down) that vs_gdrag passes from the quad's corners, not from
 * gl_FragCoord, which GLSL ES 1.00 makes mediump: with 16-bit floats it can't tell x or y from x + 1 past
 * 1024. The self-test's placement part checks it. */
#define GDRAG_TILE 1024                    /* the self-test's arithmetic tiles (GDRAG_TILE²) */
#define GDRAG_LX 37                        /* and where its placement part puts the layer (odd) */
#define GDRAG_LY 21
static const char *vs_gdrag =
    "attribute vec2 a_pos; attribute vec2 a_px; varying highp vec2 v_px;\n"
    "void main() { v_px = a_px; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char *fs_gdrag =
    "#ifndef GL_FRAGMENT_PRECISION_HIGH\n#error no highp float\n#endif\n"
    "precision highp float;\n"
    "uniform highp sampler2D u_below, u_layer;\n"
    "varying highp vec2 v_px;\n"
    "uniform vec2 u_bsize; uniform vec4 u_lrect; uniform float u_swap;\n"
    "vec4 fetch(highp sampler2D t, vec2 q, vec2 size) {\n"
    "  vec4 c = texture2D(t, (q + 0.5) / size); if (u_swap > 0.5) c = c.bgra; return floor(c * 255.0 + 0.5); }\n"
    "void main() {\n"
    "  vec2 p = floor(v_px);\n"
    "  vec4 d = fetch(u_below, p, u_bsize), s = fetch(u_layer, p - u_lrect.xy, u_lrect.zw), o = d;\n"
    "  if (s.a > 0.0) {\n"
    "    float ia = 255.0 - s.a;\n"
    "    o.rgb = s.rgb + floor(d.rgb * ia * 0.00390625);\n"
    "    o.a = s.a + floor((d.a * ia + 128.0) * 257.0 * 0.0000152587890625);\n"
    "  }\n"
    "  gl_FragColor = o / 255.0; }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "gpu: shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static int make_prog(int i, const char *fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs_src), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return -1;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_uv");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) return -1;
    G.prog[i] = p;
    G.u_tex[i] = glGetUniformLocation(p, "u_tex");
    G.u_op[i] = glGetUniformLocation(p, "u_op");
    G.u_opaque[i] = glGetUniformLocation(p, "u_opaque");
    G.u_swap[i] = glGetUniformLocation(p, "u_swap");
    return 0;
}

/* ---- flips ---- */

static void release(GOut *o, Shown *s) {
    if (s->kind == 1 && s->bo) gbm_surface_release_buffer(o->gs, s->bo);
    if (s->kind == 2) {
        kms_rm_fb(G.k, s->fb);
        for (int i = 0; i < 4; i++) {
            int dup = 0;
            for (int j = 0; j < i; j++) if (s->handles[j] == s->handles[i]) dup = 1;
            if (!dup) kms_close_handle(G.k, s->handles[i]);
        }
        if (s->done) s->done(s->data);
    }
    memset(s, 0, sizeof *s);
}

static void on_flip(void *data) {
    GOut *o = data;
    if (!o || o < G.out || o >= G.out + OUT_MAX || !o->used) return;
    release(o, &o->front);
    o->front = o->pending;
    memset(&o->pending, 0, sizeof o->pending);
}

static void bo_fb_destroy(struct gbm_bo *bo, void *data) {
    (void)bo;
    uint32_t fb = (uint32_t)(uintptr_t)data;
    if (G.k) kms_rm_fb(G.k, fb);
}

static uint32_t fb_for_bo(struct gbm_bo *bo) {
    uint32_t fb = (uint32_t)(uintptr_t)gbm_bo_get_user_data(bo);
    if (fb) return fb;
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    int n = gbm_bo_get_plane_count(bo);
    for (int i = 0; i < n && i < 4; i++) {
        handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
        pitches[i] = gbm_bo_get_stride_for_plane(bo, i);
        offsets[i] = gbm_bo_get_offset(bo, i);
    }
    uint64_t mod = gbm_bo_get_modifier(bo);
    if (kms_add_fb2(G.k, gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_format(bo), handles, pitches, offsets,
                    mod, &fb) &&
        kms_add_fb2(G.k, gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_format(bo), handles, pitches, offsets,
                    DRM_FORMAT_MOD_INVALID, &fb)) {
        fprintf(stderr, "gpu: ADDFB2 of the BO failed (%s)\n", strerror(errno));
        return 0;
    }
    gbm_bo_set_user_data(bo, (void *)(uintptr_t)fb, bo_fb_destroy);
    return fb;
}

static int present(GOut *o, Shown next) {
    if (o->first) {
        if (kms_set_fb(o->k, next.fb)) { fprintf(stderr, "gpu: SETCRTC failed (%s)\n", strerror(errno)); release(o, &next); return -1; }
        o->first = 0;
        release(o, &o->front);
        o->front = next;
        return 0;
    }
    o->pending = next;
    if (kms_flip_fb(o->k, next.fb, o)) {
        fprintf(stderr, "gpu: page flip failed (%s)\n", strerror(errno));
        memset(&o->pending, 0, sizeof o->pending);
        release(o, &next);
        return -1;
    }
    return 0;
}

/* A display's GBM surface, EGL surface and UI texture (the context already exists). */
static int out_setup(GOut *o, Kms *k) {
    memset(o, 0, sizeof *o);
    o->k = k;
    o->W = (int)k->width;
    o->H = (int)k->height;
    o->gs = gbm_surface_create(G.gbm, (uint32_t)o->W, (uint32_t)o->H, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!o->gs) return -1;
    o->surf = eglCreateWindowSurface(G.dpy, G.cfg, (EGLNativeWindowType)o->gs, NULL);
    if (o->surf == EGL_NO_SURFACE || !eglMakeCurrent(G.dpy, o->surf, o->surf, G.ctx)) {
        if (o->surf != EGL_NO_SURFACE) eglDestroySurface(G.dpy, o->surf);
        gbm_surface_destroy(o->gs);
        memset(o, 0, sizeof *o);
        return -1;
    }
    k->on_flip = on_flip;
    o->first = 1;
    o->used = 1;
    G.cur = o;
    return 0;
}

/* Its UI texture (after the GL extensions are known: BGRA or RGBA). */
static void out_texture(GOut *o) {
    glGenTextures(1, &o->canvas.id);
    glBindTexture(GL_TEXTURE_2D, o->canvas.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, G.bgra ? GL_BGRA_EXT : GL_RGBA, o->W, o->H, 0, G.bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    o->canvas.w = o->W;
    o->canvas.h = o->H;
    o->canvas.swap = !G.bgra;
    o->canvas.gen = G.gen;
}

static void out_teardown(GOut *o) {
    if (!o->used) return;
    if (o->fence > 0) close(o->fence - 1);
    release(o, &o->pending);
    release(o, &o->front);
    if (o->surf && eglMakeCurrent(G.dpy, o->surf, o->surf, G.ctx)) glDeleteTextures(1, &o->canvas.id);
    eglMakeCurrent(G.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (o->surf) eglDestroySurface(G.dpy, o->surf);
    if (o->gs) gbm_surface_destroy(o->gs);
    if (o->k) { o->k->on_flip = NULL; o->k->custom = 0; }
    memset(o, 0, sizeof *o);
}

int gpu_add_output(Kms *k) {
    if (!G.on) return -1;
    for (int i = 0; i < OUT_MAX; i++) {
        if (G.out[i].used) continue;
        if (out_setup(&G.out[i], k)) { fprintf(stderr, "gpu: no EGL surface for the display on connector %u\n", k->conn_id); return -1; }
        out_texture(&G.out[i]);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        return i;
    }
    return -1;
}

/* ---- startup ---- */

int gpu_active(void) { return G.on; }
const char *gpu_renderer(void) { return G.renderer; }

void gpu_info(GpuInfo *i) {
    i->why = last_why;
    i->renderer = G.on ? G.renderer : !strcmp(last_why, "software") ? last_renderer : "";
    i->render_node = G.on ? G.render_node : "";
    i->dmabuf = G.on && G.create_image != NULL;
    i->scanouts = total_scanouts;
}

int gpu_init(Kms *k) {
    const char *env = getenv("LORD_GPU");
    G.debug = getenv("LORD_GPU_DEBUG") != NULL;
    if (env && !strcmp(env, "0")) { fprintf(stderr, "gpu: off (LORD_GPU=0), CPU composition\n"); last_why = "env-off"; return -1; }
    G.k = k;
    char drv[64];
    if (!drm_accelerated(k->fd, drv, sizeof drv) && !(env && !strcmp(env, "1"))) {
        fprintf(stderr, "gpu: the card (%s) has no 3D acceleration: CPU composition (LORD_GPU=1 forces it)\n", drv);
        last_why = "no-3d";
        return -1;
    }
    G.gbm = gbm_create_device(k->fd);
    if (!G.gbm) { fprintf(stderr, "gpu: no GBM on the card, CPU composition\n"); last_why = "no-gbm"; return -1; }
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    G.dpy = get_display ? get_display(EGL_PLATFORM_GBM_KHR, G.gbm, NULL) : eglGetDisplay((EGLNativeDisplayType)G.gbm);
    EGLint maj, min;
    if (G.dpy == EGL_NO_DISPLAY || !eglInitialize(G.dpy, &maj, &min)) { fprintf(stderr, "gpu: EGL did not start, CPU composition\n"); last_why = "no-egl"; goto fail; }
    const char *exts = eglQueryString(G.dpy, EGL_EXTENSIONS);
    query_render_node();
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                       EGL_ALPHA_SIZE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig cfgs[64], cfg = NULL;
    EGLint n = 0;
    eglChooseConfig(G.dpy, cfg_attr, cfgs, 64, &n);
    for (int i = 0; i < n && !cfg; i++) {
        EGLint vid = 0;
        if (eglGetConfigAttrib(G.dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &vid) && vid == GBM_FORMAT_XRGB8888) cfg = cfgs[i];
    }
    if (!cfg) { fprintf(stderr, "gpu: no XRGB8888 EGL config, CPU composition\n"); last_why = "no-config"; goto fail; }
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    G.ctx = eglCreateContext(G.dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    G.cfg = cfg;
    G.gen = ++last_gen;
    if (last_gen == 0) G.gen = last_gen = 1;    /* 0 means "no context" */
    if (G.ctx == EGL_NO_CONTEXT || out_setup(&G.out[0], k)) { fprintf(stderr, "gpu: could not create the context or the surface, CPU composition\n"); last_why = "no-context"; goto fail; }

    const char *rend = (const char *)glGetString(GL_RENDERER), *gl_exts = (const char *)glGetString(GL_EXTENSIONS);
    snprintf(G.renderer, sizeof G.renderer, "%s", rend ? rend : "?");
    int software = strstr(G.renderer, "llvmpipe") || strstr(G.renderer, "softpipe") || strstr(G.renderer, "swrast");
    if (software && !(env && !strcmp(env, "1"))) {
        fprintf(stderr, "gpu: the renderer is software (%s): CPU composition (LORD_GPU=1 forces it)\n", G.renderer);
        last_why = "software";
        snprintf(last_renderer, sizeof last_renderer, "%s", G.renderer);
        goto fail;
    }
    G.bgra = has_ext(gl_exts, "GL_EXT_texture_format_BGRA8888");
    G.have_external = has_ext(gl_exts, "GL_OES_EGL_image_external");
    G.unpack_sub = has_ext(gl_exts, "GL_EXT_unpack_subimage");
    {
        const char *sc = getenv("LORD_GPU_SCISSOR");
        int want = sc && !strcmp(sc, "1");
        G.scissor = want && has_ext(exts, "EGL_EXT_buffer_age");
        fprintf(stderr, "gpu: buffer-age scissor %s\n", G.scissor ? "on" : want ? "off: no EGL_EXT_buffer_age" : "off (LORD_GPU_SCISSOR=1 turns it on)");
    }
    if (!G.unpack_sub) dbg("no GL_EXT_unpack_subimage: damage is uploaded as whole rows");
    if (make_prog(0, fs_2d) || (G.have_external && make_prog(1, fs_ext))) { fprintf(stderr, "gpu: shaders failed, CPU composition\n"); last_why = "shaders"; goto fail; }
    if (has_ext(exts, "EGL_EXT_image_dma_buf_import")) {
        G.create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        G.destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        G.image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    }
    if (has_ext(exts, "EGL_EXT_image_dma_buf_import_modifiers")) {
        G.query_formats = (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
        G.query_modifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    }

    if (has_ext(exts, "EGL_ANDROID_native_fence_sync") && has_ext(exts, "EGL_KHR_fence_sync")) {
        G.create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
        G.destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
        G.dup_fence = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
        if (!G.create_sync || !G.destroy_sync || !G.dup_fence) G.create_sync = NULL;
    }
    dbg("native fences (frame phases): %s", G.create_sync ? "yes" : "no");

    out_texture(&G.out[0]);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    G.on = 1;
    G.gdragtout[0] = G.gdragtout[1] = -1;
    last_why = "on";
    if (G.debug) {
        GpuFormat f[64];
        int nf = gpu_formats(f, 64);
        for (int i = 0; i < nf; i++) dbg("dmabuf format %08x mod %llx", f[i].fourcc, (unsigned long long)f[i].modifier);
    }
    fprintf(stderr, "gpu: GPU composition (%s, EGL %d.%d, dmabuf %s, render %s, display %s)\n", G.renderer, maj, min,
            G.create_image ? (G.query_modifiers ? "with modifiers" : "yes") : "no",
            G.render_node[0] ? G.render_node : "the card's", k->driver[0] ? k->driver : drv);
    /* GPU drag's self-test runs here, not in the first drag frame (once per renderer: gdrag_memo) */
    G.gdrag = gdrag_setup();
    if (hook_changed) hook_changed();       /* windows re-import their buffers in this context */
    return 0;
fail:
    gpu_shutdown();
    return -1;
}

/* Tests and benchmarks only (build/motion-bench, docs/MOVING-WALLPAPERS.md "Measurements"): a context on a render node
 * with no display and no surface (EGL_KHR_surfaceless_context), so the NV12 programs, the zero-copy import and the loop
 * cache's packing run on the real GPU without touching a screen. Never called by lord-server. */
int gpu_init_offscreen(int render_fd) {
    G.gbm = gbm_create_device(render_fd);
    if (!G.gbm) return -1;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    G.dpy = get_display ? get_display(EGL_PLATFORM_GBM_KHR, G.gbm, NULL) : eglGetDisplay((EGLNativeDisplayType)G.gbm);
    EGLint maj, min;
    if (G.dpy == EGL_NO_DISPLAY || !eglInitialize(G.dpy, &maj, &min)) return -1;
    const char *exts = eglQueryString(G.dpy, EGL_EXTENSIONS);
    if (!has_ext(exts, "EGL_KHR_surfaceless_context")) return -1;
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    G.ctx = eglCreateContext(G.dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
    if (G.ctx == EGL_NO_CONTEXT || !eglMakeCurrent(G.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, G.ctx)) return -1;
    const char *rend = (const char *)glGetString(GL_RENDERER), *gl_exts = (const char *)glGetString(GL_EXTENSIONS);
    snprintf(G.renderer, sizeof G.renderer, "%s", rend ? rend : "?");
    G.bgra = has_ext(gl_exts, "GL_EXT_texture_format_BGRA8888");
    G.have_external = has_ext(gl_exts, "GL_OES_EGL_image_external");
    G.unpack_sub = has_ext(gl_exts, "GL_EXT_unpack_subimage");
    if (make_prog(0, fs_2d) || (G.have_external && make_prog(1, fs_ext))) return -1;
    if (has_ext(exts, "EGL_EXT_image_dma_buf_import")) {
        G.create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        G.destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        G.image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    }
    if (has_ext(exts, "EGL_EXT_image_dma_buf_import_modifiers")) {
        G.query_formats = (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
        G.query_modifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    }
    G.gen = ++last_gen;
    G.on = 1;
    return 0;
}
/* the offscreen context's work done (a benchmark's frame): glFinish */
void gpu_offscreen_finish(void) { if (G.on) glFinish(); }

void gpu_shutdown(void) {
    int was_on = G.on;
    if (sc_frames) fprintf(stderr, "gpu: buffer-age scissor: %ld frames, %ld of them partial, composed %.1f Mpx of %.1f\n", sc_frames, sc_cut, sc_px / 1e6, sc_all_px / 1e6);
    sc_frames = sc_cut = 0;
    sc_px = sc_all_px = 0;
    if (was_on && hook_lost) {
        /* the compositor reads back and frees what it holds while the context still works */
        for (int i = 0; i < OUT_MAX; i++)
            if (G.out[i].used && eglMakeCurrent(G.dpy, G.out[i].surf, G.out[i].surf, G.ctx)) { hook_lost(); break; }
    }
    if (G.dpy != EGL_NO_DISPLAY && G.dpy) {
        for (int i = OUT_MAX - 1; i >= 0; i--) out_teardown(&G.out[i]);
        eglMakeCurrent(G.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (G.ctx) eglDestroyContext(G.dpy, G.ctx);
        eglTerminate(G.dpy);
    }
    if (G.gbm) gbm_device_destroy(G.gbm);
    if (G.k) { G.k->on_flip = NULL; G.k->custom = 0; }
    int debug = G.debug;
    memset(&G, 0, sizeof G);
    G.debug = debug;
    if (was_on && hook_changed) hook_changed();     /* now on the CPU path */
}
/* ---- formats and device (linux-dmabuf) ---- */

static const uint32_t known[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888 };

int gpu_formats(GpuFormat *out, int max) {
    if (!G.on || !G.create_image) return 0;
    int n = 0;
    for (size_t f = 0; f < sizeof known / sizeof *known; f++) {
        EGLuint64KHR mods[64];
        EGLBoolean ext[64];
        EGLint nm = 0;
        if (G.query_modifiers) G.query_modifiers(G.dpy, (EGLint)known[f], 64, mods, ext, &nm);
        for (int i = 0; i < nm && n < max; i++) out[n++] = (GpuFormat){ known[f], mods[i] };
        if (n < max) out[n++] = (GpuFormat){ known[f], DRM_FORMAT_MOD_INVALID };   /* implicit */
    }
    return n;
}

/* The render node for linux-dmabuf feedback: the one EGL uses; if EGL does not say, the one on
 * the same device as the output's card; and if that device has none (a display without a GPU, like vc4 on
 * the Pi 5), the first render node there is, which on those boards is the GPU (v3d, panfrost…). */
int gpu_device(dev_t *dev) {
    if (!G.k) return -1;
    struct stat st;
    if (G.render_node[0] && !stat(G.render_node, &st) && S_ISCHR(st.st_mode)) { *dev = st.st_rdev; return 0; }
    if (fstat(G.k->fd, &st)) return -1;
    char card[PATH_MAX], want[PATH_MAX];
    snprintf(card, sizeof card, "/sys/dev/char/%u:%u/device", major(st.st_rdev), minor(st.st_rdev));
    if (!realpath(card, want)) { *dev = st.st_rdev; return 0; }
    DIR *d = opendir("/dev/dri");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "renderD", 7)) continue;
        char path[PATH_MAX], sys[PATH_MAX], real[PATH_MAX];
        struct stat rs;
        snprintf(path, sizeof path, "/dev/dri/%s", e->d_name);
        if (stat(path, &rs)) continue;
        snprintf(sys, sizeof sys, "/sys/dev/char/%u:%u/device", major(rs.st_rdev), minor(rs.st_rdev));
        if (realpath(sys, real) && !strcmp(real, want)) { *dev = rs.st_rdev; closedir(d); return 0; }
    }
    /* none is on the same device: separate display and GPU */
    int found = 0;
    dev_t other = 0;
    if (d) rewinddir(d);
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "renderD", 7)) continue;
        char path[PATH_MAX];
        struct stat rs;
        snprintf(path, sizeof path, "/dev/dri/%s", e->d_name);
        if (stat(path, &rs) || !S_ISCHR(rs.st_mode)) continue;
        if (!found || rs.st_rdev < other) other = rs.st_rdev;       /* the lowest-numbered one */
        found = 1;
    }
    if (d) closedir(d);
    *dev = found ? other : st.st_rdev;
    return 0;
}

/* ---- textures ---- */

static void tex_params(GLenum target) {
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

GpuTex *gpu_tex_pixels(GpuTex *t, const uint32_t *px, int w, int h, int opaque) {
    if (!G.on || !px || w <= 0 || h <= 0) return t;
    if (t && (t->external || t->gen != G.gen)) { gpu_tex_free(t); t = NULL; }     /* stale: from a dead context */
    int fresh = !t;
    if (!t) {
        t = calloc(1, sizeof *t);
        if (!t) return NULL;
        glGenTextures(1, &t->id);
        glBindTexture(GL_TEXTURE_2D, t->id);
        tex_params(GL_TEXTURE_2D);
        t->gen = G.gen;
    } else glBindTexture(GL_TEXTURE_2D, t->id);
    GLenum fmt = G.bgra ? GL_BGRA_EXT : GL_RGBA;
    if (fresh || t->w != w || t->h != h) glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, px);
    else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, GL_UNSIGNED_BYTE, px);
    t->w = w;
    t->h = h;
    t->opaque = opaque;
    t->swap = !G.bgra;
    return t;
}

static long upload_rect(const uint32_t *px, int stride, int sw, int texw, Rect r);

/* A Linux app's damage (wl.c): only r is sent to a live texture of the same size; anything else is a whole upload. */
GpuTex *gpu_tex_pixels_rect(GpuTex *t, const uint32_t *px, int w, int h, int opaque, Rect r) {
    if (!G.on || !px || w <= 0 || h <= 0) return t;
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > w) r.x1 = w;
    if (r.y1 > h) r.y1 = h;
    if (!t || t->external || t->gen != G.gen || t->w != w || t->h != h || (r.x0 == 0 && r.y0 == 0 && r.x1 == w && r.y1 == h))
        return gpu_tex_pixels(t, px, w, h, opaque);
    if (r.x1 <= r.x0 || r.y1 <= r.y0) { t->opaque = opaque; return t; }
    glBindTexture(GL_TEXTURE_2D, t->id);
    upload_rect(px, w, w, t->w, r);
    t->opaque = opaque;
    return t;
}

static EGLImageKHR import_image(const GpuDmabuf *d, int with_mod);

GpuTex *gpu_tex_dmabuf(const GpuDmabuf *d) {
    if (!G.on || !G.create_image || !G.have_external || d->nplanes < 1 || d->nplanes > 4) return NULL;
    EGLImageKHR img = import_image(d, 1);
    /* linear without an explicit modifier: some drivers only accept the implicit one */
    if (img == EGL_NO_IMAGE_KHR && d->modifier == DRM_FORMAT_MOD_LINEAR) img = import_image(d, 0);
    if (img == EGL_NO_IMAGE_KHR) {
        dbg("could not import a dmabuf %dx%d fourcc %08x mod %llx (EGL 0x%x)", d->width, d->height, d->fourcc,
            (unsigned long long)d->modifier, eglGetError());
        return NULL;
    }
    GpuTex *t = calloc(1, sizeof *t);
    if (!t) { G.destroy_image(G.dpy, img); return NULL; }
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, t->id);
    tex_params(GL_TEXTURE_EXTERNAL_OES);
    G.image_target(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)img);
    t->image = img;
    t->external = 1;
    t->gen = G.gen;
    t->w = d->width;
    t->h = d->height;
    t->opaque = d->fourcc == DRM_FORMAT_XRGB8888 || d->fourcc == DRM_FORMAT_XBGR8888;
    return t;
}

static EGLImageKHR import_image(const GpuDmabuf *d, int with_mod) {
    static const EGLint fd_k[4] = { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT };
    static const EGLint off_k[4] = { EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT };
    static const EGLint pitch_k[4] = { EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT };
    static const EGLint lo_k[4] = { EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT };
    static const EGLint hi_k[4] = { EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT };
    EGLint attr[64];
    int a = 0;
    attr[a++] = EGL_WIDTH; attr[a++] = d->width;
    attr[a++] = EGL_HEIGHT; attr[a++] = d->height;
    attr[a++] = EGL_LINUX_DRM_FOURCC_EXT; attr[a++] = (EGLint)d->fourcc;
    for (int i = 0; i < d->nplanes; i++) {
        attr[a++] = fd_k[i]; attr[a++] = d->fd[i];
        attr[a++] = off_k[i]; attr[a++] = (EGLint)d->offset[i];
        attr[a++] = pitch_k[i]; attr[a++] = (EGLint)d->stride[i];
        if (with_mod && d->modifier != DRM_FORMAT_MOD_INVALID && G.query_modifiers) {
            attr[a++] = lo_k[i]; attr[a++] = (EGLint)(d->modifier & 0xffffffff);
            attr[a++] = hi_k[i]; attr[a++] = (EGLint)(d->modifier >> 32);
        }
    }
    attr[a++] = EGL_IMAGE_PRESERVED_KHR; attr[a++] = EGL_TRUE;
    attr[a] = EGL_NONE;
    return G.create_image(G.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attr);
}

int gpu_tex_width(const GpuTex *t) { return t ? t->w : 0; }

void gpu_tex_free(GpuTex *t) {
    if (!t) return;
    if (G.on && t->gen == G.gen) {          /* a stale one's GL name may now be someone else's */
        glDeleteTextures(1, &t->id);
        if (t->image && G.destroy_image) G.destroy_image(G.dpy, t->image);
    }
    free(t);
}

int gpu_tex_read(const GpuTex *t, uint32_t *px, int w, int h) {
    if (!gpu_tex_current(t) || !px || w != t->w || h != t->h || w <= 0 || h <= 0 || w > 8192 || h > 8192) return -1;
    while (glGetError() != GL_NO_ERROR) {}
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glViewport(0, 0, w, h);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        int p = t->external ? 1 : 0;
        glUseProgram(G.prog[p]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(t->external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, t->id);
        glUniform1i(G.u_tex[p], 0);
        glUniform1f(G.u_op[p], 1.0f);
        glUniform1f(G.u_opaque[p], t->opaque ? 1.0f : 0.0f);
        glUniform1f(G.u_swap[p], t->swap ? 1.0f : 0.0f);
        /* the image's first row at y = -1: glReadPixels returns it first */
        static const GLfloat v[] = { -1, -1, 0, 0,  1, -1, 1, 0,  -1, 1, 0, 1,  1, 1, 1, 1 };
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        ok = glGetError() == GL_NO_ERROR;
        for (size_t i = 0, n = (size_t)w * (size_t)h; ok && i < n; i++) {    /* RGBA bytes → ARGB words */
            const uint8_t *b = (const uint8_t *)&px[i];
            px[i] = (uint32_t)b[3] << 24 | (uint32_t)b[0] << 16 | (uint32_t)b[1] << 8 | b[2];
        }
        glEnable(GL_BLEND);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    return ok ? 0 : -1;
}

/* gpu_tex_read_rows' framebuffer: kept between calls, made again for another size or context */
static struct { GLuint tex, fbo; int w, h; unsigned gen; } vrb;

int gpu_tex_read_rows(const GpuTex *t, uint8_t *px, int y, int h, int flip) {
    if (!gpu_tex_current(t) || !px || t->w <= 0 || t->h <= 0 || t->w > 8192 || t->h > 8192 || y < 0 || h <= 0 || y + h > t->h) return -1;
    if (eglGetCurrentContext() != G.ctx) {
        int ok = 0;
        for (int i = 0; i < OUT_MAX && !ok; i++) if (G.out[i].used && eglMakeCurrent(G.dpy, G.out[i].surf, G.out[i].surf, G.ctx)) ok = 1;
        if (!ok) return -1;
    }
    while (glGetError() != GL_NO_ERROR) {}
    if (vrb.fbo && (vrb.w != t->w || vrb.h != t->h || vrb.gen != G.gen)) {
        if (vrb.gen == G.gen) { glDeleteFramebuffers(1, &vrb.fbo); glDeleteTextures(1, &vrb.tex); }
        vrb.fbo = vrb.tex = 0;
    }
    if (!vrb.fbo) {
        glGenTextures(1, &vrb.tex);
        glBindTexture(GL_TEXTURE_2D, vrb.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->w, t->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers(1, &vrb.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, vrb.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vrb.tex, 0);
        vrb.w = t->w; vrb.h = t->h; vrb.gen = G.gen;
    } else glBindFramebuffer(GL_FRAMEBUFFER, vrb.fbo);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glViewport(0, 0, t->w, t->h);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        int p = t->external ? 1 : 0;
        glUseProgram(G.prog[p]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(t->external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, t->id);
        glUniform1i(G.u_tex[p], 0);
        glUniform1f(G.u_op[p], 1.0f);
        glUniform1f(G.u_opaque[p], 1.0f);
        glUniform1f(G.u_swap[p], t->swap ? 1.0f : 0.0f);
        /* the picture's first row at y = -1: glReadPixels returns it first */
        GLfloat t0 = flip ? 1.0f : 0.0f, t1 = flip ? 0.0f : 1.0f;
        const GLfloat v[] = { -1, -1, 0, t0,  1, -1, 1, t0,  -1, 1, 0, t1,  1, 1, 1, t1 };
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        uint8_t *rows = px + (size_t)y * (size_t)t->w * 4;
        glReadPixels(0, y, t->w, h, GL_RGBA, GL_UNSIGNED_BYTE, rows);
        ok = glGetError() == GL_NO_ERROR;
        for (size_t i = 0, n = (size_t)t->w * (size_t)h; ok && i < n; i++) {    /* R, G, B, A → B, G, R, X */
            uint8_t r = rows[i * 4];
            rows[i * 4] = rows[i * 4 + 2];
            rows[i * 4 + 2] = r;
            rows[i * 4 + 3] = 255;
        }
        glEnable(GL_BLEND);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok ? 0 : -1;
}

/* ---- NV12 frames (gpu.h gpu_yuv_*, gpu_pack_*) ----
 * Sources: two planes as textures, either uploaded from lord-media's memfd (LUMINANCE and LUMINANCE_ALPHA, sampler2D)
 * or imported from its dmabuf (R8 and GR88 EGLImages, external textures: zero-copy). Programs, made at first use:
 *   conv[e]   the planes → an RGBA texture with the video's matrix (e: external)
 *   pky[e]    the Y plane packed four samples a texel into an RGBA8 texture (w/4 × h): the loop cache's own copy
 *   pkuv[e]   the U, V plane packed two pairs a texel (w/4 × h/2): 1.5 bytes a pixel in all, as NV12
 *   unpk      a packed frame → RGBA (the loop cache's playback)
 * GLES2 can't render into one- or two-byte textures, hence the packing; texel positions need highp (else no cache). */

struct GpuYuv {
    GLuint ty, tuv;
    GpuTex *ey, *euv;                   /* imported planes (external): owned, freed with it */
    int w, h, ext;
    unsigned gen;
};

struct GpuPack {
    GLuint ty, tuv;
    int w, h, tw;                       /* tw: the packed width, ceil(w / 4) */
    unsigned gen;
};

#define YUV_HEAD(ext) (ext ? "#extension GL_OES_EGL_image_external : require\n#define SAMP samplerExternalOES\n#define UVS(t) (t).rg\n" \
                           : "#define SAMP sampler2D\n#define UVS(t) (t).ra\n")
static const char *fs_conv =
    "precision mediump float; varying vec2 v_uv; uniform SAMP u_y; uniform SAMP u_uv; uniform mat4 u_m;\n"
    "void main() { vec4 q = vec4(texture2D(u_y, v_uv).r, UVS(texture2D(u_uv, v_uv)), 1.0);\n"
    "  gl_FragColor = vec4(clamp((u_m * q).rgb, 0.0, 1.0), 1.0); }\n";
static const char *vs_hp =
    "attribute vec2 a_pos; attribute vec2 a_uv; varying highp vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
/* u_size: the source plane's size in texels; v_uv * u_out: the packed texel being written (its centre) */
static const char *fs_pky =
    "#ifndef GL_FRAGMENT_PRECISION_HIGH\n#error no highp\n#endif\nprecision highp float; varying highp vec2 v_uv;\n"
    "uniform SAMP u_t; uniform vec2 u_size; uniform vec2 u_out;\n"
    "float at(float x, float y) { return texture2D(u_t, vec2((min(x, u_size.x - 1.0) + 0.5) / u_size.x, (y + 0.5) / u_size.y)).r; }\n"
    "void main() { vec2 p = floor(v_uv * u_out); float x = p.x * 4.0;\n"
    "  gl_FragColor = vec4(at(x, p.y), at(x + 1.0, p.y), at(x + 2.0, p.y), at(x + 3.0, p.y)); }\n";
static const char *fs_pkuv =
    "#ifndef GL_FRAGMENT_PRECISION_HIGH\n#error no highp\n#endif\nprecision highp float; varying highp vec2 v_uv;\n"
    "uniform SAMP u_t; uniform vec2 u_size; uniform vec2 u_out;\n"
    "vec2 at(float x, float y) { return UVS(texture2D(u_t, vec2((min(x, u_size.x - 1.0) + 0.5) / u_size.x, (y + 0.5) / u_size.y))); }\n"
    "void main() { vec2 p = floor(v_uv * u_out); float x = p.x * 2.0;\n"
    "  gl_FragColor = vec4(at(x, p.y), at(x + 1.0, p.y)); }\n";
/* u_px: the frame's size; packed textures u_y (tw × h) and u_uv (tw × h/2), chroma nearest */
static const char *fs_unpk =
    "#ifndef GL_FRAGMENT_PRECISION_HIGH\n#error no highp\n#endif\nprecision highp float; varying highp vec2 v_uv;\n"
    "uniform sampler2D u_y; uniform sampler2D u_uv; uniform vec2 u_px; uniform float u_tw; uniform mat4 u_m;\n"
    "void main() { vec2 p = floor(v_uv * u_px);\n"
    "  float ix = floor(p.x / 4.0), k = p.x - ix * 4.0;\n"
    "  vec4 y4 = texture2D(u_y, vec2((ix + 0.5) / u_tw, (p.y + 0.5) / u_px.y));\n"
    "  float y = k < 0.5 ? y4.r : k < 1.5 ? y4.g : k < 2.5 ? y4.b : y4.a;\n"
    "  float cx = floor(p.x / 2.0), jx = floor(cx / 2.0), j = cx - jx * 2.0;\n"
    "  vec4 c4 = texture2D(u_uv, vec2((jx + 0.5) / u_tw, (floor(p.y / 2.0) + 0.5) / floor(u_px.y / 2.0)));\n"
    "  vec2 c = j < 0.5 ? c4.rg : c4.ba;\n"
    "  gl_FragColor = vec4(clamp((u_m * vec4(y, c, 1.0)).rgb, 0.0, 1.0), 1.0); }\n";

enum { YP_CONV = 0, YP_PKY = 2, YP_PKUV = 4, YP_UNPK = 6, YP_N = 7 };
static struct { GLuint p[YP_N]; int tried[YP_N]; unsigned gen; } YP;

static GLuint yuv_program(int which, int ext) {
    int i = which == YP_UNPK ? YP_UNPK : which + (ext ? 1 : 0);
    if (YP.gen != G.gen) { memset(&YP, 0, sizeof YP); YP.gen = G.gen; }
    if (YP.p[i] || YP.tried[i]) return YP.p[i];
    YP.tried[i] = 1;
    if (ext && !G.have_external) return 0;
    const char *body = which == YP_CONV ? fs_conv : which == YP_PKY ? fs_pky : which == YP_PKUV ? fs_pkuv : fs_unpk;
    const char *head = which == YP_UNPK ? "" : YUV_HEAD(ext);
    /* #extension first, then the body (the #error lines stay preprocessor lines) */
    size_t nh = strlen(head), nb = strlen(body);
    char *src = malloc(nh + nb + 1);
    if (!src) return 0;
    memcpy(src, head, nh);
    memcpy(src + nh, body, nb + 1);
    GLuint v = compile(GL_VERTEX_SHADER, which == YP_CONV ? vs_src : vs_hp), f = compile(GL_FRAGMENT_SHADER, src);
    free(src);
    if (!v || !f) { if (v) glDeleteShader(v); if (f) glDeleteShader(f); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_uv");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(p); return 0; }
    YP.p[i] = p;
    return p;
}

/* the context current on some output's surface (gpu_tex_read_rows does the same): frames come between paints */
static int ctx_current(void) {
    if (!G.on) return 0;
    if (eglGetCurrentContext() == G.ctx) return 1;
    for (int i = 0; i < OUT_MAX; i++) if (G.out[i].used && eglMakeCurrent(G.dpy, G.out[i].surf, G.out[i].surf, G.ctx)) return 1;
    return 0;
}

int gpu_yuv_current(const GpuYuv *t) { return t && G.on && t->gen == G.gen; }
long gpu_yuv_bytes(const GpuYuv *t) { return t ? (long)t->w * t->h + (long)t->w * (t->h / 2) : 0; }

void gpu_yuv_free(GpuYuv *t) {
    if (!t) return;
    if (t->ext) { gpu_tex_free(t->ey); gpu_tex_free(t->euv); }
    else if (G.on && t->gen == G.gen && ctx_current()) { glDeleteTextures(1, &t->ty); glDeleteTextures(1, &t->tuv); }
    free(t);
}

/* rows of a plane (bpr bytes of pixels a row, stride bytes apart) into the bound texture */
static void plane_up(GLenum fmt, int tw, int th, int bpr, const uint8_t *px, int stride, int fresh) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (stride == bpr || G.unpack_sub) {
        if (stride != bpr) glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / (bpr / tw));
        if (fresh) glTexImage2D(GL_TEXTURE_2D, 0, fmt, tw, th, 0, fmt, GL_UNSIGNED_BYTE, px);
        else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tw, th, fmt, GL_UNSIGNED_BYTE, px);
        if (stride != bpr) glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
    } else {
        if (fresh) glTexImage2D(GL_TEXTURE_2D, 0, fmt, tw, th, 0, fmt, GL_UNSIGNED_BYTE, NULL);
        for (int y = 0; y < th; y++) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, tw, 1, fmt, GL_UNSIGNED_BYTE, px + (size_t)y * stride);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

GpuYuv *gpu_yuv_upload(GpuYuv *t, const uint8_t *y, int ystride, const uint8_t *uv, int uvstride, int w, int h) {
    if (!G.on || !y || !uv || w < 2 || h < 2 || (w & 1) || (h & 1) || w > 8192 || h > 8192 || ystride < w || uvstride < w) return t;
    if (!ctx_current()) return t;
    if (t && (t->gen != G.gen || t->ext)) { gpu_yuv_free(t); t = NULL; }
    int fresh = !t || t->w != w || t->h != h;
    if (!t) {
        t = calloc(1, sizeof *t);
        if (!t) return NULL;
        glGenTextures(1, &t->ty);
        glGenTextures(1, &t->tuv);
        t->gen = G.gen;
        for (int i = 0; i < 2; i++) { glBindTexture(GL_TEXTURE_2D, i ? t->tuv : t->ty); tex_params(GL_TEXTURE_2D); }
    }
    while (glGetError() != GL_NO_ERROR) {}
    glBindTexture(GL_TEXTURE_2D, t->ty);
    plane_up(GL_LUMINANCE, w, h, w, y, ystride, fresh);
    glBindTexture(GL_TEXTURE_2D, t->tuv);
    plane_up(GL_LUMINANCE_ALPHA, w / 2, h / 2, w, uv, uvstride, fresh);
    t->w = w;
    t->h = h;
    if (glGetError() != GL_NO_ERROR) { gpu_yuv_free(t); return NULL; }
    return t;
}

GpuYuv *gpu_yuv_import(const GpuDmabuf *y, const GpuDmabuf *uv) {
    if (!G.on || !y || !uv || !G.have_external || !ctx_current()) return NULL;
    if (y->width < 2 || y->height < 2 || (y->width & 1) || (y->height & 1) || uv->width != y->width / 2 || uv->height != y->height / 2) return NULL;
    GpuTex *a = gpu_tex_dmabuf(y);
    GpuTex *b = a ? gpu_tex_dmabuf(uv) : NULL;
    GpuYuv *t = b ? calloc(1, sizeof *t) : NULL;
    if (!t) { gpu_tex_free(a); gpu_tex_free(b); return NULL; }
    t->ey = a;
    t->euv = b;
    t->ty = a->id;
    t->tuv = b->id;
    t->ext = 1;
    t->w = y->width;
    t->h = y->height;
    t->gen = G.gen;
    return t;
}

static int yuv_modifier_known(uint32_t fourcc, uint64_t mod) {
    if (!G.create_image) return 0;
    if (!G.query_modifiers) return mod == DRM_FORMAT_MOD_INVALID || mod == DRM_FORMAT_MOD_LINEAR;
    EGLuint64KHR mods[128];
    EGLBoolean ext[128];
    EGLint nm = 0;
    if (!G.query_modifiers(G.dpy, (EGLint)fourcc, 128, mods, ext, &nm)) return 0;
    if (mod == DRM_FORMAT_MOD_INVALID) return 1;            /* implicit: the driver's own layout */
    for (int i = 0; i < nm; i++) if (mods[i] == mod) return 1;
    return 0;
}

int gpu_dmabuf_plane_ok(uint32_t fourcc, uint64_t modifier) {
    if (!G.on || !G.create_image || !G.have_external) return 0;
    if (fourcc != DRM_FORMAT_R8 && fourcc != DRM_FORMAT_GR88 && fourcc != DRM_FORMAT_R16 && fourcc != DRM_FORMAT_GR1616) return 0;
    typedef struct { uint32_t f; uint64_t m; int ok; unsigned gen; } Memo;
    static Memo memo[16];
    for (int i = 0; i < 16; i++) if (memo[i].gen == G.gen && memo[i].f == fourcc && memo[i].m == modifier) return memo[i].ok;
    int ok = yuv_modifier_known(fourcc, modifier);
    static int at;
    memo[at] = (Memo){ fourcc, modifier, ok, G.gen };
    at = (at + 1) % 16;
    return ok;
}

static void quad_full(void) {
    static const GLfloat v[] = { -1, -1, 0, 0,  1, -1, 1, 0,  -1, 1, 0, 1,  1, 1, 1, 1 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static GLuint yfbo;
static unsigned yfbo_gen;

/* draws with the bound program into tex (w × h); 0 ok */
static int fbo_draw(GLuint tex, int w, int h) {
    if (yfbo_gen != G.gen || !yfbo) { glGenFramebuffers(1, &yfbo); yfbo_gen = G.gen; }
    glBindFramebuffer(GL_FRAMEBUFFER, yfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glViewport(0, 0, w, h);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        quad_full();
        glEnable(GL_BLEND);
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok && glGetError() == GL_NO_ERROR ? 0 : -1;
}

/* *dst made or kept as an RGBA texture of w × h */
static GpuTex *rgba_target(GpuTex **dst, int w, int h) {
    GpuTex *t = *dst;
    if (t && (t->external || t->gen != G.gen)) { gpu_tex_free(t); t = NULL; *dst = NULL; }
    int fresh = !t;
    if (!t) {
        t = calloc(1, sizeof *t);
        if (!t) return NULL;
        glGenTextures(1, &t->id);
        glBindTexture(GL_TEXTURE_2D, t->id);
        tex_params(GL_TEXTURE_2D);
        t->gen = G.gen;
    } else glBindTexture(GL_TEXTURE_2D, t->id);
    if (fresh || t->w != w || t->h != h) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    t->w = w;
    t->h = h;
    t->opaque = 1;
    t->swap = 0;
    *dst = t;
    return t;
}

static void matrix4(int matrix, int full, float m4[16]) {
    float m[12];
    nv12_matrix(matrix, full, m);
    /* column-major: (u_m · q).c = Σ_k m[c][k] q[k] */
    for (int c = 0; c < 4; c++) for (int k = 0; k < 4; k++) m4[k * 4 + c] = c < 3 ? m[c * 4 + k] : (k == 3 ? 1.0f : 0.0f);
}

static void bind_plane(int unit, GLuint id, int ext) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(ext ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, id);
}

int gpu_yuv_convert(const GpuYuv *src, GpuTex **dst, int matrix, int full) {
    return src ? gpu_yuv_convert_to(src, dst, src->w, src->h, matrix, full) : -1;
}

int gpu_yuv_convert_to(const GpuYuv *src, GpuTex **dst, int w, int h, int matrix, int full) {
    if (!dst || !gpu_yuv_current(src) || !ctx_current() || w < 1 || h < 1 || w > src->w || h > src->h) return -1;
    GLuint p = yuv_program(YP_CONV, src->ext);
    if (!p) return -1;
    while (glGetError() != GL_NO_ERROR) {}
    GpuTex *t = rgba_target(dst, w, h);
    if (!t) return -1;
    float m4[16];
    matrix4(matrix, full, m4);
    glUseProgram(p);
    bind_plane(1, src->tuv, src->ext);
    bind_plane(0, src->ty, src->ext);
    glUniform1i(glGetUniformLocation(p, "u_y"), 0);
    glUniform1i(glGetUniformLocation(p, "u_uv"), 1);
    glUniformMatrix4fv(glGetUniformLocation(p, "u_m"), 1, GL_FALSE, m4);
    int r = fbo_draw(t->id, t->w, t->h);
    glActiveTexture(GL_TEXTURE0);
    return r;
}

int gpu_pack_current(const GpuPack *k) { return k && G.on && k->gen == G.gen; }
long gpu_pack_bytes(const GpuPack *k) { return k ? (long)k->tw * 4 * k->h + (long)k->tw * 4 * (k->h / 2) : 0; }

void gpu_pack_free(GpuPack *k) {
    if (!k) return;
    if (G.on && k->gen == G.gen && ctx_current()) { glDeleteTextures(1, &k->ty); glDeleteTextures(1, &k->tuv); }
    free(k);
}

GpuPack *gpu_yuv_pack(const GpuYuv *src) {
    if (!gpu_yuv_current(src) || !ctx_current()) return NULL;
    GLuint py = yuv_program(YP_PKY, src->ext), puv = yuv_program(YP_PKUV, src->ext);
    if (!py || !puv) return NULL;
    GpuPack *k = calloc(1, sizeof *k);
    if (!k) return NULL;
    k->w = src->w;
    k->h = src->h;
    k->tw = (src->w + 3) / 4;
    k->gen = G.gen;
    while (glGetError() != GL_NO_ERROR) {}
    glGenTextures(1, &k->ty);
    glGenTextures(1, &k->tuv);
    int hh[2] = { k->h, k->h / 2 };
    GLuint ids[2] = { k->ty, k->tuv };
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, ids[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, k->tw, hh[i], 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    int bad = glGetError() != GL_NO_ERROR;    /* out of GPU memory, among others */
    GLuint pr[2] = { py, puv };
    GLuint srcs[2] = { src->ty, src->tuv };
    float size[2][2] = { { (float)src->w, (float)src->h }, { (float)(src->w / 2), (float)(src->h / 2) } };
    for (int i = 0; i < 2 && !bad; i++) {
        glUseProgram(pr[i]);
        bind_plane(0, srcs[i], src->ext);
        glUniform1i(glGetUniformLocation(pr[i], "u_t"), 0);
        glUniform2f(glGetUniformLocation(pr[i], "u_size"), size[i][0], size[i][1]);
        glUniform2f(glGetUniformLocation(pr[i], "u_out"), (float)k->tw, (float)hh[i]);
        if (fbo_draw(ids[i], k->tw, hh[i])) bad = 1;
    }
    if (bad) { gpu_pack_free(k); return NULL; }
    return k;
}

int gpu_pack_convert(const GpuPack *k, GpuTex **dst, int matrix, int full) {
    if (!dst || !gpu_pack_current(k) || !ctx_current()) return -1;
    GLuint p = yuv_program(YP_UNPK, 0);
    if (!p) return -1;
    while (glGetError() != GL_NO_ERROR) {}
    GpuTex *t = rgba_target(dst, k->w, k->h);
    if (!t) return -1;
    float m4[16];
    matrix4(matrix, full, m4);
    glUseProgram(p);
    bind_plane(1, k->tuv, 0);
    bind_plane(0, k->ty, 0);
    glUniform1i(glGetUniformLocation(p, "u_y"), 0);
    glUniform1i(glGetUniformLocation(p, "u_uv"), 1);
    glUniform2f(glGetUniformLocation(p, "u_px"), (float)k->w, (float)k->h);
    glUniform1f(glGetUniformLocation(p, "u_tw"), (float)k->tw);
    glUniformMatrix4fv(glGetUniformLocation(p, "u_m"), 1, GL_FALSE, m4);
    int r = fbo_draw(t->id, t->w, t->h);
    glActiveTexture(GL_TEXTURE0);
    return r;
}

int gpu_pack_ready(void) {
    if (!G.on || !ctx_current()) return 0;
    return yuv_program(YP_PKY, 0) && yuv_program(YP_PKUV, 0) && yuv_program(YP_UNPK, 0);
}

/* ---- drawing ---- */

void gpu_draw(GpuTex *t, float dx, float dy, float dw, float dh, float sx, float sy, float sw, float sh, Rect clip, float op) {
    const float uv[8] = { sx, sy, sx + sw, sy, sx, sy + sh, sx + sw, sy + sh };
    gpu_draw_uv(t, dx, dy, dw, dh, uv, clip, op);
}

void gpu_draw_uv(GpuTex *t, float dx, float dy, float dw, float dh, const float uv[8], Rect clip, float op) {
    if (!G.on || !G.cur || !t || t->gen != G.gen || dw <= 0 || dh <= 0 || op <= 0.004f || t->w <= 0 || t->h <= 0) return;
    const int W = G.cur->W, H = G.cur->H;           /* the display being drawn */
    if (clip.x0 < 0) clip.x0 = 0;
    if (clip.y0 < 0) clip.y0 = 0;
    if (clip.x1 > W) clip.x1 = W;
    if (clip.y1 > H) clip.y1 = H;
    if (clip.x1 <= clip.x0 || clip.y1 <= clip.y0) return;
    int p = t->external ? 1 : 0;
    glUseProgram(G.prog[p]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(t->external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, t->id);
    glUniform1i(G.u_tex[p], 0);
    glUniform1f(G.u_op[p], op);
    glUniform1f(G.u_opaque[p], t->opaque ? 1.0f : 0.0f);
    glUniform1f(G.u_swap[p], t->swap ? 1.0f : 0.0f);
    glEnable(GL_SCISSOR_TEST);
    glScissor(clip.x0, H - clip.y1, clip.x1 - clip.x0, clip.y1 - clip.y0);
    float x0 = dx / W * 2 - 1, x1 = (dx + dw) / W * 2 - 1;
    float y0 = 1 - dy / H * 2, y1 = 1 - (dy + dh) / H * 2;
    const float tw = (float)t->w, th = (float)t->h;
    const GLfloat v[] = { x0, y0, uv[0] / tw, uv[1] / th,  x1, y0, uv[2] / tw, uv[3] / th,
                          x0, y1, uv[4] / tw, uv[5] / th,  x1, y1, uv[6] / tw, uv[7] / th };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_SCISSOR_TEST);
}

static struct { uint32_t *px; int w, h, done; } rb;

void gpu_read_next(uint32_t *px, int w, int h) { rb.px = px; rb.w = w; rb.h = h; rb.done = 0; }
int gpu_read_done(void) { return rb.done; }

/* RGBA bytes, bottom row first (GL) → 0xAARRGGBB words, top row first; opaque. */
static void read_back(int W, int H) {
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rb.px);
    if (glGetError() != GL_NO_ERROR) return;
    for (int y = 0; y < (H + 1) / 2; y++) {
        uint32_t *a = rb.px + (size_t)y * W, *b = rb.px + (size_t)(H - 1 - y) * W;
        for (int x = 0; x < W; x++) {
            const uint8_t *pa = (const uint8_t *)&a[x], *pb = (const uint8_t *)&b[x];
            uint32_t va = 0xff000000u | (uint32_t)pa[0] << 16 | (uint32_t)pa[1] << 8 | pa[2];
            uint32_t vb = 0xff000000u | (uint32_t)pb[0] << 16 | (uint32_t)pb[1] << 8 | pb[2];
            a[x] = vb;
            b[x] = va;
        }
    }
    rb.done = 1;
}

/* Uploads r of px (sw wide, stride px a row) into the bound texture (texw wide). Without
 * GL_EXT_unpack_subimage GLES2 reads rows of the upload's own width: whole rows then, in one call when
 * they are contiguous in px, else one row at a time. Returns the pixels sent. */
static long upload_rect(const uint32_t *px, int stride, int sw, int texw, Rect r) {
    if (r.x1 <= r.x0 || r.y1 <= r.y0) return 0;
    GLenum fmt = G.bgra ? GL_BGRA_EXT : GL_RGBA;
    if (G.unpack_sub) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride);
        glTexSubImage2D(GL_TEXTURE_2D, 0, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0, fmt, GL_UNSIGNED_BYTE,
                        px + (size_t)r.y0 * stride + r.x0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
        return (long)(r.x1 - r.x0) * (r.y1 - r.y0);
    }
    int w = sw < texw ? sw : texw;
    if (stride == w) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, r.y0, w, r.y1 - r.y0, fmt, GL_UNSIGNED_BYTE, px + (size_t)r.y0 * stride);
        return (long)w * (r.y1 - r.y0);
    }
    for (int y = r.y0; y < r.y1; y++)
        glTexSubImage2D(GL_TEXTURE_2D, 0, r.x0, y, r.x1 - r.x0, 1, fmt, GL_UNSIGNED_BYTE, px + (size_t)y * stride + r.x0);
    return (long)(r.x1 - r.x0) * (r.y1 - r.y0);
}

/* ---- GPU drag: the dragged window on the GPU ---- */

static GLuint gdrag_texture(void) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* Texture i gets output out's px (w × h, stride w): all of it if its size changed, it held another
 * output's (or nothing) or full, else the n rectangles. */
static long gdrag_upload(int i, int out, const uint32_t *px, int w, int h, const Rect *r, int n, int full) {
    glBindTexture(GL_TEXTURE_2D, G.gdragtex[i]);
    int other = G.gdragtout[i] != out;
    G.gdragtout[i] = out;
    if (full || other || G.gdragtw[i] != w || G.gdragth[i] != h) {
        GLenum fmt = G.bgra ? GL_BGRA_EXT : GL_RGBA;
        if (G.gdragtw[i] != w || G.gdragth[i] != h) glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, px);
        else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, GL_UNSIGNED_BYTE, px);
        G.gdragtw[i] = w;
        G.gdragth[i] = h;
        return (long)w * h;
    }
    long sent = 0;
    for (int k = 0; k < n; k++) {
        Rect q = r[k];
        if (q.x0 < 0) q.x0 = 0;
        if (q.y0 < 0) q.y0 = 0;
        if (q.x1 > w) q.x1 = w;
        if (q.y1 > h) q.y1 = h;
        sent += upload_rect(px, w, w, w, q);
    }
    return sent;
}

/* The GPU drag quad: the shader inside w (a framebuffer W × H px), blending and dithering off (it writes the
 * result). The quad covers the framebuffer and carries each corner's pixel position (top-down) for v_px. */
static void gdrag_draw(int W, int H, Rect w, float lx, float ly, float lw, float lh, float bw, float bh) {
    glUseProgram(G.gdragprog);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, G.gdragtex[1]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, G.gdragtex[0]);
    glUniform1i(G.gu_below, 0);
    glUniform1i(G.gu_layer, 1);
    glUniform2f(G.gu_bsize, bw, bh);
    glUniform4f(G.gu_lrect, lx, ly, lw, lh);
    glUniform1f(G.gu_swap, G.bgra ? 0.0f : 1.0f);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);
    glEnable(GL_SCISSOR_TEST);
    glScissor(w.x0, H - w.y1, w.x1 - w.x0, w.y1 - w.y0);
    const GLfloat fw = (GLfloat)W, fh = (GLfloat)H;
    const GLfloat v[] = { -1, -1, 0, fh,  1, -1, fw, fh,  -1, 1, 0, 0,  1, 1, fw, 0 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DITHER);
    glEnable(GL_BLEND);
}
/* The self-test's cases, in order: every valid premultiplied (alpha a, channel c ≤ a) with every
 * destination channel d (three cases a pixel, one per colour channel, the destination alpha varying),
 * then every (source alpha, destination alpha), then random valid pixels to fill the last tile. */
typedef struct { int a, j, grid; uint32_t p, rng; } GdragGen;
static int gdrag_gen_done(const GdragGen *g) { return g->a > 255 && g->grid >= 65536; }
static void gdrag_gen(GdragGen *g, uint32_t *s, uint32_t *d) {
    g->p++;
    g->rng ^= g->rng << 13; g->rng ^= g->rng >> 17; g->rng ^= g->rng << 5;
    if (g->a <= 255) {
        uint32_t a = (uint32_t)g->a, n = (a + 1) * 256, sc = 0, dc = 0;
        for (int k = 0; k < 3; k++) {
            uint32_t j = (uint32_t)g->j + (uint32_t)k < n ? (uint32_t)g->j + (uint32_t)k : n - 1;
            sc = sc << 8 | j >> 8;
            dc = dc << 8 | (j & 255);
        }
        *s = a << 24 | sc;
        *d = ((g->p * 151u + 7u) & 255u) << 24 | dc;
        g->j += 3;
        if ((uint32_t)g->j >= n) { g->a++; g->j = 0; }
        return;
    }
    if (g->grid < 65536) {
        uint32_t sa = (uint32_t)g->grid & 255, da = (uint32_t)g->grid >> 8, c = sa ? g->rng % (sa + 1) : 0;
        *s = sa << 24 | c << 16 | (sa - c) << 8 | c / 2;
        *d = da << 24 | (g->rng >> 8 & 0xffffff);
        g->grid++;
        return;
    }
    uint32_t a = g->rng >> 24, r = g->rng & 0xffffff;
    *s = a << 24 | (a ? (r % (a + 1)) << 16 | ((r >> 8) % (a + 1)) << 8 | (r >> 16) % (a + 1) : 0);
    *d = g->rng * 2654435761u;
}

/* One self-test pass in the bound fw × fh framebuffer: the layer lay (lw × lh) at (lx, ly)
 * over bel (fw × fh), drawn where the layer is and compared with canvas_blit on the CPU into ref; the rest
 * of the framebuffer must stay as cleared (0). rd (fw × fh × 4 bytes) may be bel's memory: it is read
 * back after the upload. -1 on a GL error, else the pixels that differ (the first few logged). */
static long gdrag_pass(int fw, int fh, const uint32_t *bel, const uint32_t *lay, int lx, int ly, int lw,
                    int lh, uint32_t *ref, uint8_t *rd, const char *what) {
    memcpy(ref, bel, (size_t)fw * fh * 4);
    Canvas c;
    canvas_init(&c, ref, fw, fh, fw);
    canvas_blit(&c, lay, lw, lh, lx, ly, 0, 1, 0);
    Rect w = { lx > 0 ? lx : 0, ly > 0 ? ly : 0, lx + lw < fw ? lx + lw : fw, ly + lh < fh ? ly + lh : fh };
    for (int y = 0; y < fh; y++)
        for (int x = 0; x < fw; x++)
            if (x < w.x0 || x >= w.x1 || y < w.y0 || y >= w.y1) ref[(size_t)y * fw + x] = 0;
    gdrag_upload(0, -1, bel, fw, fh, NULL, 0, 1);
    gdrag_upload(1, -1, lay, lw, lh, NULL, 0, 1);
    glViewport(0, 0, fw, fh);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    gdrag_draw(fw, fh, w, (float)lx, (float)ly, (float)lw, (float)lh, (float)fw, (float)fh);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, rd);
    if (glGetError() != GL_NO_ERROR) return -1;
    long differ = 0;
    for (int y = 0; y < fh; y++) {                  /* GL's rows are bottom to top */
        const uint8_t *b = rd + (size_t)(fh - 1 - y) * fw * 4;
        const uint32_t *e = ref + (size_t)y * fw;
        for (int x = 0; x < fw; x++) {
            uint32_t v = (uint32_t)b[4 * x + 3] << 24 | (uint32_t)b[4 * x] << 16 | (uint32_t)b[4 * x + 1] << 8 | b[4 * x + 2];
            if (v != e[x] && differ++ < 4)
                fprintf(stderr, "gpu: GPU drag self-test (%s): pixel (%d, %d): CPU %08x, GPU %08x\n", what, x, y, e[x], v);
        }
    }
    return differ;
}

/* The self-test, two parts. Arithmetic: every case of gdrag_gen in 1024 × 1024 tiles (the layer over the
 * copy, pixel for pixel). Placement: an fw × fh framebuffer (the output's size, at least 4096 × 2048 when
 * the GPU allows it) and a copy of that size, with the layer at an odd offset reaching its far edges,
 * every pixel a different random value: a texel read from the wrong place (gl_FragCoord or a varying
 * with too few bits, the far rows and columns) shows. 0 if every pixel is the CPU's. */
static int gdrag_selftest(int fw, int fh, long *cases, long *differ) {
    enum { T = GDRAG_TILE, LX = GDRAG_LX, LY = GDRAG_LY };
    *cases = *differ = 0;
    size_t n = (size_t)fw * fh > (size_t)T * T ? (size_t)fw * fh : (size_t)T * T;
    uint32_t *s = malloc(n * 4), *d = malloc(n * 4), *ref = malloc(n * 4);
    GLuint ft = 0, fbo = 0;
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++) {}    /* only the test's own errors count */
    int ok = s && d && ref;
    if (ok) {
        glGenTextures(1, &ft);
        glBindTexture(GL_TEXTURE_2D, ft);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, T, T, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ft, 0);
        ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    GdragGen g = { 0, 0, 0, 0, 2463534242u };
    while (ok && !gdrag_gen_done(&g)) {
        for (size_t i = 0; i < (size_t)T * T; i++) gdrag_gen(&g, &s[i], &d[i]);
        long k = gdrag_pass(T, T, d, s, 0, 0, T, T, ref, (uint8_t *)d, "arithmetic");
        if (k < 0) { ok = 0; break; }
        *differ += k;
        *cases += (long)T * T;
    }
    if (ok) {                                       /* placement */
        glBindTexture(GL_TEXTURE_2D, ft);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    if (ok) {
        uint32_t r = 88172645u;
        for (size_t i = 0; i < (size_t)fw * fh; i++) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            d[i] = r * 2654435761u;
            uint32_t a = r >> 24;
            s[i] = a << 24 | (a ? (r % (a + 1)) << 16 | ((r >> 8) % (a + 1)) << 8 | (r >> 16) % (a + 1) : 0);
        }
        long k = gdrag_pass(fw, fh, d, s, LX, LY, fw - LX, fh - LY, ref, (uint8_t *)d, "placement");
        if (k < 0) ok = 0;
        else {
            *differ += k;
            *cases += (long)(fw - LX) * (fh - LY);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (ft) glDeleteTextures(1, &ft);
    G.gdragtw[0] = G.gdragtw[1] = G.gdragth[0] = G.gdragth[1] = 0;    /* hold nothing of an output */
    G.gdragtout[0] = G.gdragtout[1] = -1;
    glDeleteTextures(2, G.gdragtex);
    G.gdragtex[0] = gdrag_texture();
    G.gdragtex[1] = gdrag_texture();
    free(s); free(d); free(ref);
    return ok && !*differ ? 0 : -1;
}

/* The self-test's outcome per GL renderer and version, kept across contexts (gpu_shutdown wipes G): a
 * GPU restart, or a later context on the same driver, does not run it again. */
static struct { char key[320]; int pass, w, h; } gdrag_memo;

static void gdrag_teardown(void) {
    if (G.gdragprog) glDeleteProgram(G.gdragprog);
    glDeleteTextures(2, G.gdragtex);
    G.gdragprog = G.gdragtex[0] = G.gdragtex[1] = 0;
}

/* GPU drag's program and textures in this context, and the self-test (or its earlier outcome). 1 = on. */
static int gdrag_setup(void) {
    const char *e = getenv("LORD_GPU_DRAG");
    /* on by default since the Raspberry Pi 5 run of 2026-10-31, where it applies to the whole drag (docs/GPU.md) */
    if (e && !strcmp(e, "0")) { fprintf(stderr, "gpu: GPU drag off (LORD_GPU_DRAG=0): the dragged window is composed by the CPU\n"); return 0; }
    GOut *o = G.cur && G.cur->used ? G.cur : &G.out[0];
    if (!o->used || !eglMakeCurrent(G.dpy, o->surf, o->surf, G.ctx)) return 0;
    GLint range[2] = { 0, 0 }, prec = 0;
    glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &prec);
    if (prec < 23 || range[1] < 24) {
        fprintf(stderr, "gpu: GPU drag off: the fragment shader's floats have %d bits (24 needed)\n", (int)prec + 1);
        return 0;
    }
    GLuint v = compile(GL_VERTEX_SHADER, vs_gdrag), f = compile(GL_FRAGMENT_SHADER, fs_gdrag);
    GLint linked = 0;
    if (v && f) {
        G.gdragprog = glCreateProgram();
        glAttachShader(G.gdragprog, v);
        glAttachShader(G.gdragprog, f);
        glBindAttribLocation(G.gdragprog, 0, "a_pos");
        glBindAttribLocation(G.gdragprog, 1, "a_px");
        glLinkProgram(G.gdragprog);
        glGetProgramiv(G.gdragprog, GL_LINK_STATUS, &linked);
    }
    if (v) glDeleteShader(v);
    if (f) glDeleteShader(f);
    if (!linked) { gdrag_teardown(); fprintf(stderr, "gpu: GPU drag off: its shader did not build\n"); return 0; }
    G.gu_below = glGetUniformLocation(G.gdragprog, "u_below");
    G.gu_layer = glGetUniformLocation(G.gdragprog, "u_layer");
    G.gu_bsize = glGetUniformLocation(G.gdragprog, "u_bsize");
    G.gu_lrect = glGetUniformLocation(G.gdragprog, "u_lrect");
    G.gu_swap = glGetUniformLocation(G.gdragprog, "u_swap");
    G.gdragtex[0] = gdrag_texture();
    G.gdragtex[1] = gdrag_texture();
    G.gdragtout[0] = G.gdragtout[1] = -1;
    /* the placement part's size: the largest output now, at least 4096 × 2048, within the GPU's limits */
    GLint maxtex = 0, vp[2] = { 0, 0 };
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, vp);
    int fw = 4096, fh = 2048;
    for (int i = 0; i < OUT_MAX; i++)
        if (G.out[i].used) {
            if (G.out[i].W > fw) fw = G.out[i].W;
            if (G.out[i].H > fh) fh = G.out[i].H;
        }
    int lw = maxtex < vp[0] ? maxtex : vp[0], lh = maxtex < vp[1] ? maxtex : vp[1];
    if (fw > lw) fw = lw;
    if (fh > lh) fh = lh;
    const char *ver = (const char *)glGetString(GL_VERSION);
    char key[sizeof gdrag_memo.key];
    snprintf(key, sizeof key, "%s|%s", G.renderer, ver ? ver : "?");
    if (!strcmp(key, gdrag_memo.key) && (!gdrag_memo.pass || (gdrag_memo.w >= fw && gdrag_memo.h >= fh))) {
        if (!gdrag_memo.pass) {
            gdrag_teardown();
            fprintf(stderr, "gpu: GPU drag off: its self-test failed earlier with this renderer\n");
            return 0;
        }
        G.gdraglw = gdrag_memo.w;
        G.gdraglh = gdrag_memo.h;
        dbg("GPU drag on: its self-test passed earlier with this renderer (%dx%d)", G.gdraglw, G.gdraglh);
        return 1;
    }
    if (fw < 4096 || fh < 2048)
        fprintf(stderr, "gpu: GPU drag self-test: placement in %dx%d, the largest framebuffer the GPU allows\n", fw, fh);
    long cases = 0, differ = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int bad = fw < GDRAG_TILE || fh < GDRAG_TILE ? -1 : gdrag_selftest(fw, fh, &cases, &differ);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (bad && !differ) {                        /* no pixel differs: it did not run to the end */
        gdrag_teardown();
        fprintf(stderr, "gpu: GPU drag off: its self-test could not run (texture size, memory or framebuffer)\n");
        return 0;
    }
    snprintf(gdrag_memo.key, sizeof gdrag_memo.key, "%s", key);
    gdrag_memo.pass = !bad;
    gdrag_memo.w = fw;
    gdrag_memo.h = fh;
    if (bad) {
        gdrag_teardown();
        fprintf(stderr, "gpu: GPU drag off: the self-test failed (%ld of %ld pixels differ from the CPU's, %.0f ms)\n", differ, cases, ms);
        return 0;
    }
    G.gdraglw = fw;
    G.gdraglh = fh;
    fprintf(stderr, "gpu: GPU drag on: the dragged window is composed on the GPU (self-test: %ld pixels, every valid "
                    "premultiplied combination and a %dx%d placement pass, the same as the CPU's, %.0f ms)\n", cases, fw, fh, ms);
    return 1;
}

int gpu_gdrag_ready(int out) {
    return G.on && G.gdrag == 1 && out >= 0 && out < OUT_MAX && G.out[out].used && G.out[out].W <= G.gdraglw &&
           G.out[out].H <= G.gdraglh;
}

void gpu_gdrag_max(int *lw, int *lh) {
    int on = G.on && G.gdrag == 1;
    *lw = on ? G.gdraglw - GDRAG_LX : 0;
    *lh = on ? G.gdraglh - GDRAG_LY : 0;
}
int gpu_gdrag_holds(int out) { return G.on && G.gdrag == 1 && out >= 0 && G.gdragtout[0] == out && G.gdragtout[1] == out; }

void gpu_gdrag_stats(long *below_px, long *layer_px, long *quads) {
    *below_px = gdrag_bpx;
    *layer_px = gdrag_lpx;
    *quads = gdrag_quads;
    gdrag_bpx = gdrag_lpx = gdrag_quads = 0;
}

/* LORD_GPU_VERIFY: what the quad wrote in w, read back, against the CPU's pixels (RGB: the
 * framebuffer has no alpha; a frame painted by the CPU shows "premultiplied over black", its RGB). */
static void gdrag_verify(GOut *o, const GpuDrag *g) {
    int ww = g->w.x1 - g->w.x0, wh = g->w.y1 - g->w.y0;
    uint8_t *rd = malloc((size_t)ww * wh * 4);
    if (!rd) return;
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(g->w.x0, o->H - g->w.y1, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, rd);
    long differ = 0;
    int worst = 0;
    if (glGetError() == GL_NO_ERROR)
        for (int y = 0; y < wh; y++) {
            const uint8_t *b = rd + (size_t)(wh - 1 - y) * ww * 4;
            const uint32_t *e = g->ref + (size_t)y * ww;
            for (int x = 0; x < ww; x++) {
                int dr = abs((int)b[4 * x] - (int)(e[x] >> 16 & 255)), dg = abs((int)b[4 * x + 1] - (int)(e[x] >> 8 & 255));
                int db = abs((int)b[4 * x + 2] - (int)(e[x] & 255)), dm = dr > dg ? dr : dg;
                if (db > dm) dm = db;
                if (dm) differ++;
                if (dm > worst) worst = dm;
            }
        }
    else differ = -1;
    fprintf(stderr, "gpu-verify: frame %ld W=%dx%d differ=%ld worst=%d\n", g->frame, ww, wh, differ, worst);
    free(rd);
}

/* GPU drag off for this context, for a reason that would come back every frame */
static int gdrag_off(const char *why) {
    G.gdragtout[0] = G.gdragtout[1] = -1;
    G.gdrag = 0;
    fprintf(stderr, "gpu: GPU drag off: %s\n", why);
    return 0;
}

/* The GPU drag part of a frame: the textures brought up to date, then the quad. 0 if it was not drawn. */
static int gdrag_frame(GOut *o, int out, const GpuDrag *g) {
    if (G.gdrag != 1) return 0;
    int sane = g->w.x0 >= 0 && g->w.y0 >= 0 && g->w.x1 <= o->W && g->w.y1 <= o->H && g->w.x1 > g->w.x0 && g->w.y1 > g->w.y0;
    /* coordinates past what the self-test's placement covered are not trusted */
    int tested = o->W <= G.gdraglw && o->H <= G.gdraglh && g->lw <= G.gdraglw - GDRAG_LX && g->lh <= G.gdraglh - GDRAG_LY &&
                 g->bw <= G.gdraglw && g->bh <= G.gdraglh;
    if (g->lpx && g->bpx) {
        if (!sane) return gdrag_off("a window rectangle outside the output");
        if (!tested) return gdrag_off("a layer or output larger than its self-test covered");
        for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++) {}
        double t0 = mono_ms();
        /* another output's texture (or nothing yet), or full: all of it (paint.c sends what the textures
         * lack between frames first, so this is rare) */
        o->times.gdrag_full = g->bfull || g->lfull || G.gdragtout[0] != out || G.gdragtout[1] != out || G.gdragtw[0] != g->bw ||
                           G.gdragth[0] != g->bh || G.gdragtw[1] != g->lw || G.gdragth[1] != g->lh;
        long b = gdrag_upload(0, out, g->bpx, g->bw, g->bh, g->bdirty, g->nb, g->bfull);
        long l = gdrag_upload(1, out, g->lpx, g->lw, g->lh, g->ldirty, g->nl, g->lfull);
        gdrag_bpx += b;
        gdrag_lpx += l;
        o->times.gdrag_px = b + l;
        o->times.gdrag_ms = mono_ms() - t0;
        if (glGetError() != GL_NO_ERROR) return gdrag_off("an upload failed");
    } else if (!sane || !tested || !gpu_gdrag_holds(out) || G.gdragtw[1] != g->lw || G.gdragth[1] != g->lh ||
               G.gdragtw[0] != g->bw || G.gdragth[0] != g->bh)
        return 0;
    gdrag_draw(o->W, o->H, g->w, (float)g->lx, (float)g->ly, (float)g->lw, (float)g->lh, (float)g->bw, (float)g->bh);
    gdrag_quads++;
    if (g->ref) gdrag_verify(o, g);
    return 1;
}

int gpu_frame_ex(int out, const Canvas *scene, const Rect *dmg, int n, const GpuDrag *gdrag, int *gdrag_drawn,
                 void (*draw_layer)(int layer)) {
    if (gdrag_drawn) *gdrag_drawn = 0;
    if (!G.on || out < 0 || out >= OUT_MAX || !G.out[out].used) return -1;
    GOut *o = &G.out[out];
    o->times = (GpuFrameTimes){ 0 };
    if (!eglMakeCurrent(G.dpy, o->surf, o->surf, G.ctx)) { fprintf(stderr, "gpu: eglMakeCurrent failed\n"); return -1; }
    G.cur = o;
    glViewport(0, 0, o->W, o->H);
    /* what changed in the UI */
    double t0 = mono_ms();
    glBindTexture(GL_TEXTURE_2D, o->canvas.id);
    for (int i = 0; i < n; i++) o->times.up_px += upload_rect(scene->px, scene->stride, scene->w, o->W, dmg[i]);
    o->times.up_ms = mono_ms() - t0;
    /* what this frame changes, the window GPU drag writes included */
    Rect all = { 0, 0, o->W, o->H }, cur = { 0, 0, 0, 0 }, reg = all;
    for (int i = 0; i < n; i++) cur = gsc_join(cur, dmg[i]);
    if (gdrag) cur = gsc_join(cur, gdrag->w);
    int partial = 0;
    if (G.scissor) {
        /* a frame with app windows, menus or a cursor is whole, and so is the one after it */
        if (o->sc_quiet && !o->sc_busy) {
            EGLint age = 0;
            if (!eglQuerySurface(G.dpy, o->surf, EGL_BUFFER_AGE_EXT, &age)) age = 0;
            reg = gsc_region(&o->sc, age, cur, o->W, o->H);
            partial = 1;
        }
        o->sc_busy = !o->sc_quiet;
        o->sc_quiet = 0;
    }
    o->times.comp_px = gsc_area(reg);
    glClearColor(0, 0, 0, 1);
    if (partial && o->times.comp_px) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(reg.x0, o->H - reg.y1, reg.x1 - reg.x0, reg.y1 - reg.y0);
    }
    if (!partial || o->times.comp_px) glClear(GL_COLOR_BUFFER_BIT);
    if (partial) glDisable(GL_SCISSOR_TEST);
    if (draw_layer) draw_layer(0);
    gpu_draw(&o->canvas, 0, 0, (float)o->W, (float)o->H, 0, 0, (float)o->W, (float)o->H, reg, 1);
    /* the dragged window over the UI's stale rectangle: nothing of layer 0 is under it (paint.c checks).
     * Not drawn, that rectangle is stale in the UI texture: nothing is shown (-2), the caller paints it */
    if (gdrag) {
        if (!gdrag_frame(o, out, gdrag)) {
            rb.px = NULL;
            gsc_reset(&o->sc);              /* the buffer was written and is not swapped */
            o->sc_busy = 1;
            return -2;
        }
        if (gdrag_drawn) *gdrag_drawn = 1;
    }
    if (draw_layer) draw_layer(1);
    if (rb.px) {
        if (rb.w == o->W && rb.h == o->H) read_back(o->W, o->H);
        rb.px = NULL;
    }
    double t1 = mono_ms();
    if (!eglSwapBuffers(G.dpy, o->surf)) { fprintf(stderr, "gpu: eglSwapBuffers failed\n"); return -1; }
    double t2 = mono_ms();
    if (G.scissor) {
        gsc_push(&o->sc, partial ? cur : all);
        sc_frames++;
        sc_cut += o->times.comp_px < (long)o->W * o->H;
        sc_px += (double)o->times.comp_px;
        sc_all_px += (double)o->W * o->H;
    }
    o->times.swap_ms = t2 - t1;
    take_fence(o);
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(o->gs);
    if (!bo) { fprintf(stderr, "gpu: no GBM buffer\n"); return -1; }
    uint32_t fb = fb_for_bo(bo);
    if (!fb) { gbm_surface_release_buffer(o->gs, bo); return -1; }
    G.frames++;
    int r = present(o, (Shown){ .kind = 1, .bo = bo, .fb = fb });
    o->times.submit_ms = mono_ms() - t2;
    return r;
}

void gpu_scissor_quiet(int out, int quiet) {
    if (G.on && out >= 0 && out < OUT_MAX && G.out[out].used) G.out[out].sc_quiet = quiet;
}

void gpu_frame_times(int out, GpuFrameTimes *t) {
    *t = G.on && out >= 0 && out < OUT_MAX && G.out[out].used ? G.out[out].times : (GpuFrameTimes){ 0 };
}

/* ---- GPU drag's textures between frames (paint_gdrag_feed) ---- */

int gpu_gdrag_feed(int out, int which, const uint32_t *px, int w, int h, Rect r) {
    if (!G.on || G.gdrag != 1 || out < 0 || out >= OUT_MAX || !G.out[out].used || which < 0 || which > 1) return -1;
    GOut *o = &G.out[out];
    /* a texture upload needs the context, not a given surface (G.cur is not updated by every
     * eglMakeCurrent, so the context actually current is asked) */
    if (eglGetCurrentContext() != G.ctx) {
        if (!eglMakeCurrent(G.dpy, o->surf, o->surf, G.ctx)) return -1;
        G.cur = o;
    }
    /* errors left by earlier calls (an import, the cursor) are not this upload's: without this one
     * of them would turn GPU drag off below */
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++) {}
    glBindTexture(GL_TEXTURE_2D, G.gdragtex[which]);
    if (G.gdragtout[which] != out || G.gdragtw[which] != w || G.gdragth[which] != h) {
        if (G.gdragtw[which] != w || G.gdragth[which] != h) {     /* its storage, filled by the slices that follow */
            GLenum fmt = G.bgra ? GL_BGRA_EXT : GL_RGBA;
            glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, NULL);
            G.gdragtw[which] = w;
            G.gdragth[which] = h;
        }
        G.gdragtout[which] = out;
        if (glGetError() != GL_NO_ERROR) { gdrag_off("a texture could not be made"); return -1; }
        return 1;
    }
    Rect q = { r.x0 < 0 ? 0 : r.x0, r.y0 < 0 ? 0 : r.y0, r.x1 > w ? w : r.x1, r.y1 > h ? h : r.y1 };
    long sent = upload_rect(px, w, w, w, q);
    if (which) gdrag_lpx += sent;
    else gdrag_bpx += sent;
    if (sent && glGetError() != GL_NO_ERROR) { gdrag_off("an upload failed"); return -1; }
    return 0;
}

/* ---- when the GPU finished a frame (LordBench's frame phases) ---- */

void gpu_fence_on(int on) { fence_wanted = on != 0; }

/* after eglSwapBuffers: a native fence that signals when everything submitted so far (the frame) is done */
static void take_fence(GOut *o) {
    if (o->fence > 0) { close(o->fence - 1); o->fence = 0; }
    if (!fence_wanted || !G.create_sync) return;
    EGLSyncKHR sy = G.create_sync(G.dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (sy == EGL_NO_SYNC_KHR) return;
    glFlush();                              /* the fence's fd exists once it is flushed */
    int fd = G.dup_fence(G.dpy, sy);
    G.destroy_sync(G.dpy, sy);
    o->fence = fd >= 0 ? fd + 1 : 0;
}

int gpu_fence_time(int out, uint64_t *ns) {
    *ns = 0;
    if (!G.on || out < 0 || out >= OUT_MAX || !G.out[out].used || G.out[out].fence <= 0) return 0;
    GOut *o = &G.out[out];
    struct sync_fence_info fi[4];
    struct sync_file_info info;
    memset(&info, 0, sizeof info);
    memset(fi, 0, sizeof fi);
    int ok = 0;
    int fd = o->fence - 1;
    if (!ioctl(fd, SYNC_IOC_FILE_INFO, &info) && info.num_fences > 0 && info.num_fences <= 4) {
        info.sync_fence_info = (uint64_t)(uintptr_t)fi;
        if (!ioctl(fd, SYNC_IOC_FILE_INFO, &info) && info.status == 1) {
            for (uint32_t i = 0; i < info.num_fences; i++)       /* the last of its fences to signal */
                if (fi[i].timestamp_ns > *ns) *ns = fi[i].timestamp_ns;
            ok = *ns != 0;
        }
    }
    close(fd);
    o->fence = 0;
    return ok;
}

int gpu_frame(int out, const Canvas *scene, Rect dmg, void (*draw_layer)(int layer)) {
    return gpu_frame_ex(out, scene, &dmg, 1, NULL, NULL, draw_layer);
}
/* ---- direct scanout ---- */

int gpu_can_scanout(const GpuDmabuf *d, int w, int h) {
    /* and that the primary plane accepts the modifier (vc4: linear only; not v3d's UIF) */
    return G.on && d->width == w && d->height == h && d->nplanes == 1 &&
           (d->fourcc == DRM_FORMAT_XRGB8888 || d->fourcc == DRM_FORMAT_ARGB8888) &&
           kms_scanout_ok(G.k, DRM_FORMAT_XRGB8888, d->modifier);
}

int gpu_scanout_tear(const GpuDmabuf *d, void (*done)(void *), void *data, int tear) {
    GOut *o = &G.out[0];                /* one display only (wl_try_scanout) */
    if (!G.on || !o->used || o->first) return -1;
    Shown s = { .kind = 2, .done = done, .data = data };
    for (int i = 0; i < d->nplanes; i++)
        if (kms_prime_import(G.k, d->fd[i], &s.handles[i])) {
            for (int j = 0; j < i; j++) kms_close_handle(G.k, s.handles[j]);
            dbg("scanout: PRIME import failed (%s)", strerror(errno));
            return -1;
        }
    /* ARGB on the primary plane is shown as XRGB (the output has nothing below it) */
    uint32_t fourcc = d->fourcc == DRM_FORMAT_ARGB8888 ? DRM_FORMAT_XRGB8888 : d->fourcc;
    if (kms_add_fb2(G.k, (uint32_t)d->width, (uint32_t)d->height, fourcc, s.handles, d->stride, d->offset, d->modifier, &s.fb)) {
        dbg("scanout: ADDFB2 failed (%s)", strerror(errno));
        s.done = NULL;
        s.kind = 2;
        release(o, &s);
        return -1;
    }
    G.scanouts++;
    total_scanouts++;
    gsc_reset(&o->sc);                  /* the UI changed with nothing composed */
    o->sc_busy = 1;
    if (G.debug && (G.scanouts == 1 || G.scanouts % 300 == 0)) dbg("direct scanout: %ld frames (composed %ld)", G.scanouts, G.frames);
    o->k->async_next = tear;            /* (kms_flip_fb: only when the driver can, and back at vblank if it refuses) */
    int r = present(o, s);
    o->k->async_next = 0;
    if (tear && o->k->flip_async && !G.torn++ && G.debug) dbg("direct scanout: asynchronous page flips (tearing-control async)");
    return r;
}

int gpu_scanout(const GpuDmabuf *d, void (*done)(void *), void *data) { return gpu_scanout_tear(d, done, data, 0); }

/* The last flip of that display was asynchronous (its presentation feedback is not vsync'd). */
int gpu_flip_torn(void) { return G.on && G.out[0].used && G.out[0].k && G.out[0].k->flip_async; }

#endif
