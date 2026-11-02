/* nethist_test.c — the Monitor's traffic history file (src/monitor/nethist.c, docs/MONITOR.md
 * "Traffic history"): totals survive writing and reading, minutes older than 7 days become hours,
 * hours older than 30 days go, a cut or damaged file never crashes and keeps what came before the
 * damage, too many apps go to "other", and Clear history removes everything.
 *   build/nethist-test */
#include "monitor/nethist.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char dir[256], path[300];

static uint8_t *slurp(size_t *n) {
    FILE *f = fopen(path, "rb");
    *n = 0;
    if (!f) return NULL;
    uint8_t *b = malloc(NH_FILE_MAX);
    *n = fread(b, 1, NH_FILE_MAX, f);
    fclose(f);
    return b;
}
static void spit(const uint8_t *b, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(b, 1, n, f);
    fclose(f);
}

static double sum(const double *v, int n) { double s = 0; for (int i = 0; i < n; i++) s += v[i]; return s; }

/* Totals of everything in [from, from + n*step) for app (NULL: the machine). */
static void totals(NetHist *h, long long now, long long from, long long step, int n, const char *app, double *rx, double *tx, int *res) {
    double *a = calloc((size_t)n, sizeof *a), *b = calloc((size_t)n, sizeof *b);
    NhTop top[4];
    int nt;
    long long old;
    *res = nh_query(h, now, from, step, n, app, a, b, top, 4, &nt, &old);
    *rx = sum(a, n);
    *tx = sum(b, n);
    free(a);
    free(b);
}

static void test_roundtrip(void) {
    unlink(path);
    long long t0 = 1760000000LL - 1760000000LL % 3600;     /* on an hour */
    NetHist *h = nh_new(path);
    for (int m = 0; m < 90; m++) {                          /* 90 minutes, 3 apps */
        long long now = t0 + m * 60 + 5;
        nh_add(h, now, NULL, 1000, 100);
        nh_add(h, now, "desktop:firefox-esr", 600, 60);
        nh_add(h, now, "lpk:fractal", m % 2 ? 300 : 0, 30);
        nh_add(h, now, "lordos", 10, 1);
        nh_flush(h, now + 50);
    }
    double rx, tx;
    int res;
    totals(h, t0 + 90 * 60, t0, 60, 90, NULL, &rx, &tx, &res);
    CHECK(rx == 90000 && tx == 9000 && res == 60, "machine: %.0f/%.0f res %d", rx, tx, res);
    totals(h, t0 + 90 * 60, t0, 60, 90, "desktop:firefox-esr", &rx, &tx, &res);
    CHECK(rx == 54000 && tx == 5400, "app: %.0f/%.0f", rx, tx);
    totals(h, t0 + 90 * 60, t0, 600, 9, "lpk:fractal", &rx, &tx, &res);
    CHECK(rx == 45 * 300 && tx == 90 * 30, "bigger buckets: %.0f/%.0f", rx, tx);
    /* top apps */
    double a[3], b[3];
    NhTop top[4];
    int nt;
    long long old;
    nh_query(h, t0 + 90 * 60, t0, 1800, 3, NULL, a, b, top, 4, &nt, &old);
    CHECK(nt == 3 && !strcmp(top[0].id, "desktop:firefox-esr") && !strcmp(top[1].id, "lpk:fractal") && old == t0, "top: %d %s, oldest %lld", nt, nt ? top[0].id : "", old - t0);
    nh_free(h);
    /* reopened: the same */
    h = nh_new(path);
    totals(h, t0 + 90 * 60, t0, 60, 90, NULL, &rx, &tx, &res);
    CHECK(rx == 90000 && tx == 9000, "after reopening: %.0f/%.0f", rx, tx);
    /* the file is small: a minute with 3 apps is a few dozen bytes */
    struct stat st;
    stat(path, &st);
    CHECK(st.st_size < 90 * 40, "90 minutes take %lld bytes", (long long)st.st_size);
    CHECK((st.st_mode & 0777) == 0600, "mode %o", st.st_mode & 0777);
    nh_free(h);
}

/* 40 days of one minute every 10: minutes past 7 days fold into hours, hours past 30 days go. */
static void test_rotation(void) {
    unlink(path);
    long long t0 = 1760000000LL - 1760000000LL % 3600;
    NetHist *h = nh_new(path);
    long long now = t0;
    long long last = 40LL * 24 * 6;
    for (long long m = 0; m < last; m++) {
        now = t0 + m * 600;
        nh_add(h, now, NULL, 500, 50);
        nh_add(h, now, "desktop:app", 200, 20);
        nh_flush(h, now + 60);
    }
    nh_compact(h, now + 60);
    /* what the last compaction keeps: minutes of the last 7 days, hours of the last 30 */
    double want_rx = 0;
    long long nowf = now + 60;
    for (long long m = 0; m < last; m++) {
        long long minute = (t0 + m * 600) / 60;
        if (minute >= nowf / 60 - NH_MIN_KEEP || minute / 60 >= nowf / 3600 - NH_HOUR_KEEP) want_rx += 500;
    }
    size_t n;
    uint8_t *b = slurp(&n);
    int recs = 0;
    CHECK(b && nh_check(b, n, &recs) == 0, "compacted file checks out");
    /* 7 days of minutes (1008) + 23 days of hours (552) + 1 app ≈ 1561 records */
    CHECK(recs > 1400 && recs < 1700, "%d records after 40 days", recs);
    CHECK(n < 64 * 1024, "40 days take %zu bytes", n);
    free(b);
    double rx, tx;
    int res;
    totals(h, now + 60, now + 60 - 31LL * 86400, 3600, 31 * 24, NULL, &rx, &tx, &res);
    CHECK(rx == want_rx && res == 60, "30 days kept: %.0f of %.0f (res %d)", rx, want_rx, res);
    totals(h, now + 60, t0, 3600, 9 * 24, NULL, &rx, &tx, &res);
    CHECK(rx == 0 && res == 0, "nothing older than 30 days: %.0f", rx);
    /* a window 10 days back has only hours: res 3600, and the totals match */
    long long w = now - 10LL * 86400;
    w -= w % 3600;
    totals(h, now + 60, w, 300, 12, "desktop:app", &rx, &tx, &res);
    CHECK(res == 3600 && fabs(rx - 6 * 200) < 1, "an old hour at 5-minute buckets: %.0f res %d", rx, res);
    nh_free(h);
}

/* A cut or damaged file: never a crash, what came before the damage stays, compaction cleans it. */
static void test_corruption(void) {
    unlink(path);
    long long t0 = 1760000000LL - 1760000000LL % 3600;
    NetHist *h = nh_new(path);
    for (int m = 0; m < 30; m++) { nh_add(h, t0 + m * 60, NULL, 100, 10); nh_add(h, t0 + m * 60, "desktop:x", 50, 5); nh_flush(h, t0 + m * 60 + 60); }
    nh_free(h);
    size_t n;
    uint8_t *good = slurp(&n);
    int recs = 0;
    CHECK(good && nh_check(good, n, &recs) == 0 && recs == 31, "%d records", recs);
    int crashes_ok = 0;
    for (size_t cut = 0; cut < n; cut++) {                  /* every truncation */
        spit(good, cut);
        h = nh_new(path);
        double rx, tx;
        int res;
        totals(h, t0 + 3600, t0, 60, 60, NULL, &rx, &tx, &res);
        if (rx > 3000 || rx < 0 || (long long)rx % 100) { CHECK(0, "cut at %zu: %.0f", cut, rx); }
        nh_add(h, t0 + 1800, NULL, 7, 7);                  /* goes on writing after the damage */
        nh_free(h);
        size_t m;
        uint8_t *b = slurp(&m);
        int r2;
        if (b && nh_check(b, m, &r2) == 0) crashes_ok++;
        free(b);
    }
    CHECK(crashes_ok == (int)n, "after any cut the file is clean again (%d of %zu)", crashes_ok, n);
    srand(7);
    int kept_before = 0, trials = 2000;
    for (int k = 0; k < trials; k++) {                     /* random bytes changed */
        uint8_t *b = malloc(n);
        memcpy(b, good, n);
        size_t at = 8 + (size_t)rand() % (n - 8);
        b[at] ^= (uint8_t)(1 + rand() % 255);
        int rc;
        size_t stop = nh_check(b, n, &rc);
        if (stop && stop <= at) kept_before++;
        spit(b, n);
        free(b);
        h = nh_new(path);
        double rx, tx;
        int res;
        totals(h, t0 + 3600, t0, 60, 60, "desktop:x", &rx, &tx, &res);
        nh_free(h);
    }
    CHECK(kept_before == trials, "a damaged record stops the reading at it or before (%d of %d)", kept_before, trials);
    /* not ours at all */
    spit((const uint8_t *)"hello, this is not a history file", 33);
    h = nh_new(path);
    nh_add(h, t0, NULL, 1, 1);
    nh_flush(h, t0 + 60);
    nh_free(h);
    uint8_t *b = slurp(&n);
    CHECK(b && n >= 8 && !memcmp(b, "LNH1", 4) && nh_check(b, n, &recs) == 0, "a foreign file is replaced by a clean one");
    free(b);
    free(good);
}

static void test_apps_and_clear(void) {
    unlink(path);
    long long t0 = 1760000000LL;
    NetHist *h = nh_new(path);
    char id[64];
    for (int i = 0; i < NH_APPS_MAX + 40; i++) { snprintf(id, sizeof id, "desktop:app%d", i); nh_add(h, t0, id, 1, 1); }
    nh_add(h, t0, "bad id with spaces", 5, 5);
    nh_flush(h, t0 + 60);
    double rx, tx;
    int res;
    totals(h, t0 + 60, t0 - t0 % 60, 60, 1, "desktop:app3", &rx, &tx, &res);
    CHECK(rx == 1, "app 3: %.0f", rx);
    nh_free(h);
    size_t n;
    uint8_t *b = slurp(&n);
    int recs;
    CHECK(b && nh_check(b, n, &recs) == 0, "a file with %d apps checks out", NH_APPS_MAX);
    free(b);
    h = nh_new(path);
    CHECK(nh_clear(h) == 0 && access(path, F_OK) != 0, "Clear history removes the file");
    totals(h, t0 + 60, t0 - t0 % 60, 60, 1, NULL, &rx, &tx, &res);
    CHECK(rx == 0 && res == 0, "nothing after Clear: %.0f", rx);
    nh_add(h, t0 + 120, NULL, 9, 9);
    nh_add(h, t0 + 120, "desktop:app1", 4, 4);
    nh_flush(h, t0 + 200);
    totals(h, t0 + 200, t0 + 120 - (t0 + 120) % 60, 60, 1, "desktop:app1", &rx, &tx, &res);
    CHECK(rx == 4, "recording goes on after Clear, with its app records again: %.0f", rx);
    nh_free(h);
    /* bad arguments */
    h = nh_new(path);
    double a[2], c[2];
    CHECK(nh_query(h, t0, t0, 0, 2, NULL, a, c, NULL, 0, NULL, NULL) == -1 && nh_query(h, t0, t0, 60, 0, NULL, a, c, NULL, 0, NULL, NULL) == -1 &&
          nh_query(h, t0, t0, 60, NH_BUCKETS_MAX + 1, NULL, a, c, NULL, 0, NULL, NULL) == -1, "bad arguments are refused");
    nh_free(h);
    /* a symlink in its place is not followed */
    unlink(path);
    if (symlink("/etc/passwd", path) == 0) {
        h = nh_new(path);
        nh_add(h, t0, NULL, 1, 1);
        CHECK(nh_flush(h, t0 + 60) == -1, "a symlink in the file's place is refused");
        nh_free(h);
        unlink(path);
    }
}

int main(void) {
    snprintf(dir, sizeof dir, "/tmp/nethist-test-XXXXXX");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(path, sizeof path, "%s/state/axia/net-history.bin", dir);
    test_roundtrip();
    test_rotation();
    test_corruption();
    test_apps_and_clear();
    unlink(path);
    char p[400];
    snprintf(p, sizeof p, "%s/state/axia", dir); rmdir(p);
    snprintf(p, sizeof p, "%s/state", dir); rmdir(p);
    rmdir(dir);
    printf("nethist-test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
