/* wdog.c — the compositor's watchdog (see wdog.h). */
#include "wdog.h"
#include "par.h"

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

_Atomic uint64_t wdog_since_ns;
atomic_int wdog_js_req;

static atomic_int on, expecting, stalls, js_stopped;
static pid_t loop_tid;
static int where_sig;
static uint64_t stall_ns = (uint64_t)WDOG_STALL_MS * 1000000ull, js_ns = (uint64_t)WDOG_JS_STOP_MS * 1000000ull;

static uint64_t mono(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* The log goes out with write(2), never stdio: the main thread may be stuck holding stderr's lock. */
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof b) n = (int)sizeof b - 1;
    if (write(2, b, (size_t)n) < 0) {}
}

/* On the main thread, from the watchdog's signal: where it is in C. backtrace() was called once in
 * wdog_start, so its library is loaded and nothing here allocates. */
static void on_where(int sig) {
    (void)sig;
    static const char h[] = "lord-server: watchdog: the main thread's C stack:\n";
    void *f[40];
    int n = backtrace(f, 40);
    if (write(2, h, sizeof h - 1) < 0) {}
    backtrace_symbols_fd(f, n, 2);
}

/* ---- crash report (wdog.h, wdog_crash_hooks) ---- */

static char crash_what[96];

/* async-signal-safe: no stdio, no allocation */
static size_t put_str(char *b, size_t n, size_t o, const char *s) {
    while (*s && o + 1 < n) b[o++] = *s++;
    return o;
}
static size_t put_num(char *b, size_t n, size_t o, uintptr_t v, unsigned base) {
    char t[24];
    int k = 0;
    do { t[k++] = "0123456789abcdef"[v % base]; v /= base; } while (v && k < (int)sizeof t);
    if (base == 16) o = put_str(b, n, o, "0x");
    while (k && o + 1 < n) b[o++] = t[--k];
    return o;
}

static void on_crash(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    int saved = errno;
    const char *name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL"
                     : sig == SIGFPE ? "SIGFPE" : sig == SIGABRT ? "SIGABRT" : "signal";
    char b[320];
    size_t o = put_str(b, sizeof b, 0, "lord-server: crashed: ");
    o = put_str(b, sizeof b, o, name);
    o = put_str(b, sizeof b, o, " (code ");
    o = put_num(b, sizeof b, o, (uintptr_t)(si ? si->si_code : 0), 10);
    if (si && sig != SIGABRT) { o = put_str(b, sizeof b, o, ", address "); o = put_num(b, sizeof b, o, (uintptr_t)si->si_addr, 16); }
    o = put_str(b, sizeof b, o, "), version ");
    o = put_str(b, sizeof b, o, crash_what[0] ? crash_what : "?");
    o = put_str(b, sizeof b, o, "; the C stack (innermost first):\n");
    if (write(2, b, o) < 0) {}
    void *f[64];
    int n = backtrace(f, 64);               /* loaded in wdog_crash_hooks: nothing is allocated here */
    backtrace_symbols_fd(f, n, 2);
    static const char end[] = "lord-server: end of the crash report\n";
    if (write(2, end, sizeof end - 1) < 0) {}
    errno = saved;
    /* SA_RESETHAND put the default action back: a fault the kernel raised happens again when this returns
     * (and the kernel logs it); anything else (abort, a kill) is raised again */
    if (si && si->si_code > 0 && sig != SIGABRT) return;
    raise(sig);
}

void wdog_crash_hooks(const char *what) {
    snprintf(crash_what, sizeof crash_what, "%s", what ? what : "");
    static char alt[64 * 1024];             /* SIGSTKSZ is too small for the unwinder */
    stack_t st = { .ss_sp = alt, .ss_size = sizeof alt };
    if (sigaltstack(&st, NULL)) return;
    void *f[2];
    backtrace(f, 2);                        /* loads libgcc's unwinder now, not in the signal handler */
    struct sigaction sa = { .sa_sigaction = on_crash, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND };
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
}

/* The main thread's state ('R' running, 'S' or 'D' waiting), CPU time (ns) and what it waits in. */
static int thread_state(char *st, uint64_t *cpu, char *wchan, size_t wn) {
    char p[64], b[1024];
    *st = '?';
    *cpu = 0;
    wchan[0] = 0;
    snprintf(p, sizeof p, "/proc/self/task/%d/wchan", (int)loop_tid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, wchan, wn - 1);
        wchan[n > 0 ? n : 0] = 0;
        close(fd);
    }
    snprintf(p, sizeof p, "/proc/self/task/%d/stat", (int)loop_tid);
    fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    char *s = strrchr(b, ')');
    if (!s || s[1] != ' ') return -1;
    s += 2;
    *st = *s;
    unsigned long long ut = 0, stt = 0;
    /* after the state: ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt utime stime */
    if (sscanf(s, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &stt) != 2) return -1;
    long hz = sysconf(_SC_CLK_TCK);
    *cpu = (ut + stt) * (1000000000ull / (uint64_t)(hz > 0 ? hz : 100));
    return 0;
}

static void report(uint64_t away) {
    char st, w[64];
    uint64_t c0, c1;
    thread_state(&st, &c0, w, sizeof w);
    struct timespec d = { 0, 100 * 1000000L };
    nanosleep(&d, NULL);
    thread_state(&st, &c1, w, sizeof w);
    say("lord-server: watchdog: the main loop has not been back in poll() for %.1f s; its thread is %s (%.0f%% of a CPU now%s%s)\n",
        away / 1e9, st == 'R' ? "running" : st == 'D' ? "waiting (uninterruptible)" : st == 'S' ? "waiting" : "in an unknown state",
        (c1 - c0) / 1e6, w[0] && strcmp(w, "0") ? ", waiting in " : "", w[0] && strcmp(w, "0") ? w : "");
}

static void *watch(void *arg) {
    (void)arg;
    uint64_t seen = 0, last = 0;
    int stage = 0;
    for (;;) {
        struct timespec d = { 0, WDOG_TICK_MS * 1000000L };
        nanosleep(&d, NULL);
        if (!atomic_load(&on)) return NULL;
        uint64_t since = atomic_load_explicit(&wdog_since_ns, memory_order_relaxed), now = mono();
        if (stage && since != seen) {             /* back in poll() (or already in a later iteration) */
            atomic_store(&wdog_js_req, 0);
            say("lord-server: watchdog: the main loop is back after about %.1f s\n", (last - seen) / 1e9 + WDOG_TICK_MS / 1e3);
            stage = 0;
        }
        last = now;
        if (!since || atomic_load(&expecting) || now < since) continue;
        uint64_t away = now - since;
        if (!stage && away > stall_ns) {
            stage = 1;
            seen = since;
            atomic_fetch_add(&stalls, 1);
            /* first what matters (the rest of the system keeps the CPUs), then what explains it */
            int was_rt = par_demote();
            report(away);
            if (was_rt) say("lord-server: watchdog: the compositor's threads are back at the normal priority for the rest of the session\n");
            if (where_sig) syscall(SYS_tgkill, getpid(), loop_tid, where_sig);
            atomic_store(&wdog_js_req, 1);       /* the JS stack, if JavaScript is what runs */
        } else if (stage == 1 && away > js_ns) {
            stage = 2;
            say("lord-server: watchdog: %.1f s without coming back: the JavaScript task that runs now is stopped\n", away / 1e9);
            atomic_store(&wdog_js_req, 2);
        }
    }
}

static uint64_t env_ms(const char *name, uint64_t def) {
    const char *v = getenv(name);
    long ms = v && *v ? atol(v) : 0;
    return ms > 0 ? (uint64_t)ms * 1000000ull : def;
}

void wdog_start(void) {
    if (atomic_load(&on)) return;
    const char *off = getenv("LORD_WDOG");
    if (off && !strcmp(off, "0")) return;
    stall_ns = env_ms("LORD_WDOG_STALL_MS", stall_ns);
    js_ns = env_ms("LORD_WDOG_JS_MS", js_ns);
    if (js_ns < stall_ns) js_ns = stall_ns;
    loop_tid = (pid_t)syscall(SYS_gettid);
    void *f[2];
    backtrace(f, 2);                              /* loads libgcc's unwinder now, not in the signal handler */
    where_sig = SIGRTMIN + 4;
    struct sigaction sa = { .sa_handler = on_where, .sa_flags = SA_RESTART };
    sigemptyset(&sa.sa_mask);
    if (sigaction(where_sig, &sa, NULL)) where_sig = 0;
    atomic_store(&on, 1);
    /* the normal policy, said explicitly (the main thread's SCHED_RESET_ON_FORK already gives it): never
     * real time (AGENTS.md); a small stack, it only sleeps, reads /proc and writes a few lines */
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, SCHED_OTHER);
    struct sched_param sp = { 0 };
    pthread_attr_setschedparam(&at, &sp);
    pthread_t t;
    if (pthread_create(&t, &at, watch, NULL)) atomic_store(&on, 0);
    else pthread_detach(t);
    pthread_attr_destroy(&at);
}

void wdog_stop(void) {
    atomic_store(&on, 0);
    wdog_idle();
    atomic_store(&wdog_js_req, 0);
}

void wdog_expect(int yes) {
    if (yes) { atomic_fetch_add(&expecting, 1); return; }
    /* the load counts as a fresh start of this iteration: only what comes after it can be a stall */
    if (atomic_load_explicit(&wdog_since_ns, memory_order_relaxed)) wdog_busy(mono());
    atomic_fetch_sub(&expecting, 1);
}

int wdog_js_slow(int req) {
    static uint64_t first;                       /* main thread only */
    if (req == 1) {
        int one = 1;
        return atomic_compare_exchange_strong(&wdog_js_req, &one, 0) ? 1 : 0;
    }
    uint64_t now = mono();
    if (!first || now - first > 4000000000ull) first = now;     /* a new request (4 s: from an earlier stall) */
    if (now - first < (uint64_t)WDOG_JS_OWN_MS * 1000000ull) return 0;
    first = 0;
    atomic_fetch_add(&js_stopped, 1);
    return 2;
}

int wdog_stalls(void) { return atomic_load(&stalls); }
int wdog_js_stopped(void) { return atomic_load(&js_stopped); }
