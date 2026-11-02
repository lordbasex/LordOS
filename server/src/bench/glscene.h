/* glscene.h — a fixed 3D scene (a rotating cube, a colour per face) drawn with OpenGL ES 2 in whatever
 * context is current: lordos-testbench's window (wayland-egl) or LordBench's offscreen context. Needs
 * GLESv2 only; the caller owns EGL. */
#ifndef LORD_BENCH_GLSCENE_H
#define LORD_BENCH_GLSCENE_H

typedef struct {
    char renderer[96], vendor[64], version[64];
    char kind[32];                      /* bench_gl_kind(renderer) */
} BenchGlInfo;

/* Compiles the scene's program in the current context and fills info; 0, or -1 if the shaders fail. */
int    bench_scene_init(BenchGlInfo *info);
/* Draws the scene at time t_ms into a w × h viewport and waits for the GPU (glFinish); returns that time, ms. */
double bench_scene_draw(int w, int h, double t_ms);
void   bench_scene_free(void);

#endif
