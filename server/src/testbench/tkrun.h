/* tkrun.h — running the toolkit audit's harnesses (tkrun.c): finds Python 3 and the harness, probes the bindings,
 * runs each combination in its own process group with a clean environment, reads its stdout and stderr, kills it
 * at 8 s, and saves the last report to ~/.cache/lordos/toolkit-audit.json (0600). docs/TOOLKIT-AUDIT.md. */
#ifndef LORD_TESTBENCH_TKRUN_H
#define LORD_TESTBENCH_TKRUN_H

#include "toolkits.h"

#define TK_HARNESS_DIR "/usr/lib/lordos/toolkit-audit"
#define TK_PYTHON "/usr/bin/python3"
#define TK_KILL_MS 8000                 /* a harness exits by itself within 5 s; at 8 s its process group is killed */
#define TK_PROBE_MS 5000
#define TK_SHOW_KILL_MS 20000           /* show mode (--show): a harness finishes within 15 s; killed at 20 s */

/* What tk_audit reports while it runs (TkRunOpts.on_event, from the thread that called it). slot: the index in
 * rep->r. START: the harness was started; STEP and CHECK: a progress line (show mode only, toolkits.h); DONE: the
 * combination's result is final (every combination asked gets one, run or not) and r points at it. */
typedef enum { TK_EV_START, TK_EV_STEP, TK_EV_CHECK, TK_EV_DONE } TkEvKind;
typedef struct { TkEvKind kind; int slot, combo, check, result; const TkResult *r; } TkEvent;
typedef void (*TkEventFn)(void *ctx, const TkEvent *e);

typedef struct {
    const char *dir;                    /* the harnesses (TK_HARNESS_DIR, or LORDOS_TOOLKIT_AUDIT_DIR) */
    const char *python;                 /* TK_PYTHON (the unit tests pass a fake one) */
    int kill_ms, probe_ms;
    int progress;                       /* "Testing ..." on stderr */
    TkText T;
    char *const *env;                   /* the caller's environment (environ): only the listed variables are kept */
    int show;                           /* show mode: the harness gets --show (slowed down, progress lines) */
    TkEventFn on_event;                 /* NULL: none */
    void *ctx;
    volatile int *abort;                /* set (another thread, a signal): the running harness is killed, no more run */
} TkRunOpts;

/* Runs the combinations which[0..n-1] (indexes of tk_combo) in that order. rep's logs are allocated here; free them
 * with tk_report_free. Returns 0, or -1 when memory ran out. */
int tk_audit(const TkRunOpts *o, const int *which, int n, TkReport *rep);
void tk_report_free(TkReport *rep);

/* The environment a combination's harness gets (tests): writes up to max-1 "K=V" strings into out (pointers into
 * buf), NULL-terminated. Returns how many, or -1 when this session has no display for it (no-display). */
int tk_env(const TkRunOpts *o, int combo, char *buf, size_t blen, char **out, int max);

/* Saves rep's JSON to path (made 0600, renamed into place; the folders 0700). 0 or -1. */
int tk_save(const TkReport *rep, const char *path);
/* $XDG_CACHE_HOME/lordos/toolkit-audit.json, or ~/.cache/lordos/toolkit-audit.json; -1 without a home */
int tk_cache_path(char *out, size_t n, char *const *env);

/* <dir>/<toolkit>.py into p when it may be run: dir absolute; dir and file no link, owned by root (TK_HARNESS_DIR)
 * or, for a development folder, by root or this user, and not writable by group or others. 0, or -1. */
int tk_harness_path(const char *dir, const char *toolkit, char *p, size_t n);

/* --only's list into which[] (indexes, no duplicates); the number, or -1 with the bad id (printable, cut) in bad */
int tk_parse_only(const char *only, int *which, char *bad, size_t blen);
/* The harness folder: LORDOS_TOOLKIT_AUDIT_DIR when absolute, else TK_HARNESS_DIR */
const char *tk_harness_dir(void);

/* SIGINT, SIGTERM and SIGHUP kill the running harness's process group, then end this process; SIGPIPE ignored */
void tk_signals(void);

/* lordos-testbench --toolkits [--show] [--json] [--only id,...] */
int toolkits_main(int argc, char **argv);

/* The saved result's bytes (a regular file owned by the caller, no link, at most TK_LAST_MAX), or NULL; free() it. */
char *tk_read_saved(const char *path, size_t *len);

#endif
