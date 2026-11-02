/* bench_test.c — the measurement pieces shared by lordos-testbench and LordBench (src/bench/bench.c). */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "../src/bench/bench.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, oks;
#define CHECK(c, ...) do { if (c) oks++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int near(double a, double b) { return fabs(a - b) < 1e-6; }

static void w_hello(FILE *o) { fputs("{\"x\":1}\n", o); }

int main(void) {
    /* frame times: 100 frames at 16 ms with one 50 ms stall */
    BenchFrames f;
    memset(&f, 0, sizeof f);
    double t = 1000;
    for (int i = 0; i < 100; i++) { bench_frames_add(&f, t); t += i == 50 ? 50 : 16; }
    BenchStats s;
    bench_frames_stats(&f, &s);
    CHECK(f.frames == 100 && f.n == 99, "frames %lu n %d", f.frames, f.n);
    CHECK(near(s.max, 50) && near(s.p50, 16) && near(s.p99, 50), "max %f p50 %f p99 %f", s.max, s.p50, s.p99);  /* 1 in 99: the p99 */
    CHECK(near(s.avg, (98 * 16 + 50) / 99.0), "avg %f", s.avg);
    CHECK(bench_frames_missed(&f, 1000.0 / 60) == 1, "missed at 60 Hz");
    CHECK(bench_frames_missed(&f, 1000.0 / 144) == 99, "missed at 144 Hz %d", bench_frames_missed(&f, 1000.0 / 144));
    CHECK(near(bench_frames_fps(&f), 99 * 1000.0 / (98 * 16 + 50)), "fps %f", bench_frames_fps(&f));
    /* the ring keeps the last BENCH_HIST intervals; p99 with two stalls in 240 */
    memset(&f, 0, sizeof f);
    for (int i = 0; i <= 1000; i++) bench_frames_add(&f, i * 10.0 + (i > 990 ? 30 : 0));
    bench_frames_stats(&f, &s);
    CHECK(f.n == BENCH_HIST && near(s.max, 40) && near(s.p50, 10), "ring n %d max %f p50 %f", f.n, s.max, s.p50);
    /* 1 stall in 199 intervals stays out of the p99 (nearest rank 198) */
    memset(&f, 0, sizeof f);
    for (int i = 0; i < 200; i++) bench_frames_add(&f, i * 16.0 + (i >= 100 ? 34 : 0));
    bench_frames_stats(&f, &s);
    CHECK(f.n == 199 && near(s.p99, 16) && near(s.max, 50), "p99 199: %f", s.p99);
    BenchFrames e;
    memset(&e, 0, sizeof e);
    bench_frames_stats(&e, &s);
    CHECK(s.avg == 0 && s.p99 == 0 && bench_frames_fps(&e) == 0 && bench_frames_missed(&e, 16) == 0, "empty");

    /* presentation: vblank counter, and without one the times */
    BenchPresent p;
    memset(&p, 0, sizeof p);
    bench_present(&p, 0, 100, 16666667, 0);
    bench_present(&p, 16.7, 101, 0, 1);
    bench_present(&p, 66.7, 104, 0, 0);
    CHECK(p.presented == 3 && p.vblank_skips == 2 && p.zero_copy == 1 && fabs(bench_refresh_hz(&p) - 60) < 0.01, "msc skips %lu", p.vblank_skips);
    memset(&p, 0, sizeof p);
    bench_present(&p, 0, 0, 16666667, 0);
    bench_present(&p, 50, 0, 0, 0);     /* 3 refreshes later: 2 skipped */
    bench_present(&p, 66.7, 0, 0, 0);
    CHECK(p.vblank_skips == 2, "time skips %lu", p.vblank_skips);

    /* latency */
    BenchLatency l = { .last = -1 };
    bench_latency_add(&l, 20);
    bench_latency_add(&l, 30);
    bench_latency_add(&l, -1);
    bench_latency_add(&l, 5000);
    CHECK(l.n == 2 && l.lost == 2 && near(bench_latency_avg(&l), 25) && near(l.last, 30), "latency");

    /* the GL renderer's kind */
    CHECK(!strcmp(bench_gl_kind("llvmpipe (LLVM 19.1.7, 256 bits)"), "SOFTWARE (CPU)"), "llvmpipe");
    CHECK(!strcmp(bench_gl_kind("virgl (AMD Radeon)"), "VIRTUAL GPU (HOST)"), "virgl");
    CHECK(!strcmp(bench_gl_kind("AMD Radeon RX 580 (radeonsi, polaris10)"), "HARDWARE GPU"), "amd");
    CHECK(!strcmp(bench_gl_kind("V3D 7.1.7.0"), "HARDWARE GPU"), "pi 5");
    CHECK(!strcmp(bench_gl_kind(""), "") && !strcmp(bench_gl_kind(NULL), ""), "empty renderer");

    /* JSON strings */
    char *buf = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&buf, &len);
    bench_json_str(m, "a\"b\\c\n\x01");
    fclose(m);
    CHECK(!strcmp(buf, "\"a\\\"b\\\\c\\u000a\\u0001\""), "json %s", buf);
    free(buf);

    /* the state file: written whole under $XDG_STATE_HOME/lordos, bad names refused */
    char dir[] = "/tmp/bench-test-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp");
    setenv("XDG_STATE_HOME", dir, 1);
    CHECK(bench_save("testbench", w_hello) == 0, "save");
    char path[128], got[32] = "";
    snprintf(path, sizeof path, "%s/lordos/testbench.json", dir);
    FILE *r = fopen(path, "r");
    CHECK(r && fgets(got, sizeof got, r) && !strcmp(got, "{\"x\":1}\n"), "saved content");
    if (r) fclose(r);
    CHECK(bench_save("../x", w_hello) < 0 && bench_save("A", w_hello) < 0 && bench_save("", w_hello) < 0, "bad names");
    unlink(path);
    snprintf(path, sizeof path, "%s/lordos", dir);
    rmdir(path);
    rmdir(dir);

    printf("bench-test: %d ok, %d failed\n", oks, fails);
    return fails ? 1 : 0;
}
