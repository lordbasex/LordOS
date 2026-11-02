/* par.c — the pixel loops' thread pool (see par.h). Workers sleep on a condition variable between
 * jobs; a job is cut into a few pieces per thread, taken with an atomic counter (the caller takes
 * pieces too), so a slow piece doesn't hold the others. Only one job at a time, from the main
 * thread; a call made inside a piece, or from another thread while a job runs, runs serially. */
#include "par.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PAR_MAX 8

static int nthreads = 1;            /* the caller included */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t go = PTHREAD_COND_INITIALIZER, done = PTHREAD_COND_INITIALIZER;
static unsigned long gen;           /* a new job (under mu) */
static ParFn job_fn;
static void *job_ctx;
static int job_j0, job_n, job_pieces;
static atomic_int next_piece, left;
static atomic_flag busy = ATOMIC_FLAG_INIT;
static _Thread_local int inside;

static void run_pieces(void) {
    inside = 1;
    for (;;) {
        int p = atomic_fetch_add(&next_piece, 1);
        if (p >= job_pieces) break;
        int a = job_j0 + (int)((long)job_n * p / job_pieces), b = job_j0 + (int)((long)job_n * (p + 1) / job_pieces);
        if (b > a) job_fn(job_ctx, a, b);
        if (atomic_fetch_sub(&left, 1) == 1) {
            pthread_mutex_lock(&mu);
            pthread_cond_signal(&done);
            pthread_mutex_unlock(&mu);
        }
    }
    inside = 0;
}

static atomic_int rt_tids[PAR_MAX];  /* the threads at real-time priority, for rt_demote */
static atomic_int rt_on;

static void *worker(void *arg) {
    if (atomic_load(&rt_on)) atomic_store(&rt_tids[(long)arg], (int)syscall(SYS_gettid));
    unsigned long seen = 0;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (gen == seen) pthread_cond_wait(&go, &mu);
        seen = gen;
        pthread_mutex_unlock(&mu);
        run_pieces();
    }
    return NULL;
}

/* SIGXCPU: a thread ran PAR_RT_TIME_US on real time without blocking (a bug, or a very long theme
 * load). Every thread goes back to the normal policy for the rest of the session; nothing is killed. */
static void rt_demote(int sig) {
    (void)sig;
    struct sched_param p = { 0 };
    for (int i = 0; i < PAR_MAX; i++) {
        int t = atomic_load(&rt_tids[i]);
        if (t) sched_setscheduler(t, SCHED_OTHER, &p);
    }
    if (atomic_exchange(&rt_on, 0)) {
        static const char m[] = "lord-server: back to the normal priority (a thread ran too long on real time, or it could not be set)\n";
        if (write(2, m, sizeof m - 1) < 0) {}
    }
}

static int rt_set(int flags) {
    struct sched_param p = { .sched_priority = PAR_RT_PRIO };
    return sched_setscheduler(0, SCHED_RR | flags, &p);
}

void par_init(int threads) {
    if (nthreads > 1) return;
    /* Real-time priority (par.h): only if lord-sessiond allowed it; the workers inherit it from this
     * thread, which then takes SCHED_RESET_ON_FORK so nothing it starts later does */
    struct rlimit rl;
    if (!getrlimit(RLIMIT_RTPRIO, &rl) && rl.rlim_cur >= PAR_RT_PRIO) {
        signal(SIGXCPU, rt_demote);
        if (!rt_set(0)) {
            atomic_store(&rt_tids[0], (int)syscall(SYS_gettid));
            atomic_store(&rt_on, 1);
        } else fprintf(stderr, "lord-server: no real-time priority (%s)\n", strerror(errno));
    }
    if (threads <= 0) {
        cpu_set_t set;
        threads = !sched_getaffinity(0, sizeof set, &set) ? CPU_COUNT(&set) : (int)sysconf(_SC_NPROCESSORS_ONLN);
    }
    if (threads > PAR_MAX) threads = PAR_MAX;
    for (int i = 1; i < threads; i++) {
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 256 * 1024);
        if (pthread_create(&t, &at, worker, (void *)(long)i)) { pthread_attr_destroy(&at); break; }
        pthread_attr_destroy(&at);
        pthread_detach(t);
        nthreads++;
    }
    if (atomic_load(&rt_on)) {
        if (rt_set(SCHED_RESET_ON_FORK)) rt_demote(0);   /* never leave a thread that forks at real time */
        else fprintf(stderr, "lord-server: compositor at real-time priority %d (SCHED_RR, %d threads)\n", PAR_RT_PRIO, nthreads);
    }
    /* the limit goes to 0 for good, hard limit included: apps, helpers and shells this server starts
     * can never ask for real time */
    rl.rlim_cur = rl.rlim_max = 0;
    setrlimit(RLIMIT_RTPRIO, &rl);
}

int par_realtime(void) { return atomic_load(&rt_on); }

int par_threads(void) { return nthreads; }

void par_rows(int j0, int j1, ParFn fn, void *ctx) {
    if (j1 <= j0) return;
    if (nthreads <= 1 || inside || j1 - j0 < 2 || atomic_flag_test_and_set(&busy)) { fn(ctx, j0, j1); return; }
    int n = j1 - j0, pieces = nthreads * 3;
    if (pieces > n) pieces = n;
    pthread_mutex_lock(&mu);
    job_fn = fn;
    job_ctx = ctx;
    job_j0 = j0;
    job_n = n;
    job_pieces = pieces;
    atomic_store(&next_piece, 0);
    atomic_store(&left, pieces);
    gen++;
    pthread_cond_broadcast(&go);
    pthread_mutex_unlock(&mu);
    run_pieces();
    pthread_mutex_lock(&mu);
    while (atomic_load(&left) > 0) pthread_cond_wait(&done, &mu);
    pthread_mutex_unlock(&mu);
    atomic_flag_clear(&busy);
}
