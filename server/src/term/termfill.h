/* termfill.h — Passwords in the Terminal (docs/TERMINAL.md, docs/PASSWORDS.md): when a program asks for a
 * password (term_asks_secret), the Terminal offers the Passwords entries; the one the user picks goes
 * from lord-vault to here on the private channel (vc_request_private: the reply never reaches JS) and is
 * written to that session's pty with Enter, then wiped. Nothing else reads it: not JS, not the screen,
 * not the session log (the program has echo off). */
#ifndef LORD_TERM_TERMFILL_H
#define LORD_TERM_TERMFILL_H

#include <stddef.h>
#include <stdint.h>

/* Asks lord-vault for an entry's field (vault "main" or "login", entry id, field key such as "password",
 * the field's index among those with that key) to write in session sid. Refused (-1) when the session
 * isn't asking for a password right now, an argument isn't valid or too many fills are pending. */
int termfill_start(int sid, const char *vault, const char *entry, const char *field, int index);
/* A private reply from lord-vault (brokerkr_tick passes on the ones that aren't its own): 1 if it was a
 * fill's. The reply is the caller's to wipe and free. */
int termfill_reply(uint32_t id, const char *rep, size_t len);
/* Saves the typed password a "Save to Passwords?" offer is about (its token, tcapture.c) in lord-vault, on the
 * private channel; the value is wiped here either way. -1 if there is no such offer any more. The "saved"
 * event says how it ended. */
int termfill_save(int sid, unsigned token);
int termfill_pending(void);             /* fills (and saves) waiting for lord-vault */

#endif
