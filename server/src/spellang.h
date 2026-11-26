/* spellang.h — which spelling dictionaries LordOS checks with (docs/WEBAPPS.md, "Spelling"): one small model for every
 * spell checker LordOS has. Today lord-web (WebKit's own checker, through enchant and hunspell) reads it; LordOS's own
 * text fields will read the same one, so both always agree. Pure apart from spellang_installed (tests/webapp_test.c).
 *
 * The rule: the interface language's dictionary, then English's, only ones that are installed (hunspell's
 * /usr/share/hunspell/<tag>.dic, the folder hunspell-en-us, hunspell-es and hunspell-pt-br install to). For a language
 * the locale's own region comes first (es_AR for es_AR.UTF-8), then the language's usual one (es_ES, pt_BR, en_US…),
 * then the bare language, then any installed region of it (the first in the sorted list). */
#ifndef LORD_SPELLANG_H
#define LORD_SPELLANG_H

#include <stddef.h>

#define SPELLANG_TAG_MAX  16                    /* "es_AR", "pt_BR", "ca_ES-valencia" cut: tags longer are left out */
#define SPELLANG_MAX      2                     /* the interface language and English */
#define SPELLANG_DIR      "/usr/share/hunspell"

/* The dictionaries for locale ("es_AR.UTF-8", "pt", "zh-Hans"; NULL or "" or "C" is English) among installed (n
 * tags, sorted), in order, into out; their count (0..SPELLANG_MAX). */
int spellang_choose(const char *locale, const char *const *installed, int n, char out[][SPELLANG_TAG_MAX], int max);

/* The tags of the dictionaries in dir (each <tag>.dic with its <tag>.aff), sorted, into out (at most max); the count. */
int spellang_installed(const char *dir, char out[][SPELLANG_TAG_MAX], int max);

#endif
