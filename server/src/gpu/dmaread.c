/* dmaread.c — a second EGL reader for dmabufs the desktop's GPU can't import (see dmaread.h). Mesa's GBM + EGL +
 * GLES2, as gpu.c (approved 2026-10-16, docs/GPU.md); only built with -DLORD_GPU, stubs otherwise. */
#include "dmaread.h"

#include <dirent.h>
#include <drm/drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef fourcc_mod_get_vendor
#  define fourcc_mod_get_vendor(m) (((m) >> 56) & 0xff)
#endif

int dmaread_mod_intel(uint64_t mod) {
    return mod != DRM_FORMAT_MOD_INVALID && mod != DRM_FORMAT_MOD_LINEAR && fourcc_mod_get_vendor(mod) == DRM_FORMAT_MOD_VENDOR_INTEL;
}

int dmaread_node_valid(const char *node) {
    if (!node || strncmp(node, "/dev/dri/renderD", 16)) return 0;
    const char *n = node + 16;
    size_t len = strlen(n);
    if (len < 3 || len > 4) return 0;
    for (size_t i = 0; i < len; i++) if (n[i] < '0' || n[i] > '9') return 0;
    int v = atoi(n);
    return v >= 128 && v <= 1023;
}

static int read_line(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    int ok = fgets(buf, (int)n, f) != NULL;
    fclose(f);
    if (!ok) return -1;
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

const char *dmaread_intel_node(void) {
    static int looked;
    static char found[32];
    if (looked) return found[0] ? found : NULL;
    looked = 1;
    DIR *d = opendir("/sys/class/drm");
    if (!d) return NULL;
    int best = -1;                      /* 1: GVT-g on, 0: an Intel GPU without it */
    for (struct dirent *e; (e = readdir(d));) {
        char node[32];
        if (strncmp(e->d_name, "renderD", 7) || strlen(e->d_name) > 11) continue;
        snprintf(node, sizeof node, "/dev/dri/%s", e->d_name);
        if (!dmaread_node_valid(node)) continue;
        char p[128], v[32];
        snprintf(p, sizeof p, "/sys/class/drm/%s/device/vendor", e->d_name);
        if (read_line(p, v, sizeof v) || strcmp(v, "0x8086")) continue;
        struct stat st;
        if (stat(node, &st) || !S_ISCHR(st.st_mode)) continue;
        snprintf(p, sizeof p, "/sys/class/drm/%s/device/mdev_supported_types", e->d_name);
        int gvt = !stat(p, &st) && S_ISDIR(st.st_mode);
        if (gvt > best || (gvt == best && strcmp(node, found) < 0)) { best = gvt; snprintf(found, sizeof found, "%s", node); }
    }
    closedir(d);
    return found[0] ? found : NULL;
}

#ifndef LORD_GPU

DmaImg *dmaread_import(const char *node, int fd, int w, int h, int stride, uint32_t fourcc, uint64_t mod) {
    (void)node; (void)fd; (void)w; (void)h; (void)stride; (void)fourcc; (void)mod;
    return NULL;
}
int  dmaread_rows(DmaImg *im, uint8_t *px, int y, int h, int flip) { (void)im; (void)px; (void)y; (void)h; (void)flip; return -1; }
int  dmaread_rows_new(DmaImg *im, uint8_t *px, int y, int h, int flip, const void *key) { (void)im; (void)px; (void)y; (void)h; (void)flip; (void)key; return -1; }
DmaOut *dmaread_out_new(const DmaImg *like) { (void)like; return NULL; }
int  dmaread_out_fd(const DmaOut *o, int *stride) { (void)o; (void)stride; return -1; }
int  dmaread_copy(DmaImg *src, DmaOut *dst, int y, int h, int flip, const DmaOut *prev) { (void)src; (void)dst; (void)y; (void)h; (void)flip; (void)prev; return -1; }
int  dmaread_changed(DmaOut *o) { (void)o; return -1; }
void dmaread_out_free(DmaOut *o) { (void)o; }
void dmaread_free(DmaImg *im) { (void)im; }
void dmaread_close_all(void) {}

#else

#include <errno.h>
#include <fcntl.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#ifndef EGL_NO_CONFIG_KHR
#  define EGL_NO_CONFIG_KHR ((EGLConfig)0)
#endif
#ifndef GL_BGRA_EXT
#  define GL_BGRA_EXT 0x80E1
#endif

#ifndef GL_ANY_SAMPLES_PASSED_EXT
#  define GL_ANY_SAMPLES_PASSED_EXT 0x8C2F
#  define GL_QUERY_RESULT_EXT 0x8866
#  define GL_QUERY_RESULT_AVAILABLE_EXT 0x8867
#endif
typedef void (*GenQueries)(GLsizei, GLuint *);
typedef void (*DelQueries)(GLsizei, const GLuint *);
typedef void (*BeginQuery)(GLenum, GLuint);
typedef void (*EndQuery)(GLenum);
typedef void (*GetQueryU)(GLuint, GLenum, GLuint *);

#define DEV_MAX 2

typedef struct {
    char node[32];
    int used, bad, fd;
    struct gbm_device *gbm;
    EGLDisplay dpy;
    EGLContext ctx;
    int mods, bgra;
    GLuint prog, fbo, ftex;
    GLint u_tex;
    int fw, fh;
    const void *fkey;                   /* whose picture the framebuffer holds (dmaread_rows_new), NULL: nobody's */
    /* the change test (GL_EXT_occlusion_query_boolean): a pass that keeps only the pixels that differ from the last
     * picture, drawn into a throwaway target of the picture's size; "any sample passed" is the answer */
    int query;
    GLuint dprog, dfbo, drb, rq;
    GLint d_tex, d_prev, d_size;
    int dw, dh;
    GenQueries gen_q;
    DelQueries del_q;
    BeginQuery begin_q;
    EndQuery end_q;
    GetQueryU get_q;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
} Dev;

struct DmaImg {
    Dev *dev;
    EGLImageKHR img;
    GLuint tex;
    int w, h;
};

/* a linear copy on the reader's device, exported for the desktop's GPU */
struct DmaOut {
    Dev *dev;
    struct gbm_bo *bo;
    int fd, stride, w, h;
    EGLImageKHR img;
    GLuint tex, fbo, q;
    int q_on;                           /* q holds the change test of the last copy */
};

static Dev devs[DEV_MAX];

static int has_ext(const char *list, const char *name) {
    size_t n = strlen(name);
    for (const char *p = list; p && (p = strstr(p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return 1;
    return 0;
}

/* Whatever was current (gpu.c's context and surfaces, or nothing) is put back by leave() */
typedef struct { EGLDisplay d; EGLSurface draw, read; EGLContext c; } Cur;

static int enter(Dev *v, Cur *s) {
    s->d = eglGetCurrentDisplay();
    s->c = eglGetCurrentContext();
    s->draw = eglGetCurrentSurface(EGL_DRAW);
    s->read = eglGetCurrentSurface(EGL_READ);
    if (s->c == v->ctx) return 0;
    return eglMakeCurrent(v->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, v->ctx) ? 0 : -1;
}

static void leave(Dev *v, const Cur *s) {
    if (s->c == v->ctx) return;
    if (s->c != EGL_NO_CONTEXT && s->d != EGL_NO_DISPLAY) eglMakeCurrent(s->d, s->draw, s->read, s->c);
    else eglMakeCurrent(v->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static const char *vs_src =
    "attribute vec2 a_pos; attribute vec2 a_uv; varying vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char *fs_src =
    "#extension GL_OES_EGL_image_external : require\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
    "varying vec2 v_uv; uniform samplerExternalOES u_tex;\n"
    "void main() { gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0); }\n";

/* the change test: a pixel that's the same as the last picture's is dropped; prev holds that picture as drawn */
static const char *dfs_src =
    "#extension GL_OES_EGL_image_external : require\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
    "varying vec2 v_uv; uniform samplerExternalOES u_tex; uniform sampler2D u_prev; uniform vec2 u_size;\n"
    "void main() { vec3 a = texture2D(u_tex, v_uv).rgb, b = texture2D(u_prev, gl_FragCoord.xy / u_size).rgb;\n"
    "  if (all(equal(a, b))) discard; gl_FragColor = vec4(1.0); }\n";

static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(s); return 0; }
    return s;
}

static GLuint link_prog(const char *fsrc) {
    GLuint vs = shader(GL_VERTEX_SHADER, vs_src), fs = shader(GL_FRAGMENT_SHADER, fsrc);
    if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_uv");
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(p); return 0; }
    return p;
}

static int make_prog(Dev *v) {
    v->prog = link_prog(fs_src);
    if (!v->prog) return -1;
    v->u_tex = glGetUniformLocation(v->prog, "u_tex");
    /* the change test is optional: without it every frame counts as changed */
    if (v->query && (v->dprog = link_prog(dfs_src))) {
        v->d_tex = glGetUniformLocation(v->dprog, "u_tex");
        v->d_prev = glGetUniformLocation(v->dprog, "u_prev");
        v->d_size = glGetUniformLocation(v->dprog, "u_size");
    } else v->query = 0;
    return 0;
}

static void dev_close(Dev *v) {
    if (v->dpy != EGL_NO_DISPLAY) {
        if (v->ctx != EGL_NO_CONTEXT) {
            Cur s;
            if (!enter(v, &s)) {
                if (v->fbo) glDeleteFramebuffers(1, &v->fbo);
                if (v->ftex) glDeleteTextures(1, &v->ftex);
                if (v->prog) glDeleteProgram(v->prog);
                if (v->dfbo) glDeleteFramebuffers(1, &v->dfbo);
                if (v->drb) glDeleteRenderbuffers(1, &v->drb);
                if (v->dprog) glDeleteProgram(v->dprog);
                if (v->rq) v->del_q(1, &v->rq);
                leave(v, &s);
            }
            eglDestroyContext(v->dpy, v->ctx);
        }
        eglTerminate(v->dpy);
    }
    if (v->gbm) gbm_device_destroy(v->gbm);
    if (v->fd >= 0) close(v->fd);
    memset(v, 0, sizeof *v);
    v->fd = -1;
    v->dpy = EGL_NO_DISPLAY;
    v->ctx = EGL_NO_CONTEXT;
}

static int dev_open(Dev *v, const char *node) {
    memset(v, 0, sizeof *v);
    v->fd = -1;
    v->dpy = EGL_NO_DISPLAY;
    v->ctx = EGL_NO_CONTEXT;
    snprintf(v->node, sizeof v->node, "%s", node);
    v->used = 1;
    const char *why = NULL;
    struct stat st;
    v->fd = open(node, O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (v->fd < 0) { why = strerror(errno); goto fail; }
    if (fstat(v->fd, &st) || !S_ISCHR(st.st_mode) || major(st.st_rdev) != 226) { why = "not a DRM device"; goto fail; }
    v->gbm = gbm_create_device(v->fd);
    if (!v->gbm) { why = "no GBM"; goto fail; }
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    v->dpy = get_display ? get_display(EGL_PLATFORM_GBM_KHR, v->gbm, NULL) : eglGetDisplay((EGLNativeDisplayType)v->gbm);
    EGLint maj, min;
    if (v->dpy == EGL_NO_DISPLAY || !eglInitialize(v->dpy, &maj, &min)) { v->dpy = EGL_NO_DISPLAY; why = "EGL did not start"; goto fail; }
    const char *exts = eglQueryString(v->dpy, EGL_EXTENSIONS);
    if (!has_ext(exts, "EGL_EXT_image_dma_buf_import") || !has_ext(exts, "EGL_KHR_surfaceless_context")) { why = "no dmabuf import or surfaceless context"; goto fail; }
    v->mods = has_ext(exts, "EGL_EXT_image_dma_buf_import_modifiers");
    v->create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    v->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    v->image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!v->create_image || !v->destroy_image || !v->image_target) { why = "no EGLImage functions"; goto fail; }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) { why = "no GLES"; goto fail; }
    EGLConfig cfg = EGL_NO_CONFIG_KHR;
    if (!has_ext(exts, "EGL_KHR_no_config_context")) {
        static const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
        EGLint n = 0;
        if (!eglChooseConfig(v->dpy, cfg_attr, &cfg, 1, &n) || n < 1) { why = "no EGL config"; goto fail; }
    }
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    v->ctx = eglCreateContext(v->dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (v->ctx == EGL_NO_CONTEXT) { why = "no context"; goto fail; }
    Cur s;
    if (enter(v, &s)) { why = "the context can't be made current"; goto fail; }
    const char *gl_exts = (const char *)glGetString(GL_EXTENSIONS);
    int ext_ok = has_ext(gl_exts, "GL_OES_EGL_image_external");
    v->bgra = has_ext(gl_exts, "GL_EXT_read_format_bgra");
    if (has_ext(gl_exts, "GL_EXT_occlusion_query_boolean")) {
        v->gen_q = (GenQueries)eglGetProcAddress("glGenQueriesEXT");
        v->del_q = (DelQueries)eglGetProcAddress("glDeleteQueriesEXT");
        v->begin_q = (BeginQuery)eglGetProcAddress("glBeginQueryEXT");
        v->end_q = (EndQuery)eglGetProcAddress("glEndQueryEXT");
        v->get_q = (GetQueryU)eglGetProcAddress("glGetQueryObjectuivEXT");
        v->query = v->gen_q && v->del_q && v->begin_q && v->end_q && v->get_q && !getenv("LORD_DMAREAD_NOTEST");
    }
    int prog_ok = ext_ok && !make_prog(v);
    leave(v, &s);
    if (!ext_ok) { why = "no GL_OES_EGL_image_external"; goto fail; }
    if (!prog_ok) { why = "shaders failed"; goto fail; }
    return 0;
fail:
    fprintf(stderr, "dmaread: %s can't read dmabufs: %s\n", node, why ? why : "?");
    dev_close(v);
    snprintf(v->node, sizeof v->node, "%s", node);
    v->used = 1;
    v->bad = 1;
    return -1;
}

static Dev *dev_get(const char *node) {
    if (!dmaread_node_valid(node)) return NULL;
    for (int i = 0; i < DEV_MAX; i++)
        if (devs[i].used && !strcmp(devs[i].node, node)) return devs[i].bad ? NULL : &devs[i];
    for (int i = 0; i < DEV_MAX; i++)
        if (!devs[i].used) return dev_open(&devs[i], node) ? NULL : &devs[i];
    return NULL;
}

static EGLImageKHR import_image(Dev *v, int fd, int w, int h, int stride, uint32_t fourcc, uint64_t mod, int with_mod) {
    EGLint attr[24];
    int a = 0;
    attr[a++] = EGL_WIDTH; attr[a++] = w;
    attr[a++] = EGL_HEIGHT; attr[a++] = h;
    attr[a++] = EGL_LINUX_DRM_FOURCC_EXT; attr[a++] = (EGLint)fourcc;
    attr[a++] = EGL_DMA_BUF_PLANE0_FD_EXT; attr[a++] = fd;
    attr[a++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attr[a++] = 0;
    attr[a++] = EGL_DMA_BUF_PLANE0_PITCH_EXT; attr[a++] = stride;
    if (with_mod && v->mods && mod != DRM_FORMAT_MOD_INVALID) {
        attr[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; attr[a++] = (EGLint)(mod & 0xffffffff);
        attr[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; attr[a++] = (EGLint)(mod >> 32);
    }
    attr[a] = EGL_NONE;
    return v->create_image(v->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attr);
}

DmaImg *dmaread_import(const char *node, int fd, int w, int h, int stride, uint32_t fourcc, uint64_t mod) {
    if (fd < 0 || w <= 0 || h <= 0 || w > 8192 || h > 8192 || stride < w * 4 || stride > 65536) return NULL;
    Dev *v = dev_get(node);
    if (!v) return NULL;
    /* a tiled modifier the EGL doesn't take is refused by create_image; with modifiers unsupported, only linear */
    if (!v->mods && mod != DRM_FORMAT_MOD_LINEAR && mod != DRM_FORMAT_MOD_INVALID) return NULL;
    EGLImageKHR img = import_image(v, fd, w, h, stride, fourcc, mod, 1);
    /* linear without an explicit modifier: some drivers only accept the implicit one */
    if (img == EGL_NO_IMAGE_KHR && mod == DRM_FORMAT_MOD_LINEAR) img = import_image(v, fd, w, h, stride, fourcc, mod, 0);
    if (img == EGL_NO_IMAGE_KHR) return NULL;
    DmaImg *im = calloc(1, sizeof *im);
    Cur s;
    if (!im || enter(v, &s)) { free(im); v->destroy_image(v->dpy, img); return NULL; }
    while (glGetError() != GL_NO_ERROR) {}
    glGenTextures(1, &im->tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, im->tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    v->image_target(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)img);
    int ok = glGetError() == GL_NO_ERROR;
    if (!ok) glDeleteTextures(1, &im->tex);
    leave(v, &s);
    if (!ok) { v->destroy_image(v->dpy, img); free(im); return NULL; }
    im->dev = v;
    im->img = img;
    im->w = w;
    im->h = h;
    return im;
}

/* The picture's rows [y, y + h) drawn with the bound program into the bound framebuffer (w × h of the import): its
 * first row at y = -1, so glReadPixels returns it first; flip: the buffer's first row is the picture's bottom */
static void draw_rows(const DmaImg *im, int y, int h, int flip) {
    glViewport(0, 0, im->w, im->h);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, y, im->w, h);          /* only the rows asked for are drawn */
    GLfloat t0 = flip ? 1.0f : 0.0f, t1 = flip ? 0.0f : 1.0f;
    const GLfloat q[] = { -1, -1, 0, t0,  1, -1, 1, t0,  -1, 1, 0, t1,  1, 1, 1, t1 };
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, q);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, q + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_SCISSOR_TEST);
}

/* The change test of rows [y, y + h) of im against prev (a texture holding the last picture as draw_rows drew it), into
 * query q: its answer says whether any pixel differs. The pass draws into a target of its own (no feedback loop with
 * prev); 0 queued, -1 not possible (no query, a GL error) */
static int diff_rows(Dev *v, const DmaImg *im, GLuint prev, int y, int h, int flip, GLuint q) {
    if (!v->query || !q) return -1;
    if (v->dfbo && (v->dw != im->w || v->dh != im->h)) {
        glDeleteFramebuffers(1, &v->dfbo);
        glDeleteRenderbuffers(1, &v->drb);
        v->dfbo = v->drb = 0;
    }
    if (!v->dfbo) {
        glGenRenderbuffers(1, &v->drb);
        glBindRenderbuffer(GL_RENDERBUFFER, v->drb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGB565, im->w, im->h);
        glGenFramebuffers(1, &v->dfbo);
        glBindFramebuffer(GL_FRAMEBUFFER, v->dfbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, v->drb);
        v->dw = im->w;
        v->dh = im->h;
    } else glBindFramebuffer(GL_FRAMEBUFFER, v->dfbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return -1;
    glUseProgram(v->dprog);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, prev);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, im->tex);
    glUniform1i(v->d_tex, 0);
    glUniform1i(v->d_prev, 1);
    glUniform2f(v->d_size, (GLfloat)im->w, (GLfloat)im->h);
    v->begin_q(GL_ANY_SAMPLES_PASSED_EXT, q);
    draw_rows(im, y, h, flip);
    v->end_q(GL_ANY_SAMPLES_PASSED_EXT);
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}

int dmaread_rows_new(DmaImg *im, uint8_t *px, int y, int h, int flip, const void *key) {
    if (!im || !px || y < 0 || h <= 0 || y + h > im->h) return -1;
    Dev *v = im->dev;
    Cur s;
    if (enter(v, &s)) return -1;
    while (glGetError() != GL_NO_ERROR) {}
    /* the framebuffer the picture is drawn into: kept, made again for another size; it holds the last picture read
     * (fkey's), which the change test compares against */
    if (v->fbo && (v->fw != im->w || v->fh != im->h)) {
        glDeleteFramebuffers(1, &v->fbo);
        glDeleteTextures(1, &v->ftex);
        v->fbo = v->ftex = 0;
        v->fkey = NULL;
    }
    int same = 0;
    if (v->fbo && key && v->fkey == key) {
        /* its answer is waited for here: this path waits on glReadPixels anyway */
        if (!v->rq && v->query) v->gen_q(1, &v->rq);
        if (!diff_rows(v, im, v->ftex, y, h, flip, v->rq)) {
            GLuint any = 1;
            v->get_q(v->rq, GL_QUERY_RESULT_EXT, &any);
            same = glGetError() == GL_NO_ERROR && !any;
        }
    }
    int ok = 1;
    if (!same) {
        if (!v->fbo) {
            glGenTextures(1, &v->ftex);
            glBindTexture(GL_TEXTURE_2D, v->ftex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, im->w, im->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glGenFramebuffers(1, &v->fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, v->fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, v->ftex, 0);
            v->fw = im->w;
            v->fh = im->h;
        } else glBindFramebuffer(GL_FRAMEBUFFER, v->fbo);
        ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    if (ok && !same) {
        glUseProgram(v->prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, im->tex);
        glUniform1i(v->u_tex, 0);
        draw_rows(im, y, h, flip);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        uint8_t *rows = px + (size_t)y * (size_t)im->w * 4;
        int done = 0;
        if (v->bgra) {                      /* B, G, R, A straight from the GPU (A is 1.0: X 255) */
            glReadPixels(0, y, im->w, h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, rows);
            if (glGetError() == GL_NO_ERROR) done = 1;
            else v->bgra = 0;
        }
        if (!done) {
            glReadPixels(0, y, im->w, h, GL_RGBA, GL_UNSIGNED_BYTE, rows);
            ok = glGetError() == GL_NO_ERROR;
            for (size_t i = 0, n = (size_t)im->w * (size_t)h; ok && i < n; i++) {     /* R, G, B, A → B, G, R, X */
                uint8_t r = rows[i * 4];
                rows[i * 4] = rows[i * 4 + 2];
                rows[i * 4 + 2] = r;
                rows[i * 4 + 3] = 255;
            }
        }
        /* only rows [y, y + h) are new in the framebuffer: it is key's picture only if the rest was key's already */
        v->fkey = ok && key && (v->fkey == key || (y == 0 && h == im->h)) ? key : NULL;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    leave(v, &s);
    return !ok ? -1 : same ? 0 : 1;
}

int dmaread_rows(DmaImg *im, uint8_t *px, int y, int h, int flip) {
    return dmaread_rows_new(im, px, y, h, flip, NULL) < 0 ? -1 : 0;
}

/* ---- linear copies for the desktop's GPU ---- */

DmaOut *dmaread_out_new(const DmaImg *like) {
    if (!like) return NULL;
    Dev *v = like->dev;
    /* the desktop's GPU takes a linear buffer whose rows are a multiple of 256 bytes (amdgpu's pitch); Intel's are of
     * 64: the width is rounded up to 64 pixels and only the picture's own width is used */
    int aw = (like->w + 63) & ~63;
    struct gbm_bo *bo = gbm_bo_create(v->gbm, (uint32_t)aw, (uint32_t)like->h, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!bo) return NULL;
    DmaOut *o = calloc(1, sizeof *o);
    if (!o) { gbm_bo_destroy(bo); return NULL; }
    o->dev = v;
    o->bo = bo;
    o->w = like->w;
    o->h = like->h;
    o->stride = (int)gbm_bo_get_stride(bo);
    o->fd = gbm_bo_get_fd(bo);
    if (o->fd < 0 || o->stride % 256 || o->stride < like->w * 4 || gbm_bo_get_modifier(bo) != DRM_FORMAT_MOD_LINEAR) goto fail;
    o->img = import_image(v, o->fd, o->w, o->h, o->stride, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 1);
    if (o->img == EGL_NO_IMAGE_KHR) o->img = import_image(v, o->fd, o->w, o->h, o->stride, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 0);
    if (o->img == EGL_NO_IMAGE_KHR) goto fail;
    Cur s;
    if (enter(v, &s)) goto fail;
    while (glGetError() != GL_NO_ERROR) {}
    glGenTextures(1, &o->tex);
    glBindTexture(GL_TEXTURE_2D, o->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    v->image_target(GL_TEXTURE_2D, (GLeglImageOES)o->img);
    glGenFramebuffers(1, &o->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, o->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, o->tex, 0);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR;
    if (ok && v->query) v->gen_q(1, &o->q);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    leave(v, &s);
    if (!ok) goto fail;
    return o;
fail:
    dmaread_out_free(o);
    return NULL;
}

int dmaread_out_fd(const DmaOut *o, int *stride) {
    if (!o) return -1;
    if (stride) *stride = o->stride;
    return o->fd;
}

int dmaread_copy(DmaImg *src, DmaOut *dst, int y, int h, int flip, const DmaOut *prev) {
    if (!src || !dst || src->dev != dst->dev || src->w != dst->w || src->h != dst->h || y < 0 || h <= 0 || y + h > src->h) return -1;
    if (prev && (prev->dev != src->dev || prev->w != src->w || prev->h != src->h || prev == dst)) prev = NULL;
    Dev *v = src->dev;
    Cur s;
    if (enter(v, &s)) return -1;
    while (glGetError() != GL_NO_ERROR) {}
    dst->q_on = prev && !diff_rows(v, src, prev->tex, y, h, flip, dst->q);
    glBindFramebuffer(GL_FRAMEBUFFER, dst->fbo);
    glUseProgram(v->prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, src->tex);
    glUniform1i(v->u_tex, 0);
    draw_rows(src, y, h, flip);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    int ok = glGetError() == GL_NO_ERROR;
    glFlush();                          /* sent to the GPU now; the copy's fence is on dst's dmabuf (poll it) */
    leave(v, &s);
    return ok ? 0 : -1;
}

int dmaread_changed(DmaOut *o) {
    if (!o || !o->q_on) return -1;
    Dev *v = o->dev;
    Cur s;
    if (enter(v, &s)) return -1;
    GLuint avail = 0, any = 1;
    v->get_q(o->q, GL_QUERY_RESULT_AVAILABLE_EXT, &avail);
    if (avail) v->get_q(o->q, GL_QUERY_RESULT_EXT, &any);
    int r = glGetError() != GL_NO_ERROR || !avail ? -1 : any ? 1 : 0;
    leave(v, &s);
    o->q_on = 0;
    return r;
}

void dmaread_out_free(DmaOut *o) {
    if (!o) return;
    Dev *v = o->dev;
    if (o->tex || o->fbo || o->q) {
        Cur s;
        if (!enter(v, &s)) {
            if (o->fbo) glDeleteFramebuffers(1, &o->fbo);
            if (o->tex) glDeleteTextures(1, &o->tex);
            if (o->q) v->del_q(1, &o->q);
            leave(v, &s);
        }
    }
    if (o->img != EGL_NO_IMAGE_KHR && o->img) v->destroy_image(v->dpy, o->img);
    if (o->fd >= 0) close(o->fd);
    if (o->bo) gbm_bo_destroy(o->bo);
    free(o);
}

void dmaread_free(DmaImg *im) {
    if (!im) return;
    Dev *v = im->dev;
    Cur s;
    if (!enter(v, &s)) {
        glDeleteTextures(1, &im->tex);
        leave(v, &s);
    }
    v->destroy_image(v->dpy, im->img);
    free(im);
}

void dmaread_close_all(void) {
    for (int i = 0; i < DEV_MAX; i++) if (devs[i].used) dev_close(&devs[i]);
}

#endif
