/* modes.h — KMS mode helpers for Settings → Displays: exact refresh rate, picking a mode by
 * resolution and refresh, and the list of resolutions a connector offers. Pure functions over
 * struct drm_mode_modeinfo (kernel uapi), unit-tested in tests/modes_test.c. */
#ifndef LORD_HW_MODES_H
#define LORD_HW_MODES_H

#include <stdint.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

/* Refresh in millihertz (59940 = 59.94 Hz), from the pixel clock and totals. 0 if invalid. */
int mode_refresh_mhz(const struct drm_mode_modeinfo *m);
/* Index of the best mode: exact w×h (0×0 = the preferred mode), refresh closest to
 * refresh_mhz (0 = highest), progressive before interlaced. -1 if no mode has that size. */
int mode_pick(const struct drm_mode_modeinfo *modes, int n, int w, int h, int refresh_mhz);
/* Index of the preferred mode (DRM_MODE_TYPE_PREFERRED), or the largest one. -1 if n == 0. */
int mode_preferred(const struct drm_mode_modeinfo *modes, int n);
/* The mode a display opens at when nothing is saved: the preferred one, unless its shape is more than 8 % off the
 * panel's physical one (mm_w × mm_h from the EDID, both at least 10 cm) and a progressive mode within 3 % of it
 * exists; then the largest such mode (highest refresh on a tie). GitHub #173. -1 if n == 0. */
int mode_native(const struct drm_mode_modeinfo *modes, int n, int mm_w, int mm_h);
/* Distinct resolutions, largest area first: out[i] = index of the highest-refresh mode of
 * that size. Returns how many (≤ max). */
int mode_resolutions(const struct drm_mode_modeinfo *modes, int n, int *out, int max);
/* Distinct refresh rates (mHz, highest first) offered for w×h. Returns how many (≤ max). */
int mode_rates(const struct drm_mode_modeinfo *modes, int n, int w, int h, int *out, int max);

#endif
