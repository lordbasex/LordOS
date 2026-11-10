/* xwayland.h — X11 apps through Xwayland, started only when one connects (xwayland.c).
 *
 * The fourth exception to "lord-server doesn't run programs" (AGENTS.md): it only ever starts
 * /usr/bin/Xwayland (root-owned, not writable by others), with an argument list fixed in C, when
 * the first X11 client connects to this session's X socket; never from JS and with no argument
 * from anyone. See docs/WAYLAND.md, "X11 apps (Xwayland)". */
#ifndef LORD_XWAYLAND_H
#define LORD_XWAYLAND_H

/* After wl_start(), in a user session (not the login screen, not app mode): if Xwayland is
 * installed, reserves a display (/tmp/.X11-unix/X<n>, 0600, and its lock file), sets DISPLAY for
 * the apps LordOS starts, and waits for a connection. 0 if there is an X socket. */
int  xwl_init(void);
void xwl_stop(void);                    /* stops Xwayland and removes the socket and the lock */
const char *xwl_display(void);          /* ":1", or "" */
const char *xwl_socket_path(void);      /* "/tmp/.X11-unix/X1", or "" (apps.c: restricted apps' view) */
int  xwl_running(void);                 /* is Xwayland running now? */
/* Settings → Displays: X11 apps scale themselves (1, the default: X pixels are device pixels on the
 * densest display and Xft.dpi says how much larger to draw) or LordOS enlarges them (0). It applies the
 * next time Xwayland starts (no X11 app open); wlx_scale() (xwm.h) is what the running one has. */
void xwl_set_self_scaling(int on);
int  xwl_self_scaling(void);

#endif
