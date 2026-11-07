/* tcapture.h — Save to Passwords from the Terminal (0.8.8.6, docs/PASSWORDS.md, AGENTS.md): what the user types
 * while the program on a session asks for a password is kept here, in locked memory, until the program accepts
 * it; the Terminal then offers to save it, and the value goes from here to lord-vault on the private channel
 * (termfill.c) without passing through JS. Rejected, abandoned, declined or after SAVE_OFFER_MS: wiped.
 *
 * This file is the state machine and the parsing, with no pty and no Session (build/term-capture-test checks
 * it): term.c feeds it the prompt's state, the keys and the output; tsecret.c says what program asks. */
#ifndef LORD_TERM_TCAPTURE_H
#define LORD_TERM_TCAPTURE_H

#include <stddef.h>
#include <stdint.h>

#define CAP_MAX 256                 /* longest password kept (bytes); a longer one is not offered */
#define CAP_SLOTS 8                 /* sessions capturing at once */
#define CAP_VERDICT_MS 1500         /* after Enter, the program must stop asking for this long without failing */
#define CAP_SENT_MS 20000           /* Enter pressed and the program still asking after this: dropped */
#define SAVE_OFFER_MS 60000         /* the offer, and the value, last this long */

/* What asked: the program and the account, from its own arguments (not the text on screen). */
enum { TK_NONE, TK_SSH, TK_DB, TK_GIT, TK_OTHER };
typedef struct TcapTarget {
    int kind;                       /* TK_* */
    char program[32];               /* "ssh", "mysql"… (the executable's name) */
    char engine[16];                /* TK_DB: "MySQL", "PostgreSQL", "Redis", "MongoDB" */
    char host[128], user[64], port[8];
} TcapTarget;

/* Whether a program's password is never offered: the user's own system password (sudo, su, passwd…). */
int tcap_excluded(const char *program);
/* The target from the asking program's argv (and the prompt, for git's URL). 0 if it could be read. */
int tcap_target(const char *const *argv, int argc, const char *prompt, TcapTarget *t);

/* ---- the state machine (one slot per session id) ---- */
/* The session's program asks for a password (on) or stopped asking (off). target is read when it starts. */
void tcap_prompt(int sid, int on, const TcapTarget *target, double now);
/* Keys the user typed in that session (only the user's: not a paste, not a fill from Passwords). */
void tcap_keys(int sid, const char *data, size_t n, double now);
/* The program's output in that session (after Enter it says whether the password was rejected). */
void tcap_output(int sid, const uint8_t *data, size_t n);
/* Time passes: an accepted password becomes an offer, fn(sid, token, target); old ones are wiped. */
void tcap_tick(double now, void (*offer)(int sid, unsigned token, const TcapTarget *t));
/* The value of an offer (NULL if there is none with that token, or it expired), for saving; then wipe it. */
const char *tcap_value(int sid, unsigned token, size_t *len, TcapTarget *t);
void tcap_drop(int sid);            /* wipes the session's slot (declined, saved, the session closed) */
int  tcap_pending(void);            /* slots in use (the main loop keeps ticking while any is) */

#endif
