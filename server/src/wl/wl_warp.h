/* wl_warp.h — wp_pointer_warp_v1 (pointer-warp-v1, version 1): an app moves the pointer to a point of its own
 * surface, under the rules of pwarp.h. See docs/WAYLAND.md, "Pointer warp". */
#ifndef LORD_WL_WARP_H
#define LORD_WL_WARP_H

struct wl_display;

void wl_warp_globals(struct wl_display *dpy);
/* main.c's: put the pointer at (x, y), logical px on screen, and send the app its motion; 0 if it can't now (LordOS's
 * own pointer lock, a window move). */
void wl_warp_set_host(int (*move)(float x, float y));

#endif
