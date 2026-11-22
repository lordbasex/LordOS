/* stgl.c — LordOS Studio's GPU composite (see stgl.h and st.h). OpenGL ES 3.0 offscreen: GBM on the render node fd
 * lord-server opened, a surfaceless EGL context, one framebuffer object the size of the frame. Each frame uploads the
 * camera picture and the small coefficient and blur planes, draws one full-screen triangle pair and reads the result
 * back (the small planes as half floats: ES 3 filters those, not 32-bit ones). Runs inside lord-studio's sandbox:
 * Mesa's driver loads from /usr/lib (read-only) and reaches the device only through that fd (DRM ioctls). Textures
 * keep BGRA's byte order, so the output needs no swizzle either. */
#include "stgl.h"
#include "st.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <gbm.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x40
#endif

struct StGl {
    struct gbm_device *gbm;
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint prog, fbo, out_tex, t_frame, t_coef, t_blur, t_pic, t_black;
    GLint u_mode, u_mirror, u_scale, a_pos;
    int fw, fh, cw, ch, bw, bh, ow, oh;
    float *coef;                 /* interleaved a, b */
    int coef_n;
};

static const char *VS =
    "#version 300 es\n"
    "in vec2 pos; out vec2 uv;\n"
    "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";
/* the same arithmetic as stfx.c's compose_cpu: alpha = a·luma + b, the background, the mix; t is the source position
 * (mirrored when asked) and every plane is sampled there with linear filtering and clamped edges */
static const char *FS =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 uv; out vec4 o;\n"
    "uniform sampler2D frame, coef, blur, pic;\n"
    "uniform int mode, mirror; uniform vec2 picScale;\n"
    "void main() {\n"
    "  vec2 t = uv; if (mirror == 1) t.x = 1.0 - t.x;\n"
    "  vec4 f = texture(frame, t);\n"
    "  if (mode == 0) { o = vec4(f.rgb, 1.0); return; }\n"
    "  float I = 0.114 * f.r + 0.587 * f.g + 0.299 * f.b;\n"
    "  vec2 ab = texture(coef, t).rg;\n"
    "  float al = clamp(ab.x * I + ab.y, 0.0, 1.0);\n"
    "  vec3 bg;\n"
    "  if (mode == 1) { vec4 q = texture(blur, t); bg = q.a > 0.001 ? q.rgb / (255.0 * q.a) : f.rgb; }\n"
    "  else bg = texture(pic, (t - 0.5) * picScale + 0.5).rgb;\n"
    "  o = vec4(mix(bg, f.rgb, al), 1.0);\n"
    "}\n";

static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "lord-studio: shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint tex(GLint filter) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

void stgl_free(StGl *g) {
    if (!g) return;
    if (g->dpy != EGL_NO_DISPLAY) {
        if (g->ctx != EGL_NO_CONTEXT) {
            eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx);
            GLuint t[] = { g->out_tex, g->t_frame, g->t_coef, g->t_blur, g->t_pic, g->t_black };
            glDeleteTextures(6, t);
            if (g->fbo) glDeleteFramebuffers(1, &g->fbo);
            if (g->prog) glDeleteProgram(g->prog);
            eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(g->dpy, g->ctx);
        }
        eglTerminate(g->dpy);
    }
    if (g->gbm) gbm_device_destroy(g->gbm);
    free(g->coef);
    free(g);
}

StGl *stgl_new(int render_fd) {
    StGl *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->dpy = EGL_NO_DISPLAY;
    g->ctx = EGL_NO_CONTEXT;
    g->gbm = gbm_create_device(render_fd);
    if (!g->gbm) goto bad;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g->dpy = get_display ? get_display(EGL_PLATFORM_GBM_KHR, g->gbm, NULL) : eglGetDisplay((EGLNativeDisplayType)g->gbm);
    if (g->dpy == EGL_NO_DISPLAY || !eglInitialize(g->dpy, NULL, NULL)) { g->dpy = EGL_NO_DISPLAY; goto bad; }
    const char *ext = eglQueryString(g->dpy, EGL_EXTENSIONS);
    if (!ext || !strstr(ext, "EGL_KHR_surfaceless_context") || !eglBindAPI(EGL_OPENGL_ES_API)) goto bad;
    static const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(g->dpy, cfg_attr, &cfg, 1, &n) || n < 1) goto bad;
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    g->ctx = eglCreateContext(g->dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g->ctx == EGL_NO_CONTEXT || !eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx)) goto bad;
    GLuint v = shader(GL_VERTEX_SHADER, VS), f = shader(GL_FRAGMENT_SHADER, FS);
    if (!v || !f) goto bad;
    g->prog = glCreateProgram();
    glAttachShader(g->prog, v);
    glAttachShader(g->prog, f);
    glLinkProgram(g->prog);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint linked = 0;
    glGetProgramiv(g->prog, GL_LINK_STATUS, &linked);
    if (!linked) goto bad;
    glUseProgram(g->prog);
    g->a_pos = glGetAttribLocation(g->prog, "pos");
    g->u_mode = glGetUniformLocation(g->prog, "mode");
    g->u_mirror = glGetUniformLocation(g->prog, "mirror");
    g->u_scale = glGetUniformLocation(g->prog, "picScale");
    glUniform1i(glGetUniformLocation(g->prog, "frame"), 0);
    glUniform1i(glGetUniformLocation(g->prog, "coef"), 1);
    glUniform1i(glGetUniformLocation(g->prog, "blur"), 2);
    glUniform1i(glGetUniformLocation(g->prog, "pic"), 3);
    g->t_frame = tex(GL_LINEAR);
    g->t_coef = tex(GL_LINEAR);
    g->t_blur = tex(GL_LINEAR);
    g->t_pic = tex(GL_LINEAR);
    g->t_black = tex(GL_NEAREST);
    static const uint8_t black[4] = { 0, 0, 0, 255 };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    g->out_tex = tex(GL_NEAREST);
    glGenFramebuffers(1, &g->fbo);
    if (glGetError() != GL_NO_ERROR) goto bad;
    return g;
bad:
    fprintf(stderr, "lord-studio: no OpenGL ES 3 on the render node; the CPU composites\n");
    stgl_free(g);
    return NULL;
}

int stgl_compose(StGl *g, const StGlJob *j) {
    if (!g || !j || j->w < 1 || j->h < 1 || j->gw < 2 || j->gh < 2) return -1;
    if (!eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx)) return -1;
    /* the camera picture */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->t_frame);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, j->stride / 4);
    if (g->fw != j->w || g->fh != j->h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, j->w, j->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, j->frame);
        g->fw = j->w;
        g->fh = j->h;
    } else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, j->w, j->h, GL_RGBA, GL_UNSIGNED_BYTE, j->frame);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (j->mode != ST_MODE_NONE) {
        /* the guided filter's coefficients, interleaved */
        int n = j->gw * j->gh;
        if (g->coef_n < n) {
            float *c = realloc(g->coef, (size_t)n * 2 * sizeof(float));
            if (!c) return -1;
            g->coef = c;
            g->coef_n = n;
        }
        for (int i = 0; i < n; i++) { g->coef[2 * i] = j->a[i]; g->coef[2 * i + 1] = j->b[i]; }
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g->t_coef);
        if (g->cw != j->gw || g->ch != j->gh) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, j->gw, j->gh, 0, GL_RG, GL_FLOAT, g->coef);
            g->cw = j->gw;
            g->ch = j->gh;
        } else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, j->gw, j->gh, GL_RG, GL_FLOAT, g->coef);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, g->t_blur);
        if (j->mode == ST_MODE_BLUR && j->blur) {
            if (g->bw != j->bw || g->bh != j->bh) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, j->bw, j->bh, 0, GL_RGBA, GL_FLOAT, j->blur);
                g->bw = j->bw;
                g->bh = j->bh;
            } else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, j->bw, j->bh, GL_RGBA, GL_FLOAT, j->blur);
        }
        glActiveTexture(GL_TEXTURE3);
        if (j->mode == ST_MODE_PICTURE && j->pic) {
            glBindTexture(GL_TEXTURE_2D, g->t_pic);
            if (j->pic_changed) {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, j->pstride / 4);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, j->pw, j->ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, j->pic);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            }
            float sc = fmaxf((float)j->w / j->pw, (float)j->h / j->ph);
            glUniform2f(g->u_scale, (float)j->w / (sc * j->pw), (float)j->h / (sc * j->ph));
        } else {
            glBindTexture(GL_TEXTURE_2D, g->t_black);
            glUniform2f(g->u_scale, 1, 1);
        }
    }
    /* the target */
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, g->out_tex);
    if (g->ow != j->w || g->oh != j->h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, j->w, j->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        g->ow = j->w;
        g->oh = j->h;
        glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->out_tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return -1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glViewport(0, 0, j->w, j->h);
    glUseProgram(g->prog);
    glUniform1i(g->u_mode, j->mode);
    glUniform1i(g->u_mirror, j->mirror);
    static const float P[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer((GLuint)g->a_pos, 2, GL_FLOAT, GL_FALSE, 0, P);
    glEnableVertexAttribArray((GLuint)g->a_pos);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, j->ostride / 4);
    glReadPixels(0, 0, j->w, j->h, GL_RGBA, GL_UNSIGNED_BYTE, j->out);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}
