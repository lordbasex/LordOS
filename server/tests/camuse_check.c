/* camuse_check.c — the capture indicator's camera rows (src/hw/camuse.c) on this computer's real cameras, driven by
 * tests/webcam_check.py. Run from server/:
 *
 *   build/camuse-check <seconds>
 *
 * Watches the /dev/video* nodes as lord-server does and prints each "camera-use" event ("camera-use: {...}") until
 * seconds pass. It opens no camera itself. */
#include "../src/hw/camuse.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void emit(const char *kind, const char *json) { printf("%s: %s\n", kind, json); fflush(stdout); }

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: camuse-check <seconds>\n"); return 2; }
    double end = now_ms() + atoi(argv[1]) * 1000.0;
    camuse_start(emit);
    if (camuse_fd() < 0) return 1;
    printf("watching\n");
    fflush(stdout);
    while (now_ms() < end) {
        double t = camuse_timeout(now_ms()), left = end - now_ms();
        if (t < 0 || t > left) t = left;
        struct pollfd p = { camuse_fd(), POLLIN, 0 };
        if (poll(&p, 1, (int)t + 1) > 0) camuse_dispatch();
        camuse_tick(now_ms());
    }
    return 0;
}
