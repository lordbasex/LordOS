/* wdog_test.c — the compositor's watchdog (src/wdog.c): a loop that stalls is seen, logged (C stack, and
 * the JS stack when JavaScript runs), its threads go back to the normal policy, a JavaScript task that
 * never ends is stopped and the loop goes on; a loop that only waits, or a theme load, is never touched.
 * The priority part runs only where real time can be had (root, or `ulimit -r` of 2 or more); elsewhere
 * it says "skipped". Thresholds are shortened with LORD_WDOG_STALL_MS and LORD_WDOG_JS_MS. */
#include "../src/wdog.h"
#include "../src/par.h"
#include "quickjs.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { fails++; printf("FAIL " __VA_ARGS__); } printf("\n"); } while (0)

static uint64_t mono(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void spin_ms(int ms) { uint64_t end = mono() + (uint64_t)ms * 1000000ull; while (mono() < end) {} }
static void sleep_ms(int ms) { struct timespec d = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&d, NULL); }
/* one iteration of a main loop: out of poll() for `busy` ms (spinning), then back in it for a while */
static void iteration(int busy) { wdog_busy(mono()); spin_ms(busy); wdog_idle(); sleep_ms(450); }

static int on_interrupt(JSRuntime *r, void *o) { (void)r; (void)o; return wdog_js_poll() == 2; }

int main(void) {
    setenv("LORD_WDOG_STALL_MS", "400", 1);
    setenv("LORD_WDOG_JS_MS", "1200", 1);
    struct rlimit rl;
    int can_rt = !getrlimit(RLIMIT_RTPRIO, &rl) && rl.rlim_cur >= PAR_RT_PRIO;
    par_init(2);                                   /* real time if this process may have it, as lord-server */
    int rt = par_realtime();
    wdog_start();

    /* a loop that is quick, or that waits long in poll(), is never a stall */
    for (int i = 0; i < 3; i++) iteration(50);
    wdog_idle();
    sleep_ms(900);
    CHECK(wdog_stalls() == 0, "quick iterations and a long wait in poll(): no stall");
    CHECK(par_realtime() == rt, "the priority is untouched while nothing stalls");

    /* a theme load is long by design: never a stall, and the time after it counts afresh */
    wdog_busy(mono());
    wdog_expect(1);
    spin_ms(900);
    wdog_expect(0);
    spin_ms(150);
    wdog_idle();
    sleep_ms(450);
    CHECK(wdog_stalls() == 0, "a theme load of 0.9 s isn't a stall");

    /* a stall in C: seen once, then the loop is back */
    iteration(800);
    CHECK(wdog_stalls() == 1, "a loop away from poll() for 0.8 s is one stall (%d)", wdog_stalls());
    if (rt) {
        CHECK(!par_realtime() && (sched_getscheduler(0) & ~SCHED_RESET_ON_FORK) == SCHED_OTHER, "its threads are back at the normal policy (policy %#x)", (unsigned)sched_getscheduler(0));
    } else {
        printf("skipped the priority drop: needs real-time priority (root or `ulimit -r` %d; %s)\n", PAR_RT_PRIO, can_rt ? "refused" : "not allowed here");
    }
    iteration(50);
    CHECK(wdog_stalls() == 1, "back to normal: no new stall");

    /* JavaScript that never returns: stopped past LORD_WDOG_JS_MS, with an error no try/catch can keep */
    JSRuntime *jrt = JS_NewRuntime();
    JS_SetInterruptHandler(jrt, on_interrupt, NULL);
    JSContext *ctx = JS_NewContext(jrt);
    static const char loop[] = "let n = 0; for (;;) { try { for (;;) n++; } catch (e) { n = -1; } }";
    uint64_t t0 = mono();
    wdog_busy(t0);
    JSValue r = JS_Eval(ctx, loop, sizeof loop - 1, "<spin>", JS_EVAL_TYPE_GLOBAL);
    double took = (mono() - t0) / 1e9;
    wdog_idle();
    int stopped = JS_IsException(r);
    const char *msg = NULL;
    if (stopped) { JSValue e = JS_GetException(ctx); msg = JS_ToCString(ctx, e); JS_FreeValue(ctx, e); }
    CHECK(stopped && msg && strstr(msg, "interrupted"), "an endless JS loop (with a try/catch) is stopped: %s", msg ? msg : "-");
    CHECK(took > 1.0 && took < 4.0, "stopped after %.1f s (limit 1.2 s, then 0.5 s of its own)", took);
    CHECK(wdog_js_stopped() == 1, "one JS task stopped");
    if (msg) JS_FreeCString(ctx, msg);
    else JS_FreeValue(ctx, r);
    sleep_ms(450);

    /* after that, JavaScript runs normally again */
    static const char ok[] = "let s = 0; for (let i = 0; i < 1e6; i++) s += i; s";
    wdog_busy(mono());
    JSValue v = JS_Eval(ctx, ok, sizeof ok - 1, "<ok>", JS_EVAL_TYPE_GLOBAL);
    wdog_idle();
    CHECK(!JS_IsException(v), "the next JS task runs to its end");
    JS_FreeValue(ctx, v);
    JS_FreeContext(ctx);
    JS_FreeRuntime(jrt);
    wdog_stop();
    printf("%s\n", fails ? "wdog: FAILED" : "wdog: all ok");
    return fails ? 1 : 0;
}
