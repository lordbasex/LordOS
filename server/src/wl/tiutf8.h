/* tiutf8.h — the pure parts of zwp_text_input_v3 (wl_textinput.c): every byte offset LordOS sends to a
 * Linux app and every text, offset and rectangle the app sends, checked against the UTF-8 it belongs
 * to. Nothing here trusts the client: a length or offset is clamped to the text and to a character
 * boundary, invalid UTF-8 is cut where it starts. */
#ifndef LORD_TIUTF8_H
#define LORD_TIUTF8_H

#include <stddef.h>
#include <stdint.h>

#define TIU_SURROUNDING 4000    /* the most of the app's text kept around the caret (bytes) */
#define TIU_COORD 32767         /* a cursor rectangle's coordinates and size are clamped to this */

/* Bytes of the longest prefix of s (at most max bytes) that is valid UTF-8 and ends on a character
 * boundary: no overlong forms, surrogates or code points past U+10FFFF. */
size_t tiu_valid_prefix(const char *s, size_t max);

/* 1 if off is a character boundary of the len-byte valid UTF-8 text s (0 and len are). */
int tiu_boundary(const char *s, size_t len, int64_t off);

/* The preedit to send: *len = its valid prefix (at most max bytes); the cursor covers the focused
 * segment [fb, fe) when it is a non-empty range on character boundaries, otherwise it sits at the end. */
void tiu_preedit(const char *s, size_t max, int fb, int fe, size_t *len, int *begin, int *end);

/* set_surrounding_text: copies text into out (size bytes, NUL-terminated) cut to its valid prefix and
 * to TIU_SURROUNDING. Returns 1 and the cursor and anchor when both fall on boundaries of what was
 * kept; 0 (out = "") when the text or an offset is garbage. */
int tiu_surrounding(const char *text, int32_t cursor, int32_t anchor, char *out, size_t size, int *cur, int *anc);

/* delete_surrounding_text around the cursor of the len-byte text s: before and after are cut to the
 * text and moved inwards to character boundaries, so the app never deletes half a character. */
void tiu_delete(const char *s, size_t len, int cursor, uint32_t before, uint32_t after, uint32_t *b, uint32_t *a);

/* set_cursor_rectangle: x and y clamped to [-TIU_COORD, TIU_COORD], width and height to [0, TIU_COORD]. */
void tiu_rect(int32_t x, int32_t y, int32_t w, int32_t h, int out[4]);

/* The rectangle (surface coordinates) placed inside a surface of lw × lh: the candidate window goes
 * next to it, so a caret the app puts far outside its own window is brought back to the edge. */
void tiu_rect_in(const int r[4], int lw, int lh, int out[4]);

#endif
