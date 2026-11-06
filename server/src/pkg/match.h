/* match.h — how a search text matches a package (0.8.8.4, #37): the words of the query against the
 * package name, the app's name, the app's summary and the package summary, case- and accent-blind,
 * with the query's words joined ("vs code" also finds "vscode") and at least one word needed when
 * not all of them are there. Pure functions: lord.pkg.search (ui/js_pkg.c) ranks with them and
 * build/pkg-test tests them. The Store's catalog search (apps/store/search.js) follows the same rules. */
#ifndef LORD_PKG_MATCH_H
#define LORD_PKG_MATCH_H

#include <stddef.h>

#define PKG_QUERY_WORDS 8
#define PKG_QUERY_WORD  64

typedef struct {
    int n;                                          /* words */
    char w[PKG_QUERY_WORDS][PKG_QUERY_WORD];        /* folded words */
    size_t len[PKG_QUERY_WORDS];
    char joined[PKG_QUERY_WORDS * PKG_QUERY_WORD];  /* the words without spaces ("vscode") */
    size_t jlen;
} PkgQuery;

/* Lower case, the accented Latin letters (Latin-1 and Latin Extended-A) as their base letter,
 * combining marks dropped; every other byte unchanged. Writes at most cap-1 bytes and a NUL;
 * returns the length. */
size_t pkg_fold(const char *in, char *out, size_t cap);

/* The same without spaces or punctuation (only letters, digits and non-ASCII bytes): "VS Code" → "vscode". */
size_t pkg_compact(const char *in, char *out, size_t cap);

/* Splits a query into folded words (spaces and punctuation separate them; at most 8 of 63 bytes). */
void pkg_query_parse(PkgQuery *q, const char *text);

/* How well a word appears in a folded text: 4 the whole text, 3 its start, 2 the start of a word,
 * 1 inside a word (words of 3 bytes or more) or across words in `compact` (may be NULL), 0 not at all. */
int pkg_match_level(const char *folded, const char *compact, const char *w, size_t wl);

/* The score of a package for the query (any of the texts may be NULL or empty; they are folded here).
 * Returns 0 when it doesn't match: every word must be somewhere, or (with two words or more) the joined
 * words must be in the name or the app's name, or at least one word must start a word of the name or
 * the app's name ("partial": *full = 0, ranked below every full match). An empty query matches
 * nothing. */
int pkg_match_score(const PkgQuery *q, const char *name, const char *app_name, const char *app_summary,
                    const char *summary, int *full);

#endif
