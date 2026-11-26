/* spellproto.h — the channel between lord-server and lord-spell, the spell checker's sandboxed helper (docs/SPELLING.md;
 * AGENTS.md "lord-spell"). Strict, length-capped frames, read and checked the same way on both sides (spellproto.c, the
 * only parser; tests/spell_proto_test.c fuzzes it).
 *
 * A frame is four bytes, 'S', the operation, the payload's length (little endian, at most SPELL_PAYLOAD_MAX), then the
 * payload. One request, one answer, in order:
 *
 *   'L' lord-server → helper, once, first:  n (1..SPELL_LANGS_MAX), then n × (length, tag)
 *       the dictionaries come as descriptors: language i's .aff is fd 3 + 2i, its .dic fd 4 + 2i
 *   'R' helper → lord-server:               n, then n bytes: 1 loaded, 0 not (unreadable, an encoding it doesn't know)
 *   'C' check:        mask (the languages, bit i = tag i), n (1..SPELL_BATCH_MAX), then n × (length, word)
 *   'c' its answer:   n (the same n), then n bytes: 1 a language of the mask knows the word, 0 none does
 *   'G' suggestions:  mask, then (length, word)
 *   'g' its answer:   n (0..SPELL_SUGG_MAX), then n × (length, suggestion)
 *
 * A word asked about is 1..SPELL_WORD_MAX bytes of well-formed UTF-8 with no control character, no space and no ASCII
 * punctuation but the apostrophe, the hyphen and the full stop; a suggestion may also have single spaces ("a lot").
 * Anything else, a short read, a frame out of order or a length that disagrees with its payload ends the channel. */
#ifndef LORD_SPELLPROTO_H
#define LORD_SPELLPROTO_H

#include <stddef.h>
#include <stdint.h>

#define SPELL_MAGIC        'S'
#define SPELL_HEAD         4
#define SPELL_PAYLOAD_MAX  8192
#define SPELL_WORD_MAX     64       /* bytes of one word or suggestion */
#define SPELL_BATCH_MAX    64       /* words in one check */
#define SPELL_SUGG_MAX     8
#define SPELL_LANGS_MAX    4
#define SPELL_TAG_MAX      16       /* "pt_BR", "en_US", "es_ES" (with its NUL) */
#define SPELL_DIR          "/usr/share/hunspell"

typedef char SpellWord[SPELL_WORD_MAX + 1];
typedef char SpellTag[SPELL_TAG_MAX];

/* A dictionary's name: [a-z]{2,3}, optionally _ and [A-Z]{2,3} (en_US, es_ES, pt_BR, fil_PH). */
int spell_tag_ok(const char *tag);
/* A word to check (see above), n bytes. */
int spell_word_ok(const char *w, size_t n);
/* A suggestion: as a word, but single spaces between parts are allowed. */
int spell_sugg_ok(const char *w, size_t n);
/* Well-formed UTF-8: no overlong form, no surrogate, nothing past U+10FFFF, no NUL. */
int spell_utf8_ok(const uint8_t *s, size_t n);

/* The frame's head: 0 with its operation and length when it is SPELL_MAGIC, a known operation and len ≤ max. */
int spell_head(const uint8_t h[SPELL_HEAD], int *op, size_t *len);
/* Writes a frame (head + payload) into out (cap bytes); its total length, or -1. */
int spell_frame(uint8_t *out, size_t cap, int op, const uint8_t *payload, size_t len);

/* Payloads. The builders return the payload's length or -1 (a word that isn't valid, too many, no room); the parsers
 * return 0 when the payload is exactly that shape and every word in it valid, -1 otherwise. */
int spell_build_load(uint8_t *out, size_t cap, const SpellTag *tags, int n);
int spell_parse_load(const uint8_t *p, size_t len, SpellTag *tags, int *n);
int spell_build_status(uint8_t *out, size_t cap, const uint8_t *ok, int n);
int spell_parse_status(const uint8_t *p, size_t len, int n, uint8_t *ok);
int spell_build_check(uint8_t *out, size_t cap, unsigned mask, const SpellWord *words, int n);
int spell_parse_check(const uint8_t *p, size_t len, unsigned *mask, SpellWord *words, int *n);
int spell_build_checked(uint8_t *out, size_t cap, const uint8_t *good, int n);
int spell_parse_checked(const uint8_t *p, size_t len, int n, uint8_t *good);
int spell_build_suggest(uint8_t *out, size_t cap, unsigned mask, const char *word);
int spell_parse_suggest(const uint8_t *p, size_t len, unsigned *mask, SpellWord word);
int spell_build_suggestions(uint8_t *out, size_t cap, const SpellWord *s, int n);
int spell_parse_suggestions(const uint8_t *p, size_t len, SpellWord *s, int *n);

#endif
