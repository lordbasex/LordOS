/* portalshot.c — the pictures of the Screenshot portal (ui/portal.c; approved by the maintainer 2026-11-22): what the
 * person picked in LordOS's chooser, painted as a screen share paints it (cast_still: protected windows black), encoded
 * to PNG by lord-media into the file the shell named (in the home folder, never over an existing file), and the color
 * under a point for PickColor. Kept out of portal.c so the portal's bus tests need no compositor. */
#include "portal.h"
#include "../cast/cast.h"
#include "../media/media.h"
#include "../media/media_int.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

int portal_shot_capture(int tag, const CastSource *src, const int *area, const char *path) {
    int w = 0, h = 0;
    uint32_t *px = cast_still(src, area, &w, &h);
    if (!px) return -1;
    char tmp[PATH_MAX + 64], dst[PATH_MAX];
    int o = media_open_output(path, tmp, sizeof tmp, dst, sizeof dst);
    if (o < 0) { free(px); return -1; }
    int mfd = media_memfd_from_pixels(px, w, h);
    free(px);
    if (mfd < 0) { close(o); unlink(tmp); return -1; }
    media_portal_saved = portal_shot_saved;
    int fds[2] = { mfd, o };
    return media_job(0, MW_ENCODE, JP_PORTAL, fds, 2, w, h, "format=png\n", tmp, dst, tag) > 0 ? 0 : -1;
}

int portal_shot_pixel(const CastSource *src, int x, int y, double rgb[3]) {
    int area[4] = { x, y, 1, 1 }, w = 0, h = 0;
    uint32_t *px = cast_still(src, area, &w, &h);
    if (!px) return -1;
    uint32_t c = px[0];
    free(px);
    rgb[0] = ((c >> 16) & 0xff) / 255.0;
    rgb[1] = ((c >> 8) & 0xff) / 255.0;
    rgb[2] = (c & 0xff) / 255.0;
    return 0;
}
