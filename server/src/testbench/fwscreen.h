/* fwscreen.h — the test bench's Frameworks screen (fwscreen.c; docs/TOOLKIT-AUDIT.md, "Show mode"): the toolkit
 * audit run in show mode, drawn live. The nine combinations as a list (waiting, running, ✓ score, ✗ reason, not
 * installed), the running one highlighted, each of its 14 checks ticking ✓ or ✗ as the harness reports it, then every
 * combination's score, the global score and a details box (what failed, the recommended variable).
 *
 * Pure: the state is fed with tk_audit's events (tkrun.h) and drawn into a Surf, so the unit test runs it headless
 * with fake harnesses. testbench.c runs the audit in a thread and locks around fw_event / fw_draw. What it shows is
 * only fixed ids, numbers, the harness's checked detail text (printable ASCII, toolkits.c) and the bench's own few
 * English words: its 5x7 font is ASCII, like the rest of the bench. */
#ifndef LORD_TESTBENCH_FWSCREEN_H
#define LORD_TESTBENCH_FWSCREEN_H

#include "draw.h"
#include "tkrun.h"

typedef enum { FWC_WAIT, FWC_RUN, FWC_DONE } FwPhase;

typedef struct {
    int combo;
    FwPhase phase;
    int step;                           /* the check the harness is on (a progress "step"), -1 */
    signed char tick[TK_CHECKS];        /* -1 not yet; TK_SKIP, TK_PASS, TK_FAIL (live, then the final result's) */
    double t0, t1;                      /* started, finished (ms) */
    /* final (DONE): a copy of the result */
    TkStatus status;
    TkFatal fatal;
    int signal_no;
    double score;
    int failed_globals, bound;
    char version[32];
    char detail[TK_CHECKS][TK_DETAIL_MAX];
    char recommend[40];
} FwCombo;

enum { FW_IDLE, FW_RUNNING, FW_DONE, FW_REFUSED };

typedef struct {
    int state;
    int n;
    FwCombo c[TK_COMBOS];
    int running;                        /* the slot running, -1 */
    double global;                      /* -1: none */
    int ran;
    int saved;                          /* 1 saved to ~/.cache/lordos/toolkit-audit.json, -1 not, 0 not yet */
    int sel;                            /* the slot in the details box; -1 picks one */
    long events;                        /* how many events arrived (tests) */
} FwState;

void fw_init(FwState *s, const int *which, int n);
void fw_event(FwState *s, const TkEvent *e, double now_ms);
/* the run ended: the global score and the recommendations from the report; saved: tk_save's outcome (0 → 1, else -1) */
void fw_finish(FwState *s, const TkReport *rep, int saved);
int  fw_detail_slot(const FwState *s);

void fw_draw(Surf *out, const FwState *s, double now_ms);

/* where a click lands: a row's slot (0 ..), or FW_HIT_AGAIN, FW_HIT_BACK, FW_HIT_NONE */
#define FW_HIT_NONE (-1)
#define FW_HIT_AGAIN (-2)
#define FW_HIT_BACK (-3)
int fw_hit(const FwState *s, int w, int h, int x, int y);

#endif
