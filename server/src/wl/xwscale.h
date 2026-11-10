/* xwscale.h — the size of an X pixel, for X11 apps on HiDPI displays (xwscale.c).
 *
 * As in KWin's "apps scale themselves": while Xwayland runs, X's coordinates are LordOS's logical
 * pixels times one scale, the largest of the displays when Xwayland started. An X11 window then
 * draws one X pixel per device pixel on that display (sharp), and the apps are told to draw larger
 * through Xft.dpi in the root window's RESOURCE_MANAGER (96 × scale). With the setting "scaled by
 * LordOS" (or every display at 100 %) the scale is 100: an X pixel is a logical pixel and LordOS
 * enlarges the window (the right size, blurry). Everything here is pure; wl.c converts what it sends
 * Xwayland and receives from it, xwm.c what goes through the X protocol. docs/WAYLAND.md, "HiDPI". */
#ifndef LORD_XWSCALE_H
#define LORD_XWSCALE_H

#include <stddef.h>

#define XWS_MIN 100
#define XWS_MAX 300                     /* the largest display scale Settings offers */

/* The scale (percent) X gets: the largest of the n display scales (clamped to XWS_MIN..XWS_MAX) when
 * self is set (the apps scale themselves), otherwise 100. No display: 100. */
int xws_pick(const int *pct, int n, int self);

/* A coordinate or size: logical → X pixels and back, rounded to the nearest (half away from zero,
 * so a negative position mirrors a positive one). to_x then from_x gives the same value back for
 * every scale of 100 or more. */
int xws_to_x(int logical, int pct);
int xws_from_x(int x, int pct);
/* the same for a point inside a surface (the pointer, a touch), unrounded */
double xws_to_xf(double logical, int pct);
double xws_from_xf(double x, int pct);

/* Xft.dpi for the scale: 96 × pct / 100, rounded. */
int xws_dpi(int pct);

/* The RESOURCE_MANAGER text for the scale ("Xft.dpi:\t192\n") in out (NUL-terminated). Returns its
 * length without the NUL; 0 (and out = "") at 100 %, where nothing is set and X's default holds, or
 * if it doesn't fit. */
size_t xws_resources(int pct, char *out, size_t size);

#endif
