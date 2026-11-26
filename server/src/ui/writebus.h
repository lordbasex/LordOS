/* writebus.h — ✨ AI Writing for the `lumo` command (docs/AI.md "Writing from the terminal"): org.lordos.Writing1 on the
 * session bus, served by lord-server on portal.c's connection. Awaiting the maintainer's approval (AGENTS.md).
 *
 *   Status() → (s state, s model, s engine)   state "ready" | "off" (Writing turned off) | "no-model" | "no-engine";
 *                                             model the id Writing uses ("" none); engine "off"|"starting"|"ready"|"busy"
 *   Start(s action, s target, s text) → (u id)  one of writing.c's fixed actions ("fix", "improve", "shorter",
 *                                             "translate" with target an interface language, else ""), the text at most
 *                                             16,000 bytes / 4,000 characters; the same job as the panel's (llm/engine.c
 *                                             llm_submit_write): the prompts are writing.c's, nothing goes online
 *   Read(u id) → (s state, s text)            "queued" | "starting" | "writing" (text so far) | "done" (the whole text)
 *                                             | "failed" (text: an error code, e.g. "llm.timeout"); after done or
 *                                             failed the job is gone
 *   Cancel(u id)
 *
 * Only for the user lord-server runs as: the bus says each caller's uid (GetConnectionUnixUser) before its first call is
 * served, and a caller of another uid gets AccessDenied. A job belongs to the caller that started it (its unique name):
 * nobody else reads or cancels it, and it is cancelled when that caller leaves the bus. At most WRITEBUS_JOBS at once.
 * Writing's switch and model are the shell's (lord.writing.serve, sdk/writing.jsx): off, Start answers
 * org.lordos.Writing1.Error.Off. */
#ifndef LORD_UI_WRITEBUS_H
#define LORD_UI_WRITEBUS_H

#include "../vault/dbus.h"

#define WRITEBUS_JOBS    4
#define WRITEBUS_CALLERS 8

/* a call on LUMO_BUS_PATH */
void writebus_call(DConn *c, const DMsg *m);
/* 1 if m answered one of our uid questions */
int  writebus_reply(DConn *c, const DMsg *m);
/* that connection left the bus: its jobs are cancelled */
void writebus_gone(const char *name);
/* the bus went away */
void writebus_lost(void);
/* the shell's Writing switch (on: the person didn't turn it off) and chosen model (Settings → AI → Brain, may be NULL) */
void writebus_serve(int on, const char *chosen);

/* ---- pure, for tests (tests/lumo_test.c) ---- */
/* the model Writing uses: chosen when it is among the installed ids (models_json, llm_models_json's form), else the
 * first; into out (64 bytes). 0, or -1 none. */
int  writebus_pick_model(const char *models_json, const char *chosen, char out[64]);
/* Read's state and text from llm_take's result (final: "done" with its content, "failed" with its error) or llm_peek's
 * (NULL: "queued"); *text is malloc'd. */
const char *writebus_state(const char *take_json, const char *peek_json, char **text);

#endif
