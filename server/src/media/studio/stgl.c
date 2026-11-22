/* stgl.c — LordOS Studio's GPU composite (see stgl.h and st.h). OpenGL ES 3.0 offscreen: GBM on the render node fd
 * lord-server opened, a surfaceless EGL context, one framebuffer object the size of the frame. Each frame uploads the
 * camera picture and the small coefficient and blur planes, draws one full-screen triangle pair and reads the result
 * back (the small planes as half floats: ES 3 filters those, not 32-bit ones). Runs inside lord-studio's sandbox:
 * Mesa's driver loads from /usr/lib (read-only) and reaches the device only through that fd (DRM ioctls). Textures
 * keep BGRA's byte order (the shader works in RGB and writes BGRA back), so nothing is swizzled on the CPU. Phase 3
 * adds the green screen (the dilated mask as one more small plane), the colour match (a 3×3 matrix and an offset),
 * Studio Light (the face as four numbers) and the filter (its 3D LUT as a 33³ texture). */
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
    GLuint prog, fbo, out_tex, t_frame, t_coef, t_blur, t_pic, t_black, t_dil, t_lut;
    GLint u_mode, u_mirror, u_scale, a_pos, u_matte, u_key, u_keycbcr, u_keyt, u_spill, u_match, u_m, u_mo, u_light, u_face,
          u_hasface, u_size, u_bggain, u_uselut, u_lutamt;
    int fw, fh, cw, ch, bw, bh, ow, oh, dw, dh;
    float *coef;                 /* interleaved a, b */
    int coef_n;
};

static const char *VS =
    "#version 300 es\n"
    "in vec2 pos; out vec2 uv;\n"
    "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";
/* the same arithmetic as stfx.c's compose_cpu and compose_cpu_fx: alpha = a·luma + b (the green screen's inside the
 * dilated person), the person (despilled, colour-matched, lifted), the background (dimmed), the mix, the filter; t is
 * the source position (mirrored when asked) and every plane is sampled there with linear filtering and clamped edges.
 * Colours are RGB 0..1 here (textures hold BGRA bytes: .bgr) */
static const char *FS =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp sampler3D;\n"
    "in vec2 uv; out vec4 o;\n"
    "uniform sampler2D frame, coef, blur, pic, dil;\n"
    "uniform sampler3D lut;\n"
    "uniform int mode, mirror, matte, key, match, light, hasFace, useLut;\n"
    "uniform vec2 picScale, keyCbCr, keyT, size;\n"
    "uniform float spill, bgGain, lutAmt;\n"
    "uniform mat3 M; uniform vec3 Mo; uniform vec4 face;\n"
    "void main() {\n"
    "  vec2 t = uv; if (mirror == 1) t.x = 1.0 - t.x;\n"
    "  vec4 f = texture(frame, t);\n"
    "  vec3 F = f.bgr, R = F;\n"
    "  if (matte == 1) {\n"
    "    float I = dot(F, vec3(0.299, 0.587, 0.114));\n"
    "    vec2 ab = texture(coef, t).rg;\n"
    "    float al = clamp(ab.x * I + ab.y, 0.0, 1.0);\n"
    "    if (key > 0) {\n"
    "      vec3 c = F * 255.0;\n"
    "      float cb = 128.0 - 0.168736 * c.r - 0.331264 * c.g + 0.5 * c.b;\n"
    "      float cr = 128.0 + 0.5 * c.r - 0.418688 * c.g - 0.081312 * c.b;\n"
    "      float ca = smoothstep(keyT.x, keyT.y, length(vec2(cb, cr) - keyCbCr) / 255.0);\n"
    "      float u = 1.0 - abs(2.0 * ca - 1.0);\n"
    "      float dd = clamp((texture(dil, t).r - 0.05) / 0.35, 0.0, 1.0);\n"
    "      float k = dd * (ca + (al - ca) * u);\n"
    "      al = max(k, clamp((al - 0.9) * 10.0, 0.0, 1.0));\n"
    "      if (key == 1) { float l = (F.r + F.b) * 0.5; if (F.g > l) F.g -= (F.g - l) * spill; }\n"
    "      else { float l = (F.r + F.g) * 0.5; if (F.b > l) F.b -= (F.b - l) * spill; }\n"
    "    }\n"
    "    if (match == 1) F = clamp(M * F + Mo, 0.0, 1.0);\n"
    "    if (light > 0) {\n"
    "      float k = float(light) / 100.0, w = 0.0;\n"
    "      if (hasFace == 1) {\n"
    "        vec2 p = t * size;\n"
    "        vec2 d = vec2((p.x - face.x) / (0.75 * face.z), (p.y - (face.y - 0.08 * face.w)) / (0.95 * face.w));\n"
    "        w = 1.0 - smoothstep(1.0, 1.7, length(d));\n"
    "      }\n"
    "      float g = exp2(k * (0.15 + 0.45 * w));\n"
    "      F = F * g / (1.0 + (g - 1.0) * F);\n"
    "    }\n"
    "    vec3 B;\n"
    "    if (mode == 1) { vec4 q = texture(blur, t); B = q.a > 0.001 ? q.bgr / (255.0 * q.a) : f.bgr; }\n"
    "    else if (mode == 0) B = f.bgr;\n"
    "    else B = texture(pic, (t - 0.5) * picScale + 0.5).bgr;\n"
    "    R = mix(B * bgGain, F, al);\n"
    "  }\n"
    "  if (useLut == 1) R = mix(R, texture(lut, clamp(R, 0.0, 1.0) * (32.0 / 33.0) + 0.5 / 33.0).rgb, lutAmt);\n"
    "  o = vec4(R.bgr, 1.0);\n"
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

static GLuint tex(GLenum target, GLint filter) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(target, t);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (target == GL_TEXTURE_3D) glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    return t;
}

void stgl_free(StGl *g) {
    if (!g) return;
    if (g->dpy != EGL_NO_DISPLAY) {
        if (g->ctx != EGL_NO_CONTEXT) {
            eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx);
            GLuint t[] = { g->out_tex, g->t_frame, g->t_coef, g->t_blur, g->t_pic, g->t_black, g->t_dil, g->t_lut };
            glDeleteTextures(8, t);
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
    g->u_matte = glGetUniformLocation(g->prog, "matte");
    g->u_key = glGetUniformLocation(g->prog, "key");
    g->u_keycbcr = glGetUniformLocation(g->prog, "keyCbCr");
    g->u_keyt = glGetUniformLocation(g->prog, "keyT");
    g->u_spill = glGetUniformLocation(g->prog, "spill");
    g->u_match = glGetUniformLocation(g->prog, "match");
    g->u_m = glGetUniformLocation(g->prog, "M");
    g->u_mo = glGetUniformLocation(g->prog, "Mo");
    g->u_light = glGetUniformLocation(g->prog, "light");
    g->u_face = glGetUniformLocation(g->prog, "face");
    g->u_hasface = glGetUniformLocation(g->prog, "hasFace");
    g->u_size = glGetUniformLocation(g->prog, "size");
    g->u_bggain = glGetUniformLocation(g->prog, "bgGain");
    g->u_uselut = glGetUniformLocation(g->prog, "useLut");
    g->u_lutamt = glGetUniformLocation(g->prog, "lutAmt");
    glUniform1i(glGetUniformLocation(g->prog, "frame"), 0);
    glUniform1i(glGetUniformLocation(g->prog, "coef"), 1);
    glUniform1i(glGetUniformLocation(g->prog, "blur"), 2);
    glUniform1i(glGetUniformLocation(g->prog, "pic"), 3);
    glUniform1i(glGetUniformLocation(g->prog, "dil"), 5);
    glUniform1i(glGetUniformLocation(g->prog, "lut"), 6);
    g->t_frame = tex(GL_TEXTURE_2D, GL_LINEAR);
    g->t_coef = tex(GL_TEXTURE_2D, GL_LINEAR);
    g->t_blur = tex(GL_TEXTURE_2D, GL_LINEAR);
    g->t_pic = tex(GL_TEXTURE_2D, GL_LINEAR);
    g->t_dil = tex(GL_TEXTURE_2D, GL_LINEAR);
    g->t_black = tex(GL_TEXTURE_2D, GL_NEAREST);
    static const uint8_t black[4] = { 0, 0, 0, 255 };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    g->t_lut = tex(GL_TEXTURE_3D, GL_LINEAR);
    g->out_tex = tex(GL_TEXTURE_2D, GL_NEAREST);
    glGenFramebuffers(1, &g->fbo);
    if (glGetError() != GL_NO_ERROR) goto bad;
    return g;
bad:
    fprintf(stderr, "lord-studio: no OpenGL ES 3 on the render node; the CPU composites\n");
    stgl_free(g);
    return NULL;
}

/* a small float plane (or two interleaved) as half floats, reallocated when its size changes */
static void plane(GLuint t, GLenum unit, GLenum ifmt, GLenum fmt, int w, int h, int *cw, int *ch, const float *data) {
    glActiveTexture(unit);
    glBindTexture(GL_TEXTURE_2D, t);
    if (*cw != w || *ch != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)ifmt, w, h, 0, fmt, GL_FLOAT, data);
        *cw = w;
        *ch = h;
    } else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, GL_FLOAT, data);
}

int stgl_compose(StGl *g, const StGlJob *j) {
    if (!g || !j || j->w < 1 || j->h < 1 || j->gw < 2 || j->gh < 2) return -1;
    if (!eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx)) return -1;
    glUseProgram(g->prog);
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
    const int matte = j->mode != ST_MODE_NONE || j->matte;
    if (matte) {
        /* the guided filter's coefficients, interleaved */
        int n = j->gw * j->gh;
        if (g->coef_n < n) {
            float *c = realloc(g->coef, (size_t)n * 2 * sizeof(float));
            if (!c) return -1;
            g->coef = c;
            g->coef_n = n;
        }
        for (int i = 0; i < n; i++) { g->coef[2 * i] = j->a[i]; g->coef[2 * i + 1] = j->b[i]; }
        plane(g->t_coef, GL_TEXTURE1, GL_RG16F, GL_RG, j->gw, j->gh, &g->cw, &g->ch, g->coef);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, g->t_blur);
        if (j->mode == ST_MODE_BLUR && j->blur) plane(g->t_blur, GL_TEXTURE2, GL_RGBA16F, GL_RGBA, j->bw, j->bh, &g->bw, &g->bh, j->blur);
        glActiveTexture(GL_TEXTURE3);
        if ((j->mode == ST_MODE_PICTURE || j->mode == ST_MODE_VIDEO) && j->pic) {
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
        if (j->key && j->dil) plane(g->t_dil, GL_TEXTURE5, GL_R16F, GL_RED, j->gw, j->gh, &g->dw, &g->dh, j->dil);
        else { glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, g->t_dil); }
    }
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_3D, g->t_lut);
    if (j->lut && j->lut_changed) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB8, ST_LUT_N, ST_LUT_N, ST_LUT_N, 0, GL_RGB, GL_UNSIGNED_BYTE, j->lut);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
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
    glUniform1i(g->u_mode, j->mode);
    glUniform1i(g->u_mirror, j->mirror);
    glUniform1i(g->u_matte, matte);
    glUniform1i(g->u_key, matte ? j->key : 0);
    glUniform2f(g->u_keycbcr, j->keycbcr[0], j->keycbcr[1]);
    {
        /* the same thresholds as st_chroma_alpha */
        float t0 = 0.04f + (float)j->similarity * 0.003f, t1 = t0 + 0.01f + (float)j->smoothness * 0.0025f;
        glUniform2f(g->u_keyt, t0, t1);
    }
    glUniform1f(g->u_spill, j->spill);
    glUniform1i(g->u_match, matte && j->match);
    glUniformMatrix3fv(g->u_m, 1, GL_TRUE, j->m);
    glUniform3f(g->u_mo, j->o[0] / 255, j->o[1] / 255, j->o[2] / 255);
    glUniform1i(g->u_light, matte ? j->light : 0);
    glUniform1i(g->u_hasface, j->has_face);
    glUniform4f(g->u_face, j->face[0], j->face[1], j->face[2], j->face[3]);
    glUniform2f(g->u_size, (float)j->w, (float)j->h);
    glUniform1f(g->u_bggain, j->light > 0 ? st_light_background(j->light) : 1.0f);
    glUniform1i(g->u_uselut, j->lut && j->lut_amt > 0);
    glUniform1f(g->u_lutamt, j->lut_amt);
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
