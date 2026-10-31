/* wl_input.h — gestures and touches for Wayland apps (wl_input.c): zwp_pointer_gestures_v1
 * (pinch, swipe, hold), fine touchpad scrolling (axis_source finger, axis_stop,
 * axis_value120) and wl_touch for touchscreens. Used by tp/tp_route.c. */
#ifndef LORD_WL_INPUT_H
#define LORD_WL_INPUT_H

#include <stdint.h>

struct wl_display;
struct wl_client;
struct wl_resource;

/* scroll sources */
enum { WLI_FINGER = 0, WLI_WHEEL = 1, WLI_CONTINUOUS = 2 };

int  wl_input_has_focus(void);          /* is the pointer over a Wayland app? */
/* scroll in pixels (positive = move the content down); stop: finished (axis_stop) */
void wl_input_axis(int source, double dx, double dy, int stop, int natural);
void wl_input_pinch(int phase, int fingers, double dx, double dy, double scale, double rotation_delta);
void wl_input_swipe(int phase, int fingers, double dx, double dy);
void wl_input_hold(int phase, int fingers);
/* touchscreen (x, y in screen pixels). On TP_BEGIN returns 1 if there is an app underneath
 * listening to wl_touch. */
int  wl_input_touch(int phase, int id, double x, double y);
void wl_input_touch_frame(void);
void wl_input_devices_changed(void);    /* a touchscreen was connected or removed: capabilities */

/* called by wl.c */
void wl_input_globals(struct wl_display *dpy);
void wl_input_get_touch(struct wl_client *c, struct wl_resource *seat, uint32_t id);
/* zwp_text_input_v3 (wl_textinput.c): fn(on, x, y, w, h) when the focused client's field takes or
 * loses focus, or its caret moves (surface coordinates). */
void wl_textinput_globals(struct wl_display *dpy, void (*fn)(int on, int x, int y, int w, int h));
void wl_textinput_focus(struct wl_client *client, struct wl_resource *surface);    /* NULL: nobody */
void wl_textinput_gone(void);           /* the surface with the keyboard is being destroyed */
void wl_input_seat_caps(struct wl_resource *seat);
/* regular mouse wheel (wl_pointer in wl.c): axis + axis_source wheel + value120 */
void wl_input_wheel(struct wl_resource *pointer, uint32_t time, int steps);
/* high-resolution or horizontal wheel: 1/120 of a step (+ up, + right), to the app with the pointer */
void wl_input_wheel120(int v120, int h120);

/* from wl.c, only for wl_input.c */
struct wl_display  *wl__display(void);
struct wl_resource *wl__pointer_focus(void);          /* wl_surface under the pointer, or NULL */
struct wl_list     *wl__pointers(void);
struct wl_resource *wl__surface_at(float x, float y, float *lx, float *ly);

#endif
