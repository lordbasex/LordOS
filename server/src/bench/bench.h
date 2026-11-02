/* bench.h — measurement pieces shared by lordos-testbench (src/testbench) and LordBench (0.8.6,
 * docs/BENCHMARK.md): the clock, frame-time statistics against a refresh budget, presentation
 * accounting, input-to-screen latency, the GL renderer's kind and the reports' state file. Pure C with
 * libc and libm only: no Wayland, no GL (the GL scene is glscene.h). */
#ifndef LORD_BENCH_H
#define LORD_BENCH_H

#include <stdint.h>
#include <stdio.h>

double bench_now_ms(void);              /* CLOCK_MONOTONIC in milliseconds */

/* ---- frame times: the interval between consecutive frames, the last BENCH_HIST of them ---- */
#define BENCH_HIST 240
typedef struct {
    double iv[BENCH_HIST];
    int n;                              /* intervals stored (≤ BENCH_HIST) */
    unsigned long frames;
    double first, last;                 /* the first and last frame's time, ms */
} BenchFrames;
typedef struct { double avg, max, p50, p99; } BenchStats;

void   bench_frames_add(BenchFrames *f, double t_ms);
double bench_frames_fps(const BenchFrames *f);
void   bench_frames_stats(const BenchFrames *f, BenchStats *out);           /* all 0 without intervals */
/* intervals that missed the budget (longer than 1.5 × budget_ms), e.g. 16.67 at 60 Hz */
int    bench_frames_missed(const BenchFrames *f, double budget_ms);

/* ---- presentation: what wp_presentation (or a page flip) reports per frame ---- */
typedef struct {
    unsigned long presented, discarded, vblank_skips, zero_copy;
    uint32_t refresh_ns;
    double last_ms;
    uint64_t last_msc;
} BenchPresent;
/* a frame shown at t_ms on vblank msc (0 when the display has no counter); refresh_ns 0 if unknown */
void bench_present(BenchPresent *p, double t_ms, uint64_t msc, uint32_t refresh_ns, int zero_copy);
double bench_refresh_hz(const BenchPresent *p);

/* ---- input to screen ---- */
typedef struct { double last, sum; unsigned long n, lost; } BenchLatency;
void   bench_latency_add(BenchLatency *l, double ms);   /* < 0 or ≥ 2000 ms counts as lost */
double bench_latency_avg(const BenchLatency *l);

/* ---- the GL renderer: "HARDWARE GPU", "VIRTUAL GPU (HOST)" or "SOFTWARE (CPU)" from GL_RENDERER ---- */
const char *bench_gl_kind(const char *renderer);

/* ---- reports ---- */
void bench_json_str(FILE *o, const char *s);            /* a JSON string, quoted and escaped */
/* writes $XDG_STATE_HOME/lordos/<name>.json (or ~/.local/state/lordos/<name>.json) whole, renamed into
 * place; name is [a-z0-9-]; 0 or -1 */
int bench_save(const char *name, void (*write)(FILE *));

#endif
