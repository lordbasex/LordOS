/* xwm.h — LordOS's X window manager for Xwayland (xwm.c) and its glue with the compositor (wl.c).
 *
 * X11 apps are clients of Xwayland; Xwayland is one Wayland client of lord-server and gives every
 * mapped top-level X window a wl_surface. The window manager is a second X client (on the fd
 * Xwayland gets with -wm) that decides what those windows are: a LordOS window with the theme's
 * frame, a menu or tooltip drawn above everything (override-redirect), their title and app
 * (WM_CLASS), focus, stacking, close, fullscreen, and the clipboard bridge. See docs/WAYLAND.md,
 * "X11 apps (Xwayland)", and docs/PRIVACY.md, "X11 apps". */
#ifndef LORD_XWM_H
#define LORD_XWM_H

#include <stddef.h>
#include <stdint.h>

struct wl_client;
struct wl_event_loop;
typedef struct XWin XWin;

/* ---- xwm.c, called by xwayland.c ---- */
int  xwm_start(int fd, struct wl_event_loop *loop);    /* the -wm connection; 0 ok (takes the fd) */
void xwm_stop(void);
int  xwm_running(void);

/* ---- xwm.c, called by wl.c ---- */
void xwm_surface_created(uint32_t res_id);     /* Xwayland created a wl_surface (WL_SURFACE_ID may wait for it) */
void xwm_surface_gone(XWin *w);                /* its wl_surface was destroyed */
int  xwm_is_dialog(const XWin *w);             /* transient, or of type dialog */
int  xwm_is_fixed(const XWin *w);              /* its minimum and maximum size are the same */
void xwm_place(XWin *w, int x, int y);         /* where its content is on screen (logical, from wl_paint) */
void xwm_resize(XWin *w, int width, int height);   /* the theme's frame gave it this size */
void xwm_set_state(XWin *w, int maximized, int fullscreen, int minimized);
void xwm_focus(XWin *w);                       /* NULL: a LordOS or Wayland window has the keyboard */
void xwm_close(XWin *w);
void xwm_tick(void);                           /* once per loop (wl_flush): clipboard changes, output */

/* ---- wl.c, called by xwm.c ---- */
typedef struct {
    const char *title, *app_id, *app_alt;      /* WM_CLASS class and instance */
    int csd;                                   /* draws its own frame (Motif hints): no theme frame */
    int popup;                                 /* menu, tooltip, override-redirect: drawn above everything */
    int x, y, width, height;                   /* logical pixels (xwm.c converts X's, xwscale.h) */
} XWinInfo;
struct wl_client *wlx_client(void);            /* Xwayland's connection (NULL if not running) */
void wlx_set_client(struct wl_client *c);
/* HiDPI (xwscale.h): before Xwayland connects, the scale its X pixels get (the largest display's when
 * self is set, the apps scale themselves; otherwise 100) for this run; wlx_scale() gives it. */
int  wlx_begin_scale(int self);
int  wlx_scale(void);
/* Gives the X window's wl_surface its role. Returns the surface id, 0 if that surface doesn't exist
 * (yet) or isn't Xwayland's. */
int  wlx_attach(uint32_t res_id, XWin *w, const XWinInfo *info);
void wlx_detach(int sid);                      /* the X window was unmapped or destroyed */
void wlx_set_title(int sid, const char *title);
void wlx_popup_moved(int sid, int x, int y);
enum { XREQ_MOVE = 1, XREQ_RESIZE, XREQ_MAXIMIZE, XREQ_UNMAXIMIZE, XREQ_MINIMIZE, XREQ_FULLSCREEN, XREQ_UNFULLSCREEN };
void wlx_request(int sid, int what, int arg);
void wlx_fullscreen_rect(int *x, int *y, int *w, int *h);  /* the display a fullscreen X window covers */
/* Clipboard: what an X app copied becomes LordOS's (and every Wayland app's) selection. */
void wlx_clip_text(const char *text, size_t n, const char *origin);
void wlx_clip_png(const unsigned char *png, size_t n, const char *origin);
void wlx_primary_text(const char *text, size_t n);
/* The PRIMARY selection of Wayland apps (middle-click paste), for X: text and a generation. */
const char *wlx_primary(size_t *n, unsigned *gen);

#endif
