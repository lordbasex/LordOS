/* wlcolor.h — color-management-v1 (wp_color_manager_v1) and color-representation-v1
 * (wp_color_representation_manager_v1): what an app says about its pixels' colour, and what LordOS converts.
 * See docs/WAYLAND.md, "Colour management", and colorconv.h for the conversion itself.
 *
 * What is advertised is only what colorconv.c converts:
 *   - features: parametric, set_primaries, set_tf_power (no ICC, no luminances or mastering metadata, no
 *     Windows-scRGB: LordOS has no HDR output);
 *   - rendering intents: perceptual and relative (the same mapping: white adapted to D65, gamut clipped);
 *   - transfer functions: gamma22, srgb, ext_linear, bt1886, gamma28, and power curves;
 *   - every named set of primaries (converted to sRGB's with a matrix);
 *   - alpha modes: premultiplied electrical, premultiplied optical, straight;
 *   - coefficients and range: identity with full range only (every format lord-server takes is RGB; no YUV
 *     buffer is imported).
 * Each output's image description is sRGB primaries with a gamma 2.2 transfer function and the default SDR
 * luminances; its target primaries are the display's own from its EDID when it gives them, else sRGB's.
 *
 * wl.c keeps the Wayland objects of surfaces and outputs; these hooks are all it calls. */
#ifndef LORD_WL_WLCOLOR_H
#define LORD_WL_WLCOLOR_H

#include <wayland-server-core.h>
#include "colorconv.h"

#define WLC_OUT_MAX 16

/* Creates both globals */
void wlcolor_init(struct wl_display *dpy);
/* wl_surface.commit, before its buffer is taken: the pending description, rendering intent and color
 * representation become current. has_buffer: the surface has content after this commit (the pixel format is then
 * checked). 1 if the conversion changed (all of the surface must be drawn again), -1 if a protocol error was posted
 * (the client is gone: do nothing more for it), else 0. */
int  wlcolor_commit(struct wl_resource *surface, int has_buffer);
/* The conversion this surface's pixels need, or NULL (none: they are shown as they are) */
const CcConv *wlcolor_conv(struct wl_resource *surface);
/* A display in output slot `slot` (wl.c's: a wl_output resource's user data is slot + 1, 0 once the display is gone)
 * came or changed. chroma: its EDID's red, green, blue and white x, y ×1,000,000, or NULL when not known.
 * Sends image_description_changed when its description changed; wl.c sends the wl_output.done after it. */
void wlcolor_set_output(int slot, const int chroma[8]);
void wlcolor_output_gone(int slot);
/* The surface's main display (-1: none yet): its preferred image description is that display's. */
void wlcolor_surface_slot(struct wl_resource *surface, int slot);

#endif
