/* toolkits.h — `lordos-testbench --toolkits`: how well the Linux GUI toolkits work on this compositor
 * (docs/TOOLKIT-AUDIT.md). The model is pure (toolkits.c: the combinations, the WAYLAND_DEBUG and stderr reader, the
 * harness's JSON line, the scoring, the JSON and the table); tkrun.c runs the harnesses. Tested by
 * tests/toolkit_audit_test.c with fake harnesses.
 *
 * A combination is one toolkit on one backend (gtk4-wayland, qt5-xcb, ...): its harness, a small Python 3 program in
 * /usr/lib/lordos/toolkit-audit/, opens one window, checks from inside what an app of that toolkit sees, prints ONE
 * JSON line and exits. Its checks are weighted, and on Wayland every global it bound is one more check (weight 1)
 * that passes when no protocol error is attributed to it. A crash, a kill at 8 s or a fatal Wayland, EGL or Vulkan
 * error gives the combination 0 %. Privacy: a result carries only toolkit names and versions, the backend, numbers
 * and check ids; the harness's free text and the log never leave this file's parser. */
#ifndef LORD_TESTBENCH_TOOLKITS_H
#define LORD_TESTBENCH_TOOLKITS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define TK_COMBOS 9                     /* gtk3, gtk4 (wayland, x11), qt5, qt6 (wayland, xcb), x11 (xwayland) */
#define TK_CHECKS 14                    /* the ids every harness shares */
#define TK_GLOBALS 48                   /* expected_globals.h (TB_EXPECTED_N; checked in toolkits.c) */
#define TK_LINE_MAX 4096                /* a stderr line longer than this is read only up to here */
#define TK_OUT_MAX (64 * 1024)          /* the harness's stdout kept: its JSON line must fit */
#define TK_DETAIL_MAX 96

typedef struct {
    const char *id;                     /* "gtk4-wayland" */
    const char *toolkit;                /* "gtk4": the harness is <toolkit>.py, the JSON's "toolkit" */
    const char *label;                  /* "GTK 4" (a product name, not translated) */
    const char *backend;                /* "wayland", "x11" or "xcb" */
    const char *env_var;                /* "GDK_BACKEND", "QT_QPA_PLATFORM" or NULL (x11: no variable) */
    int wayland;                        /* the run uses the Wayland socket (globals are scored) */
    const char *probe;                  /* python3 -I -c code that fails without the bindings */
} TkCombo;

const TkCombo *tk_combo(int i);         /* 0 .. TK_COMBOS-1, NULL otherwise */
int tk_combo_find(const char *id, size_t len);   /* index, or -1 */

/* the shared check ids and their weights: map, size, backend, first_frame, decoration 2; menu, combo, clipboard,
 * dnd 3; scale, theme, tooltip, text, fullscreen 1. A bound global weighs TK_W_GLOBAL. */
const char *tk_check_id(int i);
int tk_check_weight(int i);
#define TK_W_GLOBAL 1

enum { TK_SKIP, TK_PASS, TK_FAIL };     /* a check's result: skipped checks are not counted */

typedef enum {
    TK_ST_NONE,                         /* not run yet */
    TK_ST_RAN,                          /* the harness gave its line; scored */
    TK_ST_FATAL,                        /* crash, timeout, fatal error or no result: 0 % */
    TK_ST_NOT_INSTALLED,                /* the bindings (or Python 3) are missing: not scored */
    TK_ST_NO_DISPLAY,                   /* this session has no such display (no WAYLAND_DISPLAY, no DISPLAY) */
    TK_ST_NO_HARNESS,                   /* the harness file is missing (a broken install) */
} TkStatus;

typedef enum {
    TK_F_NONE, TK_F_TIMEOUT, TK_F_CRASH, TK_F_PROTOCOL, TK_F_LOST, TK_F_EGL, TK_F_VULKAN, TK_F_PLUGIN, TK_F_NO_RESULT,
    TK_F_N
} TkFatal;
const char *tk_fatal_id(TkFatal f);     /* "timeout", "crash", "protocol-error", ... (fixed words) */
const char *tk_status_id(TkStatus s);   /* "ran", "fatal", "not-installed", "no-display", "no-harness" */

/* object id → the expected global its chain was created from (attributes protocol errors) */
#define TK_OBJ_SLOTS 8192

typedef struct {
    /* WAYLAND_DEBUG: the globals bound (highest version, 0 = not bound), protocol errors attributed to them */
    uint32_t bound[TK_GLOBALS];
    unsigned char perr[TK_GLOBALS];
    int extra;                          /* binds of globals outside the table (counted, never named) */
    int protocol_errors, unattributed;
    struct { uint32_t id; int16_t root; } obj[TK_OBJ_SLOTS];
    /* stderr lines being assembled */
    char line[TK_LINE_MAX];
    size_t llen;
    int overlong;                       /* the current line passed TK_LINE_MAX: the rest is dropped */
    TkFatal fatal;                      /* the first fatal pattern seen */
    long lines, long_lines;
} TkLog;

void tk_log_init(TkLog *l);
void tk_log_feed(TkLog *l, const char *buf, size_t n);  /* stderr bytes, any chunking */
void tk_log_end(TkLog *l);                              /* a last line without '\n' */
void tk_log_line(TkLog *l, const char *s, size_t n);    /* one line, no '\n' (tests) */

typedef struct {
    int result;                         /* TK_SKIP, TK_PASS, TK_FAIL */
    int ms;                             /* -1: none */
    char detail[TK_DETAIL_MAX];         /* printable ASCII, shown in the table only; never in the JSON */
} TkCheck;

typedef struct {
    int combo;
    TkStatus status;
    TkFatal fatal;
    int signal_no;                      /* TK_F_CRASH: the signal */
    int exit_code;                      /* -1: signalled or not run */
    int elapsed_ms;
    char version[32];                   /* [0-9A-Za-z.+~-] only */
    char backend_seen[12];              /* "wayland", "x11" or "" */
    int json_ok;                        /* a valid line was found */
    int bad_lines;                      /* stdout lines that weren't a valid result */
    TkCheck checks[TK_CHECKS];
    /* the score (tk_score) */
    int points, total, skipped;
    double score;                       /* -1: nothing counted */
    int failed_globals;
    TkLog *log;                         /* NULL for X11 runs; owned by the caller */
} TkResult;

void tk_result_init(TkResult *r, int combo);

/* The harness's stdout (all of it, at most TK_OUT_MAX bytes; `truncated` when more came). Takes the last line that
 * is a valid result object for this combination's toolkit; sets json_ok, version, backend_seen and checks, or the
 * not-installed status for {"installed":false}. Returns 0 when a result was found. */
int tk_parse_output(TkResult *r, const char *out, size_t n, int truncated);

/* Decides the status (from the exit, the log and the line) and the score. exited: 1 exit code, 2 signal, 3 killed by
 * the 8 s timeout. */
void tk_finish(TkResult *r, int how, int code);

/* The score: passed points / counted points ×100, one decimal rounded down (as tools/wl-conformance.mjs). */
void tk_score(TkResult *r);

typedef struct {
    TkResult r[TK_COMBOS];
    int n;                              /* combinations asked (in table order) */
    double global;                      /* mean over the ones that ran (RAN or FATAL), -1 if none */
    int ran;
    long long at_ms;
} TkReport;

void tk_report_global(TkReport *rep);

/* The recommended variable for a toolkit whose Wayland run is much worse than its X11 run (or the other way round),
 * e.g. "GDK_BACKEND=x11"; NULL when none. wl and x are the two results of one toolkit. */
const char *tk_recommend(const TkResult *wl, const TkResult *x);

/* docs/TOOLKIT-AUDIT.md's schema, one line */
void tk_report_json(FILE *o, const TkReport *rep);

/* The table. T(key) gives a text from toolkitaudit.json (English when missing); fill replaces {name} placeholders. */
typedef const char *(*TkText)(const char *key);
void tk_report_table(FILE *o, const TkReport *rep, TkText T);

/* --toolkits --last (for lordos-issue): the saved result (docs/TOOLKIT-AUDIT.md's JSON, at most TK_LAST_MAX bytes) as a
 * few fixed English lines. Every value printed is checked against the fixed lists first; a file that doesn't pass
 * prints one "unreadable" line and returns 1. now_ms: the clock, for the result's age. */
#define TK_LAST_MAX (256 * 1024)
int tk_last_summary(FILE *o, const char *json, size_t len, long long now_ms);

/* {name} placeholders: kv is name, value, ..., NULL */
void tk_fill(const char *tmpl, const char *const *kv, char *out, size_t n);

#endif
