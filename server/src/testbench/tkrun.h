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

typedef struct {
    const char *dir;                    /* the harnesses (TK_HARNESS_DIR, or LORDOS_TOOLKIT_AUDIT_DIR) */
    const char *python;                 /* TK_PYTHON (the unit tests pass a fake one) */
    int kill_ms, probe_ms;
    int progress;                       /* "Testing ..." on stderr */
    TkText T;
    char *const *env;                   /* the caller's environment (environ): only the listed variables are kept */
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

/* lordos-testbench --toolkits [--json] [--only id,...] */
int toolkits_main(int argc, char **argv);

#endif
