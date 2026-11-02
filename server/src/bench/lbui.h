/* lbui.h — LordBench's workloads on LordOS's own interface code (docs/BENCHMARK.md, "Workloads"): the UI
 * engine's layout and paint (ui/node.c, layout.c, paint.c, text.c, canvas.c) on a fixed scene, and the
 * shaper (ui/otf.c, shape*.c, bidi.c) on fixed paragraphs in five scripts. Everything they use is built
 * into lord-bench: the scene is made in C from fixed data, the fonts are bytes in the binary
 * (build/gen/lb_fonts.h: LordOS's Inter and the subset Noto fonts of src/bench/fonts), and nothing is
 * opened by path. Used by lord-bench (lbmain.c) and tested in tests/lordbench_test.c. */
#ifndef LORD_BENCH_LBUI_H
#define LORD_BENCH_LBUI_H

#define LB_UI_W 1920                    /* the scene's size, logical pixels at scale 1 */
#define LB_UI_H 1080
#define LB_UI_POOL_MAX 8                /* paint threads at most, as lord-server's (par.c) */

/* one-time setup of the engine with the built-in interface font; 0 ok */
int lb_ui_init(void);

/* frames per second of the fixed scene: each frame lays the whole tree out again (every node
 * invalidated; the glyph and text caches stay warm, as on a running desktop) and paints the whole
 * 1920×1080 screen. threads = 1: everything on one core; threads > 1: the same frames with the paint's
 * pixel loops split in row bands over min(threads, LB_UI_POOL_MAX) threads, the way lord-server's paint
 * threads do (canvas_parallel). -1 if it could not run. */
double lb_ui(int threads, double secs);

/* runs per second of shaping the five fixed paragraphs (lbtext.h), summed over the threads; one run
 * shapes every paragraph once as the engine's text layout does (textlayout.c, its first step): bidi
 * levels, scripts with the common characters joining their neighbours, each character's font (Inter, else
 * its script's), each run of one font, script and direction shaped (GSUB and GPOS), then the visual order
 * of the paragraph as one line. Each thread has its own shaper and font tables. */
double lb_shape(int threads, double secs);

/* for the tests: shapes paragraph p once; glyphs, the glyphs with no glyph in their font (.notdef) and the
 * sum of the advances in each font's units (a checksum). 0 ok, -1 failed */
int lb_shape_check(int p, int *glyphs, int *notdef, long *advance);
/* for the tests: the nodes of the scene and the style keys the engine didn't know (must be 0) */
int lb_ui_check(int *nodes, int *unknown_styles);

#endif
