/* logclean.h — text from a community theme on its way to the session log (security scan 0.8.9.6.2, F8).
 * lord-log splits stderr on line breaks and takes each line's subsystem from its own "word: " prefix, so a theme's
 * lord.log or console text (or the message and stack of an exception it threw) could forge whole lines attributed to
 * any subsystem. In a community session each line is cleaned and printed with a fixed prefix. Pure; tested by
 * build/logclean-test. */
#ifndef LORD_UI_LOGCLEAN_H
#define LORD_UI_LOGCLEAN_H

#include <stddef.h>
#include <stdio.h>

#define LOGCLEAN_LINE_MAX 2048      /* bytes of one printed line (longer: cut, "…" appended) */
#define LOGCLEAN_LINES_MAX 64       /* lines of one call (more: one "(N more lines)" line) */

/* One line of in[0..n) (no '\n' in it) into out (cap > 4): every C0 control (tab, CR, ESC…), DEL, C1 control
 * (U+0080–U+009F), line or paragraph separator (U+2028/2029) and bidi control (U+202A–U+202E, U+2066–U+2069)
 * becomes '?'; a malformed byte too. Returns the length written (out NUL-terminated). */
size_t logclean_line(const char *in, size_t n, char *out, size_t cap);
/* s split on '\n', each line cleaned and written as "<prefix><line>\n" (at most LOGCLEAN_LINES_MAX lines). */
void logclean_print(FILE *f, const char *prefix, const char *s);

#endif
