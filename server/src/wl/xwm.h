/* xwm.h — LordOS's X window manager for Xwayland (xwm.c) and its glue with the compositor (wl.c).
 *
 * X11 apps are clients of Xwayland; Xwayland is one Wayland client of lord-server and gives every
 * mapped top-level X window a wl_surface. The window manager is a second X client (on the fd
 * Xwayland gets with -wm) that decides what those windows are: a LordOS window with the theme's
 * frame, a menu or tooltip drawn above everything (override-redirect), their title and app
 * (WM_CLASS), focus, stacking, close, fullscreen, the clipboard bridge and drag and drop (XDND). See docs/WAYLAND.md,
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
/* Drag and drop with X apps (XDND, see xwm.c). Actions are wl_data_device_manager's bits (1 copy, 2 move, 4 ask).
 * A drag of LordOS's Files or of a Wayland app over an X window: xwm is its XDND source. mimes: its types;
 * x, y: the pointer (logical, the X root's coordinates); want: the action asked for. */
void xwm_dnd_enter(XWin *w, const char *const *mimes, int n, int x, int y, uint32_t want);
void xwm_dnd_motion(int x, int y, uint32_t want);
void xwm_dnd_leave(void);
int  xwm_dnd_drop(void);                       /* 1 if the window took it (XdndDrop sent), 0: refused (call leave) */
void xwm_dnd_source_gone(void);                /* the Wayland source went away: nothing more is read from it */
/* An X app's drag: while a button is down on an X window and the pointer is outside the X windows, xwm's
 * proxy is mapped (out 1) so the app's XDND talks to it; the Wayland side answers with these. */
int  xwm_is_dnd_icon(const XWin *w);           /* an X app's drag icon (never the window under the pointer) */
void xwm_xdrag_out(int out);
void xwm_xdrag_status(int accept, uint32_t action);    /* what the window under the pointer takes now */
void xwm_xdrag_receive(const char *mime, int fd);      /* read that type from the X app into fd (takes it) */
void xwm_xdrag_finished(int accepted, uint32_t action);

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
/* Drag and drop (xwm.c, "drag and drop"). A Wayland drag over an X window: its data for the X window that
 * asked (written into fd, which it takes; -1 if nothing is sent), the window's status, and its end. */
int  wlx_dnd_send(const char *mime, int fd);
void wlx_dnd_status(int accept, uint32_t action);
void wlx_dnd_finished(int accepted, uint32_t action);
/* An X app's drag outside the X windows, as a Wayland drag: begin (1 if it started; its MIME types), the
 * action its source asks for, whether its files may reach LordOS (x11_xdnd_trusted), the drop (1 on a
 * Wayland window, 2 on LordOS while Files reads it, 0 refused), and its end (left, or the X app gave up). */
int  wlx_xdrag_begin(const char *const *mimes, int n);
void wlx_xdrag_action(uint32_t actions);
void wlx_xdrag_trust(int ok);
int  wlx_xdrag_drop(void);
void wlx_xdrag_end(void);
void wlx_send_bytes(int fd, const void *data, size_t n);    /* writes it without blocking, then closes fd */

#endif
