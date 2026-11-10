/* caret.h — where each character of one shaped line sits on screen, for the caret and the selection
 * of a text field in text that mixes directions (Hebrew or Arabic with numbers or Latin). See caret.c. */
#ifndef LORD_CARET_H
#define LORD_CARET_H

#include <stdint.h>

/* A glyph of the line in visual order (left to right): the code point that owns it (its cluster's
 * first one; -1 for a mark drawn on another glyph) and how far it moves the pen. */
typedef struct { int cp; float adv; } CaretGlyph;

/* A level run (bidi.c) in visual order: code points [start, end), right to left or not, and where
 * it is drawn. */
typedef struct { int start, end, rtl; float x, w; } CaretRun;

/* x0[i], x1[i]: the left and right edges of code point i, from the line's left edge. lv: the
 * resolved levels (after L1). Every code point of a grapheme cluster (grapheme.c) gets the
 * cluster's edges; a glyph that draws several clusters (a ligature: lam-alef, "fi") is split
 * evenly between them, the first one on its right in a right-to-left run. Returns -1 without memory. */
int caret_edges(const uint32_t *cp, int n, const uint8_t *lv, const CaretGlyph *g, int ng, float *x0, float *x1);

/* The level runs of the line in visual order; at most max. Returns how many. */
int caret_runs(const uint8_t *lv, int n, const float *x0, const float *x1, CaretRun *out, int max);

#endif
