/* pastefilter.h — what a paste may type into the Terminal (term.c term_paste; see pastefilter.c). */
#ifndef LORD_PASTEFILTER_H
#define LORD_PASTEFILTER_H

#include <stddef.h>

/* Copies the n bytes of in to out (room for n) as a paste types them: line ends become CR, tab stays, and every
 * other control character (C0, ESC, DEL) is dropped, so no sequence can be rebuilt from what is left. Returns the
 * bytes written. */
size_t paste_filter(const char *in, size_t n, char *out);

#endif
