/* gl3d.h — lordos-testbench's 3D view (see gl3d.c). */
#ifndef LORD_TESTBENCH_GL3D_H
#define LORD_TESTBENCH_GL3D_H

#include <stdio.h>
#include <wayland-client.h>

typedef struct {
    char renderer[96], vendor[64], version[64], egl[64];
    char kind[32];                      /* "HARDWARE GPU", "SOFTWARE (CPU)", "VIRTUAL GPU (HOST)" */
    char error[64];                     /* why it didn't start, "" otherwise */
    double gpu_ms;                      /* time to draw one frame (glFinish), smoothed */
    unsigned long frames;
} Gl3dInfo;
extern Gl3dInfo gl3d;

/* Starts the view on a subsurface at (x, y) w × h of parent; 0, or -1 with gl3d.error. */
int  gl3d_start(struct wl_display *dpy, struct wl_compositor *comp, struct wl_subcompositor *sc, struct wl_surface *parent, int x, int y, int w, int h);
void gl3d_place(int x, int y, int w, int h);
void gl3d_frame(double t_ms);           /* draws and swaps one frame */
void gl3d_stop(void);
int  gl3d_on(void);
struct wl_surface *gl3d_surface(void);

#endif
