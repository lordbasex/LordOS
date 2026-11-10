/* svgicon.h — SVG icons for lord-server's images (docs/MEDIA.md, "SVG icons").
 *
 * An <Image> whose src is an .svg file, or "svg:<px>:<rrggbb|->:<absolute path>" (sdk/iconsrc.js), is drawn by
 * lord-media (MW_SVG, helper/lm_svg.h), never parsed here: lord-server only checks the path (iconfind_svg_allowed),
 * opens it (O_NOFOLLOW, a regular file of at most 1 MB), passes the fd and shows the pixels that come back. The
 * pictures stay in the image cache (ui/image.c) under path, mtime, size, side and color, so a Dock and a bar showing
 * the same icon draw it once; a file that changes is drawn again. */
#ifndef LORD_SVGICON_H
#define LORD_SVGICON_H

#include <stdint.h>
#include "../ui/ui.h"

#define SVGICON_DEFAULT_SIDE 128        /* a plain .svg src (an app's icon in the Dock or the launcher) */

/* Called by the desktop session before its theme loads; app mode (lord-server --app) never asks lord-media. */
void svgicon_enable(void);

/* The image for an SVG drawn to fit side x side (clamped to 16..256), every painted pixel in color (0xRRGGBB) when
 * color >= 0. Empty until lord-media answers; NULL if the path isn't an allowed .svg or can't be opened. One holder
 * (image_release). */
Image *svgicon_get(const char *path, int side, long color);

/* "svg:<px>:<rrggbb|->:<absolute path>" → svgicon_get; NULL if it doesn't parse. */
Image *svgicon_get_spec(const char *spec);

/* media.c: the answer to a MW_SVG job (argb NULL: it failed). */
void svgicon_done(const char *key, int w, int h, const uint32_t *argb);

#endif
