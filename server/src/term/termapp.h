/* termapp.h — terminal apps (.desktop with Terminal=true): the one-shot slots between the launcher
 * (apps.c) and the Terminal's lord.term.create({ app: token }) (ui/js_term.c), and the command line
 * lord-server types into the tab's login shell. See termapp.c and docs/TERMINAL.md, Security. */
#ifndef LORD_TERMAPP_H
#define LORD_TERMAPP_H

#include <stddef.h>

#define TERMAPP_TOKEN_HEX 32            /* 128 random bits, as hex */
#define TERMAPP_MAX_PENDING 4           /* the oldest pending slot goes when a fifth one is stored */
#define TERMAPP_TTL_MS 10000.0          /* a slot not taken in 10 s is gone */
#define TERMAPP_ARGV_MAX 4096           /* the argv's bytes, NULs included (apps.c's split_exec buffer) */
#define TERMAPP_LINE_MAX 8192           /* the quoted line, exit rule included */

/* The monotonic clock in milliseconds that the slots use. */
double termapp_now(void);

/* Stores argv (argc elements, already parsed from the Exec line by apps.c) in a slot; the token goes to
 * token (TERMAPP_TOKEN_HEX + 1 bytes). -1 with nothing stored if argv is empty, too long, has an element
 * with a control character or invalid UTF-8 (termapp_arg_ok), or there is no randomness. */
int termapp_put(char *const *argv, int argc, double now, char *token);

/* Takes the slot of token and frees it (single use): its command line, every element single-quoted
 * (termapp_quote), followed by the exit rule's suffix (termapp_exit_suffix). -1 for an unknown, used or
 * expired token, or a line that doesn't fit: nothing is written to line then. */
int termapp_take(const char *token, double now, int fish, const char *exit_rule, char *line, size_t n);

/* An argument that may be typed: valid UTF-8 with no C0 or C1 control character and no DEL. */
int termapp_arg_ok(const char *s);

/* POSIX shell quoting of argv into out: 'arg' with every ' as '\'' , elements separated by one space.
 * fish (whose single quotes also take \\ and \' as escapes) gets every backslash doubled as well.
 * -1 if an element fails termapp_arg_ok or the line doesn't fit. */
int termapp_quote(char *const *argv, int argc, int fish, char *out, size_t n);

/* "close" → "; exit", "closeIfOk" → " && exit", "keep" → ""; anything else is "close". */
const char *termapp_exit_suffix(const char *rule);

/* How many slots are pending now (tests). */
int termapp_pending(double now);
/* Forgets every slot (tests). */
void termapp_reset(void);

#endif
