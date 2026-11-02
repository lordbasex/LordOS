/* netrec.c — feeds the traffic history (see netrec.h). Costs one read of /proc/net/dev and a look at
 * the latest lord-netperm counters every 5 s, plus one append of a few dozen bytes per minute. */
#include "netrec.h"
#include "monpriv.h"
#include "../ui/js_fw.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TICK_MS 5000
#define PREV_MAX 512
#define IF_MAX 32

static int allowed, on;
static NetHist *hist;
static double next_ms;
static int primed;                      /* a first reading was taken: new entries count whole */
static uint64_t counts_seq;

/* the previous reading of each app entry (an app may have several scopes: by occurrence) */
static struct { char app[NH_ID_MAX]; int occ; uint64_t rx, tx; } prev[PREV_MAX], cur[PREV_MAX];
static int nprev, ncur;
static struct { char name[32]; uint64_t rx, tx; } ifprev[IF_MAX];
static int nifprev, if_primed;

static double mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

long long netrec_now(void) { return (long long)time(NULL); }

static void path_of(char *out, size_t n) {
    const char *x = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    out[0] = 0;
    if (x && x[0] == '/' && !strstr(x, "/../")) snprintf(out, n, "%s/lordos/net-history.bin", x);
    else if (home && home[0] == '/') snprintf(out, n, "%s/.local/state/lordos/net-history.bin", home);
}

void netrec_init(int ok) {
    allowed = ok;
    if (!ok || hist) return;
    char p[PATH_MAX];
    path_of(p, sizeof p);
    if (p[0]) hist = nh_new(p);
}

NetHist *netrec_hist(void) { return allowed ? hist : NULL; }
int netrec_enabled(void) { return allowed && on && hist; }

void netrec_enable(int v) {
    if (!allowed || !hist || !!v == on) return;
    on = !!v;
    if (on) { primed = 0; if_primed = 0; nprev = 0; nifprev = 0; next_ms = mono_ms() + 1000; }
    else nh_flush(hist, netrec_now() + 60);          /* what was counted until now */
}

double netrec_timeout(void) {
    if (!netrec_enabled()) return -1;
    double d = next_ms - mono_ms();
    return d < 0 ? 0 : d;
}

/* {"app":[allowed,denied,rx,tx,...,[[...]]],...} from lord-netperm (npproto.h). Only the key and the
 * 3rd and 4th numbers are taken; anything unexpected ends the reading (what was read stays). */
static void read_counts(const char *j) {
    ncur = 0;
    const char *p = j;
    while (*p == ' ') p++;
    if (*p++ != '{') return;
    for (;;) {
        while (*p == ' ' || *p == ',') p++;
        if (*p != '"') return;
        const char *k = ++p;
        while (*p && *p != '"' && *p != '\\') p++;
        if (*p != '"' || p - k <= 0 || p - k >= NH_ID_MAX) return;
        char app[NH_ID_MAX];
        memcpy(app, k, (size_t)(p - k));
        app[p - k] = 0;
        p++;
        if (*p++ != ':' || *p++ != '[') return;
        unsigned long long v[4] = { 0 };
        for (int i = 0; i < 4; i++) {
            char *e;
            v[i] = strtoull(p, &e, 10);
            if (e == p) return;
            p = e;
            if (*p == ',') p++;
        }
        /* the rest of this entry: up to its closing bracket (strings hold addresses, no brackets) */
        int depth = 1;
        while (*p && depth) {
            if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; } if (!*p) return; }
            else if (*p == '[') depth++;
            else if (*p == ']') depth--;
            p++;
        }
        if (depth) return;
        if (ncur < PREV_MAX) {
            int occ = 0;
            for (int i = 0; i < ncur; i++) occ += !strcmp(cur[i].app, app);
            snprintf(cur[ncur].app, NH_ID_MAX, "%s", app);
            cur[ncur].occ = occ;
            cur[ncur].rx = v[2];
            cur[ncur].tx = v[3];
            ncur++;
        }
        while (*p == ' ') p++;
        if (*p == '}') return;
    }
}

static uint64_t delta(uint64_t now, uint64_t before) { return now >= before ? now - before : now; }   /* restarted: from 0 */

static void apps_tick(long long now) {
    uint64_t seq = counts_seq;
    const char *j = js_fw_counts(&seq);
    if (!j || seq == counts_seq) return;            /* nothing new: nothing moved */
    counts_seq = seq;
    read_counts(j);
    for (int i = 0; i < ncur; i++) {
        int f = -1;
        for (int k = 0; k < nprev && f < 0; k++) if (prev[k].occ == cur[i].occ && !strcmp(prev[k].app, cur[i].app)) f = k;
        if (f < 0 && !primed) continue;             /* the first reading is only a starting point */
        uint64_t rx = f >= 0 ? delta(cur[i].rx, prev[f].rx) : cur[i].rx, tx = f >= 0 ? delta(cur[i].tx, prev[f].tx) : cur[i].tx;
        nh_add(hist, now, cur[i].app, rx, tx);
    }
    memcpy(prev, cur, sizeof cur[0] * (size_t)ncur);
    nprev = ncur;
    primed = 1;
}

/* The machine: every interface but lo, as the Monitor's totals (mon.c). */
static void total_tick(long long now) {
    char txt[8192];
    if (mon_read_small("/proc/net/dev", txt, sizeof txt) <= 0) return;
    struct { char name[32]; uint64_t rx, tx; } c[IF_MAX];
    int n = 0;
    for (char *l = txt; l && n < IF_MAX; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        char *colon = strchr(l, ':'), *nl = strchr(l, '\n');
        if (!colon || (nl && colon > nl)) continue;
        char *s = l;
        while (*s == ' ') s++;
        if (colon - s <= 0 || colon - s >= 32) continue;
        snprintf(c[n].name, sizeof c[n].name, "%.*s", (int)(colon - s), s);
        unsigned long long v[16];
        char *p = colon + 1;
        for (int i = 0; i < 16; i++) v[i] = strtoull(p, &p, 10);
        c[n].rx = v[0];
        c[n].tx = v[8];
        n++;
    }
    uint64_t rx = 0, tx = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(c[i].name, "lo")) continue;
        int f = -1;
        for (int k = 0; k < nifprev && f < 0; k++) if (!strcmp(ifprev[k].name, c[i].name)) f = k;
        if (f < 0 && !if_primed) continue;
        rx += f >= 0 ? delta(c[i].rx, ifprev[f].rx) : c[i].rx;
        tx += f >= 0 ? delta(c[i].tx, ifprev[f].tx) : c[i].tx;
    }
    for (int i = 0; i < n; i++) { memcpy(ifprev[i].name, c[i].name, sizeof ifprev[i].name); ifprev[i].rx = c[i].rx; ifprev[i].tx = c[i].tx; }
    nifprev = n;
    if_primed = 1;
    nh_add(hist, now, NULL, rx, tx);
}

/* (0.8.5) The machine's byte counters right now, every interface but lo (the menu bar's network
 * item: one small file read per second while it is shown). 0 ok. */
int netrec_totals(uint64_t *rx, uint64_t *tx) {
    char txt[8192];
    if (mon_read_small("/proc/net/dev", txt, sizeof txt) <= 0) return -1;
    *rx = *tx = 0;
    for (char *l = txt; l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        char *colon = strchr(l, ':'), *nl = strchr(l, '\n');
        if (!colon || (nl && colon > nl)) continue;
        char *s = l;
        while (*s == ' ') s++;
        if (colon - s == 2 && !strncmp(s, "lo", 2)) continue;
        unsigned long long v[16];
        char *p = colon + 1;
        for (int i = 0; i < 16; i++) v[i] = strtoull(p, &p, 10);
        *rx += v[0];
        *tx += v[8];
    }
    return 0;
}

void netrec_tick(void) {
    if (!netrec_enabled()) return;
    double t = mono_ms();
    if (t < next_ms) return;
    next_ms = t + TICK_MS;
    long long now = netrec_now();
    total_tick(now);
    apps_tick(now);
    nh_flush(hist, now);
}

void netrec_shutdown(void) {
    if (hist && on) nh_flush(hist, netrec_now() + 60);
    nh_free(hist);
    hist = NULL;
}
