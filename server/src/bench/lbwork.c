/* lbwork.c — see lbwork.h. */
#include "lbwork.h"
#include "bench.h"
#include "../pkg/crypto.h"
#include "../pkg/xz.h"
#include "lb_corpus.h"

#include <fcntl.h>
#include <pthread.h>
#include <quickjs.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LB_MAX_THREADS 256

/* ---- running on n threads at once ---- */

typedef struct {
    void (*fn)(void *, int);
    void *arg;
    int t;
    atomic_int *gate;               /* 0 wait, 1 go, -1 give up */
} Job;

static void *job_main(void *p) {
    Job *j = p;
    int g;
    while (!(g = atomic_load(j->gate))) sched_yield();
    if (g > 0) j->fn(j->arg, j->t);
    return NULL;
}

double lb_parallel(int n, void (*fn)(void *arg, int t), void *arg) {
    if (n < 1) n = 1;
    if (n > LB_MAX_THREADS) n = LB_MAX_THREADS;
    static pthread_t th[LB_MAX_THREADS];
    static Job jobs[LB_MAX_THREADS];
    atomic_int gate = 0;
    int started = 1;
    for (int i = 1; i < n; i++, started++) {
        jobs[i] = (Job){ fn, arg, i, &gate };
        if (pthread_create(&th[i], NULL, job_main, &jobs[i])) break;
    }
    atomic_store(&gate, started == n ? 1 : -1);
    double t0 = bench_now_ms();
    if (started == n) fn(arg, 0);
    for (int i = 1; i < started; i++) pthread_join(th[i], NULL);
    return started == n ? bench_now_ms() - t0 : -1;
}

/* a timed loop shared by the CPU workloads: every thread repeats one unit of work until secs have
 * passed and counts the units it finished */
typedef struct {
    double secs;
    void (*unit)(void *state);
    void *(*setup)(void);
    void (*teardown)(void *state);
    double units[LB_MAX_THREADS];
    double ms[LB_MAX_THREADS];
    atomic_int failed;
} Timed;

static void timed_thread(void *arg, int t) {
    Timed *tm = arg;
    void *st = tm->setup ? tm->setup() : NULL;
    if (tm->setup && !st) { atomic_store(&tm->failed, 1); return; }
    double t0 = bench_now_ms(), end = t0 + tm->secs * 1000, now = t0;
    double u = 0;
    do { tm->unit(st); u++; now = bench_now_ms(); } while (now < end);
    tm->units[t] = u;
    tm->ms[t] = now - t0;
    if (tm->teardown) tm->teardown(st);
}

/* units per second, summed over the threads; -1 if a thread failed */
static double timed_run(int threads, double secs, void *(*setup)(void), void (*unit)(void *), void (*teardown)(void *)) {
    static Timed tm;
    memset(&tm, 0, sizeof tm);
    tm.secs = secs;
    tm.setup = setup;
    tm.unit = unit;
    tm.teardown = teardown;
    if (threads > LB_MAX_THREADS) threads = LB_MAX_THREADS;
    if (lb_parallel(threads, timed_thread, &tm) < 0 || atomic_load(&tm.failed)) return -1;
    double rate = 0;
    for (int i = 0; i < threads; i++) if (tm.ms[i] > 0) rate += tm.units[i] * 1000.0 / tm.ms[i];
    return rate;
}

/* ---- SHA-256 (pkg/crypto.c, with the CPU's SHA instructions when it has them) ---- */

#define SHA_BLOCK (1 << 20)
static void *sha_setup(void) {
    uint8_t *b = malloc(SHA_BLOCK);
    if (b) for (size_t i = 0; i < SHA_BLOCK; i++) b[i] = (uint8_t)(i * 2654435761u >> 13);
    return b;
}
static void sha_unit(void *st) {
    static _Thread_local uint8_t out[32];
    sha256(st, SHA_BLOCK, out);
    ((uint8_t *)st)[0] ^= out[0];                   /* depend on the result */
}

double lb_sha256(int threads, double secs) {
    sha256_select(1);
    double r = timed_run(threads, secs, sha_setup, sha_unit, free);
    return r < 0 ? r : r * SHA_BLOCK / 1e6;
}

/* ---- matrix multiply ---- */

#define MAT_N 256
typedef struct { double a[MAT_N * MAT_N], b[MAT_N * MAT_N], c[MAT_N * MAT_N]; } Mat;
static void *mat_setup(void) {
    Mat *m = aligned_alloc(64, sizeof(Mat));
    if (!m) return NULL;
    for (int i = 0; i < MAT_N * MAT_N; i++) { m->a[i] = (i % 17) * 0.25; m->b[i] = (i % 13) * 0.5; }
    return m;
}
static void mat_unit(void *st) {
    Mat *m = st;
    memset(m->c, 0, sizeof m->c);
    for (int i = 0; i < MAT_N; i++)
        for (int k = 0; k < MAT_N; k++) {
            double aik = m->a[i * MAT_N + k];
            const double *restrict b = &m->b[k * MAT_N];
            double *restrict c = &m->c[i * MAT_N];
            for (int j = 0; j < MAT_N; j++) c[j] += aik * b[j];
        }
    m->a[0] = m->c[MAT_N * MAT_N - 1] * 1e-12;      /* depend on the result */
}
#define MAT_FLOP (2.0 * MAT_N * MAT_N * MAT_N)

double lb_matmul(int threads, double secs) {
    double r = timed_run(threads, secs, mat_setup, mat_unit, free);
    return r < 0 ? r : r * MAT_FLOP / 1e9;
}

/* ---- xz (the package engine's decoder) ---- */

static void *xz_setup(void) { return (void *)1; }
static _Thread_local size_t xz_out;
static _Thread_local int xz_bad;
static void xz_unit(void *st) {
    (void)st;
    uint8_t *out = NULL;
    size_t n = 0;
    if (xz_decode(lb_corpus_xz, sizeof lb_corpus_xz, &out, &n, LB_CORPUS_SIZE + 1) || n != LB_CORPUS_SIZE) xz_bad = 1;
    free(out);
    xz_out = n;
}
static void xz_none(void *st) { (void)st; }

double lb_xz(int threads, double secs) {
    double r = timed_run(threads, secs, xz_setup, xz_unit, xz_none);
    return r < 0 ? r : r * LB_CORPUS_SIZE / 1e6;
}

/* ---- QuickJS ---- */

static const char js_script[] =
    "function work(){"
    " var s=[]; for(var i=0;i<2000;i++) s.push('item-'+i+'-'+(i*7919%1000));"
    " var o={list:s.map(function(x,i){return {id:i,name:x,tags:[x.length,i%7]};})};"
    " var j=JSON.stringify(o); var p=JSON.parse(j);"
    " p.list.sort(function(a,b){return a.name<b.name?-1:a.name>b.name?1:0;});"
    " var re=/item-(\\d+)-(\\d+)/, n=0;"
    " for(var k=0;k<p.list.length;k++){var m=re.exec(p.list[k].name); if(m) n+= +m[2];}"
    " var mp=new Map(); for(var k=0;k<p.list.length;k++) mp.set(p.list[k].name,k);"
    " return n + j.length + mp.size;"
    "}";

typedef struct { JSRuntime *rt; JSContext *c; JSValue fn; } JsState;
static void *js_setup(void) {
    JsState *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->rt = JS_NewRuntime();
    if (!s->rt) { free(s); return NULL; }
    JS_SetMemoryLimit(s->rt, 64 << 20);
    s->c = JS_NewContext(s->rt);
    if (!s->c) { JS_FreeRuntime(s->rt); free(s); return NULL; }
    JSValue r = JS_Eval(s->c, js_script, sizeof js_script - 1, "<lordbench>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(s->c, r);
    JSValue g = JS_GetGlobalObject(s->c);
    s->fn = JS_GetPropertyStr(s->c, g, "work");
    JS_FreeValue(s->c, g);
    if (!JS_IsFunction(s->c, s->fn)) { JS_FreeValue(s->c, s->fn); JS_FreeContext(s->c); JS_FreeRuntime(s->rt); free(s); return NULL; }
    return s;
}
static void js_unit(void *st) {
    JsState *s = st;
    JSValue r = JS_Call(s->c, s->fn, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(s->c, r);
}
static void js_teardown(void *st) {
    JsState *s = st;
    JS_FreeValue(s->c, s->fn);
    JS_FreeContext(s->c);
    JS_FreeRuntime(s->rt);
    free(s);
}

double lb_js(int threads, double secs) { return timed_run(threads, secs, js_setup, js_unit, js_teardown); }

/* ---- memory bandwidth ---- */

typedef struct {
    double *a, *b, *c;
    size_t n;                       /* doubles per array */
    int threads, triad;
    double secs;
    double bytes[LB_MAX_THREADS];
} Bw;

static void bw_thread(void *arg, int t) {
    Bw *w = arg;
    size_t per = w->n / (size_t)w->threads, lo = per * (size_t)t, hi = t == w->threads - 1 ? w->n : lo + per;
    double *a = w->a, *b = w->b, *c = w->c, s = 3.0;
    for (size_t i = lo; i < hi; i++) { a[i] = 1.0; b[i] = 2.0; c[i] = 0.0; }   /* first touch on this thread */
    double end = bench_now_ms() + w->secs * 1000, bytes = 0;
    do {
        if (w->triad) { for (size_t i = lo; i < hi; i++) a[i] = b[i] + s * c[i]; bytes += 3.0 * (double)(hi - lo) * sizeof(double); }
        else          { for (size_t i = lo; i < hi; i++) c[i] = a[i]; bytes += 2.0 * (double)(hi - lo) * sizeof(double); }
    } while (bench_now_ms() < end);
    w->bytes[t] = bytes;
}

static double bandwidth(int threads, size_t mb, double secs, int triad) {
    static Bw w;
    memset(&w, 0, sizeof w);
    if (threads > LB_MAX_THREADS) threads = LB_MAX_THREADS;
    w.n = mb * (1 << 20) / sizeof(double);
    w.a = aligned_alloc(64, w.n * sizeof(double));
    w.b = aligned_alloc(64, w.n * sizeof(double));
    w.c = aligned_alloc(64, w.n * sizeof(double));
    double r = -1;
    if (w.a && w.b && w.c) {
        w.threads = threads;
        w.triad = triad;
        w.secs = secs;
        double ms = lb_parallel(threads, bw_thread, &w);
        double bytes = 0;
        for (int i = 0; i < threads; i++) bytes += w.bytes[i];
        /* the first pass touched the pages: it's part of ms, a little pessimistic on purpose */
        if (ms > 0) r = bytes / 1e9 / (ms / 1000);
    }
    free(w.a);
    free(w.b);
    free(w.c);
    return r;
}

double lb_mem_copy(int threads, size_t mb, double secs) { return bandwidth(threads, mb, secs, 0); }
double lb_mem_triad(int threads, size_t mb, double secs) { return bandwidth(threads, mb, secs, 1); }

static uint64_t xs(uint64_t *s) { uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x; }

double lb_mem_latency(size_t mb, long hops) {
    size_t n = mb * (1 << 20) / 64;                 /* one node per cache line */
    if (n < 2) return -1;
    size_t *buf = aligned_alloc(64, n * 64);
    size_t *perm = malloc(n * sizeof *perm);
    if (!buf || !perm) { free(buf); free(perm); return -1; }
    for (size_t i = 0; i < n; i++) perm[i] = i;
    uint64_t seed = 0x9e3779b97f4a7c15ull;
    for (size_t i = n - 1; i > 0; i--) {            /* Sattolo: one cycle through every node */
        size_t j = (size_t)(xs(&seed) % i);
        size_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    for (size_t i = 0; i < n; i++) buf[perm[i] * 8] = perm[(i + 1) % n] * 8;
    free(perm);
    volatile size_t sink;
    size_t p = 0;
    for (long i = 0; i < hops / 10; i++) p = buf[p];               /* warm the TLB a little */
    double t0 = bench_now_ms();
    for (long i = 0; i < hops; i++) p = buf[p];
    double ms = bench_now_ms() - t0;
    sink = p;
    (void)sink;
    free(buf);
    return ms * 1e6 / (double)hops;
}

/* ---- storage ---- */

int lb_disk(int fd, size_t mb, double secs, double *seq_w, double *seq_r, double *rand_iops) {
    *seq_w = *seq_r = *rand_iops = -1;
    const size_t blk = 1 << 20;
    uint8_t *b = aligned_alloc(4096, blk);
    if (!b || mb < 8) { free(b); return -1; }
    uint64_t seed = 0x1234567ull;
    for (size_t i = 0; i < blk / 8; i++) ((uint64_t *)b)[i] = xs(&seed);   /* doesn't compress */
    int rc = -1;
    if (ftruncate(fd, 0)) goto out;
    double t0 = bench_now_ms();
    for (size_t i = 0; i < mb; i++) {
        b[0] = (uint8_t)i;
        if (pwrite(fd, b, blk, (off_t)(i * blk)) != (ssize_t)blk) goto out;
    }
    if (fdatasync(fd)) goto out;
    double ms = bench_now_ms() - t0;
    *seq_w = mb / (ms / 1000);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);  /* read from the device, not the page cache */
    t0 = bench_now_ms();
    for (size_t i = 0; i < mb; i++) if (pread(fd, b, blk, (off_t)(i * blk)) != (ssize_t)blk) goto out;
    ms = bench_now_ms() - t0;
    *seq_r = mb / (ms / 1000);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    size_t pages = mb * (blk / 4096);
    double end = bench_now_ms() + secs * 1000, n = 0;
    t0 = bench_now_ms();
    do {
        size_t pg = (size_t)(xs(&seed) % pages);
        if (pread(fd, b, 4096, (off_t)(pg * 4096)) != 4096) goto out;
        n++;
    } while (bench_now_ms() < end);
    ms = bench_now_ms() - t0;
    *rand_iops = n / (ms / 1000);
    rc = 0;
out:
    if (ftruncate(fd, 0)) rc = -1;                   /* give the space back */
    free(b);
    return rc;
}

/* ---- sustained load ---- */

typedef struct { atomic_int stop; atomic_long done; } Sus;
static void sus_thread(void *arg, int t) {
    (void)t;
    Sus *s = arg;
    Mat *m = mat_setup();
    if (!m) return;
    while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) { mat_unit(m); atomic_fetch_add_explicit(&s->done, 1, memory_order_relaxed); }
    free(m);
}
typedef struct { Sus *s; int n; } SusArgs;
static void *sus_workers(void *p) {
    SusArgs *a = p;
    lb_parallel(a->n, sus_thread, a->s);
    return NULL;
}

void lb_sustained(int threads, double secs, double interval_s, void (*sample)(double, double, void *), void *ctx) {
    static Sus s;
    atomic_store(&s.stop, 0);
    atomic_store(&s.done, 0);
    SusArgs a = { &s, threads };
    pthread_t w;
    if (pthread_create(&w, NULL, sus_workers, &a)) return;
    double t0 = bench_now_ms(), last_t = t0;
    long last = 0;
    while ((bench_now_ms() - t0) < secs * 1000) {
        struct timespec ts = { (time_t)interval_s, (long)((interval_s - (time_t)interval_s) * 1e9) };
        nanosleep(&ts, NULL);
        double now = bench_now_ms();
        long d = atomic_load(&s.done);
        sample((double)(d - last) * MAT_FLOP / 1e9 / ((now - last_t) / 1000), (now - t0) / 1000, ctx);
        last = d;
        last_t = now;
    }
    atomic_store(&s.stop, 1);
    pthread_join(w, NULL);
}

/* ---- statistics ---- */

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

double lb_median(double *v, int n, double *spread) {
    if (n <= 0) { if (spread) *spread = 0; return -1; }
    qsort(v, (size_t)n, sizeof *v, cmp_d);
    if (spread) *spread = v[n - 1] - v[0];
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}
