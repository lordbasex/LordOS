/* brokerclip.h — what an isolated app may know about the clipboard after it copied something
 * (BOP_CLIP_SET, BOP_CLIP_SECRET, BOP_CLIP_STATUS): whether its copy is still there, was cleared on time or
 * was replaced by something else, and how long a secret has left. Never what the clipboard holds: an
 * app without the clipboard permission learns only about its own copy. Pure logic (no lord-server state):
 * the caller passes the clipboard's generation, which changes with every copy, text or picture. */
#ifndef LORD_BROKERCLIP_H
#define LORD_BROKERCLIP_H

enum { BCLIP_NONE, BCLIP_MINE, BCLIP_CLEARED, BCLIP_REPLACED };

/* `app` put something on the clipboard, which is now at generation `gen`; a secret is cleared after
 * `secs` (1..600, otherwise kept) if nothing replaced it */
void bclip_copied(const char *app, unsigned gen, int secret, int secs, double now);
/* 1: a secret's time is up and the clipboard is still that copy: the caller clears it, then calls
 * bclip_cleared() with the new generation */
int bclip_due(unsigned gen, double now);
void bclip_cleared(unsigned gen);
/* BCLIP_* for `app` (BCLIP_NONE when another app copied last); *left = whole seconds before a secret
 * is cleared, 0 when it won't be */
int bclip_status(const char *app, unsigned gen, double now, int *left);
/* `app`'s secret, still on the clipboard, stays there (no clearing): 1, or 0 when it isn't its copy */
int bclip_keep(const char *app, unsigned gen);

#endif
