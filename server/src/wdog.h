/* wdog.h — the compositor's watchdog: no bug in a theme (or in this server) may leave the desktop frozen
 * for good while the compositor's threads run at real-time priority (par.h).
 *
 * The main loop says where it is with two atomic stores per iteration and no system call: wdog_idle()
 * just before it waits in poll(), wdog_busy(now) as soon as poll() returns. A thread of its own (the
 * normal policy, never real time; started after par_init) looks every WDOG_TICK_MS. When the loop has
 * been away from its poll() for longer than the stall limit (WDOG_STALL_MS):
 *   - it logs it once, with the main thread's state from /proc (running or waiting, and the CPU time it
 *     used meanwhile) and where it is: the C stack (a signal to the main thread, backtrace() there) and,
 *     if JavaScript is running, the JS stack (from QuickJS's interrupt handler, wdog_js_poll);
 *   - it puts every thread of the compositor back to the normal policy (par_demote), for the rest of the
 *     session: RLIMIT_RTPRIO is 0 since par_init, so the kernel never lets a thread take real time
 *     again (AGENTS.md: the limit drop stays);
 *   - past WDOG_JS_STOP_MS it stops the JavaScript task that is running (QuickJS throws an uncatchable
 *     "interrupted" error; js.c logs it with its stack) and the loop goes on.
 * When the loop is back in poll() it logs how long it was away. A theme being loaded (wdog_expect(1)
 * ... wdog_expect(0)) is long by design: none of this happens meanwhile.
 *
 * Test-only overrides: LORD_WDOG_STALL_MS and LORD_WDOG_JS_MS (milliseconds), LORD_WDOG=0 (off). */
#ifndef LORD_WDOG_H
#define LORD_WDOG_H

#include <stdatomic.h>
#include <stdint.h>

#define WDOG_TICK_MS     200
#define WDOG_STALL_MS    2000
#define WDOG_JS_STOP_MS  10000

extern _Atomic uint64_t wdog_since_ns;        /* CLOCK_MONOTONIC ns when the loop left poll(), 0 while it waits */
extern atomic_int wdog_js_req;                /* for the interrupt handler: 1 the JS stack is wanted, 2 stop */

void wdog_start(void);                         /* on the main thread, right before its loop; once */
void wdog_stop(void);                          /* the loop ended (a logout): nothing more is watched */
void wdog_expect(int on);                      /* a theme load starts (1) or ends (0) */

static inline void wdog_idle(void) { atomic_store_explicit(&wdog_since_ns, 0, memory_order_relaxed); }
static inline void wdog_busy(uint64_t now_ns) { atomic_store_explicit(&wdog_since_ns, now_ns ? now_ns : 1, memory_order_relaxed); }

/* QuickJS's interrupt handler calls this (every 10000 bytecode steps): 0 go on, 1 log the JS stack (and go
 * on), 2 stop the task. Only a relaxed load while nothing is wrong. A task is stopped only once it has kept
 * JavaScript running for WDOG_JS_OWN_MS itself, so one that starts right after a stall in C isn't. */
#define WDOG_JS_OWN_MS 500
int wdog_js_slow(int req);
static inline int wdog_js_poll(void) {
    int r = atomic_load_explicit(&wdog_js_req, memory_order_relaxed);
    return r ? wdog_js_slow(r) : 0;
}

/* A crash (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) leaves a line in the log before the process dies: the
 * version and build, the signal and the faulting address, and the C stack (backtrace_symbols_fd: for a
 * stripped binary each frame reads "lord-server(+0x2c45e7)", the offset addr2line wants with the build's
 * symbols, tools/iso/mkdeb.sh DEBUG_SYMBOLS=1). The handler runs on an alternate stack of its own, so a stack
 * overflow (endless recursion) is caught too; it allocates nothing and writes with write(2). A fault is then
 * left to happen again with the default action (so the kernel still logs its "segfault at" line), an abort
 * is raised again. On the calling thread (the main one), once, as early as possible. what: "0.8.9.4 (build)" */
void wdog_crash_hooks(const char *what);

/* How many stalls this session had and how many JS tasks were stopped (tests, the system report). */
int wdog_stalls(void);
int wdog_js_stopped(void);

#endif
