/* gl3d.c — lordos-testbench's 3D view: the shared scene (src/bench/glscene.c, also LordBench's) drawn
 * with EGL and OpenGL ES 2 on a subsurface, through Mesa's Wayland platform, the way a game or a GPU app draws. It shows which renderer did the
 * work (a GPU's driver, or a software one such as llvmpipe), how long the GPU takes per frame, and it
 * sends its buffers through the compositor's dmabuf path (Mesa uses linux-dmabuf with a GPU, wl_shm with
 * a software renderer). Built only when EGL, GLESv2 and wayland-egl are there (TB_GL). */
#include "gl3d.h"

#ifdef TB_GL
#include "../bench/glscene.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>
#include <string.h>
#include <wayland-egl.h>

static EGLDisplay edpy = EGL_NO_DISPLAY;
static EGLContext ectx = EGL_NO_CONTEXT;
static EGLSurface esurf = EGL_NO_SURFACE;
static struct wl_egl_window *ewin;
static struct wl_surface *gsurf;
static struct wl_subsurface *gsub;
static int gw, gh, on;
Gl3dInfo gl3d;

int gl3d_start(struct wl_display *dpy, struct wl_compositor *comp, struct wl_subcompositor *sc, struct wl_surface *parent, int x, int y, int w, int h) {
    if (on) { gl3d_place(x, y, w, h); return 0; }
    memset(&gl3d, 0, sizeof gl3d);
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    edpy = gpd ? gpd(EGL_PLATFORM_WAYLAND_KHR, dpy, NULL) : eglGetDisplay((EGLNativeDisplayType)dpy);
    EGLint maj, min;
    if (edpy == EGL_NO_DISPLAY || !eglInitialize(edpy, &maj, &min)) { snprintf(gl3d.error, sizeof gl3d.error, "EGL DID NOT START ON WAYLAND"); return -1; }
    snprintf(gl3d.egl, sizeof gl3d.egl, "EGL %d.%d %.40s", maj, min, eglQueryString(edpy, EGL_VENDOR));
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 16,
                                 EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(edpy, ca, &cfg, 1, &n) || n < 1) { snprintf(gl3d.error, sizeof gl3d.error, "NO GLES2 CONFIG"); return -1; }
    static const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    ectx = eglCreateContext(edpy, cfg, EGL_NO_CONTEXT, xa);
    if (ectx == EGL_NO_CONTEXT) { snprintf(gl3d.error, sizeof gl3d.error, "NO GLES2 CONTEXT"); return -1; }
    gsurf = wl_compositor_create_surface(comp);
    gsub = wl_subcompositor_get_subsurface(sc, gsurf, parent);
    wl_subsurface_set_desync(gsub);
    struct wl_region *empty = wl_compositor_create_region(comp);     /* input goes to the bench below */
    wl_surface_set_input_region(gsurf, empty);
    wl_region_destroy(empty);
    gw = w; gh = h;
    wl_subsurface_set_position(gsub, x, y);
    ewin = wl_egl_window_create(gsurf, w, h);
    esurf = eglCreateWindowSurface(edpy, cfg, (EGLNativeWindowType)ewin, NULL);
    if (esurf == EGL_NO_SURFACE || !eglMakeCurrent(edpy, esurf, esurf, ectx)) { snprintf(gl3d.error, sizeof gl3d.error, "NO EGL WINDOW SURFACE"); return -1; }
    eglSwapInterval(edpy, 0);           /* the bench's own frame callback paces the frames */
    BenchGlInfo gi;
    int bad = bench_scene_init(&gi);
    snprintf(gl3d.renderer, sizeof gl3d.renderer, "%s", gi.renderer);
    snprintf(gl3d.vendor, sizeof gl3d.vendor, "%s", gi.vendor);
    snprintf(gl3d.version, sizeof gl3d.version, "%s", gi.version);
    snprintf(gl3d.kind, sizeof gl3d.kind, "%s", gi.kind);
    if (bad) { snprintf(gl3d.error, sizeof gl3d.error, "SHADERS DID NOT COMPILE"); return -1; }
    on = 1;
    return 0;
}

void gl3d_place(int x, int y, int w, int h) {
    if (!on) return;
    wl_subsurface_set_position(gsub, x, y);
    if (w != gw || h != gh) { wl_egl_window_resize(ewin, w, h, 0, 0); gw = w; gh = h; }
}

struct wl_surface *gl3d_surface(void) { return on ? gsurf : NULL; }

void gl3d_frame(double t_ms) {
    if (!on) return;
    double dt = bench_scene_draw(gw, gh, t_ms);
    gl3d.gpu_ms = gl3d.frames ? gl3d.gpu_ms * 0.9 + dt * 0.1 : dt;
    gl3d.frames++;
    eglSwapBuffers(edpy, esurf);
}

void gl3d_stop(void) {
    if (!on) return;
    bench_scene_free();
    eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(edpy, esurf);
    wl_egl_window_destroy(ewin);
    wl_subsurface_destroy(gsub);
    wl_surface_destroy(gsurf);
    eglDestroyContext(edpy, ectx);
    eglTerminate(edpy);
    on = 0;
}

int gl3d_on(void) { return on; }

#else   /* built without EGL: the view says so */
Gl3dInfo gl3d;
int gl3d_start(struct wl_display *d, struct wl_compositor *c, struct wl_subcompositor *s, struct wl_surface *p, int x, int y, int w, int h) {
    (void)d; (void)c; (void)s; (void)p; (void)x; (void)y; (void)w; (void)h;
    snprintf(gl3d.error, sizeof gl3d.error, "BUILT WITHOUT EGL");
    return -1;
}
void gl3d_place(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void gl3d_frame(double t) { (void)t; }
void gl3d_stop(void) {}
int gl3d_on(void) { return 0; }
struct wl_surface *gl3d_surface(void) { return NULL; }
#endif
