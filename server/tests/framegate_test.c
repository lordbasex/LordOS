/* framegate_test.c — the frame callbacks of a window nobody can see (src/wl/framegate.h). */
#include "wl/framegate.h"

#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "framegate_test: line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void) {
    FrameGate g = { 0, 0 };
    /* a window that can be seen: every frame, whatever was sent before */
    CHECK(framegate_due(&g, 0, 0));
    framegate_sent(&g, 100);
    CHECK(framegate_due(&g, 0, 101));
    /* hidden: once a second from the last one sent */
    CHECK(!framegate_due(&g, 1, 101));
    CHECK(!framegate_due(&g, 1, 1099));
    CHECK(framegate_due(&g, 1, 1100));
    CHECK(framegate_wait(&g, 101) == 999);
    CHECK(framegate_wait(&g, 600) == 500);
    CHECK(framegate_wait(&g, 1100) == 0);
    CHECK(framegate_wait(&g, 5000) == 0);
    /* hidden before anything was ever sent: at once (an app that waits for its first frame never stalls) */
    FrameGate n = { 0, 0 };
    CHECK(framegate_due(&n, 1, 12345));
    CHECK(framegate_wait(&n, 12345) == 0);
    /* lord-server's milliseconds wrap */
    FrameGate w = { 0, 0 };
    framegate_sent(&w, 0xFFFFFF00u);
    CHECK(!framegate_due(&w, 1, 0xFFFFFFFFu));
    CHECK(!framegate_due(&w, 1, 100));
    CHECK(framegate_wait(&w, 100) == 1000 - 356);
    CHECK(framegate_due(&w, 1, 0xFFFFFF00u + 1000u));
    if (fails) return 1;
    printf("framegate_test: ok\n");
    return 0;
}
