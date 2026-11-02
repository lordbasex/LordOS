/* glscene.c — see glscene.h. */
#include "glscene.h"
#include "bench.h"

#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static GLuint prog;
static GLint u_mvp, a_pos, a_col;

static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(s); return 0; }
    return s;
}

int bench_scene_init(BenchGlInfo *info) {
    memset(info, 0, sizeof *info);
    snprintf(info->renderer, sizeof info->renderer, "%.90s", (const char *)glGetString(GL_RENDERER));
    snprintf(info->vendor, sizeof info->vendor, "%.60s", (const char *)glGetString(GL_VENDOR));
    snprintf(info->version, sizeof info->version, "%.60s", (const char *)glGetString(GL_VERSION));
    snprintf(info->kind, sizeof info->kind, "%s", bench_gl_kind(info->renderer));
    static const char *vs = "uniform mat4 mvp; attribute vec3 pos; attribute vec3 col; varying vec3 c;"
                            "void main(){ c = col; gl_Position = mvp * vec4(pos, 1.0); }";
    static const char *fs = "precision mediump float; varying vec3 c; void main(){ gl_FragColor = vec4(c, 1.0); }";
    GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return -1;
    prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    glDeleteShader(v);
    glDeleteShader(f);
    u_mvp = glGetUniformLocation(prog, "mvp");
    a_pos = glGetAttribLocation(prog, "pos");
    a_col = glGetAttribLocation(prog, "col");
    glEnable(GL_DEPTH_TEST);
    return 0;
}

static void mul(float *o, const float *a, const float *b) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            r[j * 4 + i] = 0;
            for (int k = 0; k < 4; k++) r[j * 4 + i] += a[k * 4 + i] * b[j * 4 + k];
        }
    memcpy(o, r, sizeof r);
}

double bench_scene_draw(int w, int h, double t_ms) {
    if (!prog) return 0;
    /* a cube, two triangles per face, a colour per face */
    static const float P[8][3] = { { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 }, { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } };
    static const int F[6][4] = { { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 1, 2, 6, 5 }, { 0, 3, 7, 4 } };
    static const float C[6][3] = { { .95f, .25f, .25f }, { .25f, .85f, .35f }, { .25f, .45f, .95f }, { .95f, .8f, .2f }, { .75f, .35f, .95f }, { .2f, .85f, .9f } };
    float v[36 * 3], c[36 * 3];
    int k = 0;
    for (int f = 0; f < 6; f++) {
        static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
        for (int i = 0; i < 6; i++, k++) {
            memcpy(v + k * 3, P[F[f][tri[i]]], 12);
            memcpy(c + k * 3, C[f], 12);
        }
    }
    double a = t_ms / 1000.0;
    float ca = (float)cos(a), sa = (float)sin(a), cb = (float)cos(a * 0.7), sb = (float)sin(a * 0.7);
    float ry[16] = { ca, 0, -sa, 0, 0, 1, 0, 0, sa, 0, ca, 0, 0, 0, 0, 1 };
    float rx[16] = { 1, 0, 0, 0, 0, cb, sb, 0, 0, -sb, cb, 0, 0, 0, 0, 1 };
    float asp = h ? (float)w / h : 1, fn = 1.0f / (float)tan(0.5), zn = 1, zf = 20;
    float pr[16] = { fn / asp, 0, 0, 0, 0, fn, 0, 0, 0, 0, (zf + zn) / (zn - zf), -1, 0, 0, 2 * zf * zn / (zn - zf), 0 };
    float tr[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -5, 1 };
    float m[16], mvp[16];
    mul(m, rx, ry);
    mul(m, tr, m);
    mul(mvp, pr, m);
    double t0 = bench_now_ms();
    glViewport(0, 0, w, h);
    glClearColor(0.07f, 0.07f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(prog);
    glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp);
    glVertexAttribPointer((GLuint)a_pos, 3, GL_FLOAT, GL_FALSE, 0, v);
    glVertexAttribPointer((GLuint)a_col, 3, GL_FLOAT, GL_FALSE, 0, c);
    glEnableVertexAttribArray((GLuint)a_pos);
    glEnableVertexAttribArray((GLuint)a_col);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glFinish();                         /* the GPU's time for this frame */
    return bench_now_ms() - t0;
}

void bench_scene_free(void) {
    if (prog) glDeleteProgram(prog);
    prog = 0;
}
