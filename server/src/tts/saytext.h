/* saytext.h — how `say` prepares text for lord-tts (docs/VOICE.md): pure functions, tested by tests/saytext_test.c. */
#ifndef LORD_TTS_SAYTEXT_H
#define LORD_TTS_SAYTEXT_H

#include <stddef.h>

/* In place: terminal escape sequences (CSI, OSC, the two-byte ones) and control characters out, runs of spaces
 * and tabs made one space, three or more line breaks made two, invalid UTF-8 dropped. Returns the new length. */
size_t say_clean(char *s, size_t len);

/* The next piece to speak from s[0..len): after a sentence's end (. ! ? … followed by a space or a line break, or a
 * blank line), or, past max bytes, at the last comma or space before max (never inside a UTF-8 character).
 * *skip is set to the bytes to step over first (spaces, line breaks); the piece is s[*skip .. *skip + return).
 * Returns 0 when nothing but space is left. */
size_t say_next(const char *s, size_t len, size_t max, size_t *skip);

/* The language of s[0..len) among the ones LordOS has voices for, "es", "en", "pt", "fr", "de", "it" or "nl", from its
 * most common words and the letters only one or two of them use (the first 4000 bytes). With too little to tell, the
 * fallback (when it is one of them; English otherwise). */
const char *say_language(const char *s, size_t len, const char *fallback);

/* Spanish, Portuguese, French, German, Italian and Dutch (lang "es", "pt", "fr", "de", "it" or "nl"; any other
 * language, English among them, comes back as it is): numbers, decimals, percentages, ordinals, times, dates, money,
 * degrees, the symbols # @ & + = and (fr, de, it, nl) common abbreviations written out as words, since their Pocket
 * TTS models never learned to read digits (docs/VOICE.md, "Text normalization"). Portuguese is Brazilian's, French
 * France's. region is the user's ("es_AR", "pt_BR", NULL when unknown): "$" is read as pesos in a country whose peso is
 * written "$" (AR, MX, CL, CO, UY, DO, CU), as dollars anywhere else; US$, U$S and USD are always dollars, R$ reais.
 * A new NUL-terminated string, *outlen its length; NULL when out of memory. */
char *say_normalize(const char *s, size_t len, const char *lang, const char *region, size_t *outlen);

#endif
