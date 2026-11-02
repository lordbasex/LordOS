/* keylag.c — key latency trace, see keylag.h (privacy rules there) and docs/TROUBLESHOOTING.md. */
#include "keylag.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int keylag_enabled;

#define KL_MAX 32
#define KL_GIVE_UP_MS 2000.0                    /* a stage that never came: printed with -1 */
#define KL_WINDOW_MS 120000.0                   /* stops by itself this long after the first traced key */
#define KL_MAX_LINES 4000

typedef struct {
    unsigned n;
    int press;
    double ev, rx, sent, flush, commit, rendered;   /* CLOCK_MONOTONIC ms; 0 = not yet */
} Key;

static Key keys[KL_MAX];
static int nkeys;
static Key pending;                             /* the key being dispatched (not counted yet) */
static int has_pending;
static unsigned seq, lines;
static double started;
static FILE *out;

static double mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

void keylag_init(int greeter) {
    const char *e = getenv("LORD_INPUT_DEBUG");
    keylag_enabled = !greeter && e && !strcmp(e, "1");
    if (keylag_enabled) fprintf(stderr, "keylag: key latency trace on (LORD_INPUT_DEBUG=1), in $XDG_RUNTIME_DIR/axia-keylag.log\n");
}

static void stop(const char *why) {
    keylag_enabled = 0;
    nkeys = 0;
    has_pending = 0;
    if (out) { fprintf(out, "keylag stop: %s\n", why); fclose(out); out = NULL; }
    fprintf(stderr, "keylag: trace stopped (%s)\n", why);
}

/* The trace file, created on the first traced key: only in the session's runtime folder, 0600. */
static int open_out(void) {
    if (out) return 0;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char path[512];
    if (!rt || rt[0] != '/' || snprintf(path, sizeof path, "%s/axia-keylag.log", rt) >= (int)sizeof path) return -1;
    unlink(path);                                   /* a trace left by an earlier session of this user */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (!(out = fdopen(fd, "w"))) { close(fd); return -1; }
    setvbuf(out, NULL, _IOLBF, 0);
    fprintf(out, "keylag start: times in ms after the kernel's evdev timestamp\n");
    return 0;
}

static double rel(double t, double ev) { return t > 0 ? t - ev : -1; }

static void print_key(const Key *k, double shown) {
    if (!out) return;
    fprintf(out, "keylag n=%u ev=%.3f %s rx=%.2f sent=%.2f flush=%.2f commit=%.2f shown=%.2f\n", k->n, k->ev,
            k->press ? "press" : "release", rel(k->rx, k->ev), rel(k->sent, k->ev), rel(k->flush, k->ev),
            rel(k->commit, k->ev), rel(shown, k->ev));
    if (++lines >= KL_MAX_LINES) stop("line limit");
}

static void drop_first(void) {
    memmove(keys, keys + 1, sizeof(Key) * (size_t)(nkeys - 1));
    nkeys--;
}

/* Keys that have waited too long for a stage are printed as they are; the window closes. */
static void expire(double now) {
    while (keylag_enabled && nkeys && now - keys[0].ev > KL_GIVE_UP_MS) {
        print_key(&keys[0], 0);
        if (keylag_enabled) drop_first();
    }
    if (keylag_enabled && started && now - started > KL_WINDOW_MS) stop("120 s window over");
}

void keylag_rx(int value, const struct timeval *tv) {
    has_pending = 0;
    if (!keylag_enabled || value == 2) return;      /* autorepeat: the apps repeat on their own */
    double now = mono_ms();
    struct timespec real;
    clock_gettime(CLOCK_REALTIME, &real);
    /* evdev stamps with CLOCK_REALTIME (the default clock id): moved to the monotonic clock */
    double real_ms = real.tv_sec * 1e3 + real.tv_nsec / 1e6, ev_ms = tv->tv_sec * 1e3 + tv->tv_usec / 1e3;
    pending = (Key){ .press = value == 1, .ev = now - (real_ms - ev_ms), .rx = now };
    has_pending = 1;
}

void keylag_sent_wl(int traceable) {
    if (!keylag_enabled || !has_pending) return;
    has_pending = 0;
    if (!traceable) return;                         /* protected: not traced and not counted */
    double now = mono_ms();
    expire(now);
    if (!keylag_enabled || open_out()) { if (keylag_enabled) stop("cannot create the trace file"); return; }
    if (!started) started = now;
    if (nkeys == KL_MAX) { print_key(&keys[0], 0); if (!keylag_enabled) return; drop_first(); }
    pending.n = ++seq;
    pending.sent = now;
    keys[nkeys++] = pending;
}

void keylag_handled(void) { has_pending = 0; }     /* not sent to an app: LordOS's own, never traced */

void keylag_flushed(void) {
    if (!keylag_enabled) return;
    double now = 0;
    for (int i = 0; i < nkeys; i++)
        if (!keys[i].flush) keys[i].flush = now ? now : (now = mono_ms());
}

void keylag_commit(void) {
    if (!keylag_enabled) return;
    double now = 0;
    for (int i = 0; i < nkeys; i++)
        if (keys[i].flush && !keys[i].commit) keys[i].commit = now ? now : (now = mono_ms());
}

void keylag_render(void) {
    if (!keylag_enabled) return;
    double now = 0;
    for (int i = 0; i < nkeys; i++)
        if (!keys[i].rendered && keys[i].commit) keys[i].rendered = now ? now : (now = mono_ms());
}

void keylag_presented(uint64_t flip_ns) {
    if (!keylag_enabled) return;
    double shown = flip_ns ? flip_ns / 1e6 : mono_ms();
    int kept = 0;
    for (int i = 0; i < nkeys && keylag_enabled; i++) {
        if (keys[i].rendered) print_key(&keys[i], shown);
        else keys[kept++] = keys[i];
    }
    if (!keylag_enabled) return;
    nkeys = kept;
    expire(mono_ms());
}
