/* lbwork.h — LordBench's CPU, memory and storage workloads (docs/BENCHMARK.md, "Workloads"). Pure
 * computation on memory the caller gives or allocates, plus one fd for storage: no paths, no network.
 * Used by lord-bench (lbmain.c) and tested in tests/lordbench_test.c. Each returns a rate (higher is
 * better) or, for latency, nanoseconds; a value < 0 means it could not run (out of memory, an I/O error). */
#ifndef LORD_BENCH_LBWORK_H
#define LORD_BENCH_LBWORK_H

#include <stddef.h>

/* runs fn(arg, thread_index) on n threads at once (n ≥ 1; thread 0 is the caller) and returns the
 * wall time in ms from the moment all of them were released; -1 if a thread couldn't start */
double lb_parallel(int n, void (*fn)(void *arg, int t), void *arg);

double lb_sha256(int threads, double secs);     /* MB/s hashed, summed over the threads */
double lb_matmul(int threads, double secs);     /* GFLOP/s, a 256×256 double matrix product per thread */
/* the package engine's own xz decoder (pkg/xz.c) on a fixed compressed text (build/gen/lb_corpus.h):
 * MB/s of output, summed over the threads */
double lb_xz(int threads, double secs);
/* QuickJS running a fixed script (strings, JSON, sorting, objects, a regular expression), one runtime
 * per thread: runs per second, summed */
double lb_js(int threads, double secs);

/* memory bandwidth over arrays of mb MB each, split across the threads: GB/s */
double lb_mem_copy(int threads, size_t mb, double secs);
double lb_mem_triad(int threads, size_t mb, double secs);
double lb_mem_latency(size_t mb, long hops);    /* ns per dependent load, one thread */

/* storage on fd (a scratch file lord-server opened, read and write), mb MB at most:
 * sequential write and read in MB/s, then random 4 KiB reads for secs, in IOPS */
int lb_disk(int fd, size_t mb, double secs, double *seq_w, double *seq_r, double *rand_iops);

/* sustained all-core load: calls sample(gflops, t_s, ctx) every interval_s for secs seconds */
void lb_sustained(int threads, double secs, double interval_s, void (*sample)(double gflops, double t_s, void *ctx), void *ctx);

/* the median of v[0..n) (sorted in place) and its spread max − min */
double lb_median(double *v, int n, double *spread);

#endif
