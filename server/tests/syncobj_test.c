/* syncobj_test.c — explicit sync's pure rules (src/wl/syncobj.h): the protocol's errors at commit, what a commit does
 * (applied, queued behind, or waiting), the commit queue and the limits. The kernel side is checked by
 * tests/syncobj_client.c against a running lord-server (tests/syncobj_headless.sh). */
#include "wl/syncobj.h"

#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "syncobj_test: line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void) {
    /* ---- errors: (attached, buffer, dmabuf, acquire, release, same timeline, acquire point, release point) ---- */
    /* a dmabuf with both points: fine; on one timeline only when acquire < release */
    CHECK(sobj_commit_error(1, 1, 1, 1, 1, 0, 5, 1) == SOBJ_OK);
    CHECK(sobj_commit_error(1, 1, 1, 1, 1, 1, 1, 2) == SOBJ_OK);
    CHECK(sobj_commit_error(1, 1, 1, 1, 1, 1, 2, 2) == SOBJ_ERR_CONFLICTING_POINTS);
    CHECK(sobj_commit_error(1, 1, 1, 1, 1, 1, 3, 2) == SOBJ_ERR_CONFLICTING_POINTS);
    CHECK(sobj_commit_error(1, 1, 1, 1, 1, 1, 0, UINT64_MAX) == SOBJ_OK);
    CHECK(sobj_commit_error(1, 1, 1, 1, 1, 1, UINT64_MAX, UINT64_MAX) == SOBJ_ERR_CONFLICTING_POINTS);
    /* a buffer missing a point */
    CHECK(sobj_commit_error(1, 1, 1, 0, 1, 0, 0, 1) == SOBJ_ERR_NO_ACQUIRE_POINT);
    CHECK(sobj_commit_error(1, 1, 1, 1, 0, 0, 1, 0) == SOBJ_ERR_NO_RELEASE_POINT);
    CHECK(sobj_commit_error(1, 1, 1, 0, 0, 0, 0, 0) == SOBJ_ERR_NO_ACQUIRE_POINT);
    /* shared memory (or any buffer that isn't a dmabuf) */
    CHECK(sobj_commit_error(1, 1, 0, 1, 1, 0, 1, 1) == SOBJ_ERR_UNSUPPORTED_BUFFER);
    CHECK(sobj_commit_error(1, 1, 0, 0, 0, 0, 0, 0) == SOBJ_ERR_UNSUPPORTED_BUFFER);
    /* points with no buffer: nothing attached, or a null buffer attached */
    CHECK(sobj_commit_error(0, 0, 0, 1, 1, 0, 1, 1) == SOBJ_ERR_NO_BUFFER);
    CHECK(sobj_commit_error(0, 0, 0, 1, 0, 0, 1, 0) == SOBJ_ERR_NO_BUFFER);
    CHECK(sobj_commit_error(0, 0, 0, 0, 1, 0, 0, 1) == SOBJ_ERR_NO_BUFFER);
    CHECK(sobj_commit_error(1, 0, 0, 1, 1, 0, 1, 1) == SOBJ_ERR_NO_BUFFER);
    /* no buffer and no points: a commit of state only, or a null buffer to unmap */
    CHECK(sobj_commit_error(0, 0, 0, 0, 0, 0, 0, 0) == SOBJ_OK);
    CHECK(sobj_commit_error(1, 0, 0, 0, 0, 0, 0, 0) == SOBJ_OK);
    CHECK(sobj_point(1, 2) == ((uint64_t)1 << 32 | 2));
    CHECK(sobj_point(0xffffffffu, 0xffffffffu) == UINT64_MAX);

    /* ---- what a commit does ---- */
    CHECK(sobj_commit_action(0, 0, 0) == SOBJ_APPLY);               /* implicit sync */
    CHECK(sobj_commit_action(0, 1, 1) == SOBJ_APPLY);               /* its point already signalled (the usual case) */
    CHECK(sobj_commit_action(0, 1, 0) == SOBJ_QUEUE_WAIT);          /* waits for its own point */
    CHECK(sobj_commit_action(1, 1, 1) == SOBJ_QUEUE_BEHIND);        /* ready, but an earlier one waits: in order */
    CHECK(sobj_commit_action(1, 0, 0) == SOBJ_QUEUE_BEHIND);        /* an implicit commit after a waiting one */

    /* ---- the queue: in order, at most SOBJ_MAX_PENDING, the ring wraps ---- */
    SobjQueue q = { { 0 }, 0, 0 };
    int v[3 * SOBJ_MAX_PENDING];
    CHECK(sobjq_front(&q) == NULL && sobjq_pop(&q) == NULL);
    for (int i = 0; i < SOBJ_MAX_PENDING; i++) CHECK(sobjq_push(&q, &v[i]) == 0);
    CHECK(sobjq_push(&q, &v[SOBJ_MAX_PENDING]) == -1);            /* one more: the client is disconnected */
    CHECK(q.n == SOBJ_MAX_PENDING);
    CHECK(sobjq_front(&q) == &v[0]);
    CHECK(sobjq_pop(&q) == &v[0] && sobjq_pop(&q) == &v[1]);
    CHECK(sobjq_push(&q, &v[20]) == 0 && sobjq_push(&q, &v[21]) == 0);     /* wraps around */
    CHECK(sobjq_push(&q, &v[22]) == -1);
    for (int i = 2; i < SOBJ_MAX_PENDING; i++) CHECK(sobjq_pop(&q) == &v[i]);
    CHECK(sobjq_pop(&q) == &v[20] && sobjq_pop(&q) == &v[21]);
    CHECK(q.n == 0 && sobjq_pop(&q) == NULL);
    /* a long run through the ring keeps the order */
    int order_ok = 1;
    for (int i = 0; i < 1000; i++) {
        if (sobjq_push(&q, &v[i % (3 * SOBJ_MAX_PENDING)])) order_ok = 0;
        if (i % 3 == 2) {
            while (q.n > 1) sobjq_pop(&q);
        }
    }
    CHECK(order_ok);

    /* ---- limits ---- */
    CHECK(SOBJ_MAX_PENDING >= 5);                       /* a mailbox swapchain's images, all committed at once */
    CHECK(sobj_wait_mode(0, 0, 0) == SOBJ_WAIT_EVENTFD);
    CHECK(sobj_wait_mode(SOBJ_MAX_WAITS_CLIENT - 1, SOBJ_MAX_WAITS_CLIENT - 1, SOBJ_MAX_WAITS - 1) == SOBJ_WAIT_EVENTFD);
    /* a connection past its own cap: refused (it alone is disconnected) */
    CHECK(sobj_wait_mode(SOBJ_MAX_WAITS_CLIENT, SOBJ_MAX_WAITS_CLIENT, 0) == SOBJ_WAIT_REFUSE);
    /* the session full: an innocent client is never refused, it polls; the process holding more than its share is */
    CHECK(sobj_wait_mode(0, 0, SOBJ_MAX_WAITS) == SOBJ_WAIT_POLL);
    CHECK(sobj_wait_mode(3, SOBJ_MAX_WAITS_CLIENT - 1, SOBJ_MAX_WAITS + 5) == SOBJ_WAIT_POLL);
    CHECK(sobj_wait_mode(0, SOBJ_MAX_WAITS_CLIENT, SOBJ_MAX_WAITS) == SOBJ_WAIT_REFUSE);
    CHECK(SOBJ_MAX_WAITS < 256);                        /* below wl.c's surfaces: others can still make windows */
    CHECK(SOBJ_MAX_POLLS > 0 && SOBJ_MAX_POLLS <= SOBJ_MAX_WAITS);

    if (fails) { fprintf(stderr, "syncobj_test: %d failed\n", fails); return 1; }
    printf("syncobj_test: ok\n");
    return 0;
}
