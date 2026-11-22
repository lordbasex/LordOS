/* wl_tablet.h — graphics tablets for Wayland apps: zwp_tablet_manager_v2 (tablet-v2, version 2), wl_tablet.c. See
 * docs/WAYLAND.md, "Graphics tablets".
 *
 * The devices come from tablet.c (evdev). A pen's tool goes to the surface under it when that surface's app bound
 * tablet-v2 (Xwayland does, for X apps): proximity_in/out, down/up, motion, pressure, distance, tilt, rotation,
 * slider, buttons and frame. While the tip is down the tool stays on the surface it pressed. Anywhere else — LordOS's
 * own UI, an app without tablet-v2, a window under a modal dialog — main.c moves the pointer with it and clicks as a
 * mouse. A pad's buttons, rings, strips and dials go to the window with the keyboard. */
#ifndef LORD_WL_TABLET_H
#define LORD_WL_TABLET_H

#include <stdint.h>
#include "../tablet.h"

struct wl_display;
struct wl_client;
struct wl_resource;

void wl_tablet_init(void);              /* before the input devices open: tablet.c tells us of them */
void wl_tablet_globals(struct wl_display *dpy);
/* A pen frame at (x, y), logical px on screen. 1 if an app took it by tablet-v2 (the pointer only moves); 0: the
 * pointer acts for it. */
int  wl_tablet_tool(TabDev *d, const TabFrame *f, float x, float y);
/* Frame f doesn't go to an app (main.c takes it for the pointer: a press as a mouse, a window move, a lock): the tool
 * leaves the surface it was over (buttons released, up, proximity_out) and keeps f's state for the next one. */
void wl_tablet_tool_away(TabDev *d, const TabFrame *f);
void wl_tablet_pad(TabDev *d, const TabPadFrame *p);
/* The window with the keyboard (wl.c's wl_set_focus): the pads' focus. NULL: none. */
void wl_tablet_key_focus(struct wl_resource *surface);
/* For cursor-shape-v1's tablet tool device: 1 if that zwp_tablet_tool_v2 is client c's and its tool is over one of
 * c's surfaces. tool is only compared, never dereferenced (the resource may be gone). */
int  wl_tablet_tool_over(struct wl_resource *tool, struct wl_client *c);

/* from wl.c, only for wl_tablet.c and wl_warp.c */
int  wl__surface_box(struct wl_resource *surface, float *x, float *y, float *w, float *h);
int  wl__surface_held(struct wl_resource *surface);
int  wl__same_window(struct wl_resource *a, struct wl_resource *b);
struct wl_resource *wl__key_focus(void);
uint32_t wl__pointer_enter_serial(void);
int  wl__dragging(void);
void wl__tool_press(struct wl_resource *surface, float x, float y, int down, uint32_t serial);
void wl__tool_cursor(struct wl_resource *surface, int32_t x, int32_t y);
void wl__pointer_leave(void);

#endif
