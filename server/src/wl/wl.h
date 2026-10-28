/* wl.h — lord-server's Wayland compositor: GTK and Qt apps as LordOS windows (see wl.c). */
#ifndef LORD_WL_H
#define LORD_WL_H

#include "../canvas.h"
#include "../keyboard.h"
#include <stdint.h>
#include <sys/types.h>

struct Node;

int  wl_start(void);                    /* after dropping privileges; 0 ok */
void wl_stop(void);
int  wl_fd(void);                       /* for poll; -1 if it did not start */
void wl_dispatch(void);                 /* there is something on wl_fd */
void wl_flush(void);                    /* sends what is pending to the clients */
const char *wl_socket(void);            /* "wayland-0" */
void wl_frame_done(unsigned time_ms);   /* after presenting a frame */
/* Nothing is being drawn (screen off, the session away from the monitor, nobody watching): apps
 * still waiting for a frame callback get it and their presentation feedback is discarded, so
 * none stalls. 1 if some app was waiting. */
int  wl_frame_unseen(unsigned time_ms);

/* Input (main.c). hit = node under the pointer; down[i]/button[i] = buttons of this event
 * (0 left, 1 middle, 2 right). Returns 1 if the pointer is over a Wayland app and 2 if
 * it is over one of its popup menus (drawn on top of everything: the click is not the theme's). */
int  wl_pointer(float x, float y, struct Node *hit, int moved, int nbuttons, const int *down, const int *button, int wheel);
/* While lord-server moves or resizes a window (ui/grab.c): no app receives the pointer
 * (they are sent leave) and only the button count is tracked. */
void wl_pointer_grab(int nbuttons, const int *down);
int  wl_key(int code, int value, const KeyInfo *k);   /* 1 if the key went to the focused app */
void wl_keymap_changed(void);
int  wl_send_paste(void);               /* synthetic Ctrl+V to the Linux app with the keyboard */
int  wl_has_key_focus(void);            /* does a Linux app have the keyboard? */

/* From JS (lord.wayland) */
void wl_set_focus(int id);              /* 0: no Wayland app has the keyboard */
void wl_close(int id);                  /* asks the app to close the window */
const char *wl_kill(int id);            /* Force Quit: SIGKILL to its client (same user only); NULL or an error code */
void wl_set_state(int id, int maximized, int fullscreen);
void wl_set_minimized(int id, int minimized);   /* X11 windows: iconic and at the bottom of X's stack */
int  wl_count(void);
int  wl_idle_inhibited(void);             /* idle-inhibit: a shown surface asks the screen to stay on */
void wl_output_resized(void);               /* headless: one display of ui.width × ui.height */
void wl_remap_all(void);                    /* a new JS context (theme switched): announce the open windows again */
/* The displays (docs/DISPLAYS.md): one wl_output (+ xdg-output) each. x, y, w, h: logical rect in the
 * global space; pw × ph: physical mode; scale: percent (wl_output.scale is its ceiling, the exact
 * one goes by fractional-scale-v1). Surfaces get enter/leave and their preferred scale. */
typedef struct {
    int x, y, w, h, pw, ph, refresh, scale, mm_w, mm_h;
    char name[32], desc[96];
} WlOutput;
void wl_set_outputs(const WlOutput *o, int n, int main);
void wl_set_pointer_scale(int pct);         /* the display under the pointer: cursor-shape images */               /* ui.width/ui.height changed */

/* Drawing (paint.c): surface id in the node's box, and popup menus on top of everything.
 * On the GPU path (src/gpu) the UI only leaves a hole and the surfaces are drawn when presenting. */
void wl_paint_begin(void);
void wl_paint_keep(void);                /* saves the windows noted so far (paint.c, «what is below») */
void wl_paint_kept(void);                /* and notes them again in another frame */
void wl_paint_save(void);                /* keeps the frame's list of windows (LORD_PAINT_VERIFY) */
void wl_paint_restore(void);             /* and puts it back */
void wl_paint_holes(int on);             /* 0: wl_paint only notes the window, without opening the hole */
void wl_paint(Canvas *c, int id, int node, float x, float y, float w, float h, float op);
void wl_paint_popups(Canvas *c);

/* Presentation (main.c). */
void wl_gpu_draw_layer(int layer);          /* 0 = windows (below the UI), 1 = menus and fullscreen */
void wl_paint_fullscreen(Canvas *out);      /* CPU path: the fullscreen window over the frame */
int  wl_fullscreen_active(void);
int  wl_try_scanout(int cursor_free);       /* GPU path: 1 if fullscreen went straight to the card */
int  wl_wants_frame(void);                  /* an app changed its content */
void wl_presented(uint64_t ns, unsigned seq, unsigned refresh_ns);   /* a flip finished (wp_presentation) */
void wl_set_drm_device(dev_t dev);          /* DRM device for linux-dmabuf (CPU path) */

/* Pointer: relative motion (games), locking and confinement (pointer-constraints). */
void wl_pointer_relative(double dx, double dy);
int  wl_pointer_locked(void);
void wl_pointer_confine(int *x, int *y);
/* Cursor requested by the app under the pointer: 0 = LordOS's, 1 = image (ARGB px), 2 = hidden.
 * gen changes when it must be redrawn. */
int  wl_cursor(const uint32_t **px, int *w, int *h, int *hx, int *hy, unsigned *gen);

/* Clipboard: last image copied by a Linux app (or chosen from the history). */
unsigned    wl_clip_image_gen(void);
const char *wl_clip_image_path(void);       /* "" if the last copied thing is not an image */
int         wl_clip_is_image(void);
const unsigned char *wl_clip_png(size_t *len);   /* the copied PNG, or NULL */
const char *wl_clip_image_origin(void);
int         wl_clip_image_sensitive(void);
int         wl_clip_use_image(const char *path);   /* only $XDG_RUNTIME_DIR/lordos/clip-N.png */

/* Events for JS: map, unmap, title, appid, geometry, move, resize, maximize, unmaximize,
 * minimize, fullscreen, unfullscreen. data is JSON. */
typedef void (*WlEventFn)(int id, const char *type, const char *json);
void wl_events(WlEventFn fn);

#endif
