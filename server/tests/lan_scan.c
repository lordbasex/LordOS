/* lan_scan.c — one real pass of Files → Network on the network this machine is on, printed as JSON,
 * with the real lord-lan next to this binary. Run it outside a LordOS session. */
#include "../src/lan/lan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(void) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    if (!lan_start(1)) { fprintf(stderr, "lan_start refused\n"); return 1; }
    for (;;) {
        char *s = lan_poll();
        int run = s && strstr(s, "\"running\":true");
        if (!run) {
            clock_gettime(CLOCK_MONOTONIC, &b);
            printf("%s\n", s ? s : "null");
            fprintf(stderr, "pass took %.1f s; a second lan_start now: %d\n", (double)(b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9, lan_start(1));
            free(s);
            return 0;
        }
        free(s);
        usleep(200000);
    }
}
