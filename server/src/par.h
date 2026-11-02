/* par.h — a small pool of threads for the CPU path's pixel loops (rows split between them).
 *
 * par_rows(j0, j1, fn, ctx) calls fn(ctx, a, b) over disjoint pieces of [j0, j1) on the pool and the
 * calling thread, and returns when every piece is done. fn must only write its own rows (or columns)
 * and touch no shared state: it is used for pure pixel work (blur, blits, gradients, the remote
 * desktop's comparison and copies), never for the node tree, caches or anything with side effects.
 * Without par_init (or with one CPU) it runs fn(ctx, j0, j1) on the caller. */
#ifndef LORD_PAR_H
#define LORD_PAR_H

#define PAR_RT_PRIO    2            /* the lowest real-time levels: above every normal thread, below the kernel's */
#define PAR_RT_TIME_US 5000000      /* RLIMIT_RTTIME's soft limit: 5 s of CPU without blocking */

typedef void (*ParFn)(void *ctx, int a, int b);

/* 0: the CPUs this process may use, at most 8. Called once, right after dropping root and before
 * this server starts any process or other thread: when lord-sessiond allows it (RLIMIT_RTPRIO at
 * PAR_RT_PRIO, remote/sess.c), the calling thread and the workers take SCHED_RR at PAR_RT_PRIO, so the
 * compositor runs ahead of any app (docs/BENCHMARK.md, "The desktop under load"); the calling thread
 * with SCHED_RESET_ON_FORK, so threads and processes it starts later get the normal policy. Then
 * RLIMIT_RTPRIO goes to 0 (soft and hard), always. A thread that runs PAR_RT_TIME_US without blocking
 * gets SIGXCPU (RLIMIT_RTTIME) and every thread goes back to the normal policy. */
void par_init(int threads);
int  par_realtime(void);            /* 1 while the compositor's threads run at real-time priority */
int  par_threads(void);             /* threads that take pieces, the caller included (1: serial) */
void par_rows(int j0, int j1, ParFn fn, void *ctx);

#endif
