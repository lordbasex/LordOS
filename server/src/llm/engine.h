/* engine.h — Sentinela's local model engine and the facts it reads about a process (docs/SENTINELA.md).
 *
 * lord-server runs the engine only when Sentinela asks for an analysis: lord-llm (launcher.c) starts llama-server in
 * its sandbox (llmjail.h) with an installed model, the engine listens only on $XDG_RUNTIME_DIR/lordos/llm/engine.sock
 * (0600 in a 0700 folder), and it is stopped LLM_IDLE_MS after its last answer to give the memory back. Everything that
 * waits (starting it, the answer, reading dpkg's lists) happens in this module's own thread; the JS calls only queue a
 * job and later take its result, so the screen's thread never blocks.
 *
 * What lord-server sends is fixed here: the model's system text and the summary come from Sentinela (JS, bounded),
 * the request's parameters and the answer's JSON schema (a verdict, a short reason and one action of a closed list)
 * are this file's. What comes back is only the model's text, treated as untrusted by its reader (sdk/sentinelamodel.js
 * parseVerdict). */
#ifndef LORD_LLM_ENGINE_H
#define LORD_LLM_ENGINE_H

#include <stddef.h>

#define LLM_MODEL_DIR "/usr/share/lordos/llm"     /* the lordos-llm-* packages (tools/llm/mkdeb-llm.mjs) */
#define LLM_ENGINE "/usr/bin/llama-server"        /* LordOS's llama.cpp package (Store → Artificial Intelligence) */
#define LLM_IDLE_MS (120 * 1000)
#define LLM_SYSTEM_MAX 4096
#define LLM_USER_MAX 8192
#define LLM_ANSWER_MAX (256 * 1024)               /* the HTTP answer read from the engine */
#define LLM_CONTENT_MAX 4096                      /* the model's text handed to JS */
#define LLM_START_MS (120 * 1000)                 /* loading a model from a slow disk */
#define LLM_REPLY_MS (180 * 1000)

/* 1 when lord-llm is next to lord-server (owned by root or the user, writable by no one else) and the engine is
 * installed (owned by root, not writable by others). */
int llm_available(void);
/* The installed models: [{"id":"qwen3-1.7b","bytes":N}], read from LLM_MODEL_DIR/<id>.gguf (owned by root or the
 * user, not writable by others). malloc'd. */
char *llm_models_json(void);
/* A model id: [a-z0-9] then [a-z0-9.-], at most 48, no "..". */
int llm_model_id_ok(const char *id);

/* Queue an analysis or a facts reading; a job id (> 0), or -1 (errno: EINVAL, EBUSY when 8 are queued). */
int llm_submit_analyze(const char *model, const char *system, const char *user);
int llm_submit_facts(int pid, unsigned long long start);
/* (0.8.9.6) "Investigate with Lumo", on the person's click only: user (the facts JS may send) followed by what
 * evidence.c reads of the program (a script's text or a binary's metadata, fenced as data), answered with the
 * investigation's schema (what, why, risk, action of the closed list); the result also carries "evidence": what was
 * read. And a description translated (its schema: one "text"). */
#define LLM_EVIDENCE_READ (64 * 1024)             /* a script read at most */
#define LLM_EVIDENCE_SENT 6000                    /* what of it the model reads: its context is 4096 tokens */
#define LLM_TRANSLATE_MAX 2048
int llm_submit_investigate(const char *model, const char *system, const char *user, int pid, unsigned long long start);
int llm_submit_translate(const char *model, const char *system, const char *text);
/* (0.8.9.6.2) "✨ AI" on selected text (writing.h): one of the fixed actions ("fix", "improve", "shorter",
 * "translate" with target a LordOS interface language) on the person's text (at most LLM_WRITE_MAX bytes); a job id or
 * -1 (EINVAL, EBUSY). Its answer is streamed: llm_peek gives the text so far while it runs, llm_take the whole of it at
 * the end: {"ok":true,"content":"…","truncated":b,"ms":N,"startMs":N} or {"ok":false,"error":"llm.<code>"}. */
int llm_submit_write(const char *model, const char *action, const char *target, const char *text);
/* {"state":"writing","text":"…"} while it streams, {"state":"queued"|"starting"} before; NULL otherwise (malloc'd) */
char *llm_peek(int id);
/* Drops a queued job or stops the one streaming (its result: llm.cancelled). */
void llm_cancel(int id);
/* evidence.c: the program's evidence for the process (pid, start): a JSON of what was read ({"kind":"script"|"inline"|
 * "binary"|"none", ...}, malloc'd, NULL on no memory) and in *block the text for the model (malloc'd, or NULL). Only the
 * user's own processes. */
char *llm_evidence(int pid, unsigned long long start, char **block);
/* The result of a job as JSON (malloc'd), once, or NULL while it runs (or for an unknown id).
 *   analyze: {"ok":true,"content":"…","ms":N,"startMs":N,"tokens":N} or {"ok":false,"error":"llm.<code>"}
 *   facts:   {"ok":true,"pid":N,"exe":"…","deleted":b,"pkg":"…"|null,"place":"system|home|temp|other|none",
 *             "exeUid":N,"exeMtime":N,"chain":[{"pid":N,"name":"…","exe":"…","pkg":"…"|null}]} or {"ok":false,…} */
char *llm_take(int id);
/* {"state":"off|starting|ready|busy","model":"…"|null,"pid":N,"idleMs":N} (malloc'd) */
char *llm_state_json(void);
/* Stops the engine now (Sentinela turned off). */
void llm_stop(void);

/* Tests: other folders and programs (NULL keeps the default). */
void llm_set_paths(const char *model_dir, const char *engine, const char *launcher, const char *dpkg_info);
void llm_set_idle_ms(int ms);

/* facts.c: the package that installed a file, from dpkg's lists (<info>/<pkg>[:<arch>].list), scanning them for that
 * exact line (and its /usr-merged twin); NULL when no package has it. Cached per path until the info folder changes. */
int llm_pkg_owner(const char *info, const char *path, char *out, size_t n);
/* Where an executable lives: "system" (/usr, /bin, /sbin, /lib*, /opt, /snap), "home", "temp" (/tmp, /var/tmp,
 * /dev/shm, /run/user), "other", or "none" (no path). */
const char *llm_exe_place(const char *exe, const char *home);
/* The facts JSON of one process (see llm_take), appended to a malloc'd string. */
char *llm_facts_json(int pid, unsigned long long start, const char *dpkg_info, const char *home);

#endif
