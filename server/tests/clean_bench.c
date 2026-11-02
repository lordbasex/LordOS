/* clean_bench.c — times Cleaner's scans on a throwaway folder given as the home folder (never a real one): wall
 * time, the helper's peak memory, axia-server's memory, and the cost of the calls the compositor's thread makes.
 *   build/clean-bench <folder>           needs build/axia-clean next to it
 *   build/clean-bench <folder> images N  writes N pairs of JPEG pictures in <folder>/Pictures and times similar images only */
#include "clean/clean.h"
#include "clean/rules.h"
#include "fsops.h"
#include "sec/json.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static void wr(void *ctx, void *d, int n) { if (write(*(int *)ctx, d, (size_t)n) != n) exit(1); }

/* a picture of its own per seed; at 12 MP and quality 85, the shape of a phone's photo */
static void put_jpeg(const char *path, int seed, int w, int h) {
    unsigned char *px = malloc((size_t)w * h * 3);
    if (!px) exit(1);
    double a = 1.3 + seed * 0.731, b = 2.1 + seed * 0.417, c = seed * 0.29;
    unsigned rng = (unsigned)seed + 1;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        rng = rng * 1103515245 + 12345;
        double u = (double)x / w, v = (double)y / h;
        double t = 128 + 70 * sin(a * 6.28 * u + c) * cos(b * 6.28 * v) + (double)(rng >> 27) - 16;
        unsigned char g = t < 0 ? 0 : t > 255 ? 255 : (unsigned char)t;
        unsigned char *p = px + (size_t)(y * w + x) * 3;
        p[0] = g; p[1] = (unsigned char)(g / 2 + 40); p[2] = (unsigned char)(255 - g);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) exit(1);
    stbi_write_jpg_to_func(wr, &fd, w, h, 3, px, 85);
    close(fd);
    free(px);
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
            printf("%-6s %8.0f ms  seen %ld  candidates %ld  groups %ld  complete %.5s  helper peak %ld MB  axia-server RSS %ld MB  worst poll %.2f ms\n", name, now() - t0,
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
    if (kind == CLEAN_KIND_DUPS || kind == CLEAN_KIND_IMAGES) free(clean_groups(id, 0, 100));
    double d = now();
    printf("       page %.2f ms, filtered far page %.2f ms, tree or groups %.2f ms\n", b - a, c - b, d - c);
}

int main(int argc, char **argv) {
    char h[PATH_MAX];
    if ((argc != 2 && argc != 4) || !realpath(argv[1], h)) return 1;
    fsops_init(h, "bench");
    if (argc == 4) {
        char p[PATH_MAX * 2];
        snprintf(p, sizeof p, "%s/Pictures", h);
        mkdir(p, 0700);
        int n = atoi(argv[3]);
        double t0 = now();
        for (int i = 0; i < n; i++) {
            snprintf(p, sizeof p, "%s/Pictures/%04d.jpg", h, i);
            put_jpeg(p, i, 4000, 3000);
            snprintf(p, sizeof p, "%s/Pictures/%04d-small.jpg", h, i);
            put_jpeg(p, i, 1600, 1200);
        }
        printf("wrote %d images in %.0f ms\n", n * 2, now() - t0);
        run("images", CLEAN_KIND_IMAGES, &(CleanOpts){ .folders = { "Pictures" }, .nfolders = 1 });
        char *j = clean_poll(CLEAN_KIND_IMAGES), *u = strstr(j, "\"undecoded\":");
        printf("undecoded %ld\n", u ? atol(u + 12) : -1);
        char *idp = strstr(j, "\"scan\":{\"id\":");
        int id = idp ? atoi(idp + 13) : 0;
        free(j);
        for (long off = 0;; off += 100) {
            JDoc d = { 0 };
            char *g = clean_groups(id, off, 100);
            if (json_parse(g, strlen(g), &d, NULL, 0)) break;
            int any = 0;
            for (const JVal *v = json_get(d.root, "groups")->kid; v; v = v->next) {
                any = 1;
                printf("group:");
                for (const JVal *m = json_get(v, "members")->kid; m; m = m->next) printf(" %s", strrchr(json_str(m, "path"), '/') + 1);
                printf("\n");
            }
            json_free(&d);
            free(g);
            if (!any) break;
        }
        return 0;
    }
    run("rules", CLEAN_KIND_RULES, NULL);
    double t0 = now();
    char *j = clean_poll(CLEAN_KIND_RULES);
    char *idp = strstr(j, "\"scan\":{\"id\":");
    int scan = idp ? atoi(idp + 13) : 0;
    free(j);
    const char *const names[] = { "thumbnails" };
    int plan = clean_plan_rules(scan, names, 1);
    for (int waiting = 1; waiting;) { j = clean_poll(CLEAN_KIND_RULES); waiting = strstr(j, "\"state\":\"running\"") != NULL; free(j); nanosleep(&(struct timespec){ 0, 5000000 }, NULL); }
    double t1 = now();
    clean_execute(plan);
    for (int waiting = 1; waiting;) { j = clean_poll(CLEAN_KIND_RULES); waiting = strstr(j, "\"state\":\"running\"") != NULL; free(j); nanosleep(&(struct timespec){ 0, 5000000 }, NULL); }
    printf("plan of the thumbnails %.0f ms, real delete %.0f ms\n", t1 - t0, now() - t1);
    run("tree", CLEAN_KIND_TREE, &(CleanOpts){ .min_size = 1 << 20 });
    run("tree1", CLEAN_KIND_TREE, &(CleanOpts){ .min_size = 1 });
    run("dups", CLEAN_KIND_DUPS, &(CleanOpts){ .folders = { "Data" }, .nfolders = 1 });
    return 0;
}
