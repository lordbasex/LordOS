/* lm_svg.h — SVG icons in lord-media (MW_SVG, docs/MEDIA.md "SVG icons").
 *
 * The icon themes ship many icons only as SVG (scalable/ and symbolic/ folders: a tray's volume or network icon,
 * apps without a PNG). nanosvg (third_party/nanosvg, vendored unmodified) parses and rasterizes them here, inside
 * lord-media's sandbox and never in lord-server, which only opens the file and shows the pixels. Every SVG is
 * treated as hostile: at most LM_SVG_MAX_BYTES, a scan of its markup before the parser (elements and nesting), and
 * after parsing a check of what the rasterizer would do with it (shapes, points, finite coordinates near the
 * picture, stroke widths and dashes in pixels) before it runs; lord-server kills the helper if a job still takes
 * too long (media.c, SVG_JOB_MS). No FFmpeg here: build/svgrast-test runs it directly. */
#ifndef LM_SVG_H
#define LM_SVG_H

#include <stddef.h>
#include <stdint.h>

#define LM_SVG_MAX_BYTES (1 << 20)      /* a larger file is refused before reading it (lord-server checks it too) */
#define LM_SVG_MIN_SIDE 16
#define LM_SVG_MAX_SIDE 256
#define LM_SVG_MAX_ELEMENTS 20000       /* tags in the markup */
#define LM_SVG_MAX_DEPTH 64             /* open elements inside each other */
#define LM_SVG_MAX_SHAPES 4000
#define LM_SVG_MAX_POINTS 200000        /* bezier points of every path together */
#define LM_SVG_MAX_REACH 128            /* no point further than this many sides away from the picture */

/* The markup's element count and deepest nesting (comments, CDATA, declarations and processing instructions
 * skipped). 0 if both are within the limits, -1 if not (or the markup ends inside a tag). */
int lm_svg_scan(const char *data, size_t len, int *elements, int *depth);

/* Rasterizes the SVG in data (len bytes; it's copied, data stays untouched) to fit side x side keeping its aspect
 * (side LM_SVG_MIN_SIDE..LM_SVG_MAX_SIDE). recolor >= 0 (0xRRGGBB): every painted pixel takes that color and
 * keeps its coverage, the way a -symbolic icon follows the text color. On success returns NULL, *out is *w x *h
 * premultiplied BGRA (malloc) and *w, *h are 1..side. Otherwise an error code (errcode.h): media.svgTooLarge,
 * media.svgTooComplex, media.notAnImage, media.invalidArguments or sys.outOfMemory. */
const char *lm_svg_raster(const char *data, size_t len, int side, long recolor, uint8_t **out, int *w, int *h);

#endif
