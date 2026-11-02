/* bench.c — see bench.h. */
#define _POSIX_C_SOURCE 200809L
#include "bench.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

double bench_now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

void bench_frames_add(BenchFrames *f, double t_ms) {
    if (f->frames) f->iv[f->n < BENCH_HIST ? f->n++ : (int)(f->frames % BENCH_HIST)] = t_ms - f->last;
    else f->first = t_ms;
    f->last = t_ms;
    f->frames++;
}

double bench_frames_fps(const BenchFrames *f) {
    double span = f->last - f->first;
    return span > 0 && f->frames > 1 ? (f->frames - 1) * 1000.0 / span : 0;
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

void bench_frames_stats(const BenchFrames *f, BenchStats *out) {
    memset(out, 0, sizeof *out);
    if (!f->n) return;
    double s[BENCH_HIST], sum = 0;
    memcpy(s, f->iv, f->n * sizeof *s);
    for (int i = 0; i < f->n; i++) sum += s[i];
    qsort(s, f->n, sizeof *s, cmp_d);
    out->avg = sum / f->n;
    out->max = s[f->n - 1];
    out->p50 = s[(f->n - 1) / 2];                     /* nearest rank */
    int r = (99 * f->n + 99) / 100;                   /* ceil(0.99 n) */
    out->p99 = s[(r < 1 ? 1 : r) - 1];
}

int bench_frames_missed(const BenchFrames *f, double budget_ms) {
    int m = 0;
    if (budget_ms <= 0) return 0;
    for (int i = 0; i < f->n; i++) if (f->iv[i] > budget_ms * 1.5) m++;
    return m;
}

void bench_present(BenchPresent *p, double t_ms, uint64_t msc, uint32_t refresh_ns, int zero_copy) {
    p->presented++;
    if (refresh_ns) p->refresh_ns = refresh_ns;
    if (zero_copy) p->zero_copy++;
    if (p->last_msc && msc > p->last_msc + 1) p->vblank_skips += msc - p->last_msc - 1;
    else if (!msc && p->presented > 1 && p->refresh_ns) {    /* no counter: count from the times */
        double gap = (t_ms - p->last_ms) / (p->refresh_ns / 1e6);
        if (gap > 1.5) p->vblank_skips += (unsigned long)(gap + 0.5) - 1;
    }
    p->last_msc = msc;
    p->last_ms = t_ms;
}

double bench_refresh_hz(const BenchPresent *p) { return p->refresh_ns ? 1e9 / p->refresh_ns : 0; }

void bench_latency_add(BenchLatency *l, double ms) {
    if (ms < 0 || ms >= 2000) { l->lost++; return; }
    l->last = ms;
    l->sum += ms;
    l->n++;
}

double bench_latency_avg(const BenchLatency *l) { return l->n ? l->sum / l->n : 0; }

const char *bench_gl_kind(const char *r) {
    if (!r || !*r) return "";
    if (strstr(r, "llvmpipe") || strstr(r, "softpipe") || strstr(r, "swrast") || strstr(r, "SwiftShader")) return "SOFTWARE (CPU)";
    if (strstr(r, "virgl") || strstr(r, "VirGL") || strstr(r, "SVGA") || strstr(r, "virtio")) return "VIRTUAL GPU (HOST)";
    return "HARDWARE GPU";
}

void bench_json_str(FILE *o, const char *s) {
    fputc('"', o);
    for (const unsigned char *q = (const unsigned char *)(s ? s : ""); *q; q++) {
        if (*q == '"' || *q == '\\') fprintf(o, "\\%c", *q);
        else if (*q < 0x20) fprintf(o, "\\u%04x", *q);
        else fputc(*q, o);
    }
    fputc('"', o);
}

int bench_save(const char *name, void (*write)(FILE *)) {
    if (!name || !*name || strlen(name) > 32 || strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789-") != strlen(name)) return -1;
    const char *st = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    char dir[512], tmp[600], path[600];
    if (st && st[0] == '/') snprintf(dir, sizeof dir, "%s/lordos", st);
    else if (home && home[0] == '/') {
        snprintf(dir, sizeof dir, "%s/.local", home);
        mkdir(dir, 0700);
        snprintf(dir, sizeof dir, "%s/.local/state", home);
        mkdir(dir, 0700);
        snprintf(dir, sizeof dir, "%s/.local/state/lordos", home);
    } else return -1;
    mkdir(dir, 0700);
    snprintf(tmp, sizeof tmp, "%s/.%s.json.tmp", dir, name);
    snprintf(path, sizeof path, "%s/%s.json", dir, name);
    FILE *f = fopen(tmp, "we");
    if (!f) return -1;
    write(f);
    if (fclose(f) != 0) { unlink(tmp); return -1; }
    return rename(tmp, path);
}
