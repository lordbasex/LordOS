/* spellsvc.h — lord-server's side of the spell checker (docs/SPELLING.md): lord-spell started on demand with the chosen
 * languages' dictionaries, its answers read on this module's own thread, and the person's dictionary.
 *
 * lord-server never loads Hunspell. It opens the dictionaries (/usr/share/hunspell/<tag>.aff and .dic, regular files
 * of at most 64 MB, the real file under /usr/share/hunspell or /usr/share/myspell) and hands them to lord-spell as
 * descriptors with the 'L' frame; everything that waits (starting the helper, an answer) happens on the service's
 * thread, so the screen's thread only queues a job and takes its result later (lord.spell.take). The helper is stopped
 * SPELL_IDLE_MS after its last answer and restarted when the languages change; one that misbehaves (a malformed frame,
 * no answer within SPELL_REPLY_MS) is killed. */
#ifndef LORD_SPELLSVC_H
#define LORD_SPELLSVC_H

#include <stddef.h>
#include "spellproto.h"

#define SPELL_IDLE_MS (10 * 60 * 1000)
#define SPELL_START_MS 15000           /* loading the dictionaries (a large one from a cold disk) */
#define SPELL_REPLY_MS 4000            /* Hunspell's suggestions for a long word can take a while */
#define SPELL_INSTALLED_MAX 96
#define SPELL_DICT_WORDS_MAX 5000      /* the person's dictionary */

/* The installed dictionaries (<tag>.dic with its <tag>.aff in the dictionaries' folder), sorted; their count. */
int spell_installed(SpellTag *out, int max);
/* The same as a JSON array of strings (malloc'd). */
char *spell_installed_json(void);
/* The languages to check with, in order (at most SPELL_LANGS_MAX; invalid or missing tags are left out); how many
 * were kept. A change restarts the helper on its next job. */
int spell_set_langs(const char *const *tags, int n);
/* {"langs":["es_AR","en_US"],"loaded":[1,1],"state":"off|starting|ready|failed","queued":N} (malloc'd) */
char *spell_state_json(void);

/* Queue a job; its id (> 0) or -1 (errno: EINVAL, EBUSY when the queue is full). A word that isn't one lord-spell
 * may be asked about (spell_word_ok) counts as good and is never sent. */
int spell_submit_check(const char *const *words, int n);
int spell_submit_suggest(const char *word);
/* A job's result as JSON (malloc'd), once, or NULL while it runs (or for an unknown id):
 *   check:   {"ok":true,"good":[1,0,…]} (in the words' order)
 *   suggest: {"ok":true,"suggestions":["…",…]}
 *   either:  {"ok":false,"error":"spell.nolang"|"spell.unavailable"|"spell.failed"} */
char *spell_take(int id);
void spell_stop(void);

/* The person's dictionary: ~/.config/lordos/dictionary.txt, one word per line (UTF-8, at most SPELL_DICT_WORDS_MAX,
 * each a word spell_word_ok accepts), written only here (0600, a new file renamed over the old one, no link
 * followed). lord-spell never sees it: a word in it is good before anything is asked. */
char *spell_words_json(void);                       /* ["word",…] (malloc'd) */
int spell_add_word(const char *w);                  /* 0, or -1 (errno: EINVAL, ENOSPC when full, or the write's) */
int spell_remove_word(const char *w);

/* Pure (spelldict.c, tests/spell_proto_test.c): the words of a dictionary file's text, valid ones only, without
 * duplicates, in order; their count. "# " lines are comments. */
int spelldict_parse(const char *text, size_t n, SpellWord *out, int max);
/* The file's text for these words (one per line); its length, or -1 when it doesn't fit. */
int spelldict_format(const SpellWord *w, int n, char *out, size_t cap);

/* Tests: another dictionaries' folder, helper and personal dictionary file (NULL keeps the default). */
void spell_set_paths(const char *dict_dir, const char *helper, const char *words_file);

#endif
