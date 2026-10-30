/* clean_bench.c — times Cleaner's scans on a throwaway folder given as the home folder (never a real one): wall
 * time, the helper's peak memory, lord-server's memory, and the cost of the calls the compositor's thread makes.
 *   build/clean-bench <folder>     needs build/lord-clean next to it */
#include "clean/clean.h"
#include "clean/rules.h"
#include "fsops.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static long vm(const char *path, const char *key) {
    FILE *f = fopen(path, "r");
    char line[256];
    long v = 0;
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, key, strlen(key))) v = atol(line + strlen(key) + 1);
    if (f) fclose(f);
    return v;
}

static void run(const char *name, int kind, const CleanOpts *o) {
    double t0 = now(), worst = 0;
    long peak = 0;
    int id = clean_start_kind(kind, o);
    if (id <= 0) { printf("%s: start failed %d\n", name, id); return; }
    char st[64];
    for (;;) {
        int pid = clean_test_helper_pid();
        if (pid > 0) { snprintf(st, sizeof st, "/proc/%d/status", pid); long h = vm(st, "VmHWM:"); if (h > peak) peak = h; }
        double a = now();
        char *j = clean_poll(kind);
        double b = now() - a;
        if (b > worst) worst = b;
        int running = strstr(j, "\"state\":\"running\"") != NULL;
        if (!running) {
            char *c = strstr(j, "\"seen\":"), *d = strstr(j, "\"candidates\":"), *e = strstr(j, "\"groups\":"), *l = strstr(j, "\"complete\":");
            printf("%-6s %8.0f ms  seen %ld  candidates %ld  groups %ld  complete %.5s  helper peak %ld MB  lord-server RSS %ld MB  worst poll %.2f ms\n", name, now() - t0,
                   c ? atol(c + 7) : 0, d ? atol(d + 13) : 0, e ? atol(e + 9) : 0, l ? l + 11 : "?", peak / 1024, vm("/proc/self/status", "VmRSS:") / 1024, worst);
            free(j);
            break;
        }
        free(j);
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    double a = now();
    free(clean_results_of(id, 0, CLEAN_PAGE_MAX, &(CleanFilter){ .type = -1 }));
    double b = now();
    free(clean_results_of(id, 40000, 100, &(CleanFilter){ .type = -1, .min_size = 1 }));
    double c = now();
    if (kind == CLEAN_KIND_TREE) free(clean_tree(id, 0, 500));
    if (kind == CLEAN_KIND_DUPS) free(clean_groups(id, 0, 100));
    double d = now();
    printf("       page %.2f ms, filtered far page %.2f ms, tree or groups %.2f ms\n", b - a, c - b, d - c);
}

int main(int argc, char **argv) {
    char h[PATH_MAX];
    if (argc != 2 || !realpath(argv[1], h)) return 1;
    fsops_init(h, "bench");
    run("rules", CLEAN_KIND_RULES, NULL);
    double t0 = now();
    char *j = clean_poll(CLEAN_KIND_RULES);
    char *idp = strstr(j, "\"scan\":{\"id\":");
    int scan = idp ? atoi(idp + 13) : 0;
    free(j);
    const char *const names[] = { "thumbnails" };
    int plan = clean_plan_rules(scan, names, 1);
    for (int waiting = 1; waiting;) { j = clean_poll(CLEAN_KIND_RULES); waiting = strstr(j, "\"state\":\"running\"") != NULL; free(j); }
    double t1 = now();
    clean_execute(plan);
    for (int waiting = 1; waiting;) { j = clean_poll(CLEAN_KIND_RULES); waiting = strstr(j, "\"state\":\"running\"") != NULL; free(j); }
    printf("plan of the thumbnails %.0f ms, real delete %.0f ms\n", t1 - t0, now() - t1);
    run("tree", CLEAN_KIND_TREE, &(CleanOpts){ .min_size = 1 << 20 });
    run("tree1", CLEAN_KIND_TREE, &(CleanOpts){ .min_size = 1 });
    run("dups", CLEAN_KIND_DUPS, &(CleanOpts){ .folders = { "Data" }, .nfolders = 1 });
    return 0;
}
