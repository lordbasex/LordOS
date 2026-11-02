/* lbinput.c — see lbinput.h. One producer (the thread), one consumer (the main loop): a lock-free ring. */
#include "lbinput.h"
#include "lbframes.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define RING 256                            /* 2 s of events; a main loop that far behind drops the oldest */

typedef struct { uint64_t ns; float dx, dy; } Ev;

static Ev ring[RING];
static atomic_uint head, tail;              /* head: next to write (thread); tail: next to read (main loop) */
static atomic_int stop_req;
static int efd = -1, running;
static pthread_t th;
static float p_rx, p_ry, p_period;

static uint64_t mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

void lbi_path(float rx, float ry, float period_ms, double t_ms, float *dx, float *dy) {
    double k = 2 * M_PI * t_ms / period_ms;
    *dx = (float)(rx * sin(k));
    *dy = (float)(ry * sin(2 * k) / 2);
}

static void *producer(void *arg) {
    (void)arg;
    uint64_t t0 = mono_ns(), period = 1000000000ull / LBI_HZ;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!atomic_load(&stop_req)) {
        next.tv_nsec += (long)period;
        if (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) {}
        if (atomic_load(&stop_req)) break;
        uint64_t now = mono_ns();
        unsigned h = atomic_load_explicit(&head, memory_order_relaxed);
        if (h - atomic_load_explicit(&tail, memory_order_acquire) >= RING) continue;   /* full: this one is lost */
        Ev *e = &ring[h % RING];
        e->ns = now;                        /* stamped when it exists, as the kernel stamps a device's event */
        lbi_path(p_rx, p_ry, p_period, (double)(now - t0) / 1e6, &e->dx, &e->dy);
        atomic_store_explicit(&head, h + 1, memory_order_release);
        uint64_t one = 1;
        if (write(efd, &one, sizeof one) < 0) {}
    }
    return NULL;
}

int lbi_start(float rx, float ry, float period_ms) {
    lbi_stop();
    if (!(rx >= 0 && ry >= 0 && period_ms >= 500)) return -1;
    efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) return -1;
    p_rx = rx;
    p_ry = ry;
    p_period = period_ms;
    atomic_store(&head, 0);
    atomic_store(&tail, 0);
    atomic_store(&stop_req, 0);
    if (pthread_create(&th, NULL, producer, NULL)) { close(efd); efd = -1; return -1; }
    running = 1;
    return 0;
}

void lbi_stop(void) {
    if (!running) return;
    atomic_store(&stop_req, 1);
    pthread_join(th, NULL);                 /* it sleeps at most one period (8 ms) */
    running = 0;
    close(efd);
    efd = -1;
}

int lbi_fd(void) { return running ? efd : -1; }

void lbi_drain(void) {
    uint64_t n;
    if (efd >= 0 && read(efd, &n, sizeof n) < 0) {}
}

int lbi_take(float *dx, float *dy) {
    if (!running) return 0;
    unsigned t = atomic_load_explicit(&tail, memory_order_relaxed), h = atomic_load_explicit(&head, memory_order_acquire);
    if (t == h) return 0;
    for (; t != h; t++) {
        const Ev *e = &ring[t % RING];
        lbf_input(e->ns);
        *dx = e->dx;
        *dy = e->dy;
    }
    atomic_store_explicit(&tail, t, memory_order_release);
    return 1;
}
