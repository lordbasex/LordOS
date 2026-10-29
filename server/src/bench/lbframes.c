/* lbframes.c — see lbframes.h. */
#include "lbframes.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXF 8192                       /* about two minutes at 60 Hz; later frames are not kept */
#define MAXI 16384                      /* two minutes of the synthetic pointer's events */
#define BATCH 64                        /* events waiting for one frame (8 ms each: half a second) */

static atomic_int on;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static double work[MAXF];
static uint64_t shown[MAXF];
static int nwork, nshown;
static uint32_t period;
static double lat[MAXI];                /* ms */
static int nlat;
/* events applied, not yet in a frame; in a frame painted ahead but not submitted (the frame pipeline); in the
 * frame being shown */
static uint64_t applied[BATCH], painted[BATCH], inframe[BATCH];
static int napplied, npainted, ninframe;

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

/* the p-th percentile of sorted v[0..n) (nearest rank) */
static double pct(const double *v, int n, double p) {
    if (n <= 0) return 0;
    int k = (int)(p / 100.0 * n + 0.999999) - 1;
    return v[k < 0 ? 0 : k >= n ? n - 1 : k];
}

/* ---- frame phases (lbframes.h) ---- */

enum { PH_WAKE, PH_PRE, PH_PAINT, PH_UP, PH_G0UP, PH_SWAP, PH_SUBC, PH_BUILD, PH_GPU, PH_WAIT, PH_CHAIN, PH_FEED, PH_HELD, PH_N };
typedef struct {
    atomic_int on;
    float v[PH_N][MAXF];                /* ms */
    int nv[PH_N];
    int n, on_time, cpu_late, gpu_late, other_late, gpu_known;
    int built, par, g0_tried, g0_taken, g0_refused[LBF_G0R], g0_layer[LBF_G0L], g0_full, unshown, piped;
    double up_px, g0_px, feed_px, g0_full_px;
} Rec;
static Rec rec_lb, rec_drag;            /* LordBench's recording, the scripted drag's */
static LbPhase pend;                    /* the main display's frame waiting for its flip */
static int have_pend;

static void rec_clear(Rec *r) {        /* everything but on and the values (nv says how many count) */
    memset(r->nv, 0, sizeof r->nv);
    r->n = r->on_time = r->cpu_late = r->gpu_late = r->other_late = r->gpu_known = 0;
    r->built = r->par = r->g0_tried = r->g0_taken = r->g0_full = r->unshown = r->piped = 0;
    memset(r->g0_refused, 0, sizeof r->g0_refused);
    memset(r->g0_layer, 0, sizeof r->g0_layer);
    r->up_px = r->g0_px = r->feed_px = r->g0_full_px = 0;
}
static void rec_put(Rec *r, int ph, double ms) {
    if (ms >= 0 && r->nv[ph] < MAXF) r->v[ph][r->nv[ph]++] = (float)ms;
}
static int rec_on(Rec *r) { return atomic_load_explicit(&r->on, memory_order_relaxed); }
int lbf_recording(void) { return rec_on(&rec_lb) || rec_on(&rec_drag); }

static void rec_built(Rec *r, const LbPhase *p) {
    if (!rec_on(r)) return;
    r->built++;
    r->par += p->par != 0;
    r->unshown += p->unshown != 0;
    r->piped += p->piped != 0;
    if (p->g0 >= 0) {
        r->g0_tried++;
        if (p->g0 > 0) r->g0_taken++;
        else if (p->g0_why >= 0 && p->g0_why < LBF_G0R) {
            r->g0_refused[p->g0_why]++;
            if (p->g0_sub >= 0 && p->g0_sub < LBF_G0L) r->g0_layer[p->g0_sub]++;
        }
    }
    if (p->g0_full) { r->g0_full++; r->g0_full_px += (double)p->g0_px; }
    r->up_px += (double)p->up_px;
    r->g0_px += (double)p->g0_px;
    r->feed_px += (double)p->feed_px;
    rec_put(r, PH_FEED, p->feed_ms);
}

void lbf_phase_built(const LbPhase *p) {
    if (!lbf_recording()) return;
    pthread_mutex_lock(&mu);
    rec_built(&rec_lb, p);
    rec_built(&rec_drag, p);
    pend = *p;
    have_pend = p->submit != 0;         /* no flip queued: no presentation will complete it */
    pthread_mutex_unlock(&mu);
}

static double ms_between(uint64_t a, uint64_t b) { return a && b && b >= a ? (double)(b - a) / 1e6 : -1; }

static void rec_shown(Rec *r, const LbPhase *p, uint64_t flip, uint64_t gpu) {
    if (!rec_on(r)) return;
    r->n++;
    /* a piped frame: the flip that let it be submitted woke lord-server (swake); its CPU time is its prepare
     * and its submission, and between them it was held for that flip */
    uint64_t start = p->piped ? p->swake : p->wake;
    if (p->piped ? p->sfrom_flip : p->from_flip) rec_put(r, PH_WAKE, ms_between(p->prev_flip, start));
    rec_put(r, PH_PRE, ms_between(p->wake, p->piped ? p->pre_end : p->render0));
    rec_put(r, PH_PAINT, p->paint_ms);
    rec_put(r, PH_UP, p->up_ms);
    if (p->g0 > 0) rec_put(r, PH_G0UP, p->g0_ms);
    rec_put(r, PH_SWAP, p->swap_ms);
    rec_put(r, PH_SUBC, p->submit_ms);
    double build = ms_between(p->render0, p->built);
    if (p->piped) {
        double sub = ms_between(p->sub0, p->sub1);
        build = build >= 0 && sub >= 0 ? build + sub : -1;
        rec_put(r, PH_HELD, ms_between(p->built, p->sub0));
    }
    rec_put(r, PH_BUILD, build);
    uint64_t ready = p->submit;
    if (gpu) {
        r->gpu_known++;
        rec_put(r, PH_GPU, gpu > p->submit ? (double)(gpu - p->submit) / 1e6 : 0);
        if (gpu > ready) ready = gpu;
    }
    rec_put(r, PH_WAIT, ms_between(ready, flip));
    rec_put(r, PH_CHAIN, ms_between(p->wake, flip));
    /* the first vblank after the frame started (the vblanks fall on prev_flip + k periods); a piped frame is
     * on time when shown by the first vblank after its submission's wake (its paint was done before) */
    if (!p->prev_flip || !p->period || !start || start < p->prev_flip) { r->other_late++; return; }
    uint64_t k = (start - p->prev_flip) / p->period + 1, target = p->prev_flip + k * p->period;
    if (flip <= target + p->period / 2) r->on_time++;
    else if (p->submit > target) r->cpu_late++;
    else if (gpu && gpu > target) r->gpu_late++;
    else r->other_late++;
}

void lbf_phase_shown(uint64_t flip_ns, uint64_t gpu_done_ns) {
    if (!lbf_recording()) return;
    pthread_mutex_lock(&mu);
    if (have_pend && flip_ns >= pend.submit) {
        rec_shown(&rec_lb, &pend, flip_ns, gpu_done_ns);
        rec_shown(&rec_drag, &pend, flip_ns, gpu_done_ns);
    }
    have_pend = 0;
    pthread_mutex_unlock(&mu);
}

static LbPct rec_pct(const Rec *r, int ph) {
    static double t[MAXF];
    int n = r->nv[ph];
    for (int i = 0; i < n; i++) t[i] = r->v[ph][i];
    qsort(t, (size_t)n, sizeof *t, cmp_d);
    return (LbPct){ pct(t, n, 50), pct(t, n, 99) };
}

static void rec_stats(const Rec *r, LbPhaseStats *st) {
    memset(st, 0, sizeof *st);
    st->n = r->n;
    st->wake = rec_pct(r, PH_WAKE);
    st->pre = rec_pct(r, PH_PRE);
    st->paint = rec_pct(r, PH_PAINT);
    st->up = rec_pct(r, PH_UP);
    st->g0up = rec_pct(r, PH_G0UP);
    st->swap = rec_pct(r, PH_SWAP);
    st->submit_call = rec_pct(r, PH_SUBC);
    st->build = rec_pct(r, PH_BUILD);
    st->gpu = rec_pct(r, PH_GPU);
    st->wait = rec_pct(r, PH_WAIT);
    st->chain = rec_pct(r, PH_CHAIN);
    st->feed = rec_pct(r, PH_FEED);
    st->held = rec_pct(r, PH_HELD);
    int b = r->built ? r->built : 1;
    st->up_mpx = r->up_px / b / 1e6;
    st->g0_mpx = r->g0_px / b / 1e6;
    st->feed_mpx = r->feed_px / b / 1e6;
    st->on_time = r->on_time;
    st->cpu_late = r->cpu_late;
    st->gpu_late = r->gpu_late;
    st->other_late = r->other_late;
    st->gpu_known = r->gpu_known;
    st->par = r->par;
    st->g0_tried = r->g0_tried;
    st->g0_taken = r->g0_taken;
    memcpy(st->g0_refused, r->g0_refused, sizeof st->g0_refused);
    memcpy(st->g0_layer, r->g0_layer, sizeof st->g0_layer);
    st->g0_full = r->g0_full;
    st->g0_full_px = (long)r->g0_full_px;
    st->unshown = r->unshown;
    st->piped = r->piped;
}

/* lbf_begin and lbf_end start and stop LordBench's recorder with the frame times */
static void lbf_phases_begin(void) {
    pthread_mutex_lock(&mu);
    rec_clear(&rec_lb);
    atomic_store(&rec_lb.on, 1);
    pthread_mutex_unlock(&mu);
}

void lbf_phases(LbPhaseStats *st) {
    pthread_mutex_lock(&mu);
    atomic_store(&rec_lb.on, 0);
    rec_stats(&rec_lb, st);
    pthread_mutex_unlock(&mu);
}

void lbf_drag_begin(void) {
    pthread_mutex_lock(&mu);
    rec_clear(&rec_drag);
    atomic_store(&rec_drag.on, 1);
    pthread_mutex_unlock(&mu);
}

void lbf_drag_end(LbPhaseStats *st) {
    pthread_mutex_lock(&mu);
    atomic_store(&rec_drag.on, 0);
    rec_stats(&rec_drag, st);
    pthread_mutex_unlock(&mu);
}

static const char *const g0r_names[LBF_G0R] = { "layer", "clip", "ancestor", "app", "after", "post", "feed", "" };
static const char *const g0l_names[LBF_G0L] = { "gone", "invalid", "resize", "rendering", "capture", "view", "transform", "raw",
                                                "holes", "surfaces", "premul", "nobuf", "tree", "opacity", "part", "" };
const char *lbf_g0r_name(int i) { return i >= 0 && i < LBF_G0R && g0r_names[i][0] ? g0r_names[i] : NULL; }
const char *lbf_g0l_name(int i) { return i >= 0 && i < LBF_G0L && g0l_names[i][0] ? g0l_names[i] : NULL; }

void lbf_phases_text(const LbPhaseStats *st, char *buf, size_t n) {
    size_t k = 0;
#define PUT(...) do { if (k < n) { int w_ = snprintf(buf + k, n - k, __VA_ARGS__); if (w_ > 0) k += (size_t)w_; } } while (0)
    PUT("phases, %d frames (ms p50/p99): wake %.2f/%.2f, before render %.2f/%.2f, render %.2f/%.2f (paint %.2f/%.2f, "
        "upload %.2f/%.2f, G0 upload %.2f/%.2f, swap %.2f/%.2f, flip ioctl %.2f/%.2f), GPU after the flip was queued "
        "%.2f/%.2f, to the vblank %.2f/%.2f, wake to flip %.2f/%.2f, G0 catch-up %.2f/%.2f; per frame upload %.3f Mpx, "
        "G0 %.3f, catch-up %.3f; on time %d, CPU-late %d, GPU-late %d, other %d (GPU time known for %d); paint threads "
        "on %d; G0 %d of %d (", st->n, st->wake.p50, st->wake.p99, st->pre.p50, st->pre.p99, st->build.p50, st->build.p99,
        st->paint.p50, st->paint.p99, st->up.p50, st->up.p99, st->g0up.p50, st->g0up.p99, st->swap.p50, st->swap.p99,
        st->submit_call.p50, st->submit_call.p99, st->gpu.p50, st->gpu.p99, st->wait.p50, st->wait.p99, st->chain.p50,
        st->chain.p99, st->feed.p50, st->feed.p99, st->up_mpx, st->g0_mpx, st->feed_mpx, st->on_time, st->cpu_late,
        st->gpu_late, st->other_late, st->gpu_known, st->par, st->g0_taken, st->g0_tried);
    int first = 1;
    for (int i = 0; i < LBF_G0R; i++) {
        if (!st->g0_refused[i] || !lbf_g0r_name(i)) continue;
        PUT("%snot %s %d", first ? "" : ", ", lbf_g0r_name(i), st->g0_refused[i]);
        first = 0;
        if (i == 0) {                       /* G0R_LAYER: its causes */
            int f2 = 1;
            for (int j = 0; j < LBF_G0L; j++)
                if (st->g0_layer[j] && lbf_g0l_name(j)) { PUT("%s%s %d", f2 ? " [" : ", ", lbf_g0l_name(j), st->g0_layer[j]); f2 = 0; }
            if (!f2) PUT("]");
        }
    }
    PUT("), whole G0 uploads %d (%.2f Mpx), unshown %d; prepared ahead %d (held for the flip %.2f/%.2f)", st->g0_full,
        st->g0_full_px / 1e6, st->unshown, st->piped, st->held.p50, st->held.p99);
#undef PUT
    if (n) buf[k < n ? k : n - 1] = 0;
}

void lbf_frame(double work_ms) {
    if (!atomic_load_explicit(&on, memory_order_relaxed)) return;
    pthread_mutex_lock(&mu);
    if (nwork < MAXF && work_ms >= 0) work[nwork++] = work_ms;
    pthread_mutex_unlock(&mu);
}

void lbf_presented(uint64_t ns, uint32_t period_ns) {
    if (!atomic_load_explicit(&on, memory_order_relaxed)) return;
    pthread_mutex_lock(&mu);
    if (nshown < MAXF && (!nshown || ns > shown[nshown - 1])) shown[nshown++] = ns;
    if (period_ns) period = period_ns;
    for (int i = 0; i < ninframe; i++)
        if (nlat < MAXI && ns >= inframe[i]) lat[nlat++] = (double)(ns - inframe[i]) / 1e6;
    ninframe = 0;
    pthread_mutex_unlock(&mu);
}

void lbf_input(uint64_t ev_ns) {
    if (!atomic_load_explicit(&on, memory_order_relaxed)) return;
    pthread_mutex_lock(&mu);
    if (napplied == BATCH) { memmove(applied, applied + 1, sizeof applied - sizeof *applied); napplied--; }
    applied[napplied++] = ev_ns;
    pthread_mutex_unlock(&mu);
}

/* n events of from[] join to[] (*nto of them), the newest kept when it is full */
static void move_events(uint64_t *to, int *nto, const uint64_t *from, int n) {
    for (int i = 0; i < n; i++) {
        if (*nto == BATCH) { memmove(to, to + 1, sizeof(uint64_t) * (BATCH - 1)); (*nto)--; }
        to[(*nto)++] = from[i];
    }
}

void lbf_built(int main_drawn) {
    if (!main_drawn || !atomic_load_explicit(&on, memory_order_relaxed)) return;
    pthread_mutex_lock(&mu);
    /* a frame is built and submitted only once the previous one was shown (main.c waits for the flip, or
     * prepares the next one ahead: lbf_painted, lbf_submitted); if one were not, both groups would end at
     * this frame's presentation */
    move_events(inframe, &ninframe, painted, npainted);
    move_events(inframe, &ninframe, applied, napplied);
    npainted = napplied = 0;
    pthread_mutex_unlock(&mu);
}

void lbf_painted(void) {
    if (!atomic_load_explicit(&on, memory_order_relaxed)) return;
    pthread_mutex_lock(&mu);
    move_events(painted, &npainted, applied, napplied);
    napplied = 0;
    pthread_mutex_unlock(&mu);
}

void lbf_submitted(void) {
    if (!atomic_load_explicit(&on, memory_order_relaxed)) return;
    pthread_mutex_lock(&mu);
    move_events(inframe, &ninframe, painted, npainted);
    npainted = 0;
    pthread_mutex_unlock(&mu);
}

void lbf_begin(void) {
    pthread_mutex_lock(&mu);
    nwork = nshown = nlat = napplied = npainted = ninframe = 0;
    period = 0;
    atomic_store(&on, 1);
    pthread_mutex_unlock(&mu);
    lbf_phases_begin();
}

int lbf_end(LbFrameStats *st) {
    memset(st, 0, sizeof *st);
    static double w[MAXF], l[MAXI];
    pthread_mutex_lock(&mu);
    atomic_store(&on, 0);
    atomic_store(&rec_lb.on, 0);        /* its phases stay for lbf_phases */
    int nw = nwork, ns = nshown, nl = nlat;
    memcpy(w, work, (size_t)nw * sizeof *w);
    memcpy(l, lat, (size_t)nl * sizeof *l);
    double budget = (period ? period : 16666667u) / 1e6;
    st->budget_ms = budget;
    st->frames = nw;
    st->shown = ns;
    int missed = 0;
    double gap = 0;
    for (int i = 1; i < ns; i++) {
        double d = (double)(shown[i] - shown[i - 1]) / 1e6;
        if (d > budget * 1.5) missed++;
        if (d > gap) gap = d;
    }
    if (ns >= 2) {
        st->span_s = (double)(shown[ns - 1] - shown[0]) / 1e9;
        st->fps = st->span_s > 0 ? (ns - 1) / st->span_s : 0;
        st->missed_pct = 100.0 * missed / (ns - 1);
        st->gap_max = gap;
    }
    pthread_mutex_unlock(&mu);
    qsort(w, (size_t)nw, sizeof *w, cmp_d);
    st->work_p50 = pct(w, nw, 50);
    st->work_p99 = pct(w, nw, 99);
    st->work_max = nw ? w[nw - 1] : 0;
    qsort(l, (size_t)nl, sizeof *l, cmp_d);
    st->inputs = nl;
    st->in_p50 = pct(l, nl, 50);
    st->in_p99 = pct(l, nl, 99);
    return ns >= 2 ? 0 : -1;
}
