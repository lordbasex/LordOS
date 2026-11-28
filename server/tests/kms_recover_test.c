/* kms_recover_test.c — a page flip the kernel never completes (src/kms.c kms_flip_stuck, kms_flip_abandon; QA
 * 0.8.9.6.2, BUG-B: virtio-gpu after an s2idle suspend, the greeter never showed). The stuck flip is taken as done (the
 * GPU path's on_flip with its data, or the CPU path's buffers swapped), the next frame is a full modeset, and the
 * kernel's late event for it, if it ever comes, completes nothing. No card needed: the event comes through a pipe. */
#include "kms.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int flips;
static void *flipped;
static void on_flip(void *data) { flips++; flipped = data; }

/* ---- the modeset after a dropped flip, bounded (QA ws174: virtio-gpu after an s2idle wake held SETCRTC) ---- */
static pthread_mutex_t gmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gcv = PTHREAD_COND_INITIALIZER;
static int gate_open = 1, answer, setcrtcs;
static uint32_t last_fb;
static int fake_setcrtc(int fd, struct drm_mode_crtc *set) {
    (void)fd;
    pthread_mutex_lock(&gmu);
    setcrtcs++;
    last_fb = set->fb_id;
    while (!gate_open) pthread_cond_wait(&gcv, &gmu);
    int r = answer;
    pthread_mutex_unlock(&gmu);
    return r;
}
static void gate(int open, int r) { pthread_mutex_lock(&gmu); gate_open = open; answer = r; pthread_cond_broadcast(&gcv); pthread_mutex_unlock(&gmu); }
static double ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void settle(void) { while (kms_recover_busy()) { struct timespec t = { 0, 1000000 }; nanosleep(&t, NULL); } }

static void test_bounded(void) {
    kms_test_setcrtc = fake_setcrtc;
    const uint64_t s = 1000000000ull;
    Kms k;
    memset(&k, 0, sizeof k);
    k.fd = -1;
    k.crtc_id = 41;
    k.custom = 1;
    k.on_flip = on_flip;
    int a = 1, b = 2, c = 3;
    /* a real card: the modeset answers at once, and goes on as before (shown now, on_flip now) */
    k.flip_pending = 1; k.flip_submit_ns = 1; k.flip_data = &a;
    flips = 0;
    kms_flip_abandon(&k);
    CHECK(k.recover_bg && k.modeset_pending && flips == 1);
    gate(1, 0);
    CHECK(kms_flip_fb(&k, 77, &b) == 0);
    CHECK(flips == 2 && flipped == &b && !k.flip_pending && !k.modeset_pending && !k.recover_bg && !k.recover && last_fb == 77);
    /* virtio-gpu after s2idle: the modeset doesn't answer. The call returns within its bound, the frame is a flip
     * pending that no watchdog drops, the cursor doesn't touch the CRTC */
    k.flip_pending = 1; k.flip_submit_ns = 1; k.flip_data = &a;
    kms_flip_abandon(&k);
    CHECK(flips == 3);
    gate(0, 0);
    double t0 = ms_now();
    CHECK(kms_flip_fb(&k, 78, &c) == 0);
    double took = ms_now() - t0;
    CHECK(took >= KMS_RECOVER_WAIT_MS * 0.9 && took < KMS_RECOVER_WAIT_MS * 4);
    CHECK(k.recover && k.flip_pending && flips == 3);
    CHECK(!kms_flip_stuck(&k, 1000 * s, 2 * s));
    kms_flip_abandon(&k);
    CHECK(k.recover && k.flip_pending && flips == 3);
    kms_recover_poll(&k);
    CHECK(k.recover && flips == 3 && kms_recover_busy() == 1);
    /* it answers: the frame completes as its event would */
    gate(1, 0);
    settle();
    kms_recover_poll(&k);
    CHECK(!k.recover && !k.flip_pending && !k.modeset_pending && !k.recover_bg && flips == 4 && flipped == &c);
    /* a modeset that fails after a while: completed (its buffer back), the next frame tries again on a thread */
    k.flip_pending = 1; k.flip_submit_ns = 1; k.flip_data = &a;
    kms_flip_abandon(&k);
    gate(0, -1);
    CHECK(kms_flip_fb(&k, 79, &b) == 0 && k.recover);
    gate(1, -1);
    settle();
    kms_recover_poll(&k);
    CHECK(!k.recover && !k.flip_pending && k.modeset_pending && k.recover_bg && flips == 6 && flipped == &b);
    gate(1, 0);
    CHECK(kms_flip_fb(&k, 80, &c) == 0 && !k.modeset_pending && flips == 7);
    /* the CPU path: the buffers swap when it answers */
    Kms d;
    memset(&d, 0, sizeof d);
    d.fd = -1;
    d.front = 0;
    d.flip_pending = 1; d.flip_submit_ns = 1;
    kms_flip_abandon(&d);
    CHECK(d.front == 1);
    gate(0, 0);
    CHECK(kms_flip(&d) == 0 && d.recover && d.front == 1);
    gate(1, 0);
    settle();
    kms_recover_poll(&d);
    CHECK(!d.recover && d.front == 0 && !d.modeset_pending);
    /* closed while it runs: its thread frees it (ASan: no leak, no use after free) */
    d.flip_pending = 1; d.flip_submit_ns = 1;
    kms_flip_abandon(&d);
    gate(0, 0);
    CHECK(kms_flip(&d) == 0 && d.recover);
    kms_close(&d);
    CHECK(!d.recover);
    gate(1, 0);
    settle();
    kms_test_setcrtc = NULL;
}

int main(void) {
    const uint64_t s = 1000000000ull;
    Kms k;
    memset(&k, 0, sizeof k);
    k.fd = -1;
    k.crtc_id = 41;
    /* the GPU path: kms_flip_fb's flip, pending */
    int tag = 7;
    k.custom = 1;
    k.on_flip = on_flip;
    k.flip_data = &tag;
    k.flip_pending = 1;
    k.flip_submit_ns = 10 * s;
    CHECK(!kms_flip_stuck(&k, 11 * s, 2 * s) && !kms_flip_stuck(&k, 12 * s, 2 * s) && kms_flip_stuck(&k, 12 * s + 1, 2 * s));
    kms_flip_abandon(&k);
    CHECK(!k.flip_pending && k.modeset_pending && flips == 1 && flipped == &tag);
    CHECK(!kms_flip_stuck(&k, 100 * s, 2 * s));
    kms_flip_abandon(&k);                       /* nothing pending: nothing done */
    CHECK(flips == 1);

    /* the kernel's late event for it completes nothing */
    int p[2];
    CHECK(!pipe(p));
    struct drm_event_vblank ev;
    memset(&ev, 0, sizeof ev);
    ev.base.type = DRM_EVENT_FLIP_COMPLETE;
    ev.base.length = sizeof ev;
    ev.crtc_id = 41;
    CHECK(write(p[1], &ev, sizeof ev) == (ssize_t)sizeof ev);
    k.fd = p[0];
    Kms *heads[1] = { &k };
    kms_dispatch_heads(heads, 1);
    CHECK(flips == 1 && !k.flip_pending);
    /* a real flip pending afterwards still completes with its event */
    k.flip_pending = 1;
    CHECK(write(p[1], &ev, sizeof ev) == (ssize_t)sizeof ev);
    kms_dispatch_heads(heads, 1);
    CHECK(flips == 2 && !k.flip_pending);
    close(p[0]);
    close(p[1]);

    /* the CPU path: the dumb buffers swap as the event would */
    Kms c;
    memset(&c, 0, sizeof c);
    c.fd = -1;
    c.front = 0;
    c.flip_pending = 1;
    c.flip_submit_ns = 1;
    kms_flip_abandon(&c);
    CHECK(c.front == 1 && !c.flip_pending && c.modeset_pending);
    CHECK(!kms_flip_stuck(&c, 0, 0));
    /* a flip never queued (no submit time) is never "stuck" */
    c.flip_pending = 1;
    c.flip_submit_ns = 0;
    CHECK(!kms_flip_stuck(&c, 100 * s, 2 * s));

    test_bounded();
    printf("kms-recover-test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
