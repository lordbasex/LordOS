/* pkfield.h — the pure parts of the authentication agent (pkagent.c): the password field, which is
 * never given to JavaScript, the lines polkit's helper writes, and what is shown from a request.
 * No I/O: tests/pkfield_test.c runs all of it. */
#ifndef LORD_HW_PKFIELD_H
#define LORD_HW_PKFIELD_H

#include <stddef.h>

#define PKF_MAX 512

typedef struct {
    char *buf;                  /* PKF_MAX bytes */
    size_t len;
} PkField;

/* Takes a locked page: not swapped, not in a core dump, not inherited by a child. 0 ok. */
int  pkf_open(PkField *f);
void pkf_wipe(PkField *f);      /* wipes what is typed; the page stays */
/* Adds the text of a key (UTF-8). Control characters are refused, and a text that doesn't fit. 1 added. */
int  pkf_add(PkField *f, const char *text);
void pkf_back(PkField *f);      /* removes the last whole character */
size_t pkf_chars(const PkField *f);

typedef enum { PKL_OTHER, PKL_ASK_HIDDEN, PKL_ASK_SHOWN, PKL_ERROR, PKL_INFO, PKL_SUCCESS, PKL_FAILURE } PkLine;
PkLine pk_line_kind(const char *line);

/* Copies text from a request to show it: no control characters and no bidi or invisible format
 * characters that could reorder or hide a command line; invalid UTF-8 is dropped; cut at a whole
 * character. out always ends in NUL. */
void pk_clean(char *out, size_t n, const char *in);

/* Field 22 of a /proc/<pid>/stat text (the comm may contain spaces and parentheses). 0 if malformed. */
unsigned long long pk_stat_start(const char *stat);

#endif
