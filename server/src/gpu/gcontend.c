/* gcontend.c — see gcontend.h */
#include "gcontend.h"

#include <stdio.h>
#include <stdlib.h>

const char *gc_kind_name(int kind) {
    static const char *const n[GC_KINDS] = { "none", "spin", "read", "write" };
    return kind >= 0 && kind < GC_KINDS ? n[kind] : "?";
}

const char *gc_verdict_name(int v) {
    static const char *const n[] = { "memory", "cpu", "none", "drift", "unclear" };
    return v >= 0 && v <= GC_V_UNCLEAR ? n[v] : "?";
}

int gc_plan(GcPhase *p, int max, int cores) {
    int n = 0;
    if (cores < 1) cores = 1;
    if (n < max) p[n++] = (GcPhase){ GC_NONE, 0 };
    for (int k = GC_SPIN; k <= GC_WRITE; k++)
        for (int w = 1;; w = w * 2 > cores && w < cores ? cores : w * 2) {
            if (w > cores) break;
            if (n < max) p[n++] = (GcPhase){ k, w };
            if (w == cores) break;
        }
    if (n < max) p[n++] = (GcPhase){ GC_NONE, 0 };
    return n;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

double gc_quantile(double *v, int n, double q) {
    if (n <= 0) return 0;
    qsort(v, (size_t)n, sizeof *v, cmp_double);
    int i = (int)(q * n + 0.999999) - 1;
    return v[i < 0 ? 0 : i >= n ? n - 1 : i];
}

int gc_verdict(const GcResult *r, int n) {
    double first = -1, last = -1, spin = -1, mem = -1;
    int top = 0;
    for (int i = 0; i < n; i++) if (r[i].ph.workers > top) top = r[i].ph.workers;
    for (int i = 0; i < n; i++) {
        double t = r[i].gpu_p50;
        if (r[i].ph.kind == GC_NONE) { if (first < 0) first = t; last = t; }
        else if (r[i].ph.workers != top) continue;
        else if (r[i].ph.kind == GC_SPIN) spin = t;
        else if (t > mem) mem = t;
    }
    if (first <= 0 || last <= 0 || spin < 0 || mem < 0) return GC_V_UNCLEAR;
    double lo = first < last ? first : last, hi = first < last ? last : first;
    if (hi >= 1.5 * lo) return GC_V_DRIFT;
    if (spin >= 2 * lo) return GC_V_CPU;
    if (mem >= 2 * lo) return spin < 1.5 * lo ? GC_V_MEMORY : GC_V_UNCLEAR;
    return spin < 1.5 * lo && mem < 1.5 * lo ? GC_V_NONE : GC_V_UNCLEAR;
}

size_t gc_json(const GcResult *r, char *out, size_t cap) {
    char bus[32] = "null", temp[32] = "null";
    if (r->bus_mb_s >= 0) snprintf(bus, sizeof bus, "%.1f", r->bus_mb_s);
    if (r->temp_mc >= 0) snprintf(temp, sizeof temp, "%d", r->temp_mc);
    int w = snprintf(out, cap, "{\"load\":\"%s\",\"workers\":%d,\"frames\":%d,\"gpuP50\":%.3f,\"gpuP99\":%.3f,\"upP50\":%.3f,\"upP99\":%.3f,\"busMBs\":%s,\"tempMilliC\":%s}",
                     gc_kind_name(r->ph.kind), r->ph.workers, r->frames, r->gpu_p50, r->gpu_p99, r->up_p50, r->up_p99, bus, temp);
    return w > 0 && (size_t)w < cap ? (size_t)w : 0;
}
