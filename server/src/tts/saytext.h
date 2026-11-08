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

#endif
