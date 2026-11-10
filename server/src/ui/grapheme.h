/* grapheme.h — extended grapheme clusters (UAX #29): what a person sees as one character, so the
 * caret steps over it whole (an emoji ZWJ sequence, a flag, a letter with its marks, a Hangul
 * syllable, an Indic conjunct). See grapheme.c. */
#ifndef LORD_GRAPHEME_H
#define LORD_GRAPHEME_H

#include <stdint.h>

/* End of the cluster that starts at i (i < n), or n. */
int grapheme_next(const uint32_t *cp, int n, int i);

/* brk[k] = 1 where a cluster starts (brk[0] and brk[n] are 1, brk has n + 1 entries). Returns the
 * number of clusters. */
int grapheme_breaks(const uint32_t *cp, int n, uint8_t *brk);

/* The cluster boundaries of UTF-8 text in code points (0, …, n), malloc'ed; *count gets how many.
 * NULL past max code points or without memory. */
int *grapheme_stops_utf8(const char *s, int max, int *count);

#endif
