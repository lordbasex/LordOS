/* lumo.h — the `lumo` command's pure parts (src/cli/lumo.c, docs/AI.md "Writing from the terminal"): its verbs in
 * English and Spanish, a translation's target named by code or name, the session bus method's words, and the model's
 * text made safe for a terminal as it streams. Tested by tests/lumo_test.c. */
#ifndef LORD_LLM_LUMO_H
#define LORD_LLM_LUMO_H

#include <stddef.h>

/* org.lordos.Writing1 on the session bus, served by lord-server (ui/writebus.c) */
#define LUMO_BUS_NAME  "org.lordos.Writing1"
#define LUMO_BUS_PATH  "/org/lordos/Writing1"
#define LUMO_BUS_IFACE "org.lordos.Writing1"
#define LUMO_ERR_OFF      "org.lordos.Writing1.Error.Off"        /* Writing is off (Settings → AI → Writing) */
#define LUMO_ERR_NO_MODEL "org.lordos.Writing1.Error.NoModel"    /* no model installed */
#define LUMO_ERR_NO_ENGINE "org.lordos.Writing1.Error.NoEngine"  /* the engine (llama.cpp) isn't installed */
#define LUMO_ERR_BUSY     "org.lordos.Writing1.Error.Busy"       /* too many jobs queued */
#define LUMO_ERR_DENIED   "org.freedesktop.DBus.Error.AccessDenied"
#define LUMO_TEXT_MAX     16000                                  /* bytes of text a job takes (writing.h LLM_WRITE_MAX) */
#define LUMO_CHARS_MAX    4000                                   /* characters, as the panel (writingmodel.js) */

enum { LUMO_NONE = 0, LUMO_FIX, LUMO_IMPROVE, LUMO_SHORTER, LUMO_TRANSLATE, LUMO_STATUS, LUMO_HELP, LUMO_VERSION };

/* A verb, in English or Spanish, ASCII case ignored and Spanish accents optional: correct/fix/corregir, improve/mejorar,
 * summarize/shorten/shorter/resumir/acortar, translate/traducir, status/estado, help/ayuda/--help/-h,
 * --version; LUMO_NONE otherwise. */
int lumo_verb(const char *word);
/* The bus action for a writing verb ("fix", "improve", "shorter", "translate"), NULL for the others. */
const char *lumo_action(int verb);
/* A translation target: an interface language's code ("en", "pt", "zh-Hans", case ignored, "zh" Simplified, "pt_BR"
 * and "en-US" style regions dropped) or its name in English or Spanish ("english", "inglés", "portugues"…, case and
 * accents ignored); into code (16 bytes). 0, or -1 unknown. */
int lumo_lang(const char *word, char code[16]);
/* The text's size for a job: 0 fits, -1 empty (only spaces), -2 too long (bytes or characters), -3 not UTF-8 or a NUL */
int lumo_text_ok(const char *s, size_t n);

/* The model's text as it streams, made safe and plain for a terminal: each call gets the whole text so far and writes
 * into out what wasn't written before, without control characters (line breaks and tabs stay), DEL, C1 controls or
 * bidi controls (U+200E/F, U+202A..E, U+2066..9), leading blank space, a "<think>…</think>" block or the request's
 * markers echoed on their own line ("<<<TEXT …>>>"); an incomplete last line that may still become a marker, and an
 * incomplete UTF-8 sequence, wait for the next call (or final). out gets at most cap - 1 bytes and a NUL. */
typedef struct {
    size_t seen;            /* bytes of the text already taken */
    int started;            /* something but space was written */
    int think;              /* inside a <think> block */
    size_t blanks;          /* line breaks held until more text comes (trailing ones are dropped) */
    int bol;                /* the next byte starts a line */
} LumoOut;
void lumo_out_init(LumoOut *o);
size_t lumo_out_feed(LumoOut *o, const char *text, int final, char *out, size_t cap);

#endif
