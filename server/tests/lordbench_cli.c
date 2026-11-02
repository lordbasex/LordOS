/* lordbench_cli.c — runs LordBench from a terminal through lord-server's own code (bench/lbrun.c), with
 * build/lord-bench next to it: `build/lordbench-cli quick|full [sustained_s]`. For checking the helper
 * and its sandbox on a machine without a session; prints the result JSON. */
#include "../src/bench/lbrun.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: lordbench-cli quick|full [sustained_s]\n"); return 2; }
    if (lbrun_start(argv[1], argc > 2 ? atoi(argv[2]) : 0, "{\"cli\":true}")) { fprintf(stderr, "refused\n"); return 1; }
    char last[64] = "";
    for (;;) {
        struct timespec ts = { 0, 250 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        char *p = lbrun_poll();
        if (!p) return 1;
        char *s = strstr(p, "\"step\":\"");
        if (s && strncmp(s, last, sizeof last - 1)) { snprintf(last, sizeof last, "%.63s", s); fprintf(stderr, "%.60s\n", s + 8); }
        if (!strstr(p, "\"state\":\"waiting\"") && !strstr(p, "\"state\":\"running\"")) { puts(p); free(p); return 0; }
        free(p);
    }
}
