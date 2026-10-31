/* gcontend.h — the plan, the numbers and the verdict of the contention experiment (build/gpu-contention,
 * tests/gpu_contention.c; docs/GPU.md, "What slows the GPU under CPU load"). No GL and no threads here, so
 * build/engine-test checks the verdict's rules. */
#ifndef LORD_GCONTEND_H
#define LORD_GCONTEND_H

#include <stddef.h>

enum { GC_NONE, GC_SPIN, GC_READ, GC_WRITE, GC_KINDS };
typedef struct { int kind, workers; } GcPhase;
typedef struct {
    GcPhase ph;
    int frames;
    double gpu_p50, gpu_p99;            /* ms from the end of the upload to the GPU's finish */
    double up_p50, up_p99;              /* ms of the texture upload (a CPU copy) */
    double bus_mb_s;                    /* the workers' last-level misses × 64 bytes, MB/s; < 0 no counter */
    int temp_mc;                        /* the first thermal zone, m°C; < 0 none */
} GcResult;

enum { GC_V_MEMORY, GC_V_CPU, GC_V_NONE, GC_V_DRIFT, GC_V_UNCLEAR };

const char *gc_kind_name(int kind);
const char *gc_verdict_name(int v);
/* Phases for a machine with this many cores: no load, then spin, read and write on 1, 2, 4… and all
 * of the cores, then no load again (to see drift). Returns how many (at most max). */
int gc_plan(GcPhase *p, int max, int cores);
/* The q-th quantile (0..1) of v, nearest rank; sorts v. 0 if n is 0. */
double gc_quantile(double *v, int n, double q);
/* GC_V_MEMORY: memory load at most cores slows the GPU (≥ 2× alone) and a load that stays in the cores'
 * caches doesn't (< 1.5×). GC_V_CPU: that load in the caches slows it as well, so the cause is not memory alone.
 * GC_V_NONE: no load reached 2×. GC_V_DRIFT: the two runs without load differ by 1.5× or more, so the run
 * proves nothing. GC_V_UNCLEAR: anything else. */
int gc_verdict(const GcResult *r, int n);
/* r as one JSON object, no newline; returns its length, 0 if it didn't fit */
size_t gc_json(const GcResult *r, char *out, size_t cap);

#endif
