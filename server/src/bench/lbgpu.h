/* lbgpu.h — LordBench's GPU workloads, offscreen on a render node fd (lbgpu.c). */
#ifndef LORD_BENCH_LBGPU_H
#define LORD_BENCH_LBGPU_H

#include "glscene.h"

int    lb_gpu_open(int render_fd, BenchGlInfo *info);   /* 0, or -1: no usable GLES 2 context */
double lb_gpu_scene(double secs);                       /* the 3D scene at 1920×1080: frames/s */
double lb_gpu_fill(double secs);                        /* blended full-screen quads: Mpixel/s */
double lb_gpu_upload(double secs);                      /* 1920×1080 RGBA texture uploads: MB/s */
double lb_gpu_layers(double secs);                      /* 8 blended screen-size layers: frames/s */
void   lb_gpu_close(void);

#endif
